#include "VirtualShadows/GraphIntegration/CLodShadowVariant.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "BuiltinResources.h"
#include "Scene/ECS/RendererECSManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "VirtualGeometry/GraphIntegration/CLodExtension.h"
#include <BasicRenderer/Extensions/ProducerPersistentState.h>
#include "VirtualGeometry/GraphIntegration/CLodExtensionShared.h"
#include <BasicRenderer/Extensions/VirtualShadowCasterProvider.h>
#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketBlockOffsetsPass.h"
#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketBlockScanPass.h"
#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketCompactAndArgsPass.h"
#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketCreateCommandPass.h"
#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketHistogramPass.h"
#include "VirtualGeometry/Reyes/RenderPasses/ReyesBuildRasterWorkPass.h"
#include "VirtualGeometry/Reyes/RenderPasses/ReyesClassifyPass.h"
#include "VirtualGeometry/Reyes/RenderPasses/ReyesCreateDispatchArgsPass.h"
#include "VirtualGeometry/Reyes/RenderPasses/ReyesDicePass.h"
#include "VirtualGeometry/Reyes/RenderPasses/ReyesQueueResetPass.h"
#include "VirtualGeometry/Reyes/RenderPasses/ReyesSeedPatchesPass.h"
#include "VirtualGeometry/Reyes/RenderPasses/ReyesSplitPass.h"
#include "VirtualShadows/Rasterization/RenderPasses/ReyesVirtualShadowRasterizationPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapAllocatePagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapDeduplicatePredictedPagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapExpandPredictedPagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapAdmitPagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildMarkTilesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildPageListsPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapClearDirtyBitsPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapClearPagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapComposePagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildActiveBlocksPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapDirtyHierarchyPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapFreeWrappedPagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapGatherStatsPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapInvalidatePagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapMarkPagesPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapResolveMarkedBlocksPass.h"
#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapSetupPass.h"
#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>

namespace {

bool AreRendererShadowsEnabled()
{
    return SettingsManager::GetInstance().getSettingGetter<bool>("enableShadows")();
}

org::ExternalInsertPoint MakeShadowTailInsertPoint(const std::string& afterPassName)
{
    auto insertPoint = org::ExternalInsertPoint::After(afterPassName);
    // The dynamic atlas contains current-pose skinned depth. Keep the complete
    // shadow producer chain ahead of every lighting consumer instead of
    // allowing deferred/forward shading to sample the previous frame.
    insertPoint.before.push_back("DeferredShadingPass");
    insertPoint.before.push_back("Forward render pass");
    insertPoint.before.push_back("Screen-Space Reflections Pass");
    insertPoint.before.push_back("luminanceHistogramPass");
    return insertPoint;
}

enum class ShadowRasterPhase : uint32_t {
    Phase1 = 1u,
    Phase2 = 2u,
};

} // namespace

VirtualShadowCasterBuildContext CLodShadowVariant::MakeCasterContext(CLodExtension& extension)
{
    VirtualShadowCasterBuildContext context{};
    context.pageTable = extension.m_shadowPageTableTexture;
    context.staticPhysicalPages = extension.m_shadowStaticPhysicalPagesTexture;
    context.dynamicPhysicalPages = extension.m_shadowPhysicalPagesTexture;
    context.clipmapInfo = extension.m_shadowClipmapInfoBuffer;
    context.compactShadowCameras = extension.m_shadowCompactShadowCameraBuffer;
    context.staticActiveBlockMetadata = extension.m_shadowActiveBlockMetadataBuffer;
    context.dynamicActiveBlockMetadata = extension.m_shadowDynamicActiveBlockMetadataBuffer;
    context.directionalPageViews = extension.m_shadowDirectionalPageViewInfoBuffer;
    context.statistics = extension.m_shadowStatsBuffer;
    if (extension.m_options.virtualShadowCasters) {
        context.invalidationQueue = extension.m_options.virtualShadowCasters->GetInvalidationQueue();
    }
    return context;
}

