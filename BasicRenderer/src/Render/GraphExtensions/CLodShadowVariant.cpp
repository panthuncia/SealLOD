#include "Render/GraphExtensions/CLodShadowVariant.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "BuiltinResources.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/CLodExtension.h"
#include "Render/GraphExtensions/CLodExtensionShared.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterPageJobBuildArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterPageJobExpandPass.h"
#include "Render/GraphExtensions/ClusterLOD/ClusterSoftwareRasterPageJobRasterPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketBlockOffsetsPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketBlockScanPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketCompactAndArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketCreateCommandPass.h"
#include "Render/GraphExtensions/ClusterLOD/RasterBucketHistogramPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesBuildRasterWorkPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesClassifyPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesCreateDispatchArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesDicePass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesQueueResetPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesRasterWorkCompactAndArgsPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesRasterWorkHistogramPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesSeedPatchesPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesSplitPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesVirtualShadowHardwareRasterPass.h"
#include "Render/GraphExtensions/ClusterLOD/ReyesVirtualShadowRasterizationPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapAllocatePagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapApplyUpgradesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapDeduplicatePredictedPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapExpandPredictedPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapAdmitPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildMarkTilesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildPageListsPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapClearDirtyBitsPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapClearPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildActiveBlocksPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapDirtyHierarchyPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapFreeWrappedPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapGatherStatsPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapInvalidatePagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapMarkPagesPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapNonRasterableHierarchyPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapResolveMarkedBlocksPass.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapSetupPass.h"
#include "Render/GraphExtensions/ClusterLOD/CLodStreamingSystem.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Resources/components.h"

namespace {

bool AreRendererShadowsEnabled()
{
    return SettingsManager::GetInstance().getSettingGetter<bool>("enableShadows")();
}

RenderGraph::ExternalInsertPoint MakeShadowTailInsertPoint(const std::string& afterPassName)
{
    auto insertPoint = RenderGraph::ExternalInsertPoint::After(afterPassName);
    insertPoint.before.push_back("Screen-Space Reflections Pass");
    insertPoint.before.push_back("luminanceHistogramPass");
    return insertPoint;
}

enum class ShadowRasterPhase : uint32_t {
    Phase1 = 1u,
    Phase2 = 2u,
};

} // namespace

std::string CLodShadowVariant::AppendPageJobRasterPassesForPhase(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses,
    uint32_t phaseIndex)
{
    if (traits.rasterOutputKind != CLodRasterOutputKind::VirtualShadow) {
        return {};
    }

    const ShadowRasterPhase phase = static_cast<ShadowRasterPhase>(phaseIndex);
    const bool isPhase2 = phase == ShadowRasterPhase::Phase2;
    const auto phaseSuffix = isPhase2 ? "2" : "1";
    const auto& visibleClustersBuffer = isPhase2
        ? extension.m_swPageJobVisibleClustersBufferPhase2
        : extension.m_swPageJobVisibleClustersBuffer;
    const auto& visibleClusterTransformIndicesBuffer = isPhase2
        ? extension.m_swPageJobVisibleClusterTransformIndicesBufferPhase2
        : extension.m_swPageJobVisibleClusterTransformIndicesBuffer;
    const auto& visibleClustersCounterBuffer = isPhase2
        ? extension.m_swPageJobVisibleClustersCounterBufferPhase2
        : extension.m_swPageJobVisibleClustersCounterBuffer;
    const auto& histogramBuffer = isPhase2
        ? extension.m_rasterBucketsHistogramBufferPhase2Sw
        : extension.m_rasterBucketsHistogramBufferSw;
    const auto& writeCursorBuffer = isPhase2
        ? extension.m_rasterBucketsWriteCursorBufferPhase2Sw
        : extension.m_rasterBucketsWriteCursorBufferSw;
    const auto& indirectArgsBuffer = isPhase2
        ? extension.m_rasterBucketsIndirectArgsBufferPhase2PageJob
        : extension.m_rasterBucketsIndirectArgsBufferPageJob;
    const auto& histogramIndirectCommand = isPhase2
        ? extension.m_histogramIndirectCommandPhase2PageJob
        : extension.m_histogramIndirectCommandPageJob;
    const auto& pageJobRecordsBuffer = isPhase2
        ? extension.m_swPageJobRecordsBufferPhase2
        : extension.m_swPageJobRecordsBuffer;
    const auto& pageJobCountBuffer = isPhase2
        ? extension.m_swPageJobCountBufferPhase2
        : extension.m_swPageJobCountBuffer;
    const auto& pageJobRecordsBufferSkinned = isPhase2
        ? extension.m_swPageJobRecordsBufferPhase2Skinned
        : extension.m_swPageJobRecordsBufferSkinned;
    const auto& pageJobCountBufferSkinned = isPhase2
        ? extension.m_swPageJobCountBufferPhase2Skinned
        : extension.m_swPageJobCountBufferSkinned;
    const auto& pageJobIndirectArgsBuffer = isPhase2
        ? extension.m_swPageJobIndirectArgsBufferPhase2
        : extension.m_swPageJobIndirectArgsBuffer;
    const auto& pageJobIndirectArgsBufferSkinned = isPhase2
        ? extension.m_swPageJobIndirectArgsBufferPhase2Skinned
        : extension.m_swPageJobIndirectArgsBufferSkinned;
    const auto& pageJobClusterTagsBuffer = isPhase2
        ? extension.m_swPageJobClusterTagsBufferPhase2
        : extension.m_swPageJobClusterTagsBuffer;

    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsCreateCommandPassPageJob2"),
                std::make_shared<RasterBucketCreateCommandPass>(
                    visibleClustersCounterBuffer,
                    histogramIndirectCommand,
                    extension.m_occlusionReplayStateBuffer,
                    extension.m_occlusionNodeGpuInputsBuffer,
                    extension.m_maxVisibleClusters,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsCreateCommandPassPageJob1"),
                std::make_shared<RasterBucketCreateCommandPass>(
                    visibleClustersCounterBuffer,
                    histogramIndirectCommand,
                    extension.m_occlusionReplayStateBuffer,
                    extension.m_occlusionNodeGpuInputsBuffer,
                    extension.m_maxVisibleClusters)));
    }

    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsHistogramPassPageJob2"),
                std::make_shared<RasterBucketHistogramPass>(
                    visibleClustersBuffer,
                    visibleClustersCounterBuffer,
                    histogramIndirectCommand,
                    histogramBuffer,
                    nullptr,
                    extension.m_workGraphTelemetryBuffer,
                    nullptr,
                    false,
                    extension.m_maxVisibleClusters,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsHistogramPassPageJob1"),
                std::make_shared<RasterBucketHistogramPass>(
                    visibleClustersBuffer,
                    visibleClustersCounterBuffer,
                    histogramIndirectCommand,
                    histogramBuffer,
                    nullptr,
                    extension.m_workGraphTelemetryBuffer,
                    nullptr,
                    false,
                    extension.m_maxVisibleClusters)));
    }

    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixScanPassPageJob2"),
                std::make_shared<RasterBucketBlockScanPass>(
                    histogramBuffer,
                    extension.m_rasterBucketsOffsetsBuffer,
                    extension.m_rasterBucketsBlockSumsBuffer,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixScanPassPageJob1"),
                std::make_shared<RasterBucketBlockScanPass>(
                    histogramBuffer,
                    extension.m_rasterBucketsOffsetsBuffer,
                    extension.m_rasterBucketsBlockSumsBuffer)));
    }

    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixOffsetsPassPageJob2"),
                std::make_shared<RasterBucketBlockOffsetsPass>(
                    extension.m_rasterBucketsOffsetsBuffer,
                    extension.m_rasterBucketsBlockSumsBuffer,
                    extension.m_rasterBucketsScannedBlockSumsBuffer,
                    extension.m_rasterBucketsTotalCountBuffer,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsPrefixOffsetsPassPageJob1"),
                std::make_shared<RasterBucketBlockOffsetsPass>(
                    extension.m_rasterBucketsOffsetsBuffer,
                    extension.m_rasterBucketsBlockSumsBuffer,
                    extension.m_rasterBucketsScannedBlockSumsBuffer,
                    extension.m_rasterBucketsTotalCountBuffer)));
    }

    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsCompactAndArgsPassPageJob2"),
                std::make_shared<RasterBucketCompactAndArgsPass>(
                    visibleClustersBuffer,
                    visibleClusterTransformIndicesBuffer,
                    visibleClustersCounterBuffer,
                    visibleClustersCounterBuffer,
                    nullptr,
                    histogramIndirectCommand,
                    histogramBuffer,
                    extension.m_rasterBucketsOffsetsBuffer,
                    writeCursorBuffer,
                    extension.m_compactedVisibleClustersBuffer,
                    extension.m_compactedVisibleClusterTransformIndicesBuffer,
                    indirectArgsBuffer,
                    extension.m_sortedToUnsortedMappingBuffer,
                    nullptr,
                    extension.m_workGraphTelemetryBuffer,
                    extension.m_maxVisibleClusters,
                    false,
                    false,
                    true,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "RasterBucketsCompactAndArgsPassPageJob1"),
                std::make_shared<RasterBucketCompactAndArgsPass>(
                    visibleClustersBuffer,
                    visibleClusterTransformIndicesBuffer,
                    visibleClustersCounterBuffer,
                    visibleClustersCounterBuffer,
                    nullptr,
                    histogramIndirectCommand,
                    histogramBuffer,
                    extension.m_rasterBucketsOffsetsBuffer,
                    writeCursorBuffer,
                    extension.m_compactedVisibleClustersBuffer,
                    extension.m_compactedVisibleClusterTransformIndicesBuffer,
                    indirectArgsBuffer,
                    extension.m_sortedToUnsortedMappingBuffer,
                    nullptr,
                    extension.m_workGraphTelemetryBuffer,
                    extension.m_maxVisibleClusters,
                    false,
                    false,
                    true)));
    }

    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "SoftwareRasterPageJobExpandPass2"),
                std::make_shared<ClusterSoftwareRasterPageJobExpandPass>(
                    extension.m_compactedVisibleClustersBuffer,
                    extension.m_compactedVisibleClusterTransformIndicesBuffer,
                    histogramBuffer,
                    indirectArgsBuffer,
                    extension.m_viewRasterInfoBuffer,
                    extension.m_shadowPageTableTexture,
                    extension.m_shadowClipmapInfoBuffer,
                    pageJobRecordsBuffer,
                    pageJobCountBuffer,
                    pageJobRecordsBufferSkinned,
                    pageJobCountBufferSkinned,
                    pageJobClusterTagsBuffer,
                    extension.m_shadowStatsBuffer,
                    extension.m_shadowConfiguredPageJobRecordCapacity,
                    slabGroup,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "SoftwareRasterPageJobExpandPass1"),
                std::make_shared<ClusterSoftwareRasterPageJobExpandPass>(
                    extension.m_compactedVisibleClustersBuffer,
                    extension.m_compactedVisibleClusterTransformIndicesBuffer,
                    histogramBuffer,
                    indirectArgsBuffer,
                    extension.m_viewRasterInfoBuffer,
                    extension.m_shadowPageTableTexture,
                    extension.m_shadowClipmapInfoBuffer,
                    pageJobRecordsBuffer,
                    pageJobCountBuffer,
                    pageJobRecordsBufferSkinned,
                    pageJobCountBufferSkinned,
                    pageJobClusterTagsBuffer,
                    extension.m_shadowStatsBuffer,
                    extension.m_shadowConfiguredPageJobRecordCapacity,
                    slabGroup)));
    }

    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "SoftwareRasterPageJobBuildArgsPass2"),
                std::make_shared<ClusterSoftwareRasterPageJobBuildArgsPass>(
                    pageJobCountBuffer,
                    pageJobIndirectArgsBuffer,
                    pageJobCountBufferSkinned,
                    pageJobIndirectArgsBufferSkinned,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "SoftwareRasterPageJobBuildArgsPass1"),
                std::make_shared<ClusterSoftwareRasterPageJobBuildArgsPass>(
                    pageJobCountBuffer,
                    pageJobIndirectArgsBuffer,
                    pageJobCountBufferSkinned,
                    pageJobIndirectArgsBufferSkinned)));
    }

    const std::string shadowRasterPassName = MakeVariantPassName(
        traits,
        std::string("SoftwareRasterPageJobRasterPass") + phaseSuffix);
    if (isPhase2) {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                shadowRasterPassName,
                std::make_shared<ClusterSoftwareRasterPageJobRasterPass>(
                    extension.m_compactedVisibleClustersBuffer,
                    extension.m_compactedVisibleClusterTransformIndicesBuffer,
                    extension.m_viewRasterInfoBuffer,
                    extension.m_shadowPageTableTexture,
                    extension.m_shadowPhysicalPagesTexture,
                    extension.m_shadowClipmapInfoBuffer,
                    pageJobCountBuffer,
                    pageJobRecordsBuffer,
                    pageJobIndirectArgsBuffer,
                    pageJobCountBufferSkinned,
                    pageJobRecordsBufferSkinned,
                    pageJobIndirectArgsBufferSkinned,
                    extension.m_shadowStatsBuffer,
                    slabGroup,
                    true)));
    }
    else {
        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                shadowRasterPassName,
                std::make_shared<ClusterSoftwareRasterPageJobRasterPass>(
                    extension.m_compactedVisibleClustersBuffer,
                    extension.m_compactedVisibleClusterTransformIndicesBuffer,
                    extension.m_viewRasterInfoBuffer,
                    extension.m_shadowPageTableTexture,
                    extension.m_shadowPhysicalPagesTexture,
                    extension.m_shadowClipmapInfoBuffer,
                    pageJobCountBuffer,
                    pageJobRecordsBuffer,
                    pageJobIndirectArgsBuffer,
                    pageJobCountBufferSkinned,
                    pageJobRecordsBufferSkinned,
                    pageJobIndirectArgsBufferSkinned,
                    extension.m_shadowStatsBuffer,
                    slabGroup)));
    }

    return shadowRasterPassName;
}

