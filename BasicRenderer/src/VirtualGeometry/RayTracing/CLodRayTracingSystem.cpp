#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "BasicRenderer/Assets/Import/CLodOpacityMicromapCache.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Streaming/CLodGeometryTypes.h"
#include "Pipeline/PipelineState/PSOManager.h"
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
constexpr uint32_t kStatsBytes = CLOD_RT_STAT_WORDS * 4u;
constexpr uint32_t kMaxGroupUpdatesPerFrame = 1u << 14;
constexpr uint32_t kMaxSegmentUpdatesPerFrame = 1u << 14;
constexpr uint32_t kClasAlignment = 128u;
constexpr uint32_t kValidationsPerFrame = 1u << 14;
constexpr uint32_t kUavClearDispatchWidth = 256u;
constexpr uint32_t kSceneTimers = 8u;
constexpr uint32_t kRequestBytes = 8u; // [group, the asking alpha-tested draw's material identity or zero]
constexpr uint32_t kOmmArrayAlignment = 128u; // D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT
constexpr uint32_t kOmmScratchAlignment = 256u;
constexpr uint32_t kOmmUploadAlignment = 256u;
static_assert(CLOD_RT_OMM_INDEX_UNIT_BYTES == CLodCache::kOpacityMicromapIndexUnitBytes, "a baked group's index units are the pool's");

constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) noexcept {
    return alignment == 0u ? value : ((value + alignment - 1u) & ~(alignment - 1u));
}

constexpr uint32_t Low(uint64_t value) noexcept { return static_cast<uint32_t>(value); }
constexpr uint32_t High(uint64_t value) noexcept { return static_cast<uint32_t>(value >> 32u); }

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

void BufferBarrier(rhi::CommandList& commands, rhi::ResourceHandle buffer,
    rhi::ResourceSyncState beforeSync, rhi::ResourceAccessType beforeAccess,
    rhi::ResourceSyncState afterSync, rhi::ResourceAccessType afterAccess) {
    rhi::BufferBarrier barrier{};
    barrier.buffer = buffer;
    barrier.beforeSync = beforeSync;
    barrier.beforeAccess = beforeAccess;
    barrier.afterSync = afterSync;
    barrier.afterAccess = afterAccess;
    rhi::BarrierBatch batch{};
    batch.buffers = { &barrier, 1u };
    commands.Barriers(batch);
}

void AsWriteToRead(rhi::CommandList& commands, rhi::ResourceHandle buffer, rhi::ResourceSyncState afterSync) {
    BufferBarrier(commands, buffer,
        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::RaytracingAccelerationStructureWrite,
        afterSync, rhi::ResourceAccessType::RaytracingAccelerationStructureRead);
}

constexpr rhi::ResourceFlags AsStorageFlags() noexcept {
    return static_cast<rhi::ResourceFlags>(rhi::RF_AllowUnorderedAccess | rhi::RF_RaytracingAccelerationStructure);
}

} // namespace

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
    auto [it, inserted] = m_free.emplace(first, count);
    (void)inserted;
    if (auto next = std::next(it); next != m_free.end() && it->first + it->second == next->first) {
        it->second += next->second;
        m_free.erase(next);
    }
    if (it != m_free.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second == it->first) {
            previous->second += it->second;
            m_free.erase(it);
        }
    }
    m_used -= count;
}

bool CLodRayTracingSystem::CreateBuffer(RawBuffer& out, uint64_t bytes, rhi::HeapType heap, rhi::ResourceFlags flags, const char* name, View view, uint32_t stride) {
    const auto desc = rhi::helpers::ResourceDesc::Buffer(bytes, heap, flags, name);
    if (!rhi::IsOk(m_device.CreateCommittedResource(desc, out.resource)) || !out.resource) {
        spdlog::error("CLod RT: failed to allocate '{}' ({} MiB)", name, bytes >> 20u);
        return false;
    }
    out.bytes = bytes;
    out.address = m_device.GetBufferDeviceAddress({ out.Handle(), 0u });
    m_stats.totalMemoryBytes += bytes;
    if (heap == rhi::HeapType::Upload || heap == rhi::HeapType::Readback) {
        void* mapped = nullptr;
        out.resource->Map(&mapped, 0u, bytes);
        out.mapped = static_cast<uint8_t*>(mapped);
    }
    if (view == View::None) return true;

    auto& descriptorHeap = org::DescriptorHeapManager::GetInstance().GetCBVSRVUAVHeap();
    const rhi::DescriptorHeapHandle heapHandle = descriptorHeap->GetHeap().GetHandle();
    const uint64_t elements = view == View::Raw ? bytes / 4u : bytes / stride;

    rhi::SrvDesc srv{};
    srv.dimension = rhi::SrvDim::Buffer;
    srv.buffer.kind = view == View::Raw ? rhi::BufferViewKind::Raw : rhi::BufferViewKind::Structured;
    srv.buffer.numElements = static_cast<uint32_t>(elements);
    srv.buffer.structureByteStride = view == View::Raw ? 0u : stride;
    out.srv = descriptorHeap->AllocateDescriptor();
    if (!rhi::IsOk(m_device.CreateShaderResourceView({ heapHandle, out.srv }, out.Handle(), srv))) return false;

    if ((static_cast<uint32_t>(flags) & rhi::RF_AllowUnorderedAccess) != 0u) {
        rhi::UavDesc uav{};
        uav.dimension = rhi::UavDim::Buffer;
        uav.buffer.kind = srv.buffer.kind;
        uav.buffer.numElements = srv.buffer.numElements;
        uav.buffer.structureByteStride = srv.buffer.structureByteStride;
        out.uav = descriptorHeap->AllocateDescriptor();
        if (!rhi::IsOk(m_device.CreateUnorderedAccessView({ heapHandle, out.uav }, out.Handle(), uav))) return false;
    }
    return true;
}

