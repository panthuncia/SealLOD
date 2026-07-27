#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Interfaces/IResourceProvider.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"

class Buffer;
class CLodStreamingSystem;
class CLodAlphaVariant;
class CLodShadowVariant;
class CLodVisibilityVariant;
class PixelBuffer;
class ResourceGroup;
struct CLodVariantTraits;

struct CLodExtensionOptions {
    bool enableReyes = true;
    bool enableVoxelRasterization = false;
    uint32_t voxelRasterWorkCapacity = 0u;
    std::shared_ptr<CLodStreamingSystem> streamingSystem;
};

class CLodExtension final : public RenderGraph::IRenderGraphExtension, public IResourceProvider {
public:
    explicit CLodExtension(
        CLodExtensionType type,
        uint32_t maxVisibleClusters,
        CLodExtensionOptions options = {});
    ~CLodExtension();

    void PrepareForBuild(RenderGraph& rg) override;
    void Initialize(RenderGraph& rg) override;
    void Shutdown(RenderGraph& rg) override;
    void OnRegistryReset(ResourceRegistry* reg) override;
    void GatherStructuralPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses) override;
    void GatherFramePasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses) override;
    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
    std::vector<ResourceIdentifier> GetSupportedKeys() override;

private:
    friend class CLodAlphaVariant;
    friend class CLodShadowVariant;
    friend class CLodVisibilityVariant;

    bool IsReyesTessellationDisabled() const;
    void RefreshShadowConfiguredSettings();
    uint32_t GetVisibleClusterCapacity() const;
    void RefreshCoreVisibleClusterCapacity();
    void InitializeCoreResources();
    void InitializeDeepVisibilityResources();
    void InitializeAVBOITResources();
    void InitializeShadowResources();
    void RefreshTransparencyResourcesForCurrentSettings();
    void RefreshShadowResourcesForCurrentSettings();
    void TagCoreResourceUsages();
    void TagTransparencyResourceUsages();
    void TagShadowResourceUsages();
    void ReleaseBufferBackings();
    void ReleaseTransparencyResourceBackings();
    void ReleaseShadowResourceBackings();
    void EnsureReyesResourcesInitialized();
    void SyncReyesResourceEntities(bool enabled);
    void AppendPhaseReyesStructuralPasses(
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
        std::string& shadowClearDirtyBitsAfterPassName);

    CLodExtensionType m_type;
    CLodExtensionOptions m_options;
    uint32_t m_maxVisibleClusters = 0u;
    uint32_t m_visibleClusterCapacity = 0u;
    uint32_t m_reyesFullClusterOutputCapacity = 0u;
    uint32_t m_reyesOwnedClusterCapacity = 0u;
    uint32_t m_reyesSplitQueueCapacity = 0u;
    uint32_t m_reyesDiceQueueCapacity = 0u;
    uint32_t m_reyesDiceQueuePhysicalCapacity = 0u;
    uint32_t m_reyesRasterWorkCapacity = 0u;
    uint32_t m_reyesOwnershipBitsetWordCount = 0u;
    uint64_t m_reyesRequestedBudgetBytes = 0u;
    uint64_t m_reyesAllocatedBudgetBytes = 0u;
    bool m_reyesBudgetLimited = false;

    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_workGraphTelemetryBuffer;
    std::shared_ptr<Buffer> m_occlusionReplayBuffer;
    std::shared_ptr<Buffer> m_occlusionReplayStateBuffer;
    std::shared_ptr<Buffer> m_occlusionNodeGpuInputsBuffer;
    std::shared_ptr<Buffer> m_viewDepthSrvIndicesBuffer;
    std::shared_ptr<Buffer> m_viewDepthSrvIndicesBufferPhase2;

    std::shared_ptr<Buffer> m_histogramIndirectCommand;
    std::shared_ptr<Buffer> m_histogramIndirectCommandPhase2;
    std::shared_ptr<Buffer> m_histogramIndirectCommandSw;
    std::shared_ptr<Buffer> m_histogramIndirectCommandPhase2Sw;
    std::shared_ptr<Buffer> m_histogramIndirectCommandPageJob;
    std::shared_ptr<Buffer> m_histogramIndirectCommandPhase2PageJob;
    std::shared_ptr<Buffer> m_histogramIndirectCommandReyes;
    std::shared_ptr<Buffer> m_histogramIndirectCommandPhase2Reyes;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBuffer;

    std::shared_ptr<Buffer> m_rasterBucketsOffsetsBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsBlockSumsBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsScannedBlockSumsBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsTotalCountBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsTotalCountBufferPhase1;
    std::shared_ptr<Buffer> m_rasterBucketsTotalCountBufferPhase1Sw;

    std::shared_ptr<Buffer> m_visibleClustersCounterBufferPhase2;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBufferPhase2;
    std::shared_ptr<Buffer> m_rasterBucketsWriteCursorBufferPhase2;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBufferSw;
    std::shared_ptr<Buffer> m_rasterBucketsHistogramBufferPhase2Sw;
    std::shared_ptr<Buffer> m_rasterBucketsWriteCursorBufferSw;
    std::shared_ptr<Buffer> m_rasterBucketsWriteCursorBufferPhase2Sw;

    std::shared_ptr<Buffer> m_compactedVisibleClustersBuffer;
    std::shared_ptr<Buffer> m_compactedVisibleClustersBufferSw;
    std::shared_ptr<Buffer> m_compactedVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_compactedVisibleClusterTransformIndicesBufferSw;
    std::shared_ptr<Buffer> m_rasterBucketsWriteCursorBuffer;
    // TODO: Raster-bucket indirect args have exhibited invalid data when reused across otherwise separate
    // CLod rasterization paths. Until the root cause is understood, keep HW, compute SW, and SW page-job
    // indirect command streams on dedicated resources for each phase.
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBufferPhase2;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBufferSw;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBufferPhase2Sw;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBufferPageJob;
    std::shared_ptr<Buffer> m_rasterBucketsIndirectArgsBufferPhase2PageJob;

    std::shared_ptr<Buffer> m_reyesFullClusterOutputsBuffer;
    std::shared_ptr<Buffer> m_reyesFullClusterOutputsCounterBuffer;
    std::shared_ptr<Buffer> m_reyesOwnedClustersBuffer;
    std::shared_ptr<Buffer> m_reyesOwnedClustersCounterBuffer;
    std::shared_ptr<Buffer> m_reyesOwnershipBitsetBuffer;
    std::shared_ptr<Buffer> m_reyesOwnershipBitsetBufferPhase2;
    std::shared_ptr<Buffer> m_reyesClassifyIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_reyesClassifyIndirectArgsBufferPhase2;
    std::shared_ptr<Buffer> m_reyesSplitIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_reyesSplitIndirectArgsBufferPhase2;
    std::shared_ptr<Buffer> m_reyesSplitQueueBufferA;
    std::shared_ptr<Buffer> m_reyesSplitQueueCounterBufferA;
    std::shared_ptr<Buffer> m_reyesSplitQueueOverflowBufferA;
    std::shared_ptr<Buffer> m_reyesSplitQueueBufferB;
    std::shared_ptr<Buffer> m_reyesSplitQueueCounterBufferB;
    std::shared_ptr<Buffer> m_reyesSplitQueueOverflowBufferB;
    std::shared_ptr<Buffer> m_reyesReplaySplitQueueBuffer;
    std::shared_ptr<Buffer> m_reyesReplaySplitQueueCounterBuffer;
    std::shared_ptr<Buffer> m_reyesReplaySplitQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_reyesDiceQueueBuffer;
    std::shared_ptr<Buffer> m_reyesDiceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_reyesDiceQueuePhase1CountBuffer;
    std::shared_ptr<Buffer> m_reyesDiceQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_reyesReplayDiceQueueBuffer;
    std::shared_ptr<Buffer> m_reyesReplayDiceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_reyesReplayDiceQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_reyesRasterWorkBuffer;
    std::shared_ptr<Buffer> m_reyesRasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_reyesRasterWorkIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_reyesCompactedRasterWorkIndicesBuffer;
    std::shared_ptr<Buffer> m_reyesPackedRasterWorkGroupsBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableVerticesBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableTrianglesBuffer;
    std::shared_ptr<Buffer> m_reyesDiceIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_reyesDiceIndirectArgsBufferPhase2;
    std::shared_ptr<Buffer> m_reyesRasterWorkBufferPhase2;
    std::shared_ptr<Buffer> m_reyesRasterWorkCounterBufferPhase2;
    std::shared_ptr<Buffer> m_reyesRasterWorkIndirectArgsBufferPhase2;
    std::shared_ptr<Buffer> m_reyesCompactedRasterWorkIndicesBufferPhase2;
    std::shared_ptr<Buffer> m_reyesPackedRasterWorkGroupsBufferPhase2;
    std::shared_ptr<Buffer> m_reyesTelemetryBufferPhase1;
    std::shared_ptr<Buffer> m_reyesTelemetryBufferPhase2;

    std::shared_ptr<Buffer> m_swVisibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_swVisibleClustersCounterBufferPhase2;
    std::shared_ptr<Buffer> m_voxelRasterWorkBuffer;
    std::shared_ptr<Buffer> m_voxelRasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_skinnedVoxelRasterWorkBuffer;
    std::shared_ptr<Buffer> m_skinnedVoxelRasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_voxelRasterIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_skinnedVoxelRasterIndirectArgsBuffer;
    uint32_t m_voxelRasterWorkCapacity = 0u;
    std::shared_ptr<Buffer> m_sortedToUnsortedMappingBuffer;
    std::shared_ptr<Buffer> m_sortedToUnsortedMappingBufferSw;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityNodesBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityOverflowCounterBuffer;
    std::shared_ptr<Buffer> m_deepVisibilityStatsBuffer;
    std::shared_ptr<Buffer> m_AVBOITConfigBuffer;
    std::shared_ptr<Buffer> m_AVBOITOccupancyHistogramBuffer;
    std::shared_ptr<Buffer> m_AVBOITDepthWarpLUTBuffer;
    std::shared_ptr<Buffer> m_AVBOITFitStateBuffer;
    std::shared_ptr<Buffer> m_AVBOITEarlyDepthTileCommandsBuffer;
    std::shared_ptr<Buffer> m_AVBOITEarlyDepthTileCountBuffer;
    std::shared_ptr<PixelBuffer> m_AVBOITOccupancyTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITCoverageTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITOccupancySliceMaskTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITChromaticExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITIntegratedTransmittanceTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITZeroTransmittanceSliceTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITAccumulationTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITNormalizationTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITShadingExtinctionTexture;
    std::shared_ptr<PixelBuffer> m_AVBOITEarlyDepthTexture;
    std::shared_ptr<PixelBuffer> m_shadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_shadowPhysicalPagesTexture;
    std::shared_ptr<PixelBuffer> m_shadowStaticPhysicalPagesTexture;
    std::shared_ptr<Buffer> m_shadowPageMetadataBuffer;
    std::shared_ptr<Buffer> m_shadowInvalidationInputsBuffer;
    std::shared_ptr<Buffer> m_shadowInvalidationCountBuffer;
    std::shared_ptr<Buffer> m_shadowInvalidatedInstancesBitsetBuffer;
    std::vector<std::shared_ptr<Buffer>> m_shadowUpgradeInvalidationUploadBuffers;
    std::shared_ptr<Buffer> m_shadowPredictiveInvalidationCandidatesBuffer;
    std::shared_ptr<Buffer> m_shadowPredictiveInvalidationCandidateCountBuffer;
    std::shared_ptr<Buffer> m_shadowPredictiveRawPagesBuffer;
    std::shared_ptr<Buffer> m_shadowPredictiveRawPageCountBuffer;
    std::shared_ptr<Buffer> m_shadowPredictedInvalidationScratchBitsetBuffer;
    std::shared_ptr<Buffer> m_shadowPredictedInvalidationPagesBufferA;
    std::shared_ptr<Buffer> m_shadowPredictedInvalidationPageCountBufferA;
    std::shared_ptr<Buffer> m_shadowAllocationRequestsBuffer;
    std::shared_ptr<Buffer> m_shadowAllocationCountBuffer;
    std::shared_ptr<Buffer> m_shadowAllocationIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_shadowMarkTileWorkBuffer;
    std::shared_ptr<Buffer> m_shadowMarkTileCountBuffer;
    std::shared_ptr<Buffer> m_shadowMarkTileIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_shadowMarkedBlocksMaskBuffer;
    std::shared_ptr<Buffer> m_shadowMarkedBlocksListBuffer;
    std::shared_ptr<Buffer> m_shadowMarkedBlocksCountBuffer;
    std::shared_ptr<Buffer> m_shadowActiveBlockMetadataBuffer;
    std::shared_ptr<Buffer> m_shadowDynamicActiveBlockMetadataBuffer;
    std::shared_ptr<Buffer> m_shadowBlockClusterCoverageBuffer;
    std::shared_ptr<Buffer> m_shadowFreePhysicalPagesBuffer;
    std::shared_ptr<Buffer> m_shadowReusablePhysicalPagesBuffer;
    std::shared_ptr<Buffer> m_shadowPageListHeaderBuffer;
    std::shared_ptr<Buffer> m_shadowDirtyPageFlagsBuffer;
    std::shared_ptr<PixelBuffer> m_shadowDirtyPageHierarchyTexture;
    std::shared_ptr<PixelBuffer> m_shadowNonRasterablePageHierarchyTexture;
    std::shared_ptr<Buffer> m_shadowClipmapInfoBuffer;
    std::shared_ptr<Buffer> m_shadowMarkClipmapDataBuffer;
    std::shared_ptr<Buffer> m_shadowCompactMainCameraBuffer;
    std::shared_ptr<Buffer> m_shadowCompactShadowCameraBuffer;
    std::shared_ptr<Buffer> m_shadowDirectionalPageViewInfoBuffer;
    std::shared_ptr<Buffer> m_shadowRuntimeStateBuffer;
    std::shared_ptr<Buffer> m_shadowStatsBuffer;
    std::shared_ptr<Buffer> m_swPageJobVisibleClustersBuffer;
    std::shared_ptr<Buffer> m_swPageJobVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_swPageJobVisibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_swPageJobVisibleClustersBufferPhase2;
    std::shared_ptr<Buffer> m_swPageJobVisibleClusterTransformIndicesBufferPhase2;
    std::shared_ptr<Buffer> m_swPageJobVisibleClustersCounterBufferPhase2;
    std::shared_ptr<Buffer> m_swPageJobRecordsBuffer;
    std::shared_ptr<Buffer> m_swPageJobRecordsBufferSkinned;
    std::shared_ptr<Buffer> m_swPageJobCountBuffer;
    std::shared_ptr<Buffer> m_swPageJobCountBufferSkinned;
    std::shared_ptr<Buffer> m_swPageJobRecordsBufferPhase2;
    std::shared_ptr<Buffer> m_swPageJobRecordsBufferPhase2Skinned;
    std::shared_ptr<Buffer> m_swPageJobCountBufferPhase2;
    std::shared_ptr<Buffer> m_swPageJobCountBufferPhase2Skinned;
    std::shared_ptr<Buffer> m_swPageJobIndirectArgsBuffer;
    std::shared_ptr<Buffer> m_swPageJobIndirectArgsBufferSkinned;
    std::shared_ptr<Buffer> m_swPageJobIndirectArgsBufferPhase2;
    std::shared_ptr<Buffer> m_swPageJobIndirectArgsBufferPhase2Skinned;
    std::shared_ptr<Buffer> m_swPageJobClusterTagsBuffer;
    std::shared_ptr<Buffer> m_swPageJobClusterTagsBufferPhase2;
    std::shared_ptr<Buffer> m_vsmExpandedVisibleClustersBuffer;
    std::shared_ptr<Buffer> m_vsmExpandedVisibleClustersBufferSw;
    std::shared_ptr<Buffer> m_vsmExpandedVisibleClusterTransformIndicesBufferSw;

    std::shared_ptr<CLodStreamingSystem> m_streamingSystem;
    bool m_providerRegisteredForCurrentRegistry = false;
    bool m_shadowVirtualResourcesNeedReset = true;
    uint32_t m_transparencyConfiguredRenderWidth = 0u;
    uint32_t m_transparencyConfiguredRenderHeight = 0u;
    uint32_t m_shadowConfiguredBackingResolution = 0u;
    uint32_t m_shadowConfiguredMaxPhysicalPageCount = 0u;
    uint32_t m_shadowConfiguredPageJobMaxPages = 0u;
    uint32_t m_shadowConfiguredPageJobRecordCapacity = 0u;
    uint32_t m_shadowConfiguredComputeClusterCapacity = 0u;
    uint32_t m_shadowConfiguredExpandedRecordCapacity = 0u;
};