std::string CLodShadowVariant::AppendFineRasterPassForPhase(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses,
    uint32_t phaseIndex)
{
    if (traits.type != CLodExtensionType::Shadow) {
        return {};
    }

    const ShadowRasterPhase phase = static_cast<ShadowRasterPhase>(phaseIndex);
    const bool isPhase2 = phase == ShadowRasterPhase::Phase2;
    const auto phaseSuffix = isPhase2 ? "2" : "1";
    const std::string reyesShadowRasterPassName = MakeVariantPassName(
        traits,
        std::string("ReyesVirtualShadowRasterPass") + phaseSuffix);
    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            reyesShadowRasterPassName,
            std::make_shared<ReyesVirtualShadowRasterizationPass>(
                extension.m_visibleClustersBuffer,
                extension.m_visibleClusterTransformIndicesBuffer,
                extension.m_reyesDiceQueueBuffer,
                extension.m_reyesDiceQueueCounterBuffer,
                isPhase2 ? extension.m_reyesRasterWorkBufferPhase2 : extension.m_reyesRasterWorkBuffer,
                isPhase2 ? extension.m_reyesRasterWorkCounterBufferPhase2 : extension.m_reyesRasterWorkCounterBuffer,
                extension.m_reyesTessTableConfigsBuffer,
                extension.m_reyesTessTableVerticesBuffer,
                extension.m_reyesTessTableTrianglesBuffer,
                isPhase2 ? extension.m_reyesRasterWorkIndirectArgsBufferPhase2 : extension.m_reyesRasterWorkIndirectArgsBuffer,
                isPhase2 ? extension.m_reyesTelemetryBufferPhase2 : extension.m_reyesTelemetryBufferPhase1,
                extension.m_shadowPageTableTexture,
                extension.m_shadowPhysicalPagesTexture,
                extension.m_shadowClipmapInfoBuffer,
                slabGroup,
                MakeVariantResourceName(
                    traits,
                    isPhase2
                        ? "Reyes Virtual Shadow View Raster Info Buffer Phase2"
                        : "Reyes Virtual Shadow View Raster Info Buffer"),
                static_cast<uint32_t>(phase))));

    return reyesShadowRasterPassName;
}
void CLodShadowVariant::RefreshConfiguredSettings(CLodExtension& extension)
{
    if (extension.m_type != CLodExtensionType::Shadow) {
        return;
    }

    extension.m_shadowConfiguredBackingResolution = CLodVirtualShadowGetConfiguredMaxBackingResolution();
    extension.m_shadowConfiguredMaxPhysicalPageCount = CLodVirtualShadowGetConfiguredMaxPhysicalPageCapacity();
    extension.m_shadowConfiguredPageJobMaxPages = std::max(
        1u,
        std::min(
            SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName)(),
            255u));
    extension.m_shadowConfiguredPageJobRecordCapacity = std::max(
        1u,
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodPageJobRecordCapacitySettingName)());
    extension.m_shadowConfiguredComputeClusterCapacity =
        CLodVirtualShadowGetConfiguredComputeClusterCapacity(extension.m_maxVisibleClusters);
    const uint64_t budgetedExpandedRecordCapacity =
        static_cast<uint64_t>(extension.m_shadowConfiguredComputeClusterCapacity) *
        static_cast<uint64_t>(CLodVirtualShadowExpandedRecordCapacityMultiplier);
    extension.m_shadowConfiguredExpandedRecordCapacity = static_cast<uint32_t>(
        std::max<uint64_t>(
            1u,
            std::min({
                budgetedExpandedRecordCapacity,
                static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) })));
}

uint32_t CLodShadowVariant::GetVisibleClusterCapacity(const CLodExtension& extension)
{
    return extension.m_maxVisibleClusters;
}

