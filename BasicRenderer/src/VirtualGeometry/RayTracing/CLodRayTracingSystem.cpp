#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"

#include <algorithm>
#include <format>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>

#include <spdlog/spdlog.h>
#include <BasicTelemetry/Tracy.h>

#include "BasicRenderer/Assets/Import/CLodOpacityMicromapCache.h"
#include "BasicRenderer/Extensions/Buffers/PagePool.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Render/PassBuilders.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ResourceGroup.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "BuiltinResources.h"
#include "Runtime/Device/DescriptorHeapManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "../shaders/Common/defines.h"
#include "../shaders/PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include <rhi_helpers.h>

namespace br::render {

namespace {

constexpr uint32_t kMaxClusterTriangles = MS_MESHLET_SIZE;
constexpr uint32_t kMaxClusterVertices = MS_MESHLET_SIZE;
constexpr uint32_t kCountersBytes = CLOD_RT_COUNTER_WORDS * 4u;
static_assert(kCountersBytes % 8u == 0u, "the readback slice resolves 8-byte query data after the counters");
constexpr uint32_t kStatsBytes = CLOD_RT_STAT_WORDS * 4u;
constexpr uint32_t kCtrlBytes = CLOD_RT_CTRL_WORDS * 4u;
static_assert(kCtrlBytes % 8u == 0u, "the readback slice resolves 8-byte query data after the cache's control words");
constexpr uint32_t kUavClearDispatchWidth = 256u;
constexpr uint32_t kSceneTimers = 8u;
constexpr uint32_t kRequestBytes = 8u; // [group, the asking alpha-tested draw's material identity or zero]
constexpr uint32_t kOmmScratchAlignment = 256u;
constexpr uint32_t kOmmUploadAlignment = 256u;
constexpr uint32_t kReleaseBytes = CLOD_RT_OMM_RETIRE_WINDOW_MAX * 16u; // [slot, group + 1, index unit, array unit] each.
constexpr uint64_t kPageBytes = uint64_t(CLOD_RT_CACHE_PAGE_UNITS) * CLOD_RT_CACHE_UNIT_BYTES;
// The scene build's readback slice: counters, control words, timers, then micromap requests (the build budget's) and releases.
constexpr uint64_t kReadbackCtrl = kCountersBytes;
constexpr uint64_t kReadbackTimers = kReadbackCtrl + kCtrlBytes;
constexpr uint64_t kReadbackOmmWanted = kReadbackTimers + kSceneTimers * 8u;
// The visibility comparison's and pixel watch's own readback slices.
constexpr uint32_t kStatsSliceBytes = (kStatsBytes + 255u) & ~255u;
constexpr uint32_t kWatchSliceBytes = (CLOD_RT_WATCH_WORDS * 4u + 255u) & ~255u;
static_assert(CLOD_RT_OMM_INDEX_UNIT_BYTES == CLodCache::kOpacityMicromapIndexUnitBytes, "a baked group's index units are the pool's");
static_assert(sizeof(CLodCache::OpacityMicromapDesc) % 4u == 0u);

uint32_t OmmHash(uint32_t group) noexcept {
    // clodRayTracingSceneBuild.hlsl's CLodRtOmmHash.
    uint32_t h = group * 0x9E3779B1u;
    h ^= h >> 15u;
    return h & (CLOD_RT_OMM_TABLE_ENTRIES - 1u);
}

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    return alignment == 0u ? value : ((value + alignment - 1u) & ~(alignment - 1u));
}

constexpr uint32_t Low(uint64_t value) noexcept { return static_cast<uint32_t>(value); }
constexpr uint32_t High(uint64_t value) noexcept { return static_cast<uint32_t>(value >> 32u); }

// Between dispatches of one pass that write and read the same buffers.
void GlobalUavBarrier(rhi::CommandList& commands) {
    rhi::GlobalBarrier barrier{
        .beforeSync = rhi::ResourceSyncState::All,
        .afterSync = rhi::ResourceSyncState::All,
        .beforeAccess = rhi::ResourceAccessType::UnorderedAccess,
        .afterAccess = rhi::ResourceAccessType::UnorderedAccess,
    };
    rhi::BarrierBatch batch{};
    batch.globals = { &barrier, 1u };
    commands.Barriers(batch);
}

// A buffer's use within a pass. The graph puts a buffer in the state its pass declares it in (Uav, BuildInput,
// BuildOutput, AsBuild, AsRead, CopyDest); the pass moves it between these, and back, or declares where it leaves it.
struct Use {
    rhi::ResourceAccessType access;
    rhi::ResourceSyncState sync;
};
constexpr Use kUav{ rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceSyncState::AllShading };
constexpr Use kBuildInput{ rhi::ResourceAccessType::ShaderResource, org::kAccelerationStructureBuildStageSync };
constexpr Use kBuildOutput{ rhi::ResourceAccessType::UnorderedAccess, org::kAccelerationStructureBuildStageSync };
constexpr Use kAsWritten{ rhi::ResourceAccessType::RaytracingAccelerationStructureWrite, rhi::ResourceSyncState::BuildRaytracingAccelerationStructure };
constexpr Use kAsRead{ rhi::ResourceAccessType::RaytracingAccelerationStructureRead, rhi::ResourceSyncState::BuildRaytracingAccelerationStructure };
constexpr Use kCopySource{ rhi::ResourceAccessType::CopySource, rhi::ResourceSyncState::Copy };
constexpr Use kCopyDest{ rhi::ResourceAccessType::CopyDest, rhi::ResourceSyncState::Copy };

void Transition(rhi::CommandList& commands, std::initializer_list<rhi::ResourceHandle> buffers, Use before, Use after) {
    std::vector<rhi::BufferBarrier> barriers;
    barriers.reserve(buffers.size());
    for (const rhi::ResourceHandle buffer : buffers) {
        rhi::BufferBarrier barrier{};
        barrier.buffer = buffer;
        barrier.beforeSync = before.sync;
        barrier.beforeAccess = before.access;
        barrier.afterSync = after.sync;
        barrier.afterAccess = after.access;
        barriers.push_back(barrier);
    }
    rhi::BarrierBatch batch{};
    batch.buffers = { barriers.data(), static_cast<uint32_t>(barriers.size()) };
    commands.Barriers(batch);
}

// States a pass leaves a buffer in, for its internal transition declarations.
const org::ResourceState kLeftAsBuildInput = org::kAccelerationStructureBuildInputState;
const org::ResourceState kLeftAsCopySource{ rhi::ResourceAccessType::CopySource, rhi::AccessToLayout(rhi::ResourceAccessType::CopySource, true),
    rhi::RenderSyncFromAccess(rhi::ResourceAccessType::CopySource) };

constexpr rhi::ResourceFlags AsStorageFlags() noexcept {
    return static_cast<rhi::ResourceFlags>(rhi::RF_AllowUnorderedAccess | rhi::RF_RaytracingAccelerationStructure);
}

} // namespace

CLodRayTracingSystem::CLodRayTracingSystem() {
    const auto read = [](const char* name) -> unsigned long long {
        char* value = nullptr;
        size_t length = 0;
        unsigned long long result = 0;
        if (_dupenv_s(&value, &length, name) == 0 && value) result = std::strtoull(value, nullptr, 10);
        std::free(value);
        return result;
    };
    if (const auto clusters = read("SARP_RT_MAX_LIST_CLUSTERS"))
        m_limits.maxClustersInBvh = static_cast<uint32_t>(std::min<unsigned long long>(clusters, 1ull << 24));
    if (const auto references = read("SARP_RT_MAX_CLUSTER_REFERENCES"))
        m_limits.maxClusterReferences = static_cast<uint32_t>(std::clamp<unsigned long long>(references, 1ull << 16, 1ull << 24));
    if (const auto instances = read("SARP_RT_MAX_INSTANCES"))
        m_limits.maxInstances = static_cast<uint32_t>(std::min<unsigned long long>(instances, 1ull << 24));
    if (const auto mib = read("SARP_RT_CLAS_POOL_MB")) m_limits.clasPoolBytes = std::min<unsigned long long>(mib, 1ull << 16) << 20u;
    if (const auto mib = read("SARP_RT_OMM_POOL_MB")) {
        const uint64_t defaultBytes = m_limits.ommArrayPoolBytes;
        m_limits.ommArrayPoolBytes = std::clamp<unsigned long long>(mib, 1ull, 4096ull) << 20u;
        m_limits.ommIndexUnits = static_cast<uint32_t>(std::max<uint64_t>(1024u, uint64_t(m_limits.ommIndexUnits) * m_limits.ommArrayPoolBytes / defaultBytes));
    }
    if (const auto budget = read("SARP_RT_BUILD_BUDGET"))
        m_limits.clusterBuildBudget = static_cast<uint32_t>(std::clamp<unsigned long long>(budget, CLOD_RT_CACHE_MAX_GROUP_CLUSTERS, 1ull << 15));
    if (const auto budget = read("SARP_RT_TRANSIENT_BUDGET"))
        m_limits.transientBuildBudget = static_cast<uint32_t>(std::clamp<unsigned long long>(budget, CLOD_RT_CACHE_MAX_GROUP_CLUSTERS, 1ull << 15));
    char* csvPath = nullptr;
    size_t csvLength = 0;
    if (_dupenv_s(&csvPath, &csvLength, "SARP_RT_FRAME_CSV") == 0 && csvPath && csvPath[0] != '\0') {
        std::FILE* file = nullptr;
        if (fopen_s(&file, csvPath, "w") == 0 && file) {
            m_frameCsv.reset(file);
            std::fputs("ms,frame,instances,coarse,clusters,requests,persistent,transient,transient_clusters,refused,evicted,stale,"
                "admitted,not_built,discarded,discarded_clusters,missing,cached_groups,cached_pages,free_pages,builds,"
                "ms_count,ms_cache,ms_allocate,ms_place,ms_blas,ms_instances,ms_tlas,"
                "watch_bright,watch_bad_material,watch_dark_terrain,watch_resolve_failed,watch_dark_albedo,watch_primary_ended,watch_max_luminance,watch_non_finite\n", file);
            m_frameCsvStartTicks = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }
        else {
            spdlog::warn("CLod RT: could not open SARP_RT_FRAME_CSV '{}'", csvPath);
        }
    }
    std::free(csvPath);

    if (const char* watch = std::getenv("SARP_RT_PIXEL_WATCH"); watch && watch[0] != '\0') {
        m_watchBright = std::strtof(watch, nullptr);
        if (!(m_watchBright > 0.0f)) m_watchBright = 0.0f;
    }
    char* watchCsvPath = nullptr;
    size_t watchCsvLength = 0;
    if (m_watchBright > 0.0f && _dupenv_s(&watchCsvPath, &watchCsvLength, "SARP_RT_PIXEL_WATCH_CSV") == 0 && watchCsvPath && watchCsvPath[0] != '\0') {
        std::FILE* file = nullptr;
        if (fopen_s(&file, watchCsvPath, "w") == 0 && file) {
            m_watchCsv.reset(file);
            // Words as the GPU wrote them (see pathTracingWatch.hlsli); a, b and c are floats or uints by kind.
            std::fputs("frame,kind,bounce,flags,x,y,instance_id,instance_index,cluster,primitive,misc,a0,a1,a2,b0,b1,b2,c0,c1,c2\n", file);
        }
    }
    std::free(watchCsvPath);
}

CLodRayTracingSystem::~CLodRayTracingSystem() {
    Shutdown();
}

void CLodRayTracingSystem::Shutdown() {
    if (m_ommScope.Valid()) m_ommScope.CancelAndWait();
}

bool CLodRayTracingSystem::EnsureOmmPools() {
    if (m_ommPoolsReady || m_ommPoolsFailed) return m_ommPoolsReady;
    const rhi::ResourceFlags uavFlag = rhi::RF_AllowUnorderedAccess;
    constexpr rhi::HeapType local = rhi::HeapType::DeviceLocal;
    bool ok = CreateBuffer(m_ommArrayPool, m_limits.ommArrayPoolBytes, local, AsStorageFlags(), "CLod RT micromap arrays");
    ok = ok && CreateBuffer(m_ommIndexPool, uint64_t(m_limits.ommIndexUnits) * CLOD_RT_OMM_INDEX_UNIT_BYTES, local, uavFlag, "CLod RT micromap indices", View::Raw);
    if (!ok) {
        m_ommPoolsFailed = true;
        spdlog::warn("CLod RT: could not make the micromap pools; alpha tests stay in any-hit");
        return false;
    }
    m_ommArrayAllocator.Reset(static_cast<uint32_t>(m_limits.ommArrayPoolBytes / CLOD_RT_OMM_ARRAY_ALIGNMENT));
    m_ommIndexAllocator.Reset(m_limits.ommIndexUnits);
    // The passes that use them declare these groups, so the graph picks them up when it next lowers.
    m_ommArrays->AddResource(m_ommArrayPool.buffer);
    m_ommIndices->AddResource(m_ommIndexPool.buffer);
    m_ommPoolsReady = true;
    spdlog::info("CLod RT: micromap pools made on first use, {} MiB of arrays, {} MiB of indices",
        m_ommArrayPool.bytes >> 20u, m_ommIndexPool.bytes >> 20u);
    return true;
}

void CLodRayTracingSystem::RangeAllocator::Reset(uint32_t total) {
    m_free.clear();
    if (total != 0u) m_free[0u] = total;
    m_used = 0u;
}

bool CLodRayTracingSystem::RangeAllocator::Allocate(uint32_t count, uint32_t& outFirst) {
    for (auto it = m_free.begin(); it != m_free.end(); ++it) {
        if (it->second < count) continue;
        outFirst = it->first;
        const uint32_t remaining = it->second - count;
        const uint32_t next = it->first + count;
        m_free.erase(it);
        if (remaining != 0u) m_free[next] = remaining;
        m_used += count;
        return true;
    }
    return false;
}

void CLodRayTracingSystem::RangeAllocator::Free(uint32_t first, uint32_t count) {
    if (count == 0u) return;
    m_used -= std::min(m_used, count);
    auto next = m_free.lower_bound(first);
    if (next != m_free.begin()) {
        const auto previous = std::prev(next);
        if (previous->first + previous->second == first) {
            first = previous->first;
            count += previous->second;
            m_free.erase(previous);
        }
    }
    if (next != m_free.end() && first + count == next->first) {
        count += next->second;
        m_free.erase(next);
    }
    m_free[first] = count;
}

rhi::ResourceHandle CLodRayTracingSystem::GraphBuffer::Handle() const {
    return buffer->GetAPIResource().GetHandle();
}