std::string CLodShadowVariant::AppendFineRasterPassForPhase(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<org::ResourceGroup>& slabGroup,
    std::vector<org::ExternalPassDesc>& outPasses,
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
        org::ExternalPassDesc::Compute(
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
                extension.m_shadowStaticPhysicalPagesTexture,
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
    std::vector<org::ExternalPassDesc>& outPasses)
{
    if (traits.type != CLodExtensionType::Shadow) {
        return {};
    }

    const std::string shadowSetupPassName = MakeVariantPassName(traits, "VirtualShadowSetupPass");
    auto shadowSetupPassDesc = org::ExternalPassDesc::Compute(
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
            extension.m_options.virtualShadowCasters,
            extension.m_shadowVirtualResourcesNeedReset));
    shadowSetupPassDesc.At(org::ExternalInsertPoint::After("CLod::StreamingBeginFramePass"));
    outPasses.push_back(std::move(shadowSetupPassDesc));
    extension.m_shadowVirtualResourcesNeedReset = false;

    const std::string shadowFreeWrappedPagesPassName = MakeVariantPassName(traits, "VirtualShadowFreeWrappedPagesPass");
    auto shadowFreeWrappedPagesPassDesc = org::ExternalPassDesc::Compute(
        shadowFreeWrappedPagesPassName,
        std::make_shared<VirtualShadowMapFreeWrappedPagesPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowStatsBuffer));
    shadowFreeWrappedPagesPassDesc.At(org::ExternalInsertPoint::After(shadowSetupPassName));
    outPasses.push_back(std::move(shadowFreeWrappedPagesPassDesc));

    const std::string shadowInvalidatePagesPassName = MakeVariantPassName(traits, "VirtualShadowInvalidatePagesPass");
    std::string invalidateAfterPassName = shadowFreeWrappedPagesPassName;
    if (extension.m_options.virtualShadowCasters && !extension.m_options.virtualShadowCasters->Empty()) {
        VirtualShadowPassBuilder preparationBuilder(
            outPasses,
            shadowFreeWrappedPagesPassName,
            shadowInvalidatePagesPassName);
        extension.m_options.virtualShadowCasters->GatherPreparationPasses(
            MakeCasterContext(extension),
            preparationBuilder);
        invalidateAfterPassName = preparationBuilder.LastPassName();
    }
    auto shadowInvalidatePagesPassDesc = org::ExternalPassDesc::Compute(
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
            extension.m_shadowStatsBuffer,
            extension.m_options.virtualShadowCasters
                ? extension.m_options.virtualShadowCasters->GetInvalidationQueue()
                : nullptr));
    shadowInvalidatePagesPassDesc.At(org::ExternalInsertPoint::After(invalidateAfterPassName));
    outPasses.push_back(std::move(shadowInvalidatePagesPassDesc));

    const std::string shadowMarkPagesPassName = MakeVariantPassName(traits, "VirtualShadowMarkPagesPass");
    const std::string shadowBuildMarkTilesPassName = MakeVariantPassName(traits, "VirtualShadowBuildMarkTilesPass");
    auto shadowBuildMarkTilesPassDesc = org::ExternalPassDesc::Compute(
        shadowBuildMarkTilesPassName,
        std::make_shared<VirtualShadowMapBuildMarkTilesPass>(
            extension.m_shadowMarkTileWorkBuffer,
            extension.m_shadowMarkTileCountBuffer));
    shadowBuildMarkTilesPassDesc.At(org::ExternalInsertPoint::After(shadowInvalidatePagesPassName));
    outPasses.push_back(std::move(shadowBuildMarkTilesPassDesc));

    const std::string shadowBuildMarkTileDispatchArgsPassName = MakeVariantPassName(traits, "VirtualShadowBuildMarkTileDispatchArgsPass");
    auto shadowBuildMarkTileDispatchArgsPassDesc = org::ExternalPassDesc::Compute(
        shadowBuildMarkTileDispatchArgsPassName,
        std::make_shared<ReyesCreateDispatchArgsPass>(
            extension.m_shadowMarkTileCountBuffer,
            extension.m_shadowMarkTileIndirectArgsBuffer,
            nullptr,
            128u,
            CLodVirtualShadowMaxMarkTileCount));
    shadowBuildMarkTileDispatchArgsPassDesc.At(org::ExternalInsertPoint::After(shadowBuildMarkTilesPassName));
    outPasses.push_back(std::move(shadowBuildMarkTileDispatchArgsPassDesc));

    auto shadowMarkPagesPassDesc = org::ExternalPassDesc::Compute(
        shadowMarkPagesPassName,
        std::make_shared<VirtualShadowMapMarkPagesPass>(
            extension.m_shadowMarkTileWorkBuffer,
            extension.m_shadowMarkTileCountBuffer,
            extension.m_shadowMarkTileIndirectArgsBuffer,
            extension.m_shadowMarkClipmapDataBuffer,
            extension.m_shadowMarkedBlocksMaskBuffer,
            extension.m_shadowMarkedBlocksListBuffer,
            extension.m_shadowMarkedBlocksCountBuffer,
            extension.m_shadowReceiverSubpageMaskBuffer));
    shadowMarkPagesPassDesc.At(org::ExternalInsertPoint::After(shadowBuildMarkTileDispatchArgsPassName));
    shadowMarkPagesPassDesc.preferredQueueKind = org::QueueKind::Graphics;
    outPasses.push_back(std::move(shadowMarkPagesPassDesc));

    const std::string shadowResolveMarkedBlocksPassName = MakeVariantPassName(traits, "VirtualShadowResolveMarkedBlocksPass");
    auto shadowResolveMarkedBlocksPassDesc = org::ExternalPassDesc::Compute(
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
    shadowResolveMarkedBlocksPassDesc.At(org::ExternalInsertPoint::After(shadowMarkPagesPassName));
    shadowResolveMarkedBlocksPassDesc.preferredQueueKind = org::QueueKind::Graphics;
    outPasses.push_back(std::move(shadowResolveMarkedBlocksPassDesc));

    const std::string shadowBuildPageListsPassName = MakeVariantPassName(traits, "VirtualShadowBuildPageListsPass");
    auto shadowBuildPageListsPassDesc = org::ExternalPassDesc::Compute(
        shadowBuildPageListsPassName,
        std::make_shared<VirtualShadowMapBuildPageListsPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowFreePhysicalPagesBuffer,
            extension.m_shadowReusablePhysicalPagesBuffer,
            extension.m_shadowPageListHeaderBuffer));
    shadowBuildPageListsPassDesc.At(org::ExternalInsertPoint::After(shadowResolveMarkedBlocksPassName));
    outPasses.push_back(std::move(shadowBuildPageListsPassDesc));

    const std::string shadowBuildDispatchArgsPassName = MakeVariantPassName(traits, "VirtualShadowBuildAllocationDispatchArgsPass");
    auto shadowBuildDispatchArgsPassDesc = org::ExternalPassDesc::Compute(
        shadowBuildDispatchArgsPassName,
        std::make_shared<ReyesCreateDispatchArgsPass>(
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowAllocationIndirectArgsBuffer,
            nullptr,
            64u,
            extension.m_shadowConfiguredMaxPhysicalPageCount));
    shadowBuildDispatchArgsPassDesc.At(org::ExternalInsertPoint::After(shadowBuildPageListsPassName));
    outPasses.push_back(std::move(shadowBuildDispatchArgsPassDesc));

    const std::string shadowPreAllocateStatsPassName = MakeVariantPassName(traits, "VirtualShadowPreAllocateStatsPass");
    auto shadowPreAllocateStatsPassDesc = org::ExternalPassDesc::Compute(
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
    shadowPreAllocateStatsPassDesc.At(org::ExternalInsertPoint::After(shadowBuildDispatchArgsPassName));
    outPasses.push_back(std::move(shadowPreAllocateStatsPassDesc));

    const std::string shadowAllocationPassName = MakeVariantPassName(traits, "VirtualShadowAllocatePagesPass");
    auto shadowAllocationPassDesc = org::ExternalPassDesc::Compute(
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
    shadowAllocationPassDesc.At(org::ExternalInsertPoint::After(shadowPreAllocateStatsPassName));
    outPasses.push_back(std::move(shadowAllocationPassDesc));

    const std::string shadowGatherStatsPassName = MakeVariantPassName(traits, "VirtualShadowGatherStatsPass");
    auto shadowGatherStatsPassDesc = org::ExternalPassDesc::Compute(
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
    shadowGatherStatsPassDesc.At(org::ExternalInsertPoint::After(shadowAllocationPassName));
    outPasses.push_back(std::move(shadowGatherStatsPassDesc));

    const std::string shadowAdmitPagesPassName = MakeVariantPassName(traits, "VirtualShadowAdmitPagesPass");
    auto shadowAdmitPagesPassDesc = org::ExternalPassDesc::Compute(
        shadowAdmitPagesPassName,
        std::make_shared<VirtualShadowMapAdmitPagesPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowUpgradeInvalidationUploadBuffers,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowCompactShadowCameraBuffer,
            extension.m_shadowStatsBuffer,
            extension.m_streamingSystem ? extension.m_streamingSystem->GetVirtualShadowUpgradeQueue()
                                        : VirtualShadowUpgradeQueue{}));
    shadowAdmitPagesPassDesc.At(org::ExternalInsertPoint::After(shadowGatherStatsPassName));
    outPasses.push_back(std::move(shadowAdmitPagesPassDesc));

    const std::string shadowClearPagesPassName = MakeVariantPassName(traits, "VirtualShadowClearPagesPass");
    auto shadowClearPagesPassDesc = org::ExternalPassDesc::Compute(
        shadowClearPagesPassName,
        std::make_shared<VirtualShadowMapClearPagesPass>(
            extension.m_shadowStaticPhysicalPagesTexture,
            extension.m_shadowPhysicalPagesTexture,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowPageTableTexture,
            extension.m_shadowPageMetadataBuffer,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowDirectionalPageViewInfoBuffer,
            extension.m_shadowStatsBuffer));
    shadowClearPagesPassDesc.At(org::ExternalInsertPoint::After(shadowAdmitPagesPassName));
    outPasses.push_back(std::move(shadowClearPagesPassDesc));

    const std::string shadowBuildActiveBlocksPassName = MakeVariantPassName(traits, "VirtualShadowBuildActiveBlocksPass");
    auto shadowBuildActiveBlocksPassDesc = org::ExternalPassDesc::Compute(
        shadowBuildActiveBlocksPassName,
        std::make_shared<VirtualShadowMapBuildActiveBlocksPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowClipmapInfoBuffer,
            extension.m_shadowActiveBlockMetadataBuffer));
    shadowBuildActiveBlocksPassDesc.At(org::ExternalInsertPoint::After(shadowClearPagesPassName));
    outPasses.push_back(std::move(shadowBuildActiveBlocksPassDesc));

    const std::string shadowBuildDynamicActiveBlocksPassName =
        MakeVariantPassName(
            traits, "VirtualShadowBuildDynamicActiveBlocksPass");
    auto shadowBuildDynamicActiveBlocksPassDesc =
        org::ExternalPassDesc::Compute(
            shadowBuildDynamicActiveBlocksPassName,
            std::make_shared<VirtualShadowMapBuildActiveBlocksPass>(
                extension.m_shadowPageTableTexture,
                extension.m_shadowClipmapInfoBuffer,
                extension.m_shadowDynamicActiveBlockMetadataBuffer,
                true));
    shadowBuildDynamicActiveBlocksPassDesc.At(
        org::ExternalInsertPoint::After(
            shadowBuildActiveBlocksPassName));
    outPasses.push_back(
        std::move(shadowBuildDynamicActiveBlocksPassDesc));

    const std::string shadowDirtyHierarchyPassName = MakeVariantPassName(traits, "VirtualShadowDirtyHierarchyPass");
    auto shadowDirtyHierarchyPassDesc = org::ExternalPassDesc::Compute(
        shadowDirtyHierarchyPassName,
        std::make_shared<VirtualShadowMapDirtyHierarchyPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowDirtyPageHierarchyTexture,
            extension.m_shadowClipmapInfoBuffer));
    shadowDirtyHierarchyPassDesc.At(
        org::ExternalInsertPoint::After(
            shadowBuildDynamicActiveBlocksPassName));
    outPasses.push_back(std::move(shadowDirtyHierarchyPassDesc));

    return shadowDirtyHierarchyPassName;
}

