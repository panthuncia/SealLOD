#include "Render/GraphExtensions/CLodExtension.h"

#include "Render/GraphExtensions/CLodAlphaVariant.h"
#include "Render/GraphExtensions/CLodExtensionShared.h"
#include "Render/GraphExtensions/CLodShadowVariant.h"
#include "Render/GraphExtensions/CLodVisibilityVariant.h"

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "Managers/MeshManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodStreamingSystem.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterRasterizationPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterPageJobBuildArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterPageJobExpandPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterPageJobRasterPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterizationPass.h"
#include "Render/GraphExtensions/ClusterLOD/HierarchicalDispatchCullingPass.h"
#include "Render/GraphExtensions/ClusterLOD/HierarchicalCullingPass.h"
#include "Render/GraphExtensions/ClusterLOD/PerViewLinearDepthCopyPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketBlockOffsetsPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketBlockScanPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketCompactAndArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketCreateCommandPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketHistogramPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesClassifyPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesBuildRasterWorkPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesRasterWorkCompactAndArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesRasterWorkHistogramPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesCopyCounterPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesCreateDispatchArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesDicePass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesVirtualShadowRasterizationPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesVirtualShadowHardwareRasterPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesQueueResetPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesReplayMergePass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesSeedPatchesPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesSplitPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesTessellationTable.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesTessellationTableUploadPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapAllocatePagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowBlockExpandPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowBuildRasterArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildPageListsPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildMarkTilesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapClearDirtyBitsPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapClearPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapFreeWrappedPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapGatherStatsPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapDirtyHierarchyPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapNonRasterableHierarchyPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapDeduplicatePredictedPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapExpandPredictedPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapInvalidatePagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapMarkPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapResolveMarkedBlocksPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapSetupPass.h"
#include "Render/GraphExtensions/ClusterLOD/VoxelSoftwareRasterizationPass.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/RenderPhase.h"
#include "RenderPasses/FidelityFX/Downsample.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Buffers/PagePool.h"
#include "Resources/components.h"
#include "ShaderBuffers.h"
#include "Utilities/ProportionalBudgetAllocator.h"
#include "BuiltinResources.h"

namespace {

CLodTransparencyMode GetTransparencyMode(CLodExtensionType type)
{
    if (type != CLodExtensionType::AlphaBlend) {
        return CLodTransparencyMode::LinkedListDeepVisibility;
    }

    return SettingsManager::GetInstance().getSettingGetter<CLodTransparencyMode>(CLodTransparencyModeSettingName)();
}

HierarchicalCullingWorkGraphMode GetCullingWorkGraphMode(CLodSoftwareRasterMode softwareRasterMode)
{
    switch (softwareRasterMode) {
    case CLodSoftwareRasterMode::Disabled:
        return HierarchicalCullingWorkGraphMode::HardwareOnly;
    case CLodSoftwareRasterMode::Compute:
        return HierarchicalCullingWorkGraphMode::SoftwareRasterCompute;
    case CLodSoftwareRasterMode::WorkGraph:
        return HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph;
    }

    return HierarchicalCullingWorkGraphMode::HardwareOnly;
}

HierarchicalCullingBackend GetHierarchicalCullingBackend(CLodCullingBackend backend)
{
    switch (backend) {
    case CLodCullingBackend::PureCompute:
        return HierarchicalCullingBackend::PureCompute;
    case CLodCullingBackend::WorkGraph:
    default:
        return HierarchicalCullingBackend::WorkGraph;
    }
}

bool UseHierarchicalDispatchCullingPass(
    HierarchicalCullingBackend backend,
    HierarchicalCullingWorkGraphMode workGraphMode,
    CLodRasterOutputKind rasterOutputKind)
{
    (void)workGraphMode;
    (void)rasterOutputKind;
    return backend == HierarchicalCullingBackend::PureCompute;
}

struct StructuralSchedulingPolicy {
    CLodTransparencyMode transparencyMode;
    bool disableReyesTessellation;
    bool useComputeSWRaster;
    bool useShadowPageJob;
    bool useShadowReyesRouting;
    bool useReyesForThisVariant;
    bool useWorkGraphReyesVisibility;
    HierarchicalCullingWorkGraphMode workGraphMode;
    HierarchicalCullingBackend cullingBackend;
    RenderPhase renderPhase;
};

StructuralSchedulingPolicy BuildStructuralSchedulingPolicy(
    const CLodVariantTraits& traits,
    CLodExtensionType type,
    bool enableReyes)
{
    const auto softwareRasterMode =
        SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)();
    const auto cullingBackendMode =
        SettingsManager::GetInstance().getSettingGetter<CLodCullingBackend>(CLodCullingBackendSettingName)();
    const auto shadowVSMRasterMode =
        traits.type == CLodExtensionType::Shadow
            ? SettingsManager::GetInstance().getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName)()
            : CLodVSMRasterMode::Standard;
    const CLodTransparencyMode transparencyMode = GetTransparencyMode(type);
    const bool disableReyesTessellation = !enableReyes;
    const bool forceHardwareOnly =
        traits.scheduleMode == CLodVariantTraits::ScheduleMode::SinglePassCullOnly ||
        traits.scheduleMode == CLodVariantTraits::ScheduleMode::SinglePassDeepVisibility ||
        (traits.type == CLodExtensionType::Shadow && CLodVSMRasterModeUsesLegacyRasterOnly(shadowVSMRasterMode));
    const bool useComputeSWRaster = !forceHardwareOnly && CLodSoftwareRasterUsesCompute(softwareRasterMode);
    const bool useShadowPageJob =
        traits.type == CLodExtensionType::Shadow && CLodVSMRasterModeUsesLargeClusterPageJob(shadowVSMRasterMode);
    const bool useShadowReyesRouting =
        traits.type == CLodExtensionType::Shadow && CLodVSMRasterModeUsesReyes(shadowVSMRasterMode);
    const bool useReyesForThisVariant =
        !disableReyesTessellation && (traits.type != CLodExtensionType::Shadow || useShadowReyesRouting);
    const HierarchicalCullingWorkGraphMode workGraphMode = forceHardwareOnly
        ? HierarchicalCullingWorkGraphMode::HardwareOnly
        : GetCullingWorkGraphMode(softwareRasterMode);
    const HierarchicalCullingBackend cullingBackend = GetHierarchicalCullingBackend(cullingBackendMode);
    const bool useWorkGraphReyesVisibility =
        useReyesForThisVariant &&
        traits.type == CLodExtensionType::VisiblityBuffer &&
        traits.rasterOutputKind == CLodRasterOutputKind::VisibilityBuffer &&
        cullingBackend == HierarchicalCullingBackend::WorkGraph &&
        workGraphMode == HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph &&
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodWorkGraphReyesVisibilitySettingName)();

    return StructuralSchedulingPolicy{
        .transparencyMode = transparencyMode,
        .disableReyesTessellation = disableReyesTessellation,
        .useComputeSWRaster = useComputeSWRaster,
        .useShadowPageJob = useShadowPageJob,
        .useShadowReyesRouting = useShadowReyesRouting,
        .useReyesForThisVariant = useReyesForThisVariant,
        .useWorkGraphReyesVisibility = useWorkGraphReyesVisibility,
        .workGraphMode = workGraphMode,
        .cullingBackend = cullingBackend,
        .renderPhase = RenderPhase(traits.renderPhaseName.data()),
    };
}

std::shared_ptr<ResourceGroup> GetSlabResourceGroup()
{
    try {
        auto getter = SettingsManager::GetInstance().getSettingGetter<std::function<MeshManager*()>>(CLodStreamingMeshManagerGetterSettingName);
        if (auto* meshManager = getter()()) {
            if (auto* pool = meshManager->GetCLodPagePool()) {
                return pool->GetSlabResourceGroup();
            }
        }
    } catch (...) {}

    return nullptr;
}

template<typename... Tags>
void SyncTaggedBufferEntity(const std::shared_ptr<Buffer>& buffer, const flecs::entity& typeEntity, bool enabled)
{
    if (!buffer) {
        return;
    }

    auto entity = buffer->GetECSEntity();
    if (enabled) {
        entity.set<Components::Resource>({ buffer });
        entity.add<CLodExtensionTypeTag>(typeEntity);
        (entity.add<Tags>(), ...);
    }
    else if (entity.has<Components::Resource>()) {
        entity.remove<Components::Resource>();
    }
}

struct ReyesResourceSizing {
    uint32_t fullClusterOutputCapacity = 0u;
    uint32_t ownedClusterCapacity = 0u;
    uint32_t splitQueueCapacity = 0u;
    uint32_t diceQueueCapacity = 0u;
    uint32_t diceQueuePhysicalCapacity = 0u;
    uint32_t rasterWorkCapacity = 0u;
    uint32_t ownershipBitsetWordCount = 0u;
    uint64_t requestedBudgetBytes = 0u;
    uint64_t allocatedBudgetBytes = 0u;
    bool budgetLimited = false;
};

uint64_t BufferBytes(uint32_t elementCount, size_t elementSize)
{
    return static_cast<uint64_t>(elementCount) * static_cast<uint64_t>(elementSize);
}

uint32_t BytesToElementCount(uint64_t allocatedBytes, size_t elementSize)
{
    if (elementSize == 0u) {
        throw std::runtime_error("Element size must be non-zero when converting budgeted bytes to element count.");
    }

    return static_cast<uint32_t>(allocatedBytes / static_cast<uint64_t>(elementSize));
}

ReyesResourceSizing BuildReyesResourceSizing(const CLodVariantTraits& traits, uint32_t maxVisibleClusters)
{
    const bool usesPhase2ReyesResources = traits.usesPhase2OcclusionReplay;
    const uint32_t ownershipBitsetWordCount = CLodBitsetWordCount(maxVisibleClusters);
    const uint32_t idealFullClusterOutputCapacity = maxVisibleClusters;
    const uint32_t idealOwnedClusterCapacity = maxVisibleClusters;
    const uint32_t idealSplitQueueCapacity = CLodReyesSplitQueueCapacity(maxVisibleClusters);
    const uint32_t idealDiceQueueCapacity = CLodReyesDiceQueueCapacity(maxVisibleClusters);
    const uint32_t idealRasterWorkCapacity = CLodReyesRasterWorkCapacity(maxVisibleClusters);
    const uint32_t diceQueuePhaseMultiplier = usesPhase2ReyesResources ? 2u : 1u;
    const ReyesTessellationTableData& reyesTessellationTableData = GetReyesTessellationTableData();

    std::vector<budget::ProportionalBudgetItem> budgetItems;
    budgetItems.reserve(24);

    auto addFixedItem = [&budgetItems](std::string_view id, uint64_t bytes) {
        budgetItems.push_back(budget::ProportionalBudgetItem{
            .id = id,
            .idealBytes = bytes,
            .minBytes = bytes,
            .maxBytes = bytes,
            .quantumBytes = 1u,
        });
    };

    auto addFlexibleItem = [&budgetItems](std::string_view id, uint64_t idealBytes, uint64_t quantumBytes) {
        budgetItems.push_back(budget::ProportionalBudgetItem{
            .id = id,
            .idealBytes = idealBytes,
            .minBytes = quantumBytes,
            .maxBytes = idealBytes,
            .quantumBytes = quantumBytes,
        });
    };

    addFlexibleItem("fullClusterOutputs", BufferBytes(idealFullClusterOutputCapacity, sizeof(CLodReyesFullClusterOutput)), sizeof(CLodReyesFullClusterOutput));
    addFixedItem("fullClusterOutputCounter", sizeof(uint32_t));
    addFlexibleItem("ownedClusters", BufferBytes(idealOwnedClusterCapacity, sizeof(CLodReyesOwnedClusterEntry)), sizeof(CLodReyesOwnedClusterEntry));
    addFixedItem("ownedClusterCounter", sizeof(uint32_t));
    addFixedItem("ownershipBitset", BufferBytes(ownershipBitsetWordCount, sizeof(uint32_t)));
    if (usesPhase2ReyesResources) {
        addFixedItem("ownershipBitsetPhase2", BufferBytes(ownershipBitsetWordCount, sizeof(uint32_t)));
    }

    addFixedItem("classifyIndirectArgs", sizeof(CLodReyesDispatchIndirectCommand));
    if (usesPhase2ReyesResources) {
        addFixedItem("classifyIndirectArgsPhase2", sizeof(CLodReyesDispatchIndirectCommand));
    }

    addFixedItem("splitIndirectArgs", sizeof(CLodReyesDispatchIndirectCommand));
    if (usesPhase2ReyesResources) {
        addFixedItem("splitIndirectArgsPhase2", sizeof(CLodReyesDispatchIndirectCommand));
    }

    addFlexibleItem("splitQueueA", BufferBytes(idealSplitQueueCapacity, sizeof(CLodReyesSplitQueueEntry)), sizeof(CLodReyesSplitQueueEntry));
    addFixedItem("splitQueueCounterA", sizeof(uint32_t));
    addFixedItem("splitQueueOverflowA", sizeof(uint32_t));
    addFlexibleItem("splitQueueB", BufferBytes(idealSplitQueueCapacity, sizeof(CLodReyesSplitQueueEntry)), sizeof(CLodReyesSplitQueueEntry));
    addFixedItem("splitQueueCounterB", sizeof(uint32_t));
    addFixedItem("splitQueueOverflowB", sizeof(uint32_t));
    if (usesPhase2ReyesResources) {
        addFlexibleItem("replaySplitQueue", BufferBytes(idealSplitQueueCapacity, sizeof(CLodReyesSplitQueueEntry)), sizeof(CLodReyesSplitQueueEntry));
        addFixedItem("replaySplitQueueCounter", sizeof(uint32_t));
        addFixedItem("replaySplitQueueOverflow", sizeof(uint32_t));
    }

    addFlexibleItem(
        "diceQueue",
        BufferBytes(idealDiceQueueCapacity * diceQueuePhaseMultiplier, sizeof(CLodReyesDiceQueueEntry)),
        static_cast<uint64_t>(sizeof(CLodReyesDiceQueueEntry)) * static_cast<uint64_t>(diceQueuePhaseMultiplier));
    addFixedItem("diceQueueCounter", sizeof(uint32_t));
    if (usesPhase2ReyesResources) {
        addFixedItem("diceQueuePhase1Count", sizeof(uint32_t));
        addFlexibleItem("replayDiceQueue", BufferBytes(idealDiceQueueCapacity, sizeof(CLodReyesDiceQueueEntry)), sizeof(CLodReyesDiceQueueEntry));
        addFixedItem("replayDiceQueueCounter", sizeof(uint32_t));
        addFixedItem("replayDiceQueueOverflow", sizeof(uint32_t));
    }
    addFixedItem("diceQueueOverflow", sizeof(uint32_t));

    addFlexibleItem("rasterWorkPhase1", BufferBytes(idealRasterWorkCapacity, sizeof(CLodReyesRasterWorkEntry)), sizeof(CLodReyesRasterWorkEntry));
    addFixedItem("rasterWorkCounterPhase1", sizeof(uint32_t));
    addFixedItem("rasterWorkIndirectArgsPhase1", sizeof(CLodReyesDispatchIndirectCommand));
    if (usesPhase2ReyesResources) {
        addFlexibleItem("rasterWorkPhase2", BufferBytes(idealRasterWorkCapacity, sizeof(CLodReyesRasterWorkEntry)), sizeof(CLodReyesRasterWorkEntry));
        addFixedItem("rasterWorkCounterPhase2", sizeof(uint32_t));
        addFixedItem("rasterWorkIndirectArgsPhase2", sizeof(CLodReyesDispatchIndirectCommand));
    }

    addFixedItem("tessTableConfigs", BufferBytes(static_cast<uint32_t>(reyesTessellationTableData.configs.size()), sizeof(CLodReyesTessTableConfigEntry)));
    addFixedItem("tessTableVertices", BufferBytes(static_cast<uint32_t>(reyesTessellationTableData.vertices.size()), sizeof(uint32_t)));
    addFixedItem("tessTableTriangles", BufferBytes(static_cast<uint32_t>(reyesTessellationTableData.triangles.size()), sizeof(uint32_t)));
    addFixedItem("diceIndirectArgsPhase1", sizeof(CLodReyesDispatchIndirectCommand));
    if (usesPhase2ReyesResources) {
        addFixedItem("diceIndirectArgsPhase2", sizeof(CLodReyesDispatchIndirectCommand));
    }
    addFixedItem("telemetryPhase1", sizeof(CLodReyesTelemetry));
    if (usesPhase2ReyesResources) {
        addFixedItem("telemetryPhase2", sizeof(CLodReyesTelemetry));
    }

    uint64_t idealBudgetBytes = 0u;
    for (const budget::ProportionalBudgetItem& item : budgetItems) {
        idealBudgetBytes += item.idealBytes;
    }

    const uint64_t configuredBudgetBytes = static_cast<uint64_t>(SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodReyesResourceBudgetBytesSettingName)());
    const uint64_t effectiveBudgetBytes = configuredBudgetBytes == 0u ? idealBudgetBytes : configuredBudgetBytes;
    const budget::ProportionalBudgetResult budgetResult = budget::AllocateProportionalBudget(budgetItems, effectiveBudgetBytes);

    ReyesResourceSizing sizing{};
    sizing.fullClusterOutputCapacity = BytesToElementCount(budgetResult.Find("fullClusterOutputs").allocatedBytes, sizeof(CLodReyesFullClusterOutput));
    sizing.ownedClusterCapacity = BytesToElementCount(budgetResult.Find("ownedClusters").allocatedBytes, sizeof(CLodReyesOwnedClusterEntry));
    sizing.splitQueueCapacity = BytesToElementCount(budgetResult.Find("splitQueueA").allocatedBytes, sizeof(CLodReyesSplitQueueEntry));
    sizing.diceQueueCapacity = static_cast<uint32_t>(
        budgetResult.Find("diceQueue").allocatedBytes /
        (static_cast<uint64_t>(sizeof(CLodReyesDiceQueueEntry)) * static_cast<uint64_t>(diceQueuePhaseMultiplier)));
    sizing.diceQueuePhysicalCapacity = sizing.diceQueueCapacity * diceQueuePhaseMultiplier;
    sizing.rasterWorkCapacity = BytesToElementCount(budgetResult.Find("rasterWorkPhase1").allocatedBytes, sizeof(CLodReyesRasterWorkEntry));
    sizing.ownershipBitsetWordCount = ownershipBitsetWordCount;
    sizing.requestedBudgetBytes = budgetResult.requestedTotalBytes;
    sizing.allocatedBudgetBytes = budgetResult.allocatedTotalBytes;
    sizing.budgetLimited = budgetResult.allocatedTotalBytes < budgetResult.requestedTotalBytes;
    return sizing;
}

} // namespace