std::string CLodShadowVariant::AppendStructuralPrelude(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    if (traits.type != CLodExtensionType::Shadow) {
        return {};
    }

    const std::string shadowSetupPassName = MakeVariantPassName(traits, "VirtualShadowSetupPass");
    auto shadowSetupPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowSetupPassName,
        std::make_shared<VirtualShadowMapSetupPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowMarkClipmapDataBuffer,
            extension.m_shadowCompactMainCameraBuffer,
            extension.m_shadowCompactShadowCameraBuffer,
            extension.m_shadowStatsBuffer,
            extension.m_shadowRuntimeStateBuffer,
            extension.m_shadowPredictiveInvalidationCandidateCountBuffer,
            extension.m_shadowVirtualResourcesNeedReset));
    shadowSetupPassDesc.At(RenderGraph::ExternalInsertPoint::After("CLod::StreamingBeginFramePass"));
    outPasses.push_back(std::move(shadowSetupPassDesc));
    extension.m_shadowVirtualResourcesNeedReset = false;

    const std::string shadowFreeWrappedPagesPassName = MakeVariantPassName(traits, "VirtualShadowFreeWrappedPagesPass");
    auto shadowFreeWrappedPagesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowFreeWrappedPagesPassName,
        std::make_shared<VirtualShadowMapFreeWrappedPagesPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowStatsBuffer));
    shadowFreeWrappedPagesPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowSetupPassName));
    outPasses.push_back(std::move(shadowFreeWrappedPagesPassDesc));

    const std::string shadowInvalidatePagesPassName = MakeVariantPassName(traits, "VirtualShadowInvalidatePagesPass");
    auto shadowInvalidatePagesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowInvalidatePagesPassName,
        std::make_shared<VirtualShadowMapInvalidatePagesPass>(
            extension.m_shadowInvalidationInputsBuffer,
            extension.m_shadowInvalidationCountBuffer,
            extension.m_shadowInvalidatedInstancesBitsetBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowDirectionalPageViewInfoBuffer,
            extension.m_shadowStatsBuffer));
    shadowInvalidatePagesPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowFreeWrappedPagesPassName));
    outPasses.push_back(std::move(shadowInvalidatePagesPassDesc));

    const std::string shadowMarkPagesPassName = MakeVariantPassName(traits, "VirtualShadowMarkPagesPass");
    const std::string shadowBuildMarkTilesPassName = MakeVariantPassName(traits, "VirtualShadowBuildMarkTilesPass");
    auto shadowBuildMarkTilesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowBuildMarkTilesPassName,
        std::make_shared<VirtualShadowMapBuildMarkTilesPass>(
            extension.m_shadowMarkTileWorkBuffer,
            extension.m_shadowMarkTileCountBuffer));
    shadowBuildMarkTilesPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowInvalidatePagesPassName));
    outPasses.push_back(std::move(shadowBuildMarkTilesPassDesc));

    const std::string shadowBuildMarkTileDispatchArgsPassName = MakeVariantPassName(traits, "VirtualShadowBuildMarkTileDispatchArgsPass");
    auto shadowBuildMarkTileDispatchArgsPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowBuildMarkTileDispatchArgsPassName,
        std::make_shared<ReyesCreateDispatchArgsPass>(
            extension.m_shadowMarkTileCountBuffer,
            extension.m_shadowMarkTileIndirectArgsBuffer,
            nullptr,
            128u,
            CLodVirtualShadowMaxMarkTileCount));
    shadowBuildMarkTileDispatchArgsPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowBuildMarkTilesPassName));
    outPasses.push_back(std::move(shadowBuildMarkTileDispatchArgsPassDesc));

    auto shadowMarkPagesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowMarkPagesPassName,
        std::make_shared<VirtualShadowMapMarkPagesPass>(
            extension.m_shadowMarkTileWorkBuffer,
            extension.m_shadowMarkTileCountBuffer,
            extension.m_shadowMarkTileIndirectArgsBuffer,
            extension.m_shadowMarkClipmapDataBuffer,
            extension.m_shadowMarkedBlocksMaskBuffer,
            extension.m_shadowMarkedBlocksListBuffer,
            extension.m_shadowMarkedBlocksCountBuffer));
    shadowMarkPagesPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowBuildMarkTileDispatchArgsPassName));
    shadowMarkPagesPassDesc.preferredQueueKind = QueueKind::Graphics;
    outPasses.push_back(std::move(shadowMarkPagesPassDesc));

    const std::string shadowResolveMarkedBlocksPassName = MakeVariantPassName(traits, "VirtualShadowResolveMarkedBlocksPass");
    auto shadowResolveMarkedBlocksPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowResolveMarkedBlocksPassName,
        std::make_shared<VirtualShadowMapResolveMarkedBlocksPass>(
            extension.m_shadowMarkedBlocksMaskBuffer,
            extension.m_shadowMarkedBlocksListBuffer,
            extension.m_shadowMarkedBlocksCountBuffer,
            extension.m_shadowAllocationRequestsBuffer,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowMarkClipmapDataBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowDirectionalPageViewInfoBuffer,
            extension.m_shadowStatsBuffer));
    shadowResolveMarkedBlocksPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowMarkPagesPassName));
    shadowResolveMarkedBlocksPassDesc.preferredQueueKind = QueueKind::Graphics;
    outPasses.push_back(std::move(shadowResolveMarkedBlocksPassDesc));

    const std::string shadowBuildPageListsPassName = MakeVariantPassName(traits, "VirtualShadowBuildPageListsPass");
    auto shadowBuildPageListsPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowBuildPageListsPassName,
        std::make_shared<VirtualShadowMapBuildPageListsPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowFreePhysicalPagesBuffer,
            extension.m_shadowReusablePhysicalPagesBuffer,
            extension.m_shadowPageListHeaderBuffer));
    shadowBuildPageListsPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowResolveMarkedBlocksPassName));
    outPasses.push_back(std::move(shadowBuildPageListsPassDesc));

    const std::string shadowBuildDispatchArgsPassName = MakeVariantPassName(traits, "VirtualShadowBuildAllocationDispatchArgsPass");
    auto shadowBuildDispatchArgsPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowBuildDispatchArgsPassName,
        std::make_shared<ReyesCreateDispatchArgsPass>(
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowAllocationIndirectArgsBuffer,
            nullptr,
            64u,
            extension.m_shadowConfiguredMaxPhysicalPageCount));
    shadowBuildDispatchArgsPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowBuildPageListsPassName));
    outPasses.push_back(std::move(shadowBuildDispatchArgsPassDesc));

    const std::string shadowPreAllocateStatsPassName = MakeVariantPassName(traits, "VirtualShadowPreAllocateStatsPass");
    auto shadowPreAllocateStatsPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowPreAllocateStatsPassName,
        std::make_shared<VirtualShadowMapGatherStatsPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowAllocationIndirectArgsBuffer,
            extension.m_shadowPageListHeaderBuffer,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowStatsBuffer,
            true));
    shadowPreAllocateStatsPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowBuildDispatchArgsPassName));
    outPasses.push_back(std::move(shadowPreAllocateStatsPassDesc));

    const std::string shadowAllocationPassName = MakeVariantPassName(traits, "VirtualShadowAllocatePagesPass");
    auto shadowAllocationPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowAllocationPassName,
        std::make_shared<VirtualShadowMapAllocatePagesPass>(
            extension.m_shadowAllocationRequestsBuffer,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowAllocationIndirectArgsBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowFreePhysicalPagesBuffer,
            extension.m_shadowReusablePhysicalPagesBuffer,
            extension.m_shadowPageListHeaderBuffer,
            extension.m_shadowStatsBuffer));
    shadowAllocationPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowPreAllocateStatsPassName));
    outPasses.push_back(std::move(shadowAllocationPassDesc));

    const std::string shadowApplyUpgradesPassName = MakeVariantPassName(traits, "VirtualShadowApplyUpgradesPass");
    auto shadowApplyUpgradesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowApplyUpgradesPassName,
        std::make_shared<VirtualShadowMapApplyUpgradesPass>(
            extension.m_shadowUpgradeInvalidationInputsBuffer,
            extension.m_shadowUpgradeInvalidationCountBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowStatsBuffer,
            [&extension](uint32_t maxEvents) {
                return extension.m_streamingSystem
                    ? extension.m_streamingSystem->DrainVirtualShadowUpgradeEvents(maxEvents)
                    : std::vector<CLodVirtualShadowUpgradeInvalidationInput>{};
            },
            [&extension]() {
                return extension.m_streamingSystem
                    ? extension.m_streamingSystem->GetVirtualShadowUpgradeQueueStats()
                    : CLodVirtualShadowUpgradeQueueStats{};
            }));
    shadowApplyUpgradesPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowAllocationPassName));
    outPasses.push_back(std::move(shadowApplyUpgradesPassDesc));

    const std::string shadowGatherStatsPassName = MakeVariantPassName(traits, "VirtualShadowGatherStatsPass");
    auto shadowGatherStatsPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowGatherStatsPassName,
        std::make_shared<VirtualShadowMapGatherStatsPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowAllocationIndirectArgsBuffer,
            extension.m_shadowPageListHeaderBuffer,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowStatsBuffer,
            false));
    shadowGatherStatsPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowApplyUpgradesPassName));
    outPasses.push_back(std::move(shadowGatherStatsPassDesc));

    const std::string shadowAdmitPagesPassName = MakeVariantPassName(traits, "VirtualShadowAdmitPagesPass");
    auto shadowAdmitPagesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowAdmitPagesPassName,
        std::make_shared<VirtualShadowMapAdmitPagesPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowUpgradeInvalidationInputsBuffer,
            extension.m_shadowUpgradeInvalidationCountBuffer,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowCompactShadowCameraBuffer,
            extension.m_shadowStatsBuffer));
    shadowAdmitPagesPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowGatherStatsPassName));
    outPasses.push_back(std::move(shadowAdmitPagesPassDesc));

    const std::string shadowClearPagesPassName = MakeVariantPassName(traits, "VirtualShadowClearPagesPass");
    auto shadowClearPagesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowClearPagesPassName,
        std::make_shared<VirtualShadowMapClearPagesPass>(
            extension.m_shadowPhysicalPagesTexture,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowDirectionalPageViewInfoBuffer,
            extension.m_shadowStatsBuffer));
    shadowClearPagesPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowAdmitPagesPassName));
    outPasses.push_back(std::move(shadowClearPagesPassDesc));

    const std::string shadowBuildActiveBlocksPassName = MakeVariantPassName(traits, "VirtualShadowBuildActiveBlocksPass");
    auto shadowBuildActiveBlocksPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowBuildActiveBlocksPassName,
        std::make_shared<VirtualShadowMapBuildActiveBlocksPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowActiveBlockMetadataBuffer));
    shadowBuildActiveBlocksPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowClearPagesPassName));
    outPasses.push_back(std::move(shadowBuildActiveBlocksPassDesc));

    const std::string shadowDirtyHierarchyPassName = MakeVariantPassName(traits, "VirtualShadowDirtyHierarchyPass");
    auto shadowDirtyHierarchyPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowDirtyHierarchyPassName,
        std::make_shared<VirtualShadowMapDirtyHierarchyPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowDirtyPageHierarchyTexture,
            extension.m_shadowClipmapInfoBuffer));
    shadowDirtyHierarchyPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowBuildActiveBlocksPassName));
    outPasses.push_back(std::move(shadowDirtyHierarchyPassDesc));

    const std::string shadowNonRasterableHierarchyPassName = MakeVariantPassName(traits, "VirtualShadowNonRasterableHierarchyPass");
    auto shadowNonRasterableHierarchyPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowNonRasterableHierarchyPassName,
        std::make_shared<VirtualShadowMapNonRasterableHierarchyPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowNonRasterablePageHierarchyTexture,
            extension.m_shadowClipmapInfoBuffer));
    shadowNonRasterableHierarchyPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowDirtyHierarchyPassName));
    outPasses.push_back(std::move(shadowNonRasterableHierarchyPassDesc));

    return shadowNonRasterableHierarchyPassName;
}