std::string CLodShadowVariant::AppendCasterRasterPasses(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    std::vector<org::ExternalPassDesc>& outPasses,
    const std::string& afterPassName)
{
    if (traits.type != CLodExtensionType::Shadow || afterPassName.empty() ||
        !extension.m_options.virtualShadowCasters || extension.m_options.virtualShadowCasters->Empty()) {
        return afterPassName;
    }

    VirtualShadowPassBuilder builder(
        outPasses,
        afterPassName,
        MakeVariantPassName(traits, "VirtualShadowFinalizeFallbackPagesPass"));
    extension.m_options.virtualShadowCasters->GatherRasterPasses(MakeCasterContext(extension), builder);
    return builder.LastPassName();
}

void CLodShadowVariant::AppendStructuralTail(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    std::vector<org::ExternalPassDesc>& outPasses,
    const std::string& shadowClearDirtyBitsAfterPassName)
{
    if (traits.type != CLodExtensionType::Shadow || shadowClearDirtyBitsAfterPassName.empty()) {
        return;
    }

    const std::string shadowExpandPredictedPagesPassName = MakeVariantPassName(traits, "VirtualShadowFinalizeFallbackPagesPass");
    spdlog::info("CLod shadow structural tail: construct {} begin", shadowExpandPredictedPagesPassName);
    auto shadowExpandPredictedPagesPassDesc = org::ExternalPassDesc::Compute(
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
    spdlog::info("CLod shadow structural tail: construct {} complete", shadowExpandPredictedPagesPassName);
    shadowExpandPredictedPagesPassDesc.At(
        org::ExternalInsertPoint::After(shadowClearDirtyBitsAfterPassName));
    outPasses.push_back(std::move(shadowExpandPredictedPagesPassDesc));

    const std::string shadowDeduplicatePredictedPagesPassName = MakeVariantPassName(traits, "VirtualShadowDeduplicateFallbackPagesPass");
    spdlog::info("CLod shadow structural tail: construct {} begin", shadowDeduplicatePredictedPagesPassName);
    auto shadowDeduplicatePredictedPagesPassDesc = org::ExternalPassDesc::Compute(
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
    spdlog::info("CLod shadow structural tail: construct {} complete", shadowDeduplicatePredictedPagesPassName);
    shadowDeduplicatePredictedPagesPassDesc.At(
        org::ExternalInsertPoint::After(shadowExpandPredictedPagesPassName));
    outPasses.push_back(std::move(shadowDeduplicatePredictedPagesPassDesc));

    const std::string shadowClearDirtyBitsPassName = MakeVariantPassName(traits, "VirtualShadowClearDirtyBitsPass");
    auto shadowClearDirtyBitsPassDesc = org::ExternalPassDesc::Compute(
        shadowClearDirtyBitsPassName,
        std::make_shared<VirtualShadowMapClearDirtyBitsPass>(
            extension.m_shadowPageTableTexture,
            extension.m_shadowAllocationRequestsBuffer,
            extension.m_shadowAllocationCountBuffer,
            extension.m_shadowAllocationIndirectArgsBuffer,
            extension.m_shadowDirtyPageFlagsBuffer,
            extension.m_shadowStatsBuffer));
    shadowClearDirtyBitsPassDesc.At(org::ExternalInsertPoint::After(shadowDeduplicatePredictedPagesPassName));
    outPasses.push_back(std::move(shadowClearDirtyBitsPassDesc));

    const std::string shadowComposePagesPassName =
        MakeVariantPassName(traits, "VirtualShadowComposePagesPass");
    auto shadowComposePagesPassDesc =
        org::ExternalPassDesc::Compute(
            shadowComposePagesPassName,
            std::make_shared<VirtualShadowMapComposePagesPass>(
                extension.m_shadowStaticPhysicalPagesTexture,
                extension.m_shadowPhysicalPagesTexture,
                extension.m_shadowPageTableTexture,
                extension.m_shadowPageMetadataBuffer,
                extension.m_shadowStatsBuffer));
    shadowComposePagesPassDesc.At(
        MakeShadowTailInsertPoint(shadowClearDirtyBitsPassName));
    outPasses.push_back(std::move(shadowComposePagesPassDesc));
}

std::string CLodShadowVariant::AppendPhase1FineRasterPass(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<org::ResourceGroup>& slabGroup,
    std::vector<org::ExternalPassDesc>& outPasses)
{
    return AppendFineRasterPassForPhase(extension, traits, slabGroup, outPasses, 1u);
}

std::string CLodShadowVariant::AppendPhase2FineRasterPass(
    CLodExtension& extension,
    const CLodVariantTraits& traits,
    const std::shared_ptr<org::ResourceGroup>& slabGroup,
    std::vector<org::ExternalPassDesc>& outPasses)
{
    return AppendFineRasterPassForPhase(extension, traits, slabGroup, outPasses, 2u);
}

std::shared_ptr<org::Resource> CLodShadowVariant::ProvideResource(CLodExtension& extension, org::ResourceIdentifier const& key)
{
    if (extension.m_type != CLodExtensionType::Shadow || !AreRendererShadowsEnabled()) {
        return nullptr;
    }

    if (key == Builtin::Shadows::CLodPageTable) {
        return extension.m_shadowPageTableTexture;
    }

    if (key.ToString() == "sarp.vsm.directional.page-table") return extension.m_shadowPageTableTexture;

    if (key == Builtin::Shadows::CLodPhysicalPages) {
        return extension.m_shadowPhysicalPagesTexture;
    }

    if (key.ToString() == "sarp.vsm.directional.physical-pages") return extension.m_shadowPhysicalPagesTexture;

    if (key == Builtin::Shadows::CLodClipmapInfo) {
        return extension.m_shadowClipmapInfoBuffer;
    }

    if (key.ToString() == "sarp.vsm.directional.clipmap-info") return extension.m_shadowClipmapInfoBuffer;

    if (key == Builtin::Shadows::CLodCompactMainCamera) {
        return extension.m_shadowCompactMainCameraBuffer;
    }

    if (key.ToString() == "sarp.vsm.directional.main-camera") return extension.m_shadowCompactMainCameraBuffer;

    if (key == Builtin::Shadows::CLodCompactShadowCameras) {
        return extension.m_shadowCompactShadowCameraBuffer;
    }

    if (key.ToString() == "sarp.vsm.directional.shadow-cameras") return extension.m_shadowCompactShadowCameraBuffer;

    if (key == Builtin::Shadows::CLodDirectionalPageViewInfo) {
        return extension.m_shadowDirectionalPageViewInfoBuffer;
    }

    if (key.ToString() == "sarp.vsm.directional.page-view-info") return extension.m_shadowDirectionalPageViewInfoBuffer;

    if (key == Builtin::Shadows::CLodPageMetadata) {
        return extension.m_shadowPageMetadataBuffer;
    }

    if (key.ToString() == "sarp.vsm.directional.page-metadata") return extension.m_shadowPageMetadataBuffer;

    if (key == Builtin::Shadows::CLodStats) {
        return extension.m_shadowStatsBuffer;
    }

    return nullptr;
}

std::vector<org::ResourceIdentifier> CLodShadowVariant::GetSupportedKeys(const CLodExtension& extension)
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
        Builtin::Shadows::CLodStats,
        org::ResourceIdentifier{ "sarp.vsm.directional.page-table" },
        org::ResourceIdentifier{ "sarp.vsm.directional.physical-pages" },
        org::ResourceIdentifier{ "sarp.vsm.directional.clipmap-info" },
        org::ResourceIdentifier{ "sarp.vsm.directional.main-camera" },
        org::ResourceIdentifier{ "sarp.vsm.directional.shadow-cameras" },
        org::ResourceIdentifier{ "sarp.vsm.directional.page-view-info" },
        org::ResourceIdentifier{ "sarp.vsm.directional.page-metadata" },
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
    const uint32_t vsmExpandedRecordCapacity = extension.m_shadowConfiguredExpandedRecordCapacity;
    const uint32_t shadowAtlasPagesWide = CLodVirtualShadowPhysicalAtlasPagesWideFromPhysicalPageCount(
        maxShadowPhysicalPageCount,
        extension.m_shadowConfiguredBackingResolution);
    const uint32_t shadowAtlasPagesHigh = CLodVirtualShadowPhysicalAtlasPagesHighFromPhysicalPageCount(
        maxShadowPhysicalPageCount,
        extension.m_shadowConfiguredBackingResolution);
    const uint64_t shadowAtlasWidth =
        static_cast<uint64_t>(shadowAtlasPagesWide) *
        CLodVirtualShadowPhysicalPageSize;
    const uint64_t shadowAtlasHeight =
        static_cast<uint64_t>(shadowAtlasPagesHigh) *
        CLodVirtualShadowPhysicalPageSize;
    const uint64_t combinedAtlasBytes =
        shadowAtlasWidth * shadowAtlasHeight * sizeof(uint32_t) * 2u;

    spdlog::info(
        "{}Shadow sizing: backingResolution={} physicalAtlas={}x{} pages={} texture={}x{} atlasLayers=2 combinedAtlasMiB={:.2f} computeClusters={} expandedRecords={}",
        traits.resourcePrefix,
        extension.m_shadowConfiguredBackingResolution,
        shadowAtlasPagesWide,
        shadowAtlasPagesHigh,
        maxShadowPhysicalPageCount,
        shadowAtlasWidth,
        shadowAtlasHeight,
        static_cast<double>(combinedAtlasBytes) / (1024.0 * 1024.0),
        extension.m_shadowConfiguredComputeClusterCapacity,
        vsmExpandedRecordCapacity);

    auto persistent = extension.m_options.persistentState;
    if (persistent && persistent->directionalVsm.pageTable)
        extension.m_shadowPageTableTexture = std::dynamic_pointer_cast<org::PixelBuffer>(persistent->directionalVsm.pageTable);
    if (!extension.m_shadowPageTableTexture)
        extension.m_shadowPageTableTexture = org::PixelBuffer::CreateSharedUnmaterialized(CreateVirtualShadowPageTableDescription());
    extension.m_shadowPageTableTexture->SetName(MakeVariantResourceName(traits, "Virtual Shadow Page Table"));
    extension.m_shadowPageTableTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPageTableTexture })
        .add<CLodVirtualShadowPageTableTag>()
        .add<CLodExtensionTypeTag>(typeEntity);

    if (persistent && persistent->directionalVsm.physicalPages)
        extension.m_shadowPhysicalPagesTexture = std::dynamic_pointer_cast<org::PixelBuffer>(persistent->directionalVsm.physicalPages);
    if (!extension.m_shadowPhysicalPagesTexture)
        extension.m_shadowPhysicalPagesTexture = org::PixelBuffer::CreateSharedUnmaterialized(
            CreateVirtualShadowPhysicalPagesDescription(
                extension.m_shadowConfiguredBackingResolution,
                maxShadowPhysicalPageCount));
    extension.m_shadowPhysicalPagesTexture->SetName(MakeVariantResourceName(traits, "Virtual Shadow Physical Pages"));
    extension.m_shadowPhysicalPagesTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowPhysicalPagesTexture })
        .add<CLodVirtualShadowPhysicalPagesTag>()
        .add<CLodExtensionTypeTag>(typeEntity);
    if (persistent) {
        persistent->directionalVsm.pageTable = extension.m_shadowPageTableTexture;
        persistent->directionalVsm.physicalPages = extension.m_shadowPhysicalPagesTexture;
    }

    extension.m_shadowStaticPhysicalPagesTexture =
        org::PixelBuffer::CreateSharedUnmaterialized(
            CreateVirtualShadowPhysicalPagesDescription(
                extension.m_shadowConfiguredBackingResolution,
                maxShadowPhysicalPageCount));
    extension.m_shadowStaticPhysicalPagesTexture->SetName(
        MakeVariantResourceName(
            traits, "Virtual Shadow Static Physical Pages"));
    extension.m_shadowStaticPhysicalPagesTexture->GetECSEntity()
        .set<Components::Resource>({
            extension.m_shadowStaticPhysicalPagesTexture })
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

    const uint32_t upgradeUploadSlotCount = std::min<uint32_t>(
        static_cast<uint32_t>(
            SettingsManager::GetInstance()
                .getSettingGetter<uint8_t>("numFramesInFlight")()) +
            1u,
        8u);
    extension.m_shadowUpgradeInvalidationUploadBuffers.clear();
    extension.m_shadowUpgradeInvalidationUploadBuffers.reserve(
        upgradeUploadSlotCount);
    for (uint32_t slotIndex = 0u; slotIndex < upgradeUploadSlotCount;
         ++slotIndex) {
        auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(
            CLodVirtualShadowMaxInvalidationInputs,
            sizeof(CLodVirtualShadowUpgradeInvalidationInput),
            false,
            false,
            false,
            rhi::HeapType::Upload);
        buffer->SetName(MakeVariantResourceName(
            traits,
            "Virtual Shadow Upgrade Upload Slot " +
                std::to_string(slotIndex)));
        buffer->GetECSEntity()
            .set<Components::Resource>({buffer})
            .add<CLodExtensionTypeTag>(typeEntity);
        extension.m_shadowUpgradeInvalidationUploadBuffers.push_back(
            std::move(buffer));
    }
    if (extension.m_streamingSystem) {
        extension.m_streamingSystem->SetVirtualShadowUpgradeUploadBuffers(
            extension.m_shadowUpgradeInvalidationUploadBuffers);
    }

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

    const uint32_t receiverSubpageMode = SettingsManager::GetInstance().getSettingGetter<uint32_t>(
        CLodDirectionalVirtualShadowReceiverSubpageModeSettingName)();
    if (receiverSubpageMode == CLodVirtualShadowReceiverSubpageMode4x4 ||
        receiverSubpageMode == CLodVirtualShadowReceiverSubpageMode8x8) {
        extension.m_shadowReceiverSubpageMaskBuffer =
            CreateAliasedUnmaterializedStructuredBuffer(
                CLodVirtualShadowMaxReceiverPageCount,
                receiverSubpageMode == CLodVirtualShadowReceiverSubpageMode8x8
                    ? sizeof(uint32_t) * 2u
                    : sizeof(uint32_t),
                true,
                false,
                false,
                false);
        extension.m_shadowReceiverSubpageMaskBuffer->SetName(
            MakeVariantResourceName(
                traits,
                "Virtual Shadow Receiver Subpage Mask Buffer"));
    }

    extension.m_shadowActiveBlockMetadataBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodVirtualShadowMaxMarkedBlockCount,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    extension.m_shadowActiveBlockMetadataBuffer->SetName(MakeVariantResourceName(traits, "Virtual Shadow Active Block Metadata Buffer"));

    extension.m_shadowDynamicActiveBlockMetadataBuffer =
        CreateAliasedUnmaterializedStructuredBuffer(
            CLodVirtualShadowMaxMarkedBlockCount,
            sizeof(uint32_t),
            true,
            false,
            false,
            false);
    extension.m_shadowDynamicActiveBlockMetadataBuffer->SetName(
        MakeVariantResourceName(
            traits,
            "Virtual Shadow Dynamic Active Block Metadata Buffer"));

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

    extension.m_shadowDirtyPageHierarchyTexture = org::PixelBuffer::CreateSharedUnmaterialized(CreateVirtualShadowDirtyHierarchyDescription());
    extension.m_shadowDirtyPageHierarchyTexture->SetName(MakeVariantResourceName(traits, "Virtual Shadow Dirty Hierarchy"));
    extension.m_shadowDirtyPageHierarchyTexture->GetECSEntity()
        .set<Components::Resource>({ extension.m_shadowDirtyPageHierarchyTexture })
        .add<CLodVirtualShadowDirtyHierarchyTag>()
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

    if (persistent) {
        persistent->directionalVsm.clipmapInfo = extension.m_shadowClipmapInfoBuffer;
        persistent->directionalVsm.mainCamera = extension.m_shadowCompactMainCameraBuffer;
        persistent->directionalVsm.shadowCameras = extension.m_shadowCompactShadowCameraBuffer;
        persistent->directionalVsm.pageViewInfo = extension.m_shadowDirectionalPageViewInfoBuffer;
        persistent->directionalVsm.pageMetadata = extension.m_shadowPageMetadataBuffer;
    }

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
    auto tagBufferUsage = [](const std::shared_ptr<org::Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            org::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };
    auto tagTextureUsage = [](const std::shared_ptr<org::PixelBuffer>& texture, std::string_view usage) {
        if (texture) {
            org::memory::SetResourceUsageHint(*texture, std::string(usage));
        }
    };

    tagTextureUsage(extension.m_shadowPageTableTexture, "Cluster LOD virtual shadow maps");
    tagTextureUsage(extension.m_shadowPhysicalPagesTexture, "Cluster LOD virtual shadow maps");
    tagTextureUsage(extension.m_shadowStaticPhysicalPagesTexture, "Cluster LOD virtual shadow maps");
    tagTextureUsage(extension.m_shadowDirtyPageHierarchyTexture, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowPageMetadataBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowInvalidationInputsBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowInvalidationCountBuffer, "Cluster LOD virtual shadow maps");
    for (const auto& buffer :
         extension.m_shadowUpgradeInvalidationUploadBuffers) {
        tagBufferUsage(buffer, "Cluster LOD virtual shadow maps");
    }
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
    tagBufferUsage(extension.m_shadowReceiverSubpageMaskBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowActiveBlockMetadataBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_shadowDynamicActiveBlockMetadataBuffer, "Cluster LOD virtual shadow maps");
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
    tagBufferUsage(extension.m_vsmExpandedVisibleClustersBuffer, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_vsmExpandedVisibleClustersBufferSw, "Cluster LOD virtual shadow maps");
    tagBufferUsage(extension.m_vsmExpandedVisibleClusterTransformIndicesBufferSw, "Cluster LOD virtual shadow maps");
}