void CLodExtension::AppendPhaseReyesStructuralPasses(
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    const std::shared_ptr<Buffer>& reyesOwnershipBitsetBuffer,
    uint32_t reyesSplitQueueCapacity,
    uint32_t reyesDiceQueueCapacity,
    uint32_t reyesRasterWorkCapacity,
    uint32_t phaseIndex,
    bool uploadTessellationTable,
    bool preserveDiceCountForPhase2Replay,
    bool workGraphReyesVisibility,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses,
    std::string& shadowClearDirtyBitsAfterPassName)
{
    const auto phaseSuffix = std::to_string(phaseIndex);
    const auto classifyIndirectArgsBuffer = phaseIndex == 1u ? m_reyesClassifyIndirectArgsBuffer : m_reyesClassifyIndirectArgsBufferPhase2;
    const auto splitIndirectArgsBuffer = phaseIndex == 1u ? m_reyesSplitIndirectArgsBuffer : m_reyesSplitIndirectArgsBufferPhase2;
    const auto diceIndirectArgsBuffer = phaseIndex == 1u ? m_reyesDiceIndirectArgsBuffer : m_reyesDiceIndirectArgsBufferPhase2;
    const auto telemetryBuffer = phaseIndex == 1u ? m_reyesTelemetryBufferPhase1 : m_reyesTelemetryBufferPhase2;
    const auto currentVisibleClustersCounterBuffer = phaseIndex == 1u ? m_visibleClustersCounterBuffer : m_visibleClustersCounterBufferPhase2;
    const auto previousVisibleClustersCounterBuffer = phaseIndex == 1u ? nullptr : m_visibleClustersCounterBuffer;
    const auto diceQueuePhase1CountBuffer = phaseIndex == 1u ? nullptr : m_reyesDiceQueuePhase1CountBuffer;
    const auto rasterWorkBuffer = phaseIndex == 1u ? m_reyesRasterWorkBuffer : m_reyesRasterWorkBufferPhase2;
    const auto rasterWorkCounterBuffer = phaseIndex == 1u ? m_reyesRasterWorkCounterBuffer : m_reyesRasterWorkCounterBufferPhase2;
    const auto rasterWorkIndirectArgsBuffer = phaseIndex == 1u ? m_reyesRasterWorkIndirectArgsBuffer : m_reyesRasterWorkIndirectArgsBufferPhase2;

    if (uploadTessellationTable) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesTessellationTableUploadPass"),
                std::make_shared<ReyesTessellationTableUploadPass>(
                    m_reyesTessTableConfigsBuffer,
                    m_reyesTessTableVerticesBuffer,
                    m_reyesTessTableTrianglesBuffer)));
    }

    if (workGraphReyesVisibility) {
        return;
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesQueueResetPass" + phaseSuffix),
            std::make_shared<ReyesQueueResetPass>(
                m_reyesFullClusterOutputsCounterBuffer,
                m_reyesOwnedClustersCounterBuffer,
                std::vector<std::shared_ptr<Buffer>>{ m_reyesSplitQueueCounterBufferA, m_reyesSplitQueueCounterBufferB },
                std::vector<std::shared_ptr<Buffer>>{ m_reyesSplitQueueOverflowBufferA, m_reyesSplitQueueOverflowBufferB },
                m_reyesDiceQueueCounterBuffer,
                m_reyesDiceQueueOverflowBuffer,
                reyesOwnershipBitsetBuffer,
                telemetryBuffer,
                phaseIndex,
                phaseIndex == 1u,
                phaseIndex == 1u ? m_reyesReplaySplitQueueCounterBuffer : nullptr,
                phaseIndex == 1u ? m_reyesReplaySplitQueueOverflowBuffer : nullptr,
                phaseIndex == 1u ? m_reyesReplayDiceQueueCounterBuffer : nullptr,
                phaseIndex == 1u ? m_reyesReplayDiceQueueOverflowBuffer : nullptr)));

    const bool enableReyesPatchOcclusion =
        traits.type == CLodExtensionType::VisiblityBuffer &&
        (phaseIndex == 2u || preserveDiceCountForPhase2Replay);
    const std::shared_ptr<Buffer> reyesPatchOcclusionDepthIndices =
        enableReyesPatchOcclusion
            ? (phaseIndex == 1u ? m_viewDepthSrvIndicesBuffer : m_viewDepthSrvIndicesBufferPhase2)
            : nullptr;

    if (phaseIndex == 2u && traits.type == CLodExtensionType::VisiblityBuffer) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesCreateReplayDiceMergeDispatchArgs2"),
                std::make_shared<ReyesCreateDispatchArgsPass>(
                    m_reyesReplayDiceQueueCounterBuffer,
                    diceIndirectArgsBuffer,
                    nullptr,
                    64u,
                    reyesDiceQueueCapacity)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesReplayDiceMergePass2"),
                std::make_shared<ReyesReplayMergePass>(
                    ReyesReplayMergeKind::Dice,
                    m_reyesReplayDiceQueueBuffer,
                    m_reyesReplayDiceQueueCounterBuffer,
                    m_reyesDiceQueueBuffer,
                    m_reyesDiceQueueCounterBuffer,
                    m_reyesDiceQueueOverflowBuffer,
                    diceIndirectArgsBuffer,
                    telemetryBuffer,
                    m_reyesDiceQueuePhysicalCapacity)));
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesCreateClassifyDispatchArgsPass" + phaseSuffix),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                currentVisibleClustersCounterBuffer,
                classifyIndirectArgsBuffer)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesClassifyPass" + phaseSuffix),
            std::make_shared<ReyesClassifyPass>(
                m_visibleClustersBuffer,
                currentVisibleClustersCounterBuffer,
                previousVisibleClustersCounterBuffer,
                m_reyesFullClusterOutputsBuffer,
                m_reyesFullClusterOutputsCounterBuffer,
                m_reyesFullClusterOutputCapacity,
                m_reyesOwnedClustersBuffer,
                m_reyesOwnedClustersCounterBuffer,
                m_reyesOwnedClusterCapacity,
                reyesOwnershipBitsetBuffer,
                classifyIndirectArgsBuffer,
                telemetryBuffer,
                phaseIndex,
                traits.type == CLodExtensionType::Shadow
                    ? ReyesClassifyMode::ShadowFineDisplacedOnly
                    : ReyesClassifyMode::Default)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesCreateSeedDispatchArgsPass" + phaseSuffix),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                m_reyesOwnedClustersCounterBuffer,
                splitIndirectArgsBuffer)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesSeedPatchesPass" + phaseSuffix),
            std::make_shared<ReyesSeedPatchesPass>(
                m_visibleClustersBuffer,
                m_reyesOwnedClustersBuffer,
                m_reyesOwnedClustersCounterBuffer,
                m_reyesSplitQueueBufferA,
                m_reyesSplitQueueCounterBufferA,
                m_reyesSplitQueueOverflowBufferA,
                splitIndirectArgsBuffer,
                slabGroup,
                reyesSplitQueueCapacity,
                phaseIndex)));

    if (phaseIndex == 2u && traits.type == CLodExtensionType::VisiblityBuffer) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesCreateReplaySplitMergeDispatchArgs2"),
                std::make_shared<ReyesCreateDispatchArgsPass>(
                    m_reyesReplaySplitQueueCounterBuffer,
                    splitIndirectArgsBuffer,
                    nullptr,
                    64u,
                    reyesSplitQueueCapacity)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesReplaySplitMergePass2"),
                std::make_shared<ReyesReplayMergePass>(
                    ReyesReplayMergeKind::Split,
                    m_reyesReplaySplitQueueBuffer,
                    m_reyesReplaySplitQueueCounterBuffer,
                    m_reyesSplitQueueBufferA,
                    m_reyesSplitQueueCounterBufferA,
                    m_reyesSplitQueueOverflowBufferA,
                    splitIndirectArgsBuffer,
                    telemetryBuffer,
                    reyesSplitQueueCapacity)));
    }

    const std::shared_ptr<Buffer> reyesSplitBuffers[] = { m_reyesSplitQueueBufferA, m_reyesSplitQueueBufferB };
    const std::shared_ptr<Buffer> reyesSplitCounters[] = { m_reyesSplitQueueCounterBufferA, m_reyesSplitQueueCounterBufferB };
    const std::shared_ptr<Buffer> reyesSplitOverflows[] = { m_reyesSplitQueueOverflowBufferA, m_reyesSplitQueueOverflowBufferB };
    for (uint32_t splitPassIndex = 0; splitPassIndex < CLodReyesMaxSplitPassCount; ++splitPassIndex) {
        const uint32_t inputIndex = splitPassIndex & 1u;
        const uint32_t outputIndex = inputIndex ^ 1u;

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesCreateSplitDispatchArgsPass" + phaseSuffix + "_" + std::to_string(splitPassIndex)),
                std::make_shared<ReyesCreateDispatchArgsPass>(
                    reyesSplitCounters[inputIndex],
                    splitIndirectArgsBuffer)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesSplitPass" + phaseSuffix + "_" + std::to_string(splitPassIndex)),
                std::make_shared<ReyesSplitPass>(
                    m_visibleClustersBuffer,
                    reyesSplitBuffers[inputIndex],
                    reyesSplitCounters[inputIndex],
                    reyesSplitBuffers[outputIndex],
                    reyesSplitCounters[outputIndex],
                    reyesSplitOverflows[outputIndex],
                    m_reyesDiceQueueBuffer,
                    m_reyesDiceQueueCounterBuffer,
                    m_reyesDiceQueueOverflowBuffer,
                    m_reyesTessTableConfigsBuffer,
                    m_reyesTessTableVerticesBuffer,
                    m_reyesTessTableTrianglesBuffer,
                    traits.type == CLodExtensionType::Shadow ? m_shadowClipmapInfoBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowDirtyPageHierarchyTexture : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowNonRasterablePageHierarchyTexture : nullptr,
                    splitIndirectArgsBuffer,
                    telemetryBuffer,
                    reyesSplitQueueCapacity,
                    splitPassIndex,
                    CLodReyesMaxSplitPassCount,
                    phaseIndex,
                    reyesPatchOcclusionDepthIndices,
                    enableReyesPatchOcclusion ? m_reyesReplaySplitQueueBuffer : nullptr,
                    enableReyesPatchOcclusion ? m_reyesReplaySplitQueueCounterBuffer : nullptr,
                    enableReyesPatchOcclusion ? m_reyesReplaySplitQueueOverflowBuffer : nullptr)));
    }

    if (phaseIndex == 1u) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesCreateDiceDispatchArgsPass1"),
                std::make_shared<ReyesCreateDispatchArgsPass>(
                    m_reyesDiceQueueCounterBuffer,
                    diceIndirectArgsBuffer)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesCreateDiceDispatchArgsPass2"),
                std::make_shared<ReyesCreateDispatchArgsPass>(
                    m_reyesDiceQueueCounterBuffer,
                    diceIndirectArgsBuffer,
                    m_reyesDiceQueuePhase1CountBuffer)));
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesDicePass" + phaseSuffix),
            std::make_shared<ReyesDicePass>(
                m_reyesDiceQueueBuffer,
                m_reyesDiceQueueCounterBuffer,
                diceQueuePhase1CountBuffer,
                m_reyesTessTableConfigsBuffer,
                diceIndirectArgsBuffer,
                telemetryBuffer,
                reyesDiceQueueCapacity,
                phaseIndex)));

    if (preserveDiceCountForPhase2Replay) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Copy(
                MakeVariantPassName(traits, "ReyesCopyDiceCountPass1"),
                std::make_shared<ReyesCopyCounterPass>(
                    m_reyesDiceQueueCounterBuffer,
                    m_reyesDiceQueuePhase1CountBuffer)));
    }

    if (traits.type == CLodExtensionType::VisiblityBuffer) {
        if (phaseIndex == 1u) {
            CLodVisibilityVariant::AppendPhase1ReyesRasterPasses(*this, traits, slabGroup, outPasses);
        }
        else {
            CLodVisibilityVariant::AppendPhase2ReyesRasterPasses(*this, traits, slabGroup, outPasses);
        }
        return;
    }

    if (traits.type != CLodExtensionType::AlphaBlend && traits.type != CLodExtensionType::Shadow) {
        return;
    }

    if (phaseIndex != 1u && traits.type != CLodExtensionType::Shadow) {
        return;
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesBuildRasterWorkPass" + phaseSuffix),
            std::make_shared<ReyesBuildRasterWorkPass>(
                m_reyesDiceQueueBuffer,
                m_reyesDiceQueueCounterBuffer,
                diceQueuePhase1CountBuffer,
                m_reyesTessTableConfigsBuffer,
                rasterWorkBuffer,
                rasterWorkCounterBuffer,
                diceIndirectArgsBuffer,
                telemetryBuffer,
                reyesRasterWorkCapacity,
                phaseIndex,
                enableReyesPatchOcclusion ? m_visibleClustersBuffer : nullptr,
                enableReyesPatchOcclusion ? m_visibleClusterTransformIndicesBuffer : nullptr,
                reyesPatchOcclusionDepthIndices,
                enableReyesPatchOcclusion ? m_reyesReplayDiceQueueBuffer : nullptr,
                enableReyesPatchOcclusion ? m_reyesReplayDiceQueueCounterBuffer : nullptr,
                enableReyesPatchOcclusion ? m_reyesReplayDiceQueueOverflowBuffer : nullptr,
                reyesDiceQueueCapacity,
                slabGroup)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesCreateRasterWorkDispatchArgsPass" + phaseSuffix),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                rasterWorkCounterBuffer,
                rasterWorkIndirectArgsBuffer)));

    if (traits.type == CLodExtensionType::Shadow) {
        shadowClearDirtyBitsAfterPassName = phaseIndex == 1u
            ? CLodShadowVariant::AppendPhase1FineRasterPass(*this, traits, slabGroup, outPasses)
            : CLodShadowVariant::AppendPhase2FineRasterPass(*this, traits, slabGroup, outPasses);
    }
}