void CLodShadowVariant::AppendStructuralTail(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses,
    const std::string& shadowClearDirtyBitsAfterPassName)
{
    if (traits.type != CLodExtensionType::Shadow || shadowClearDirtyBitsAfterPassName.empty()) {
        return;
    }

    const std::string shadowExpandPredictedPagesPassName = MakeVariantPassName(traits, "VirtualShadowFinalizeFallbackPagesPass");
    auto shadowExpandPredictedPagesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowExpandPredictedPagesPassName,
        std::make_shared<VirtualShadowMapExpandPredictedPagesPass>(
            extension.m_shadowPredictiveInvalidationCandidatesBuffer,
            extension.m_shadowPredictiveInvalidationCandidateCountBuffer,
            extension.m_shadowPredictiveRawPagesBuffer,
            extension.m_shadowPredictiveRawPageCountBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowPredictedInvalidationScratchBitsetBuffer,
            extension.m_shadowStatsBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowDirectionalPageViewInfoBuffer,
            extension.m_shadowConfiguredMaxPhysicalPageCount));
    shadowExpandPredictedPagesPassDesc.At(
        RenderGraph::ExternalInsertPoint::After(shadowClearDirtyBitsAfterPassName));
    outPasses.push_back(std::move(shadowExpandPredictedPagesPassDesc));

    const std::string shadowDeduplicatePredictedPagesPassName = MakeVariantPassName(traits, "VirtualShadowDeduplicateFallbackPagesPass");
    auto shadowDeduplicatePredictedPagesPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowDeduplicatePredictedPagesPassName,
        std::make_shared<VirtualShadowMapDeduplicatePredictedPagesPass>(
            extension.m_shadowPredictiveRawPagesBuffer,
            extension.m_shadowPredictiveRawPageCountBuffer,
            extension.m_shadowPredictedInvalidationScratchBitsetBuffer,
            extension.m_shadowPredictedInvalidationPagesBufferA,
            extension.m_shadowPredictedInvalidationPageCountBufferA,
            extension.m_shadowStatsBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowConfiguredMaxPhysicalPageCount));
    shadowDeduplicatePredictedPagesPassDesc.At(
        RenderGraph::ExternalInsertPoint::After(shadowExpandPredictedPagesPassName));
    outPasses.push_back(std::move(shadowDeduplicatePredictedPagesPassDesc));

    const std::string shadowClearDirtyBitsPassName = MakeVariantPassName(traits, "VirtualShadowClearDirtyBitsPass");
    auto shadowClearDirtyBitsPassDesc = RenderGraph::ExternalPassDesc::Compute(
        shadowClearDirtyBitsPassName,
        std::make_shared<VirtualShadowMapClearDirtyBitsPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowAllocationRequestsBuffer,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowAllocationIndirectArgsBuffer,
            extension.m_shadowStatsBuffer));
    shadowClearDirtyBitsPassDesc.At(RenderGraph::ExternalInsertPoint::After(shadowDeduplicatePredictedPagesPassName));
    outPasses.push_back(std::move(shadowClearDirtyBitsPassDesc));

}

std::string CLodShadowVariant::AppendPhase1PageJobRasterPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    return AppendPageJobRasterPassesForPhase(extension, traits, slabGroup, outPasses, 1u);
}

std::string CLodShadowVariant::AppendPhase2PageJobRasterPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    return AppendPageJobRasterPassesForPhase(extension, traits, slabGroup, outPasses, 2u);
}

std::string CLodShadowVariant::AppendPhase1FineRasterPass(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    return AppendFineRasterPassForPhase(extension, traits, slabGroup, outPasses, 1u);
}

std::string CLodShadowVariant::AppendPhase2FineRasterPass(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    return AppendFineRasterPassForPhase(extension, traits, slabGroup, outPasses, 2u);
}

std::string CLodShadowVariant::AppendPhase1ReyesLargeRasterPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    if (traits.rasterOutputKind != CLodRasterOutputKind::VirtualShadow) {
        return {};
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeQueueResetPass1"),
            std::make_shared<ReyesQueueResetPass>(
                extension.m_reyesFullClusterOutputsCounterBuffer,
                extension.m_reyesOwnedClustersCounterBuffer,
                std::vector<std::shared_ptr<Buffer>>{ extension.m_reyesSplitQueueCounterBufferA, extension.m_reyesSplitQueueCounterBufferB },
                std::vector<std::shared_ptr<Buffer>>{ extension.m_reyesSplitQueueOverflowBufferA, extension.m_reyesSplitQueueOverflowBufferB },
                extension.m_reyesDiceQueueCounterBuffer,
                extension.m_reyesDiceQueueOverflowBuffer,
                nullptr,
                extension.m_reyesTelemetryBufferPhase1,
                1u,
                true)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeCreateClassifyDispatchArgsPass1"),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                extension.m_swPageJobVisibleClustersCounterBuffer,
                extension.m_reyesClassifyIndirectArgsBuffer)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeClassifyPass1"),
            std::make_shared<ReyesClassifyPass>(
                extension.m_swPageJobVisibleClustersBuffer,
                extension.m_swPageJobVisibleClustersCounterBuffer,
                nullptr,
                extension.m_reyesFullClusterOutputsBuffer,
                extension.m_reyesFullClusterOutputsCounterBuffer,
                extension.m_reyesFullClusterOutputCapacity,
                extension.m_reyesOwnedClustersBuffer,
                extension.m_reyesOwnedClustersCounterBuffer,
                extension.m_reyesOwnedClusterCapacity,
                nullptr,
                extension.m_reyesClassifyIndirectArgsBuffer,
                extension.m_reyesTelemetryBufferPhase1,
                1u,
                ReyesClassifyMode::ShadowCoarseLargeOnly)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeCreateSeedDispatchArgsPass1"),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                extension.m_reyesOwnedClustersCounterBuffer,
                extension.m_reyesSplitIndirectArgsBuffer)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeSeedPatchesPass1"),
            std::make_shared<ReyesSeedPatchesPass>(
                extension.m_swPageJobVisibleClustersBuffer,
                extension.m_reyesOwnedClustersBuffer,
                extension.m_reyesOwnedClustersCounterBuffer,
                extension.m_reyesSplitQueueBufferA,
                extension.m_reyesSplitQueueCounterBufferA,
                extension.m_reyesSplitQueueOverflowBufferA,
                extension.m_reyesSplitIndirectArgsBuffer,
                slabGroup,
                extension.m_reyesSplitQueueCapacity,
                1u)));

    const std::shared_ptr<Buffer> reyesLargeSplitBuffers[] = { extension.m_reyesSplitQueueBufferA, extension.m_reyesSplitQueueBufferB };
    const std::shared_ptr<Buffer> reyesLargeSplitCounters[] = { extension.m_reyesSplitQueueCounterBufferA, extension.m_reyesSplitQueueCounterBufferB };
    const std::shared_ptr<Buffer> reyesLargeSplitOverflows[] = { extension.m_reyesSplitQueueOverflowBufferA, extension.m_reyesSplitQueueOverflowBufferB };
    for (uint32_t splitPassIndex = 0; splitPassIndex < CLodReyesMaxSplitPassCount; ++splitPassIndex) {
        const uint32_t inputIndex = splitPassIndex & 1u;
        const uint32_t outputIndex = inputIndex ^ 1u;

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesLargeCreateSplitDispatchArgsPass1_" + std::to_string(splitPassIndex)),
                std::make_shared<ReyesCreateDispatchArgsPass>(
                    reyesLargeSplitCounters[inputIndex],
                    extension.m_reyesSplitIndirectArgsBuffer)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesLargeSplitPass1_" + std::to_string(splitPassIndex)),
                std::make_shared<ReyesSplitPass>(
                    extension.m_swPageJobVisibleClustersBuffer,
                    reyesLargeSplitBuffers[inputIndex],
                    reyesLargeSplitCounters[inputIndex],
                    reyesLargeSplitBuffers[outputIndex],
                    reyesLargeSplitCounters[outputIndex],
                    reyesLargeSplitOverflows[outputIndex],
                    extension.m_reyesDiceQueueBuffer,
                    extension.m_reyesDiceQueueCounterBuffer,
                    extension.m_reyesDiceQueueOverflowBuffer,
                    extension.m_reyesTessTableConfigsBuffer,
                    extension.m_reyesTessTableVerticesBuffer,
                    extension.m_reyesTessTableTrianglesBuffer,
                    extension.m_shadowClipmapInfoBuffer,
                    extension.m_shadowDirtyPageHierarchyTexture,
                    extension.m_shadowNonRasterablePageHierarchyTexture,
                    extension.m_reyesSplitIndirectArgsBuffer,
                    extension.m_reyesTelemetryBufferPhase1,
                    extension.m_reyesSplitQueueCapacity,
                    splitPassIndex,
                    CLodReyesMaxSplitPassCount,
                    1u)));
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeCreateDiceDispatchArgsPass1"),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                extension.m_reyesDiceQueueCounterBuffer,
                extension.m_reyesDiceIndirectArgsBuffer)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeDicePass1"),
            std::make_shared<ReyesDicePass>(
                extension.m_reyesDiceQueueBuffer,
                extension.m_reyesDiceQueueCounterBuffer,
                nullptr,
                extension.m_reyesTessTableConfigsBuffer,
                extension.m_reyesDiceIndirectArgsBuffer,
                extension.m_reyesTelemetryBufferPhase1,
                extension.m_reyesDiceQueueCapacity,
                1u)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeBuildRasterWorkPass1"),
            std::make_shared<ReyesBuildRasterWorkPass>(
                extension.m_reyesDiceQueueBuffer,
                extension.m_reyesDiceQueueCounterBuffer,
                nullptr,
                extension.m_reyesTessTableConfigsBuffer,
                extension.m_reyesRasterWorkBuffer,
                extension.m_reyesRasterWorkCounterBuffer,
                extension.m_reyesDiceIndirectArgsBuffer,
                extension.m_reyesTelemetryBufferPhase1,
                extension.m_reyesRasterWorkCapacity)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "RasterBucketsCreateCommandPassReyesHW1"),
            std::make_shared<RasterBucketCreateCommandPass>(
                extension.m_reyesRasterWorkCounterBuffer,
                extension.m_histogramIndirectCommandReyes,
                extension.m_occlusionReplayStateBuffer,
                extension.m_occlusionNodeGpuInputsBuffer,
                extension.m_reyesRasterWorkCapacity)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesRasterWorkHistogramPass1"),
            std::make_shared<ReyesRasterWorkHistogramPass>(
                extension.m_reyesRasterWorkBuffer,
                extension.m_reyesRasterWorkCounterBuffer,
                extension.m_histogramIndirectCommandReyes,
                extension.m_rasterBucketsHistogramBufferSw)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesRasterWorkPrefixScanPass1"),
            std::make_shared<RasterBucketBlockScanPass>(
                extension.m_rasterBucketsHistogramBufferSw,
                extension.m_rasterBucketsOffsetsBuffer,
                extension.m_rasterBucketsBlockSumsBuffer)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesRasterWorkPrefixOffsetsPass1"),
            std::make_shared<RasterBucketBlockOffsetsPass>(
                extension.m_rasterBucketsOffsetsBuffer,
                extension.m_rasterBucketsBlockSumsBuffer,
                extension.m_rasterBucketsScannedBlockSumsBuffer,
                extension.m_rasterBucketsTotalCountBuffer)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesRasterWorkCompactAndArgsPass1"),
            std::make_shared<ReyesRasterWorkCompactAndArgsPass>(
                extension.m_reyesRasterWorkBuffer,
                extension.m_reyesRasterWorkCounterBuffer,
                extension.m_histogramIndirectCommandReyes,
                extension.m_rasterBucketsHistogramBufferSw,
                extension.m_rasterBucketsOffsetsBuffer,
                extension.m_rasterBucketsWriteCursorBufferSw,
                extension.m_reyesCompactedRasterWorkIndicesBuffer,
                extension.m_reyesPackedRasterWorkGroupsBuffer,
                extension.m_rasterBucketsIndirectArgsBufferPageJob)));

    const std::string reyesLargeShadowRasterPassName = MakeVariantPassName(traits, "ReyesLargeVirtualShadowHardwareRasterPass1");
    auto reyesLargeShadowRasterPassDesc = RenderGraph::ExternalPassDesc::Render(
        reyesLargeShadowRasterPassName,
        std::make_shared<ReyesVirtualShadowHardwareRasterPass>(
            extension.m_swPageJobVisibleClustersBuffer,
            extension.m_rasterBucketsHistogramBufferSw,
            extension.m_rasterBucketsIndirectArgsBufferPageJob,
            extension.m_reyesPackedRasterWorkGroupsBuffer,
            extension.m_reyesCompactedRasterWorkIndicesBuffer,
            extension.m_reyesRasterWorkBuffer,
            extension.m_reyesDiceQueueBuffer,
            extension.m_reyesTessTableConfigsBuffer,
            extension.m_reyesTessTableVerticesBuffer,
            extension.m_reyesTessTableTrianglesBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPhysicalPagesTexture,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_reyesTelemetryBufferPhase1,
            slabGroup));
    reyesLargeShadowRasterPassDesc.GeometryPass();
    outPasses.push_back(std::move(reyesLargeShadowRasterPassDesc));

    return reyesLargeShadowRasterPassName;
}

