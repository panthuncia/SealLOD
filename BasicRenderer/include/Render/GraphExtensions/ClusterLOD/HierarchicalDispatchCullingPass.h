#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rhi.h>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/HierarchicalCullingPass.h"

class HierarchicalDispatchCullingPass : public ComputePass, public IDynamicDeclaredResources {
public:
    HierarchicalDispatchCullingPass(
        std::string stablePassIdentifier,
        HierarchicalCullingPassInputs inputs,
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<Buffer> swVisibleClustersCounterBuffer,
        std::shared_ptr<Buffer> voxelRasterWorkBuffer,
        std::shared_ptr<Buffer> voxelRasterWorkCounterBuffer,
        std::shared_ptr<Buffer> skinnedVoxelRasterWorkBuffer,
        std::shared_ptr<Buffer> skinnedVoxelRasterWorkCounterBuffer,
        uint32_t voxelRasterWorkCapacity,
        std::shared_ptr<Buffer> pageJobVisibleClustersBuffer,
        std::shared_ptr<Buffer> pageJobVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<Buffer> pageJobVisibleClustersCounterBuffer,
        std::shared_ptr<Buffer> histogramIndirectCommand,
        std::shared_ptr<Buffer> workGraphTelemetryBuffer,
        std::shared_ptr<Buffer> occlusionReplayBuffer,
        std::shared_ptr<Buffer> occlusionReplayStateBuffer,
        std::shared_ptr<Buffer> occlusionNodeGpuInputsBuffer,
        std::shared_ptr<Buffer> viewDepthSrvIndicesBuffer,
        std::shared_ptr<Buffer> viewRasterInfoBuffer,
        std::shared_ptr<PixelBuffer> shadowDirtyHierarchyTexture = nullptr,
        std::shared_ptr<ResourceGroup> slabResourceGroup = nullptr,
        std::shared_ptr<Buffer> phase1VisibleClustersCounterBuffer = nullptr,
        std::shared_ptr<Buffer> swWriteBaseCounterBuffer = nullptr,
        std::shared_ptr<Buffer> shadowPredictiveInvalidationCandidatesBuffer = nullptr,
        std::shared_ptr<Buffer> shadowPredictiveInvalidationCandidateCountBuffer = nullptr,
        std::shared_ptr<Buffer> shadowInvalidatedInstancesBitsetBuffer = nullptr,
        std::shared_ptr<PixelBuffer> shadowPageTableTexture = nullptr,
        std::shared_ptr<PixelBuffer> shadowPhysicalPagesTexture = nullptr,
        std::shared_ptr<Buffer> shadowActiveBlockMetadataBuffer = nullptr,
        std::shared_ptr<PixelBuffer> shadowDynamicPhysicalPagesTexture = nullptr,
        std::shared_ptr<Buffer> shadowDynamicActiveBlockMetadataBuffer = nullptr);
    ~HierarchicalDispatchCullingPass() override;

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    void Cleanup() override;
    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
    std::vector<ResourceIdentifier> GetSupportedKeys() override;

private:
    struct PureComputeDispatchCommand
    {
        uint32_t dispatchX;
        uint32_t dispatchY;
        uint32_t dispatchZ;
    };

    struct ObjectCullRecord
    {
        uint32_t viewDataIndex;
        uint32_t activeDrawSetIndicesSRVIndex;
        uint32_t activeDrawCount;
        uint32_t drawRecordVisibilityGenerationSRVIndex;
        uint32_t dispatchGridX;
        uint32_t dispatchGridY;
        uint32_t dispatchGridZ;
    };