CLodExtension::~CLodExtension() = default;

bool CLodExtension::IsReyesTessellationDisabled() const
{
    return !m_options.enableReyes;
}

void CLodExtension::RefreshShadowConfiguredSettings()
{
    CLodShadowVariant::RefreshConfiguredSettings(*this);
}

uint32_t CLodExtension::GetVisibleClusterCapacity() const
{
    return CLodShadowVariant::GetVisibleClusterCapacity(*this);
}

void CLodExtension::RefreshCoreVisibleClusterCapacity()
{
    const uint32_t desiredVisibleClusterCapacity = GetVisibleClusterCapacity();
    if (m_visibleClusterCapacity == desiredVisibleClusterCapacity) {
        return;
    }

    m_visibleClusterCapacity = desiredVisibleClusterCapacity;

    if (m_visibleClustersBuffer) {
        m_visibleClustersBuffer->ResizeBytes(static_cast<uint64_t>(m_visibleClusterCapacity) * PackedVisibleClusterStrideBytes);
        m_visibleClustersBuffer->GetECSEntity().set<CLodVisibleClusterCapacity>({ m_visibleClusterCapacity });
    }
    if (m_visibleClusterTransformIndicesBuffer) {
        m_visibleClusterTransformIndicesBuffer->ResizeStructured(m_visibleClusterCapacity);
        m_visibleClusterTransformIndicesBuffer->GetECSEntity().set<CLodVisibleClusterCapacity>({ m_visibleClusterCapacity });
    }

    if (m_compactedVisibleClustersBuffer) {
        m_compactedVisibleClustersBuffer->ResizeBytes(static_cast<uint64_t>(m_visibleClusterCapacity) * PackedVisibleClusterStrideBytes);
    }
    if (m_compactedVisibleClusterTransformIndicesBuffer) {
        m_compactedVisibleClusterTransformIndicesBuffer->ResizeStructured(m_visibleClusterCapacity);
    }

    if (m_compactedVisibleClustersBufferSw) {
        m_compactedVisibleClustersBufferSw->ResizeBytes(static_cast<uint64_t>(m_visibleClusterCapacity) * PackedVisibleClusterStrideBytes);
    }
    if (m_compactedVisibleClusterTransformIndicesBufferSw) {
        m_compactedVisibleClusterTransformIndicesBufferSw->ResizeStructured(m_visibleClusterCapacity);
    }

    if (m_sortedToUnsortedMappingBuffer) {
        m_sortedToUnsortedMappingBuffer->ResizeStructured(m_visibleClusterCapacity);
    }

    if (m_sortedToUnsortedMappingBufferSw) {
        m_sortedToUnsortedMappingBufferSw->ResizeStructured(m_visibleClusterCapacity);
    }
}

void CLodExtension::InitializeCoreResources()
{
    const auto& traits = GetVariantTraits(m_type);
    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    const flecs::entity typeEntity = traits.ensureTypeEntity(ecsWorld);

    m_visibleClustersBuffer = CreateAliasedUnmaterializedRawBuffer(static_cast<uint64_t>(m_visibleClusterCapacity) * PackedVisibleClusterStrideBytes, true, false, true);
    m_visibleClustersBuffer->SetName(MakeVariantResourceName(traits, "Visible Clusters Buffer (uncompacted)"));
    m_visibleClusterTransformIndicesBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_visibleClusterCapacity, sizeof(uint32_t), true, false, false, true);
    m_visibleClusterTransformIndicesBuffer->SetName(MakeVariantResourceName(traits, "Visible Cluster Transform Indices Buffer (uncompacted)"));

    auto createHistogramIndirectCommand = [&](std::string_view name) {
        auto buffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterBucketsHistogramIndirectCommand), true, false, true, true);
        buffer->SetName(MakeVariantResourceName(traits, name));
        return buffer;
    };
    m_histogramIndirectCommand = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer HW phase1");
    m_histogramIndirectCommandPhase2 = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer HW phase2");
    m_histogramIndirectCommandSw = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer SW phase1");
    m_histogramIndirectCommandPhase2Sw = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer SW phase2");
    m_histogramIndirectCommandPageJob = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer Page Job phase1");
    m_histogramIndirectCommandPhase2PageJob = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer Page Job phase2");
    m_histogramIndirectCommandReyes = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer Reyes phase1");
    m_histogramIndirectCommandPhase2Reyes = createHistogramIndirectCommand("Raster Buckets Histogram Indirect Command Buffer Reyes phase2");

    m_rasterBucketsHistogramBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    m_rasterBucketsHistogramBuffer->SetName(MakeVariantResourceName(traits, "Raster bucket histogram"));

    m_visibleClustersCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(unsigned int), true, false, false, false);
    m_visibleClustersCounterBuffer->SetName(MakeVariantResourceName(traits, "Visible Clusters Counter Buffer"));
    m_visibleClustersCounterBuffer->GetECSEntity()
        .set<Components::Resource>({ m_visibleClustersCounterBuffer })
        .add<VisibleClustersCounterTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_workGraphTelemetryBuffer = CreateAliasedUnmaterializedStructuredBuffer(CLodWorkGraphCounterCount, sizeof(uint32_t), true, false, false, false);
    m_workGraphTelemetryBuffer->SetName(MakeVariantResourceName(traits, "Work Graph Telemetry Buffer"));
    m_workGraphTelemetryBuffer->GetECSEntity()
        .set<Components::Resource>({ m_workGraphTelemetryBuffer })
        .add<CLodWorkGraphTelemetryBufferTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    // HLSL binds this as RWByteAddressBuffer; publish a raw UAV so Store/Load offsets are byte offsets.
    m_occlusionReplayBuffer = CreateAliasedUnmaterializedRawBuffer(CLodReplayBufferSizeBytes, true, false, false); // TODO: Alias this when we don't need the gpu address in node input during setup
    m_occlusionReplayBuffer->SetName(MakeVariantResourceName(traits, "Occlusion Replay Buffer"));

    m_occlusionReplayStateBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReplayBufferState), true, false, false, false);
    m_occlusionReplayStateBuffer->SetName(MakeVariantResourceName(traits, "Occlusion Replay State Buffer"));

    m_occlusionNodeGpuInputsBuffer = CreateAliasedUnmaterializedStructuredBuffer(5, sizeof(CLodNodeGpuInput), true, false, false, false);
    m_occlusionNodeGpuInputsBuffer->SetName(MakeVariantResourceName(traits, "Occlusion Node GPU Inputs Buffer"));

    m_viewDepthSrvIndicesBuffer = CreateAliasedUnmaterializedStructuredBuffer(CLodMaxViewDepthIndices, sizeof(CLodViewDepthSRVIndex), true, false, false, false);
    m_viewDepthSrvIndicesBuffer->SetName(MakeVariantResourceName(traits, "View Depth SRV Indices Buffer"));

    m_viewDepthSrvIndicesBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(CLodMaxViewDepthIndices, sizeof(CLodViewDepthSRVIndex), true, false, false, false);
    m_viewDepthSrvIndicesBufferPhase2->SetName(MakeVariantResourceName(traits, "View Depth SRV Indices Buffer Phase2"));

    m_rasterBucketsOffsetsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    m_rasterBucketsOffsetsBuffer->SetName(MakeVariantResourceName(traits, "Raster bucket offsets"));

    m_rasterBucketsBlockSumsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    m_rasterBucketsBlockSumsBuffer->SetName(MakeVariantResourceName(traits, "Raster bucket block sums"));

    m_rasterBucketsScannedBlockSumsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    m_rasterBucketsScannedBlockSumsBuffer->SetName(MakeVariantResourceName(traits, "Raster bucket scanned block sums"));

    m_rasterBucketsTotalCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    m_rasterBucketsTotalCountBuffer->SetName(MakeVariantResourceName(traits, "Raster bucket total count"));

    m_rasterBucketsTotalCountBufferPhase1 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    m_rasterBucketsTotalCountBufferPhase1->SetName(MakeVariantResourceName(traits, "Raster bucket total count phase1"));

    m_rasterBucketsTotalCountBufferPhase1Sw = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, true);
    m_rasterBucketsTotalCountBufferPhase1Sw->SetName(MakeVariantResourceName(traits, "Raster bucket total count phase1 SW"));

    m_compactedVisibleClustersBuffer = CreateAliasedUnmaterializedRawBuffer(static_cast<uint64_t>(m_visibleClusterCapacity) * PackedVisibleClusterStrideBytes, true, false);
    m_compactedVisibleClustersBuffer->SetName(MakeVariantResourceName(traits, "Compacted Visible Clusters Buffer"));
    m_compactedVisibleClusterTransformIndicesBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_visibleClusterCapacity, sizeof(uint32_t), true, false, false, true);
    m_compactedVisibleClusterTransformIndicesBuffer->SetName(MakeVariantResourceName(traits, "Compacted Visible Cluster Transform Indices Buffer"));

    m_compactedVisibleClustersBufferSw = CreateAliasedUnmaterializedRawBuffer(static_cast<uint64_t>(m_visibleClusterCapacity) * PackedVisibleClusterStrideBytes, true, false);
    m_compactedVisibleClustersBufferSw->SetName(MakeVariantResourceName(traits, "Compacted Visible Clusters Buffer SW"));
    m_compactedVisibleClusterTransformIndicesBufferSw = CreateAliasedUnmaterializedStructuredBuffer(m_visibleClusterCapacity, sizeof(uint32_t), true, false, false, true);
    m_compactedVisibleClusterTransformIndicesBufferSw->SetName(MakeVariantResourceName(traits, "Compacted Visible Cluster Transform Indices Buffer SW"));

    m_visibleClustersBuffer->GetECSEntity()
        .set<Components::Resource>({ m_visibleClustersBuffer })
        .set<CLodVisibleClusterCapacity>({ m_visibleClusterCapacity })
        .add<VisibleClustersBufferTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_visibleClusterTransformIndicesBuffer->GetECSEntity()
        .set<Components::Resource>({ m_visibleClusterTransformIndicesBuffer })
        .set<CLodVisibleClusterCapacity>({ m_visibleClusterCapacity })
        .add<VisibleClusterTransformIndicesBufferTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_rasterBucketsWriteCursorBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    m_rasterBucketsWriteCursorBuffer->SetName(MakeVariantResourceName(traits, "Raster bucket write cursor"));

    m_rasterBucketsIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, true);
    m_rasterBucketsIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Raster bucket indirect args HW phase1"));

    m_rasterBucketsIndirectArgsBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, true);
    m_rasterBucketsIndirectArgsBufferPhase2->SetName(MakeVariantResourceName(traits, "Raster bucket indirect args HW phase2"));

    m_rasterBucketsIndirectArgsBufferSw = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, true);
    m_rasterBucketsIndirectArgsBufferSw->SetName(MakeVariantResourceName(traits, "Raster bucket indirect args SW phase1"));

    m_rasterBucketsIndirectArgsBufferPhase2Sw = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, true);
    m_rasterBucketsIndirectArgsBufferPhase2Sw->SetName(MakeVariantResourceName(traits, "Raster bucket indirect args SW phase2"));

    m_rasterBucketsIndirectArgsBufferPageJob = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, true);
    m_rasterBucketsIndirectArgsBufferPageJob->SetName(MakeVariantResourceName(traits, "Raster bucket indirect args Page Job phase1"));

    m_rasterBucketsIndirectArgsBufferPhase2PageJob = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, true);
    m_rasterBucketsIndirectArgsBufferPhase2PageJob->SetName(MakeVariantResourceName(traits, "Raster bucket indirect args Page Job phase2"));

    m_visibleClustersCounterBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(unsigned int), true, false, false, false);
    m_visibleClustersCounterBufferPhase2->SetName(MakeVariantResourceName(traits, "Visible Clusters Counter Buffer Phase2"));

    m_rasterBucketsHistogramBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    m_rasterBucketsHistogramBufferPhase2->SetName(MakeVariantResourceName(traits, "Raster bucket histogram phase2"));

    m_rasterBucketsHistogramBufferSw = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, true, false);
    m_rasterBucketsHistogramBufferSw->SetName(MakeVariantResourceName(traits, "Raster bucket histogram SW phase1"));

    m_rasterBucketsHistogramBufferPhase2Sw = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, true, false);
    m_rasterBucketsHistogramBufferPhase2Sw->SetName(MakeVariantResourceName(traits, "Raster bucket histogram SW phase2"));

    m_rasterBucketsWriteCursorBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    m_rasterBucketsWriteCursorBufferPhase2->SetName(MakeVariantResourceName(traits, "Raster bucket write cursor phase2"));

    m_rasterBucketsWriteCursorBufferSw = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, true, false);
    m_rasterBucketsWriteCursorBufferSw->SetName(MakeVariantResourceName(traits, "Raster bucket write cursor SW phase1"));

    m_rasterBucketsWriteCursorBufferPhase2Sw = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, true, false);
    m_rasterBucketsWriteCursorBufferPhase2Sw->SetName(MakeVariantResourceName(traits, "Raster bucket write cursor SW phase2"));

    m_swVisibleClustersCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(unsigned int), true, false, false, false);
    m_swVisibleClustersCounterBuffer->SetName(MakeVariantResourceName(traits, "SW Visible Clusters Counter Buffer Phase1"));

    m_swVisibleClustersCounterBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(unsigned int), true, false, false, false);
    m_swVisibleClustersCounterBufferPhase2->SetName(MakeVariantResourceName(traits, "SW Visible Clusters Counter Buffer Phase2"));

    if (m_options.enableVoxelRasterization) {
        m_voxelRasterWorkCapacity = std::clamp(
            m_options.voxelRasterWorkCapacity,
            1u,
            m_visibleClusterCapacity);
        m_voxelRasterWorkBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_voxelRasterWorkCapacity, sizeof(CLodVoxelRasterWorkRecord), true, false, false, true);
        m_voxelRasterWorkBuffer->SetName(MakeVariantResourceName(traits, "Voxel Raster Work Buffer Rigid"));

        m_voxelRasterWorkCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
        m_voxelRasterWorkCounterBuffer->SetName(MakeVariantResourceName(traits, "Voxel Raster Work Counter Buffer Rigid"));

        m_skinnedVoxelRasterWorkBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_voxelRasterWorkCapacity, sizeof(CLodVoxelRasterWorkRecord), true, false, false, true);
        m_skinnedVoxelRasterWorkBuffer->SetName(MakeVariantResourceName(traits, "Voxel Raster Work Buffer Skinned"));

        m_skinnedVoxelRasterWorkCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
        m_skinnedVoxelRasterWorkCounterBuffer->SetName(MakeVariantResourceName(traits, "Voxel Raster Work Counter Buffer Skinned"));

        m_voxelRasterIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodVoxelRasterDispatchCommand), true, false, false, true);
        m_voxelRasterIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Voxel Raster Indirect Args Buffer Rigid"));

        m_skinnedVoxelRasterIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodVoxelRasterDispatchCommand), true, false, false, true);
        m_skinnedVoxelRasterIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Voxel Raster Indirect Args Buffer Skinned"));

        const uint64_t queueBytes = static_cast<uint64_t>(m_voxelRasterWorkCapacity) * sizeof(CLodVoxelRasterWorkRecord);
        spdlog::info(
            "CLod voxel raster allocation '{}': capacity={} perQueueMiB={:.2f} pairMiB={:.2f}",
            traits.resourcePrefix,
            m_voxelRasterWorkCapacity,
            static_cast<double>(queueBytes) / (1024.0 * 1024.0),
            static_cast<double>(queueBytes * 2u) / (1024.0 * 1024.0));
    }

    m_sortedToUnsortedMappingBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_visibleClusterCapacity, sizeof(uint32_t), true, false, false, true);
    m_sortedToUnsortedMappingBuffer->SetName(MakeVariantResourceName(traits, "Sorted-to-Unsorted Mapping Buffer"));

    m_sortedToUnsortedMappingBufferSw = CreateAliasedUnmaterializedStructuredBuffer(m_visibleClusterCapacity, sizeof(uint32_t) * 2u, true, false, false, true);
    m_sortedToUnsortedMappingBufferSw->SetName(MakeVariantResourceName(traits, "Sorted-to-Unsorted Mapping Buffer SW"));

    m_viewRasterInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodViewRasterInfo), false, false, false, false);
    m_viewRasterInfoBuffer->SetName(MakeVariantResourceName(traits, "View Raster Info Buffer"));
}