bool CLodRayTracingSystem::CreateBuffer(GraphBuffer& out, uint64_t bytes, rhi::HeapType heap, rhi::ResourceFlags flags, const char* name, View view, uint32_t stride,
    bool highPriority) {
    const bool unorderedAccess = (static_cast<uint32_t>(flags) & rhi::RF_AllowUnorderedAccess) != 0u;
    std::shared_ptr<org::Buffer> buffer;
    if (view == View::Structured) {
        buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<uint32_t>(bytes / stride), stride, unorderedAccess, false, false, heap);
    }
    else if (view == View::Raw) {
        buffer = CreateAliasedUnmaterializedRawBuffer(bytes, unorderedAccess, false, false);
    }
    else {
        buffer = org::Buffer::CreateSharedUnmaterialized(heap, bytes, unorderedAccess);
    }
    // Persistent: the GPU keeps addresses into these, and their contents carry over between frames.
    buffer->SetAllowAlias(false);
    buffer->SetAccelerationStructureStorage((static_cast<uint32_t>(flags) & rhi::RF_RaytracingAccelerationStructure) != 0u);
    buffer->SetName(name);
    org::memory::SetResourceUsageHint(*buffer, "Cluster LOD ray tracing");
    if (highPriority) buffer->SetResidencyPriority(rhi::ResidencyPriorityHigh);
    try {
        buffer->Materialize();
    }
    catch (const std::exception& error) {
        spdlog::error("CLod RT: failed to allocate '{}' ({} MiB): {}", name, bytes >> 20u, error.what());
        return false;
    }
    out.buffer = std::move(buffer);
    out.bytes = bytes;
    out.address = m_device.GetBufferDeviceAddress({ out.Handle(), 0u });
    m_stats.totalMemoryBytes += bytes;
    m_allocations.emplace_back(name, bytes, heap);
    if (heap == rhi::HeapType::Upload || heap == rhi::HeapType::Readback) {
        void* mapped = nullptr;
        out.buffer->GetAPIResource().Map(&mapped, 0u, bytes);
        out.mapped = static_cast<uint8_t*>(mapped);
    }
    if (view != View::None) {
        out.srv = out.buffer->GetSRVInfo(0).slot.index;
        if (unorderedAccess) out.uav = out.buffer->GetUAVShaderVisibleInfo(0).slot.index;
    }
    return true;
}

bool CLodRayTracingSystem::EnsureInitialized(rhi::Device device, const RayTracingFeatureInfo& features) {
    if (m_stats.initialized) return true;
    if (!device || !features.clusterAccelerationStructure || !features.rayQuery) return false;
    m_device = device;
    m_framesInFlight = std::max<uint32_t>(1u, SettingsManager::GetInstance().getSettingGetter<uint8_t>("numFramesInFlight")());
    m_sliceHasReadback.assign(m_framesInFlight, 0u);
    m_sliceHasStats.assign(m_framesInFlight, 0u);
    m_sliceHasWatch.assign(m_framesInFlight, 0u);
    const bool dxr2 = features.backendTier == RayTracingBackendTier::DXR_2_0;
    m_ommEnabled = m_ommRequested && features.opacityMicromap && dxr2;
    if (m_ommRequested && !m_ommEnabled) {
        spdlog::warn("CLod RT: opacity micromaps requested, but this device has none; alpha tests stay in any-hit");
    }
    m_stats.ommEnabled = m_ommEnabled;
    m_clusterInfoStride = dxr2
        ? sizeof(rhi::PackedDxr2ClusterBuildTriangleInfo)
        : sizeof(rhi::PackedRayTracingClusterBuildTriangleInfo);
    const uint32_t budget = m_limits.clusterBuildBudget;

    // The build arena: an implicit-destination build of a frame's budget, at the largest a cluster can be. A transient
    // arena holds the transient budget the same way, plus a unit of rounding per cluster, as it is placed by real size.
    const auto clasPrebuild = [&](uint32_t clusters, rhi::RayTracingRtasPrebuildInfo& prebuild) {
        rhi::RayTracingClusterTriangleInputsDesc buildBatch{};
        buildBatch.vertexFormat = rhi::RayTracingClusterVertexFormat::Float32x3;
        buildBatch.indexFormat = rhi::RayTracingClusterIndexFormat::Uint8;
        buildBatch.maxClusterTriangleCount = kMaxClusterTriangles;
        buildBatch.maxClusterVertexCount = kMaxClusterVertices;
        buildBatch.maxTotalTriangleCount = kMaxClusterTriangles * clusters;
        buildBatch.maxTotalVertexCount = kMaxClusterVertices * clusters;
        if (m_ommEnabled) {
            buildBatch.opacityMicromapIndexFormat = rhi::RayTracingClusterIndexFormat::Uint32;
            buildBatch.maxOpacityMicromapIndicesPerCluster = kMaxClusterTriangles;
        }
        rhi::RayTracingRtasOperationInputs clasInputs{};
        clasInputs.allowOpacityMicromaps = m_ommEnabled;
        clasInputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
        clasInputs.operationMode = rhi::RayTracingRtasOperationMode::ImplicitDestinations;
        clasInputs.flags = rhi::RTASBuild_PreferFastTrace;
        clasInputs.maxAccelerationStructureCount = clusters;
        clasInputs.triangleClusters = &buildBatch;
        return rhi::IsOk(device.GetRayTracingAccelerationStructureOperationPrebuildInfo(clasInputs, prebuild));
    };
    rhi::RayTracingRtasPrebuildInfo buildPrebuild{};
    rhi::RayTracingRtasPrebuildInfo transientPrebuild{};
    if (!clasPrebuild(budget, buildPrebuild) || !clasPrebuild(m_limits.transientBuildBudget, transientPrebuild)) return false;
    m_arenaBytes = buildPrebuild.resultDataMaxSizeInBytes;
    m_transientSliceBytes = AlignUp(transientPrebuild.resultDataMaxSizeInBytes + uint64_t(m_limits.transientBuildBudget) * CLOD_RT_CACHE_UNIT_BYTES, kPageBytes);

    rhi::RayTracingClusterMovesDesc moves{};
    moves.type = rhi::RayTracingClusterAccelerationStructureType::TriangleCluster;
    moves.noMoveOverlap = true;
    moves.maxMovedBytes = m_arenaBytes;
    rhi::RayTracingRtasOperationInputs moveInputs{};
    moveInputs.operationType = rhi::RayTracingRtasOperationType::MoveObjects;
    moveInputs.operationMode = rhi::RayTracingRtasOperationMode::ExplicitDestinations;
    moveInputs.maxAccelerationStructureCount = budget;
    moveInputs.moveObjects = &moves;
    moveInputs.allowOpacityMicromaps = m_ommEnabled;
    rhi::RayTracingRtasPrebuildInfo movePrebuild{};
    if (!rhi::IsOk(device.GetRayTracingAccelerationStructureOperationPrebuildInfo(moveInputs, movePrebuild))) return false;

    rhi::RayTracingClusterBottomLevelInputsDesc blasBatch{ m_limits.maxClusterReferences, m_limits.maxClustersPerInstance };
    rhi::RayTracingRtasOperationInputs blasInputs{};
    blasInputs.operationType = rhi::RayTracingRtasOperationType::BuildClustersBottomLevel;
    blasInputs.flags = rhi::RTASBuild_PreferFastBuild;
    blasInputs.maxAccelerationStructureCount = m_limits.maxBlases;
    blasInputs.clustersBottomLevel = &blasBatch;
    blasInputs.allowOpacityMicromaps = m_ommEnabled;
    rhi::RayTracingRtasPrebuildInfo blasPrebuild{};
    if (!rhi::IsOk(device.GetRayTracingAccelerationStructureOperationPrebuildInfo(blasInputs, blasPrebuild))) return false;
    m_stats.blasResultBytes = blasPrebuild.resultDataMaxSizeInBytes;

    // Pages, CLAS ID chunks and entries; after the cached IDs, a region of IDs per transient arena.
    m_protectedFrames = std::min<uint32_t>(std::max(m_limits.protectedFrames, m_framesInFlight), CLOD_RT_CACHE_HISTORY - 1u);
    if (m_limits.clasPoolBytes == 0u) {
        rhi::VideoMemoryInfo memory{};
        const uint64_t budget = rhi::IsOk(device.QueryVideoMemoryInfo(0u, rhi::MemorySegmentGroup::Local, memory)) ? memory.budgetBytes : 0u;
        m_limits.clasPoolBytes = AlignUp(std::clamp<uint64_t>(budget / 6u, 768ull << 20u, 2048ull << 20u), 1ull << 20u);
    }
    m_pageCount = static_cast<uint32_t>(std::max<uint64_t>(m_limits.clasPoolBytes / kPageBytes, 64u));
    m_chunkCount = m_limits.maxClasSlots / CLOD_RT_CACHE_CHUNK_IDS;
    m_transientSlices = m_protectedFrames + 1u;
    const uint64_t clasIds = uint64_t(m_chunkCount) * CLOD_RT_CACHE_CHUNK_IDS + uint64_t(m_transientSlices) * budget;

    const rhi::ResourceFlags uavFlag = rhi::RF_AllowUnorderedAccess;
    constexpr rhi::HeapType local = rhi::HeapType::DeviceLocal;
    m_stats.totalMemoryBytes = 0u;
    m_allocations.clear();
    m_ommArrays = std::make_shared<org::ResourceGroup>("CLod RT micromap arrays");
    m_ommIndices = std::make_shared<org::ResourceGroup>("CLod RT micromap indices");
    bool ok = true;
    m_stats.clasPoolBytes = m_limits.clasPoolBytes;
    ok = ok && CreateBuffer(m_clasPool, uint64_t(m_pageCount) * kPageBytes + uint64_t(m_transientSlices) * m_transientSliceBytes, local, AsStorageFlags(),
        "CLod RT CLAS pages and transient arenas", View::None, 0u, true);
    ok = ok && CreateBuffer(m_clasInfo, clasIds * 16u, local, uavFlag, "CLod RT CLAS info", View::Structured, 16u, true);
    ok = ok && CreateBuffer(m_clasAddresses, clasIds * 8u, local, uavFlag, "CLod RT CLAS addresses", View::Structured, 8u, true);
    ok = ok && CreateBuffer(m_slotOmmKeys, clasIds * 4u, local, uavFlag, "CLod RT CLAS micromap keys", View::Structured, 4u, true);
    // The demand bitset covers CLOD_RT_DEMAND_GROUPS groups; Count ignores any past it.
    m_limits.maxGroups = std::min<uint32_t>(m_limits.maxGroups, CLOD_RT_DEMAND_GROUPS);
    ok = ok && CreateBuffer(m_groupState, uint64_t(m_limits.maxGroups) * CLOD_RT_CACHE_STATE_BYTES, local, uavFlag, "CLod RT group state", View::Raw, 0u, true);
    ok = ok && CreateBuffer(m_cacheCtrl, kCtrlBytes, local, uavFlag, "CLod RT cache control", View::Raw);
    ok = ok && CreateBuffer(m_cacheEntries, uint64_t(m_limits.maxEntries) * CLOD_RT_CACHE_ENTRY_BYTES, local, uavFlag, "CLod RT cache entries", View::Raw);
    ok = ok && CreateBuffer(m_pageNext, uint64_t(m_pageCount) * 4u, local, uavFlag, "CLod RT cache page chains", View::Structured, 4u);
    ok = ok && CreateBuffer(m_pageRefs, uint64_t(m_pageCount) * 4u, local, uavFlag, "CLod RT cache page users", View::Structured, 4u);
    ok = ok && CreateBuffer(m_cacheQueues, (uint64_t(m_pageCount) + m_chunkCount + m_limits.maxEntries) * 4u, local, uavFlag, "CLod RT cache free queues", View::Raw);
    ok = ok && CreateBuffer(m_transient, uint64_t(m_limits.maxGroups) * 8u, local, uavFlag, "CLod RT cache transient groups", View::Structured, 8u);
    ok = ok && CreateBuffer(m_cacheRanges, uint64_t(budget) * CLOD_RT_CACHE_RANGE_BYTES, local, uavFlag, "CLod RT cache ranges", View::Raw);
    ok = ok && CreateBuffer(m_cachePending, uint64_t(budget) * 4u, local, uavFlag, "CLod RT cache pending clusters", View::Structured, 4u);
    ok = ok && CreateBuffer(m_buildArgs, uint64_t(budget) * m_clusterInfoStride, local, uavFlag, "CLod RT CLAS build args", View::Structured, m_clusterInfoStride);
    const auto makeScratch = [&](uint64_t bytes, const char* name) {
        auto buffer = CreateAliasedUnmaterializedRawBuffer(AlignUp(std::max<uint64_t>(bytes, 256u), 256u), true);
        buffer->SetName(name);
        org::memory::SetResourceUsageHint(*buffer, "Cluster LOD ray tracing scratch");
        return buffer;
    };
    m_buildScratch = makeScratch(std::max(buildPrebuild.scratchDataSizeInBytes, movePrebuild.scratchDataSizeInBytes), "CLod RT CLAS scratch");
    m_buildArena = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, AlignUp(m_arenaBytes, 256u), true);
    m_buildArena->SetAccelerationStructureStorage(true);
    m_buildArena->SetAllowAlias(true);
    m_buildArena->SetName("CLod RT CLAS build arena");
    org::memory::SetResourceUsageHint(*m_buildArena, "Cluster LOD ray tracing scratch");
    ok = ok && CreateBuffer(m_arenaAddresses, uint64_t(budget) * 8u, local, uavFlag, "CLod RT arena addresses", View::Structured, 8u);
    ok = ok && CreateBuffer(m_arenaSizes, uint64_t(budget) * 8u, local, uavFlag, "CLod RT arena sizes", View::Raw);
    ok = ok && CreateBuffer(m_moveArgs, uint64_t(budget) * 8u, local, uavFlag, "CLod RT move args", View::Structured, 8u);
    ok = ok && CreateBuffer(m_moveDestinations, uint64_t(budget) * 8u, local, uavFlag, "CLod RT move destinations", View::Structured, 8u);
    ok = ok && CreateBuffer(m_packScratch, uint64_t(budget) * 8u, local, uavFlag, "CLod RT cache pack scratch", View::Structured, 4u);
    ok = ok && CreateBuffer(m_slabAddresses, uint64_t(m_limits.slabAddressEntries) * 8u, rhi::HeapType::Upload, {}, "CLod RT slab addresses", View::Structured, 8u);
    ok = ok && CreateBuffer(m_requests, uint64_t(m_limits.requestCapacity) * kRequestBytes, local, uavFlag, "CLod RT CLAS requests", View::Structured, kRequestBytes);
    ok = ok && CreateBuffer(m_demand, CLOD_RT_DEMAND_BYTES, local, uavFlag, "CLod RT CLAS demand", View::Raw);
    ok = ok && CreateBuffer(m_blasOmmKeys, uint64_t(m_limits.maxBlases) * 4u, local, uavFlag, "CLod RT BLAS micromap keys", View::Structured, 4u, true);
    ok = ok && CreateBuffer(m_drawTable, (uint64_t(m_limits.maxDrawRecords) + m_limits.maxPortalOccurrences) * 16u, local, uavFlag, "CLod RT draw table", View::Raw, 0u, true);
    ok = ok && CreateBuffer(m_instanceBlas, uint64_t(m_limits.maxInstances) * 4u, local, uavFlag, "CLod RT instance BLAS index", View::Structured, 4u, true);
    ok = ok && CreateBuffer(m_shareTable, uint64_t(m_limits.blasShareEntries) * 16u, local, uavFlag, "CLod RT BLAS sharing", View::Raw, 0u, true);
    ok = ok && CreateBuffer(m_instanceList, uint64_t(m_limits.maxInstances) * 4u, local, uavFlag, "CLod RT instance list", View::Structured, 4u, true);
    ok = ok && CreateBuffer(m_instanceRows, uint64_t(m_limits.maxInstances) * 16u, local, uavFlag, "CLod RT instance rows", View::Structured, 16u, true);
    ok = ok && CreateBuffer(m_coarseCut, CLOD_RT_COARSE_BUFFER_BYTES, local, uavFlag, "CLod RT coarse cut", View::Raw);
    ok = ok && CreateBuffer(m_counters, kCountersBytes, local, uavFlag, "CLod RT counters", View::Raw, 0u, true);
    ok = ok && CreateBuffer(m_blasArgs, uint64_t(m_limits.maxBlases) * sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo), local, uavFlag, "CLod RT BLAS args", View::Structured, sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo), true);
    ok = ok && CreateBuffer(m_blasAddressArray, uint64_t(m_limits.maxClusterReferences) * 8u, local, uavFlag, "CLod RT BLAS CLAS references", View::Structured, 8u, true);
    m_blasResult = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, AlignUp(blasPrebuild.resultDataMaxSizeInBytes, 256u), true);
    m_blasResult->SetAccelerationStructureStorage(true);
    m_blasResult->SetAllowAlias(true);
    m_blasResult->SetName("CLod RT BLAS data");
    org::memory::SetResourceUsageHint(*m_blasResult, "Cluster LOD ray tracing");
    m_blasScratch = makeScratch(blasPrebuild.scratchDataSizeInBytes, "CLod RT BLAS scratch");
    ok = ok && CreateBuffer(m_blasAddresses, uint64_t(m_limits.maxBlases) * 8u, local, uavFlag, "CLod RT BLAS addresses", View::Structured, 8u, true);
    ok = ok && CreateBuffer(m_blasSizes, uint64_t(m_limits.maxBlases) * 8u, local, uavFlag, "CLod RT BLAS sizes");
    m_tlasInstances = CreateAliasedUnmaterializedStructuredBuffer(m_limits.maxInstances, sizeof(rhi::PackedRayTracingInstanceDesc), true);
    m_tlasInstances->SetName("CLod RT TLAS instances");
    org::memory::SetResourceUsageHint(*m_tlasInstances, "Cluster LOD ray tracing scratch");
    ok = ok && CreateBuffer(m_visibilityHash, uint64_t(m_limits.visibilityHashEntries) * 8u, local, uavFlag, "CLod RT visible cluster hash", View::Raw);
    ok = ok && CreateBuffer(m_visibilityStats, kStatsBytes, local, uavFlag, "CLod RT visibility comparison", View::Raw);
    if (m_watchBright > 0.0f) {
        ok = ok && CreateBuffer(m_pixelWatch, CLOD_RT_WATCH_WORDS * 4u, local, uavFlag, "CLod RT pixel watch", View::Raw);
        spdlog::info("CLod RT: pixel watch on, bright above luminance {}", m_watchBright);
    }

    // Opacity micromaps: what the GPU found missing, what the CPU loaded, the groups to build again.
    ok = ok && CreateBuffer(m_ommTable, uint64_t(CLOD_RT_OMM_TABLE_ENTRIES) * 16u, local, uavFlag, "CLod RT micromap table", View::Structured, 16u);
    ok = ok && CreateBuffer(m_ommWanted, uint64_t(budget) * 8u, local, uavFlag, "CLod RT micromap requests", View::Structured, 8u);
    ok = ok && CreateBuffer(m_ommRebuild, 4u + uint64_t(m_limits.ommRebuildsPerFrame) * 4u, local, uavFlag, "CLod RT micromap rebuilds", View::Raw);
    ok = ok && CreateBuffer(m_ommRelease, kReleaseBytes, local, uavFlag, "CLod RT micromap releases", View::Structured, 16u);
    m_ommTableCpu.assign(CLOD_RT_OMM_TABLE_ENTRIES, {});
    m_ommSlotAllocations.assign(CLOD_RT_OMM_TABLE_ENTRIES, {});
    if (m_ommEnabled) {
        // Per frame in flight: baked micromaps, which array builds read straight from here, then table writes, then
        // the rebuild list.
        m_ommUploadSliceBytes = AlignUp(m_limits.ommUploadBytesPerFrame + uint64_t(m_limits.ommTableWritesPerFrame) * 16u
            + 4u + uint64_t(m_limits.ommRebuildsPerFrame) * 4u, kOmmUploadAlignment);
        ok = ok && CreateBuffer(m_ommUpload, m_ommUploadSliceBytes * m_framesInFlight, rhi::HeapType::Upload, {}, "CLod RT baked micromaps");
        m_ommScratch = makeScratch(AlignUp(m_limits.ommScratchBytes, kOmmScratchAlignment), "CLod RT micromap scratch");
        m_ommScope = TaskSchedulerManager::GetInstance().CreateScope("CLodRayTracingMicromaps");
    }

    // The scene build's slice: counters, the cache's control words, timers, micromap requests and releases.
    m_readbackSliceBytes = AlignUp(uint64_t(kCountersBytes) + kCtrlBytes + kSceneTimers * 8u + uint64_t(budget) * 8u + kReleaseBytes, 256u);
    ok = ok && CreateBuffer(m_readback, m_readbackSliceBytes * m_framesInFlight, rhi::HeapType::Readback, {}, "CLod RT readback");
    ok = ok && CreateBuffer(m_statsReadback, uint64_t(kStatsSliceBytes) * m_framesInFlight, rhi::HeapType::Readback, {}, "CLod RT visibility comparison readback");
    if (m_watchBright > 0.0f) {
        ok = ok && CreateBuffer(m_watchReadback, uint64_t(kWatchSliceBytes) * m_framesInFlight, rhi::HeapType::Readback, {}, "CLod RT pixel watch readback");
    }
    if (!ok) return false;
    std::memset(m_slabAddresses.mapped, 0, m_slabAddresses.bytes);

    rhi::RayTracingGeometryDesc tlasGeometry{};
    tlasGeometry.type = rhi::RayTracingGeometryType::Instances;
    // Sizing reads no instances, but wants a buffer: the rows the TLAS pass writes into are placed by the graph, not made yet.
    tlasGeometry.instances.instanceBuffer = { m_instanceRows.Handle(), 0u, m_instanceRows.bytes };
    tlasGeometry.instances.stride = sizeof(rhi::PackedRayTracingInstanceDesc);
    tlasGeometry.instances.count = m_limits.maxInstances;
    rhi::AccelerationStructureBuildInputs tlasInputs{};
    tlasInputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
    tlasInputs.flags = rhi::RTASBuild_PreferFastBuild;
    tlasInputs.geometries = { &tlasGeometry, 1u };
    rhi::AccelerationStructurePrebuildInfo tlasPrebuild{};
    if (!rhi::IsOk(device.GetAccelerationStructurePrebuildInfo(tlasInputs, tlasPrebuild))) return false;
    m_stats.tlasBytes = tlasPrebuild.resultDataMaxSizeInBytes;
    ok = ok && CreateBuffer(m_tlasStorage, tlasPrebuild.resultDataMaxSizeInBytes, local, AsStorageFlags(), "CLod RT TLAS", View::None, 0u, true);
    m_tlasScratch = makeScratch(tlasPrebuild.scratchDataSizeInBytes, "CLod RT TLAS scratch");
    if (!ok) return false;

    rhi::AccelerationStructureDesc tlasDesc{};
    tlasDesc.type = rhi::RayTracingAccelerationStructureType::TopLevel;
    tlasDesc.storage = m_tlasStorage.Handle();
    tlasDesc.sizeBytes = tlasPrebuild.resultDataMaxSizeInBytes;
    tlasDesc.flags = rhi::RTASBuild_PreferFastBuild;
    tlasDesc.debugName = "CLod RT TLAS";
    if (!rhi::IsOk(device.CreateAccelerationStructure(tlasDesc, m_tlas)) || !m_tlas) return false;

    auto& descriptorHeap = org::DescriptorHeapManager::GetInstance().GetCBVSRVUAVHeap();
    m_tlasSrv = descriptorHeap->AllocateDescriptor();
    rhi::SrvDesc tlasSrv{};
    tlasSrv.dimension = rhi::SrvDim::AccelerationStruct;
    // A union member: SrvDesc{} leaves these fields unset.
    tlasSrv.accel.deviceAddress = 0u;
    tlasSrv.accel.address = {};
    tlasSrv.accel.partitioned = false;
    tlasSrv.accel.accelerationStructure = m_tlas->GetHandle();
    tlasSrv.accel.sizeBytes = tlasPrebuild.resultDataMaxSizeInBytes;
    if (!rhi::IsOk(device.CreateShaderResourceView({ descriptorHeap->GetHeap().GetHandle(), m_tlasSrv }, {}, tlasSrv))) return false;

    rhi::QueryPoolDesc timerDesc{};
    timerDesc.type = rhi::QueryType::Timestamp;
    timerDesc.count = kSceneTimers * m_framesInFlight;
    if (rhi::IsOk(device.CreateQueryPool(timerDesc, m_timers)) && m_timers) {
        m_timerTicksPerSecond = device.GetTimestampCalibration(rhi::QueueKind::Compute).ticksPerSecond;
    }

    m_tlasInstanceCount = m_limits.maxInstances;
    m_stats.initialized = true;
    if (m_ommEnabled) {
        spdlog::info("CLod RT: baked opacity micromaps, {} MiB of arrays, {} clusters of indices, {} MiB uploaded a frame at most",
            m_limits.ommArrayPoolBytes >> 20u, m_limits.ommIndexUnits, m_limits.ommUploadBytesPerFrame >> 20u);
    }
    spdlog::info(
        "CLod RT: {} MiB CLAS cache in {} pages of {} KiB, {} CLAS IDs, {} entries, {} clusters built a frame into a {} MiB arena, {} of them transient in {} arenas of {} MiB, BLAS batch {} MiB + {} MiB scratch, TLAS {} MiB + {} MiB scratch, total {} MiB",
        (uint64_t(m_pageCount) * kPageBytes) >> 20u, m_pageCount, kPageBytes >> 10u, uint64_t(m_chunkCount) * CLOD_RT_CACHE_CHUNK_IDS, m_limits.maxEntries, budget, m_arenaBytes >> 20u,
        m_limits.transientBuildBudget, m_transientSlices, m_transientSliceBytes >> 20u,
        blasPrebuild.resultDataMaxSizeInBytes >> 20u, blasPrebuild.scratchDataSizeInBytes >> 20u,
        tlasPrebuild.resultDataMaxSizeInBytes >> 20u, tlasPrebuild.scratchDataSizeInBytes >> 20u, m_stats.totalMemoryBytes >> 20u);
    // The render graph's memory introspection does not see these buffers: name the large ones here.
    std::sort(m_allocations.begin(), m_allocations.end(), [](const auto& a, const auto& b) { return std::get<1>(a) > std::get<1>(b); });
    std::string largest;
    uint64_t smallBytes = 0;
    for (const auto& [name, bytes, heap] : m_allocations) {
        if (bytes < (4ull << 20u)) { smallBytes += bytes; continue; }
        largest += std::format("{}{} {:.1f}{}", largest.empty() ? "" : "; ", name, double(bytes) / 1048576.0,
            heap == rhi::HeapType::DeviceLocal ? "" : " (host)");
    }
    spdlog::info("CLod RT buffers (MiB): {}; {:.1f} in smaller ones", largest, double(smallBytes) / 1048576.0);
    return true;
}