std::string CLodShadowVariant::AppendPhase2ReyesLargeRasterPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<ResourceGroup>& slabGroup,
    std::vector<RenderGraph::ExternalPassDesc>& outPasses)
{
    if (traits.rasterOutputKind != CLodRasterOutputKind::VirtualShadow) {
        return {};
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeQueueResetPass2"),
            std::make_shared<ReyesQueueResetPass>(
                extension.m_reyesFullClusterOutputsCounterBuffer,
                extension.m_reyesOwnedClustersCounterBuffer,
                std::vector<std::shared_ptr<Buffer>>{ extension.m_reyesSplitQueueCounterBufferA, extension.m_reyesSplitQueueCounterBufferB },
                std::vector<std::shared_ptr<Buffer>>{ extension.m_reyesSplitQueueOverflowBufferA, extension.m_reyesSplitQueueOverflowBufferB },
                extension.m_reyesDiceQueueCounterBuffer,
                extension.m_reyesDiceQueueOverflowBuffer,
                nullptr,
                extension.m_reyesTelemetryBufferPhase2,
                2u,
                true)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeCreateClassifyDispatchArgsPass2"),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                extension.m_swPageJobVisibleClustersCounterBufferPhase2,
                extension.m_reyesClassifyIndirectArgsBufferPhase2)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeClassifyPass2"),
            std::make_shared<ReyesClassifyPass>(
                extension.m_swPageJobVisibleClustersBufferPhase2,
                extension.m_swPageJobVisibleClustersCounterBufferPhase2,
                nullptr,
                extension.m_reyesFullClusterOutputsBuffer,
                extension.m_reyesFullClusterOutputsCounterBuffer,
                extension.m_reyesFullClusterOutputCapacity,
                extension.m_reyesOwnedClustersBuffer,
                extension.m_reyesOwnedClustersCounterBuffer,
                extension.m_reyesOwnedClusterCapacity,
                nullptr,
                extension.m_reyesClassifyIndirectArgsBufferPhase2,
                extension.m_reyesTelemetryBufferPhase2,
                2u,
                ReyesClassifyMode::ShadowCoarseLargeOnly)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeCreateSeedDispatchArgsPass2"),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                extension.m_reyesOwnedClustersCounterBuffer,
                extension.m_reyesSplitIndirectArgsBufferPhase2)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeSeedPatchesPass2"),
            std::make_shared<ReyesSeedPatchesPass>(
                extension.m_swPageJobVisibleClustersBufferPhase2,
                extension.m_reyesOwnedClustersBuffer,
                extension.m_reyesOwnedClustersCounterBuffer,
                extension.m_reyesSplitQueueBufferA,
                extension.m_reyesSplitQueueCounterBufferA,
                extension.m_reyesSplitQueueOverflowBufferA,
                extension.m_reyesSplitIndirectArgsBufferPhase2,
                slabGroup,
                extension.m_reyesSplitQueueCapacity,
                2u)));

    const std::shared_ptr<Buffer> reyesLargeSplitBuffersPhase2[] = { extension.m_reyesSplitQueueBufferA, extension.m_reyesSplitQueueBufferB };
    const std::shared_ptr<Buffer> reyesLargeSplitCountersPhase2[] = { extension.m_reyesSplitQueueCounterBufferA, extension.m_reyesSplitQueueCounterBufferB };
    const std::shared_ptr<Buffer> reyesLargeSplitOverflowsPhase2[] = { extension.m_reyesSplitQueueOverflowBufferA, extension.m_reyesSplitQueueOverflowBufferB };
    for (uint32_t splitPassIndex = 0; splitPassIndex < CLodReyesMaxSplitPassCount; ++splitPassIndex) {
        const uint32_t inputIndex = splitPassIndex & 1u;
        const uint32_t outputIndex = inputIndex ^ 1u;

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesLargeCreateSplitDispatchArgsPass2_" + std::to_string(splitPassIndex)),
                std::make_shared<ReyesCreateDispatchArgsPass>(
                    reyesLargeSplitCountersPhase2[inputIndex],
                    extension.m_reyesSplitIndirectArgsBufferPhase2)));

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                MakeVariantPassName(traits, "ReyesLargeSplitPass2_" + std::to_string(splitPassIndex)),
                std::make_shared<ReyesSplitPass>(
                    extension.m_swPageJobVisibleClustersBufferPhase2,
                    reyesLargeSplitBuffersPhase2[inputIndex],
                    reyesLargeSplitCountersPhase2[inputIndex],
                    reyesLargeSplitBuffersPhase2[outputIndex],
                    reyesLargeSplitCountersPhase2[outputIndex],
                    reyesLargeSplitOverflowsPhase2[outputIndex],
                    extension.m_reyesDiceQueueBuffer,
                    extension.m_reyesDiceQueueCounterBuffer,
                    extension.m_reyesDiceQueueOverflowBuffer,
                    extension.m_reyesTessTableConfigsBuffer,
                    extension.m_reyesTessTableVerticesBuffer,
                    extension.m_reyesTessTableTrianglesBuffer,
                    extension.m_shadowClipmapInfoBuffer,
                    extension.m_shadowDirtyPageHierarchyTexture,
                    extension.m_shadowNonRasterablePageHierarchyTexture,
                    extension.m_reyesSplitIndirectArgsBufferPhase2,
                    extension.m_reyesTelemetryBufferPhase2,
                    extension.m_reyesSplitQueueCapacity,
                    splitPassIndex,
                    CLodReyesMaxSplitPassCount,
                    2u)));
    }

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeCreateDiceDispatchArgsPass2"),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                extension.m_reyesDiceQueueCounterBuffer,
                extension.m_reyesDiceIndirectArgsBufferPhase2)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeDicePass2"),
            std::make_shared<ReyesDicePass>(
                extension.m_reyesDiceQueueBuffer,
                extension.m_reyesDiceQueueCounterBuffer,
                nullptr,
                extension.m_reyesTessTableConfigsBuffer,
                extension.m_reyesDiceIndirectArgsBufferPhase2,
                extension.m_reyesTelemetryBufferPhase2,
                extension.m_reyesDiceQueueCapacity,
                2u)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeBuildRasterWorkPass2"),
            std::make_shared<ReyesBuildRasterWorkPass>(
                extension.m_reyesDiceQueueBuffer,
                extension.m_reyesDiceQueueCounterBuffer,
                nullptr,
                extension.m_reyesTessTableConfigsBuffer,
                extension.m_reyesRasterWorkBufferPhase2,
                extension.m_reyesRasterWorkCounterBufferPhase2,
                extension.m_reyesDiceIndirectArgsBufferPhase2,
                extension.m_reyesTelemetryBufferPhase2,
                extension.m_reyesRasterWorkCapacity)));

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Compute(
            MakeVariantPassName(traits, "ReyesLargeCreateRasterWorkDispatchArgsPass2"),
            std::make_shared<ReyesCreateDispatchArgsPass>(
                extension.m_reyesRasterWorkCounterBufferPhase2,
                extension.m_reyesRasterWorkIndirectArgsBufferPhase2)));

    const std::string reyesLargeShadowRasterPassName = MakeVariantPassName(traits, "ReyesLargeVirtualShadowRasterPass2");
    auto reyesLargeShadowRasterPassDesc = RenderGraph::ExternalPassDesc::Compute(
        reyesLargeShadowRasterPassName,
        std::make_shared<ReyesVirtualShadowRasterizationPass>(
            extension.m_swPageJobVisibleClustersBufferPhase2,
            extension.m_swPageJobVisibleClusterTransformIndicesBufferPhase2,
            extension.m_reyesDiceQueueBuffer,
            extension.m_reyesDiceQueueCounterBuffer,
            extension.m_reyesRasterWorkBufferPhase2,
            extension.m_reyesRasterWorkCounterBufferPhase2,
            extension.m_reyesTessTableConfigsBuffer,
            extension.m_reyesTessTableVerticesBuffer,
            extension.m_reyesTessTableTrianglesBuffer,
            extension.m_reyesRasterWorkIndirectArgsBufferPhase2,
            extension.m_reyesTelemetryBufferPhase2,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPhysicalPagesTexture,
            extension.m_shadowClipmapInfoBuffer,
            slabGroup,
            MakeVariantResourceName(traits, "Reyes Virtual Shadow View Raster Info Buffer Phase2 Large"),
            2u));
    reyesLargeShadowRasterPassDesc.At(RenderGraph::ExternalInsertPoint::Before("MaterialHistogramPass"));
    outPasses.push_back(std::move(reyesLargeShadowRasterPassDesc));

    return reyesLargeShadowRasterPassName;
}

