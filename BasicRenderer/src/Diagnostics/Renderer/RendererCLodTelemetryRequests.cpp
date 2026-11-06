#include <BasicRenderer/Renderer.h>
#include "OpenRenderGraph/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "Scene/ECS/RendererECSManager.h"
#include "Runtime/Frame/RendererFrameInputs.h"
#include "VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.h"
#include "Runtime/Settings/SettingsManager.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Pipeline/CLodOptions.h"
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"
#include "Scene/Objects/IndirectStateArtifacts.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>
#include "Scene/Objects/ObjectManager.h"
#include <BasicRenderer/Extensions/VirtualShadowCasterProvider.h>
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include <filesystem>
#include <fstream>

namespace {
constexpr const char* CLodVisibilityTelemetryDebugSettingName = "clodVisibilityTelemetryDebug";
constexpr const char* CLodVirtualShadowTelemetryDebugSettingName = "clodVirtualShadowTelemetryDebug";

bool ReadTruthyEnvironmentFlag(const char* name)
{
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return false;
    }

    const bool result =
        std::strcmp(value, "1") == 0 ||
        _stricmp(value, "true") == 0 ||
        _stricmp(value, "yes") == 0 ||
        _stricmp(value, "on") == 0;
    std::free(value);
    return result;
}

bool IsCLodVisibilityTelemetryEnabledByEnvironment() {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, "SARP_CLOD_VISIBILITY_TELEMETRY") != 0 || value == nullptr) {
        return false;
    }
    const bool enabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
    free(value);
    return enabled;
}

bool IsCLodVisibilityTelemetryDebugEnabled() {
	return IsCLodVisibilityTelemetryEnabledByEnvironment() ||
		SettingsManager::GetInstance().getSettingGetter<bool>(CLodVisibilityTelemetryDebugSettingName)();
}

bool IsCLodVirtualShadowTelemetryDebugEnabled()
{
    return ReadTruthyEnvironmentFlag("SARP_CLOD_VSM_TELEMETRY") ||
        SettingsManager::GetInstance().getSettingGetter<bool>(
            CLodVirtualShadowTelemetryDebugSettingName)();
}

// Clears an in-flight flag once every callback holding this has run or been
// dropped: a capture anchored to a pass that never prepares is discarded without
// calling back.
struct ReadbackInFlightRelease {
    explicit ReadbackInFlightRelease(std::shared_ptr<std::atomic_bool> inFlightFlag) : flag(std::move(inFlightFlag)) {}
    ReadbackInFlightRelease(const ReadbackInFlightRelease&) = delete;
    ReadbackInFlightRelease& operator=(const ReadbackInFlightRelease&) = delete;
    ~ReadbackInFlightRelease() { flag->store(false, std::memory_order_release); }

    std::shared_ptr<std::atomic_bool> flag;
};

// Joins one phase-1 raster path's histogram (exact clusters per bucket) with its
// indirect arguments (dispatch groups) and logs them with the bucket's flags.
struct RasterBucketAudit {
    const char* path = "";
    std::uint64_t frame = 0;
    std::vector<MaterialRasterFlags> flags;
    std::shared_ptr<ReadbackInFlightRelease> inFlight;
    std::mutex mutex;
    std::optional<std::vector<std::byte>> histogram;
    std::optional<std::vector<std::byte>> args;

    void SetHistogram(std::vector<std::byte> data) {
        std::lock_guard lock(mutex);
        histogram = std::move(data);
        TryReport();
    }

    void SetArgs(std::vector<std::byte> data) {
        std::lock_guard lock(mutex);
        args = std::move(data);
        TryReport();
    }

private:
    void TryReport() const {
        if (!histogram || !args) return;
        const auto bucketCount = (std::min)(histogram->size() / sizeof(std::uint32_t),
            args->size() / sizeof(RasterizeClustersCommand));
        std::uint32_t nonZero = 0;
        std::uint64_t totalClusters = 0, totalGroups = 0;
        std::uint64_t alphaTestClusters = 0, doubleSidedClusters = 0, skinnedClusters = 0;
        for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
            std::uint32_t clusters = 0;
            std::memcpy(&clusters, histogram->data() + bucket * sizeof(clusters), sizeof(clusters));
            RasterizeClustersCommand command{};
            std::memcpy(&command, args->data() + bucket * sizeof(command), sizeof(command));
            const std::uint64_t groups = static_cast<std::uint64_t>(command.dispatchX) *
                command.dispatchY * command.dispatchZ;
            if (clusters == 0u && groups == 0u) continue;

            const bool flagsKnown = bucket < flags.size();
            const std::uint32_t bucketFlags = flagsKnown ? static_cast<std::uint32_t>(flags[bucket]) : 0u;
            const bool alphaTest = (bucketFlags & MaterialRasterFlagsAlphaTest) != 0u;
            const bool doubleSided = (bucketFlags & MaterialRasterFlagsDoubleSided) != 0u;
            const bool displacement = (bucketFlags & MaterialRasterFlagsGeometricDisplacement) != 0u;
            const bool skinned = (bucketFlags & MaterialRasterFlagsSkinned) != 0u;
            ++nonZero;
            totalClusters += clusters;
            totalGroups += groups;
            if (alphaTest) alphaTestClusters += clusters;
            if (doubleSided) doubleSidedClusters += clusters;
            if (skinned) skinnedClusters += clusters;
            spdlog::info(
                "SARP CLOD phase-1 raster bucket: frame={} path={} bucket={} clusters={} groups={} "
                "flags=0x{:x} flags_known={} alpha_test={} double_sided={} displacement={} skinned={}",
                frame, path, bucket, clusters, groups,
                bucketFlags, flagsKnown ? 1 : 0, alphaTest ? 1 : 0, doubleSided ? 1 : 0, displacement ? 1 : 0, skinned ? 1 : 0);
        }
        spdlog::info(
            "SARP CLOD phase-1 raster buckets: frame={} path={} buckets={} nonzero={} clusters={} groups={} "
            "alpha_test_clusters={} double_sided_clusters={} skinned_clusters={}",
            frame, path, bucketCount, nonZero, totalClusters, totalGroups,
            alphaTestClusters, doubleSidedClusters, skinnedClusters);
    }
};

// The immutable CPU images a visible cluster's mesh and LOD are resolved through.
// Draw records are not used for identity: the static import path leaves their
// expectedMeshIdentity fields zero.
struct VisibleClusterTables {
    const std::vector<std::byte>& drawRecords;
    const std::vector<std::byte>& clodOffsets;
    const std::vector<std::byte>& meshMetadata;
    const std::vector<std::byte>& groups;
};