void CLodExtension::InitializeDeepVisibilityResources()
{
    CLodAlphaVariant::InitializeDeepVisibilityResources(*this);
}

void CLodExtension::InitializeAVBOITResources()
{
    CLodAlphaVariant::InitializeAVBOITResources(*this);
}

void CLodExtension::InitializeShadowResources()
{
    CLodShadowVariant::InitializeResources(*this);
}

void CLodExtension::TagCoreResourceUsages()
{
    auto tagBufferUsage = [](const std::shared_ptr<Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            rg::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };

    tagBufferUsage(m_visibleClustersBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_visibleClusterTransformIndicesBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_histogramIndirectCommand, "Cluster LOD rasterization");
    tagBufferUsage(m_histogramIndirectCommandPhase2, "Cluster LOD rasterization");
    tagBufferUsage(m_histogramIndirectCommandSw, "Cluster LOD rasterization");
    tagBufferUsage(m_histogramIndirectCommandPhase2Sw, "Cluster LOD rasterization");
    tagBufferUsage(m_histogramIndirectCommandPageJob, "Cluster LOD rasterization");
    tagBufferUsage(m_histogramIndirectCommandPhase2PageJob, "Cluster LOD rasterization");
    tagBufferUsage(m_histogramIndirectCommandReyes, "Cluster LOD rasterization");
    tagBufferUsage(m_histogramIndirectCommandPhase2Reyes, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsHistogramBuffer, "Cluster LOD rasterization");
    tagBufferUsage(m_visibleClustersCounterBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_workGraphTelemetryBuffer, "Cluster LOD telemetry");
    tagBufferUsage(m_occlusionReplayBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_occlusionReplayStateBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_occlusionNodeGpuInputsBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_viewDepthSrvIndicesBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_viewDepthSrvIndicesBufferPhase2, "Cluster LOD visibility");
    tagBufferUsage(m_rasterBucketsOffsetsBuffer, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsBlockSumsBuffer, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsScannedBlockSumsBuffer, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsTotalCountBuffer, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsTotalCountBufferPhase1, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsTotalCountBufferPhase1Sw, "Cluster LOD rasterization");
    tagBufferUsage(m_compactedVisibleClustersBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_compactedVisibleClustersBufferSw, "Cluster LOD visibility");
    tagBufferUsage(m_compactedVisibleClusterTransformIndicesBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_compactedVisibleClusterTransformIndicesBufferSw, "Cluster LOD visibility");
    tagBufferUsage(m_rasterBucketsWriteCursorBuffer, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsIndirectArgsBuffer, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsIndirectArgsBufferPhase2, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsIndirectArgsBufferSw, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsIndirectArgsBufferPhase2Sw, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsIndirectArgsBufferPageJob, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsIndirectArgsBufferPhase2PageJob, "Cluster LOD rasterization");
    tagBufferUsage(m_visibleClustersCounterBufferPhase2, "Cluster LOD visibility");
    tagBufferUsage(m_rasterBucketsHistogramBufferPhase2, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsHistogramBufferSw, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsHistogramBufferPhase2Sw, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsWriteCursorBufferPhase2, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsWriteCursorBufferSw, "Cluster LOD rasterization");
    tagBufferUsage(m_rasterBucketsWriteCursorBufferPhase2Sw, "Cluster LOD rasterization");
    tagBufferUsage(m_swVisibleClustersCounterBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_swVisibleClustersCounterBufferPhase2, "Cluster LOD visibility");
    tagBufferUsage(m_voxelRasterWorkBuffer, "Cluster LOD voxel rasterization");
    tagBufferUsage(m_voxelRasterWorkCounterBuffer, "Cluster LOD voxel rasterization");
    tagBufferUsage(m_skinnedVoxelRasterWorkBuffer, "Cluster LOD voxel rasterization");
    tagBufferUsage(m_skinnedVoxelRasterWorkCounterBuffer, "Cluster LOD voxel rasterization");
    tagBufferUsage(m_voxelRasterIndirectArgsBuffer, "Cluster LOD voxel rasterization");
    tagBufferUsage(m_sortedToUnsortedMappingBuffer, "Cluster LOD visibility");
    tagBufferUsage(m_sortedToUnsortedMappingBufferSw, "Cluster LOD visibility");
    tagBufferUsage(m_viewRasterInfoBuffer, "Cluster LOD rasterization");
}

void CLodExtension::TagShadowResourceUsages()
{
    CLodShadowVariant::TagResourceUsages(*this);
}

void CLodExtension::TagTransparencyResourceUsages()
{
    CLodAlphaVariant::TagResourceUsages(*this);
}

void CLodExtension::ReleaseBufferBackings()
{
    std::unordered_set<Buffer*> releasedBuffers;
    auto releaseBufferBacking = [&releasedBuffers](const std::shared_ptr<Buffer>& buffer) {
        if (buffer && releasedBuffers.insert(buffer.get()).second) {
            buffer->Dematerialize();
        }
    };

    releaseBufferBacking(m_visibleClustersBuffer);
    releaseBufferBacking(m_visibleClusterTransformIndicesBuffer);
    releaseBufferBacking(m_visibleClustersCounterBuffer);
    releaseBufferBacking(m_workGraphTelemetryBuffer);
    releaseBufferBacking(m_occlusionReplayBuffer);
    releaseBufferBacking(m_occlusionReplayStateBuffer);
    releaseBufferBacking(m_occlusionNodeGpuInputsBuffer);
    releaseBufferBacking(m_viewDepthSrvIndicesBuffer);
    releaseBufferBacking(m_viewDepthSrvIndicesBufferPhase2);
    releaseBufferBacking(m_histogramIndirectCommand);
    releaseBufferBacking(m_histogramIndirectCommandPhase2);
    releaseBufferBacking(m_histogramIndirectCommandSw);
    releaseBufferBacking(m_histogramIndirectCommandPhase2Sw);
    releaseBufferBacking(m_histogramIndirectCommandPageJob);
    releaseBufferBacking(m_histogramIndirectCommandPhase2PageJob);
    releaseBufferBacking(m_histogramIndirectCommandReyes);
    releaseBufferBacking(m_histogramIndirectCommandPhase2Reyes);
    releaseBufferBacking(m_rasterBucketsHistogramBuffer);
    releaseBufferBacking(m_rasterBucketsOffsetsBuffer);
    releaseBufferBacking(m_rasterBucketsBlockSumsBuffer);
    releaseBufferBacking(m_reyesPackedRasterWorkGroupsBuffer);
    releaseBufferBacking(m_rasterBucketsScannedBlockSumsBuffer);
    releaseBufferBacking(m_rasterBucketsTotalCountBuffer);
    releaseBufferBacking(m_rasterBucketsTotalCountBufferPhase1);
    releaseBufferBacking(m_rasterBucketsTotalCountBufferPhase1Sw);
    releaseBufferBacking(m_visibleClustersCounterBufferPhase2);
    releaseBufferBacking(m_rasterBucketsHistogramBufferPhase2);
    releaseBufferBacking(m_rasterBucketsWriteCursorBufferPhase2);
    releaseBufferBacking(m_reyesPackedRasterWorkGroupsBufferPhase2);
    releaseBufferBacking(m_rasterBucketsHistogramBufferSw);
    releaseBufferBacking(m_rasterBucketsHistogramBufferPhase2Sw);
    releaseBufferBacking(m_rasterBucketsWriteCursorBufferSw);
    releaseBufferBacking(m_rasterBucketsWriteCursorBufferPhase2Sw);
    releaseBufferBacking(m_compactedVisibleClustersBuffer);
    releaseBufferBacking(m_compactedVisibleClustersBufferSw);
    releaseBufferBacking(m_compactedVisibleClusterTransformIndicesBuffer);
    releaseBufferBacking(m_compactedVisibleClusterTransformIndicesBufferSw);
    releaseBufferBacking(m_rasterBucketsWriteCursorBuffer);
    releaseBufferBacking(m_rasterBucketsIndirectArgsBuffer);
    releaseBufferBacking(m_rasterBucketsIndirectArgsBufferPhase2);
    releaseBufferBacking(m_rasterBucketsIndirectArgsBufferSw);
    releaseBufferBacking(m_rasterBucketsIndirectArgsBufferPhase2Sw);
    releaseBufferBacking(m_rasterBucketsIndirectArgsBufferPageJob);
    releaseBufferBacking(m_rasterBucketsIndirectArgsBufferPhase2PageJob);
    releaseBufferBacking(m_reyesFullClusterOutputsBuffer);
    releaseBufferBacking(m_reyesFullClusterOutputsCounterBuffer);
    releaseBufferBacking(m_reyesOwnedClustersBuffer);
    releaseBufferBacking(m_reyesOwnedClustersCounterBuffer);
    releaseBufferBacking(m_reyesOwnershipBitsetBuffer);
    releaseBufferBacking(m_reyesOwnershipBitsetBufferPhase2);
    releaseBufferBacking(m_reyesClassifyIndirectArgsBuffer);
    releaseBufferBacking(m_reyesClassifyIndirectArgsBufferPhase2);
    releaseBufferBacking(m_reyesSplitIndirectArgsBuffer);
    releaseBufferBacking(m_reyesSplitIndirectArgsBufferPhase2);
    releaseBufferBacking(m_reyesSplitQueueBufferA);
    releaseBufferBacking(m_reyesSplitQueueCounterBufferA);
    releaseBufferBacking(m_reyesSplitQueueOverflowBufferA);
    releaseBufferBacking(m_reyesSplitQueueBufferB);
    releaseBufferBacking(m_reyesSplitQueueCounterBufferB);
    releaseBufferBacking(m_reyesSplitQueueOverflowBufferB);
    releaseBufferBacking(m_reyesReplaySplitQueueBuffer);
    releaseBufferBacking(m_reyesReplaySplitQueueCounterBuffer);
    releaseBufferBacking(m_reyesReplaySplitQueueOverflowBuffer);
    releaseBufferBacking(m_reyesDiceQueueBuffer);
    releaseBufferBacking(m_reyesDiceQueueCounterBuffer);
    releaseBufferBacking(m_reyesDiceQueuePhase1CountBuffer);
    releaseBufferBacking(m_reyesDiceQueueOverflowBuffer);
    releaseBufferBacking(m_reyesReplayDiceQueueBuffer);
    releaseBufferBacking(m_reyesReplayDiceQueueCounterBuffer);
    releaseBufferBacking(m_reyesReplayDiceQueueOverflowBuffer);
    releaseBufferBacking(m_reyesRasterWorkBuffer);
    releaseBufferBacking(m_reyesRasterWorkCounterBuffer);
    releaseBufferBacking(m_reyesRasterWorkIndirectArgsBuffer);
    releaseBufferBacking(m_reyesCompactedRasterWorkIndicesBuffer);
    releaseBufferBacking(m_reyesPackedRasterWorkGroupsBuffer);
    releaseBufferBacking(m_reyesTessTableConfigsBuffer);
    releaseBufferBacking(m_reyesTessTableVerticesBuffer);
    releaseBufferBacking(m_reyesTessTableTrianglesBuffer);
    releaseBufferBacking(m_reyesDiceIndirectArgsBuffer);
    releaseBufferBacking(m_reyesDiceIndirectArgsBufferPhase2);
    releaseBufferBacking(m_reyesRasterWorkBufferPhase2);
    releaseBufferBacking(m_reyesRasterWorkCounterBufferPhase2);
    releaseBufferBacking(m_reyesRasterWorkIndirectArgsBufferPhase2);
    releaseBufferBacking(m_reyesCompactedRasterWorkIndicesBufferPhase2);
    releaseBufferBacking(m_reyesPackedRasterWorkGroupsBufferPhase2);
    releaseBufferBacking(m_reyesTelemetryBufferPhase1);
    releaseBufferBacking(m_reyesTelemetryBufferPhase2);
    releaseBufferBacking(m_swVisibleClustersCounterBuffer);
    releaseBufferBacking(m_swVisibleClustersCounterBufferPhase2);
    releaseBufferBacking(m_voxelRasterWorkBuffer);
    releaseBufferBacking(m_voxelRasterWorkCounterBuffer);
    releaseBufferBacking(m_skinnedVoxelRasterWorkBuffer);
    releaseBufferBacking(m_skinnedVoxelRasterWorkCounterBuffer);
    releaseBufferBacking(m_voxelRasterIndirectArgsBuffer);
    releaseBufferBacking(m_skinnedVoxelRasterIndirectArgsBuffer);
    releaseBufferBacking(m_sortedToUnsortedMappingBuffer);
    releaseBufferBacking(m_sortedToUnsortedMappingBufferSw);
    releaseBufferBacking(m_viewRasterInfoBuffer);
    releaseBufferBacking(m_shadowPageMetadataBuffer);
    releaseBufferBacking(m_shadowInvalidationInputsBuffer);
    releaseBufferBacking(m_shadowInvalidationCountBuffer);
    for (const auto& buffer : m_shadowUpgradeInvalidationUploadBuffers) {
        releaseBufferBacking(buffer);
    }
    releaseBufferBacking(m_shadowInvalidatedInstancesBitsetBuffer);
    releaseBufferBacking(m_shadowPredictiveInvalidationCandidatesBuffer);
    releaseBufferBacking(m_shadowPredictiveInvalidationCandidateCountBuffer);
    releaseBufferBacking(m_shadowPredictiveRawPagesBuffer);
    releaseBufferBacking(m_shadowPredictiveRawPageCountBuffer);
    releaseBufferBacking(m_shadowPredictedInvalidationScratchBitsetBuffer);
    releaseBufferBacking(m_shadowPredictedInvalidationPagesBufferA);
    releaseBufferBacking(m_shadowPredictedInvalidationPageCountBufferA);
    releaseBufferBacking(m_shadowAllocationRequestsBuffer);
    releaseBufferBacking(m_shadowAllocationCountBuffer);
    releaseBufferBacking(m_shadowAllocationIndirectArgsBuffer);
    releaseBufferBacking(m_shadowMarkTileWorkBuffer);
    releaseBufferBacking(m_shadowMarkTileCountBuffer);
    releaseBufferBacking(m_shadowMarkTileIndirectArgsBuffer);
    releaseBufferBacking(m_shadowMarkedBlocksMaskBuffer);
    releaseBufferBacking(m_shadowMarkedBlocksListBuffer);
    releaseBufferBacking(m_shadowMarkedBlocksCountBuffer);
    releaseBufferBacking(m_shadowActiveBlockMetadataBuffer);
    releaseBufferBacking(m_shadowBlockClusterCoverageBuffer);
    releaseBufferBacking(m_shadowFreePhysicalPagesBuffer);
    releaseBufferBacking(m_shadowReusablePhysicalPagesBuffer);
    releaseBufferBacking(m_shadowPageListHeaderBuffer);
    releaseBufferBacking(m_shadowDirtyPageFlagsBuffer);
    releaseBufferBacking(m_shadowClipmapInfoBuffer);
    releaseBufferBacking(m_shadowMarkClipmapDataBuffer);
    releaseBufferBacking(m_shadowCompactMainCameraBuffer);
    releaseBufferBacking(m_shadowCompactShadowCameraBuffer);
    releaseBufferBacking(m_shadowDirectionalPageViewInfoBuffer);
    releaseBufferBacking(m_shadowRuntimeStateBuffer);
    releaseBufferBacking(m_shadowStatsBuffer);
    releaseBufferBacking(m_swPageJobVisibleClustersBuffer);
    releaseBufferBacking(m_swPageJobVisibleClusterTransformIndicesBuffer);
    releaseBufferBacking(m_swPageJobVisibleClustersCounterBuffer);
    releaseBufferBacking(m_swPageJobVisibleClustersBufferPhase2);
    releaseBufferBacking(m_swPageJobVisibleClusterTransformIndicesBufferPhase2);
    releaseBufferBacking(m_swPageJobVisibleClustersCounterBufferPhase2);
    releaseBufferBacking(m_swPageJobRecordsBuffer);
    releaseBufferBacking(m_swPageJobRecordsBufferSkinned);
    releaseBufferBacking(m_swPageJobCountBuffer);
    releaseBufferBacking(m_swPageJobCountBufferSkinned);
    releaseBufferBacking(m_swPageJobRecordsBufferPhase2);
    releaseBufferBacking(m_swPageJobRecordsBufferPhase2Skinned);
    releaseBufferBacking(m_swPageJobCountBufferPhase2);
    releaseBufferBacking(m_swPageJobCountBufferPhase2Skinned);
    releaseBufferBacking(m_swPageJobIndirectArgsBuffer);
    releaseBufferBacking(m_swPageJobIndirectArgsBufferSkinned);
    releaseBufferBacking(m_swPageJobIndirectArgsBufferPhase2);
    releaseBufferBacking(m_swPageJobIndirectArgsBufferPhase2Skinned);
    releaseBufferBacking(m_swPageJobClusterTagsBuffer);
    releaseBufferBacking(m_swPageJobClusterTagsBufferPhase2);
    releaseBufferBacking(m_vsmExpandedVisibleClustersBuffer);
    releaseBufferBacking(m_vsmExpandedVisibleClustersBufferSw);
    releaseBufferBacking(m_vsmExpandedVisibleClusterTransformIndicesBufferSw);
}