std::shared_ptr<Resource> CLodShadowVariant::ProvideResource(CLodExtension& extension, ResourceIdentifier const& key)
{
    if (extension.m_type != CLodExtensionType::Shadow || !AreRendererShadowsEnabled()) {
        return nullptr;
    }

    if (key == Builtin::Shadows::CLodPageTable) {
        return extension.m_shadowPageTableTexture;
    }

    if (key == Builtin::Shadows::CLodPhysicalPages) {
        return extension.m_shadowPhysicalPagesTexture;
    }

    if (key == Builtin::Shadows::CLodClipmapInfo) {
        return extension.m_shadowClipmapInfoBuffer;
    }

    if (key == Builtin::Shadows::CLodCompactMainCamera) {
        return extension.m_shadowCompactMainCameraBuffer;
    }

    if (key == Builtin::Shadows::CLodCompactShadowCameras) {
        return extension.m_shadowCompactShadowCameraBuffer;
    }

    if (key == Builtin::Shadows::CLodDirectionalPageViewInfo) {
        return extension.m_shadowDirectionalPageViewInfoBuffer;
    }

    if (key == Builtin::Shadows::CLodPageMetadata) {
        return extension.m_shadowPageMetadataBuffer;
    }

    return nullptr;
}

std::vector<ResourceIdentifier> CLodShadowVariant::GetSupportedKeys(const CLodExtension& extension)
{
    if (extension.m_type != CLodExtensionType::Shadow || !AreRendererShadowsEnabled()) {
        return {};
    }

    return {
        Builtin::Shadows::CLodPageTable,
        Builtin::Shadows::CLodPhysicalPages,
        Builtin::Shadows::CLodClipmapInfo,
        Builtin::Shadows::CLodCompactMainCamera,
        Builtin::Shadows::CLodCompactShadowCameras,
        Builtin::Shadows::CLodDirectionalPageViewInfo,
        Builtin::Shadows::CLodPageMetadata,
    };
}