bool CLodRayTracingSystem::EnsureInitialized(rhi::Device device, const RayTracingFeatureInfo& features) {
    if (m_stats.initialized) return true;
    if (!device || !features.clusterAccelerationStructure || !features.rayQuery) return false;
    m_device = device;
    m_framesInFlight = std::max<uint32_t>(1u, SettingsManager::GetInstance().getSettingGetter<uint8_t>("numFramesInFlight")());
    m_slices.resize(m_framesInFlight);
    m_sliceHasReadback.assign(m_framesInFlight, 0u);
    m_sliceHasStats.assign(m_framesInFlight, 0u);
    const bool dxr2 = features.backendTier == RayTracingBackendTier::DXR_2_0;
    m_ommEnabled = m_ommRequested && features.opacityMicromap && dxr2;
    if (m_ommRequested && !m_ommEnabled) {
        spdlog::warn("CLod RT: opacity micromaps requested, but this device has none; alpha tests stay in any-hit");
    }
    m_stats.ommEnabled = m_ommEnabled;
    m_clusterInfoStride = dxr2
        ? sizeof(rhi::PackedDxr2ClusterBuildTriangleInfo)
        : sizeof(rhi::PackedRayTracingClusterBuildTriangleInfo);

    rhi::RayTracingClusterTriangleInputsDesc buildBatch{};
    buildBatch.vertexFormat = rhi::RayTracingClusterVertexFormat::Float32x3;
    buildBatch.indexFormat = rhi::RayTracingClusterIndexFormat::Uint8;
    buildBatch.maxClusterTriangleCount = kMaxClusterTriangles;
    buildBatch.maxClusterVertexCount = kMaxClusterVertices;
    buildBatch.maxTotalTriangleCount = kMaxClusterTriangles * m_limits.maxClusterBuildsPerFrame;
    buildBatch.maxTotalVertexCount = kMaxClusterVertices * m_limits.maxClusterBuildsPerFrame;
    if (m_ommEnabled) {
        buildBatch.opacityMicromapIndexFormat = rhi::RayTracingClusterIndexFormat::Uint32;
        buildBatch.maxOpacityMicromapIndicesPerCluster = kMaxClusterTriangles;
    }
    rhi::RayTracingRtasOperationInputs clasInputs{};
    clasInputs.allowOpacityMicromaps = m_ommEnabled;
    clasInputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
    clasInputs.operationMode = rhi::RayTracingRtasOperationMode::ExplicitDestinations;
    clasInputs.flags = rhi::RTASBuild_PreferFastTrace;
    clasInputs.maxAccelerationStructureCount = m_limits.maxClusterBuildsPerFrame;
    clasInputs.triangleClusters = &buildBatch;
    rhi::RayTracingRtasPrebuildInfo buildPrebuild{};
    if (!rhi::IsOk(device.GetRayTracingAccelerationStructureOperationPrebuildInfo(clasInputs, buildPrebuild))) return false;
    clasInputs.operationMode = rhi::RayTracingRtasOperationMode::ComputeSizes;
    rhi::RayTracingRtasPrebuildInfo measurePrebuild{};
    if (!rhi::IsOk(device.GetRayTracingAccelerationStructureOperationPrebuildInfo(clasInputs, measurePrebuild))) return false;

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

    const rhi::ResourceFlags uavFlag = rhi::RF_AllowUnorderedAccess;
    constexpr rhi::HeapType local = rhi::HeapType::DeviceLocal;
    m_stats.totalMemoryBytes = 0u;
    bool ok = true;
    m_stats.clasPoolBytes = m_limits.clasPoolBytes;
    ok = ok && CreateBuffer(m_clasPool, m_limits.clasPoolBytes, local, AsStorageFlags(), "CLod RT CLAS pool");
    ok = ok && CreateBuffer(m_clasInfo, uint64_t(m_limits.maxClasSlots) * 16u, local, uavFlag, "CLod RT CLAS info", View::Structured, 16u);
    ok = ok && CreateBuffer(m_clasAddresses, uint64_t(m_limits.maxClasSlots) * 8u, local, uavFlag, "CLod RT CLAS addresses", View::Structured, 8u);
    ok = ok && CreateBuffer(m_groupTable, uint64_t(m_limits.maxGroups) * 8u, local, uavFlag, "CLod RT group table", View::Structured, 8u);
    ok = ok && CreateBuffer(m_segmentTable, uint64_t(m_limits.maxSegments) * sizeof(Segment), local, {}, "CLod RT segment table", View::Structured, sizeof(Segment));
    ok = ok && CreateBuffer(m_buildArgs, uint64_t(m_limits.maxClusterBuildsPerFrame) * 2u * m_clusterInfoStride, local, uavFlag, "CLod RT CLAS build args", View::Structured, m_clusterInfoStride);
    ok = ok && CreateBuffer(m_buildDestinations, uint64_t(m_limits.maxClusterBuildsPerFrame) * 8u, local, uavFlag, "CLod RT CLAS destinations", View::Structured, 8u);
    ok = ok && CreateBuffer(m_buildScratch, std::max<uint64_t>({ buildPrebuild.scratchDataSizeInBytes, measurePrebuild.scratchDataSizeInBytes, 256u }), local, uavFlag, "CLod RT CLAS scratch");
    ok = ok && CreateBuffer(m_requests, uint64_t(m_limits.requestCapacity) * kRequestBytes, local, uavFlag, "CLod RT CLAS requests", View::Structured, kRequestBytes);
    ok = ok && CreateBuffer(m_slotOmmKeys, uint64_t(m_limits.maxClasSlots) * 4u, local, uavFlag, "CLod RT CLAS micromap keys", View::Structured, 4u);
    ok = ok && CreateBuffer(m_blasOmmKeys, uint64_t(m_limits.maxBlases) * 4u, local, uavFlag, "CLod RT BLAS micromap keys", View::Structured, 4u);
    ok = ok && CreateBuffer(m_requestCounter, 16u, local, uavFlag, "CLod RT CLAS request counter", View::Structured, 4u);
    ok = ok && CreateBuffer(m_drawTable, uint64_t(m_limits.maxDrawRecords) * 16u, local, uavFlag, "CLod RT draw table", View::Raw);
    ok = ok && CreateBuffer(m_instanceBlas, uint64_t(m_limits.maxInstances) * 4u, local, uavFlag, "CLod RT instance BLAS index", View::Structured, 4u);
    ok = ok && CreateBuffer(m_shareTable, uint64_t(m_limits.blasShareEntries) * 16u, local, uavFlag, "CLod RT BLAS sharing", View::Raw);
    ok = ok && CreateBuffer(m_instanceList, uint64_t(m_limits.maxInstances) * 4u, local, uavFlag, "CLod RT instance list", View::Structured, 4u);
    ok = ok && CreateBuffer(m_coarseCut, CLOD_RT_COARSE_BUFFER_BYTES, local, uavFlag, "CLod RT coarse cut", View::Raw);
    ok = ok && CreateBuffer(m_counters, kCountersBytes, local, uavFlag, "CLod RT counters", View::Raw);
    ok = ok && CreateBuffer(m_blasArgs, uint64_t(m_limits.maxBlases) * sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo), local, uavFlag, "CLod RT BLAS args", View::Structured, sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo));
    ok = ok && CreateBuffer(m_blasAddressArray, uint64_t(m_limits.maxClusterReferences) * 8u, local, uavFlag, "CLod RT BLAS CLAS references", View::Structured, 8u);
    ok = ok && CreateBuffer(m_blasResult, blasPrebuild.resultDataMaxSizeInBytes, local, AsStorageFlags(), "CLod RT BLAS data");
    ok = ok && CreateBuffer(m_blasScratch, std::max<uint64_t>(blasPrebuild.scratchDataSizeInBytes, 256u), local, uavFlag, "CLod RT BLAS scratch");
    ok = ok && CreateBuffer(m_blasAddresses, uint64_t(m_limits.maxBlases) * 8u, local, uavFlag, "CLod RT BLAS addresses", View::Structured, 8u);
    ok = ok && CreateBuffer(m_blasSizes, uint64_t(m_limits.maxBlases) * 8u, local, uavFlag, "CLod RT BLAS sizes");
    ok = ok && CreateBuffer(m_tlasInstances, uint64_t(m_limits.maxInstances) * sizeof(rhi::PackedRayTracingInstanceDesc), local, uavFlag, "CLod RT TLAS instances", View::Structured, sizeof(rhi::PackedRayTracingInstanceDesc));
    ok = ok && CreateBuffer(m_visibilityHash, uint64_t(m_limits.visibilityHashEntries) * 8u, local, uavFlag, "CLod RT visible cluster hash", View::Raw);
    ok = ok && CreateBuffer(m_visibilityStats, kStatsBytes, local, uavFlag, "CLod RT visibility comparison", View::Raw);

    // A whole number of BuildSources, so one structured view can reach every slice.
    m_uploadSliceBytes = (uint64_t(m_limits.maxClusterBuildsPerFrame) * (sizeof(BuildSource) * 2u + 8u)
        + uint64_t(kMaxGroupUpdatesPerFrame) * 8u + uint64_t(kMaxSegmentUpdatesPerFrame) * sizeof(Segment) + sizeof(BuildSource) * 2u)
        / sizeof(BuildSource) * sizeof(BuildSource);
    m_readbackSliceBytes = AlignUp(uint64_t(kCountersBytes) + kStatsBytes + 16u + uint64_t(m_limits.requestCapacity) * kRequestBytes + kSceneTimers * 8u, 256u);
    ok = ok && CreateBuffer(m_upload, m_uploadSliceBytes * m_framesInFlight, rhi::HeapType::Upload, {}, "CLod RT upload", View::Structured, sizeof(BuildSource));
    ok = ok && CreateBuffer(m_readback, m_readbackSliceBytes * m_framesInFlight, rhi::HeapType::Readback, {}, "CLod RT readback");
    ok = ok && CreateBuffer(m_clasSizes, uint64_t(m_limits.maxClusterBuildsPerFrame) * 8u, local, uavFlag, "CLod RT CLAS sizes");
    ok = ok && CreateBuffer(m_clasSizesReadback, uint64_t(m_limits.maxClusterBuildsPerFrame) * 8u * m_framesInFlight, rhi::HeapType::Readback, {}, "CLod RT CLAS sizes readback");
    if (m_ommEnabled) {
        ok = ok && CreateBuffer(m_ommArrayPool, m_limits.ommArrayPoolBytes, local, AsStorageFlags(), "CLod RT micromap arrays");
        ok = ok && CreateBuffer(m_ommIndexPool, uint64_t(m_limits.ommIndexUnits) * CLOD_RT_OMM_INDEX_UNIT_BYTES, local, uavFlag, "CLod RT micromap indices", View::Raw);
        ok = ok && CreateBuffer(m_ommScratch, AlignUp(m_limits.ommScratchBytes, kOmmScratchAlignment), local, uavFlag, "CLod RT micromap scratch");
        // Baked micromaps go to the array builds straight from here.
        ok = ok && CreateBuffer(m_ommUpload, m_limits.ommUploadBytesPerFrame * m_framesInFlight, rhi::HeapType::Upload, {}, "CLod RT baked micromaps");
        m_ommArrayAllocator.Reset(static_cast<uint32_t>(m_limits.ommArrayPoolBytes / kOmmArrayAlignment));
        m_ommIndexAllocator.Reset(m_limits.ommIndexUnits);
    }
    if (!ok) return false;

    rhi::RayTracingGeometryDesc tlasGeometry{};
    tlasGeometry.type = rhi::RayTracingGeometryType::Instances;
    tlasGeometry.instances.instanceBuffer = { m_tlasInstances.Handle(), 0u, m_tlasInstances.bytes };
    tlasGeometry.instances.stride = sizeof(rhi::PackedRayTracingInstanceDesc);
    tlasGeometry.instances.count = m_limits.maxInstances;
    rhi::AccelerationStructureBuildInputs tlasInputs{};
    tlasInputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
    tlasInputs.flags = rhi::RTASBuild_PreferFastBuild;
    tlasInputs.geometries = { &tlasGeometry, 1u };
    rhi::AccelerationStructurePrebuildInfo tlasPrebuild{};
    if (!rhi::IsOk(device.GetAccelerationStructurePrebuildInfo(tlasInputs, tlasPrebuild))) return false;
    m_stats.tlasBytes = tlasPrebuild.resultDataMaxSizeInBytes;
    ok = ok && CreateBuffer(m_tlasStorage, tlasPrebuild.resultDataMaxSizeInBytes, local, AsStorageFlags(), "CLod RT TLAS");
    ok = ok && CreateBuffer(m_tlasScratch, std::max<uint64_t>(tlasPrebuild.scratchDataSizeInBytes, 256u), local, uavFlag, "CLod RT TLAS scratch");
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

    // Over budget, the OS demotes what it likes, and the per-instance reads here get about twenty times slower.
    for (const RawBuffer* buffer : { &m_clasPool, &m_clasInfo, &m_clasAddresses, &m_groupTable, &m_segmentTable, &m_drawTable, &m_instanceList, &m_instanceBlas, &m_shareTable, &m_counters,
        &m_blasArgs, &m_blasAddressArray, &m_blasResult, &m_blasScratch, &m_blasAddresses, &m_tlasInstances, &m_tlasStorage, &m_tlasScratch, &m_slotOmmKeys, &m_blasOmmKeys }) {
        rhi::PageableRef pageable(buffer->Handle());
        if (!rhi::IsOk(device.SetResidencyPriority(rhi::Span<rhi::PageableRef>{ &pageable, 1u }, rhi::ResidencyPriorityHigh))) {
            spdlog::warn("CLod RT: could not raise the residency priority of its buffers");
            break;
        }
    }

    m_slotAllocator.Reset(m_limits.maxClasSlots);
    m_memoryAllocator.Reset(static_cast<uint32_t>(m_clasPool.bytes / kClasAlignment));
    m_segmentAllocator.Reset(m_limits.maxSegments);
    m_stats.initialized = true;
    if (m_ommEnabled) {
        spdlog::info("CLod RT: baked opacity micromaps, {} MiB of arrays, {} clusters of indices, {} MiB uploaded a frame at most",
            m_limits.ommArrayPoolBytes >> 20u, m_limits.ommIndexUnits, m_limits.ommUploadBytesPerFrame >> 20u);
    }
    spdlog::info(
        "CLod RT: {} MiB CLAS pool for up to {} clusters, BLAS batch {} MiB + {} MiB scratch, TLAS {} MiB + {} MiB scratch, CLAS scratch {} MiB, total {} MiB",
        m_clasPool.bytes >> 20u, m_limits.maxClasSlots,
        blasPrebuild.resultDataMaxSizeInBytes >> 20u, blasPrebuild.scratchDataSizeInBytes >> 20u,
        tlasPrebuild.resultDataMaxSizeInBytes >> 20u, tlasPrebuild.scratchDataSizeInBytes >> 20u,
        buildPrebuild.scratchDataSizeInBytes >> 20u, m_stats.totalMemoryBytes >> 20u);
    return true;
}