void CLodExtension::ReleaseTransparencyResourceBackings()
{
    CLodAlphaVariant::ReleaseResourceBackings(*this);
}

void CLodExtension::ReleaseShadowResourceBackings()
{
    CLodShadowVariant::ReleaseResourceBackings(*this);
}

void CLodExtension::SyncReyesResourceEntities(bool enabled)
{
    if (!m_reyesFullClusterOutputsBuffer) {
        return;
    }

    const auto& traits = GetVariantTraits(m_type);
    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    const flecs::entity typeEntity = traits.ensureTypeEntity(ecsWorld);

    SyncTaggedBufferEntity<CLodReyesFullClustersCounterTag>(m_reyesFullClusterOutputsCounterBuffer, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesSplitQueueCounterATag>(m_reyesSplitQueueCounterBufferA, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesSplitQueueOverflowATag>(m_reyesSplitQueueOverflowBufferA, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesSplitQueueCounterBTag>(m_reyesSplitQueueCounterBufferB, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesSplitQueueOverflowBTag>(m_reyesSplitQueueOverflowBufferB, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesDiceQueueTag>(m_reyesDiceQueueBuffer, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesTessTableConfigsTag>(m_reyesTessTableConfigsBuffer, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesTessTableVerticesTag>(m_reyesTessTableVerticesBuffer, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesTessTableTrianglesTag>(m_reyesTessTableTrianglesBuffer, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesDiceQueueCounterTag>(m_reyesDiceQueueCounterBuffer, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesDiceQueueOverflowTag>(m_reyesDiceQueueOverflowBuffer, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesTelemetryBufferPhase1Tag>(m_reyesTelemetryBufferPhase1, typeEntity, enabled);
    SyncTaggedBufferEntity<CLodReyesTelemetryBufferPhase2Tag>(m_reyesTelemetryBufferPhase2, typeEntity, enabled);
}

void CLodExtension::EnsureReyesResourcesInitialized()
{
    if (m_reyesFullClusterOutputsBuffer) {
        SyncReyesResourceEntities(true);
        return;
    }

    auto tagBufferUsage = [](const std::shared_ptr<Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            rg::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };

    const auto& traits = GetVariantTraits(m_type);
    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    const flecs::entity typeEntity = traits.ensureTypeEntity(ecsWorld);
    const ReyesResourceSizing reyesResourceSizing = BuildReyesResourceSizing(traits, m_visibleClusterCapacity);
    const bool usesPhase2ReyesResources = traits.usesPhase2OcclusionReplay;
    m_reyesFullClusterOutputCapacity = reyesResourceSizing.fullClusterOutputCapacity;
    m_reyesOwnedClusterCapacity = reyesResourceSizing.ownedClusterCapacity;
    m_reyesSplitQueueCapacity = reyesResourceSizing.splitQueueCapacity;
    m_reyesDiceQueueCapacity = reyesResourceSizing.diceQueueCapacity;
    m_reyesDiceQueuePhysicalCapacity = reyesResourceSizing.diceQueuePhysicalCapacity;
    m_reyesRasterWorkCapacity = reyesResourceSizing.rasterWorkCapacity;
    m_reyesOwnershipBitsetWordCount = reyesResourceSizing.ownershipBitsetWordCount;
    m_reyesRequestedBudgetBytes = reyesResourceSizing.requestedBudgetBytes;
    m_reyesAllocatedBudgetBytes = reyesResourceSizing.allocatedBudgetBytes;
    m_reyesBudgetLimited = reyesResourceSizing.budgetLimited;

    m_reyesFullClusterOutputsBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesFullClusterOutputCapacity, sizeof(CLodReyesFullClusterOutput), true, false, false, true);
    m_reyesFullClusterOutputsBuffer->SetName(MakeVariantResourceName(traits, "Reyes Full Cluster Outputs Buffer"));

    m_reyesFullClusterOutputsCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesFullClusterOutputsCounterBuffer->SetName(MakeVariantResourceName(traits, "Reyes Full Cluster Outputs Counter Buffer"));
    m_reyesFullClusterOutputsCounterBuffer->GetECSEntity()
        .set<Components::Resource>({ m_reyesFullClusterOutputsCounterBuffer })
        .add<CLodReyesFullClustersCounterTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesOwnedClustersBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesOwnedClusterCapacity, sizeof(CLodReyesOwnedClusterEntry), true, false, false, true);
    m_reyesOwnedClustersBuffer->SetName(MakeVariantResourceName(traits, "Reyes Owned Clusters Buffer"));

    m_reyesOwnedClustersCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesOwnedClustersCounterBuffer->SetName(MakeVariantResourceName(traits, "Reyes Owned Clusters Counter Buffer"));

    m_reyesOwnershipBitsetBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesOwnershipBitsetWordCount, sizeof(uint32_t), true, false, false, true);
    m_reyesOwnershipBitsetBuffer->SetName(MakeVariantResourceName(traits, "Reyes Ownership Bitset Buffer"));

    if (usesPhase2ReyesResources) {
        m_reyesOwnershipBitsetBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(m_reyesOwnershipBitsetWordCount, sizeof(uint32_t), true, false, false, true);
        m_reyesOwnershipBitsetBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Ownership Bitset Buffer Phase2"));
    }

    m_reyesClassifyIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
    m_reyesClassifyIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Reyes Classify Indirect Args Buffer"));

    if (usesPhase2ReyesResources) {
        m_reyesClassifyIndirectArgsBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
        m_reyesClassifyIndirectArgsBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Classify Indirect Args Buffer Phase2"));
    }

    m_reyesSplitIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
    m_reyesSplitIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Reyes Split Indirect Args Buffer"));

    if (usesPhase2ReyesResources) {
        m_reyesSplitIndirectArgsBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
        m_reyesSplitIndirectArgsBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Split Indirect Args Buffer Phase2"));
    }

    m_reyesSplitQueueBufferA = CreateAliasedUnmaterializedStructuredBuffer(m_reyesSplitQueueCapacity, sizeof(CLodReyesSplitQueueEntry), true, false, true);
    m_reyesSplitQueueBufferA->SetName(MakeVariantResourceName(traits, "Reyes Split Queue Buffer A"));

    m_reyesSplitQueueCounterBufferA = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesSplitQueueCounterBufferA->SetName(MakeVariantResourceName(traits, "Reyes Split Queue Counter Buffer A"));
    m_reyesSplitQueueCounterBufferA->GetECSEntity()
        .set<Components::Resource>({ m_reyesSplitQueueCounterBufferA })
        .add<CLodReyesSplitQueueCounterATag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesSplitQueueOverflowBufferA = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesSplitQueueOverflowBufferA->SetName(MakeVariantResourceName(traits, "Reyes Split Queue Overflow Buffer A"));
    m_reyesSplitQueueOverflowBufferA->GetECSEntity()
        .set<Components::Resource>({ m_reyesSplitQueueOverflowBufferA })
        .add<CLodReyesSplitQueueOverflowATag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesSplitQueueBufferB = CreateAliasedUnmaterializedStructuredBuffer(m_reyesSplitQueueCapacity, sizeof(CLodReyesSplitQueueEntry), true, false, true);
    m_reyesSplitQueueBufferB->SetName(MakeVariantResourceName(traits, "Reyes Split Queue Buffer B"));

    m_reyesSplitQueueCounterBufferB = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesSplitQueueCounterBufferB->SetName(MakeVariantResourceName(traits, "Reyes Split Queue Counter Buffer B"));
    m_reyesSplitQueueCounterBufferB->GetECSEntity()
        .set<Components::Resource>({ m_reyesSplitQueueCounterBufferB })
        .add<CLodReyesSplitQueueCounterBTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesSplitQueueOverflowBufferB = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesSplitQueueOverflowBufferB->SetName(MakeVariantResourceName(traits, "Reyes Split Queue Overflow Buffer B"));
    m_reyesSplitQueueOverflowBufferB->GetECSEntity()
        .set<Components::Resource>({ m_reyesSplitQueueOverflowBufferB })
        .add<CLodReyesSplitQueueOverflowBTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesReplaySplitQueueBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesSplitQueueCapacity, sizeof(CLodReyesSplitQueueEntry), true, false, true);
    m_reyesReplaySplitQueueBuffer->SetName(MakeVariantResourceName(traits, "Reyes Replay Split Queue Buffer"));

    m_reyesReplaySplitQueueCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesReplaySplitQueueCounterBuffer->SetName(MakeVariantResourceName(traits, "Reyes Replay Split Queue Counter Buffer"));

    m_reyesReplaySplitQueueOverflowBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesReplaySplitQueueOverflowBuffer->SetName(MakeVariantResourceName(traits, "Reyes Replay Split Queue Overflow Buffer"));

    m_reyesDiceQueueBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesDiceQueuePhysicalCapacity, sizeof(CLodReyesDiceQueueEntry), true, false, true);
    m_reyesDiceQueueBuffer->SetName(MakeVariantResourceName(traits, "Reyes Dice Queue Buffer"));
    m_reyesDiceQueueBuffer->GetECSEntity()
        .set<Components::Resource>({ m_reyesDiceQueueBuffer })
        .add<CLodReyesDiceQueueTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    const ReyesTessellationTableData& reyesTessellationTableData = GetReyesTessellationTableData();
    m_reyesTessTableConfigsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        static_cast<uint32_t>(reyesTessellationTableData.configs.size()),
        sizeof(CLodReyesTessTableConfigEntry),
        false,
        false,
        false,
        false);
    m_reyesTessTableConfigsBuffer->SetName(MakeVariantResourceName(traits, "Reyes Tess Table Configs Buffer"));
    m_reyesTessTableConfigsBuffer->GetECSEntity()
        .set<Components::Resource>({ m_reyesTessTableConfigsBuffer })
        .add<CLodReyesTessTableConfigsTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesTessTableVerticesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        static_cast<uint32_t>(reyesTessellationTableData.vertices.size()),
        sizeof(uint32_t),
        false,
        false,
        false,
        false);
    m_reyesTessTableVerticesBuffer->SetName(MakeVariantResourceName(traits, "Reyes Tess Table Vertices Buffer"));
    m_reyesTessTableVerticesBuffer->GetECSEntity()
        .set<Components::Resource>({ m_reyesTessTableVerticesBuffer })
        .add<CLodReyesTessTableVerticesTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesTessTableTrianglesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        static_cast<uint32_t>(reyesTessellationTableData.triangles.size()),
        sizeof(uint32_t),
        false,
        false,
        false,
        false);
    m_reyesTessTableTrianglesBuffer->SetName(MakeVariantResourceName(traits, "Reyes Tess Table Triangles Buffer"));
    m_reyesTessTableTrianglesBuffer->GetECSEntity()
        .set<Components::Resource>({ m_reyesTessTableTrianglesBuffer })
        .add<CLodReyesTessTableTrianglesTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesDiceQueueCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesDiceQueueCounterBuffer->SetName(MakeVariantResourceName(traits, "Reyes Dice Queue Counter Buffer"));
    m_reyesDiceQueueCounterBuffer->GetECSEntity()
        .set<Components::Resource>({ m_reyesDiceQueueCounterBuffer })
        .add<CLodReyesDiceQueueCounterTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    if (usesPhase2ReyesResources) {
        m_reyesDiceQueuePhase1CountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), false, false, false);
        m_reyesDiceQueuePhase1CountBuffer->SetName(MakeVariantResourceName(traits, "Reyes Dice Queue Phase1 Count Buffer"));
    }

    m_reyesDiceQueueOverflowBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesDiceQueueOverflowBuffer->SetName(MakeVariantResourceName(traits, "Reyes Dice Queue Overflow Buffer"));
    m_reyesDiceQueueOverflowBuffer->GetECSEntity()
        .set<Components::Resource>({ m_reyesDiceQueueOverflowBuffer })
        .add<CLodReyesDiceQueueOverflowTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    m_reyesReplayDiceQueueBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesDiceQueueCapacity, sizeof(CLodReyesDiceQueueEntry), true, false, true);
    m_reyesReplayDiceQueueBuffer->SetName(MakeVariantResourceName(traits, "Reyes Replay Dice Queue Buffer"));

    m_reyesReplayDiceQueueCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesReplayDiceQueueCounterBuffer->SetName(MakeVariantResourceName(traits, "Reyes Replay Dice Queue Counter Buffer"));

    m_reyesReplayDiceQueueOverflowBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesReplayDiceQueueOverflowBuffer->SetName(MakeVariantResourceName(traits, "Reyes Replay Dice Queue Overflow Buffer"));

    m_reyesRasterWorkBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesRasterWorkCapacity, sizeof(CLodReyesRasterWorkEntry), true, false, false, true);
    m_reyesRasterWorkBuffer->SetName(MakeVariantResourceName(traits, "Reyes Raster Work Buffer"));

    m_reyesRasterWorkCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    m_reyesRasterWorkCounterBuffer->SetName(MakeVariantResourceName(traits, "Reyes Raster Work Counter Buffer"));

    m_reyesRasterWorkIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
    m_reyesRasterWorkIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Reyes Raster Work Indirect Args Buffer"));

    m_reyesCompactedRasterWorkIndicesBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesRasterWorkCapacity, sizeof(uint32_t), true, false, false, true);
    m_reyesCompactedRasterWorkIndicesBuffer->SetName(MakeVariantResourceName(traits, "Reyes Compacted Raster Work Indices Buffer"));

    m_reyesPackedRasterWorkGroupsBuffer = CreateAliasedUnmaterializedStructuredBuffer(m_reyesRasterWorkCapacity, sizeof(CLodReyesPackedRasterWorkGroupEntry), true, false, false, true);
    m_reyesPackedRasterWorkGroupsBuffer->SetName(MakeVariantResourceName(traits, "Reyes Packed Raster Work Groups Buffer"));

    if (usesPhase2ReyesResources) {
        m_reyesRasterWorkBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(m_reyesRasterWorkCapacity, sizeof(CLodReyesRasterWorkEntry), true, false, false, true);
        m_reyesRasterWorkBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Raster Work Buffer Phase2"));

        m_reyesRasterWorkCounterBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
        m_reyesRasterWorkCounterBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Raster Work Counter Buffer Phase2"));

        m_reyesRasterWorkIndirectArgsBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
        m_reyesRasterWorkIndirectArgsBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Raster Work Indirect Args Buffer Phase2"));

        m_reyesCompactedRasterWorkIndicesBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(m_reyesRasterWorkCapacity, sizeof(uint32_t), true, false, false, true);
        m_reyesCompactedRasterWorkIndicesBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Compacted Raster Work Indices Buffer Phase2"));

        m_reyesPackedRasterWorkGroupsBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(m_reyesRasterWorkCapacity, sizeof(CLodReyesPackedRasterWorkGroupEntry), true, false, false, true);
        m_reyesPackedRasterWorkGroupsBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Packed Raster Work Groups Buffer Phase2"));
    }

    m_reyesDiceIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
    m_reyesDiceIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Reyes Dice Indirect Args Buffer"));

    if (usesPhase2ReyesResources) {
        m_reyesDiceIndirectArgsBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
        m_reyesDiceIndirectArgsBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Dice Indirect Args Buffer Phase2"));
    }

    m_reyesTelemetryBufferPhase1 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesTelemetry), true, false, false, false);
    m_reyesTelemetryBufferPhase1->SetName(MakeVariantResourceName(traits, "Reyes Telemetry Buffer Phase1"));
    m_reyesTelemetryBufferPhase1->GetECSEntity()
        .set<Components::Resource>({ m_reyesTelemetryBufferPhase1 })
        .add<CLodReyesTelemetryBufferPhase1Tag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    if (usesPhase2ReyesResources) {
        m_reyesTelemetryBufferPhase2 = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesTelemetry), true, false, false, false);
        m_reyesTelemetryBufferPhase2->SetName(MakeVariantResourceName(traits, "Reyes Telemetry Buffer Phase2"));
        m_reyesTelemetryBufferPhase2->GetECSEntity()
            .set<Components::Resource>({ m_reyesTelemetryBufferPhase2 })
            .add<CLodReyesTelemetryBufferPhase2Tag>()
            .add<CLodExtensionTypeTag>(typeEntity);
    }

    tagBufferUsage(m_reyesFullClusterOutputsBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesFullClusterOutputsCounterBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesOwnedClustersBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesOwnedClustersCounterBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesOwnershipBitsetBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesOwnershipBitsetBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesClassifyIndirectArgsBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesClassifyIndirectArgsBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitIndirectArgsBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitIndirectArgsBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitQueueBufferA, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitQueueCounterBufferA, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitQueueOverflowBufferA, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitQueueBufferB, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitQueueCounterBufferB, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesSplitQueueOverflowBufferB, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesReplaySplitQueueBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesReplaySplitQueueCounterBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesReplaySplitQueueOverflowBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesDiceQueueBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesTessTableConfigsBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesTessTableVerticesBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesTessTableTrianglesBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesDiceQueueCounterBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesDiceQueuePhase1CountBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesDiceQueueOverflowBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesReplayDiceQueueBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesReplayDiceQueueCounterBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesReplayDiceQueueOverflowBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesRasterWorkBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesRasterWorkCounterBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesRasterWorkIndirectArgsBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesCompactedRasterWorkIndicesBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesPackedRasterWorkGroupsBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesRasterWorkBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesRasterWorkCounterBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesRasterWorkIndirectArgsBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesCompactedRasterWorkIndicesBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesPackedRasterWorkGroupsBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesDiceIndirectArgsBuffer, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesDiceIndirectArgsBufferPhase2, "Cluster LOD Reyes");
    tagBufferUsage(m_reyesTelemetryBufferPhase1, "Cluster LOD telemetry");
    tagBufferUsage(m_reyesTelemetryBufferPhase2, "Cluster LOD telemetry");
}