std::shared_ptr<org::Buffer> CLodRayTracingSystem::GetCoarseCut() const {
    return m_coarseCut.buffer;
}

void CLodRayTracingSystem::RegisterGraphResources(org::RenderGraph& graph) {
    graph.RegisterResource(Builtin::RayTracing::Tlas, m_tlasStorage.buffer);
    graph.RegisterResource(Builtin::RayTracing::BlasData, m_blasResult);
    graph.RegisterResource(Builtin::RayTracing::ClasPool, m_clasPool.buffer);
    graph.RegisterResource(Builtin::RayTracing::ClasInfo, m_clasInfo.buffer);
    graph.RegisterResource(Builtin::RayTracing::InstanceRows, m_instanceRows.buffer);
    graph.RegisterResolver(Builtin::RayTracing::MicromapArrays, std::make_shared<ResourceGroupResolver>(m_ommArrays));
}

void CLodRayTracingSystem::DeclareTracing(org::RenderPassBuilder& builder) {
    // Rays reach the BLAS batch through the TLAS, the CLAS in the pool through the BLASes, and micromaps through those.
    builder.AccelerationStructureRead(Builtin::RayTracing::Tlas);
    builder.AccelerationStructureRead(Builtin::RayTracing::BlasData);
    builder.AccelerationStructureRead(Builtin::RayTracing::ClasPool);
    builder.AccelerationStructureRead(Builtin::RayTracing::MicromapArrays);
    builder.ShaderResource(Builtin::RayTracing::ClasInfo);
    builder.ShaderResource(Builtin::RayTracing::InstanceRows);
}

void CLodRayTracingSystem::DeclareSceneClas(org::RenderPassBuilder& builder) const {
    // The first frame's clears, last frame's cleanup, count, and the cache's passes.
    for (const GraphBuffer* buffer : { &m_groupState, &m_cacheCtrl, &m_cacheEntries, &m_pageNext, &m_pageRefs, &m_cacheQueues, &m_transient, &m_cacheRanges,
             &m_cachePending, &m_buildArgs, &m_moveArgs, &m_moveDestinations, &m_packScratch, &m_requests, &m_demand, &m_clasInfo, &m_clasAddresses,
             &m_slotOmmKeys, &m_blasOmmKeys, &m_drawTable, &m_instanceList, &m_instanceRows, &m_shareTable, &m_counters, &m_ommTable, &m_ommWanted,
             &m_ommRebuild, &m_ommRelease }) {
        builder.UnorderedAccess(buffer->buffer);
    }
    builder.ShaderResource(m_slabAddresses.buffer);
    // The CLAS build and move.
    builder.AccelerationStructureBuild(m_buildArena);
    builder.AccelerationStructureBuild(m_clasPool.buffer);
    builder.AccelerationStructureBuildOutput(m_buildScratch);
    builder.AccelerationStructureBuildOutput(m_arenaAddresses.buffer);
    builder.AccelerationStructureBuildOutput(m_arenaSizes.buffer);
    // Micromap arrays are built from the upload heap at the start of the pass, and the CLAS built reference them.
    builder.AccelerationStructureBuild(ResourceGroupResolver(m_ommArrays));
    builder.AccelerationStructureBuildInput(ResourceGroupResolver(m_ommIndices));
    if (m_ommUpload) {
        builder.CopySource(m_ommUpload.buffer);
        builder.AccelerationStructureBuildOutput(m_ommScratch);
    }
    // As RecordCache leaves them.
    for (const GraphBuffer* buffer : { &m_buildArgs, &m_moveArgs, &m_moveDestinations, &m_cacheCtrl, &m_arenaAddresses, &m_arenaSizes }) {
        builder.InternalTransition(buffer->buffer, kLeftAsBuildInput);
    }
}

void CLodRayTracingSystem::DeclareSceneBlas(org::RenderPassBuilder& builder) const {
    // Allocate, coarse consumers, place.
    for (const GraphBuffer* buffer : { &m_counters, &m_drawTable, &m_instanceList, &m_instanceBlas, &m_shareTable, &m_instanceRows, &m_coarseCut,
             &m_cacheCtrl, &m_cacheEntries, &m_transient, &m_groupState, &m_clasAddresses, &m_slotOmmKeys, &m_blasOmmKeys, &m_blasArgs,
             &m_blasAddressArray }) {
        builder.UnorderedAccess(buffer->buffer);
    }
    // The BLAS build, from the CLAS in the pool.
    builder.AccelerationStructureRead(m_clasPool.buffer);
    builder.AccelerationStructureBuild(m_blasResult);
    builder.AccelerationStructureBuildOutput(m_blasScratch);
    builder.AccelerationStructureBuildOutput(m_blasAddresses.buffer);
    builder.AccelerationStructureBuildOutput(m_blasSizes.buffer);
    for (const GraphBuffer* buffer : { &m_blasArgs, &m_blasAddressArray, &m_counters }) {
        builder.InternalTransition(buffer->buffer, kLeftAsBuildInput);
    }
}