void CLodShadowVariant::InitializeResources(CLodExtension& extension)
{
    const auto& traits = GetVariantTraits(extension.m_type);
    if (traits.type != CLodExtensionType::Shadow) {
        return;
    }

    auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
    const flecs::entity typeEntity = traits.ensureTypeEntity(ecsWorld);
    const uint32_t maxShadowPhysicalPageCount = extension.m_shadowConfiguredMaxPhysicalPageCount;
    const uint32_t pageJobRecordCapacity = extension.m_shadowConfiguredPageJobRecordCapacity;
    const uint32_t vsmExpandedRecordCapacity = extension.m_shadowConfiguredExpandedRecordCapacity;
    const uint32_t shadowAtlasPagesWide = CLodVirtualShadowPhysicalAtlasPagesWideFromPhysicalPageCount(
        maxShadowPhysicalPageCount,
        extension.m_shadowConfiguredBackingResolution);
    const uint32_t shadowAtlasPagesHigh = CLodVirtualShadowPhysicalAtlasPagesHighFromPhysicalPageCount(
        maxShadowPhysicalPageCount,
        extension.m_shadowConfiguredBackingResolution);

    spdlog::info(
        "{}Shadow sizing: backingResolution={} physicalAtlas={}x{} pages={} texture={}x{} computeClusters={} expandedRecords={} pageJobRecords={} pageJobMaxPages={}",
        traits.resourcePrefix,
        extension.m_shadowConfiguredBackingResolution,
        shadowAtlasPagesWide,
        shadowAtlasPagesHigh,
        maxShadowPhysicalPageCount,
        shadowAtlasPagesWide * CLodVirtualShadowPhysicalPageSize,
        shadowAtlasPagesHigh * CLodVirtualShadowPhysicalPageSize,
        extension.m_shadowConfiguredComputeClusterCapacity,
        vsmExpandedRecordCapacity,
        pageJobRecordCapacity,
        extension.m_shadowConfiguredPageJobMaxPages);

    extension.m_shadowPageTableTexture = PixelBuffer::CreateSharedUnmaterialized(CreateVirtualShadowPageTableDescription());
    extension.m_shadowPageTableTexture->SetName(MakeVariantResourceName(traits, "Virtual Shadow Page Table"));
    extension.m_shadowPageTableTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPageTableTexture })
        .add<CLodVirtualShadowPageTableTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPhysicalPagesTexture = PixelBuffer::CreateSharedUnmaterialized(
        CreateVirtualShadowPhysicalPagesDescription(
            extension.m_shadowConfiguredBackingResolution,
            maxShadowPhysicalPageCount));
    extension.m_shadowPhysicalPagesTexture->SetName(MakeVariantResourceName(traits, "Virtual Shadow Physical Pages"));
    extension.m_shadowPhysicalPagesTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPhysicalPagesTexture })
        .add<CLodVirtualShadowPhysicalPagesTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPageMetadataBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        maxShadowPhysicalPageCount,
        sizeof(CLodVirtualShadowPhysicalPageMeta),
        true,
        false,
        false,
        false);
    extension.m_shadowPageMetadataBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Page Metadata Buffer"));
    extension.m_shadowPageMetadataBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPageMetadataBuffer })
        .add<CLodVirtualShadowPageMetadataTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowInvalidationInputsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxInvalidationInputs,
        sizeof(CLodVirtualShadowInvalidationInput),
        false,
        false,
        false);
    extension.m_shadowInvalidationInputsBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Invalidation Inputs Buffer"));
    extension.m_shadowInvalidationInputsBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowInvalidationInputsBuffer })
        .add<CLodVirtualShadowInvalidationInputsTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowInvalidationCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), false, false, false);
    extension.m_shadowInvalidationCountBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Invalidation Count Buffer"));
    extension.m_shadowInvalidationCountBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowInvalidationCountBuffer })
        .add<CLodVirtualShadowInvalidationCountTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowInvalidatedInstancesBitsetBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMovedInstanceBitWordCount(),
        sizeof(uint32_t),
        false,
        false,
        false);
    extension.m_shadowInvalidatedInstancesBitsetBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Invalidated Instances Bitset Buffer"));

    extension.m_shadowUpgradeInvalidationInputsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxInvalidationInputs,
        sizeof(CLodVirtualShadowUpgradeInvalidationInput),
        false,
        false,
        false);
    extension.m_shadowUpgradeInvalidationInputsBuffer->SetName(
        MakeVariantResourceName(traits, "Virtual Shadow Upgrade Invalidation Inputs Buffer"));
    extension.m_shadowUpgradeInvalidationInputsBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowUpgradeInvalidationInputsBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowUpgradeInvalidationCountBuffer =
        CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), false, false, false);
    extension.m_shadowUpgradeInvalidationCountBuffer->SetName(
        MakeVariantResourceName(traits, "Virtual Shadow Upgrade Invalidation Count Buffer"));
    extension.m_shadowUpgradeInvalidationCountBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowUpgradeInvalidationCountBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPredictiveInvalidationCandidatesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowPredictiveCandidateCapacity,
        sizeof(CLodVirtualShadowPredictiveInvalidationCandidate),
        true,
        false,
        false);
    extension.m_shadowPredictiveInvalidationCandidatesBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Predictive Invalidation Candidates Buffer"));
    extension.m_shadowPredictiveInvalidationCandidatesBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPredictiveInvalidationCandidatesBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPredictiveInvalidationCandidateCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    extension.m_shadowPredictiveInvalidationCandidateCountBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Predictive Invalidation Candidate Count Buffer"));
    extension.m_shadowPredictiveInvalidationCandidateCountBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPredictiveInvalidationCandidateCountBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPredictiveRawPagesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowPredictiveRawPageCapacity,
        sizeof(CLodVirtualShadowPredictedRawPage),
        true,
        false,
        false);
    extension.m_shadowPredictiveRawPagesBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Predictive Raw Pages Buffer"));
    extension.m_shadowPredictiveRawPagesBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPredictiveRawPagesBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPredictiveRawPageCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    extension.m_shadowPredictiveRawPageCountBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Predictive Raw Page Count Buffer"));
    extension.m_shadowPredictiveRawPageCountBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPredictiveRawPageCountBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPredictedInvalidationScratchBitsetBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowFallbackDependencyHashCapacity,
        sizeof(uint64_t),
        true,
        false,
        false,
        true);
    extension.m_shadowPredictedInvalidationScratchBitsetBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Predicted Invalidation Scratch Bitset Buffer"));
    extension.m_shadowPredictedInvalidationScratchBitsetBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPredictedInvalidationScratchBitsetBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPredictedInvalidationPagesBufferA = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowPredictedPageListCapacity(),
        sizeof(CLodVirtualShadowPredictedPage),
        true,
        false,
        false,
        false);
    extension.m_shadowPredictedInvalidationPagesBufferA->SetName(MakeVariantResourceName(traits, "Virtual Shadow Predicted Invalidation Pages Buffer"));
    extension.m_shadowPredictedInvalidationPagesBufferA->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPredictedInvalidationPagesBufferA })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPredictedInvalidationPageCountBufferA = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    extension.m_shadowPredictedInvalidationPageCountBufferA->SetName(MakeVariantResourceName(traits, "Virtual Shadow Predicted Invalidation Page Count Buffer"));
    extension.m_shadowPredictedInvalidationPageCountBufferA->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPredictedInvalidationPageCountBufferA })
        .add<CLodExtensionTypeTag>(typeEntity);
    if (extension.m_streamingSystem) {
        extension.m_streamingSystem->SetVirtualShadowFallbackFeedbackResources(
            extension.m_shadowPredictedInvalidationPagesBufferA,
            extension.m_shadowPredictedInvalidationPageCountBufferA);
    }

    extension.m_shadowAllocationRequestsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxAllocationRequests,
        sizeof(CLodVirtualShadowPageAllocationRequest),
        true,
        false,
        false,
        true);
    extension.m_shadowAllocationRequestsBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Allocation Requests Buffer"));
    extension.m_shadowAllocationRequestsBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowAllocationRequestsBuffer })
        .add<CLodVirtualShadowAllocationRequestsTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowAllocationCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    extension.m_shadowAllocationCountBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Allocation Count Buffer"));
    extension.m_shadowAllocationCountBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowAllocationCountBuffer })
        .add<CLodVirtualShadowAllocationCountTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowAllocationIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
    extension.m_shadowAllocationIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Allocation Indirect Args Buffer"));

    extension.m_shadowMarkTileWorkBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxMarkTileCount,
        sizeof(CLodVirtualShadowMarkTileWorkItem),
        true,
        false,
        false,
        true);
    extension.m_shadowMarkTileWorkBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Mark Tile Work Buffer"));

    extension.m_shadowMarkTileCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    extension.m_shadowMarkTileCountBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Mark Tile Count Buffer"));

    extension.m_shadowMarkTileIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodReyesDispatchIndirectCommand), true, false, false);
    extension.m_shadowMarkTileIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Mark Tile Indirect Args Buffer"));

    extension.m_shadowMarkedBlocksMaskBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxMarkedBlockCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowMarkedBlocksMaskBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Marked Blocks Mask Buffer"));

    extension.m_shadowMarkedBlocksListBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxMarkedBlockCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowMarkedBlocksListBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Marked Blocks List Buffer"));

    extension.m_shadowMarkedBlocksCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false);
    extension.m_shadowMarkedBlocksCountBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Marked Blocks Count Buffer"));

    extension.m_shadowActiveBlockMetadataBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxMarkedBlockCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowActiveBlockMetadataBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Active Block Metadata Buffer"));

    extension.m_shadowBlockClusterCoverageBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        extension.m_shadowConfiguredComputeClusterCapacity,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowBlockClusterCoverageBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Block Cluster Coverage Buffer"));

    extension.m_shadowFreePhysicalPagesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        maxShadowPhysicalPageCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowFreePhysicalPagesBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Free Physical Pages Buffer"));
    extension.m_shadowFreePhysicalPagesBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowFreePhysicalPagesBuffer })
        .add<CLodVirtualShadowFreePhysicalPagesTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowReusablePhysicalPagesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        maxShadowPhysicalPageCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowReusablePhysicalPagesBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Reusable Physical Pages Buffer"));
    extension.m_shadowReusablePhysicalPagesBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowReusablePhysicalPagesBuffer })
        .add<CLodVirtualShadowReusablePhysicalPagesTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowPageListHeaderBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodVirtualShadowPageListHeader), true, false, false, false);
    extension.m_shadowPageListHeaderBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Page List Header Buffer"));
    extension.m_shadowPageListHeaderBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPageListHeaderBuffer })
        .add<CLodVirtualShadowPageListHeaderTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowDirtyPageFlagsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowDirtyWordCount(maxShadowPhysicalPageCount),
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowDirtyPageFlagsBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Dirty Page Flags Buffer"));
    extension.m_shadowDirtyPageFlagsBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowDirtyPageFlagsBuffer })
        .add<CLodVirtualShadowDirtyPageFlagsTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowDirtyPageHierarchyTexture = PixelBuffer::CreateSharedUnmaterialized(CreateVirtualShadowDirtyHierarchyDescription());
    extension.m_shadowDirtyPageHierarchyTexture->SetName(MakeVariantResourceName(traits, "Virtual Shadow Dirty Hierarchy"));
    extension.m_shadowDirtyPageHierarchyTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowDirtyPageHierarchyTexture })
        .add<CLodVirtualShadowDirtyHierarchyTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowNonRasterablePageHierarchyTexture = PixelBuffer::CreateSharedUnmaterialized(CreateVirtualShadowDirtyHierarchyDescription());
    extension.m_shadowNonRasterablePageHierarchyTexture->SetName(MakeVariantResourceName(traits, "Virtual Shadow Non-Rasterable Hierarchy"));
    extension.m_shadowNonRasterablePageHierarchyTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowNonRasterablePageHierarchyTexture })
        .add<CLodVirtualShadowNonRasterableHierarchyTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowClipmapInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxSupportedClipmapCount,
        sizeof(CLodVirtualShadowClipmapInfo),
        true,
        false,
        false,
        false);
    extension.m_shadowClipmapInfoBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Clipmap Info Buffer"));
    extension.m_shadowClipmapInfoBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowClipmapInfoBuffer })
        .add<CLodVirtualShadowClipmapInfoTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowMarkClipmapDataBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxSupportedClipmapCount,
        sizeof(CLodVirtualShadowMarkClipmapData),
        true,
        false,
        false,
        false);
    extension.m_shadowMarkClipmapDataBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Mark Clipmap Data Buffer"));
    extension.m_shadowMarkClipmapDataBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowMarkClipmapDataBuffer })
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowCompactMainCameraBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        1,
        sizeof(CLodVirtualShadowMainCameraInfo),
        true,
        false,
        false,
        false);
    extension.m_shadowCompactMainCameraBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Compact Main Camera Buffer"));

    extension.m_shadowCompactShadowCameraBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxSupportedClipmapCount,
        sizeof(CLodVirtualShadowCompactShadowCameraInfo),
        true,
        false,
        false,
        false);
    extension.m_shadowCompactShadowCameraBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Compact Shadow Camera Buffer"));

    extension.m_shadowDirectionalPageViewInfoBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxDirectionalPageViewInfoEntryCount(),
        sizeof(float) * 4u,
        true,
        false,
        false,
        false);
    extension.m_shadowDirectionalPageViewInfoBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Directional Page View Info Buffer"));
    extension.m_shadowDirectionalPageViewInfoBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowDirectionalPageViewInfoBuffer })
        .add<CLodVirtualShadowDirectionalPageViewInfoTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowRuntimeStateBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodVirtualShadowRuntimeState), true, false, false);
    extension.m_shadowRuntimeStateBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Runtime State Buffer"));
    extension.m_shadowRuntimeStateBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowRuntimeStateBuffer })
        .add<CLodVirtualShadowRuntimeStateTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_shadowStatsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodVirtualShadowStats), true, false, false, true);
    extension.m_shadowStatsBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Stats Buffer"));
    extension.m_shadowStatsBuffer->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowStatsBuffer })
        .add<CLodVirtualShadowStatsTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    extension.m_swPageJobVisibleClustersBuffer = CreateAliasedUnmaterializedRawBuffer(extension.m_maxVisibleClusters * PackedVisibleClusterStrideBytes, true, false, true);
    extension.m_swPageJobVisibleClustersBuffer->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Visible Clusters Buffer"));

    extension.m_swPageJobVisibleClusterTransformIndicesBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        extension.m_maxVisibleClusters,
        sizeof(uint32_t),
        true,
        false,
        false,
        true);
    extension.m_swPageJobVisibleClusterTransformIndicesBuffer->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Visible Cluster Transform Indices Buffer"));

    extension.m_swPageJobVisibleClustersCounterBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    extension.m_swPageJobVisibleClustersCounterBuffer->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Visible Clusters Counter Buffer"));

    extension.m_swPageJobVisibleClustersBufferPhase2 = extension.m_swPageJobVisibleClustersBuffer;
    extension.m_swPageJobVisibleClusterTransformIndicesBufferPhase2 = extension.m_swPageJobVisibleClusterTransformIndicesBuffer;
    extension.m_swPageJobVisibleClustersCounterBufferPhase2 = extension.m_swPageJobVisibleClustersCounterBuffer;

    extension.m_swPageJobRecordsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        pageJobRecordCapacity,
        sizeof(CLodSoftwareRasterPageJobRecord),
        true,
        false,
        false,
        true);
    extension.m_swPageJobRecordsBuffer->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Records Buffer"));
    extension.m_swPageJobRecordsBufferSkinned = CreateAliasedUnmaterializedStructuredBuffer(
        pageJobRecordCapacity,
        sizeof(CLodSoftwareRasterPageJobRecord),
        true,
        false,
        false,
        true);
    extension.m_swPageJobRecordsBufferSkinned->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Records Buffer Skinned"));

    extension.m_swPageJobRecordsBufferPhase2 = extension.m_swPageJobRecordsBuffer;
    extension.m_swPageJobRecordsBufferPhase2Skinned = extension.m_swPageJobRecordsBufferSkinned;

    extension.m_swPageJobCountBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    extension.m_swPageJobCountBuffer->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Count Buffer"));
    extension.m_swPageJobCountBufferSkinned = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    extension.m_swPageJobCountBufferSkinned->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Count Buffer Skinned"));

    extension.m_swPageJobCountBufferPhase2 = extension.m_swPageJobCountBuffer;
    extension.m_swPageJobCountBufferPhase2Skinned = extension.m_swPageJobCountBufferSkinned;

    extension.m_swPageJobIndirectArgsBuffer = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, false);
    extension.m_swPageJobIndirectArgsBuffer->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Indirect Args Buffer Phase1"));
    extension.m_swPageJobIndirectArgsBufferSkinned = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(RasterizeClustersCommand), true, false, false, false);
    extension.m_swPageJobIndirectArgsBufferSkinned->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Indirect Args Buffer Phase1 Skinned"));

    extension.m_swPageJobIndirectArgsBufferPhase2 = extension.m_swPageJobIndirectArgsBuffer;
    extension.m_swPageJobIndirectArgsBufferPhase2Skinned = extension.m_swPageJobIndirectArgsBufferSkinned;

    extension.m_swPageJobClusterTagsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        extension.m_maxVisibleClusters,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_swPageJobClusterTagsBuffer->SetName(MakeVariantResourceName(traits, "Software Raster Page Job Cluster Tags Buffer"));

    extension.m_swPageJobClusterTagsBufferPhase2 = extension.m_swPageJobClusterTagsBuffer;

    extension.m_vsmExpandedVisibleClustersBuffer = CreateAliasedUnmaterializedRawBuffer(
        static_cast<uint64_t>(vsmExpandedRecordCapacity) * PackedVisibleClusterStrideBytes,
        true,
        false);
    extension.m_vsmExpandedVisibleClustersBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Expanded Visible Clusters Buffer"));

    extension.m_vsmExpandedVisibleClustersBufferSw = extension.m_vsmExpandedVisibleClustersBuffer;

    extension.m_vsmExpandedVisibleClusterTransformIndicesBufferSw = CreateAliasedUnmaterializedStructuredBuffer(
        vsmExpandedRecordCapacity,
        sizeof(uint32_t),
        true,
        false,
        false,
        true);
    extension.m_vsmExpandedVisibleClusterTransformIndicesBufferSw->SetName(MakeVariantResourceName(traits, "Virtual Shadow Expanded Visible Cluster Transform Indices Buffer SW"));

    extension.m_shadowVirtualResourcesNeedReset = true;
}