CLodRayTracingSystem::CullingDescriptors CLodRayTracingSystem::GetCullingDescriptors() const noexcept {
    return { m_groupTable.srv, m_requests.uav, m_requestCounter.uav, m_limits.requestCapacity, m_coarseCut.uav };
}

CLodRayTracingSystem::VisibilityDescriptors CLodRayTracingSystem::GetVisibilityDescriptors() const noexcept {
    return { m_tlasSrv, m_clasInfo.srv, m_visibilityHash.uav, m_limits.visibilityHashEntries, m_visibilityStats.uav };
}

void CLodRayTracingSystem::SetPrimaryVisibleClusters(std::shared_ptr<org::Buffer> clusters, std::shared_ptr<org::Buffer> transformIndices, uint32_t capacity) {
    m_primaryVisibleClusters = std::move(clusters);
    m_primaryTransformIndices = std::move(transformIndices);
    m_primaryVisibleClusterCapacity = capacity;
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
    resultBytes = AlignUp(prebuild.resultDataMaxSizeInBytes, kOmmArrayAlignment);
    scratchBytes = AlignUp(std::max<uint64_t>(prebuild.scratchDataSizeInBytes, 1u), kOmmScratchAlignment);
    m_ommArraySizes.emplace(levelCounts, std::make_pair(resultBytes, scratchBytes));
    return true;
}

void CLodRayTracingSystem::FreeGroup(uint32_t groupGlobal, CachedGroup& group) {
    UploadSlice& slice = m_slices[m_frame % m_framesInFlight];
    slice.groupUpdates.push_back({ groupGlobal, 0u, 0u });
    for (const Run& run : group.slots) {
        m_slotAllocator.Free(run.first, run.count);
        m_stats.clasFreedTotal += run.count;
    }
    if (group.memory.count != 0u) m_memoryAllocator.Free(group.memory.first, group.memory.count);
    if (group.ommArray.count != 0u) m_ommArrayAllocator.Free(group.ommArray.first, group.ommArray.count);
    if (group.ommIndices.count != 0u) m_ommIndexAllocator.Free(group.ommIndices.first, group.ommIndices.count);
    group.ommArray = {};
    group.ommIndices = {};
    m_segmentAllocator.Free(group.firstSegment, group.segmentCount);
}

std::shared_ptr<const CLodCache::OpacityMicromapFile> CLodRayTracingSystem::OmmFile(const std::wstring& containerPath, uint32_t ommIdentity) {
    const std::wstring path = CLodCache::OpacityMicromapPath(containerPath, ommIdentity);
    if (const auto it = m_ommFiles.find(path); it != m_ommFiles.end()) return it->second;
    if (m_ommFiles.size() >= m_limits.ommFilesCached) {
        // Groups already built keep their micromaps; only reopening costs.
        m_ommFiles.clear();
    }
    std::string reason;
    auto file = CLodCache::OpacityMicromapFile::Open(path, ommIdentity, CLodCache::OpacityMicromapContainerBytes(containerPath), &reason);
    if (!file && reason != "no file") {
        spdlog::warn("CLod RT: ignoring '{}': {}", std::filesystem::path(path).filename().string(), reason);
    }
    m_ommFiles.emplace(path, file);
    m_stats.ommFilesOpen = static_cast<uint32_t>(m_ommFiles.size());
    return file;
}

bool CLodRayTracingSystem::LoadOmmGroup(const std::wstring& containerPath, uint32_t groupLocalIndex, uint32_t ommIdentity, uint32_t groupGlobal,
    std::vector<BuildSource>& sources, Run& outIndices, Run& outArray) {
    outIndices = {};
    outArray = {};
    const auto file = containerPath.empty() ? nullptr : OmmFile(containerPath, ommIdentity);
    CLodCache::OpacityMicromapGroupView view{};
    uint32_t clusterCount = 0u;
    for (const BuildSource& source : sources) clusterCount += source.meshletCount;
    if (!file || !file->Group(groupLocalIndex, view) || view.clusterCount != clusterCount) {
        ++m_stats.ommGroupsWithout;
        return true;
    }

    UploadSlice& slice = m_slices[m_frame % m_framesInFlight];
    const uint64_t indexBytes = view.indices.size();
    const uint64_t descsBytes = view.descs.size_bytes();
    const uint64_t uploadBytes = AlignUp(indexBytes, kOmmUploadAlignment) + AlignUp(descsBytes, kOmmUploadAlignment) + AlignUp(view.data.size(), kOmmUploadAlignment);
    if (uploadBytes > m_limits.ommUploadBytesPerFrame) {
        // Never fits: any-hit instead of waiting forever.
        ++m_stats.ommGroupsWithout;
        return true;
    }
    if (slice.ommUploadBytes + uploadBytes > m_limits.ommUploadBytesPerFrame) return false;
    uint64_t arrayBytes = 0u;
    uint64_t scratchBytes = 0u;
    const bool hasArray = !view.descs.empty();
    if (hasArray) {
        if (!OmmArraySize(view.levelCounts, arrayBytes, scratchBytes) || scratchBytes > m_ommScratch.bytes) {
            ++m_stats.ommGroupsWithout;
            return true;
        }
        if (slice.ommScratchBytes + scratchBytes > m_ommScratch.bytes) return false;
    }

    // Indices first, then the array: either may evict, and evicting never touches this group.
    Run indices{};
    while (!m_ommIndexAllocator.Allocate(clusterCount, indices.first)) {
        if (!EvictOldest(groupGlobal)) return false;
    }
    indices.count = clusterCount;
    Run array{};
    if (hasArray) {
        const uint32_t units = static_cast<uint32_t>(arrayBytes / kOmmArrayAlignment);
        while (!m_ommArrayAllocator.Allocate(units, array.first)) {
            if (!EvictOldest(groupGlobal)) {
                m_ommIndexAllocator.Free(indices.first, indices.count);
                return false;
            }
        }
        array.count = units;
    }

    uint8_t* upload = m_ommUpload.mapped + uint64_t(m_frame % m_framesInFlight) * m_limits.ommUploadBytesPerFrame;
    const uint64_t indicesOffset = slice.ommUploadBytes;
    const uint64_t descsOffset = indicesOffset + AlignUp(indexBytes, kOmmUploadAlignment);
    const uint64_t dataOffset = descsOffset + AlignUp(descsBytes, kOmmUploadAlignment);
    std::memcpy(upload + indicesOffset, view.indices.data(), indexBytes);
    if (hasArray) {
        std::memcpy(upload + descsOffset, view.descs.data(), descsBytes);
        std::memcpy(upload + dataOffset, view.data.data(), view.data.size());
    }
    slice.ommUploadBytes += uploadBytes;
    slice.ommIndexCopies.push_back({ indicesOffset, indices.first, indices.count });
    const uint64_t arrayAddress = hasArray ? m_ommArrayPool.address + uint64_t(array.first) * kOmmArrayAlignment : 0u;
    if (hasArray) {
        OmmArrayBuild build{};
        build.destination = arrayAddress;
        build.scratchOffset = slice.ommScratchBytes;
        build.descsOffset = uint64_t(m_frame % m_framesInFlight) * m_limits.ommUploadBytesPerFrame + descsOffset;
        build.dataOffset = uint64_t(m_frame % m_framesInFlight) * m_limits.ommUploadBytesPerFrame + dataOffset;
        build.count = static_cast<uint32_t>(view.descs.size());
        build.levelCounts = view.levelCounts;
        slice.ommArrayBuilds.push_back(build);
        slice.ommScratchBytes += scratchBytes;
    }
    uint32_t unit = indices.first;
    for (BuildSource& source : sources) {
        source.ommKey = ommIdentity;
        source.ommIndexUnit = unit;
        source.ommArray = arrayAddress;
        unit += source.meshletCount;
    }
    outIndices = indices;
    outArray = array;
    ++m_stats.ommGroupsLoaded;
    m_stats.ommMicromapsTotal += view.descs.size();
    m_stats.ommUploadBytesTotal += uploadBytes;
    return true;
}

