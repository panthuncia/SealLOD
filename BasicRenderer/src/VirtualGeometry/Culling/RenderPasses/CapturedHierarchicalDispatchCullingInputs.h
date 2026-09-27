#pragma once
#include "Render/PublicationBindingBundle.h"
#include "Render/PipelineState.h"
#include "VirtualGeometry/Culling/RenderPasses/HierarchicalCullingPass.h"
#include <memory>
#include <array>
#include <stdexcept>

namespace br::render {
// Worker input adapters own immutable producer snapshots, never live managers,
// resource wrappers or pipeline slots. Native handles remain exact versions.
struct CapturedCullingResource {
    std::shared_ptr<const org::ResourceBindingSnapshot> snapshot;
    const rhi::Resource& GetAPIResource() const {
        if (!snapshot) throw std::invalid_argument("Missing captured culling resource");
        return snapshot->resource;
    }
};
struct CapturedCullingProgram {
    std::shared_ptr<const org::PipelineStatePayload> payload;
    const rhi::Pipeline& GetAPIPipelineState() const {
        if (!payload) throw std::invalid_argument("Missing captured culling program");
        return payload->pso.Get();
    }
    const org::PipelineResources& GetResourceDescriptorSlots() const {
        if (!payload) throw std::invalid_argument("Missing captured culling program");
        return payload->pipelineResources;
    }
};
struct CapturedHierarchicalDispatchCullingInputs {
    using EmissionBuffer = std::shared_ptr<const CapturedCullingResource>;
    uint32_t m_activeTraversalDepth = 0u;
    CapturedCullingProgram m_clearPipelineState;
    CapturedCullingProgram m_createCommandPipelineState;
    EmissionBuffer m_dynamicWindBoundsCacheBuffer;
    uint32_t m_dynamicWindBoundsCacheEntryCount = 0u;
    uint32_t m_dynamicWindBoundsCacheGeneration = 1u;
    EmissionBuffer m_dynamicWindVisibleMembershipBuffer;
    EmissionBuffer m_histogramIndirectCommand;
    bool m_isFirstPass = true;
    unsigned int m_maxVisibleClusters = 0u;
    EmissionBuffer m_occlusionReplayBuffer;
    EmissionBuffer m_occlusionReplayStateBuffer;
    EmissionBuffer m_pageJobVisibleClustersCounterBuffer;
    EmissionBuffer m_phase1VisibleClustersCounterBuffer;
    CapturedCullingProgram m_pureComputeBuildDispatchArgsPipelineState;
    CapturedCullingProgram m_pureComputeBuildDualDispatchArgsPipelineState;
    CapturedCullingProgram m_pureComputeBuildReplayDispatchArgsPipelineState;
    CapturedCullingProgram m_pureComputeClearTraversalCountersPipelineState;
    EmissionBuffer m_pureComputeClusterCounterBuffer;
    EmissionBuffer m_pureComputeClusterDispatchArgsBuffer;
    EmissionBuffer m_pureComputeClusterFrontierBuffer;
    CapturedCullingProgram m_pureComputeClusterPipelineState;
    EmissionBuffer m_pureComputeCurrentLeafCounterBuffer;
    EmissionBuffer m_pureComputeCurrentLeafFrontierBuffer;
    EmissionBuffer m_pureComputeCurrentNodeCounterBuffer;
    EmissionBuffer m_pureComputeCurrentNodeFrontierBuffer;
    CapturedCullingProgram m_pureComputeDenseClusterPipelineState;
    std::shared_ptr<const rhi::CommandSignaturePtr> m_pureComputeDispatchCommandSignature;
    EmissionBuffer m_pureComputeLeafDispatchArgsBuffer;
    CapturedCullingProgram m_pureComputeLeafPipelineState;
    EmissionBuffer m_pureComputeNextLeafCounterBuffer;
    EmissionBuffer m_pureComputeNextLeafFrontierBuffer;
    EmissionBuffer m_pureComputeNextNodeCounterBuffer;
    EmissionBuffer m_pureComputeNextNodeFrontierBuffer;
    EmissionBuffer m_pureComputeNodeDispatchArgsBuffer;
    CapturedCullingProgram m_pureComputeObjectCullPipelineState;
    CapturedCullingProgram m_pureComputeReplayClustersPipelineState;
    CapturedCullingProgram m_pureComputeReplayNodesPipelineState;
    CapturedCullingProgram m_pureComputeTraversePipelineState;
    CLodRasterOutputKind m_rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
    EmissionBuffer m_shadowActiveBlockMetadataBuffer;
    EmissionBuffer m_shadowDirtyHierarchyTexture;
    EmissionBuffer m_shadowDynamicActiveBlockMetadataBuffer;
    EmissionBuffer m_shadowDynamicPhysicalPagesTexture;
    EmissionBuffer m_shadowInvalidatedInstancesBitsetBuffer;
    EmissionBuffer m_shadowInvalidationCountBuffer;
    EmissionBuffer m_shadowPageTableTexture;
    EmissionBuffer m_shadowPhysicalPagesTexture;
    EmissionBuffer m_shadowPredictiveInvalidationCandidateCountBuffer;
    EmissionBuffer m_shadowPredictiveInvalidationCandidatesBuffer;
    EmissionBuffer m_shadowReceiverSubpageMaskBuffer;
    EmissionBuffer m_skinnedVoxelRasterWorkCounterBuffer;
    EmissionBuffer m_swVisibleClustersCounterBuffer;
    EmissionBuffer m_swWriteBaseCounterBuffer;
    EmissionBuffer m_viewRasterInfoBuffer;
    EmissionBuffer m_visibleClusterTransformIndicesBuffer;
    EmissionBuffer m_visibleClustersBuffer;
    EmissionBuffer m_visibleClustersCounterBuffer;
    uint32_t m_voxelRasterWorkCapacity = 0u;
    EmissionBuffer m_voxelRasterWorkCounterBuffer;
    HierarchicalCullingWorkGraphMode m_workGraphMode = HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph;
    EmissionBuffer m_workGraphTelemetryBuffer;
    auto Programs() const {
        return std::array{
            m_clearPipelineState.payload, m_createCommandPipelineState.payload,
            m_pureComputeBuildDispatchArgsPipelineState.payload, m_pureComputeBuildDualDispatchArgsPipelineState.payload,
            m_pureComputeBuildReplayDispatchArgsPipelineState.payload, m_pureComputeClearTraversalCountersPipelineState.payload,
            m_pureComputeClusterPipelineState.payload, m_pureComputeDenseClusterPipelineState.payload,
            m_pureComputeLeafPipelineState.payload, m_pureComputeObjectCullPipelineState.payload,
            m_pureComputeReplayClustersPipelineState.payload, m_pureComputeReplayNodesPipelineState.payload,
            m_pureComputeTraversePipelineState.payload};
    }
};
}