    PipelineState m_clearPipelineState;
    PipelineState m_createCommandPipelineState;
    PipelineState m_pureComputeBuildDispatchArgsPipelineState;
    PipelineState m_pureComputeBuildDualDispatchArgsPipelineState;
    PipelineState m_pureComputeClearTraversalCountersPipelineState;
    PipelineState m_pureComputeBuildReplayDispatchArgsPipelineState;
    PipelineState m_pureComputeObjectCullPipelineState;
    PipelineState m_pureComputeReplayNodesPipelineState;
    PipelineState m_pureComputeReplayClustersPipelineState;
    PipelineState m_pureComputeTraversePipelineState;
    PipelineState m_pureComputeLeafPipelineState;
    PipelineState m_pureComputeClusterPipelineState;
    PipelineState m_pureComputeDenseClusterPipelineState;
    rhi::CommandSignaturePtr m_pureComputeDispatchCommandSignature;
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
    std::shared_ptr<Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_swVisibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_voxelRasterWorkBuffer;
    std::shared_ptr<Buffer> m_voxelRasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_skinnedVoxelRasterWorkBuffer;
    std::shared_ptr<Buffer> m_skinnedVoxelRasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_voxelRasterQueueDescriptorsBuffer;
    std::string m_voxelRasterQueueDescriptorResourceId;
    uint32_t m_voxelRasterWorkCapacity = 0u;
    std::shared_ptr<Buffer> m_pageJobVisibleClustersBuffer;
    std::shared_ptr<Buffer> m_pageJobVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<Buffer> m_pageJobVisibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_workGraphComputePageJobDescriptorsBuffer;
    std::string m_workGraphComputePageJobDescriptorResourceId;
    std::shared_ptr<Buffer> m_histogramIndirectCommand;
    std::shared_ptr<Buffer> m_workGraphTelemetryBuffer;
    std::shared_ptr<Buffer> m_occlusionReplayBuffer;
    std::shared_ptr<Buffer> m_occlusionReplayStateBuffer;
    std::shared_ptr<Buffer> m_occlusionNodeGpuInputsBuffer;
    std::shared_ptr<Buffer> m_viewDepthSrvIndicesBuffer;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<Buffer> m_phase1VisibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_swWriteBaseCounterBuffer;
    std::shared_ptr<PixelBuffer> m_shadowDirtyHierarchyTexture;
    std::shared_ptr<Buffer> m_shadowPredictiveInvalidationCandidatesBuffer;
    std::shared_ptr<Buffer> m_shadowPredictiveInvalidationCandidateCountBuffer;
    std::shared_ptr<Buffer> m_shadowInvalidatedInstancesBitsetBuffer;
    std::shared_ptr<PixelBuffer> m_shadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_shadowPhysicalPagesTexture;
    std::shared_ptr<Buffer> m_shadowActiveBlockMetadataBuffer;
    std::shared_ptr<PixelBuffer> m_shadowDynamicPhysicalPagesTexture;
    std::shared_ptr<Buffer> m_shadowDynamicActiveBlockMetadataBuffer;
    std::shared_ptr<Buffer> m_pureComputeCurrentNodeFrontierBuffer;
    std::shared_ptr<Buffer> m_pureComputeNextNodeFrontierBuffer;
    std::shared_ptr<Buffer> m_pureComputeCurrentLeafFrontierBuffer;
    std::shared_ptr<Buffer> m_pureComputeNextLeafFrontierBuffer;
    std::shared_ptr<Buffer> m_pureComputeClusterFrontierBuffer;
    std::shared_ptr<Buffer> m_pureComputeCurrentNodeCounterBuffer;
    std::shared_ptr<Buffer> m_pureComputeNextNodeCounterBuffer;
    std::shared_ptr<Buffer> m_pureComputeCurrentLeafCounterBuffer;
    std::shared_ptr<Buffer> m_pureComputeNextLeafCounterBuffer;
    std::shared_ptr<Buffer> m_pureComputeClusterCounterBuffer;
    std::shared_ptr<Buffer> m_pureComputeNodeDispatchArgsBuffer;
    std::shared_ptr<Buffer> m_pureComputeLeafDispatchArgsBuffer;
    std::shared_ptr<Buffer> m_pureComputeClusterDispatchArgsBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    std::vector<uint64_t> m_declaredDrawSetResourceIds;
    std::vector<CLodViewRasterInfo> m_cachedViewRasterInfo;
    std::vector<CLodViewDepthSRVIndex> m_cachedViewDepthSrvIndices;
    std::vector<uint32_t> m_zeroTelemetryScratch;
    CLodVoxelRasterQueueDescriptors m_cachedVoxelQueueDescriptors{};
    CLodWorkGraphComputePageJobDescriptors m_cachedPageJobDescriptors{};
    uint64_t m_lastDrawSetDeclarationRevision = 0u;
    uint64_t m_lastViewResourceLayoutRevision = 0u;
    uint32_t m_sizedPureComputeFrontierCapacity = 0u;
    bool m_hasCachedVoxelQueueDescriptors = false;
    bool m_hasCachedPageJobDescriptors = false;
    bool m_hasUploadedViewDepthSrvIndices = false;
    bool m_isFirstPass = true;
    bool m_declaredResourcesChanged = true;
    unsigned int m_maxVisibleClusters = 0u;
    HierarchicalCullingWorkGraphMode m_workGraphMode = HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph;
    CLodRasterOutputKind m_rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
    RenderPhase m_renderPhase;
    bool m_clodOnlyWorkloads = false;
    bool m_useShadowCascadeViews = false;
    uint32_t m_activeTraversalDepth = 0u;
    bool m_loggedUnsupportedConfiguration = false;
};