org::DeclaredViewToken CLodRayTracingSystem::DeclareSceneTlas(org::RenderPassBuilder& builder) const {
    // Write instances, the readback, the coarse cut's clear, the end of frame.
    for (const GraphBuffer* buffer : { &m_counters, &m_instanceBlas, &m_instanceRows, &m_shareTable, &m_blasOmmKeys, &m_cacheCtrl, &m_ommWanted,
             &m_ommRelease, &m_ommRebuild, &m_coarseCut }) {
        builder.UnorderedAccess(buffer->buffer);
    }
    builder.ShaderResource(m_blasAddresses.buffer);
    const org::DeclaredViewToken rows = builder.UnorderedAccess(m_tlasInstances).View();
    // The TLAS build.
    builder.AccelerationStructureRead(m_blasResult);
    builder.AccelerationStructureBuild(m_tlasStorage.buffer);
    builder.AccelerationStructureBuildOutput(m_tlasScratch);
    builder.CopyDestination(m_readback.buffer);
    builder.InternalTransition(m_tlasInstances, kLeftAsBuildInput);
    for (const GraphBuffer* buffer : { &m_counters, &m_ommWanted, &m_ommRelease }) {
        builder.InternalTransition(buffer->buffer, kLeftAsCopySource);
    }
    return rows;
}

void CLodRayTracingSystem::DeclareVisibility(org::RenderPassBuilder& builder, bool readStats) const {
    builder.UnorderedAccess(m_visibilityHash.buffer);
    builder.UnorderedAccess(m_visibilityStats.buffer);
    if (readStats) builder.CopyDestination(m_statsReadback.buffer);
}

void CLodRayTracingSystem::DeclarePixelWatch(org::RenderPassBuilder& builder) const {
    if (!m_pixelWatch) return;
    builder.UnorderedAccess(m_pixelWatch.buffer);
    builder.CopyDestination(m_watchReadback.buffer);
}

CLodRayTracingSystem::VisibilityDescriptors CLodRayTracingSystem::GetVisibilityDescriptors() const noexcept {
    return { m_tlasSrv, m_clasInfo.srv, m_instanceRows.srv, m_visibilityHash.uav, m_limits.visibilityHashEntries, m_visibilityStats.uav };
}

void CLodRayTracingSystem::SetPrimaryVisibleClusters(std::shared_ptr<org::Buffer> clusters, std::shared_ptr<org::Buffer> transformIndices, uint32_t capacity) {
    m_primaryVisibleClusters = std::move(clusters);
    m_primaryTransformIndices = std::move(transformIndices);
    m_primaryVisibleClusterCapacity = capacity;
}

void CLodRayTracingSystem::BeginFrame(MeshManager& meshManager) {
    if (!m_stats.initialized) return;
    BT_ZONE_SCOPE("CLodRT::BeginFrame");

    // New page slabs: the GPU builds CLAS from their device addresses. Slabs are only ever added.
    if (PagePool* pool = meshManager.GetCLodPagePool()) {
        const uint32_t slabs = pool->GetSlabCount();
        for (; m_knownSlabs < slabs; ++m_knownSlabs) {
            const auto slab = pool->GetSlab(m_knownSlabs);
            if (!slab) continue;
            const uint32_t descriptor = slab->GetSRVInfo(0).slot.index;
            if (descriptor >= m_limits.slabAddressEntries) {
                spdlog::error("CLod RT: page slab descriptor {} is past the slab address table's {}; its pages get no CLAS", descriptor, m_limits.slabAddressEntries);
                continue;
            }
            const uint64_t address = m_device.GetBufferDeviceAddress({ slab->GetAPIResource().GetHandle(), 0u });
            std::memcpy(m_slabAddresses.mapped + uint64_t(descriptor) * 8u, &address, 8u);
        }
    }

    // This slice was last written framesInFlight frames ago, so its readback has landed.
    const uint32_t sliceIndex = m_frame % m_framesInFlight;
    if (m_sliceHasReadback[sliceIndex] == 0u) {
        if (OpacityMicromapsActive()) StageOmmLoads();
        return;
    }
    m_sliceHasReadback[sliceIndex] = 0u;
    const uint8_t* base = m_readback.mapped + SliceOffset(m_readbackSliceBytes, sliceIndex);
    uint32_t counters[CLOD_RT_COUNTER_WORDS]{};
    std::memcpy(counters, base, sizeof(counters));
    m_stats.instances = std::min(counters[CLOD_RT_COUNTER_INSTANCES], m_limits.maxInstances);
    m_stats.clustersInBvh = counters[CLOD_RT_COUNTER_CLUSTERS];
    m_stats.distinctBlases = counters[CLOD_RT_COUNTER_BLAS_ARGS];
    m_stats.droppedInstances = counters[CLOD_RT_COUNTER_DROPPED_INSTANCES];
    m_stats.droppedClusters = counters[CLOD_RT_COUNTER_DROPPED_CLUSTERS];
    m_stats.droppedBlases = counters[CLOD_RT_COUNTER_DROPPED_BLASES];
    m_stats.listClusters = counters[CLOD_RT_COUNTER_LIST_CLUSTERS];
    m_stats.coarseConsumers = counters[CLOD_RT_COUNTER_COARSE_CONSUMERS];
    m_stats.alphaInstances = counters[CLOD_RT_COUNTER_ALPHA_INSTANCES];
    m_stats.ommInstances = counters[CLOD_RT_COUNTER_OMM_INSTANCES];
    m_stats.ommDisabledInstances = counters[CLOD_RT_COUNTER_OMM_DISABLED_INSTANCES];
    if (counters[CLOD_RT_COUNTER_REJECTED_CLAS_ARGS] != m_stats.rejectedClasArgs) {
        spdlog::warn("CLod RT: {} cluster(s) had build arguments outside the batch limits and were built empty ({} in all)",
            counters[CLOD_RT_COUNTER_REJECTED_CLAS_ARGS] - m_stats.rejectedClasArgs, counters[CLOD_RT_COUNTER_REJECTED_CLAS_ARGS]);
        m_stats.rejectedClasArgs = counters[CLOD_RT_COUNTER_REJECTED_CLAS_ARGS];
    }

    if (m_sliceHasStats[sliceIndex] != 0u) {
        std::memcpy(m_stats.comparison.data(), m_statsReadback.mapped + SliceOffset(kStatsSliceBytes, sliceIndex), kStatsBytes);
        m_stats.comparisonValid = true;
    }
    std::array<uint32_t, CLOD_RT_WATCH_HEADER_WORDS> watchCounts{};
    if (m_sliceHasWatch[sliceIndex] != 0u) {
        m_sliceHasWatch[sliceIndex] = 0u;
        const auto* words = reinterpret_cast<const uint32_t*>(m_watchReadback.mapped + SliceOffset(kWatchSliceBytes, sliceIndex));
        std::memcpy(watchCounts.data(), words, sizeof(watchCounts));
        ReadPixelWatch(words);
    }

    uint32_t ctrl[CLOD_RT_CTRL_WORDS]{};
    std::memcpy(ctrl, base + kReadbackCtrl, sizeof(ctrl));
    m_stats.requestsLastFrame = ctrl[CLOD_RT_CTRL_REQUESTS];
    m_stats.persistentLastFrame = ctrl[CLOD_RT_CTRL_PERSISTENT];
    m_stats.transientLastFrame = ctrl[CLOD_RT_CTRL_TRANSIENT];
    m_stats.transientClustersLastFrame = ctrl[CLOD_RT_CTRL_TRANSIENT_CLUSTERS];
    m_stats.refusedLastFrame = ctrl[CLOD_RT_CTRL_REFUSED];
    m_stats.evictedLastFrame = ctrl[CLOD_RT_CTRL_EVICTED];
    m_stats.staleLastFrame = ctrl[CLOD_RT_CTRL_STALE];
    m_stats.clasBuildsLastFrame = ctrl[CLOD_RT_CTRL_BUILD_ARG_COUNT];
    m_stats.clasBuildsTotal = ctrl[CLOD_RT_CTRL_BUILT_TOTAL];
    m_stats.clasFreedTotal = ctrl[CLOD_RT_CTRL_FREED_TOTAL];
    m_stats.evictedTotal = ctrl[CLOD_RT_CTRL_EVICTED_TOTAL];
    m_stats.transientClustersTotal = ctrl[CLOD_RT_CTRL_TRANSIENT_TOTAL];
    m_stats.admittedLastFrame = ctrl[CLOD_RT_CTRL_ADMITTED];
    m_stats.notBuiltLastFrame = ctrl[CLOD_RT_CTRL_NOT_BUILT];
    m_stats.discardedLastFrame = ctrl[CLOD_RT_CTRL_DISCARDED];
    m_stats.discardedClustersLastFrame = ctrl[CLOD_RT_CTRL_DISCARDED_CLUSTERS];
    m_stats.notBuiltTotal = ctrl[CLOD_RT_CTRL_NOT_BUILT_TOTAL];
    m_stats.discardedTotal = ctrl[CLOD_RT_CTRL_DISCARDED_TOTAL];
    m_stats.missingClustersLastFrame = ctrl[CLOD_RT_CTRL_MISSING_TOTAL] - static_cast<uint32_t>(m_stats.missingClustersTotal);
    m_stats.missingClustersTotal = ctrl[CLOD_RT_CTRL_MISSING_TOTAL];
    m_stats.cachedGroups = ctrl[CLOD_RT_CTRL_CACHED_GROUPS];
    m_stats.cachedPages = ctrl[CLOD_RT_CTRL_CACHED_PAGES];
    m_stats.cachedClusters = ctrl[CLOD_RT_CTRL_CACHED_CLUSTERS];
    m_stats.clasBytesUsed = uint64_t(m_stats.cachedPages) * kPageBytes;
    m_stats.packingWaste = m_stats.cachedPages == 0u ? 0.0f
        : 1.0f - float(ctrl[CLOD_RT_CTRL_CACHED_UNITS]) / (float(m_stats.cachedPages) * float(CLOD_RT_CACHE_PAGE_UNITS));
    m_stats.freePages = ctrl[CLOD_RT_CTRL_PAGE_TAIL] - ctrl[CLOD_RT_CTRL_PAGE_HEAD];
    m_stats.freeChunks = ctrl[CLOD_RT_CTRL_CHUNK_TAIL] - ctrl[CLOD_RT_CTRL_CHUNK_HEAD];
    m_stats.freeEntries = ctrl[CLOD_RT_CTRL_ENTRY_TAIL] - ctrl[CLOD_RT_CTRL_ENTRY_HEAD];
    m_stats.keepAge = ctrl[CLOD_RT_CTRL_KEEP_AGE];
    if (ctrl[CLOD_RT_CTRL_OVERSIZE] != m_stats.oversizeClusters) {
        spdlog::warn("CLod RT: {} cluster(s) had a CLAS larger than a {} B page and stay transient ({} in all)",
            ctrl[CLOD_RT_CTRL_OVERSIZE] - m_stats.oversizeClusters, kPageBytes, ctrl[CLOD_RT_CTRL_OVERSIZE]);
        m_stats.oversizeClusters = ctrl[CLOD_RT_CTRL_OVERSIZE];
    }
    m_stats.rebuiltLastFrame = ctrl[CLOD_RT_CTRL_REBUILT];
    if (m_ommEnabled) {
        // Releases first: a group retired and asked for again in one frame loads again.
        const auto* released = reinterpret_cast<const uint32_t*>(base + kReadbackOmmWanted + uint64_t(m_limits.clusterBuildBudget) * 8u);
        ReleaseOmmEntries(released, std::min<uint32_t>(ctrl[CLOD_RT_CTRL_OMM_RELEASED], CLOD_RT_OMM_RETIRE_WINDOW_MAX));
    }
    if (OpacityMicromapsActive()) {
        const auto* wanted = reinterpret_cast<const uint32_t*>(base + kReadbackOmmWanted);
        RequestOmmLoads(meshManager, wanted, std::min(ctrl[CLOD_RT_CTRL_OMM_WANTED], m_limits.clusterBuildBudget));
        StageOmmLoads();
    }
    if (m_timerTicksPerSecond != 0u) {
        uint64_t ticks[kSceneTimers]{};
        std::memcpy(ticks, base + kReadbackTimers, sizeof(ticks));
        const double msPerTick = 1000.0 / static_cast<double>(m_timerTicksPerSecond);
        const auto stage = [&](uint32_t index) { return static_cast<float>(static_cast<double>(ticks[index + 1u] - ticks[index]) * msPerTick); };
        m_stats.msCount = stage(0u);
        m_stats.msClasBuild = stage(1u);
        m_stats.msAllocate = stage(2u);
        m_stats.msPlace = stage(3u);
        m_stats.msBlasBuild = stage(4u);
        m_stats.msWriteInstances = stage(5u);
        m_stats.msTlasBuild = stage(6u);
    }

    if (m_frameCsv) {
        const Stats& c = m_stats;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(
            static_cast<int64_t>(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) - m_frameCsvStartTicks))).count();
        float maxLuminance = 0.0f;
        std::memcpy(&maxLuminance, &watchCounts[CLOD_RT_WATCH_MAX_LUMINANCE], 4u);
        std::fprintf(m_frameCsv.get(), "%.1f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%.1f,%u\n",
            ms, m_frame, c.instances, c.coarseConsumers, c.clustersInBvh, c.requestsLastFrame, c.persistentLastFrame, c.transientLastFrame,
            c.transientClustersLastFrame, c.refusedLastFrame, c.evictedLastFrame, c.staleLastFrame,
            c.admittedLastFrame, c.notBuiltLastFrame, c.discardedLastFrame, c.discardedClustersLastFrame, c.missingClustersLastFrame,
            c.cachedGroups, c.cachedPages, c.freePages, c.clasBuildsLastFrame,
            c.msCount, c.msClasBuild, c.msAllocate, c.msPlace, c.msBlasBuild, c.msWriteInstances, c.msTlasBuild,
            watchCounts[CLOD_RT_WATCH_BRIGHT_PIXEL], watchCounts[CLOD_RT_WATCH_BAD_MATERIAL], watchCounts[CLOD_RT_WATCH_DARK_TERRAIN_PIXEL],
            watchCounts[CLOD_RT_WATCH_RESOLVE_FAILED], watchCounts[CLOD_RT_WATCH_DARK_TERRAIN_ALBEDO], watchCounts[CLOD_RT_WATCH_PRIMARY_ENDED],
            maxLuminance, watchCounts[CLOD_RT_WATCH_NON_FINITE]);
    }

    if ((m_frame % 120u) == 0u) LogStats();
}