// Attributes phase-1 visible clusters to meshes. Hardware records fill the buffer
// from the start; software records fill it from the end (index = capacity - 1 - i).
void LogVisibleClusterAttribution(std::uint64_t frame, const std::vector<std::byte>& records, std::uint32_t hwReported,
    std::uint32_t swReported, std::uint32_t capacity, const VisibleClusterTables& tables) {
    constexpr std::size_t recordBytes = sizeof(std::uint32_t) * 4u;
    constexpr std::size_t kMeshesToLog = 30u;
    constexpr std::uint32_t kVoxelFlag = 0x80000000u;

    const auto available = static_cast<std::uint32_t>((std::min<std::size_t>)(records.size() / recordBytes, UINT32_MAX));
    const std::uint32_t recordCapacity = capacity != 0u ? (std::min)(capacity, available) : available;
    const std::uint32_t hwCount = (std::min)(hwReported, recordCapacity);
    const std::uint32_t swCount = (std::min)(swReported, recordCapacity - hwCount);
    const std::size_t drawRecordCount = tables.drawRecords.size() / sizeof(InstanceDrawRecordCB);
    const std::size_t clodOffsetCount = tables.clodOffsets.size() / sizeof(MeshInstanceClodOffsets);
    const std::size_t metadataCount = tables.meshMetadata.size() / sizeof(CLodMeshMetadata);
    const std::size_t groupCount = tables.groups.size() / sizeof(ClusterLODGroup);

    struct MeshRow {
        std::uint32_t metadataIndex = 0;
        CLodMeshMetadata metadata{};
        std::uint32_t sw = 0, hw = 0, instances = 0;
        std::uint64_t depthSum = 0, depthSamples = 0, atDepth0 = 0;
        std::uint32_t depthMin = UINT32_MAX, depthMax = 0;
        CLodMeshSource source;
        bool hasSource = false;
    };
    std::vector<MeshRow> rows;
    std::unordered_map<std::uint32_t, std::uint32_t> rowByMetadataIndex;
    constexpr std::int32_t kUnresolved = -1, kInvalid = -2;
    std::vector<std::int32_t> rowByDrawRecord(drawRecordCount, kUnresolved);
    std::uint64_t voxelSkipped = 0, invalidRecords = 0, hwAttributed = 0, swAttributed = 0;

    const auto resolveRow = [&](std::uint32_t drawRecordIndex) -> std::int32_t {
        InstanceDrawRecordCB drawRecord{};
        std::memcpy(&drawRecord, tables.drawRecords.data() + drawRecordIndex * sizeof(InstanceDrawRecordCB), sizeof(drawRecord));
        if (drawRecord.clodOffsetIndex >= clodOffsetCount) return kInvalid;
        MeshInstanceClodOffsets offsets{};
        std::memcpy(&offsets, tables.clodOffsets.data() + drawRecord.clodOffsetIndex * sizeof(MeshInstanceClodOffsets), sizeof(offsets));
        if (offsets.clodMeshMetadataIndex >= metadataCount) return kInvalid;
        const auto [found, inserted] = rowByMetadataIndex.try_emplace(
            offsets.clodMeshMetadataIndex, static_cast<std::uint32_t>(rows.size()));
        if (inserted) {
            auto& row = rows.emplace_back();
            row.metadataIndex = offsets.clodMeshMetadataIndex;
            std::memcpy(&row.metadata, tables.meshMetadata.data() + offsets.clodMeshMetadataIndex * sizeof(CLodMeshMetadata), sizeof(CLodMeshMetadata));
            const std::uint64_t identity = (static_cast<std::uint64_t>(row.metadata.meshIdentityHi) << 32u) | row.metadata.meshIdentityLo;
            std::lock_guard lock(g_clodMeshSources.mutex);
            if (const auto known = g_clodMeshSources.meshes.find(identity); known != g_clodMeshSources.meshes.end()) {
                row.source = known->second;
                row.hasSource = true;
            }
        }
        return static_cast<std::int32_t>(found->second);
    };

    const auto visit = [&](std::uint32_t recordIndex, bool software) {
        std::array<std::uint32_t, 4> words{};
        std::memcpy(words.data(), records.data() + recordIndex * recordBytes, recordBytes);
        if ((words[3] & kVoxelFlag) != 0u) {
            ++voxelSkipped;
            return;
        }
        const std::uint32_t drawRecordIndex = (words[0] >> 8u) & 0xFFFFFFu;
        if (drawRecordIndex >= drawRecordCount) {
            ++invalidRecords;
            return;
        }
        auto& rowIndex = rowByDrawRecord[drawRecordIndex];
        const bool firstClusterOfInstance = rowIndex == kUnresolved;
        if (firstClusterOfInstance) rowIndex = resolveRow(drawRecordIndex);
        if (rowIndex == kInvalid) {
            ++invalidRecords;
            return;
        }
        auto& row = rows[static_cast<std::size_t>(rowIndex)];
        if (firstClusterOfInstance) ++row.instances;
        if (software) {
            ++row.sw;
            ++swAttributed;
        } else {
            ++row.hw;
            ++hwAttributed;
        }
        // The group id is local to the mesh; its depth is 0 at the finest level.
        const std::uint32_t localGroup = ((words[1] >> 14u) & 0x3FFFFu) | ((words[2] & 0x3u) << 18u);
        const std::size_t globalGroup = static_cast<std::size_t>(row.metadata.groupsBase) + localGroup;
        if (localGroup < row.metadata.groupChunkTableCount && globalGroup < groupCount) {
            std::int32_t storedDepth = 0;
            std::memcpy(&storedDepth,
                tables.groups.data() + globalGroup * sizeof(ClusterLODGroup) + offsetof(ClusterLODGroup, depth),
                sizeof(storedDepth));
            const auto depth = static_cast<std::uint32_t>((std::max)(storedDepth, 0));
            row.depthSum += depth;
            ++row.depthSamples;
            if (depth == 0u) ++row.atDepth0;
            row.depthMin = (std::min)(row.depthMin, depth);
            row.depthMax = (std::max)(row.depthMax, depth);
        }
    };
    for (std::uint32_t i = 0; i < hwCount; ++i) visit(i, false);
    for (std::uint32_t i = 0; i < swCount; ++i) visit(recordCapacity - 1u - i, true);

    const auto bySoftwareClusters = [](const MeshRow& a, const MeshRow& b) {
        return a.sw != b.sw ? a.sw > b.sw : a.hw > b.hw;
    };
    const std::size_t rowsToLog = (std::min)(rows.size(), kMeshesToLog);
    std::partial_sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(rowsToLog), rows.end(), bySoftwareClusters);

    spdlog::info(
        "SARP CLOD cluster attribution summary: frame={} hw_reported={} sw_reported={} capacity={} hw_clusters={} sw_clusters={} "
        "voxel_skipped={} invalid_records={} meshes={}",
        frame, hwReported, swReported, recordCapacity, hwAttributed, swAttributed, voxelSkipped, invalidRecords, rows.size());
    for (std::size_t rank = 0; rank < rowsToLog; ++rank) {
        const auto& row = rows[rank];
        const int alpha = row.hasSource ? ((row.source.rasterFlags & MaterialRasterFlagsAlphaTest) != 0u ? 1 : 0) : -1;
        const int doubleSided = row.hasSource ? ((row.source.rasterFlags & MaterialRasterFlagsDoubleSided) != 0u ? 1 : 0) : -1;
        const bool hasDepth = row.depthSamples != 0u;
        spdlog::info(
            "SARP CLOD cluster attribution: frame={} rank={} mesh={} name=\"{}\" sw_clusters={} hw_clusters={} instances={} "
            "alpha={} double_sided={} groups={} lod_levels={} max_depth={} selected_depth_min={} selected_depth_avg={:.2f} "
            "selected_depth_max={} selected_at_depth0={}",
            frame, rank, row.metadataIndex, row.source.name.empty() ? "unregistered" : row.source.name.c_str(),
            row.sw, row.hw, row.instances, alpha, doubleSided,
            row.metadata.groupChunkTableCount, row.metadata.lodLevelCount, row.metadata.maxDepth,
            hasDepth ? static_cast<std::int64_t>(row.depthMin) : -1,
            hasDepth ? static_cast<double>(row.depthSum) / static_cast<double>(row.depthSamples) : -1.0,
            hasDepth ? static_cast<std::int64_t>(row.depthMax) : -1,
            row.atDepth0);
    }
}

}