bool CLodRayTracingSystem::EvictOldest(uint32_t except) {
    // First-in-first-out: anything still in use asks again next frame.
    while (!m_buildOrder.empty()) {
        const uint32_t oldest = m_buildOrder.front();
        if (oldest == except) return false;
        m_buildOrder.pop_front();
        if (auto it = m_groups.find(oldest); it != m_groups.end()) {
            FreeGroup(oldest, it->second);
            m_groups.erase(it);
            return true;
        }
    }
    return false;
}

bool CLodRayTracingSystem::MeasureGroup(const MeshManager& meshManager, uint32_t groupGlobal, uint32_t ommIdentity, uint32_t& measuresLeft) {
    CLodRayTracingGroupLookup lookup{};
    if (groupGlobal >= m_limits.maxGroups || !meshManager.GetCLodRayTracingGroup(groupGlobal, lookup)) return true;
    const CLodRayTracingResidentGroup& group = lookup.group;
    if ((group.group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL)) != 0u
        || group.chunk.compressedPositionQuantExp != CLOD_POSITION_FORMAT_FLOAT3
        || group.pageAllocations.empty()) {
        return true; // Nothing to build. Ignored, and asked for again by anyone who still selects it.
    }

    uint32_t total = 0u;
    uint32_t segmentCount = 0u;
    for (const ClusterLODGroupSegment& segment : group.segments) {
        if (segment.meshletCount != 0u) {
            total += segment.meshletCount;
            ++segmentCount;
        }
    }
    if (total == 0u) return true;
    if (total > measuresLeft) return false;

    UploadSlice& slice = m_slices[m_frame % m_framesInFlight];
    while (m_slotAllocator.Used() + total > m_limits.maxClasSlots && EvictOldest(groupGlobal)) {}

    CachedGroup cached{};
    cached.owner = lookup.owner;
    cached.localIndex = group.groupLocalIndex;
    cached.firstPageId = group.pageAllocations.front().firstPageID;
    cached.segmentCount = segmentCount;
    cached.id = m_nextGroupId++;
    if (!m_segmentAllocator.Allocate(segmentCount, cached.firstSegment)) return false;

    MeasureRequest request{};
    request.groupGlobal = groupGlobal;
    request.id = cached.id;
    request.clusterCount = total;
    request.firstMeasureIndex = slice.measureCount;

    PagePool& pool = *lookup.pagePool;
    uint32_t measureIndex = slice.measureCount;
    for (const ClusterLODGroupSegment& segment : group.segments) {
        if (segment.meshletCount == 0u) continue;
        uint32_t localPage = segment.pageIndex >= group.group.pageMapBase ? segment.pageIndex - group.group.pageMapBase : ~0u;
        uint32_t pageId = ~0u;
        const PagePool::PageAllocation* pageAllocation = nullptr;
        for (const PagePool::PageAllocation& allocation : group.pageAllocations) {
            if (localPage < allocation.pageCount) {
                pageId = allocation.firstPageID + localPage;
                pageAllocation = &allocation;
                break;
            }
            localPage -= allocation.pageCount;
        }
        uint32_t firstSlot = 0u;
        const bool allocated = pageId != ~0u && m_slotAllocator.Allocate(segment.meshletCount, firstSlot);
        if (!allocated) {
            for (const Run& run : cached.slots) m_slotAllocator.Free(run.first, run.count);
            m_segmentAllocator.Free(cached.firstSegment, cached.segmentCount);
            return pageId == ~0u; // A bad mapping won't improve; a full table may.
        }
        cached.slots.push_back({ firstSlot, segment.meshletCount });

        const uint64_t pageByteOffset = pool.PageToSlabByteOffset(pageId);
        const auto slab = pool.GetSlab(pool.PageToSlabIndex(pageId));
        BuildSource source{};
        source.slabAddress = m_device.GetBufferDeviceAddress({ slab->GetAPIResource().GetHandle(), 0u });
        source.pageByteOffset = static_cast<uint32_t>(pageByteOffset);
        source.slabDescriptor = pool.GetSlabDescriptorIndex(*pageAllocation);
        source.pageOffsetUnits = static_cast<uint32_t>(pageByteOffset >> 14u);
        source.firstMeshlet = segment.firstMeshletInPage;
        source.meshletCount = segment.meshletCount;
        source.firstBuildIndex = measureIndex;
        source.firstClasSlot = firstSlot;
        source.groupLocalIndex = group.groupLocalIndex;
        request.sources.push_back(source);
        measureIndex += segment.meshletCount;

        Segment value{};
        value.slabDescriptor = source.slabDescriptor;
        value.pageOffsetUnits = source.pageOffsetUnits;
        value.firstMeshlet = source.firstMeshlet;
        value.meshletCount = source.meshletCount;
        value.firstClasSlot = firstSlot;
        request.segments.push_back(value);
    }
    if (OpacityMicromapsActive() && ommIdentity != 0u) {
        // Micromaps baked for the asking draw's material, when there are some. Deferring frees what this took.
        if (!LoadOmmGroup(lookup.containerPath, group.groupLocalIndex, ommIdentity, groupGlobal, request.sources, cached.ommIndices, cached.ommArray)) {
            for (const Run& run : cached.slots) m_slotAllocator.Free(run.first, run.count);
            m_segmentAllocator.Free(cached.firstSegment, cached.segmentCount);
            return false;
        }
    }
    slice.measureSources.insert(slice.measureSources.end(), request.sources.begin(), request.sources.end());
    slice.measureCount = measureIndex;
    slice.measuring.push_back(std::move(request));
    measuresLeft -= total;
    m_groups.emplace(groupGlobal, std::move(cached));
    m_buildOrder.push_back(groupGlobal);
    m_validationQueue.push_back(groupGlobal);
    return true;
}

// True when finished with this entry, whether that means built or no longer wanted.
bool CLodRayTracingSystem::BuildMeasuredGroup(const MeshManager& meshManager, AwaitingBuild& awaiting, uint32_t& buildsLeft) {
    const auto it = m_groups.find(awaiting.groupGlobal);
    if (it == m_groups.end() || it->second.id != awaiting.id) return true;
    CachedGroup& cached = it->second;
    if (!meshManager.IsCLodRayTracingGroupCurrent(cached.owner, cached.localIndex, cached.firstPageId)) {
        FreeGroup(awaiting.groupGlobal, cached);
        m_groups.erase(it);
        return true;
    }

    const uint32_t clusterCount = static_cast<uint32_t>(awaiting.sizes.size());
    UploadSlice& slice = m_slices[m_frame % m_framesInFlight];
    if (clusterCount > buildsLeft || slice.groupUpdates.size() >= kMaxGroupUpdatesPerFrame
        || slice.segmentUpdates.size() + awaiting.segments.size() > kMaxSegmentUpdatesPerFrame) {
        return false;
    }

    uint32_t units = 0u;
    for (const uint32_t size : awaiting.sizes) units += static_cast<uint32_t>(AlignUp(std::max(size, 1u), kClasAlignment) / kClasAlignment);
    uint32_t first = 0u;
    while (!m_memoryAllocator.Allocate(units, first)) {
        if (!EvictOldest(awaiting.groupGlobal)) return false;
    }
    cached.memory = { first, units };
    m_stats.clasBytesUsed = uint64_t(m_memoryAllocator.Used()) * kClasAlignment;

    uint64_t address = m_clasPool.address + uint64_t(first) * kClasAlignment;
    for (const uint32_t size : awaiting.sizes) {
        slice.destinations.push_back(address);
        address += AlignUp(std::max(size, 1u), kClasAlignment);
    }
    uint32_t buildIndex = slice.buildCount;
    for (BuildSource source : awaiting.sources) {
        source.firstBuildIndex = buildIndex;
        buildIndex += source.meshletCount;
        slice.sources.push_back(source);
    }
    slice.buildCount = buildIndex;
    for (size_t i = 0; i < awaiting.segments.size(); ++i) {
        slice.segmentUpdates.push_back({ cached.firstSegment + static_cast<uint32_t>(i), awaiting.segments[i] });
    }
    slice.groupUpdates.push_back({ awaiting.groupGlobal, cached.firstSegment, cached.segmentCount });
    cached.sizesKnown = true;
    buildsLeft -= clusterCount;
    m_stats.clasBuildsLastFrame += clusterCount;
    m_stats.clasBuildsTotal += clusterCount;
    return true;
}