bool CLodRayTracingSystem::OmmArraySize(const std::array<uint32_t, 13>& levelCounts, uint64_t& resultBytes, uint64_t& scratchBytes) {
    if (const auto it = m_ommArraySizes.find(levelCounts); it != m_ommArraySizes.end()) {
        resultBytes = it->second.first;
        scratchBytes = it->second.second;
        return true;
    }
    std::vector<rhi::RayTracingOpacityMicromapHistogramEntry> histogram;
    for (uint32_t level = 0u; level < levelCounts.size(); ++level) {
        if (levelCounts[level] != 0u) histogram.push_back({ levelCounts[level], level, rhi::RayTracingOpacityMicromapFormat::OC1_4State });
    }
    rhi::RayTracingOpacityMicromapArrayInputs inputs{};
    inputs.flags = rhi::RTASBuild_PreferFastTrace;
    inputs.histogram = { histogram.data(), static_cast<uint32_t>(histogram.size()) };
    rhi::AccelerationStructurePrebuildInfo prebuild{};
    if (histogram.empty() || !rhi::IsOk(m_device.GetOpacityMicromapArrayPrebuildInfo(inputs, prebuild))) {
        spdlog::error("CLod RT: no prebuild information for a micromap array");
        return false;
    }
    resultBytes = AlignUp(prebuild.resultDataMaxSizeInBytes, CLOD_RT_OMM_ARRAY_ALIGNMENT);
    scratchBytes = AlignUp(std::max<uint64_t>(prebuild.scratchDataSizeInBytes, 1u), kOmmScratchAlignment);
    m_ommArraySizes.emplace(levelCounts, std::make_pair(resultBytes, scratchBytes));
    return true;
}

void CLodRayTracingSystem::RequestOmmLoads(MeshManager& meshManager, const uint32_t* wanted, uint32_t count) {
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t group = wanted[i * 2u];
        const uint32_t identity = wanted[i * 2u + 1u];
        if (!m_ommKnown.insert(group).second) continue;
        CLodRayTracingGroupLookup lookup{};
        uint32_t clusters = 0u;
        if (meshManager.GetCLodRayTracingGroup(group, lookup)) {
            for (const ClusterLODGroupSegment& segment : lookup.group.segments) clusters += segment.meshletCount;
        }
        if (clusters == 0u || lookup.containerPath.empty()) {
            OmmLoad none{};
            none.group = group;
            none.identity = identity;
            m_ommWaiting.push_back(std::move(none));
            continue;
        }
        SubmitOmmOpen(group, identity, clusters, lookup.group.groupLocalIndex, lookup.containerPath);
    }
}

void CLodRayTracingSystem::SubmitOmmOpen(uint32_t group, uint32_t identity, uint32_t clusters, uint32_t localIndex, const std::wstring& containerPath) {
    {
        std::scoped_lock lock(m_ommMutex);
        ++m_ommOpening;
    }
    // Opening goes through the virtual file system and can take milliseconds: never on the frame.
    const bool submitted = TaskSchedulerManager::GetInstance().SubmitBlockingIo(
        m_ommScope, TaskDomain::GeometryStreaming, "CLodRT::OmmOpen",
        [this, group, identity, clusters, localIndex, containerPath](const TaskContext& context) {
            OmmLoad load{};
            load.group = group;
            load.identity = identity;
            load.localIndex = localIndex;
            load.containerPath = containerPath;
            if (!context.StopRequested()) {
                BT_ZONE_SCOPE("CLodRT::OmmOpen");
                const std::wstring path = CLodCache::OpacityMicromapPath(containerPath, identity);
                std::shared_ptr<const CLodCache::OpacityMicromapFile> file;
                bool cached = false;
                {
                    std::scoped_lock lock(m_ommMutex);
                    if (const auto it = m_ommFiles.find(path); it != m_ommFiles.end()) {
                        file = it->second.file;
                        cached = true;
                    }
                }
                if (!cached) {
                    std::string reason;
                    file = CLodCache::OpacityMicromapFile::Open(path, identity, CLodCache::OpacityMicromapContainerBytes(containerPath), &reason);
                    if (!file && reason != "no file") {
                        spdlog::warn("CLod RT: ignoring '{}': {}", std::filesystem::path(path).filename().string(), reason);
                    }
                    else if (!file && std::getenv("SARP_RT_OMM_LOG_MISSING") != nullptr) {
                        spdlog::info("CLod RT: no micromaps baked: '{}'", std::filesystem::path(path).string());
                    }
                    std::scoped_lock lock(m_ommMutex);
                    m_ommFiles.try_emplace(path, OmmFile{ file, 0u, 0u });
                }
                CLodCache::OpacityMicromapGroupView view{};
                const bool inFile = file && file->Group(localIndex, view);
                load.noneReason = !file ? OmmLoad::NoFile : !inFile ? OmmLoad::NoGroup : OmmLoad::ClusterMismatch;
                if (inFile && view.clusterCount == clusters) {
                    load.none = false;
                    load.clusters = clusters;
                    load.descCount = static_cast<uint32_t>(view.descs.size());
                    load.levelCounts = view.levelCounts;
                    load.indices.assign(view.indices.begin(), view.indices.end());
                    const auto descBytes = std::as_bytes(view.descs);
                    load.descs.assign(descBytes.begin(), descBytes.end());
                    load.data.assign(view.data.begin(), view.data.end());
                    load.path = path;
                }
            }
            std::scoped_lock lock(m_ommMutex);
            m_ommLoaded.push_back(std::move(load));
            --m_ommOpening;
        },
        TaskLane::Background, [](const TaskContext&) {});
    if (!submitted) {
        std::scoped_lock lock(m_ommMutex);
        --m_ommOpening;
        m_ommKnown.erase(group);
    }
}

void CLodRayTracingSystem::ReleaseOmmFile(const std::wstring& path) {
    if (path.empty()) return;
    std::scoped_lock lock(m_ommMutex);
    const auto it = m_ommFiles.find(path);
    if (it == m_ommFiles.end() || it->second.entries == 0u) return;
    if (--it->second.entries == 0u) it->second.idleSince = ++m_ommFileClock;
}

void CLodRayTracingSystem::ReleaseOmmEntries(const uint32_t* released, uint32_t count) {
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t slot = released[i * 4u] & (CLOD_RT_OMM_TABLE_ENTRIES - 1u);
        const uint32_t groupPlusOne = released[i * 4u + 1u];
        auto& entry = m_ommTableCpu[slot];
        if (entry[0] != groupPlusOne || groupPlusOne == 0u || groupPlusOne == CLOD_RT_OMM_TOMBSTONE) continue; // Not what this side put there.
        OmmSlotAllocation& allocation = m_ommSlotAllocations[slot];
        if (allocation.units != 0u) m_ommIndexAllocator.Free(allocation.firstUnit, allocation.units);
        if (allocation.arrayUnits != 0u) m_ommArrayAllocator.Free(allocation.arrayFirst, allocation.arrayUnits);
        ReleaseOmmFile(allocation.path);
        allocation = {};
        entry = { CLOD_RT_OMM_TOMBSTONE, 0u, CLOD_RT_OMM_NONE, CLOD_RT_OMM_NONE };
        m_ommKnown.erase(groupPlusOne - 1u);
        ++m_stats.ommReleasedTotal;
        m_ommFreedSinceRetry = true;
    }
}

bool CLodRayTracingSystem::InsertOmmEntry(uint32_t group, const std::array<uint32_t, 4>& entry) {
    // The group's own slot if it has one, else the first free or acknowledged tombstone on its probe sequence.
    uint32_t free = CLOD_RT_CACHE_INVALID;
    uint32_t slot = OmmHash(group);
    for (uint32_t probe = 0u; probe < CLOD_RT_OMM_TABLE_PROBES; ++probe) {
        const auto& existing = m_ommTableCpu[slot];
        if (existing[0] == group + 1u) {
            free = slot;
            break;
        }
        if ((existing[0] == 0u || existing[0] == CLOD_RT_OMM_TOMBSTONE) && free == CLOD_RT_CACHE_INVALID) free = slot;
        if (existing[0] == 0u) break;
        slot = (slot + 1u) & (CLOD_RT_OMM_TABLE_ENTRIES - 1u);
    }
    if (free != CLOD_RT_CACHE_INVALID) {
        m_ommTableCpu[free] = entry;
        m_ommTableWrites.push_back(free);
        m_ommInsertedSlot = free;
        return true;
    }
    static bool logged = false;
    if (!logged) {
        spdlog::warn("CLod RT: the micromap table has no room near group {}; more groups trace alpha in any-hit", group);
        logged = true;
    }
    return false;
}

void CLodRayTracingSystem::StageOmmLoads() {
    if (m_ommStaged) return; // Last frame's staging was never recorded: its upload slice is still pending.
    {
        std::scoped_lock lock(m_ommMutex);
        for (OmmLoad& load : m_ommLoaded) m_ommWaiting.push_back(std::move(load));
        m_ommLoaded.clear();
        // Files no loaded group needs: the most recently used few stay open.
        uint32_t idle = 0u;
        for (const auto& [path, file] : m_ommFiles) idle += file.file && file.entries == 0u ? 1u : 0u;
        if (idle > m_limits.ommIdleFiles + m_limits.ommIdleFiles / 4u) {
            std::vector<uint64_t> ages;
            for (const auto& [path, file] : m_ommFiles) {
                if (file.file && file.entries == 0u) ages.push_back(file.idleSince);
            }
            std::nth_element(ages.begin(), ages.begin() + (idle - m_limits.ommIdleFiles), ages.end());
            const uint64_t cutoff = ages[idle - m_limits.ommIdleFiles];
            for (auto it = m_ommFiles.begin(); it != m_ommFiles.end();) {
                if (it->second.file && it->second.entries == 0u && it->second.idleSince < cutoff) it = m_ommFiles.erase(it);
                else ++it;
            }
        }
        uint32_t open = 0u;
        for (const auto& [path, file] : m_ommFiles) open += file.file ? 1u : 0u;
        m_stats.ommFilesOpen = open;
        m_stats.ommPending = m_ommOpening + static_cast<uint32_t>(m_ommWaiting.size());
    }
    // Loads that found no room, once something was freed.
    if (m_ommFreedSinceRetry && !m_ommRetries.empty()) {
        const size_t batch = std::min<size_t>(m_ommRetries.size(), 256u);
        for (size_t i = m_ommRetries.size() - batch; i < m_ommRetries.size(); ++i) {
            const OmmRetry& retry = m_ommRetries[i];
            SubmitOmmOpen(retry.group, retry.identity, retry.clusters, retry.localIndex, retry.containerPath);
        }
        m_ommRetries.resize(m_ommRetries.size() - batch);
        m_ommFreedSinceRetry = false;
    }
    const uint64_t sliceBase = SliceOffset(m_ommUploadSliceBytes, m_frame % m_framesInFlight);
    size_t done = 0u;
    for (; done < m_ommWaiting.size(); ++done) {
        if (m_ommTableWrites.size() >= m_limits.ommTableWritesPerFrame || m_ommRebuilds.size() >= m_limits.ommRebuildsPerFrame) break;
        OmmLoad& load = m_ommWaiting[done];
        const std::array<uint32_t, 4> none = { load.group + 1u, load.identity, CLOD_RT_OMM_NONE, CLOD_RT_OMM_NONE };
        if (!load.none && !EnsureOmmPools()) load.none = true;
        if (load.none) {
            if (!InsertOmmEntry(load.group, none)) m_ommKnown.erase(load.group);
            ++m_stats.ommGroupsWithout;
            ++m_stats.ommWithoutReasons[load.noneReason];
            continue;
        }
        const uint64_t indexBytes = load.indices.size();
        const uint64_t descsBytes = load.descs.size();
        const uint64_t uploadBytes = AlignUp(indexBytes, kOmmUploadAlignment) + AlignUp(descsBytes, kOmmUploadAlignment) + AlignUp(load.data.size(), kOmmUploadAlignment);
        const bool hasArray = load.descCount != 0u;
        uint64_t arrayBytes = 0u;
        uint64_t scratchBytes = 0u;
        if (uploadBytes > m_limits.ommUploadBytesPerFrame || (hasArray && (!OmmArraySize(load.levelCounts, arrayBytes, scratchBytes) || scratchBytes > m_ommScratch->GetSize()))) {
            if (!InsertOmmEntry(load.group, none)) m_ommKnown.erase(load.group); // Never fits: any-hit instead of waiting forever.
            ++m_stats.ommGroupsWithout;
            ++m_stats.ommWithoutReasons[OmmLoad::TooLarge];
            continue;
        }
        if (m_ommUploadBytes + uploadBytes > m_limits.ommUploadBytesPerFrame || m_ommScratchUsed + scratchBytes > m_ommScratch->GetSize()) break; // Next frame.
        uint32_t firstUnit = 0u;
        uint32_t arrayFirst = 0u;
        const uint32_t arrayUnits = static_cast<uint32_t>(arrayBytes / CLOD_RT_OMM_ARRAY_ALIGNMENT);
        const bool indexRoom = m_ommIndexAllocator.Allocate(load.clusters, firstUnit);
        const bool arrayRoom = indexRoom && (!hasArray || m_ommArrayAllocator.Allocate(arrayUnits, arrayFirst));
        if (!arrayRoom) {
            if (indexRoom) m_ommIndexAllocator.Free(firstUnit, load.clusters);
            // Full: the GPU retires idle groups' micromaps while this waits, and it is loaded again once some are freed.
            if (m_ommRetries.size() < m_limits.ommRetryCapacity) {
                m_ommRetries.push_back({ load.group, load.identity, load.clusters, load.localIndex, load.containerPath });
            }
            else {
                m_ommKnown.erase(load.group); // Asked for again when it is next built.
            }
            continue;
        }
        uint8_t* upload = m_ommUpload.mapped + sliceBase;
        const uint64_t indicesOffset = m_ommUploadBytes;
        const uint64_t descsOffset = indicesOffset + AlignUp(indexBytes, kOmmUploadAlignment);
        const uint64_t dataOffset = descsOffset + AlignUp(descsBytes, kOmmUploadAlignment);
        if (!InsertOmmEntry(load.group, { load.group + 1u, load.identity, firstUnit, hasArray ? arrayFirst : CLOD_RT_OMM_NONE })) {
            m_ommIndexAllocator.Free(firstUnit, load.clusters);
            if (hasArray) m_ommArrayAllocator.Free(arrayFirst, arrayUnits);
            m_ommKnown.erase(load.group);
            continue;
        }
        m_ommSlotAllocations[m_ommInsertedSlot] = { firstUnit, load.clusters, arrayFirst, hasArray ? arrayUnits : 0u, load.path };
        {
            std::scoped_lock lock(m_ommMutex);
            if (const auto it = m_ommFiles.find(load.path); it != m_ommFiles.end()) ++it->second.entries;
            else m_ommSlotAllocations[m_ommInsertedSlot].path.clear();
        }
        std::memcpy(upload + indicesOffset, load.indices.data(), indexBytes);
        if (hasArray) {
            std::memcpy(upload + descsOffset, load.descs.data(), descsBytes);
            std::memcpy(upload + dataOffset, load.data.data(), load.data.size());
            OmmArrayBuild build{};
            build.destination = m_ommArrayPool.address + uint64_t(arrayFirst) * CLOD_RT_OMM_ARRAY_ALIGNMENT;
            build.scratchOffset = m_ommScratchUsed;
            build.descsOffset = sliceBase + descsOffset;
            build.dataOffset = sliceBase + dataOffset;
            build.count = load.descCount;
            build.levelCounts = load.levelCounts;
            m_ommBuilds.push_back(build);
            m_ommScratchUsed += scratchBytes;
        }
        m_ommCopies.push_back({ sliceBase + indicesOffset, firstUnit, load.clusters });
        m_ommUploadBytes += uploadBytes;
        m_ommRebuilds.push_back(load.group);
        ++m_stats.ommGroupsLoaded;
        m_stats.ommMicromapsTotal += load.descCount;
        m_stats.ommUploadBytesTotal += uploadBytes;
    }
    m_ommWaiting.erase(m_ommWaiting.begin(), m_ommWaiting.begin() + static_cast<ptrdiff_t>(done));
    m_stats.ommArrayBytesUsed = uint64_t(m_ommArrayAllocator.Used()) * CLOD_RT_OMM_ARRAY_ALIGNMENT;
    m_stats.ommIndexUnitsUsed = m_ommIndexAllocator.Used();
    m_stats.ommRetries = static_cast<uint32_t>(m_ommRetries.size());
    // Pressure: the GPU retires idle groups' micromaps until loads find room again.
    m_ommPressure = !m_ommRetries.empty()
        || m_stats.ommArrayBytesUsed * 10u > m_limits.ommArrayPoolBytes * 9u
        || uint64_t(m_stats.ommIndexUnitsUsed) * 10u > uint64_t(m_limits.ommIndexUnits) * 9u;
    m_stats.ommPressure = m_ommPressure;
    m_ommStaged = true;
}