void CLodShadowVariant::TagResourceUsages(CLodExtension& extension)
{
    auto tagBufferUsage = [](const std::shared_ptr<Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            rg::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };
    auto tagTextureUsage = [](const std::shared_ptr<PixelBuffer>& texture, std::string_view usage) {
        if (texture) {
            rg::memory::SetResourceUsageHint(*texture, std::string(usage));
        }
    };

    tagTextureUsage(extension.m_shadowPageTableTexture, "Cluster LOD virtual shadow maps");
    tagTextureUsage(extension.m_shadowPhysicalPagesTexture, "Cluster LOD virtual shadow maps");
    tagTextureUsage(extension.m_shadowDirtyPageHierarchyTexture, "Cluster LOD virtual shadow maps");
    tagTextureUsage(extension.m_shadowNonRasterablePageHierarchyTexture, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPageMetadataBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowInvalidationInputsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowInvalidationCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowUpgradeInvalidationInputsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowUpgradeInvalidationCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowInvalidatedInstancesBitsetBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPredictiveInvalidationCandidatesBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPredictiveInvalidationCandidateCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPredictiveRawPagesBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPredictiveRawPageCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPredictedInvalidationScratchBitsetBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPredictedInvalidationPagesBufferA, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPredictedInvalidationPageCountBufferA, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowAllocationRequestsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowAllocationCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowAllocationIndirectArgsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowMarkTileWorkBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowMarkTileCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowMarkTileIndirectArgsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowMarkedBlocksMaskBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowMarkedBlocksListBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowMarkedBlocksCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowActiveBlockMetadataBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowBlockClusterCoverageBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowFreePhysicalPagesBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowReusablePhysicalPagesBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPageListHeaderBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowDirtyPageFlagsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowClipmapInfoBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowMarkClipmapDataBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowCompactMainCameraBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowCompactShadowCameraBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowDirectionalPageViewInfoBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowRuntimeStateBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowStatsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobVisibleClustersBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobVisibleClusterTransformIndicesBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobVisibleClustersCounterBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobVisibleClustersBufferPhase2, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobVisibleClusterTransformIndicesBufferPhase2, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobVisibleClustersCounterBufferPhase2, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobRecordsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobRecordsBufferSkinned, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobCountBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobCountBufferSkinned, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobRecordsBufferPhase2, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobRecordsBufferPhase2Skinned, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobCountBufferPhase2, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobCountBufferPhase2Skinned, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobIndirectArgsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobIndirectArgsBufferSkinned, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobIndirectArgsBufferPhase2, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobIndirectArgsBufferPhase2Skinned, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobClusterTagsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_swPageJobClusterTagsBufferPhase2, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_vsmExpandedVisibleClustersBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_vsmExpandedVisibleClustersBufferSw, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_vsmExpandedVisibleClusterTransformIndicesBufferSw, "Cluster LOD virtual shadow maps");
}

void CLodShadowVariant::ReleaseResourceBackings(CLodExtension& extension)
{
    std::unordered_set<Buffer*> releasedBuffers;
    std::unordered_set<PixelBuffer*> releasedTextures;
    auto releaseBufferBacking = [&releasedBuffers](const std::shared_ptr<Buffer>& buffer) {
        if (buffer && releasedBuffers.insert(buffer.get()).second) {
            buffer->Dematerialize();
        }
    };
    auto releaseTextureBacking = [&releasedTextures](const std::shared_ptr<PixelBuffer>& texture) {
        if (texture && releasedTextures.insert(texture.get()).second) {
            texture->Dematerialize();
        }
    };

    releaseTextureBacking(extension.m_shadowPageTableTexture);
    releaseTextureBacking(extension.m_shadowPhysicalPagesTexture);
    releaseTextureBacking(extension.m_shadowDirtyPageHierarchyTexture);
    releaseTextureBacking(extension.m_shadowNonRasterablePageHierarchyTexture);
    releaseBufferBacking(extension.m_swPageJobVisibleClustersBuffer);
    releaseBufferBacking(extension.m_swPageJobVisibleClusterTransformIndicesBuffer);
    releaseBufferBacking(extension.m_swPageJobVisibleClustersCounterBuffer);
    releaseBufferBacking(extension.m_swPageJobVisibleClustersBufferPhase2);
    releaseBufferBacking(extension.m_swPageJobVisibleClusterTransformIndicesBufferPhase2);
    releaseBufferBacking(extension.m_swPageJobVisibleClustersCounterBufferPhase2);
    releaseBufferBacking(extension.m_swPageJobRecordsBuffer);
    releaseBufferBacking(extension.m_swPageJobRecordsBufferSkinned);
    releaseBufferBacking(extension.m_swPageJobCountBuffer);
    releaseBufferBacking(extension.m_swPageJobCountBufferSkinned);
    releaseBufferBacking(extension.m_swPageJobRecordsBufferPhase2);
    releaseBufferBacking(extension.m_swPageJobRecordsBufferPhase2Skinned);
    releaseBufferBacking(extension.m_swPageJobCountBufferPhase2);
    releaseBufferBacking(extension.m_swPageJobCountBufferPhase2Skinned);
    releaseBufferBacking(extension.m_swPageJobIndirectArgsBuffer);
    releaseBufferBacking(extension.m_swPageJobIndirectArgsBufferSkinned);
    releaseBufferBacking(extension.m_swPageJobIndirectArgsBufferPhase2);
    releaseBufferBacking(extension.m_swPageJobIndirectArgsBufferPhase2Skinned);
    releaseBufferBacking(extension.m_swPageJobClusterTagsBuffer);
    releaseBufferBacking(extension.m_swPageJobClusterTagsBufferPhase2);
    releaseBufferBacking(extension.m_vsmExpandedVisibleClustersBuffer);
    releaseBufferBacking(extension.m_vsmExpandedVisibleClustersBufferSw);
    releaseBufferBacking(extension.m_vsmExpandedVisibleClusterTransformIndicesBufferSw);
    extension.m_shadowVirtualResourcesNeedReset = true;
}

void CLodShadowVariant::RefreshResourcesForCurrentSettings(CLodExtension& extension)
{
    if (extension.m_type != CLodExtensionType::Shadow) {
        return;
    }

    const uint32_t previousBackingResolution = extension.m_shadowConfiguredBackingResolution;
    const uint32_t previousMaxPhysicalPageCount = extension.m_shadowConfiguredMaxPhysicalPageCount;
    const uint32_t previousPageJobMaxPages = extension.m_shadowConfiguredPageJobMaxPages;
    const uint32_t previousPageJobRecordCapacity = extension.m_shadowConfiguredPageJobRecordCapacity;
    const uint32_t previousComputeClusterCapacity = extension.m_shadowConfiguredComputeClusterCapacity;
    const uint32_t previousVisibleClusterCapacity = extension.m_visibleClusterCapacity;

    RefreshConfiguredSettings(extension);

    if (extension.m_shadowPhysicalPagesTexture &&
        previousBackingResolution == extension.m_shadowConfiguredBackingResolution &&
        previousMaxPhysicalPageCount == extension.m_shadowConfiguredMaxPhysicalPageCount &&
        previousPageJobMaxPages == extension.m_shadowConfiguredPageJobMaxPages &&
        previousPageJobRecordCapacity == extension.m_shadowConfiguredPageJobRecordCapacity &&
        previousComputeClusterCapacity == extension.m_shadowConfiguredComputeClusterCapacity &&
        previousVisibleClusterCapacity == GetVisibleClusterCapacity(extension)) {
        return;
    }

    extension.RefreshCoreVisibleClusterCapacity();

    ReleaseResourceBackings(extension);
    InitializeResources(extension);
    TagResourceUsages(extension);
    extension.m_shadowVirtualResourcesNeedReset = true;
}