void CLodRayTracingSystem::Refresh(const MeshManager& meshManager) {
    if (!m_stats.initialized) return;
    const uint32_t sliceIndex = m_frame % m_framesInFlight;
    UploadSlice& slice = m_slices[sliceIndex];
    slice.sources.clear();
    slice.groupUpdates.clear();
    slice.segmentUpdates.clear();
    slice.buildCount = 0u;
    m_stats.clasBuildsLastFrame = 0u;

    // This slice's last use was framesInFlight frames ago, so its readback has landed.
    // Sizes measured when this slice last ran have landed too, and the groups they belong to can now be built.
    const auto* measuredSizes = reinterpret_cast<const uint64_t*>(m_clasSizesReadback.mapped + uint64_t(sliceIndex) * m_clasSizes.bytes);
    for (MeasureRequest& request : slice.measuring) {
        AwaitingBuild awaiting{};
        awaiting.groupGlobal = request.groupGlobal;
        awaiting.id = request.id;
        awaiting.sources = std::move(request.sources);
        awaiting.segments = std::move(request.segments);
        awaiting.sizes.resize(request.clusterCount);
        for (uint32_t i = 0u; i < request.clusterCount; ++i) {
            awaiting.sizes[i] = static_cast<uint32_t>(measuredSizes[request.firstMeasureIndex + i]);
        }
        m_awaitingBuild.push_back(std::move(awaiting));
    }
    slice.measuring.clear();
    slice.measureSources.clear();
    slice.measureCount = 0u;
    slice.destinations.clear();

    slice.ommArrayBuilds.clear();
    slice.ommIndexCopies.clear();
    slice.ommUploadBytes = 0u;
    slice.ommScratchBytes = 0u;

    std::vector<std::pair<uint32_t, uint32_t>> requests; // Group, and the material identity of an alpha-tested draw that asked.
    if (m_sliceHasReadback[sliceIndex] != 0u) {
        const uint8_t* base = m_readback.mapped + SliceOffset(m_readbackSliceBytes, sliceIndex);
        uint32_t counters[CLOD_RT_COUNTER_WORDS]{};
        std::memcpy(counters, base, sizeof(counters));
        m_stats.instances = std::min(counters[CLOD_RT_COUNTER_INSTANCES], m_limits.maxInstances);
        m_stats.clustersInBvh = counters[CLOD_RT_COUNTER_CLUSTERS];
        m_stats.distinctBlases = counters[CLOD_RT_COUNTER_BLAS_ARGS];
        // A list that outgrows this loses clusters from the BVH for a few frames while it catches up, at most.
        m_listClusterEstimate = std::min(counters[CLOD_RT_COUNTER_LIST_CLUSTERS] + counters[CLOD_RT_COUNTER_LIST_CLUSTERS] / 2u + (1u << 16), m_limits.maxClustersInBvh);
        m_stats.droppedInstances = counters[CLOD_RT_COUNTER_DROPPED_INSTANCES];
        m_stats.droppedClusters = counters[CLOD_RT_COUNTER_DROPPED_CLUSTERS];
        m_stats.droppedBlases = counters[CLOD_RT_COUNTER_DROPPED_BLASES];
        m_stats.listClusters = counters[CLOD_RT_COUNTER_LIST_CLUSTERS];
        m_stats.coarseConsumers = counters[CLOD_RT_COUNTER_COARSE_CONSUMERS];
        m_stats.alphaInstances = counters[CLOD_RT_COUNTER_ALPHA_INSTANCES];
        m_stats.ommInstances = counters[CLOD_RT_COUNTER_OMM_INSTANCES];
        m_stats.ommDisabledInstances = counters[CLOD_RT_COUNTER_OMM_DISABLED_INSTANCES];
        // Some headroom: rows past the instance count are inactive, so overshooting costs little and undershooting
        // drops instances for a frame.
        m_tlasInstanceCount = std::clamp(m_stats.instances + m_stats.instances / 4u + 256u, 256u, m_limits.maxInstances);

        if (m_timerTicksPerSecond != 0u) {
            uint64_t ticks[kSceneTimers]{};
            std::memcpy(ticks, base + kCountersBytes + kStatsBytes + 16u + uint64_t(m_limits.requestCapacity) * kRequestBytes, sizeof(ticks));
            const double msPerTick = 1000.0 / static_cast<double>(m_timerTicksPerSecond);
            const auto stage = [&](uint32_t index) { return static_cast<float>(static_cast<double>(ticks[index + 1u] - ticks[index]) * msPerTick); };
            m_stats.msClasBuild = stage(0u);
            m_stats.msCount = stage(1u);
            m_stats.msAllocate = stage(2u);
            m_stats.msPlace = stage(3u);
            m_stats.msBlasBuild = stage(4u);
            m_stats.msWriteInstances = stage(5u);
            m_stats.msTlasBuild = stage(6u);
        }

        if (m_sliceHasStats[sliceIndex] != 0u) {
            std::memcpy(m_stats.comparison.data(), base + kCountersBytes, kStatsBytes);
            m_stats.comparisonValid = true;
        }

        uint32_t requestCount = 0u;
        std::memcpy(&requestCount, base + kCountersBytes + kStatsBytes, sizeof(requestCount));
        m_stats.requestsLastFrame = requestCount;
        requestCount = std::min(requestCount, m_limits.requestCapacity);
        std::unordered_map<uint32_t, size_t> seen;
        seen.reserve(requestCount);
        const auto* requested = reinterpret_cast<const uint32_t*>(base + kCountersBytes + kStatsBytes + 16u);
        for (uint32_t i = 0u; i < requestCount; ++i) {
            const uint32_t group = requested[i * 2u];
            const uint32_t material = requested[i * 2u + 1u];
            if (const auto found = seen.find(group); found != seen.end()) {
                // Any alpha-tested draw that asked is enough to load micromaps for.
                if (requests[found->second].second == 0u) requests[found->second].second = material;
                continue;
            }
            if (m_groups.contains(group)) continue;
            seen.emplace(group, requests.size());
            requests.emplace_back(group, material);
        }
    }

    // Free what streaming took away. In batches, so a large cache doesn't cost a long stall on one frame.
    const size_t validations = std::min<size_t>(kValidationsPerFrame, m_validationQueue.size());
    for (size_t i = 0u; i < validations; ++i) {
        if (slice.groupUpdates.size() >= kMaxGroupUpdatesPerFrame / 2u) break;
        if (m_validationCursor >= m_validationQueue.size()) m_validationCursor = 0u;
        const uint32_t groupGlobal = m_validationQueue[m_validationCursor];
        const auto it = m_groups.find(groupGlobal);
        if (it == m_groups.end()) {
            m_validationQueue[m_validationCursor] = m_validationQueue.back();
            m_validationQueue.pop_back();
            continue;
        }
        const CachedGroup& group = it->second;
        if (!meshManager.IsCLodRayTracingGroupCurrent(group.owner, group.localIndex, group.firstPageId)) {
            FreeGroup(groupGlobal, it->second);
            m_groups.erase(it);
            m_validationQueue[m_validationCursor] = m_validationQueue.back();
            m_validationQueue.pop_back();
            continue;
        }
        ++m_validationCursor;
    }
    while (m_buildOrder.size() > m_groups.size() * 2u + 1024u) m_buildOrder.pop_front();

    uint32_t buildsLeft = m_limits.maxClusterBuildsPerFrame;
    while (!m_awaitingBuild.empty() && BuildMeasuredGroup(meshManager, m_awaitingBuild.front(), buildsLeft)) {
        m_awaitingBuild.pop_front();
    }
    m_stats.groupsAwaitingBuild = static_cast<uint32_t>(m_awaitingBuild.size());

    uint32_t measuresLeft = m_limits.maxClusterBuildsPerFrame;
    uint32_t deferred = 0u;
    for (const auto& [group, identity] : requests) {
        if (!MeasureGroup(meshManager, group, identity, measuresLeft)) ++deferred;
    }
    m_stats.requestsDeferred = deferred;
    m_stats.cachedGroups = static_cast<uint32_t>(m_groups.size());
    m_stats.clasSlotsUsed = m_slotAllocator.Used();
    m_stats.clasBytesUsed = uint64_t(m_memoryAllocator.Used()) * kClasAlignment;

    // Fill the upload slice.
    uint8_t* upload = m_upload.mapped + SliceOffset(m_uploadSliceBytes, sliceIndex);
    if (!slice.sources.empty()) std::memcpy(upload, slice.sources.data(), slice.sources.size() * sizeof(BuildSource));
    upload += uint64_t(m_limits.maxClusterBuildsPerFrame) * sizeof(BuildSource);
    if (!slice.measureSources.empty()) std::memcpy(upload, slice.measureSources.data(), slice.measureSources.size() * sizeof(BuildSource));
    upload += uint64_t(m_limits.maxClusterBuildsPerFrame) * sizeof(BuildSource);
    if (!slice.destinations.empty()) std::memcpy(upload, slice.destinations.data(), slice.destinations.size() * 8u);
    upload += uint64_t(m_limits.maxClusterBuildsPerFrame) * 8u;
    for (size_t i = 0; i < slice.groupUpdates.size(); ++i) {
        const uint32_t value[2] = { slice.groupUpdates[i].first, slice.groupUpdates[i].count };
        std::memcpy(upload + i * sizeof(value), value, sizeof(value));
    }
    upload += uint64_t(kMaxGroupUpdatesPerFrame) * 8u;
    for (size_t i = 0; i < slice.segmentUpdates.size(); ++i) {
        std::memcpy(upload + i * sizeof(Segment), &slice.segmentUpdates[i].value, sizeof(Segment));
    }
    upload += uint64_t(kMaxSegmentUpdatesPerFrame) * sizeof(Segment);
    std::memcpy(upload, &slice.buildCount, sizeof(uint32_t));
    std::memcpy(upload + 4u, &slice.measureCount, sizeof(uint32_t));

    if (m_ommEnabled) {
        m_stats.ommArrayBytesUsed = uint64_t(m_ommArrayAllocator.Used()) * kOmmArrayAlignment;
        m_stats.ommIndexUnitsUsed = m_ommIndexAllocator.Used();
    }

    if ((m_frame % 120u) == 0u) {
        rhi::VideoMemoryInfo local{}, nonLocal{};
        if (rhi::IsOk(m_device.QueryVideoMemoryInfo(0u, rhi::MemorySegmentGroup::Local, local)) && rhi::IsOk(m_device.QueryVideoMemoryInfo(0u, rhi::MemorySegmentGroup::NonLocal, nonLocal))) {
            spdlog::info("CLod RT memory: local {} of {} MiB budget, non-local {} of {} MiB budget", local.currentUsageBytes >> 20u, local.budgetBytes >> 20u, nonLocal.currentUsageBytes >> 20u, nonLocal.budgetBytes >> 20u);
        }
        spdlog::info(
            "CLod RT: instances={} untraversed={} BLASes={} clusters={} list={} (dropped {} instances, {} clusters, {} BLASes) CLAS {} of {} MiB, {} slots, groups={} awaiting={} built={} freed={} requests={} deferred={} TLAS rows={} | GPU ms: CLAS {:.2f} count {:.2f} allocate {:.2f} place {:.2f} BLAS {:.2f} instances {:.2f} TLAS {:.2f}",
            m_stats.instances, m_stats.coarseConsumers, m_stats.distinctBlases, m_stats.clustersInBvh, m_stats.listClusters, m_stats.droppedInstances, m_stats.droppedClusters, m_stats.droppedBlases,
            m_stats.clasBytesUsed >> 20u, m_stats.clasPoolBytes >> 20u, m_stats.clasSlotsUsed, m_stats.cachedGroups, m_stats.groupsAwaitingBuild, m_stats.clasBuildsTotal, m_stats.clasFreedTotal,
            m_stats.requestsLastFrame, m_stats.requestsDeferred, m_tlasInstanceCount,
            m_stats.msClasBuild, m_stats.msCount, m_stats.msAllocate, m_stats.msPlace, m_stats.msBlasBuild, m_stats.msWriteInstances, m_stats.msTlasBuild);
        if (m_ommEnabled) {
            spdlog::info("CLod RT micromaps: groups loaded={} without={} micromaps={} uploaded {} MiB, arrays {} of {} MiB, index clusters {} of {}, files {} | instances alpha={} with={} disabled={}",
                m_stats.ommGroupsLoaded, m_stats.ommGroupsWithout, m_stats.ommMicromapsTotal, m_stats.ommUploadBytesTotal >> 20u,
                m_stats.ommArrayBytesUsed >> 20u, m_limits.ommArrayPoolBytes >> 20u, m_stats.ommIndexUnitsUsed, m_limits.ommIndexUnits, m_stats.ommFilesOpen,
                m_stats.alphaInstances, m_stats.ommInstances, m_stats.ommDisabledInstances);
        }
        if (m_stats.comparisonValid) {
            const auto& c = m_stats.comparison;
            spdlog::info(
                "CLod RT compare: match={} differentTriangle={} differentCluster={} differentInstance={} rayMissed={} rasterMissed={} bothMissed={} depthClose={} farther={} nearer={} notCovered={} differentInstanceAlpha={} differentInstanceDoubleSided={}",
                c[CLOD_RT_STAT_BOTH_HIT_MATCH], c[CLOD_RT_STAT_DIFFERENT_TRIANGLE], c[CLOD_RT_STAT_DIFFERENT_CLUSTER], c[CLOD_RT_STAT_DIFFERENT_INSTANCE],
                c[CLOD_RT_STAT_RAY_MISSED_RASTER_HIT], c[CLOD_RT_STAT_RAY_HIT_RASTER_MISSED], c[CLOD_RT_STAT_BOTH_MISSED], c[CLOD_RT_STAT_DEPTH_WITHIN_TOLERANCE],
                c[CLOD_RT_STAT_RAY_HIT_FARTHER], c[CLOD_RT_STAT_RAY_HIT_NEARER], c[CLOD_RT_STAT_RASTER_NOT_COVERED],
                c[CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_ALPHA], c[CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_DOUBLE_SIDED]);
        }
    }
}