void CLodRayTracingSystem::RecordOmmUploads(rhi::CommandList& commands) {
    if (!m_ommEnabled || !m_ommStaged) return;
    const uint64_t sliceBase = SliceOffset(m_ommUploadSliceBytes, m_frame % m_framesInFlight);
    uint8_t* upload = m_ommUpload.mapped + sliceBase;
    const uint64_t writesOffset = m_limits.ommUploadBytesPerFrame;
    const uint64_t rebuildOffset = writesOffset + uint64_t(m_limits.ommTableWritesPerFrame) * 16u;

    // The table and the rebuild list: copies, so the GPU sees an entry only once this frame has built what it names.
    for (size_t i = 0; i < m_ommTableWrites.size(); ++i) {
        std::memcpy(upload + writesOffset + i * 16u, m_ommTableCpu[m_ommTableWrites[i]].data(), 16u);
    }
    const uint32_t rebuildCount = static_cast<uint32_t>(m_ommRebuilds.size());
    std::memcpy(upload + rebuildOffset, &rebuildCount, 4u);
    if (rebuildCount != 0u) std::memcpy(upload + rebuildOffset + 4u, m_ommRebuilds.data(), size_t(rebuildCount) * 4u);

    // Declared by the CLAS pass as unordered access (the table, the rebuild list) and a build input (the index pool).
    Transition(commands, { m_ommTable.Handle(), m_ommRebuild.Handle() }, kUav, kCopyDest);
    if (m_ommPoolsReady) Transition(commands, { m_ommIndexPool.Handle() }, kBuildInput, kCopyDest);
    for (size_t i = 0; i < m_ommTableWrites.size(); ++i) {
        commands.CopyBufferRegion(m_ommTable.Handle(), uint64_t(m_ommTableWrites[i]) * 16u, m_ommUpload.Handle(), sliceBase + writesOffset + i * 16u, 16u);
    }
    commands.CopyBufferRegion(m_ommRebuild.Handle(), 0u, m_ommUpload.Handle(), sliceBase + rebuildOffset, 4u + uint64_t(rebuildCount) * 4u);
    for (const OmmIndexCopy& copy : m_ommCopies) {
        commands.CopyBufferRegion(m_ommIndexPool.Handle(), uint64_t(copy.firstUnit) * CLOD_RT_OMM_INDEX_UNIT_BYTES, m_ommUpload.Handle(),
            copy.uploadOffset, uint64_t(copy.units) * CLOD_RT_OMM_INDEX_UNIT_BYTES);
    }
    Transition(commands, { m_ommTable.Handle(), m_ommRebuild.Handle() }, kCopyDest, kUav);
    if (m_ommPoolsReady) Transition(commands, { m_ommIndexPool.Handle() }, kCopyDest, kBuildInput);

    if (!m_ommBuilds.empty()) {
        // Straight from the upload heap (a copy source to the graph, which leaves upload heaps as they are).
        std::vector<std::vector<rhi::RayTracingOpacityMicromapHistogramEntry>> histograms(m_ommBuilds.size());
        std::vector<rhi::RayTracingOpacityMicromapArrayBuildDesc> builds(m_ommBuilds.size());
        for (size_t i = 0; i < m_ommBuilds.size(); ++i) {
            const OmmArrayBuild& array = m_ommBuilds[i];
            for (uint32_t level = 0u; level < array.levelCounts.size(); ++level) {
                if (array.levelCounts[level] != 0u) histograms[i].push_back({ array.levelCounts[level], level, rhi::RayTracingOpacityMicromapFormat::OC1_4State });
            }
            rhi::RayTracingOpacityMicromapArrayBuildDesc& build = builds[i];
            build.inputs.flags = rhi::RTASBuild_PreferFastTrace;
            build.inputs.histogram = { histograms[i].data(), static_cast<uint32_t>(histograms[i].size()) };
            build.inputBuffer = { m_ommUpload.Handle(), array.dataOffset };
            build.perOmmDescs = { m_ommUpload.Handle(), array.descsOffset, uint64_t(array.count) * sizeof(CLodCache::OpacityMicromapDesc), sizeof(CLodCache::OpacityMicromapDesc) };
            build.destination = { m_ommArrayPool.Handle(), array.destination - m_ommArrayPool.address };
            build.scratch = { m_ommScratch->GetAPIResource().GetHandle(), array.scratchOffset };
        }
        commands.BuildOpacityMicromapArrays(builds.data(), static_cast<uint32_t>(builds.size()));
        Transition(commands, { m_ommArrayPool.Handle() }, kAsWritten, kAsRead); // This pass's CLAS builds read them.
    }
    m_ommBuilds.clear();
    m_ommCopies.clear();
    m_ommTableWrites.clear();
    m_ommRebuilds.clear();
    m_ommUploadBytes = 0u;
    m_ommScratchUsed = 0u;
    m_ommStaged = false;
}

void CLodRayTracingSystem::LogStats() {
    BT_ZONE_SCOPE("CLodRT::LogStats");
    const Stats& s = m_stats;
    spdlog::info(
        "CLod RT: instances={} untraversed={} BLASes={} clusters={} list={} (dropped {} instances, {} clusters, {} BLASes) CLAS {} of {} MiB ({} groups, {} clusters, {:.0f}% of page space unused), free pages={} chunks={} entries={}, built={} freed={} | "
        "last frame: requests={} admitted={} built={} clusters, cached={} transient={} ({} clusters) refused={} evicted={} (stale {}), no CLAS: not built={} discarded={} ({} clusters) missing clusters={} | "
        "evicted {} transient clusters {} not built {} discarded {} missing clusters {} in all, keep age={} | "
        "GPU ms: count {:.2f} cache+CLAS {:.2f} allocate {:.2f} place {:.2f} BLAS {:.2f} instances {:.2f} TLAS {:.2f}",
        s.instances, s.coarseConsumers, s.distinctBlases, s.clustersInBvh, s.listClusters, s.droppedInstances, s.droppedClusters, s.droppedBlases,
        s.clasBytesUsed >> 20u, s.clasPoolBytes >> 20u, s.cachedGroups, s.cachedClusters, s.packingWaste * 100.0f, s.freePages, s.freeChunks, s.freeEntries,
        s.clasBuildsTotal, s.clasFreedTotal,
        s.requestsLastFrame, s.admittedLastFrame, s.clasBuildsLastFrame, s.persistentLastFrame, s.transientLastFrame, s.transientClustersLastFrame, s.refusedLastFrame,
        s.evictedLastFrame, s.staleLastFrame, s.notBuiltLastFrame, s.discardedLastFrame, s.discardedClustersLastFrame, s.missingClustersLastFrame,
        s.evictedTotal, s.transientClustersTotal, s.notBuiltTotal, s.discardedTotal, s.missingClustersTotal, s.keepAge,
        s.msCount, s.msClasBuild, s.msAllocate, s.msPlace, s.msBlasBuild, s.msWriteInstances, s.msTlasBuild);
    if (m_ommEnabled) {
        spdlog::info("CLod RT micromaps: groups loaded={} without={} (no container {}, no file {}, not in file {}, cluster mismatch {}, too large {}) pending={} waiting for room={} released={} pressure={} rebuilt last frame={} micromaps={} uploaded {} MiB, arrays {} of {} MiB, index clusters {} of {}, files {} | instances alpha={} with={} disabled={}",
            s.ommGroupsLoaded, s.ommGroupsWithout, s.ommWithoutReasons[0], s.ommWithoutReasons[1], s.ommWithoutReasons[2], s.ommWithoutReasons[3],
            s.ommWithoutReasons[4], s.ommPending, s.ommRetries, s.ommReleasedTotal, s.ommPressure, s.rebuiltLastFrame, s.ommMicromapsTotal, s.ommUploadBytesTotal >> 20u,
            s.ommArrayBytesUsed >> 20u, m_limits.ommArrayPoolBytes >> 20u, s.ommIndexUnitsUsed, m_limits.ommIndexUnits, s.ommFilesOpen,
            s.alphaInstances, s.ommInstances, s.ommDisabledInstances);
    }
    if (s.comparisonValid) {
        const auto& c = s.comparison;
        spdlog::info(
            "CLod RT compare: match={} differentTriangle={} differentCluster={} differentInstance={} rayMissed={} rasterMissed={} bothMissed={} depthClose={} farther={} nearer={} notCovered={} differentInstanceAlpha={} differentInstanceDoubleSided={}",
            c[CLOD_RT_STAT_BOTH_HIT_MATCH], c[CLOD_RT_STAT_DIFFERENT_TRIANGLE], c[CLOD_RT_STAT_DIFFERENT_CLUSTER], c[CLOD_RT_STAT_DIFFERENT_INSTANCE],
            c[CLOD_RT_STAT_RAY_MISSED_RASTER_HIT], c[CLOD_RT_STAT_RAY_HIT_RASTER_MISSED], c[CLOD_RT_STAT_BOTH_MISSED], c[CLOD_RT_STAT_DEPTH_WITHIN_TOLERANCE],
            c[CLOD_RT_STAT_RAY_HIT_FARTHER], c[CLOD_RT_STAT_RAY_HIT_NEARER], c[CLOD_RT_STAT_RASTER_NOT_COVERED],
            c[CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_ALPHA], c[CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_DOUBLE_SIDED]);
    }
}

void CLodRayTracingSystem::RecordCache(org::RecordingContext& recording, const ScenePipelines& pipelines) {
    auto& commands = recording.Commands();
    const uint32_t budget = m_limits.clusterBuildBudget;
    MiscConstants cache = CacheConstants();
    const auto dispatch = [&](const org::PipelineState& pipeline, uint32_t threads, uint32_t groupWidth) {
        recording.BindComputePipeline(pipeline);
        PushMiscConstants(recording, cache);
        commands.Dispatch((threads + groupWidth - 1u) / groupWidth, 1u, 1u);
    };

    // Count's demand and marks, last frame's placing.
    GlobalUavBarrier(commands);
    dispatch(pipelines.cacheCollect, CLOD_RT_CACHE_GROUP_THREADS, CLOD_RT_CACHE_GROUP_THREADS);
    GlobalUavBarrier(commands);
    if (m_ommEnabled && cache[CLOD_RT_CACHE_OMM_RETIRE_WINDOW] != 0u) {
        dispatch(pipelines.cacheOmmRetire, CLOD_RT_OMM_RETIRE_WINDOW_MAX, 64u);
        GlobalUavBarrier(commands);
    }
    dispatch(pipelines.cacheEvict, m_limits.maxEntries, 64u);
    GlobalUavBarrier(commands);
    dispatch(pipelines.cacheAdmit, CLOD_RT_CACHE_GROUP_THREADS, CLOD_RT_CACHE_GROUP_THREADS);
    GlobalUavBarrier(commands);
    dispatch(pipelines.cacheAssign, CLOD_RT_CACHE_GROUP_THREADS, CLOD_RT_CACHE_GROUP_THREADS);
    GlobalUavBarrier(commands);
    dispatch(pipelines.cacheArgs, budget, 64u);
    // The build reads its arguments and their count. The arena, scratch and destination arrays are this pass's alone.
    Transition(commands, { m_buildArgs.Handle(), m_cacheCtrl.Handle() }, kUav, kBuildInput);

    rhi::RayTracingClusterTriangleInputsDesc triangles{};
    triangles.vertexFormat = rhi::RayTracingClusterVertexFormat::Float32x3;
    triangles.indexFormat = rhi::RayTracingClusterIndexFormat::Uint8;
    triangles.maxClusterTriangleCount = kMaxClusterTriangles;
    triangles.maxClusterVertexCount = kMaxClusterVertices;
    triangles.maxTotalTriangleCount = kMaxClusterTriangles * budget;
    triangles.maxTotalVertexCount = kMaxClusterVertices * budget;
    if (m_ommEnabled) {
        triangles.opacityMicromapIndexFormat = rhi::RayTracingClusterIndexFormat::Uint32;
        triangles.maxOpacityMicromapIndicesPerCluster = kMaxClusterTriangles;
    }
    rhi::RayTracingRtasOperationDesc build{};
    build.inputs.allowOpacityMicromaps = m_ommEnabled;
    build.inputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
    build.inputs.operationMode = rhi::RayTracingRtasOperationMode::ImplicitDestinations;
    build.inputs.flags = rhi::RTASBuild_PreferFastTrace;
    build.inputs.maxAccelerationStructureCount = budget;
    build.inputs.triangleClusters = &triangles;
    build.batched.dstImplicitData = { m_buildArena->GetAPIResource().GetHandle(), 0u };
    build.batched.scratchData = { m_buildScratch->GetAPIResource().GetHandle(), 0u };
    build.batched.dstAddressesArray = { m_arenaAddresses.Handle(), 0u, m_arenaAddresses.bytes, 8u };
    build.batched.dstSizesArray = { m_arenaSizes.Handle(), 0u, m_arenaSizes.bytes, 8u };
    build.batched.srcInfosArray = { m_buildArgs.Handle(), 0u, m_buildArgs.bytes, m_clusterInfoStride };
    build.batched.srcInfosCount = { m_cacheCtrl.Handle(), CLOD_RT_CTRL_BUILD_ARG_COUNT * 4u };
    commands.ExecuteIndirectRtasOperations(&build, 1u);
    // Pack reads where the build put each CLAS and how large it is, and writes the control words again.
    Transition(commands, { m_arenaAddresses.Handle(), m_arenaSizes.Handle() }, kBuildOutput, kBuildInput);
    Transition(commands, { m_cacheCtrl.Handle() }, kBuildInput, kUav);

    dispatch(pipelines.cachePack, CLOD_RT_CACHE_GROUP_THREADS, CLOD_RT_CACHE_GROUP_THREADS);
    // The move reads pack's arguments, destinations and count, the CLAS just built, and the scratch the build used.
    Transition(commands, { m_moveArgs.Handle(), m_moveDestinations.Handle(), m_cacheCtrl.Handle() }, kUav, kBuildInput);
    Transition(commands, { m_buildArena->GetAPIResource().GetHandle() }, kAsWritten, kAsRead);
    Transition(commands, { m_buildScratch->GetAPIResource().GetHandle() }, kBuildOutput, kBuildOutput);

    rhi::RayTracingClusterMovesDesc moves{};
    moves.type = rhi::RayTracingClusterAccelerationStructureType::TriangleCluster;
    moves.noMoveOverlap = true;
    moves.maxMovedBytes = m_arenaBytes;
    rhi::RayTracingRtasOperationDesc move{};
    move.inputs.operationType = rhi::RayTracingRtasOperationType::MoveObjects;
    move.inputs.operationMode = rhi::RayTracingRtasOperationMode::ExplicitDestinations;
    move.inputs.maxAccelerationStructureCount = budget;
    move.inputs.moveObjects = &moves;
    move.inputs.allowOpacityMicromaps = m_ommEnabled;
    move.batched.scratchData = { m_buildScratch->GetAPIResource().GetHandle(), 0u };
    move.batched.dstAddressesArray = { m_moveDestinations.Handle(), 0u, m_moveDestinations.bytes, 8u };
    move.batched.srcInfosArray = { m_moveArgs.Handle(), 0u, m_moveArgs.bytes, sizeof(rhi::PackedRayTracingClusterMoveObjectsInfo) };
    move.batched.srcInfosCount = { m_cacheCtrl.Handle(), CLOD_RT_CTRL_MOVE_COUNT * 4u };
    commands.ExecuteIndirectRtasOperations(&move, 1u);
    // Left as build inputs (DeclareSceneClas): the arguments, destinations, arena addresses and sizes, control words.
}