CLodExtension::CLodExtension(
    CLodExtensionType type,
    uint32_t maxVisibleClusters,
    CLodExtensionOptions options)
    : m_type(type)
    , m_options(options)
    , m_maxVisibleClusters(maxVisibleClusters) {
    const auto& traits = GetVariantTraits(type);

    m_streamingSystem = std::move(m_options.streamingSystem);
    if (traits.ownsStreaming && !m_streamingSystem) {
        m_streamingSystem = std::make_shared<CLodStreamingSystem>();
    }

    if (m_type == CLodExtensionType::Shadow) {
        RefreshShadowConfiguredSettings();
    }
    m_visibleClusterCapacity = GetVisibleClusterCapacity();

    InitializeCoreResources();

    if (!IsReyesTessellationDisabled()) {
        EnsureReyesResourcesInitialized();
    }

    InitializeDeepVisibilityResources();
    InitializeAVBOITResources();
    InitializeShadowResources();
    TagCoreResourceUsages();
    TagTransparencyResourceUsages();
    TagShadowResourceUsages();
}

void CLodExtension::RefreshTransparencyResourcesForCurrentSettings()
{
    CLodAlphaVariant::RefreshResourcesForCurrentSettings(*this);
}

void CLodExtension::RefreshShadowResourcesForCurrentSettings()
{
    CLodShadowVariant::RefreshResourcesForCurrentSettings(*this);
}

void CLodExtension::PrepareForBuild(RenderGraph& rg)
{
    RefreshTransparencyResourcesForCurrentSettings();
    RefreshShadowResourcesForCurrentSettings();

    if (m_type == CLodExtensionType::Shadow && !m_providerRegisteredForCurrentRegistry) {
        rg.RegisterProvider(this);
        m_providerRegisteredForCurrentRegistry = true;
    }
}

void CLodExtension::Initialize(RenderGraph& rg)
{
    PrepareForBuild(rg);

    if (GetVariantTraits(m_type).ownsStreaming && m_streamingSystem) {
        m_streamingSystem->Initialize(rg);
    }
}

void CLodExtension::Shutdown(RenderGraph& rg)
{
    (void)rg;
    if (GetVariantTraits(m_type).ownsStreaming && m_streamingSystem) {
        m_streamingSystem->ShutdownGraphResources();
    }
}

void CLodExtension::OnRegistryReset(ResourceRegistry* reg)
{
    m_providerRegisteredForCurrentRegistry = false;
    ReleaseBufferBackings();
    ReleaseTransparencyResourceBackings();
    ReleaseShadowResourceBackings();
    m_shadowVirtualResourcesNeedReset = true;

    SyncReyesResourceEntities(!IsReyesTessellationDisabled());

    if (GetVariantTraits(m_type).ownsStreaming && m_streamingSystem) {
        m_streamingSystem->OnRegistryReset(reg);
    }
}

std::shared_ptr<Resource> CLodExtension::ProvideResource(ResourceIdentifier const& key)
{
    return CLodShadowVariant::ProvideResource(*this, key);
}

std::vector<ResourceIdentifier> CLodExtension::GetSupportedKeys()
{
    return CLodShadowVariant::GetSupportedKeys(*this);
}