void CLodRayTracingSystem::RecordSceneBuild(org::RecordingContext& recording, const ScenePipelines& pipelines, uint32_t clustersSrv, uint32_t counterSrv, uint32_t clusterCapacity) {
    auto& commands = recording.Commands();
    const uint32_t sliceIndex = m_frame % m_framesInFlight;
    const UploadSlice& slice = m_slices[sliceIndex];
    const uint64_t sliceBase = SliceOffset(m_uploadSliceBytes, sliceIndex);
    const uint32_t firstTimer = sliceIndex * kSceneTimers;
    const auto stamp = [&](uint32_t index) {
        if (m_timers) commands.WriteTimestamp(m_timers->GetHandle(), firstTimer + index, rhi::Stage::Bottom);
    };
    stamp(0u);

    const auto clear = [&](const RawBuffer& buffer, uint64_t bytes) {
        const uint32_t words = static_cast<uint32_t>(bytes / 4u);
        MiscConstants constants{};
        constants[CLOD_RT_CLEAR_UAV] = buffer.uav;
        constants[CLOD_RT_CLEAR_WORD_COUNT] = words;
        recording.BindComputePipeline(pipelines.clear);
        PushMiscConstants(recording, constants);
        commands.Dispatch((words + kUavClearDispatchWidth - 1u) / kUavClearDispatchWidth, 1u, 1u);
    };

    MiscConstants scene{};
    scene[CLOD_RT_SCENE_CLUSTERS_SRV] = clustersSrv;
    scene[CLOD_RT_SCENE_CLUSTER_COUNTER_SRV] = counterSrv;
    scene[CLOD_RT_SCENE_CLUSTER_CAPACITY] = std::min(clusterCapacity, m_limits.maxClustersInBvh);
    scene[CLOD_RT_SCENE_REFERENCE_CAPACITY] = m_limits.maxClusterReferences;
    scene[CLOD_RT_SCENE_BLAS_CAPACITY] = m_limits.maxBlases;
    scene[CLOD_RT_SCENE_DRAW_TABLE_UAV] = m_drawTable.uav;
    scene[CLOD_RT_SCENE_DRAW_TABLE_ENTRIES] = m_limits.maxDrawRecords;
    scene[CLOD_RT_SCENE_INSTANCE_LIST_UAV] = m_instanceList.uav;
    scene[CLOD_RT_SCENE_COUNTERS_UAV] = m_counters.uav;
    scene[CLOD_RT_SCENE_INSTANCE_CAPACITY] = m_limits.maxInstances;
    scene[CLOD_RT_SCENE_BLAS_ARGS_UAV] = m_blasArgs.uav;
    scene[CLOD_RT_SCENE_ADDRESS_ARRAY_UAV] = m_blasAddressArray.uav;
    scene[CLOD_RT_SCENE_ADDRESS_ARRAY_LOW] = Low(m_blasAddressArray.address);
    scene[CLOD_RT_SCENE_ADDRESS_ARRAY_HIGH] = High(m_blasAddressArray.address);
    scene[CLOD_RT_SCENE_MAX_CLUSTERS_PER_INSTANCE] = m_limits.maxClustersPerInstance;
    scene[CLOD_RT_SCENE_GROUP_TABLE_SRV] = m_groupTable.srv;
    scene[CLOD_RT_SCENE_SEGMENT_TABLE_SRV] = m_segmentTable.srv;
    scene[CLOD_RT_SCENE_CLAS_ADDRESSES_UAV] = m_clasAddresses.uav;
    scene[CLOD_RT_SCENE_COARSE_CUT_UAV] = m_coarseCut.uav;
    scene[CLOD_RT_SCENE_BLAS_ADDRESSES_SRV] = m_blasAddresses.srv;
    scene[CLOD_RT_SCENE_TLAS_INSTANCES_UAV] = m_tlasInstances.uav;
    scene[CLOD_RT_SCENE_TLAS_INSTANCE_COUNT] = m_tlasInstanceCount;
    scene[CLOD_RT_SCENE_INSTANCE_BLAS_UAV] = m_instanceBlas.uav;
    scene[CLOD_RT_SCENE_SHARE_TABLE_UAV] = m_shareTable.uav;
    scene[CLOD_RT_SCENE_SHARE_TABLE_ENTRIES] = m_limits.blasShareEntries;
    scene[CLOD_RT_SCENE_SLOT_OMM_KEYS_UAV] = m_slotOmmKeys.uav;
    scene[CLOD_RT_SCENE_BLAS_OMM_KEYS_UAV] = m_blasOmmKeys.uav;
    scene[CLOD_RT_SCENE_OMM_ACTIVE] = OpacityMicromapsActive() ? 1u : 0u;

    if (!m_tablesCleared) {
        clear(m_groupTable, m_groupTable.bytes);
        clear(m_drawTable, m_drawTable.bytes);
        clear(m_slotOmmKeys, m_slotOmmKeys.bytes);
        clear(m_counters, kCountersBytes);
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
    }

    const uint64_t measureSourcesOffset = sliceBase + uint64_t(m_limits.maxClusterBuildsPerFrame) * sizeof(BuildSource);
    const uint64_t destinationsOffset = measureSourcesOffset + uint64_t(m_limits.maxClusterBuildsPerFrame) * sizeof(BuildSource);
    const uint64_t groupUpdatesOffset = destinationsOffset + uint64_t(m_limits.maxClusterBuildsPerFrame) * 8u;
    const uint64_t countsOffset = groupUpdatesOffset + uint64_t(kMaxGroupUpdatesPerFrame) * 8u + uint64_t(kMaxSegmentUpdatesPerFrame) * sizeof(Segment);

    // Table updates. Culling this frame ran on last frame's tables, which is fine: what it selected still has its CLAS,
    // because blocks freed here aren't rebuilt until after this frame's BLASes reference only what's still current.
    if (!slice.groupUpdates.empty() || !slice.segmentUpdates.empty()) {
        BufferBarrier(commands, m_groupTable.Handle(), rhi::ResourceSyncState::All, rhi::ResourceAccessType::Common,
            rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest);
        BufferBarrier(commands, m_segmentTable.Handle(), rhi::ResourceSyncState::All, rhi::ResourceAccessType::Common,
            rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest);
        const uint64_t groupBase = groupUpdatesOffset;
        for (size_t i = 0; i < slice.groupUpdates.size(); ++i) {
            commands.CopyBufferRegion(m_groupTable.Handle(), uint64_t(slice.groupUpdates[i].index) * 8u, m_upload.Handle(), groupBase + i * 8u, 8u);
        }
        const uint64_t segmentBase = groupBase + uint64_t(kMaxGroupUpdatesPerFrame) * 8u;
        for (size_t i = 0; i < slice.segmentUpdates.size(); ++i) {
            commands.CopyBufferRegion(m_segmentTable.Handle(), uint64_t(slice.segmentUpdates[i].index) * sizeof(Segment), m_upload.Handle(), segmentBase + i * sizeof(Segment), sizeof(Segment));
        }
        BufferBarrier(commands, m_groupTable.Handle(), rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest,
            rhi::ResourceSyncState::All, rhi::ResourceAccessType::ShaderResource);
        BufferBarrier(commands, m_segmentTable.Handle(), rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest,
            rhi::ResourceSyncState::All, rhi::ResourceAccessType::ShaderResource);
    }

    // ---- Micromaps: the baked indices and arrays of what was just asked for, ahead of its CLAS
    if (m_ommEnabled) {
        RecordOpacityMicromaps(recording, slice, sliceIndex);
    }

    // ---- CLAS: measure what was just asked for, and build what has been measured
    if (slice.buildCount != 0u || slice.measureCount != 0u) {
        if (slice.buildCount != 0u) {
            BufferBarrier(commands, m_buildDestinations.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
                rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest);
            commands.CopyBufferRegion(m_buildDestinations.Handle(), 0u, m_upload.Handle(), destinationsOffset, uint64_t(slice.buildCount) * 8u);
            BufferBarrier(commands, m_buildDestinations.Handle(), rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest,
                rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
        }
        MiscConstants constants{};
        constants[CLOD_RT_BUILD_SOURCES_SRV] = m_upload.srv;
        constants[CLOD_RT_BUILD_ARGS_UAV] = m_buildArgs.uav;
        constants[CLOD_RT_BUILD_DESTINATIONS_UAV] = m_buildDestinations.uav;
        constants[CLOD_RT_BUILD_CLAS_INFO_UAV] = m_clasInfo.uav;
        constants[CLOD_RT_BUILD_CLAS_ADDRESSES_UAV] = m_clasAddresses.uav;
        constants[CLOD_RT_BUILD_SLOT_OMM_KEYS_UAV] = m_slotOmmKeys.uav;
        constants[CLOD_RT_BUILD_OMM_INDEX_POOL_LOW] = Low(m_ommIndexPool.address);
        constants[CLOD_RT_BUILD_OMM_INDEX_POOL_HIGH] = High(m_ommIndexPool.address);
        recording.BindComputePipeline(pipelines.buildClasArgs);
        const auto writeArguments = [&](size_t sourceCount, uint64_t sourcesOffset, uint32_t firstArg, bool publish) {
            if (sourceCount == 0u) return;
            constants[CLOD_RT_BUILD_SOURCE_COUNT] = static_cast<uint32_t>(sourceCount);
            constants[CLOD_RT_BUILD_SOURCE_FIRST] = static_cast<uint32_t>(sourcesOffset / sizeof(BuildSource));
            constants[CLOD_RT_BUILD_FIRST_ARG] = firstArg;
            constants[CLOD_RT_BUILD_PUBLISH] = publish ? 1u : 0u;
            PushMiscConstants(recording, constants);
            commands.Dispatch((static_cast<uint32_t>(sourceCount) + 15u) / 16u, 1u, 1u);
        };
        writeArguments(slice.sources.size(), sliceBase, 0u, true);
        writeArguments(slice.measureSources.size(), measureSourcesOffset, m_limits.maxClusterBuildsPerFrame, false);

        BufferBarrier(commands, m_buildArgs.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource);
        BufferBarrier(commands, m_buildDestinations.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
            rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource);
        if (m_ommEnabled) {
            BufferBarrier(commands, m_ommIndexPool.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
                rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource);
        }

        rhi::RayTracingClusterTriangleInputsDesc triangles{};
        triangles.vertexFormat = rhi::RayTracingClusterVertexFormat::Float32x3;
        triangles.indexFormat = rhi::RayTracingClusterIndexFormat::Uint8;
        triangles.maxClusterTriangleCount = kMaxClusterTriangles;
        triangles.maxClusterVertexCount = kMaxClusterVertices;
        triangles.maxTotalTriangleCount = kMaxClusterTriangles * m_limits.maxClusterBuildsPerFrame;
        triangles.maxTotalVertexCount = kMaxClusterVertices * m_limits.maxClusterBuildsPerFrame;
        if (m_ommEnabled) {
            triangles.opacityMicromapIndexFormat = rhi::RayTracingClusterIndexFormat::Uint32;
            triangles.maxOpacityMicromapIndicesPerCluster = kMaxClusterTriangles;
        }
        rhi::RayTracingRtasOperationDesc desc{};
        desc.inputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
        desc.inputs.allowOpacityMicromaps = m_ommEnabled;
        desc.inputs.flags = rhi::RTASBuild_PreferFastTrace;
        desc.inputs.maxAccelerationStructureCount = m_limits.maxClusterBuildsPerFrame;
        desc.inputs.triangleClusters = &triangles;
        desc.batched.scratchData = { m_buildScratch.Handle(), 0u };

        if (slice.measureCount != 0u) {
            rhi::RayTracingRtasOperationDesc measure = desc;
            measure.inputs.operationMode = rhi::RayTracingRtasOperationMode::ComputeSizes;
            measure.batched.dstSizesArray = { m_clasSizes.Handle(), 0u, uint64_t(slice.measureCount) * 8u, 8u };
            measure.batched.srcInfosArray = { m_buildArgs.Handle(), uint64_t(m_limits.maxClusterBuildsPerFrame) * m_clusterInfoStride, uint64_t(slice.measureCount) * m_clusterInfoStride, m_clusterInfoStride };
            measure.batched.srcInfosCount = { m_upload.Handle(), countsOffset + 4u };
            commands.ExecuteIndirectRtasOperations(&measure, 1u);
            BufferBarrier(commands, m_clasSizes.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::UnorderedAccess,
                rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource);
            commands.CopyBufferRegion(m_clasSizesReadback.Handle(), uint64_t(sliceIndex) * m_clasSizes.bytes, m_clasSizes.Handle(), 0u, uint64_t(slice.measureCount) * 8u);
            BufferBarrier(commands, m_clasSizes.Handle(), rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource,
                rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::UnorderedAccess);
            GlobalUavBarrier(commands); // The two operations share scratch.
        }
        if (slice.buildCount != 0u) {
            desc.inputs.operationMode = rhi::RayTracingRtasOperationMode::ExplicitDestinations;
            desc.batched.dstAddressesArray = { m_buildDestinations.Handle(), 0u, uint64_t(slice.buildCount) * 8u, 8u };
            desc.batched.srcInfosArray = { m_buildArgs.Handle(), 0u, uint64_t(slice.buildCount) * m_clusterInfoStride, m_clusterInfoStride };
            desc.batched.srcInfosCount = { m_upload.Handle(), countsOffset };
            commands.ExecuteIndirectRtasOperations(&desc, 1u);
            BufferBarrier(commands, m_clasPool.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::RaytracingAccelerationStructureWrite,
                rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::RaytracingAccelerationStructureRead);
        }
        BufferBarrier(commands, m_buildArgs.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource,
            rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
        BufferBarrier(commands, m_buildDestinations.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource,
            rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
        if (m_ommEnabled) {
            BufferBarrier(commands, m_ommIndexPool.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource,
                rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
        }
    }

    stamp(1u);

    // ---- Group selected clusters by instance
    clear(m_counters, kCountersBytes);
    clear(m_shareTable, m_shareTable.bytes);
    clear(m_blasOmmKeys, m_blasOmmKeys.bytes);
    GlobalUavBarrier(commands);

    const uint32_t clusterGroups = (std::min(scene[CLOD_RT_SCENE_CLUSTER_CAPACITY], m_listClusterEstimate) + 255u) / 256u;
    recording.BindComputePipeline(pipelines.count);
    PushMiscConstants(recording, scene);
    commands.Dispatch(clusterGroups, 1u, 1u);
    GlobalUavBarrier(commands);
    stamp(2u);

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
    stamp(3u);

    recording.BindComputePipeline(pipelines.place);
    PushMiscConstants(recording, scene);
    commands.Dispatch(clusterGroups, 1u, 1u);
    GlobalUavBarrier(commands);
    stamp(4u);

    // ---- BLASes
    BufferBarrier(commands, m_blasArgs.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource);
    BufferBarrier(commands, m_blasAddressArray.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource);
    BufferBarrier(commands, m_counters.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource);

    rhi::RayTracingClusterBottomLevelInputsDesc blasInputs{ m_limits.maxClusterReferences, m_limits.maxClustersPerInstance };
    rhi::RayTracingRtasOperationDesc blas{};
    blas.inputs.operationType = rhi::RayTracingRtasOperationType::BuildClustersBottomLevel;
    blas.inputs.flags = rhi::RTASBuild_PreferFastBuild;
    blas.inputs.maxAccelerationStructureCount = m_limits.maxBlases;
    blas.inputs.clustersBottomLevel = &blasInputs;
    blas.inputs.allowOpacityMicromaps = m_ommEnabled;
    constexpr uint64_t blasArgStride = sizeof(rhi::PackedRayTracingClusterBuildBottomLevelInfo);
    blas.batched.dstImplicitData = { m_blasResult.Handle(), 0u };
    blas.batched.scratchData = { m_blasScratch.Handle(), 0u };
    blas.batched.dstAddressesArray = { m_blasAddresses.Handle(), 0u, m_blasAddresses.bytes, 8u };
    blas.batched.dstSizesArray = { m_blasSizes.Handle(), 0u, m_blasSizes.bytes, 8u };
    blas.batched.srcInfosArray = { m_blasArgs.Handle(), 0u, m_blasArgs.bytes, blasArgStride };
    blas.batched.srcInfosCount = { m_counters.Handle(), CLOD_RT_COUNTER_BLAS_ARGS * 4u };
    commands.ExecuteIndirectRtasOperations(&blas, 1u);

    AsWriteToRead(commands, m_blasResult.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
    BufferBarrier(commands, m_blasAddresses.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::ShaderResource);
    BufferBarrier(commands, m_counters.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
    BufferBarrier(commands, m_blasArgs.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
    BufferBarrier(commands, m_blasAddressArray.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);

    stamp(5u);

    // ---- TLAS
    recording.BindComputePipeline(pipelines.writeInstances);
    PushMiscConstants(recording, scene);
    commands.Dispatch((m_tlasInstanceCount + 63u) / 64u, 1u, 1u);
    BufferBarrier(commands, m_tlasInstances.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource);
    stamp(6u);

    rhi::RayTracingGeometryDesc geometry{};
    geometry.type = rhi::RayTracingGeometryType::Instances;
    geometry.instances.instanceBuffer = { m_tlasInstances.Handle(), 0u, uint64_t(m_tlasInstanceCount) * sizeof(rhi::PackedRayTracingInstanceDesc) };
    geometry.instances.stride = sizeof(rhi::PackedRayTracingInstanceDesc);
    geometry.instances.count = m_tlasInstanceCount;
    rhi::AccelerationStructureBuildDesc build{};
    build.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
    build.destination = m_tlas->GetHandle();
    build.inputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
    build.inputs.flags = rhi::RTASBuild_PreferFastBuild; // Measured: 0.2 ms cheaper, and no slower to trace.
    build.inputs.geometries = { &geometry, 1u };
    build.scratch = { m_tlasScratch.Handle(), 0u, m_tlasScratch.bytes };
    commands.BuildAccelerationStructures(&build, 1u);
    AsWriteToRead(commands, m_tlasStorage.Handle(), rhi::ResourceSyncState::Raytracing);
    m_tlasBuilt = true;
    stamp(7u);

    BufferBarrier(commands, m_tlasInstances.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
    BufferBarrier(commands, m_blasAddresses.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::ShaderResource,
        rhi::ResourceSyncState::BuildRaytracingAccelerationStructure, rhi::ResourceAccessType::UnorderedAccess);

    // ---- What the CPU reads back next time this slice comes around
    const uint64_t readbackBase = SliceOffset(m_readbackSliceBytes, sliceIndex);
    BufferBarrier(commands, m_counters.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource);
    BufferBarrier(commands, m_requestCounter.Handle(), rhi::ResourceSyncState::All, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource);
    BufferBarrier(commands, m_requests.Handle(), rhi::ResourceSyncState::All, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource);
    commands.CopyBufferRegion(m_readback.Handle(), readbackBase, m_counters.Handle(), 0u, kCountersBytes);
    commands.CopyBufferRegion(m_readback.Handle(), readbackBase + kCountersBytes + kStatsBytes, m_requestCounter.Handle(), 0u, 4u);
    commands.CopyBufferRegion(m_readback.Handle(), readbackBase + kCountersBytes + kStatsBytes + 16u, m_requests.Handle(), 0u, m_requests.bytes);
    BufferBarrier(commands, m_requestCounter.Handle(), rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
    BufferBarrier(commands, m_requests.Handle(), rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
    clear(m_requestCounter, 4u);
    clear(m_coarseCut, m_coarseCut.bytes); // Culling starts with it empty next frame.
    if (m_timers) {
        commands.ResolveQueryData(m_timers->GetHandle(), firstTimer, kSceneTimers, m_readback.Handle(),
            readbackBase + kCountersBytes + kStatsBytes + 16u + uint64_t(m_limits.requestCapacity) * kRequestBytes);
    }
    m_sliceHasReadback[sliceIndex] = 1u;
    m_sliceHasStats[sliceIndex] = 0u;
}

void CLodRayTracingSystem::RecordOpacityMicromaps(org::RecordingContext& recording, const UploadSlice& slice, uint32_t sliceIndex) {
    if (slice.ommIndexCopies.empty()) return;
    auto& commands = recording.Commands();
    const uint64_t uploadBase = uint64_t(sliceIndex) * m_limits.ommUploadBytesPerFrame;

    // The index pool rests as a compute UAV. The CLAS section makes it readable to builds, and gives it back.
    BufferBarrier(commands, m_ommIndexPool.Handle(), rhi::ResourceSyncState::All, rhi::ResourceAccessType::Common,
        rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest);
    for (const OmmIndexCopy& copy : slice.ommIndexCopies) {
        commands.CopyBufferRegion(m_ommIndexPool.Handle(), uint64_t(copy.firstUnit) * CLOD_RT_OMM_INDEX_UNIT_BYTES, m_ommUpload.Handle(),
            uploadBase + copy.uploadOffset, uint64_t(copy.units) * CLOD_RT_OMM_INDEX_UNIT_BYTES);
    }
    BufferBarrier(commands, m_ommIndexPool.Handle(), rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopyDest,
        rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess);
    if (slice.ommArrayBuilds.empty()) return;

    // Straight from the upload heap, which needs no barrier.
    std::vector<std::vector<rhi::RayTracingOpacityMicromapHistogramEntry>> histograms(slice.ommArrayBuilds.size());
    std::vector<rhi::RayTracingOpacityMicromapArrayBuildDesc> builds(slice.ommArrayBuilds.size());
    for (size_t i = 0; i < slice.ommArrayBuilds.size(); ++i) {
        const OmmArrayBuild& array = slice.ommArrayBuilds[i];
        for (uint32_t level = 0u; level < array.levelCounts.size(); ++level) {
            if (array.levelCounts[level] != 0u) histograms[i].push_back({ array.levelCounts[level], level, rhi::RayTracingOpacityMicromapFormat::OC1_4State });
        }
        rhi::RayTracingOpacityMicromapArrayBuildDesc& build = builds[i];
        build.inputs.flags = rhi::RTASBuild_PreferFastTrace;
        build.inputs.histogram = { histograms[i].data(), static_cast<uint32_t>(histograms[i].size()) };
        build.inputBuffer = { m_ommUpload.Handle(), array.dataOffset };
        build.perOmmDescs = { m_ommUpload.Handle(), array.descsOffset, uint64_t(array.count) * sizeof(CLodCache::OpacityMicromapDesc), sizeof(CLodCache::OpacityMicromapDesc) };
        build.destination = { m_ommArrayPool.Handle(), array.destination - m_ommArrayPool.address };
        build.scratch = { m_ommScratch.Handle(), array.scratchOffset };
    }
    commands.BuildOpacityMicromapArrays(builds.data(), static_cast<uint32_t>(builds.size()));
    AsWriteToRead(commands, m_ommArrayPool.Handle(), rhi::ResourceSyncState::BuildRaytracingAccelerationStructure);
    GlobalUavBarrier(commands); // Scratch is reused next frame.
}

void CLodRayTracingSystem::RecordVisibilityBegin(org::RecordingContext& recording, const org::PipelineState& clearPipeline, bool clearStats) {
    auto& commands = recording.Commands();
    const auto clear = [&](const RawBuffer& buffer) {
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
    BufferBarrier(commands, m_visibilityStats.Handle(), rhi::ResourceSyncState::ComputeShading, rhi::ResourceAccessType::UnorderedAccess,
        rhi::ResourceSyncState::Copy, rhi::ResourceAccessType::CopySource);
    commands.CopyBufferRegion(m_readback.Handle(), SliceOffset(m_readbackSliceBytes, sliceIndex) + kCountersBytes, m_visibilityStats.Handle(), 0u, kStatsBytes);
    m_sliceHasStats[sliceIndex] = 1u;
}

void CLodRayTracingSystem::AdvanceFrame() noexcept {
    ++m_frame;
}

} // namespace br::render