void Renderer::MaybeRequestCLodVisibilityTelemetry() {
    if (!currentRenderGraph) {
        return;
    }

    if (!IsCLodVisibilityTelemetryDebugEnabled()) {
        if (m_clodVisibilityTelemetryDebugEnabledByRenderer) {
            SetCLodWorkGraphTelemetryRequested(CLodWorkGraphTelemetryRequester::VisibilityDebug, false);
            m_clodVisibilityTelemetryDebugEnabledByRenderer = false;
            m_loggedCLodVisibilityTelemetryEnabled = false;
        }
        return;
    }

    SetCLodWorkGraphTelemetryRequested(CLodWorkGraphTelemetryRequester::VisibilityDebug, true);
    m_clodVisibilityTelemetryDebugEnabledByRenderer = true;

    if (!m_loggedCLodVisibilityTelemetryEnabled) {
        spdlog::info(
            "SARP CLOD visibility telemetry debug enabled (setting '{}' or SARP_CLOD_VISIBILITY_TELEMETRY).",
            CLodVisibilityTelemetryDebugSettingName);
        m_loggedCLodVisibilityTelemetryEnabled = true;
    }

    constexpr uint64_t kCaptureIntervalFrames = 30;
    if (m_lastCLodVisibilityTelemetryRequestFrame != UINT64_MAX &&
        m_totalFramesRendered - m_lastCLodVisibilityTelemetryRequestFrame < kCaptureIntervalFrames) {
        return;
    }
    if (m_clodTelemetryReadbackPending ||
        m_clodRasterArgsReadbackPending ||
        m_clodVisibleCounterReadbackPending ||
        m_clodVisibleRecordsReadbackPending ||
        m_clodReplayStateReadbackPending) {
        return;
    }

    auto* readbackService = m_orgRuntime->Readbacks().get();
    if (!readbackService) {
        return;
    }

    auto& world = RendererECSManager::GetInstance().GetWorld();
    const auto visibilityTag = world.component<CLodExtensionVisibilityBufferTag>();

    std::shared_ptr<org::Resource> telemetryResource;
    world.query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!telemetryResource) {
                telemetryResource = component.resource.lock();
            }
        });

    std::shared_ptr<org::Resource> visibleCounterResource;
    world.query_builder<const Components::Resource>()
        .with<VisibleClustersCounterTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!visibleCounterResource) {
                visibleCounterResource = component.resource.lock();
            }
        });

    std::shared_ptr<org::Resource> visibleRecordsResource;
    world.query_builder<const Components::Resource>()
        .with<VisibleClustersBufferTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!visibleRecordsResource) visibleRecordsResource = component.resource.lock();
        });

    std::shared_ptr<org::Resource> rasterArgsResource;
    world.query_builder<const Components::Resource>()
        .with<CLodPrimaryPhase1RasterIndirectArgsTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!rasterArgsResource) {
                rasterArgsResource = component.resource.lock();
            }
        });

    std::shared_ptr<org::Resource> replayStateResource;
    world.query_builder<const Components::Resource>()
        .with<CLodOcclusionReplayStateBufferTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!replayStateResource) {
                replayStateResource = component.resource.lock();
            }
        });

    if (!telemetryResource || !rasterArgsResource || !visibleCounterResource ||
        !visibleRecordsResource || !replayStateResource) {
        return;
    }

    const auto firstResource = [](auto query) {
        std::shared_ptr<org::Resource> found;
        query.each([&](const Components::Resource& component) {
            if (!found) found = component.resource.lock();
        });
        return found;
    };
    const auto hwHistogramResource = firstResource(world.query_builder<const Components::Resource>()
        .with<CLodPrimaryPhase1RasterHistogramTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build());
    const auto swHistogramResource = firstResource(world.query_builder<const Components::Resource>()
        .with<CLodPrimaryPhase1SwRasterHistogramTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build());
    const auto swArgsResource = firstResource(world.query_builder<const Components::Resource>()
        .with<CLodPrimaryPhase1SwRasterIndirectArgsTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build());

    const auto swVisibleCounterResource = firstResource(world.query_builder<const Components::Resource>()
        .with<CLodPrimaryPhase1SwVisibleClustersCounterTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build());
    std::uint32_t visibleClusterCapacity = 0u;
    world.query_builder<const CLodVisibleClusterCapacity>()
        .with<VisibleClustersBufferTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const CLodVisibleClusterCapacity& capacity) {
            visibleClusterCapacity = capacity.maxVisibleClusters;
        });

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastCLodVisibilityTelemetryRequestFrame = requestedFrame;
    m_clodTelemetryReadbackPending = true;
    m_clodRasterArgsReadbackPending = true;
    m_clodVisibleCounterReadbackPending = true;
    m_clodVisibleRecordsReadbackPending = true;
    m_clodReplayStateReadbackPending = true;

    uint32_t primaryActiveSetWorkloads = 0u;
    uint32_t primaryActiveSetMembers = 0u;
    struct VisibleRecordAudit {
        std::mutex mutex;
        std::optional<std::uint32_t> count;
        std::optional<std::uint32_t> swCount;
        std::optional<std::vector<std::byte>> indices;
        std::shared_ptr<const std::vector<std::byte>> drawRecords;
        std::shared_ptr<const br::render::PublishedGpuBufferVersion> clodOffsetsVersion;
        std::shared_ptr<const br::render::PublishedGpuBufferVersion> meshMetadataVersion;
        std::shared_ptr<const br::render::PublishedGpuBufferVersion> groupsVersion;
        std::uint32_t capacity = 0;
        bool attributed = false;
        std::unordered_set<std::uint32_t> allowed;
        std::uint32_t residentDrawRecordCount = 0;
        std::uint64_t drawRecordsRevision = 0;
        std::uint64_t indirectRevision = 0;
        bool reported = false;

        void TryReport(std::uint64_t frame) {
            std::lock_guard lock(mutex);
            if (!attributed && count && swCount && indices && drawRecords) {
                attributed = true;
                const auto clodOffsets = clodOffsetsVersion ? clodOffsetsVersion->MaterializeCpuImage() : nullptr;
                const auto meshMetadata = meshMetadataVersion ? meshMetadataVersion->MaterializeCpuImage() : nullptr;
                const auto groups = groupsVersion ? groupsVersion->MaterializeCpuImage() : nullptr;
                if (clodOffsets && meshMetadata && groups) {
                    LogVisibleClusterAttribution(frame, *indices, *count, *swCount, capacity,
                        VisibleClusterTables{*drawRecords, *clodOffsets, *meshMetadata, *groups});
                } else {
                    spdlog::warn("SARP CLOD cluster attribution: frame={} geometry CPU images unavailable (offsets={} metadata={} groups={}).",
                        frame, clodOffsets != nullptr, meshMetadata != nullptr, groups != nullptr);
                }
            }
            if (reported || !count || !indices) return;
            constexpr std::size_t recordBytes = sizeof(std::uint32_t) * 4u;
            const auto available = static_cast<std::uint32_t>(indices->size() / recordBytes);
            const auto used = (std::min)(*count, available);
            std::uint32_t matched = 0, unpublished = 0, outOfRange = 0;
            for (std::uint32_t i = 0; i < used; ++i) {
                std::uint32_t packedIdentity = 0;
                std::memcpy(&packedIdentity, indices->data() + i * recordBytes, sizeof(packedIdentity));
                const auto drawRecordIndex = (packedIdentity >> 8u) & 0xFFFFFFu;
                if (drawRecordIndex >= residentDrawRecordCount) ++outOfRange;
                if (allowed.contains(drawRecordIndex)) ++matched; else ++unpublished;
            }
            reported = true;
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.VisibleRecordsMatched", matched);
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.VisibleRecordsUnpublished", unpublished);
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.VisibleRecordsOutOfRange", outOfRange);
            spdlog::info(
                "SARP CLOD visible-record audit: frame={} draw_revision={} indirect_revision={} "
                "visible={} available={} allowed={} matched={} unpublished={} out_of_range={}",
                frame, drawRecordsRevision, indirectRevision, *count, available, allowed.size(),
                matched, unpublished, outOfRange);
        }
    };
    auto visibleTransformAudit = std::make_shared<VisibleRecordAudit>();
    visibleTransformAudit->capacity = visibleClusterCapacity;
    if (const auto published = m_context.publishedRendererState) {
        visibleTransformAudit->drawRecordsRevision = published->drawRecords.revision;
        visibleTransformAudit->indirectRevision = published->indirectWorkloads.revision;
        const auto objects = published->drawRecords.payload.Get<br::render::PublishedObjectBufferState>();
        const auto indirect = published->indirectWorkloads.payload.Get<br::render::PublishedIndirectState>();
        const auto drawVersion = objects ? objects->FindVersion(br::render::kObjectDrawRecordVariant) : nullptr;
        const auto generationVersion = objects ? objects->FindVersion(br::render::kObjectVisibilityGenerationVariant) : nullptr;
        visibleTransformAudit->residentDrawRecordCount = drawVersion
            ? static_cast<std::uint32_t>((std::min<std::uint64_t>)(drawVersion->elementCount, UINT32_MAX)) : 0u;
        const auto drawBytes = drawVersion ? drawVersion->MaterializeCpuImage() : nullptr;
        visibleTransformAudit->drawRecords = drawBytes;
        if (const auto geometry = published->geometry.selectedState.Get<br::render::PublishedGeometryBufferState>()) {
            // Catalog variants from MeshManager's geometry buffer definitions.
            constexpr std::uint64_t kClodOffsetsVariant = 3u, kMeshMetadataVariant = 5u, kGroupsVariant = 7u;
            for (std::size_t i = 0; i < geometry->buffers.size() && i < geometry->versions.size(); ++i) {
                const auto variant = geometry->buffers[i].catalogVariant;
                if (variant == kClodOffsetsVariant) visibleTransformAudit->clodOffsetsVersion = geometry->versions[i];
                else if (variant == kMeshMetadataVariant) visibleTransformAudit->meshMetadataVersion = geometry->versions[i];
                else if (variant == kGroupsVariant) visibleTransformAudit->groupsVersion = geometry->versions[i];
            }
        }
        const auto generationBytes = generationVersion ? generationVersion->MaterializeCpuImage() : nullptr;
        if (drawBytes && generationBytes && indirect) {
            for (const auto& active : indirect->activeListVersions) {
                const auto activeBytes = active.version ? active.version->MaterializeCpuImage() : nullptr;
                if (!activeBytes) continue;
                const auto activeByteCount = (std::min<std::size_t>)(activeBytes->size(),
                    static_cast<std::size_t>(active.version->elementCount) * sizeof(br::render::ActiveDrawEntryDTO));
                for (std::size_t offset = 0; offset + sizeof(br::render::ActiveDrawEntryDTO) <= activeByteCount;
                    offset += sizeof(br::render::ActiveDrawEntryDTO)) {
                    br::render::ActiveDrawEntryDTO entry{};
                    std::memcpy(&entry, activeBytes->data() + offset, sizeof(entry));
                    const auto generationOffset = static_cast<std::size_t>(entry.drawRecordIndex) * sizeof(std::uint32_t);
                    const auto drawOffset = static_cast<std::size_t>(entry.drawRecordIndex) * sizeof(InstanceDrawRecordCB);
                    if (generationOffset + sizeof(std::uint32_t) > generationBytes->size() ||
                        drawOffset + sizeof(InstanceDrawRecordCB) > drawBytes->size()) continue;
                    std::uint32_t generation = 0;
                    std::memcpy(&generation, generationBytes->data() + generationOffset, sizeof(generation));
                    if (generation != entry.generation) continue;
                    visibleTransformAudit->allowed.insert(entry.drawRecordIndex);
                }
            }
        }
    }
    if (auto* objectManager = m_pObjectManager.get()) {
        auto activeStats = objectManager->SnapshotActiveDrawSetDebugStats();
        std::uint64_t totalSpan = 0;
        std::uint64_t totalLive = 0;
        std::uint64_t totalTombstoneEstimate = 0;
        std::uint64_t totalCpuMatches = 0;
        std::uint64_t totalCpuStale = 0;
        std::uint64_t totalCpuOutOfRange = 0;
        for (const auto& row : activeStats) {
            totalSpan += row.span;
            totalLive += row.liveSize;
            totalTombstoneEstimate += row.tombstoneEstimate;
            totalCpuMatches += row.cpuGenerationMatches;
            totalCpuStale += row.cpuGenerationStale;
            totalCpuOutOfRange += row.cpuGenerationOutOfRange;
        }
        primaryActiveSetWorkloads = static_cast<uint32_t>(activeStats.size());
        primaryActiveSetMembers = static_cast<uint32_t>((std::min<std::uint64_t>)(totalLive, UINT32_MAX));
        spdlog::info(
            "SARP CLOD active-set CPU telemetry: frame={} workloads={} span={} live={} tombstone_est={} cpu_match={} cpu_stale={} cpu_oob={}",
            requestedFrame,
            activeStats.size(),
            totalSpan,
            totalLive,
            totalTombstoneEstimate,
            totalCpuMatches,
            totalCpuStale,
            totalCpuOutOfRange);

        const std::size_t rowsToLog = (std::min<std::size_t>)(activeStats.size(), 6u);
        for (std::size_t i = 0; i < rowsToLog; ++i) {
            const auto& row = activeStats[i];
            const auto bad = row.cpuGenerationStale + row.cpuGenerationOutOfRange;
            if (i >= 3u && bad == 0u) {
                break;
            }
            spdlog::info(
                "SARP CLOD active-set CPU workload[{}]: frame={} flags={} phase={} clodOnly={} span={} live={} tombstone_est={} cpu_match={} cpu_stale={} cpu_oob={}",
                i,
                requestedFrame,
                static_cast<std::uint64_t>(row.workloadKey.compileFlags),
                row.workloadKey.renderPhase.hash,
                row.workloadKey.clodOnly ? 1 : 0,
                row.span,
                row.liveSize,
                row.tombstoneEstimate,
                row.cpuGenerationMatches,
                row.cpuGenerationStale,
                row.cpuGenerationOutOfRange);
        }
    }

    readbackService->RequestReadbackCapture(
        "CLodOpaque::RasterizeClustersPass2",
        telemetryResource.get(),
        org::RangeSpec{},
        [this, requestedFrame, primaryActiveSetWorkloads, primaryActiveSetMembers](org::ReadbackCaptureResult&& result) {
            m_clodTelemetryReadbackPending = false;

            constexpr size_t telemetryBytes = sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
            if (result.data.size() < telemetryBytes) {
                spdlog::warn(
                    "SARP CLOD visibility telemetry: frame={} work-graph payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            CLodWorkGraphTelemetryCounters decoded{};
            std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);
            auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                return decoded.counters[static_cast<size_t>(idx)];
            };
            const auto publishCounter = [&](std::string_view name,
                CLodWorkGraphCounterIndex index) {
                basic_telemetry::SetGauge(name,
                    static_cast<std::int64_t>(counter(index)));
            };
            auto distributionCounter = [&](uint32_t depthBin, uint32_t footprintBin) -> uint32_t {
                constexpr uint32_t footprintBinCount = 6u;
                const auto index = static_cast<size_t>(CLodWorkGraphCounterIndex::VoxelRasterDistributionBinBase) +
                    depthBin * footprintBinCount + footprintBin;
                return decoded.counters[index];
            };

            const uint32_t traversalLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords);
            const uint32_t nonresidentLeaves = counter(CLodWorkGraphCounterIndex::SegmentEvaluateNonResidentRefinedChildThreads);
            PublishCLodTelemetrySnapshot(g_clodPrimaryVisibility, CLodPrimaryVisibilitySnapshot{
                .frame = requestedFrame,
                .traversalLeaves = traversalLeaves,
                .errorRejectedLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesRejectedByErrorRecords),
                .residentLeaves = traversalLeaves > nonresidentLeaves ? traversalLeaves - nonresidentLeaves : 0u,
                .nonresidentLeaves = nonresidentLeaves,
                .visibleClusterWrites = counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites),
                .bucketRecordsDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched),
                .histogramInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs),
                .histogramTriangleContributors = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors),
                .compactionInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs),
                .compactionTriangleEmitted = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted),
                .rasterInitializationFailures = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed),
                .sourceGroupMismatches = counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch),
                .outputTriangles = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles),
                .activeSetWorkloads = primaryActiveSetWorkloads,
                .activeSetMembers = primaryActiveSetMembers,
                .depthTileOccupancyAvailable = false,
            });

            // Keep the GPU readback boundaries in structured telemetry.  These
            // counters are specifically intended to distinguish missing scene
            // admission from traversal, bucket, mesh-shader, and pixel-stage
            // collapses in automated runs; the log-only diagnostic was too easy
            // to lose when the host exits immediately after benchmarking.
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Frame",
                static_cast<std::int64_t>(requestedFrame));
            publishCounter("BasicRenderer.CLod.Readback.ObjectCullInRange",
                CLodWorkGraphCounterIndex::ObjectCullInRangeThreads);
            publishCounter("BasicRenderer.CLod.Readback.ObjectCullVisible",
                CLodWorkGraphCounterIndex::ObjectCullVisibleThreads);
            publishCounter("BasicRenderer.CLod.Readback.TraversalLeaves",
                CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords);
            publishCounter("BasicRenderer.CLod.Readback.TraversalEmitted",
                CLodWorkGraphCounterIndex::TraverseNodesTraverseRecordsEmitted);
            publishCounter("BasicRenderer.CLod.Readback.ClusterVisibleWrites",
                CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites);
            publishCounter("BasicRenderer.CLod.Readback.BucketRecords",
                CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
            publishCounter("BasicRenderer.CLod.Readback.HistogramInputs",
                CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
            publishCounter("BasicRenderer.CLod.Readback.CompactionInputs",
                CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
            publishCounter("BasicRenderer.CLod.Readback.CompactionTriangles",
                CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
            publishCounter("BasicRenderer.CLod.Readback.RasterArgsNonZeroBuckets",
                CLodWorkGraphCounterIndex::RasterArgsNonZeroBuckets);
            publishCounter("BasicRenderer.CLod.Readback.RasterArgsDispatchGroups",
                CLodWorkGraphCounterIndex::RasterArgsDispatchGroups);
            publishCounter("BasicRenderer.CLod.Readback.RasterGroups",
                CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
            publishCounter("BasicRenderer.CLod.Readback.RasterInRange",
                CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
            publishCounter("BasicRenderer.CLod.Readback.RasterInitFailures",
                CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
            publishCounter("BasicRenderer.CLod.Readback.RasterSourceGroupMismatches",
                CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch);
            publishCounter("BasicRenderer.CLod.Readback.RasterOutputTriangles",
                CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
            publishCounter("BasicRenderer.CLod.Readback.PixelInvocations",
                CLodWorkGraphCounterIndex::RasterPixelShaderInvocations);
            publishCounter("BasicRenderer.CLod.Readback.PixelVisibilityWrites",
                CLodWorkGraphCounterIndex::RasterPixelVisibilityWrites);
            publishCounter("BasicRenderer.CLod.Readback.PixelScissorRejected",
                CLodWorkGraphCounterIndex::RasterPixelScissorRejected);
            publishCounter("BasicRenderer.CLod.Readback.PixelTargetBoundsRejected",
                CLodWorkGraphCounterIndex::RasterPixelTargetBoundsRejected);

            const auto compactedTriangles = counter(
                CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
            const auto argumentDispatchGroups = counter(
                CLodWorkGraphCounterIndex::RasterArgsDispatchGroups);
            const auto rasterGroups = counter(
                CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
            const auto outputTriangles = counter(
                CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
            const auto pixelInvocations = counter(
                CLodWorkGraphCounterIndex::RasterPixelShaderInvocations);
            const auto visibilityWrites = counter(
                CLodWorkGraphCounterIndex::RasterPixelVisibilityWrites);
            const bool collapsed = CLodRasterPipelineCollapsed(compactedTriangles,
                argumentDispatchGroups, rasterGroups, outputTriangles,
                pixelInvocations, visibilityWrites);
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.RasterPipelineCollapsed",
                collapsed ? 1 : 0);
            if (collapsed) {
                basic_telemetry::AddCounter(
                    "BasicRenderer.CLod.Readback.RasterPipelineCollapseSamples");
            }

			spdlog::info(
				"SARP CLOD visibility telemetry: frame={} object(in_range={} visible={} total={} rejected_stale_generation={} rejected_frustum={} rejected_occlusion={} replay_rejected_occlusion={} invalid_bounds={}) traverse(internal={} leaf={} culled={} rejected_error={} active_children={} emitted={} child_frustum={} child_lod={}) stream(request_attempts={} range_rejects={} resident_hits={} request_appends={}) cluster(in_range={} visible_writes={} total={} rejected_frustum={} rejected_condition2={} rejected_occlusion={} rejected_out_of_range={} zero_survivor_waves={} nonresident_leaf={} emit_bucket={}) voxel_object(candidates={} frustum_reject={} visible={} traverse={} root_internal={} root_leaf={}) voxel(leaves={} rejected_error={} desc_hits={} desc_misses={} raster_work={} raster_dropped={}) voxel_raster(groups={} rigid={} skinned={} cube_candidates={} skin_bone_groups={} invalid_cluster={} desc_miss={} invalid_payload={} bad_width={} proj_reject={} scissor_reject={} depth_reject={} dda_miss={} vis_writes={} vis_wins={} vis_losses={} projected_px={} queued_px={} queue_overflow={} nonpos_depth={}) raster(groups={} in_range={} init_failed={} source_group_mismatch={} zero_tri_outputs={} out_tris={}) sort(compact_inputs={} voxel_skipped={} reyes_skipped={} compact_tris={})",
				requestedFrame,
				counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads),
				counter(CLodWorkGraphCounterIndex::ObjectCullVisibleThreads),
				counter(CLodWorkGraphCounterIndex::ObjectCullThreads),
				counter(CLodWorkGraphCounterIndex::ObjectCullRejectedStaleGeneration),
				counter(CLodWorkGraphCounterIndex::ObjectCullRejectedFrustum),
				counter(CLodWorkGraphCounterIndex::ObjectCullRejectedOcclusion),
				counter(CLodWorkGraphCounterIndex::ObjectReplayRejectedOcclusion),
				counter(CLodWorkGraphCounterIndex::ObjectCullInvalidBounds),
				counter(CLodWorkGraphCounterIndex::TraverseNodesInternalNodeRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesCulledNodeRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesRejectedByErrorRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads),
				counter(CLodWorkGraphCounterIndex::TraverseNodesTraverseRecordsEmitted),
				counter(CLodWorkGraphCounterIndex::ChildPrefilterFrustumCulled),
				counter(CLodWorkGraphCounterIndex::ChildPrefilterLodRejected),
				counter(CLodWorkGraphCounterIndex::StreamRequestAttempts),
				counter(CLodWorkGraphCounterIndex::StreamRequestRangeRejects),
				counter(CLodWorkGraphCounterIndex::StreamResidentHits),
				counter(CLodWorkGraphCounterIndex::StreamRequestAppends),
				counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads),
                counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites),
                counter(CLodWorkGraphCounterIndex::ClusterCullThreads),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedFrustum),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCondition2),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOcclusion),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOutOfRange),
                counter(CLodWorkGraphCounterIndex::ClusterCullZeroSurvivorWaves),
                counter(CLodWorkGraphCounterIndex::SegmentEvaluateNonResidentRefinedChildThreads),
                counter(CLodWorkGraphCounterIndex::SegmentEvaluateEmitBucketThreads),
                counter(CLodWorkGraphCounterIndex::VoxelObjectCandidates),
                counter(CLodWorkGraphCounterIndex::VoxelObjectFrustumRejected),
                counter(CLodWorkGraphCounterIndex::VoxelObjectVisible),
                counter(CLodWorkGraphCounterIndex::VoxelObjectTraverseRecords),
                counter(CLodWorkGraphCounterIndex::VoxelRootInternalRecords),
                counter(CLodWorkGraphCounterIndex::VoxelRootLeafRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped),
                counter(CLodWorkGraphCounterIndex::VoxelRasterWorkGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterRigidWorkGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterSkinnedWorkGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterPreparedCubeCandidates),
                counter(CLodWorkGraphCounterIndex::VoxelRasterSkinBoneGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterInvalidCluster),
                counter(CLodWorkGraphCounterIndex::VoxelRasterSegmentPageMisses),
                counter(CLodWorkGraphCounterIndex::VoxelRasterInvalidPackedCluster),
                counter(CLodWorkGraphCounterIndex::VoxelRasterInvalidVoxelWidth),
                counter(CLodWorkGraphCounterIndex::VoxelRasterProjectionRejected),
                counter(CLodWorkGraphCounterIndex::VoxelRasterScissorRejected),
                counter(CLodWorkGraphCounterIndex::VoxelRasterDepthRejected),
                counter(CLodWorkGraphCounterIndex::VoxelRasterDdaMisses),
                counter(CLodWorkGraphCounterIndex::VoxelRasterVisibilityWrites),
                counter(CLodWorkGraphCounterIndex::VoxelRasterVisibilityWins),
                counter(CLodWorkGraphCounterIndex::VoxelRasterVisibilityLosses),
                counter(CLodWorkGraphCounterIndex::VoxelRasterProjectedPixels),
                counter(CLodWorkGraphCounterIndex::VoxelRasterQueuedPixels),
                counter(CLodWorkGraphCounterIndex::VoxelRasterQueueOverflow),
                counter(CLodWorkGraphCounterIndex::VoxelRasterNonPositiveDepth),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderZeroTriangleOutputs),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionReyesSkipped),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted));

            for (uint32_t depthBin = 0u; depthBin < 8u; ++depthBin) {
                constexpr std::array<uint32_t, 9> depthEdges = {
                    0u, 4096u, 8192u, 16384u, 32768u, 65536u, 131072u, 262144u, UINT32_MAX
                };
                spdlog::info(
                    "SARP CLOD voxel distribution: frame={} depth_bin={} depth_range=[{},{}) occupied_voxels_by_projected_px(<0.5,0.5-1,1-2,2-4,4-8,>=8)=[{},{},{},{},{},{}]",
                    requestedFrame,
                    depthBin,
                    depthEdges[depthBin],
                    depthEdges[depthBin + 1u],
                    distributionCounter(depthBin, 0u),
                    distributionCounter(depthBin, 1u),
                    distributionCounter(depthBin, 2u),
                    distributionCounter(depthBin, 3u),
                    distributionCounter(depthBin, 4u),
                    distributionCounter(depthBin, 5u));
            }
            spdlog::info(
                "SARP CLOD assembly traversal telemetry: frame={} instance_roots={} part_instance_roots={} assembly_part(traverse={} voxel_leaves={} voxel_raster={} triangle_buckets={}) assembly_voxel(leaves={} rejected_error={} suppressed_by_child={} nonresident={} raster_work={})",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::AssemblyInstanceRootRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartInstanceRootRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartTraversalRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartVoxelRasterWorkRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartTriangleBucketRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelRejectedByErrorRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelSuppressedByChildRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelNonResidentRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelRasterWorkRecords));
			spdlog::info(
				"SARP CLOD animated node bounds: frame={} explicit_evaluations={} explicit_frustum_rejections={} overflow_fallbacks={} assembly_fallbacks={} invalid_data_fallbacks={}",
				requestedFrame,
				counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitEvaluations),
				counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitFrustumRejected),
				counter(CLodWorkGraphCounterIndex::NodeBoundsOverflowFallbacks),
				counter(CLodWorkGraphCounterIndex::NodeBoundsAssemblyFallbacks),
				counter(CLodWorkGraphCounterIndex::NodeBoundsInvalidFallbacks));
            spdlog::info(
                "SARP CLOD traversal divergence: frame={} waves={} active_lanes={} avg_active={:.2f} node_type_waves(internal_only={} leaf_only={} mixed={}) skinning(lanes_rigid={} lanes_skinned={} waves_rigid_only={} waves_skinned_only={} waves_mixed={}) child_loops(nodes={} slots={} emitted={} avg_slots={:.2f} survival={:.3f}) explicit_bounds(total_bones={} avg_bones={:.2f} hist_1={} hist_2={} hist_3_4={} hist_5_8={} hist_9_plus={})",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::TraverseWaves),
                counter(CLodWorkGraphCounterIndex::TraverseActiveLanes),
                counter(CLodWorkGraphCounterIndex::TraverseWaves) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseActiveLanes)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseWaves))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::TraverseInternalOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseLeafOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseMixedNodeTypeWaves),
                counter(CLodWorkGraphCounterIndex::TraverseRigidLanes),
                counter(CLodWorkGraphCounterIndex::TraverseSkinnedLanes),
                counter(CLodWorkGraphCounterIndex::TraverseRigidOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseSkinnedOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseMixedSkinningWaves),
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopNodes),
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots),
                counter(CLodWorkGraphCounterIndex::TraverseChildRecordsEmitted),
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopNodes) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildLoopNodes))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildRecordsEmitted)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitEvaluations) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitEvaluations))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount1),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount2),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount3To4),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount5To8),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount9Plus));
			spdlog::info(
				"SARP CLOD animated meshlet bounds: frame={} live_evaluations={} invalid_slot_fallbacks={} no_valid_bone_fallbacks={} fallback_frustum_rejections={}",
				requestedFrame,
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedLiveEvaluations),
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedInvalidSlotFallbacks),
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedNoValidBoneFallbacks),
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedFallbackFrustumRejected));
            spdlog::info(
                "SARP CLOD HW raster telemetry: frame={} out_tris={} pixel_invocations={} visibility_writes={}",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles),
                counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations),
                counter(CLodWorkGraphCounterIndex::RasterPixelVisibilityWrites));
            spdlog::info(
                "SARP CLOD SW raster telemetry: frame={} clusters={} input_tris={} backface_culled={} degenerate_or_offscreen={} "
                "no_sample_covered={} covered_px={} alpha_rejected_px={} vis_writes={}",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::SWRasterClusters),
                counter(CLodWorkGraphCounterIndex::SWRasterInputTriangles),
                counter(CLodWorkGraphCounterIndex::SWRasterBackfaceCulled),
                counter(CLodWorkGraphCounterIndex::SWRasterDegenerateOrOffscreen),
                counter(CLodWorkGraphCounterIndex::SWRasterNoSampleCovered),
                counter(CLodWorkGraphCounterIndex::SWRasterCoveredPixels),
                counter(CLodWorkGraphCounterIndex::SWRasterAlphaRejectedPixels),
                counter(CLodWorkGraphCounterIndex::SWRasterVisibilityWrites));
            spdlog::info(
                "SARP CLOD occlusion replay telemetry: frame={} node_enqueue_attempts={} cluster_enqueue_attempts={} "
                "phase2_node_launches={} phase2_node_inputs={} phase2_node_emitted={} "
                "phase2_meshlet_launches={} phase2_meshlet_inputs={} phase2_meshlet_emitted={}",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionNodeReplayEnqueueAttempts),
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionClusterReplayEnqueueAttempts),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeRecordsEmitted),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletBucketRecordsEmitted));
        });

    std::vector<MaterialRasterFlags> rasterBucketFlags;
    if (m_frameInputs && m_frameInputs->Render()) {
        rasterBucketFlags = m_frameInputs->Render()->preparedRasterBucketFlags;
    }
    auto hwRasterBuckets = std::make_shared<RasterBucketAudit>();
    hwRasterBuckets->path = "hw";
    hwRasterBuckets->frame = requestedFrame;
    hwRasterBuckets->flags = rasterBucketFlags;
    if (hwHistogramResource) {
        readbackService->RequestReadbackCapture(
            "CLodOpaque::RasterizeClustersPass1",
            hwHistogramResource.get(),
            org::RangeSpec{},
            [hwRasterBuckets](org::ReadbackCaptureResult&& result) {
                hwRasterBuckets->SetHistogram(std::move(result.data));
            });
    }

    // The software pass exists only in compute software-raster mode. Its captures
    // stay outside the pending flags above: if the graph has no such pass they are
    // dropped without a callback, which must not stall the rest of this telemetry.
    static const auto swRasterBucketsInFlight = std::make_shared<std::atomic_bool>(false);
    if (swHistogramResource && swArgsResource &&
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance()
            .getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()) &&
        !swRasterBucketsInFlight->exchange(true, std::memory_order_acq_rel)) {
        auto swRasterBuckets = std::make_shared<RasterBucketAudit>();
        swRasterBuckets->path = "sw";
        swRasterBuckets->frame = requestedFrame;
        swRasterBuckets->flags = std::move(rasterBucketFlags);
        swRasterBuckets->inFlight = std::make_shared<ReadbackInFlightRelease>(swRasterBucketsInFlight);
        readbackService->RequestReadbackCapture(
            "CLodOpaque::RasterBucketsCompactAndArgsPassSW1",
            swHistogramResource.get(),
            org::RangeSpec{},
            [swRasterBuckets](org::ReadbackCaptureResult&& result) {
                swRasterBuckets->SetHistogram(std::move(result.data));
            });
        readbackService->RequestReadbackCapture(
            "CLodOpaque::RasterBucketsCompactAndArgsPassSW1",
            swArgsResource.get(),
            org::RangeSpec{},
            [swRasterBuckets](org::ReadbackCaptureResult&& result) {
                swRasterBuckets->SetArgs(std::move(result.data));
            });
    }

    readbackService->RequestReadbackCapture(
        "CLodOpaque::RasterizeClustersPass1",
        rasterArgsResource.get(),
        org::RangeSpec{},
        [this, requestedFrame, hwRasterBuckets](org::ReadbackCaptureResult&& result) {
            m_clodRasterArgsReadbackPending = false;

            const size_t commandCount = result.data.size() / sizeof(RasterizeClustersCommand);
            uint64_t dispatchGroups = 0u;
            uint32_t nonZeroCommands = 0u;
            RasterizeClustersCommand firstNonZero{};
            bool foundFirst = false;
            for (size_t i = 0; i < commandCount; ++i) {
                RasterizeClustersCommand command{};
                std::memcpy(&command,
                    result.data.data() + i * sizeof(RasterizeClustersCommand),
                    sizeof(command));
                const uint64_t groups = static_cast<uint64_t>(command.dispatchX) *
                    command.dispatchY * command.dispatchZ;
                if (groups == 0u) {
                    continue;
                }
                ++nonZeroCommands;
                dispatchGroups += groups;
                if (!foundFirst) {
                    firstNonZero = command;
                    foundFirst = true;
                }
            }

            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferBytes",
                static_cast<std::int64_t>(result.data.size()));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferCommands",
                static_cast<std::int64_t>(commandCount));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferNonZeroCommands",
                static_cast<std::int64_t>(nonZeroCommands));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferDispatchGroups",
                static_cast<std::int64_t>(dispatchGroups));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferFirstBase",
                static_cast<std::int64_t>(firstNonZero.baseClusterOffset));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferFirstXDim",
                static_cast<std::int64_t>(firstNonZero.xDim));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferFirstBucket",
                static_cast<std::int64_t>(firstNonZero.rasterBucketID));

            spdlog::info(
                "SARP CLOD phase-1 raster-args readback: frame={} bytes={} commands={} nonzero={} groups={} "
                "first(base={} xdim={} bucket={} dispatch={},{},{})",
                requestedFrame,
                result.data.size(),
                commandCount,
                nonZeroCommands,
                dispatchGroups,
                firstNonZero.baseClusterOffset,
                firstNonZero.xDim,
                firstNonZero.rasterBucketID,
                firstNonZero.dispatchX,
                firstNonZero.dispatchY,
                firstNonZero.dispatchZ);
            hwRasterBuckets->SetArgs(std::move(result.data));
        });

    if (swVisibleCounterResource) {
        readbackService->RequestReadbackCapture(
            "CLodOpaque::HierarchicalCullingPass2",
            swVisibleCounterResource.get(),
            org::RangeSpec{},
            [requestedFrame, visibleTransformAudit](org::ReadbackCaptureResult&& result) {
                std::uint32_t swVisibleClusters = 0;
                if (result.data.size() >= sizeof(swVisibleClusters)) {
                    std::memcpy(&swVisibleClusters, result.data.data(), sizeof(swVisibleClusters));
                }
                {
                    std::lock_guard lock(visibleTransformAudit->mutex);
                    visibleTransformAudit->swCount = swVisibleClusters;
                }
                visibleTransformAudit->TryReport(requestedFrame);
            });
    } else {
        visibleTransformAudit->swCount = 0u;
    }

    readbackService->RequestReadbackCapture(
        "CLodOpaque::HierarchicalCullingPass2",
        visibleCounterResource.get(),
        org::RangeSpec{},
        [this, requestedFrame, visibleTransformAudit](org::ReadbackCaptureResult&& result) {
            m_clodVisibleCounterReadbackPending = false;

            if (result.data.size() < sizeof(uint32_t)) {
                spdlog::warn(
                    "SARP CLOD visibility telemetry: frame={} visible-counter payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            uint32_t visibleClusters = 0;
            std::memcpy(&visibleClusters, result.data.data(), sizeof(uint32_t));
            {
                std::lock_guard lock(visibleTransformAudit->mutex);
                visibleTransformAudit->count = visibleClusters;
            }
            visibleTransformAudit->TryReport(requestedFrame);
            spdlog::info(
                "SARP CLOD visibility counter: frame={} visible_clusters={}",
                requestedFrame,
                visibleClusters);
        });

    readbackService->RequestReadbackCapture(
        "CLodOpaque::HierarchicalCullingPass2",
        visibleRecordsResource.get(),
        org::RangeSpec{},
        [this, requestedFrame, visibleTransformAudit](org::ReadbackCaptureResult&& result) {
            m_clodVisibleRecordsReadbackPending = false;
            {
                std::lock_guard lock(visibleTransformAudit->mutex);
                visibleTransformAudit->indices = std::move(result.data);
            }
            visibleTransformAudit->TryReport(requestedFrame);
        });

    readbackService->RequestReadbackCapture(
        "CLodOpaque::HierarchicalCullingPass2",
        replayStateResource.get(),
        org::RangeSpec{},
        [this, requestedFrame](org::ReadbackCaptureResult&& result) {
            m_clodReplayStateReadbackPending = false;

            if (result.data.size() < sizeof(CLodReplayBufferState)) {
                spdlog::warn(
                    "SARP CLOD replay-state telemetry: frame={} payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            CLodReplayBufferState state{};
            std::memcpy(&state, result.data.data(), sizeof(state));
            spdlog::info(
                "SARP CLOD replay-state telemetry: frame={} node_writes={} node_dropped={} "
                "meshlet_writes={} meshlet_dropped={} reyes_split_writes={} reyes_split_dropped={} "
                "reyes_dice_writes={} reyes_dice_dropped={}",
                requestedFrame,
                state.nodeWriteCount,
                state.nodeDropped,
                state.meshletWriteCount,
                state.meshletDropped,
                state.reyesSplitWriteCount,
                state.reyesSplitDropped,
                state.reyesDiceWriteCount,
                state.reyesDiceDropped);
        });

}

void Renderer::MaybeRequestCLodVirtualShadowTelemetry()
{
    if (!currentRenderGraph || !IsCLodVirtualShadowTelemetryDebugEnabled()) {
        SetCLodWorkGraphTelemetryRequested(CLodWorkGraphTelemetryRequester::VirtualShadowDebug, false);
        m_loggedCLodVirtualShadowTelemetryEnabled = false;
        return;
    }

    if (!m_loggedCLodVirtualShadowTelemetryEnabled) {
        spdlog::info(
            "CLOD VSM telemetry enabled (setting '{}' or SARP_CLOD_VSM_TELEMETRY).",
            CLodVirtualShadowTelemetryDebugSettingName);
        m_loggedCLodVirtualShadowTelemetryEnabled = true;
    }
    SetCLodWorkGraphTelemetryRequested(CLodWorkGraphTelemetryRequester::VirtualShadowDebug, true);

    constexpr uint64_t kCaptureIntervalFrames = 30u;
    // Startup and resize can replace the aliased shadow resources after the
    // ECS query has found them but before the readback pass is compiled.
    // Wait for one full capture interval so the graph/resource generation is
    // stable before arming the first capture.
    if (m_totalFramesRendered < kCaptureIntervalFrames) {
        return;
    }
    if (m_lastCLodVirtualShadowTelemetryRequestFrame != UINT64_MAX &&
        m_totalFramesRendered - m_lastCLodVirtualShadowTelemetryRequestFrame < kCaptureIntervalFrames) {
        return;
    }
    if (m_clodVirtualShadowTelemetryReadbackPending ||
        m_clodVirtualShadowWorkTelemetryReadbackPending ||
        m_virtualShadowCasterTelemetryReadbacksPending != 0u) {
        return;
    }

    auto* readbackService = m_orgRuntime->Readbacks().get();
    if (!readbackService) {
        return;
    }
    auto& world = RendererECSManager::GetInstance().GetWorld();
    const auto shadowTag = world.component<CLodExtensionShadowTag>();
    std::shared_ptr<org::Resource> statsResource;
    std::shared_ptr<org::Resource> workTelemetryResource;
    world.query_builder<const Components::Resource>()
        .with<CLodVirtualShadowStatsTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!statsResource) {
                statsResource = component.resource.lock();
            }
        });
    if (!statsResource) {
        return;
    }
    world.query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!workTelemetryResource) {
                workTelemetryResource = component.resource.lock();
            }
        });

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastCLodVirtualShadowTelemetryRequestFrame = requestedFrame;
    m_clodVirtualShadowTelemetryReadbackPending = true;
    readbackService->RequestReadbackCapture(
        "DeferredShadingPass",
        statsResource.get(),
        org::RangeSpec{},
        [this, requestedFrame](org::ReadbackCaptureResult&& result) {
            m_clodVirtualShadowTelemetryReadbackPending = false;
            if (result.data.size() < sizeof(CLodVirtualShadowStats)) {
                spdlog::error(
                    "CLOD VSM telemetry frame={}: payload too small ({} < {}).",
                    requestedFrame,
                    result.data.size(),
                    sizeof(CLodVirtualShadowStats));
                return;
            }

            CLodVirtualShadowStats stats{};
            std::memcpy(&stats, result.data.data(), sizeof(stats));
            const bool normalBudgetValid =
                stats.configuredPageRenderBudget == 0u ||
                stats.normalAdmittedPageCount <= stats.configuredPageRenderBudget;
            const bool upgradeBudgetValid =
                stats.configuredUpgradePageRenderBudget == 0u ||
                stats.upgradeAdmittedPageCount <= stats.configuredUpgradePageRenderBudget;
            const bool renderedWithinAdmission =
                stats.normalRenderedPageCount + stats.upgradeRenderedPageCount <= stats.admittedPageCount;
            const uint32_t admittedNotRendered =
                stats.admittedPageCount > stats.normalRenderedPageCount + stats.upgradeRenderedPageCount
                ? stats.admittedPageCount - stats.normalRenderedPageCount - stats.upgradeRenderedPageCount
                : 0u;
            const bool admittedPagesCleared =
                stats.physicalPageClearCount == stats.admittedPageCount;
            const auto sumClipmapCounters = [](const auto& counters) {
                uint32_t total = 0u;
                for (uint32_t value : counters) {
                    total += value;
                }
                return total;
            };

            spdlog::info(
                "CLOD VSM budget frame={}: totalBudget={} upgradeBudget={} eligible(normal={},upgrade={}) admitted(total={},normal={},upgrade={}) deferred(normal={},upgrade={}) rendered(normal={},upgrade={}) admittedNotRendered={} valid(total={},upgrade={},rendered={})",
                requestedFrame,
                stats.configuredPageRenderBudget,
                stats.configuredUpgradePageRenderBudget,
                stats.normalEligiblePageCount,
                stats.upgradeEligiblePageCount,
                stats.admittedPageCount,
                stats.normalAdmittedPageCount,
                stats.upgradeAdmittedPageCount,
                stats.normalDeferredPageCount,
                stats.upgradeDeferredPageCount,
                stats.normalRenderedPageCount,
                stats.upgradeRenderedPageCount,
                admittedNotRendered,
                normalBudgetValid,
                upgradeBudgetValid,
                renderedWithinAdmission);
            spdlog::info(
                "CLOD VSM exact recovery frame={}: fallbackCandidates={} candidateOverflow={} finalizedRaw={} rawOverflow={} finalizedDependencies={} dedupOverflow={} captureRetries={} upgradeInputs(accepted={},rejected={},pageTouches={}) upgradePages(eligible={},admitted={},deferred={},rendered={}) cumulative(inputs={},touches={},admitted={},rendered={})",
                requestedFrame,
                stats.upgradeCandidateInputCount,
                stats.upgradeCandidateAppendOverflowCount,
                stats.upgradeRawPageCount,
                stats.upgradeRawPageOverflowCount,
                stats.readyUpgradePageCount,
                stats.readyUpgradePageOverflowCount,
                stats.invalidUpgradeDependencyCount,
                stats.upgradeInvalidationInputCount,
                stats.upgradeInvalidationRejectedInputCount,
                stats.upgradeInvalidationAllocatedPageTouchCount,
                stats.upgradeEligiblePageCount,
                stats.upgradeAdmittedPageCount,
                stats.upgradeDeferredPageCount,
                stats.upgradeRenderedPageCount,
                stats.cumulativeUpgradeInvalidationInputCount,
                stats.cumulativeUpgradeInvalidationAllocatedPageTouchCount,
                stats.cumulativeUpgradePageAdmittedCount,
                stats.cumulativeUpgradePageRenderedCount);
            spdlog::info(
                "CLOD VSM ownership frame={}: pool(free={},reusable={},allocationRequests={}) pageTableMismatches={} contentValidMismatches={} residentTagMismatches={} renderedWithoutMatchingClear={} syntheticEmptyValid={} newlyAllocated={} staticClears={} dynamicClears={} composed={} admitted={} clearAdmissionInvariant={}",
                requestedFrame,
                stats.freePhysicalPageCount,
                stats.reusablePhysicalPageCount,
                stats.allocationRequestCount,
                stats.pageTableOwnerMismatchCount,
                stats.contentValidOwnerMismatchCount,
                stats.markResidentTagMismatchCount,
                stats.renderedWithoutMatchingClearCount,
                stats.syntheticEmptyValidPageCount,
                stats.newlyAllocatedPageCount,
                stats.physicalPageClearCount,
                stats.dynamicPageClearCount,
                stats.composedPageCount,
                stats.admittedPageCount,
                admittedPagesCleared);
            constexpr uint64_t physicalPageTexelCount =
                static_cast<uint64_t>(CLodVirtualShadowPhysicalPageSize) *
                static_cast<uint64_t>(CLodVirtualShadowPhysicalPageSize);
            const uint64_t staticQueuedTexels =
                static_cast<uint64_t>(stats.physicalPageClearCount) * physicalPageTexelCount;
            const uint64_t dynamicQueuedTexels =
                static_cast<uint64_t>(stats.dynamicPageClearCount) * physicalPageTexelCount;
            CLodVirtualShadowPageAttributionSnapshot pageAttribution{
                .frame = requestedFrame,
                .pageSize = CLodVirtualShadowPhysicalPageSize,
                .staticQueuedPages = stats.physicalPageClearCount,
                .dynamicQueuedPages = stats.dynamicPageClearCount,
                .composedPages = stats.composedPageCount,
                .admittedPages = stats.admittedPageCount,
            };
            std::copy_n(stats.selectedPixels, pageAttribution.selectedPixels.size(), pageAttribution.selectedPixels.begin());
            std::copy_n(stats.requestedPages, pageAttribution.requestedPages.size(), pageAttribution.requestedPages.begin());
            std::copy_n(stats.allocatedPageTableEntries, pageAttribution.allocatedPages.size(), pageAttribution.allocatedPages.begin());
            std::copy_n(stats.visitedPageTableEntries, pageAttribution.visitedPages.size(), pageAttribution.visitedPages.begin());
            PublishCLodTelemetrySnapshot(g_clodVsmPageAttribution, pageAttribution);
            spdlog::info(
                "CLOD VSM page area frame={}: pageSize={} staticQueued(pages={},texels={}) dynamicQueued(pages={},texels={}) totalQueued(pages={},texels={}) composed(pages={},texels={})",
                requestedFrame,
                CLodVirtualShadowPhysicalPageSize,
                stats.physicalPageClearCount,
                staticQueuedTexels,
                stats.dynamicPageClearCount,
                dynamicQueuedTexels,
                stats.physicalPageClearCount + stats.dynamicPageClearCount,
                staticQueuedTexels + dynamicQueuedTexels,
                stats.composedPageCount,
                static_cast<uint64_t>(stats.composedPageCount) * physicalPageTexelCount);
            spdlog::info(
                "CLOD VSM raster expansion frame={}: swBlocks(requested={},committed={},dropped={})",
                requestedFrame,
                stats.blockExpandedRequestedRecordCount,
                stats.blockExpandedCommittedRecordCount,
                stats.blockExpandedDroppedRecordCount);
            spdlog::info(
                "CLOD VSM dirty sources frame={}: residentDirtyHits={} dirtyPages={} visitedDirty={} predictiveInvalidated={} currentBoundsInvalidated={} previousBoundsInvalidated={}",
                requestedFrame,
                sumClipmapCounters(stats.markResidentDirtyHits),
                sumClipmapCounters(stats.dirtyPageTableEntries),
                sumClipmapCounters(stats.visitedDirtyPageTableEntries),
                sumClipmapCounters(stats.predictiveInvalidatedPageTableEntries),
                sumClipmapCounters(stats.invalidatedCurrentBoundsPageTableEntries),
                sumClipmapCounters(stats.invalidatedPreviousBoundsPageTableEntries));
            if (!normalBudgetValid || !upgradeBudgetValid || !renderedWithinAdmission ||
                !admittedPagesCleared) {
                spdlog::error(
                    "CLOD VSM budget invariant violation frame={}: totalValid={} upgradeValid={} renderedWithinAdmission={} admittedPagesCleared={}.",
                    requestedFrame,
                    normalBudgetValid,
                    upgradeBudgetValid,
                    renderedWithinAdmission,
                    admittedPagesCleared);
            }
        });

    if (workTelemetryResource) {
        m_clodVirtualShadowWorkTelemetryReadbackPending = true;
        readbackService->RequestReadbackCapture(
            "CLodShadow::RasterizeClustersPass1",
            workTelemetryResource.get(),
            org::RangeSpec{},
            [requestedFrame](org::ReadbackCaptureResult&& result) {
                constexpr size_t telemetryBytes =
                    sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    return;
                }
                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);
                const auto counter = [&](CLodWorkGraphCounterIndex index) {
                    return decoded.counters[static_cast<size_t>(index)];
                };
                PublishCLodTelemetrySnapshot(g_clodVsmHardwareAttribution, CLodVirtualShadowHardwareAttributionSnapshot{
                    .frame = requestedFrame,
                    .invocations = counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations),
                    .pageRejected = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected),
                    .writes = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites),
                });
            });
        readbackService->RequestReadbackCapture(
            // Compose consumes the completed static and dynamic shadow layers,
            // so it is the first stable cross-queue attribution point. Reading
            // at either raster pass can observe only graphics or only compute.
            "CLodShadow::VirtualShadowComposePagesPass",
            workTelemetryResource.get(),
            org::RangeSpec{},
            [this, requestedFrame](org::ReadbackCaptureResult&& result) {
                m_clodVirtualShadowWorkTelemetryReadbackPending = false;
                constexpr size_t telemetryBytes =
                    sizeof(uint32_t) *
                    static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    spdlog::warn(
                        "CLOD VSM work telemetry frame={}: payload too small.",
                        requestedFrame);
                    return;
                }
                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(
                    decoded.counters.data(),
                    result.data.data(),
                    telemetryBytes);
                PublishCLodTelemetrySnapshot(g_clodVsmWorkAttribution, CLodVirtualShadowWorkAttributionSnapshot{
                    .frame = requestedFrame,
                    .counters = decoded,
                });
                const auto counter = [&](CLodWorkGraphCounterIndex index) {
                    return decoded.counters[static_cast<size_t>(index)];
                };
                spdlog::info(
                "CLOD VSM work frame={}: object(total={},inRange={},visible={},staleRejected={},frustumRejected={},occlusionRejected={},emitted={}) traverse(internal={},leaf={},culled={},emitted={}) cluster(visibleWrites={},skinnedBounds={},dirtyQueries={},dirtyHits={},cleanRejected={}) sort(inputs={},emitted={}) raster(groups={},triangles={},pixels={},pageRejected={},writes={})",
                    requestedFrame,
                    counter(CLodWorkGraphCounterIndex::ObjectCullThreads),
                    counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads),
                    counter(CLodWorkGraphCounterIndex::ObjectCullVisibleThreads),
                    counter(CLodWorkGraphCounterIndex::ObjectCullRejectedStaleGeneration),
                    counter(CLodWorkGraphCounterIndex::ObjectCullRejectedFrustum),
                    counter(CLodWorkGraphCounterIndex::ObjectCullRejectedOcclusion),
                    counter(CLodWorkGraphCounterIndex::ObjectCullTraverseRecordsEmitted),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesInternalNodeRecords),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesCulledNodeRecords),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesTraverseRecordsEmitted),
                    counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites),
                    counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedLiveEvaluations),
                    counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyQueries),
                    counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyRegionHits),
                    counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCleanPages),
                    counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs),
                    counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted),
                    counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups),
                    counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles),
                    counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites));
                spdlog::info(
                    "CLOD VSM routing frame={}: contributing={} hardware={} software={}",
                    requestedFrame,
                    counter(CLodWorkGraphCounterIndex::ClassifyContributing),
                    counter(CLodWorkGraphCounterIndex::ClassifyRoutedHW),
                    counter(CLodWorkGraphCounterIndex::ClassifyRoutedSW));
                // The sw counters are zero unless the software raster PSOs are compiled with
                // CLOD_SW_RASTER_TELEMETRY=1 (a pso.recompile define override): they cost atomics per pixel.
                spdlog::info(
                    "CLOD VSM pixel attribution frame={}: hw(invocations={},pageRejected={},writes={}) "
                    "sw(clusters={},tris={},backface={},offscreen={},noSample={},coveredPx={},alphaRejectedPx={},writes={})",
                    requestedFrame,
                    counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites),
                    counter(CLodWorkGraphCounterIndex::SWRasterClusters),
                    counter(CLodWorkGraphCounterIndex::SWRasterInputTriangles),
                    counter(CLodWorkGraphCounterIndex::SWRasterBackfaceCulled),
                    counter(CLodWorkGraphCounterIndex::SWRasterDegenerateOrOffscreen),
                    counter(CLodWorkGraphCounterIndex::SWRasterNoSampleCovered),
                    counter(CLodWorkGraphCounterIndex::SWRasterCoveredPixels),
                    counter(CLodWorkGraphCounterIndex::SWRasterAlphaRejectedPixels),
                    counter(CLodWorkGraphCounterIndex::SWRasterVisibilityWrites));
				spdlog::info(
					"CLOD VSM DynamicWind cache frame={}: bounds(hit={},miss={},insert={},race={},probeFail={},ineligible={}) clusters(eligible={},unique={},duplicate={}) vertices(requested={},skinned={},fallback={},cachedRaster={},inlineRaster={})",
					requestedFrame,
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheHits),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheMisses),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheInsertions),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheRaces),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheProbeFailures),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheIneligible),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheEligibleClusters),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheUniqueClusters),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheDuplicateClusters),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheRequestedVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheSkinnedVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheFallbackVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheCachedRasterVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheInlineRasterVertices));
            });
    }

    struct CasterTelemetryResource {
        std::shared_ptr<org::Resource> resource;
        std::string providerId;
        std::string completionPassName;
    };
    std::vector<CasterTelemetryResource> casterTelemetryResources;
    world.query_builder<const Components::Resource, const VirtualShadowCasterTelemetryTag>()
        .build()
        .each([&](const Components::Resource& component, const VirtualShadowCasterTelemetryTag& tag) {
            if (auto resource = component.resource.lock()) {
                casterTelemetryResources.push_back({
                    std::move(resource), tag.providerId, tag.completionPassName });
            }
        });
    m_virtualShadowCasterTelemetryReadbacksPending =
        static_cast<uint32_t>(casterTelemetryResources.size());
    for (auto& telemetry : casterTelemetryResources) {
        readbackService->RequestReadbackCapture(
            telemetry.completionPassName,
            telemetry.resource.get(),
            org::RangeSpec{},
            [this, requestedFrame, providerId = telemetry.providerId](org::ReadbackCaptureResult&& result) {
                if (m_virtualShadowCasterTelemetryReadbacksPending != 0u) {
                    --m_virtualShadowCasterTelemetryReadbacksPending;
                }
                constexpr size_t counterCount = 8u;
                if (result.data.size() < sizeof(uint32_t) * counterCount) {
                    spdlog::warn(
                        "VSM caster telemetry provider={} frame={}: payload too small ({} bytes).",
                        providerId, requestedFrame, result.data.size());
                    return;
                }
                std::array<uint32_t, counterCount> counters{};
                std::memcpy(counters.data(), result.data.data(), sizeof(counters));
                spdlog::info(
                    "VSM caster telemetry provider={} frame={}: records={} candidates={} activeBlockOverlaps={} staticRecords={} dynamicRecords={} depthWrites={} capacityOverflows={}",
                    providerId, requestedFrame, counters[0], counters[1], counters[2],
                    counters[3], counters[4], counters[5], counters[6]);
            });
    }
}