MiscConstants CLodRayTracingSystem::CacheConstants() const {
    MiscConstants cache{};
    cache[CLOD_RT_CACHE_STATE_UAV] = m_groupState.uav;
    cache[CLOD_RT_CACHE_CTRL_UAV] = m_cacheCtrl.uav;
    cache[CLOD_RT_CACHE_ENTRIES_UAV] = m_cacheEntries.uav;
    cache[CLOD_RT_CACHE_PAGE_NEXT_UAV] = m_pageNext.uav;
    cache[CLOD_RT_CACHE_QUEUES_UAV] = m_cacheQueues.uav;
    cache[CLOD_RT_CACHE_REQUESTS_UAV] = m_requests.uav;
    cache[CLOD_RT_CACHE_DEMAND_UAV] = m_demand.uav;
    cache[CLOD_RT_CACHE_REQUEST_CAPACITY] = m_limits.requestCapacity;
    cache[CLOD_RT_CACHE_RANGES_UAV] = m_cacheRanges.uav;
    cache[CLOD_RT_CACHE_PENDING_UAV] = m_cachePending.uav;
    cache[CLOD_RT_CACHE_ARGS_UAV] = m_buildArgs.uav;
    cache[CLOD_RT_CACHE_CLAS_INFO_UAV] = m_clasInfo.uav;
    cache[CLOD_RT_CACHE_CLAS_ADDRESSES_UAV] = m_clasAddresses.uav;
    cache[CLOD_RT_CACHE_SLOT_OMM_KEYS_UAV] = m_slotOmmKeys.uav;
    cache[CLOD_RT_CACHE_ARENA_ADDRESSES_SRV] = m_arenaAddresses.srv;
    cache[CLOD_RT_CACHE_ARENA_SIZES_SRV] = m_arenaSizes.srv;
    cache[CLOD_RT_CACHE_TRANSIENT_UAV] = m_transient.uav;
    cache[CLOD_RT_CACHE_PACK_SCRATCH_UAV] = m_packScratch.uav;
    cache[CLOD_RT_CACHE_PAGE_REFS_UAV] = m_pageRefs.uav;
    cache[CLOD_RT_CACHE_MOVE_ARGS_UAV] = m_moveArgs.uav;
    cache[CLOD_RT_CACHE_MOVE_DESTINATIONS_UAV] = m_moveDestinations.uav;
    cache[CLOD_RT_CACHE_SLAB_ADDRESSES_SRV] = m_slabAddresses.srv;
    cache[CLOD_RT_CACHE_SLAB_ADDRESS_COUNT] = m_limits.slabAddressEntries;
    cache[CLOD_RT_CACHE_POOL_LOW] = Low(m_clasPool.address);
    cache[CLOD_RT_CACHE_POOL_HIGH] = High(m_clasPool.address);
    cache[CLOD_RT_CACHE_BUDGET] = m_limits.clusterBuildBudget;
    cache[CLOD_RT_CACHE_TRANSIENT_ARENA_UNIT] = static_cast<uint32_t>((uint64_t(m_pageCount) * kPageBytes + uint64_t(ArenaSlot()) * m_transientSliceBytes) / CLOD_RT_CACHE_UNIT_BYTES);
    cache[CLOD_RT_CACHE_TRANSIENT_ARENA_UNITS] = static_cast<uint32_t>(m_transientSliceBytes / CLOD_RT_CACHE_UNIT_BYTES);
    cache[CLOD_RT_CACHE_PROTECTED_FRAMES] = m_protectedFrames;
    cache[CLOD_RT_CACHE_PAGE_COUNT] = m_pageCount;
    cache[CLOD_RT_CACHE_CHUNK_COUNT] = m_chunkCount;
    cache[CLOD_RT_CACHE_ENTRY_COUNT] = m_limits.maxEntries;
    cache[CLOD_RT_CACHE_OMM_ACTIVE] = OpacityMicromapsActive() ? 1u : 0u;
    cache[CLOD_RT_CACHE_COUNTERS_UAV] = m_counters.uav;
    cache[CLOD_RT_CACHE_OMM_TABLE_UAV] = m_ommTable.uav;
    cache[CLOD_RT_CACHE_OMM_WANTED_UAV] = m_ommWanted.uav;
    cache[CLOD_RT_CACHE_OMM_REBUILD_SRV] = m_ommRebuild.srv;
    cache[CLOD_RT_CACHE_OMM_REBUILD_CAPACITY] = m_limits.ommRebuildsPerFrame;
    cache[CLOD_RT_CACHE_OMM_INDEX_POOL_LOW] = Low(m_ommIndexPool.address);
    cache[CLOD_RT_CACHE_OMM_INDEX_POOL_HIGH] = High(m_ommIndexPool.address);
    cache[CLOD_RT_CACHE_OMM_ARRAY_POOL_LOW] = Low(m_ommArrayPool.address);
    cache[CLOD_RT_CACHE_OMM_ARRAY_POOL_HIGH] = High(m_ommArrayPool.address);
    cache[CLOD_RT_CACHE_OMM_REBUILD_UAV] = m_ommRebuild.uav;
    cache[CLOD_RT_CACHE_OMM_RELEASE_UAV] = m_ommRelease.uav;
    cache[CLOD_RT_CACHE_OMM_RETIRE_WINDOW] = m_ommEnabled && m_ommPressure ? CLOD_RT_OMM_RETIRE_WINDOW_MAX : 0u;
    cache[CLOD_RT_CACHE_TRANSIENT_ID_BASE] = m_chunkCount * CLOD_RT_CACHE_CHUNK_IDS + ArenaSlot() * m_limits.clusterBuildBudget;
    cache[CLOD_RT_CACHE_MAX_KEEP_AGE] = m_limits.maxKeepAge;
    // Two frames of 4,096 cluster builds at 3 KiB a cluster: a usual frame's need, not a burst's.
    cache[CLOD_RT_CACHE_WATERMARK_PAGES] = std::min<uint32_t>(m_pageCount / 4u, 4096u * 2u * 24u / CLOD_RT_CACHE_PAGE_UNITS);
    return cache;
}

MiscConstants CLodRayTracingSystem::SceneConstants(const SceneInputs& inputs) const {
    MiscConstants scene{};
    scene[CLOD_RT_SCENE_CLUSTERS_SRV] = inputs.clustersSrv;
    scene[CLOD_RT_SCENE_CLUSTER_COUNTER_SRV] = inputs.counterSrv;
    scene[CLOD_RT_SCENE_CLUSTER_CAPACITY] = std::min(inputs.clusterCapacity, m_limits.maxClustersInBvh);
    scene[CLOD_RT_SCENE_REFERENCE_CAPACITY] = m_limits.maxClusterReferences;
    scene[CLOD_RT_SCENE_BLAS_CAPACITY] = m_limits.maxBlases;
    scene[CLOD_RT_SCENE_DRAW_TABLE_UAV] = m_drawTable.uav;
    scene[CLOD_RT_SCENE_DRAW_TABLE_ENTRIES] = m_limits.maxDrawRecords + m_limits.maxPortalOccurrences;
    scene[CLOD_RT_SCENE_DRAW_KEYS] = m_limits.maxDrawRecords;
    scene[CLOD_RT_SCENE_CLUSTER_SIDE_BAND_SRV] = inputs.sideBandSrv;
    scene[CLOD_RT_SCENE_INSTANCE_ROWS_UAV] = m_instanceRows.uav;
    scene[CLOD_RT_SCENE_INSTANCE_LIST_UAV] = m_instanceList.uav;
    scene[CLOD_RT_SCENE_COUNTERS_UAV] = m_counters.uav;
    scene[CLOD_RT_SCENE_INSTANCE_CAPACITY] = m_limits.maxInstances;
    scene[CLOD_RT_SCENE_BLAS_ARGS_UAV] = m_blasArgs.uav;
    scene[CLOD_RT_SCENE_ADDRESS_ARRAY_UAV] = m_blasAddressArray.uav;
    scene[CLOD_RT_SCENE_ADDRESS_ARRAY_LOW] = Low(m_blasAddressArray.address);
    scene[CLOD_RT_SCENE_ADDRESS_ARRAY_HIGH] = High(m_blasAddressArray.address);
    scene[CLOD_RT_SCENE_MAX_CLUSTERS_PER_INSTANCE] = m_limits.maxClustersPerInstance;
    scene[CLOD_RT_SCENE_GROUP_STATE_SRV] = m_groupState.srv;
    scene[CLOD_RT_SCENE_CACHE_ENTRIES_SRV] = m_cacheEntries.srv;
    scene[CLOD_RT_SCENE_CACHE_TRANSIENT_SRV] = m_transient.srv;
    scene[CLOD_RT_SCENE_GROUP_STATE_UAV] = m_groupState.uav;
    scene[CLOD_RT_SCENE_DEMAND_UAV] = m_demand.uav;
    scene[CLOD_RT_SCENE_CACHE_CTRL_UAV] = m_cacheCtrl.uav;
    scene[CLOD_RT_SCENE_CLAS_ADDRESSES_UAV] = m_clasAddresses.uav;
    scene[CLOD_RT_SCENE_COARSE_CUT_UAV] = m_coarseCut.uav;
    scene[CLOD_RT_SCENE_BLAS_ADDRESSES_SRV] = m_blasAddresses.srv;
    scene[CLOD_RT_SCENE_TLAS_INSTANCES_UAV] = inputs.tlasInstancesUav;
    scene[CLOD_RT_SCENE_TLAS_INSTANCE_COUNT] = m_tlasInstanceCount;
    scene[CLOD_RT_SCENE_INSTANCE_BLAS_UAV] = m_instanceBlas.uav;
    scene[CLOD_RT_SCENE_SHARE_TABLE_UAV] = m_shareTable.uav;
    scene[CLOD_RT_SCENE_SHARE_TABLE_ENTRIES] = m_limits.blasShareEntries;
    scene[CLOD_RT_SCENE_SLOT_OMM_KEYS_UAV] = m_slotOmmKeys.uav;
    scene[CLOD_RT_SCENE_BLAS_OMM_KEYS_UAV] = m_blasOmmKeys.uav;
    scene[CLOD_RT_SCENE_OMM_ACTIVE] = OpacityMicromapsActive() ? 1u : 0u;
    return scene;
}

void CLodRayTracingSystem::RecordSceneClear(org::RecordingContext& recording, const ScenePipelines& pipelines, const GraphBuffer& buffer, uint64_t bytes) {
    const uint32_t words = static_cast<uint32_t>(bytes / 4u);
    MiscConstants constants{};
    constants[CLOD_RT_CLEAR_UAV] = buffer.uav;
    constants[CLOD_RT_CLEAR_WORD_COUNT] = words;
    recording.BindComputePipeline(pipelines.clear);
    PushMiscConstants(recording, constants);
    recording.Commands().Dispatch((words + kUavClearDispatchWidth - 1u) / kUavClearDispatchWidth, 1u, 1u);
}

void CLodRayTracingSystem::SceneStamp(rhi::CommandList& commands, uint32_t index) {
    if (m_timers) commands.WriteTimestamp(m_timers->GetHandle(), (m_frame % m_framesInFlight) * kSceneTimers + index, rhi::Stage::Bottom);
}

// The stages are separate passes. The graph handles the buffers it owns between them (scratch, arena, BLAS data, TLAS
// rows); the barriers here are for this class's own buffers, which it doesn't see, and for the TLAS rows between two uses
// inside the TLAS pass.
void CLodRayTracingSystem::RecordSceneClas(org::RecordingContext& recording, const ScenePipelines& pipelines, const SceneInputs& inputs) {
    auto& commands = recording.Commands();
    SceneStamp(commands, 0u);
    const auto clear = [&](const GraphBuffer& buffer, uint64_t bytes) { RecordSceneClear(recording, pipelines, buffer, bytes); };
    const MiscConstants scene = SceneConstants(inputs);

    if (!m_tablesCleared) {
        // The first frame: everything starts empty, and the cache's queues full.
        clear(m_groupState, m_groupState.bytes);
        clear(m_cacheEntries, m_cacheEntries.bytes);
        clear(m_pageRefs, m_pageRefs.bytes);
        clear(m_transient, m_transient.bytes);
        clear(m_clasAddresses, m_clasAddresses.bytes);
        clear(m_drawTable, m_drawTable.bytes);
        clear(m_slotOmmKeys, m_slotOmmKeys.bytes);
        clear(m_counters, kCountersBytes);
        clear(m_cacheCtrl, kCtrlBytes);
        clear(m_demand, m_demand.bytes);
        clear(m_ommTable, m_ommTable.bytes);
        clear(m_ommRebuild, m_ommRebuild.bytes);
        GlobalUavBarrier(commands);
        const MiscConstants init = CacheConstants();
        recording.BindComputePipeline(pipelines.cacheInit);
        PushMiscConstants(recording, init);
        commands.Dispatch((std::max({ m_pageCount, m_chunkCount, m_limits.maxEntries }) + 255u) / 256u, 1u, 1u);
        GlobalUavBarrier(commands);
        m_tablesCleared = true;
    }
    else {
        // Clean up after last frame while the counters still say how much it touched.
        recording.BindComputePipeline(pipelines.resetDraws);
        PushMiscConstants(recording, scene);
        commands.Dispatch((m_limits.maxInstances + 63u) / 64u, 1u, 1u);
        GlobalUavBarrier(commands);
        // A dropped instance is touched but not listed, so the reset above can't find it.
        if (m_stats.droppedInstances != 0u) {
            clear(m_drawTable, m_drawTable.bytes);
            GlobalUavBarrier(commands);
        }
        RecordOmmUploads(commands);
    }

    // ---- Group selected clusters by instance
    clear(m_counters, uint64_t(CLOD_RT_COUNTER_FRAME_WORDS) * 4u); // Not the cumulative words after them.
    clear(m_shareTable, m_shareTable.bytes);
    clear(m_blasOmmKeys, m_blasOmmKeys.bytes);
    GlobalUavBarrier(commands);

    // Over the whole list's capacity: sizing these from a list read back frames ago dropped every cluster past it for
    // the frames a sudden load took to show up, and threads past this frame's count only load it and leave. Count also
    // records what the list needs from the CLAS cache, which then builds it.
    const uint32_t clusterGroups = (scene[CLOD_RT_SCENE_CLUSTER_CAPACITY] + 255u) / 256u;
    recording.BindComputePipeline(pipelines.count);
    PushMiscConstants(recording, scene);
    commands.Dispatch(clusterGroups, 1u, 1u);
    SceneStamp(commands, 1u);
    RecordCache(recording, pipelines); // Starts by waiting for count.
    SceneStamp(commands, 2u);
}