void CLodShadowVariant::ReleaseResourceBackings(CLodExtension& extension)
{
    std::unordered_set<org::Buffer*> releasedBuffers;
    std::unordered_set<org::PixelBuffer*> releasedTextures;
    auto releaseBufferBacking = [&releasedBuffers](const std::shared_ptr<org::Buffer>& buffer) {
        if (buffer && releasedBuffers.insert(buffer.get()).second) {
            buffer->Dematerialize();
        }
    };
    auto releaseTextureBacking = [&releasedTextures](const std::shared_ptr<org::PixelBuffer>& texture) {
        if (texture && releasedTextures.insert(texture.get()).second) {
            texture->Dematerialize();
        }
    };

    releaseTextureBacking(extension.m_shadowPageTableTexture);
    releaseTextureBacking(extension.m_shadowPhysicalPagesTexture);
    releaseTextureBacking(extension.m_shadowStaticPhysicalPagesTexture);
    releaseTextureBacking(extension.m_shadowDirtyPageHierarchyTexture);
    releaseBufferBacking(extension.m_vsmExpandedVisibleClustersBuffer);
    releaseBufferBacking(extension.m_vsmExpandedVisibleClustersBufferSw);
    releaseBufferBacking(extension.m_vsmExpandedVisibleClusterTransformIndicesBufferSw);
    releaseBufferBacking(extension.m_shadowDynamicActiveBlockMetadataBuffer);
    extension.m_shadowVirtualResourcesNeedReset = true;
}

void CLodShadowVariant::RefreshResourcesForCurrentSettings(CLodExtension& extension)
{
    if (extension.m_type != CLodExtensionType::Shadow) {
        return;
    }

    const uint32_t previousBackingResolution = extension.m_shadowConfiguredBackingResolution;
    const uint32_t previousMaxPhysicalPageCount = extension.m_shadowConfiguredMaxPhysicalPageCount;
    const uint32_t previousComputeClusterCapacity = extension.m_shadowConfiguredComputeClusterCapacity;
    const uint32_t previousVisibleClusterCapacity = extension.m_visibleClusterCapacity;

    RefreshConfiguredSettings(extension);

    if (extension.m_shadowPhysicalPagesTexture &&
        previousBackingResolution == extension.m_shadowConfiguredBackingResolution &&
        previousMaxPhysicalPageCount == extension.m_shadowConfiguredMaxPhysicalPageCount &&
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