void CLodExtension::GatherStructuralPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    PrepareForBuild(rg);

    const auto& traits = GetVariantTraits(m_type);
    if (traits.ownsStreaming && m_streamingSystem) {
        m_streamingSystem->GatherStructuralPasses(rg, outPasses);
    }

    bool streamingTailAppended = false;
    const auto appendStreamingTailPasses = [&]() {
        if (traits.ownsStreaming && m_streamingSystem && !streamingTailAppended) {
            // Keep after-present streaming passes at the extension tail. If they
            // are emitted before CLod raster passes, extension-local chaining can
            // create Present -> streaming tail -> CLod raster -> Present cycles.
            m_streamingSystem->GatherStructuralTailPasses(rg, outPasses);
            streamingTailAppended = true;
        }
    };

    if (!traits.schedulesStructuralPasses) {
        appendStreamingTailPasses();
        return;
    }

    const size_t techniqueTaggedPassStart = outPasses.size();
    const auto applyTechniqueTags = [&]() {
        for (size_t passIndex = techniqueTaggedPassStart; passIndex < outPasses.size(); ++passIndex) {
            auto& passDesc = outPasses[passIndex];
            if (passDesc.techniquePath.empty()) {
                passDesc.Technique(GetVariantTechniquePath(traits, passDesc.name));
            }
        }
    };

    // Snapshot the setting-driven scheduling policy up front so the rest of the
    // function can read as orchestration rather than a sequence of settings lookups.
    const StructuralSchedulingPolicy schedulingPolicy =
        BuildStructuralSchedulingPolicy(traits, m_type, m_options.enableReyes);
    if (traits.type == CLodExtensionType::AlphaBlend && schedulingPolicy.transparencyMode == CLodTransparencyMode::Disabled) {
        applyTechniqueTags();
        appendStreamingTailPasses();
        return;
    }
    if (!schedulingPolicy.disableReyesTessellation) {
        EnsureReyesResourcesInitialized();
    }
    SyncReyesResourceEntities(schedulingPolicy.useReyesForThisVariant);
    const uint32_t reyesSplitQueueCapacity = m_reyesSplitQueueCapacity;
    const uint32_t reyesDiceQueueCapacity = m_reyesDiceQueueCapacity;
    const uint32_t reyesRasterWorkCapacity = m_reyesRasterWorkCapacity;
    const auto transparencyMode = schedulingPolicy.transparencyMode;
    const bool disableReyesTessellation = schedulingPolicy.disableReyesTessellation;
    const bool useComputeSWRaster = schedulingPolicy.useComputeSWRaster;
    const bool useShadowPageJob = schedulingPolicy.useShadowPageJob;
    const bool useShadowReyesRouting = schedulingPolicy.useShadowReyesRouting;
    const bool useReyesForThisVariant = schedulingPolicy.useReyesForThisVariant;
    const bool useWorkGraphReyesVisibility = schedulingPolicy.useWorkGraphReyesVisibility;
    const auto workGraphMode = schedulingPolicy.workGraphMode;
    const auto cullingBackend = schedulingPolicy.cullingBackend;
    const auto renderPhase = schedulingPolicy.renderPhase;
    const bool enablePhase2OcclusionReplay = traits.usesPhase2OcclusionReplay &&
        SettingsManager::GetInstance().getSettingGetter<bool>("enableOcclusionCulling")();
    const std::shared_ptr<Buffer> reyesOwnershipBitsetBuffer =
        useReyesForThisVariant ? m_reyesOwnershipBitsetBuffer : nullptr;
    const std::shared_ptr<Buffer> reyesOwnershipBitsetBufferPhase2 =
        useReyesForThisVariant ? m_reyesOwnershipBitsetBufferPhase2 : nullptr;

    std::string shadowNonRasterableHierarchyPassName;
    std::string shadowClearDirtyBitsAfterPassName;
    shadowNonRasterableHierarchyPassName = CLodShadowVariant::AppendStructuralPrelude(*this, traits, outPasses);

    std::shared_ptr<ResourceGroup> slabGroup = GetSlabResourceGroup();
    const auto appendHierarchicalCullingPass = [&](uint32_t phaseIndex, const std::string& afterPassName) {
        const bool isPhase1 = phaseIndex == 1u;
        HierarchicalCullingPassInputs cullPassInputs;
        cullPassInputs.isFirstPass = isPhase1;
        cullPassInputs.maxVisibleClusters = m_visibleClusterCapacity;
        cullPassInputs.backend = cullingBackend;
        cullPassInputs.workGraphMode = workGraphMode;
        cullPassInputs.workGraphReyesVisibility = useWorkGraphReyesVisibility;
        cullPassInputs.renderPhase = renderPhase;
        cullPassInputs.clodOnlyWorkloads = true;
        cullPassInputs.useShadowCascadeViews = (traits.type == CLodExtensionType::Shadow);
        cullPassInputs.rasterOutputKind = traits.rasterOutputKind;

        const std::string cullPassName = MakeVariantPassName(traits, isPhase1 ? "HierarchicalCullingPass1" : "HierarchicalCullingPass2");
        const bool useDispatchCullingPass = UseHierarchicalDispatchCullingPass(
            cullingBackend,
            cullPassInputs.workGraphMode,
            cullPassInputs.rasterOutputKind);
        const std::shared_ptr<Buffer> visibleClustersCounterBuffer =
            isPhase1 ? m_visibleClustersCounterBuffer : m_visibleClustersCounterBufferPhase2;
        const std::shared_ptr<Buffer> histogramIndirectCommand =
            isPhase1 ? m_histogramIndirectCommand : m_histogramIndirectCommandPhase2;
        const std::shared_ptr<Buffer> swVisibleClustersCounterBuffer =
            isPhase1 ? m_swVisibleClustersCounterBuffer : m_swVisibleClustersCounterBufferPhase2;
        const std::shared_ptr<Buffer> swPageJobVisibleClustersBuffer =
            isPhase1 ? m_swPageJobVisibleClustersBuffer : m_swPageJobVisibleClustersBufferPhase2;
        const std::shared_ptr<Buffer> swPageJobVisibleClusterTransformIndicesBuffer =
            isPhase1 ? m_swPageJobVisibleClusterTransformIndicesBuffer : m_swPageJobVisibleClusterTransformIndicesBufferPhase2;
        const std::shared_ptr<Buffer> swPageJobVisibleClustersCounterBuffer =
            isPhase1 ? m_swPageJobVisibleClustersCounterBuffer : m_swPageJobVisibleClustersCounterBufferPhase2;
        const std::shared_ptr<Buffer> previousVisibleClustersCounterBuffer =
            isPhase1 ? std::shared_ptr<Buffer>{} : m_visibleClustersCounterBuffer;
        const std::shared_ptr<Buffer> previousSwVisibleClustersCounterBuffer =
            isPhase1 ? std::shared_ptr<Buffer>{} : m_swVisibleClustersCounterBuffer;

        std::shared_ptr<ComputePass> cullPass = useDispatchCullingPass
            ? std::static_pointer_cast<ComputePass>(
                std::make_shared<HierarchicalDispatchCullingPass>(
                    cullPassName,
                    cullPassInputs,
                    m_visibleClustersBuffer,
                    m_visibleClusterTransformIndicesBuffer,
                    visibleClustersCounterBuffer,
                    swVisibleClustersCounterBuffer,
                    m_voxelRasterWorkBuffer,
                    m_voxelRasterWorkCounterBuffer,
                    m_skinnedVoxelRasterWorkBuffer,
                    m_skinnedVoxelRasterWorkCounterBuffer,
                    m_voxelRasterWorkCapacity,
                    traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? swPageJobVisibleClustersBuffer : nullptr,
                    traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? swPageJobVisibleClusterTransformIndicesBuffer : nullptr,
                    traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? swPageJobVisibleClustersCounterBuffer : nullptr,
                    histogramIndirectCommand,
                    m_workGraphTelemetryBuffer,
                    m_occlusionReplayBuffer,
                    m_occlusionReplayStateBuffer,
                    m_occlusionNodeGpuInputsBuffer,
                    isPhase1 ? m_viewDepthSrvIndicesBuffer : m_viewDepthSrvIndicesBufferPhase2,
                    m_viewRasterInfoBuffer,
                    traits.type == CLodExtensionType::Shadow ? m_shadowDirtyPageHierarchyTexture : nullptr,
                    slabGroup,
                    previousVisibleClustersCounterBuffer,
                    previousSwVisibleClustersCounterBuffer,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPredictiveInvalidationCandidatesBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPredictiveInvalidationCandidateCountBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowInvalidatedInstancesBitsetBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPageTableTexture : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowStaticPhysicalPagesTexture : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowActiveBlockMetadataBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPhysicalPagesTexture : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowDynamicActiveBlockMetadataBuffer : nullptr))
            : std::static_pointer_cast<ComputePass>(
                std::make_shared<HierarchicalCullingPass>(
                    cullPassName,
                    cullPassInputs,
                    m_visibleClustersBuffer,
                    m_visibleClusterTransformIndicesBuffer,
                    visibleClustersCounterBuffer,
                    swVisibleClustersCounterBuffer,
                    m_voxelRasterWorkBuffer,
                    m_voxelRasterWorkCounterBuffer,
                    m_skinnedVoxelRasterWorkBuffer,
                    m_skinnedVoxelRasterWorkCounterBuffer,
                    m_voxelRasterWorkCapacity,
                    traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? swPageJobVisibleClustersBuffer : nullptr,
                    traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? swPageJobVisibleClusterTransformIndicesBuffer : nullptr,
                    traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? swPageJobVisibleClustersCounterBuffer : nullptr,
                    histogramIndirectCommand,
                    m_workGraphTelemetryBuffer,
                    m_occlusionReplayBuffer,
                    m_occlusionReplayStateBuffer,
                    m_occlusionNodeGpuInputsBuffer,
                    isPhase1 ? m_viewDepthSrvIndicesBuffer : m_viewDepthSrvIndicesBufferPhase2,
                    m_viewRasterInfoBuffer,
                    traits.type == CLodExtensionType::Shadow ? m_shadowDirtyPageHierarchyTexture : nullptr,
                    slabGroup,
                    previousVisibleClustersCounterBuffer,
                    previousSwVisibleClustersCounterBuffer,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPredictiveInvalidationCandidatesBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPredictiveInvalidationCandidateCountBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowInvalidatedInstancesBitsetBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPageTableTexture : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowStaticPhysicalPagesTexture : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowActiveBlockMetadataBuffer : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowPhysicalPagesTexture : nullptr,
                    traits.type == CLodExtensionType::Shadow ? m_shadowDynamicActiveBlockMetadataBuffer : nullptr,
                    useWorkGraphReyesVisibility ? m_reyesDiceQueueBuffer : nullptr,
                    useWorkGraphReyesVisibility ? m_reyesDiceQueueCounterBuffer : nullptr,
                    useWorkGraphReyesVisibility ? m_reyesDiceQueueOverflowBuffer : nullptr,
                    useWorkGraphReyesVisibility ? m_reyesTessTableConfigsBuffer : nullptr,
                    useWorkGraphReyesVisibility ? m_reyesTessTableVerticesBuffer : nullptr,
                    useWorkGraphReyesVisibility ? m_reyesTessTableTrianglesBuffer : nullptr,
                    useWorkGraphReyesVisibility
                        ? (isPhase1 ? m_reyesTelemetryBufferPhase1 : m_reyesTelemetryBufferPhase2)
                        : nullptr,
                    useWorkGraphReyesVisibility ? m_reyesDiceQueueCapacity : 0u));

        auto cullPassDesc = RenderGraph::ExternalPassDesc::Compute(cullPassName, cullPass);
        cullPassDesc.At(RenderGraph::ExternalInsertPoint::After(afterPassName));
        outPasses.push_back(std::move(cullPassDesc));
    };
    const auto appendRasterBucketCompactionPasses = [&](uint32_t phaseIndex, const std::shared_ptr<Buffer>& reyesOwnershipBitsetBuffer) {
        const bool isPhase1 = phaseIndex == 1u;
        const std::string phaseSuffix = std::to_string(phaseIndex);
        const std::shared_ptr<Buffer> visibleClustersCounterBuffer =
            isPhase1 ? m_visibleClustersCounterBuffer : m_visibleClustersCounterBufferPhase2;
        const std::shared_ptr<Buffer> histogramIndirectCommand =
            isPhase1 ? m_histogramIndirectCommand : m_histogramIndirectCommandPhase2;
        const std::shared_ptr<Buffer> histogramBuffer =
            isPhase1 ? m_rasterBucketsHistogramBuffer : m_rasterBucketsHistogramBufferPhase2;
        const std::shared_ptr<Buffer> totalCountBuffer =
            isPhase1 ? m_rasterBucketsTotalCountBufferPhase1 : m_rasterBucketsTotalCountBuffer;
        const std::shared_ptr<Buffer> writeCursorBuffer =
            isPhase1 ? m_rasterBucketsWriteCursorBuffer : m_rasterBucketsWriteCursorBufferPhase2;
        const std::shared_ptr<Buffer> indirectArgsBuffer =
            isPhase1 ? m_rasterBucketsIndirectArgsBuffer : m_rasterBucketsIndirectArgsBufferPhase2;
        const std::shared_ptr<Buffer> previousTotalCountBuffer =
            isPhase1 ? m_visibleClustersCounterBuffer : m_rasterBucketsTotalCountBufferPhase1;
        // Phase 2 reads the shared visible-cluster buffer after all phase-1 entries,
        // but appends compacted triangle work after only phase-1 compacted entries.
        const std::shared_ptr<Buffer> visibleClusterReadBaseCounterBuffer =
            isPhase1 ? std::shared_ptr<Buffer>{} : m_visibleClustersCounterBuffer;

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsHistogramPass" + phaseSuffix),
                std::make_shared<RasterBucketHistogramPass>(
                    m_visibleClustersBuffer,
                    visibleClustersCounterBuffer,
                    histogramIndirectCommand,
                    histogramBuffer,
                    reyesOwnershipBitsetBuffer,
                    m_workGraphTelemetryBuffer,
                    isPhase1 ? nullptr : m_visibleClustersCounterBuffer,
                    false,
                    m_visibleClusterCapacity)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixScanPass" + phaseSuffix),
                std::make_shared<RasterBucketBlockScanPass>(
                    histogramBuffer,
                    m_rasterBucketsOffsetsBuffer,
                    m_rasterBucketsBlockSumsBuffer)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixOffsetsPass" + phaseSuffix),
                std::make_shared<RasterBucketBlockOffsetsPass>(
                    m_rasterBucketsOffsetsBuffer,
                    m_rasterBucketsBlockSumsBuffer,
                    m_rasterBucketsScannedBlockSumsBuffer,
                    totalCountBuffer)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsCompactAndArgsPass" + phaseSuffix),
                std::make_shared<RasterBucketCompactAndArgsPass>(
                    m_visibleClustersBuffer,
                    m_visibleClusterTransformIndicesBuffer,
                    visibleClustersCounterBuffer,
                    previousTotalCountBuffer,
                    visibleClusterReadBaseCounterBuffer,
                    histogramIndirectCommand,
                    histogramBuffer,
                    m_rasterBucketsOffsetsBuffer,
                    writeCursorBuffer,
                    m_compactedVisibleClustersBuffer,
                    m_compactedVisibleClusterTransformIndicesBuffer,
                    indirectArgsBuffer,
                    m_sortedToUnsortedMappingBuffer,
                    reyesOwnershipBitsetBuffer,
                    m_workGraphTelemetryBuffer,
                    m_visibleClusterCapacity,
                    !isPhase1)));
    };
    const auto phaseFeedsPrimaryVisibility = [&](uint32_t phaseIndex) {
        return traits.rasterOutputKind == CLodRasterOutputKind::VisibilityBuffer &&
            (!enablePhase2OcclusionReplay || phaseIndex == 2u);
    };
    const auto appendFixedRasterPass = [&](uint32_t phaseIndex, bool updateShadowClearDirtyBitsAfter) -> std::string {
        const bool isPhase1 = phaseIndex == 1u;
        const std::string phaseSuffix = std::to_string(phaseIndex);
        ClusterRasterizationPassInputs rasterizePassInputs;
        rasterizePassInputs.clearGbuffer = isPhase1;
        rasterizePassInputs.wireframe = false;
        rasterizePassInputs.renderPhase = renderPhase;
        rasterizePassInputs.outputKind = traits.rasterOutputKind;

        const auto shadowPageTable = traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? m_shadowPageTableTexture : nullptr;
        const auto shadowStaticPhysicalPages =
            traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? m_shadowStaticPhysicalPagesTexture : nullptr;
        const auto shadowDynamicPhysicalPages =
            traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? m_shadowPhysicalPagesTexture : nullptr;
        const auto shadowClipmapInfo = traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow ? m_shadowClipmapInfoBuffer : nullptr;
        const std::shared_ptr<Buffer> rasterHistogramBuffer =
            isPhase1 ? m_rasterBucketsHistogramBuffer : m_rasterBucketsHistogramBufferPhase2;
        const std::shared_ptr<Buffer> rasterIndirectArgsBuffer =
            isPhase1 ? m_rasterBucketsIndirectArgsBuffer : m_rasterBucketsIndirectArgsBufferPhase2;
        const std::string passName = MakeVariantPassName(traits, "RasterizeClustersPass" + phaseSuffix);

        auto rasterizePassDesc = RenderGraph::ExternalPassDesc::Render(
            passName,
            std::make_shared<ClusterRasterizationPass>(
                rasterizePassInputs,
                m_compactedVisibleClustersBuffer,
                m_compactedVisibleClusterTransformIndicesBuffer,
                rasterHistogramBuffer,
                rasterIndirectArgsBuffer,
                m_sortedToUnsortedMappingBuffer,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                slabGroup,
                shadowPageTable,
                shadowStaticPhysicalPages,
                shadowClipmapInfo,
                nullptr,
                nullptr,
                m_workGraphTelemetryBuffer,
                m_streamingSystem ? m_streamingSystem->GetSourceGroupMismatchCounterBuffer() : nullptr,
                m_streamingSystem ? m_streamingSystem->GetSourceGroupMismatchDetailsBuffer() : nullptr,
                shadowDynamicPhysicalPages));
        if (phaseFeedsPrimaryVisibility(phaseIndex)) {
            rasterizePassDesc.At(RenderGraph::ExternalInsertPoint::Before("MaterialHistogramPass"));
        }
        rasterizePassDesc.GeometryPass();
        outPasses.push_back(std::move(rasterizePassDesc));

        if (updateShadowClearDirtyBitsAfter) {
            shadowClearDirtyBitsAfterPassName = passName;
        }

        return passName;
    };
    const auto appendComputeSoftwareRasterPasses = [&](uint32_t phaseIndex, const std::shared_ptr<Buffer>& reyesOwnershipBitsetBuffer) -> std::string {
        const bool isPhase1 = phaseIndex == 1u;
        const std::string phaseSuffix = std::to_string(phaseIndex);
        const std::shared_ptr<Buffer> swVisibleClustersCounterBuffer =
            isPhase1 ? m_swVisibleClustersCounterBuffer : m_swVisibleClustersCounterBufferPhase2;
        const std::shared_ptr<Buffer> swHistogramIndirectCommand =
            isPhase1 ? m_histogramIndirectCommandSw : m_histogramIndirectCommandPhase2Sw;
        const std::shared_ptr<Buffer> swHistogramBuffer =
            isPhase1 ? m_rasterBucketsHistogramBufferSw : m_rasterBucketsHistogramBufferPhase2Sw;
        const std::shared_ptr<Buffer> swTotalCountBuffer =
            isPhase1 ? m_rasterBucketsTotalCountBufferPhase1Sw : m_rasterBucketsTotalCountBuffer;
        const std::shared_ptr<Buffer> swWriteCursorBuffer =
            isPhase1 ? m_rasterBucketsWriteCursorBufferSw : m_rasterBucketsWriteCursorBufferPhase2Sw;
        const std::shared_ptr<Buffer> swIndirectArgsBuffer =
            isPhase1 ? m_rasterBucketsIndirectArgsBufferSw : m_rasterBucketsIndirectArgsBufferPhase2Sw;
        const std::shared_ptr<Buffer> previousSwVisibleClustersCounterBuffer =
            isPhase1 ? std::shared_ptr<Buffer>{} : m_swVisibleClustersCounterBuffer;
        const std::shared_ptr<Buffer> previousSwTotalCountBuffer =
            isPhase1 ? m_swVisibleClustersCounterBuffer : m_rasterBucketsTotalCountBufferPhase1Sw;
        // See the HW path above: SW compaction has the same split between
        // visible-buffer read base and compacted-buffer append base.
        const std::shared_ptr<Buffer> swVisibleClusterReadBaseCounterBuffer =
            isPhase1 ? std::shared_ptr<Buffer>{} : m_swVisibleClustersCounterBuffer;

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsCreateCommandPassSW" + phaseSuffix),
                std::make_shared<RasterBucketCreateCommandPass>(
                    swVisibleClustersCounterBuffer,
                    swHistogramIndirectCommand,
                    m_occlusionReplayStateBuffer,
                    m_occlusionNodeGpuInputsBuffer,
                    m_visibleClusterCapacity,
                    true)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsHistogramPassSW" + phaseSuffix),
                std::make_shared<RasterBucketHistogramPass>(
                    m_visibleClustersBuffer,
                    swVisibleClustersCounterBuffer,
                    swHistogramIndirectCommand,
                    swHistogramBuffer,
                    reyesOwnershipBitsetBuffer,
                    m_workGraphTelemetryBuffer,
                    previousSwVisibleClustersCounterBuffer,
                    true,
                    m_visibleClusterCapacity,
                    true)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixScanPassSW" + phaseSuffix),
                std::make_shared<RasterBucketBlockScanPass>(
                    swHistogramBuffer,
                    m_rasterBucketsOffsetsBuffer,
                    m_rasterBucketsBlockSumsBuffer,
                    true)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixOffsetsPassSW" + phaseSuffix),
                std::make_shared<RasterBucketBlockOffsetsPass>(
                    m_rasterBucketsOffsetsBuffer,
                    m_rasterBucketsBlockSumsBuffer,
                    m_rasterBucketsScannedBlockSumsBuffer,
                    swTotalCountBuffer,
                    true)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsCompactAndArgsPassSW" + phaseSuffix),
                std::make_shared<RasterBucketCompactAndArgsPass>(
                    m_visibleClustersBuffer,
                    m_visibleClusterTransformIndicesBuffer,
                    swVisibleClustersCounterBuffer,
                    previousSwTotalCountBuffer,
                    swVisibleClusterReadBaseCounterBuffer,
                    swHistogramIndirectCommand,
                    swHistogramBuffer,
                    m_rasterBucketsOffsetsBuffer,
                    swWriteCursorBuffer,
                    m_compactedVisibleClustersBufferSw,
                    m_compactedVisibleClusterTransformIndicesBufferSw,
                    swIndirectArgsBuffer,
                    m_sortedToUnsortedMappingBufferSw,
                    reyesOwnershipBitsetBuffer,
                    m_workGraphTelemetryBuffer,
                    m_visibleClusterCapacity,
                    !isPhase1,
                    true,
                    true,
                    true)));

        std::shared_ptr<Buffer> swRasterClustersBuffer = m_compactedVisibleClustersBufferSw;
        std::shared_ptr<Buffer> swRasterClusterTransformIndicesBuffer = m_compactedVisibleClusterTransformIndicesBufferSw;
        std::shared_ptr<Buffer> swRasterMappingBuffer = m_sortedToUnsortedMappingBufferSw;
        std::shared_ptr<Buffer> swRasterHistogramBuffer = swHistogramBuffer;
        std::shared_ptr<Buffer> swRasterIndirectArgsBuffer = swIndirectArgsBuffer;

        if (traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow) {
            const std::shared_ptr<Buffer> blockHistogramBuffer =
                isPhase1 ? m_rasterBucketsHistogramBufferPhase2Sw : m_rasterBucketsHistogramBufferSw;
            const std::shared_ptr<Buffer> blockWriteCursorBuffer =
                isPhase1 ? m_rasterBucketsWriteCursorBufferPhase2Sw : m_rasterBucketsWriteCursorBufferSw;
            const std::shared_ptr<Buffer> blockIndirectArgsBuffer =
                isPhase1 ? m_rasterBucketsIndirectArgsBufferPhase2Sw : m_rasterBucketsIndirectArgsBufferSw;
            const std::shared_ptr<Buffer> blockTotalCountBuffer =
                isPhase1 ? m_rasterBucketsTotalCountBuffer : m_rasterBucketsTotalCountBufferPhase1Sw;

            outPasses.push_back(
                RenderGraph::ExternalPassDesc::Compute(
                    MakeVariantPassName(traits, "VirtualShadowBlockHistogramPassSW" + phaseSuffix),
                    std::make_shared<VirtualShadowBlockExpandPass>(
                        VirtualShadowBlockExpandMode::Histogram,
                        m_compactedVisibleClustersBufferSw,
                        m_compactedVisibleClusterTransformIndicesBufferSw,
                        swHistogramBuffer,
                        swIndirectArgsBuffer,
                        blockHistogramBuffer,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        m_shadowClipmapInfoBuffer,
                        m_shadowActiveBlockMetadataBuffer,
                        m_shadowDynamicActiveBlockMetadataBuffer,
                        m_shadowBlockClusterCoverageBuffer,
                        m_shadowStatsBuffer,
                        m_shadowConfiguredExpandedRecordCapacity,
                        slabGroup,
                        true)));

            outPasses.push_back(
                RenderGraph::ExternalPassDesc::Compute(
                    MakeVariantPassName(traits, "VirtualShadowBlockPrefixScanPassSW" + phaseSuffix),
                    std::make_shared<RasterBucketBlockScanPass>(
                        blockHistogramBuffer,
                        m_rasterBucketsOffsetsBuffer,
                        m_rasterBucketsBlockSumsBuffer,
                        true)));

            outPasses.push_back(
                RenderGraph::ExternalPassDesc::Compute(
                    MakeVariantPassName(traits, "VirtualShadowBlockPrefixOffsetsPassSW" + phaseSuffix),
                    std::make_shared<RasterBucketBlockOffsetsPass>(
                        m_rasterBucketsOffsetsBuffer,
                        m_rasterBucketsBlockSumsBuffer,
                        m_rasterBucketsScannedBlockSumsBuffer,
                        blockTotalCountBuffer,
                        true)));

            outPasses.push_back(
                RenderGraph::ExternalPassDesc::Compute(
                    MakeVariantPassName(traits, "VirtualShadowBlockEmitPassSW" + phaseSuffix),
                    std::make_shared<VirtualShadowBlockExpandPass>(
                        VirtualShadowBlockExpandMode::Emit,
                        m_compactedVisibleClustersBufferSw,
                        m_compactedVisibleClusterTransformIndicesBufferSw,
                        swHistogramBuffer,
                        swIndirectArgsBuffer,
                        blockHistogramBuffer,
                        m_rasterBucketsOffsetsBuffer,
                        blockWriteCursorBuffer,
                        m_vsmExpandedVisibleClustersBufferSw,
                        m_vsmExpandedVisibleClusterTransformIndicesBufferSw,
                        m_shadowClipmapInfoBuffer,
                        m_shadowActiveBlockMetadataBuffer,
                        m_shadowDynamicActiveBlockMetadataBuffer,
                        m_shadowBlockClusterCoverageBuffer,
                        m_shadowStatsBuffer,
                        m_shadowConfiguredExpandedRecordCapacity,
                        slabGroup,
                        true)));

            outPasses.push_back(
                RenderGraph::ExternalPassDesc::Compute(
                    MakeVariantPassName(traits, "VirtualShadowBuildArgsPassSW" + phaseSuffix),
                    std::make_shared<VirtualShadowBuildRasterArgsPass>(
                        blockHistogramBuffer,
                        m_rasterBucketsOffsetsBuffer,
                        blockIndirectArgsBuffer,
                        true)));

            swRasterClustersBuffer = m_vsmExpandedVisibleClustersBufferSw;
            swRasterClusterTransformIndicesBuffer = m_vsmExpandedVisibleClusterTransformIndicesBufferSw;
            swRasterMappingBuffer = m_sortedToUnsortedMappingBufferSw;
            swRasterHistogramBuffer = blockHistogramBuffer;
            swRasterIndirectArgsBuffer = blockIndirectArgsBuffer;
        }

        const std::string passName = MakeVariantPassName(traits, "SoftwareRasterizeClustersPass" + phaseSuffix);
        auto softwareRasterPassDesc = RenderGraph::ExternalPassDesc::Compute(
            passName,
            std::make_shared<ClusterSoftwareRasterizationPass>(
                    swRasterClustersBuffer,
                    swRasterClusterTransformIndicesBuffer,
                    swRasterHistogramBuffer,
                    swRasterIndirectArgsBuffer,
                    swRasterMappingBuffer,
                    m_viewRasterInfoBuffer,
                    traits.rasterOutputKind,
                    m_shadowPageTableTexture,
                    m_shadowStaticPhysicalPagesTexture,
                    m_shadowPhysicalPagesTexture,
                    m_shadowClipmapInfoBuffer,
                    m_workGraphTelemetryBuffer,
                    slabGroup,
                    true));
        if (phaseFeedsPrimaryVisibility(phaseIndex)) {
            softwareRasterPassDesc.At(RenderGraph::ExternalInsertPoint::Before("MaterialHistogramPass"));
        }
        outPasses.push_back(std::move(softwareRasterPassDesc));

        if (traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow) {
            shadowClearDirtyBitsAfterPassName = passName;
        }

        return passName;
    };
    const auto appendPerViewDepthCopyPass = [&](uint32_t phaseIndex) {
        if (!traits.schedulesPerViewDepthCopy) {
            return;
        }

        const std::string passName = MakeVariantPassName(traits, "LinearDepthCopyPass" + std::to_string(phaseIndex));
        auto depthCopyPassDesc = RenderGraph::ExternalPassDesc::Compute(
            passName,
            std::make_shared<PerViewLinearDepthCopyPass>());
        if (phaseFeedsPrimaryVisibility(phaseIndex)) {
            auto depthCopyInsertPoint = RenderGraph::ExternalInsertPoint::Before("DeferredShadingPass");
            if (phaseIndex == 2u && SettingsManager::GetInstance().getSettingGetter<bool>("enableGTAO")()) {
                depthCopyInsertPoint.AlsoBefore("GTAOFilterPass");
            }
            depthCopyPassDesc.At(std::move(depthCopyInsertPoint));
        }
        outPasses.push_back(std::move(depthCopyPassDesc));
    };
    const auto appendLinearDepthDownsamplePass = [&](uint32_t phaseIndex, const std::string& afterPassName = {}) {
        auto downsamplePassDesc = RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "LinearDepthDownsamplePass" + std::to_string(phaseIndex)),
            std::make_shared<DownsamplePass>());
        if (!afterPassName.empty()) {
            auto downsampleInsertPoint = RenderGraph::ExternalInsertPoint::After(afterPassName);
            if (traits.rasterOutputKind == CLodRasterOutputKind::VisibilityBuffer) {
                downsampleInsertPoint.AlsoBefore("DeferredShadingPass");
            }
            downsamplePassDesc.At(std::move(downsampleInsertPoint));
        }
        else if (traits.rasterOutputKind == CLodRasterOutputKind::VisibilityBuffer) {
            downsamplePassDesc.At(RenderGraph::ExternalInsertPoint::Before("DeferredShadingPass"));
        }
        outPasses.push_back(std::move(downsamplePassDesc));
    };
    const auto appendPhaseRasterRouting = [&](uint32_t phaseIndex, const std::shared_ptr<Buffer>& reyesOwnershipBitsetBuffer) -> std::string {
        const std::string fixedRasterPassName = appendFixedRasterPass(
            phaseIndex,
            phaseIndex == 2u || traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow);

        std::string lastRasterPassName = fixedRasterPassName;

        if (useComputeSWRaster) {
            lastRasterPassName = appendComputeSoftwareRasterPasses(phaseIndex, reyesOwnershipBitsetBuffer);
        }

        if (traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow && useShadowPageJob) {
            lastRasterPassName = phaseIndex == 1u
                ? CLodShadowVariant::AppendPhase1PageJobRasterPasses(*this, traits, slabGroup, outPasses)
                : CLodShadowVariant::AppendPhase2PageJobRasterPasses(*this, traits, slabGroup, outPasses);
            shadowClearDirtyBitsAfterPassName = lastRasterPassName;
        }
        else if (traits.rasterOutputKind == CLodRasterOutputKind::VirtualShadow && useShadowReyesRouting) {
            lastRasterPassName = phaseIndex == 1u
                ? CLodShadowVariant::AppendPhase1ReyesLargeRasterPasses(*this, traits, slabGroup, outPasses)
                : CLodShadowVariant::AppendPhase2ReyesLargeRasterPasses(*this, traits, slabGroup, outPasses);
            shadowClearDirtyBitsAfterPassName = lastRasterPassName;
        }

        if (!m_options.enableVoxelRasterization) {
            appendPerViewDepthCopyPass(phaseIndex);
            return lastRasterPassName;
        }

        const std::string voxelRasterPassName = MakeVariantPassName(traits, "VoxelSoftwareRasterizePass" + std::to_string(phaseIndex));
        auto voxelRasterPassDesc = RenderGraph::ExternalPassDesc::Compute(
            voxelRasterPassName,
            std::make_shared<VoxelSoftwareRasterizationPass>(
                    m_visibleClustersBuffer,
                    m_visibleClusterTransformIndicesBuffer,
                    m_voxelRasterWorkBuffer,
                    m_voxelRasterWorkCounterBuffer,
                    m_skinnedVoxelRasterWorkBuffer,
                    m_skinnedVoxelRasterWorkCounterBuffer,
                    m_voxelRasterIndirectArgsBuffer,
                    m_skinnedVoxelRasterIndirectArgsBuffer,
                    m_workGraphTelemetryBuffer,
                    m_viewRasterInfoBuffer,
                    traits.rasterOutputKind,
                    m_shadowPageTableTexture,
                    m_shadowStaticPhysicalPagesTexture,
                    m_shadowPhysicalPagesTexture,
                    m_shadowClipmapInfoBuffer,
                    slabGroup,
                    m_voxelRasterWorkCapacity));
        if (phaseFeedsPrimaryVisibility(phaseIndex)) {
            auto voxelInsertPoint = RenderGraph::ExternalInsertPoint::After(lastRasterPassName);
            voxelInsertPoint.AlsoBefore("MaterialHistogramPass");
            voxelInsertPoint.AlsoBefore("BuildPixelListPass");
            voxelRasterPassDesc.At(std::move(voxelInsertPoint));
        }
        else {
            voxelRasterPassDesc.At(RenderGraph::ExternalInsertPoint::After(lastRasterPassName));
        }
        voxelRasterPassDesc.PreferQueue(QueueKind::Graphics);
        outPasses.push_back(std::move(voxelRasterPassDesc));
        lastRasterPassName = voxelRasterPassName;

        appendPerViewDepthCopyPass(phaseIndex);
        return lastRasterPassName;
    };
    const auto appendReplayPhase = [&]() {
        appendHierarchicalCullingPass(2u, MakeVariantPassName(traits, "LinearDepthDownsamplePass1"));

        if (useReyesForThisVariant && !useWorkGraphReyesVisibility) {
            AppendPhaseReyesStructuralPasses(
                traits,
                slabGroup,
                reyesOwnershipBitsetBufferPhase2,
                reyesSplitQueueCapacity,
                reyesDiceQueueCapacity,
                reyesRasterWorkCapacity,
                2u,
                false,
                false,
                false,
                outPasses,
                shadowClearDirtyBitsAfterPassName);
        }

        appendRasterBucketCompactionPasses(2u, reyesOwnershipBitsetBufferPhase2);
        appendPhaseRasterRouting(2u, reyesOwnershipBitsetBufferPhase2);
    };

    // Phase 1 starts with primary culling, then optionally prepares Reyes work before
    // any variant-specific raster routing takes over.
    std::string phase1CullAfterPassName;
    if (traits.scheduleMode == CLodVariantTraits::ScheduleMode::SinglePassCullOnly ||
        traits.scheduleMode == CLodVariantTraits::ScheduleMode::SinglePassDeepVisibility) {
        phase1CullAfterPassName = "CLodOpaque::LinearDepthDownsamplePass2";
    }
    else {
        if (traits.type == CLodExtensionType::Shadow) {
            phase1CullAfterPassName = shadowNonRasterableHierarchyPassName;
        }
        else {
            phase1CullAfterPassName = "CLod::StreamingBeginFramePass";
        }
    }
    if (useWorkGraphReyesVisibility &&
        traits.scheduleMode != CLodVariantTraits::ScheduleMode::SinglePassCullOnly &&
        useReyesForThisVariant) {
        AppendPhaseReyesStructuralPasses(
            traits,
            slabGroup,
            reyesOwnershipBitsetBuffer,
            reyesSplitQueueCapacity,
            reyesDiceQueueCapacity,
            reyesRasterWorkCapacity,
            1u,
            true,
            false,
            true,
            outPasses,
            shadowClearDirtyBitsAfterPassName);
    }

    appendHierarchicalCullingPass(1u, phase1CullAfterPassName);

    if (traits.scheduleMode != CLodVariantTraits::ScheduleMode::SinglePassCullOnly && useReyesForThisVariant && !useWorkGraphReyesVisibility) {
        AppendPhaseReyesStructuralPasses(
            traits,
            slabGroup,
            reyesOwnershipBitsetBuffer,
            reyesSplitQueueCapacity,
            reyesDiceQueueCapacity,
            reyesRasterWorkCapacity,
            1u,
            true,
            enablePhase2OcclusionReplay,
            false,
            outPasses,
            shadowClearDirtyBitsAfterPassName);
    }

    appendRasterBucketCompactionPasses(1u, reyesOwnershipBitsetBuffer);

    if (traits.scheduleMode == CLodVariantTraits::ScheduleMode::SinglePassCullOnly) {
        applyTechniqueTags();
        appendStreamingTailPasses();
        return;
    }

    if (traits.scheduleMode == CLodVariantTraits::ScheduleMode::SinglePassDeepVisibility) {
        const bool useAVBOIT = transparencyMode == CLodTransparencyMode::AVBOIT;
        CLodAlphaVariant::AppendSinglePassStructuralPasses(
            *this,
            traits,
            slabGroup,
            renderPhase,
            useAVBOIT,
            useReyesForThisVariant,
            disableReyesTessellation,
            outPasses);
        applyTechniqueTags();
        appendStreamingTailPasses();
        return;
    }

    const std::string phase1LastRasterPassName = appendPhaseRasterRouting(1u, reyesOwnershipBitsetBuffer);
    appendLinearDepthDownsamplePass(1u, phase1LastRasterPassName);

    // Only visibility scheduling currently uses phase 2 replay. Tie the entire
    // replay phase to the occlusion setting so disabling occlusion removes all
    // second-phase visibility work, not just the shader-side occlusion tests.
    if (enablePhase2OcclusionReplay) {
        appendReplayPhase();
    }

    CLodShadowVariant::AppendStructuralTail(*this, traits, outPasses, shadowClearDirtyBitsAfterPassName);
    appendLinearDepthDownsamplePass(2u);

    applyTechniqueTags();
    appendStreamingTailPasses();
}

void CLodExtension::GatherFramePasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    if (GetVariantTraits(m_type).ownsStreaming && m_streamingSystem) {
        m_streamingSystem->GatherFramePasses(rg, outPasses);
    }
}