void CLodRayTracingSystem::RecordSceneBlas(org::RecordingContext& recording, const ScenePipelines& pipelines, const SceneInputs& inputs) {
    auto& commands = recording.Commands();
    const MiscConstants scene = SceneConstants(inputs);
    const uint32_t clusterGroups = (scene[CLOD_RT_SCENE_CLUSTER_CAPACITY] + 255u) / 256u;

    recording.BindComputePipeline(pipelines.allocate);
    PushMiscConstants(recording, scene);
    commands.Dispatch((m_limits.maxInstances + 63u) / 64u, 1u, 1u);
    GlobalUavBarrier(commands);

    recording.BindComputePipeline(pipelines.addCoarseConsumers);
    PushMiscConstants(recording, scene);
    commands.Dispatch((m_limits.maxInstances + 63u) / 64u, 1u, 1u);
    GlobalUavBarrier(commands);

    recording.BindComputePipeline(pipelines.allocateFinish);
    PushMiscConstants(recording, scene);
    commands.Dispatch(1u, 1u, 1u);
    GlobalUavBarrier(commands);
    SceneStamp(commands, 3u);

    recording.BindComputePipeline(pipelines.place);
    PushMiscConstants(recording, scene);
    commands.Dispatch(clusterGroups, 1u, 1u);
    SceneStamp(commands, 4u);

    // ---- BLASes: the build reads the arguments, the CLAS references they point into, and their count. The pass leaves them
    // that way (DeclareSceneBlas); the BLAS data, addresses and sizes it writes are the graph's to hand on.
    Transition(commands, { m_blasArgs.Handle(), m_blasAddressArray.Handle(), m_counters.Handle() }, kUav, kBuildInput);

    rhi::RayTracingClusterBottomLevelInputsDesc blasInputs{ m_limits.maxClusterReferences, m_limits.maxClustersPerInstance };
    rhi::RayTracingRtasOperationDesc blas{};
    blas.inputs.operationType = rhi::RayTracingRtasOperationType::BuildClustersBottomLevel;
    blas.inputs.flags = rhi::RTASBuild_PreferFastBuild;
    blas.inputs.maxAccelerationStructureCount = m_limits.maxBlases;
    blas.inputs.clustersBottomLevel = &blasInputs;
    blas.inputs.allowOpacityMicromaps = m_ommEnabled;
    constexpr uint64_t blasArgStride = sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo);
    blas.batched.dstImplicitData = { m_blasResult->GetAPIResource().GetHandle(), 0u };
    blas.batched.scratchData = { m_blasScratch->GetAPIResource().GetHandle(), 0u };
    blas.batched.dstAddressesArray = { m_blasAddresses.Handle(), 0u, m_blasAddresses.bytes, 8u };
    blas.batched.dstSizesArray = { m_blasSizes.Handle(), 0u, m_blasSizes.bytes, 8u };
    blas.batched.srcInfosArray = { m_blasArgs.Handle(), 0u, m_blasArgs.bytes, blasArgStride };
    blas.batched.srcInfosCount = { m_counters.Handle(), CLOD_RT_COUNTER_BLAS_ARGS * 4u };
    commands.ExecuteIndirectRtasOperations(&blas, 1u);
    SceneStamp(commands, 5u);
}

void CLodRayTracingSystem::RecordSceneTlas(org::RecordingContext& recording, const ScenePipelines& pipelines, const SceneInputs& inputs) {
    auto& commands = recording.Commands();
    const uint32_t sliceIndex = m_frame % m_framesInFlight;
    const MiscConstants scene = SceneConstants(inputs);

    // ---- TLAS: the rows are written, then the build's input, and stay that way (DeclareSceneTlas).
    recording.BindComputePipeline(pipelines.writeInstances);
    PushMiscConstants(recording, scene);
    commands.Dispatch((m_tlasInstanceCount + 63u) / 64u, 1u, 1u);
    Transition(commands, { m_tlasInstances->GetAPIResource().GetHandle() }, kUav, kBuildInput);
    SceneStamp(commands, 6u);

    rhi::RayTracingGeometryDesc geometry{};
    geometry.type = rhi::RayTracingGeometryType::Instances;
    geometry.instances.instanceBuffer = { m_tlasInstances->GetAPIResource().GetHandle(), 0u, uint64_t(m_tlasInstanceCount) * sizeof(rhi::PackedRayTracingInstanceDesc) };
    geometry.instances.stride = sizeof(rhi::PackedRayTracingInstanceDesc);
    geometry.instances.count = m_tlasInstanceCount;
    rhi::AccelerationStructureBuildDesc build{};
    build.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
    build.destination = m_tlas->GetHandle();
    build.inputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
    build.inputs.flags = rhi::RTASBuild_PreferFastBuild; // Measured: 0.2 ms cheaper, and no slower to trace.
    build.inputs.geometries = { &geometry, 1u };
    build.scratch = { m_tlasScratch->GetAPIResource().GetHandle(), 0u, m_tlasScratch->GetSize() };
    commands.BuildAccelerationStructures(&build, 1u);
    m_tlasBuilt = true;
    SceneStamp(commands, 7u);

    // ---- What the CPU reads back next time this slice comes around. Left as copy sources, but the control words, which
    // the end of frame writes.
    const uint64_t readbackBase = SliceOffset(m_readbackSliceBytes, sliceIndex);
    Transition(commands, { m_counters.Handle(), m_cacheCtrl.Handle(), m_ommWanted.Handle(), m_ommRelease.Handle() }, kUav, kCopySource);
    commands.CopyBufferRegion(m_readback.Handle(), readbackBase, m_counters.Handle(), 0u, kCountersBytes);
    commands.CopyBufferRegion(m_readback.Handle(), readbackBase + kReadbackCtrl, m_cacheCtrl.Handle(), 0u, kCtrlBytes);
    commands.CopyBufferRegion(m_readback.Handle(), readbackBase + kReadbackOmmWanted, m_ommWanted.Handle(), 0u, m_ommWanted.bytes);
    commands.CopyBufferRegion(m_readback.Handle(), readbackBase + kReadbackOmmWanted + m_ommWanted.bytes, m_ommRelease.Handle(), 0u, kReleaseBytes);
    if (m_timers) {
        commands.ResolveQueryData(m_timers->GetHandle(), sliceIndex * kSceneTimers, kSceneTimers, m_readback.Handle(), readbackBase + kReadbackTimers);
    }
    m_sliceHasReadback[sliceIndex] = 1u;
    m_sliceHasStats[sliceIndex] = 0u;
    Transition(commands, { m_cacheCtrl.Handle() }, kCopySource, kUav);

    RecordSceneClear(recording, pipelines, m_coarseCut, m_coarseCut.bytes); // Culling starts with it empty next frame.
    // The next frame: the tails, its frame number, an empty request list.
    const MiscConstants end = CacheConstants();
    recording.BindComputePipeline(pipelines.cacheEnd);
    PushMiscConstants(recording, end);
    commands.Dispatch(1u, 1u, 1u);
}

void CLodRayTracingSystem::RecordVisibilityBegin(org::RecordingContext& recording, const org::PipelineState& clearPipeline, bool clearStats) {
    auto& commands = recording.Commands();
    const auto clear = [&](const GraphBuffer& buffer) {
        const uint32_t words = static_cast<uint32_t>(buffer.bytes / 4u);
        MiscConstants constants{};
        constants[CLOD_RT_CLEAR_UAV] = buffer.uav;
        constants[CLOD_RT_CLEAR_WORD_COUNT] = words;
        recording.BindComputePipeline(clearPipeline);
        PushMiscConstants(recording, constants);
        commands.Dispatch((words + kUavClearDispatchWidth - 1u) / kUavClearDispatchWidth, 1u, 1u);
    };
    clear(m_visibilityHash);
    if (clearStats) clear(m_visibilityStats);
    GlobalUavBarrier(commands);
}

void CLodRayTracingSystem::RecordVisibilityEnd(org::RecordingContext& recording, bool readStats) {
    if (!readStats) return;
    auto& commands = recording.Commands();
    const uint32_t sliceIndex = m_frame % m_framesInFlight;
    // Back as declared: the pass skips this when there's no TLAS yet.
    Transition(commands, { m_visibilityStats.Handle() }, kUav, kCopySource);
    commands.CopyBufferRegion(m_statsReadback.Handle(), SliceOffset(kStatsSliceBytes, sliceIndex), m_visibilityStats.Handle(), 0u, kStatsBytes);
    Transition(commands, { m_visibilityStats.Handle() }, kCopySource, kUav);
    m_sliceHasStats[sliceIndex] = 1u;
}

uint32_t CLodRayTracingSystem::GetPixelWatchUav() const noexcept {
    return m_watchBright > 0.0f && m_pixelWatch.bytes != 0u ? m_pixelWatch.uav : CLOD_RT_PT_NO_DESCRIPTOR;
}

void CLodRayTracingSystem::RecordPixelWatchBegin(org::RecordingContext& recording, const org::PipelineState& clearPipeline) {
    if (GetPixelWatchUav() == CLOD_RT_PT_NO_DESCRIPTOR) return;
    auto& commands = recording.Commands();
    MiscConstants constants{};
    constants[CLOD_RT_CLEAR_UAV] = m_pixelWatch.uav;
    constants[CLOD_RT_CLEAR_WORD_COUNT] = CLOD_RT_WATCH_WORDS;
    recording.BindComputePipeline(clearPipeline);
    PushMiscConstants(recording, constants);
    commands.Dispatch((CLOD_RT_WATCH_WORDS + kUavClearDispatchWidth - 1u) / kUavClearDispatchWidth, 1u, 1u);
    GlobalUavBarrier(commands);
}

void CLodRayTracingSystem::RecordPixelWatchEnd(org::RecordingContext& recording) {
    if (GetPixelWatchUav() == CLOD_RT_PT_NO_DESCRIPTOR) return;
    auto& commands = recording.Commands();
    const uint32_t sliceIndex = m_frame % m_framesInFlight;
    // Back as declared: the pass skips this when there's no TLAS yet.
    Transition(commands, { m_pixelWatch.Handle() }, kUav, kCopySource);
    commands.CopyBufferRegion(m_watchReadback.Handle(), SliceOffset(kWatchSliceBytes, sliceIndex), m_pixelWatch.Handle(), 0u, CLOD_RT_WATCH_WORDS * 4u);
    Transition(commands, { m_pixelWatch.Handle() }, kCopySource, kUav);
    m_sliceHasWatch[sliceIndex] = 1u;
}

void CLodRayTracingSystem::ReadPixelWatch(const uint32_t* words) {
    static constexpr const char* kKindNames[CLOD_RT_WATCH_KINDS + 1u] = { "", "bright pixel", "bad material", "dark terrain pixel", "resolve failed",
        "dark terrain albedo", "primary path ended" };
    bool any = false;
    for (uint32_t kind = 1u; kind <= CLOD_RT_WATCH_KINDS; ++kind) {
        m_watchTotals[kind] += words[kind];
        any = any || words[kind] != 0u;
    }
    m_watchTotals[CLOD_RT_WATCH_NON_FINITE] += words[CLOD_RT_WATCH_NON_FINITE];
    any = any || words[CLOD_RT_WATCH_NON_FINITE] != 0u;
    if (!any) return;
    const auto asFloat = [](uint32_t word) { float value; std::memcpy(&value, &word, 4u); return value; };

    if (m_watchCsv) {
        for (uint32_t kind = 1u; kind <= CLOD_RT_WATCH_KINDS; ++kind) {
            const uint32_t records = std::min(words[kind], CLOD_RT_WATCH_RECORDS);
            for (uint32_t index = 0u; index < records; ++index) {
                const uint32_t* r = words + CLOD_RT_WATCH_HEADER_WORDS + ((kind - 1u) * CLOD_RT_WATCH_RECORDS + index) * CLOD_RT_WATCH_RECORD_WORDS;
                std::fprintf(m_watchCsv.get(), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", m_frame, r[0] & 0xFFu, (r[0] >> 8u) & 0xFFu,
                    r[0] >> 16u, r[1] & 0xFFFFu, r[1] >> 16u, r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - m_watchLastLog < std::chrono::seconds(1)) return;
    m_watchLastLog = now;
    spdlog::warn("CLod RT pixel watch, frame {}: bright pixels={} (max luminance {:.0f}) bad materials={} dark terrain pixels={} resolve failures={} "
                 "dark terrain albedo={} primary paths ended={} non-finite={} | totals {} {} {} {} {} {} {}",
        m_frame, words[CLOD_RT_WATCH_BRIGHT_PIXEL], asFloat(words[CLOD_RT_WATCH_MAX_LUMINANCE]), words[CLOD_RT_WATCH_BAD_MATERIAL],
        words[CLOD_RT_WATCH_DARK_TERRAIN_PIXEL], words[CLOD_RT_WATCH_RESOLVE_FAILED], words[CLOD_RT_WATCH_DARK_TERRAIN_ALBEDO],
        words[CLOD_RT_WATCH_PRIMARY_ENDED], words[CLOD_RT_WATCH_NON_FINITE], m_watchTotals[1], m_watchTotals[2], m_watchTotals[3],
        m_watchTotals[4], m_watchTotals[5], m_watchTotals[6], m_watchTotals[CLOD_RT_WATCH_NON_FINITE]);
    // One example of each kind, a few times each.
    for (uint32_t kind = 1u; kind <= CLOD_RT_WATCH_KINDS; ++kind) {
        if (words[kind] == 0u || m_watchLogged[kind] >= 20u) continue;
        ++m_watchLogged[kind];
        const uint32_t* r = words + CLOD_RT_WATCH_HEADER_WORDS + (kind - 1u) * CLOD_RT_WATCH_RECORDS * CLOD_RT_WATCH_RECORD_WORDS;
        spdlog::warn("CLod RT pixel watch {}: pixel {},{} bounce {} flags {:#x} instance {} index {} cluster {} primitive {} misc {:#x} "
                     "a ({:.4g} {:.4g} {:.4g}) b ({:.4g} {:.4g} {:.4g}) c ({:.4g} {:.4g} {:.4g}) | raw a {} {} {} b {} {} {} c {} {} {}",
            kKindNames[kind], r[1] & 0xFFFFu, r[1] >> 16u, (r[0] >> 8u) & 0xFFu, r[0] >> 16u, r[2], r[3], r[4], r[5], r[6],
            asFloat(r[7]), asFloat(r[8]), asFloat(r[9]), asFloat(r[10]), asFloat(r[11]), asFloat(r[12]), asFloat(r[13]), asFloat(r[14]), asFloat(r[15]),
            r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    }
}

void CLodRayTracingSystem::AdvanceFrame() noexcept {
    ++m_frame;
}

} // namespace br::render