// SARP_CLOD_VSM_FRAME_TELEMETRY=<csv>: every frame's VSM page counters and the
// primary deferred pass's receiver lookup outcomes, one row per frame. Flicker is
// a frame-to-frame effect, so this samples every frame; the capture rides the
// deferred pass's own command list (it binds CLodStats), adding no graph pass.
class CLodVirtualShadowFrameTelemetryState {
public:
    std::mutex mutex;
    std::ofstream output;
    std::weak_ptr<org::Resource> statsResource;
    std::atomic<uint32_t> inFlight{0u};
};

void Renderer::MaybeRequestCLodVirtualShadowFrameTelemetry()
{
    const std::string& path = br::runtime::renderer_settings::CLodVirtualShadowFrameTelemetryPath();
    if (path.empty() || !currentRenderGraph || !m_orgRuntime) {
        return;
    }
    auto* readbackService = m_orgRuntime->Readbacks().get();
    if (!readbackService) {
        return;
    }
    if (!m_clodVirtualShadowFrameTelemetry) {
        auto state = std::make_shared<CLodVirtualShadowFrameTelemetryState>();
        std::error_code error;
        const std::filesystem::path outputPath(path);
        if (outputPath.has_parent_path()) {
            std::filesystem::create_directories(outputPath.parent_path(), error);
        }
        state->output.open(outputPath, std::ios::out | std::ios::trunc);
        if (!state->output) {
            spdlog::error("CLOD VSM frame telemetry: cannot open '{}'.", path);
            return;
        }
        state->output <<
            "frame,cam_x,cam_y,cam_z,"
            "reset,reset_forced,reset_no_previous,reset_structure,reset_light_direction,"
            "allocation_requests,free_pages,reusable_pages,newly_allocated,mark_overflow,"
            "allocation_pct,pressure_bias,"
            "normal_eligible,normal_admitted,normal_deferred,upgrade_eligible,upgrade_admitted,upgrade_deferred,"
            "requested_pages,visited_pages,dirty_pages,stale_dirty_cleared,wrapped_cleared,"
            "recv_lookup,recv_preferred,recv_fallback1,recv_fallback2,recv_none,"
            "recv_pref_unallocated,recv_pref_not_visited,recv_pref_no_content,recv_pref_tag_mismatch,"
            "recv_pref_depth_missing,recv_texel_cleared,recv_shadowed,"
            "hist_valid,flip,hard_flip,flip_clip_changed,flip_same_page,flip_page_changed,hard_flip_same_page,soft_only_flip,flip_to_lit,"
            "probe_packed,probe_page,probe_prev_packed,probe_prev_page,flip_page_changed_by_clip\n";
        m_clodVirtualShadowFrameTelemetry = std::move(state);
        spdlog::info("CLOD VSM frame telemetry writing '{}'.", path);
    }
    auto state = m_clodVirtualShadowFrameTelemetry;
    // Startup and resize replace the aliased shadow resources before the
    // readback pass exists; wait for the graph to settle as the 30-frame log does.
    if (m_totalFramesRendered < 30u) {
        return;
    }
    // Readbacks complete a few frames late; bound the backlog if the GPU stalls.
    if (state->inFlight.load(std::memory_order_acquire) >= 16u) {
        return;
    }
    auto statsResource = state->statsResource.lock();
    if (!statsResource) {
        auto& world = RendererECSManager::GetInstance().GetWorld();
        const auto shadowTag = world.component<CLodExtensionShadowTag>();
        world.query_builder<const Components::Resource>()
            .with<CLodVirtualShadowStatsTag>()
            .with<CLodExtensionTypeTag>(shadowTag)
            .build()
            .each([&](const Components::Resource& component) {
                if (!statsResource) {
                    statsResource = component.resource.lock();
                }
            });
        if (!statsResource) {
            return;
        }
        state->statsResource = statsResource;
    }

    DirectX::XMFLOAT4 camera{};
    if (m_frameInputs) {
        for (const auto& view : m_frameInputs->Update()->Views()) {
            if (view.primary) {
                camera = view.cameraInfo.positionWorldSpace;
                break;
            }
        }
    }
    // The graph frame number, which the setup log and the deferred pass's
    // history ping-pong also use.
    const uint64_t requestedFrame = m_frameInputs ? m_frameInputs->FrameNumber() : m_totalFramesRendered;
    state->inFlight.fetch_add(1u, std::memory_order_acq_rel);
    // A dropped capture never calls back; the guard returns its in-flight slot.
    auto release = std::shared_ptr<void>(nullptr, [state](void*) {
        state->inFlight.fetch_sub(1u, std::memory_order_acq_rel);
    });
    readbackService->RequestReadbackCapture(
        "DeferredShadingPass",
        statsResource.get(),
        org::RangeSpec{},
        [state, release, requestedFrame, camera](org::ReadbackCaptureResult&& result) {
            if (result.data.size() < sizeof(CLodVirtualShadowStats)) {
                return;
            }
            CLodVirtualShadowStats stats{};
            std::memcpy(&stats, result.data.data(), sizeof(stats));
            const auto sum = [](const auto& counters) {
                uint64_t total = 0u;
                for (uint32_t value : counters) {
                    total += value;
                }
                return total;
            };
            std::scoped_lock lock(state->mutex);
            auto& o = state->output;
            o << requestedFrame << ',' << camera.x << ',' << camera.y << ',' << camera.z << ','
              << stats.setupResetApplied << ',' << stats.setupResetForced << ','
              << stats.setupResetNoPreviousState << ',' << stats.setupResetStructureMismatch << ','
              << stats.setupResetLightDirectionChanged << ','
              << stats.allocationRequestCount << ',' << stats.freePhysicalPageCount << ','
              << stats.reusablePhysicalPageCount << ',' << stats.newlyAllocatedPageCount << ','
              << stats.markRequestOverflowCount << ','
              << stats.currentAllocationPercentage << ',' << stats.smoothedPressureLodBias << ','
              << stats.normalEligiblePageCount << ',' << stats.normalAdmittedPageCount << ','
              << stats.normalDeferredPageCount << ',' << stats.upgradeEligiblePageCount << ','
              << stats.upgradeAdmittedPageCount << ',' << stats.upgradeDeferredPageCount << ','
              << sum(stats.requestedPages) << ',' << sum(stats.visitedPageTableEntries) << ','
              << sum(stats.dirtyPageTableEntries) << ',' << sum(stats.setupStaleDirtyClearedPageTableEntries) << ','
              << sum(stats.setupWrappedClearedPageTableEntries) << ','
              << stats.receiverLookupPixels << ',' << stats.receiverPreferredSampledPixels << ','
              << stats.receiverFallbackOnePixels << ',' << stats.receiverFallbackTwoPixels << ','
              << stats.receiverNoSamplePixels << ',' << stats.receiverPreferredUnallocatedPixels << ','
              << stats.receiverPreferredNotVisitedPixels << ',' << stats.receiverPreferredNoContentPixels << ','
              << stats.receiverPreferredTagMismatchPixels << ',' << stats.receiverPreferredDepthMissingPixels << ','
              << stats.receiverTexelClearedPixels << ',' << stats.receiverShadowedPixels << ','
              << stats.receiverHistoryValidPixels << ',' << stats.receiverFlipPixels << ',' << stats.receiverHardFlipPixels << ','
              << stats.receiverFlipClipChangedPixels << ',' << stats.receiverFlipSamePagePixels << ','
              << stats.receiverFlipPageChangedPixels << ',' << stats.receiverHardFlipSamePagePixels << ','
              << stats.receiverSoftOnlyFlipPixels << ',' << stats.receiverFlipToLitPixels << ','
              << stats.receiverProbe[0] << ',' << stats.receiverProbe[1] << ','
              << stats.receiverProbe[2] << ',' << stats.receiverProbe[3] << ',';
            for (uint32_t clip = 0u; clip < CLodVirtualShadowMaxSupportedClipmapCount; ++clip) {
                o << (clip ? ";" : "") << stats.receiverFlipPageChangedByClip[clip];
            }
            o << '\n';
            o.flush();
        });
}
