#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <rhi.h>

#include "BuiltinRenderPasses.h"
#include "Managers/Singletons/PSOManager.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderPhase.h"
#include "RenderPasses/Base/ComputePass.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"

class Buffer;
class PixelBuffer;
class ResourceGroup;

enum class HierarchicalCullingWorkGraphMode : uint8_t {
    HardwareOnly,
    SoftwareRasterCompute,
    SoftwareRasterWorkGraph,
};

enum class HierarchicalCullingBackend : uint8_t {
    WorkGraph,
    PureCompute,
};

struct HierarchicalCullingPassInputs {
    bool isFirstPass;
    unsigned int maxVisibleClusters;
    HierarchicalCullingBackend backend = HierarchicalCullingBackend::WorkGraph;
    HierarchicalCullingWorkGraphMode workGraphMode = HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph;
    bool workGraphReyesVisibility = false;
    RenderPhase renderPhase;
    bool clodOnlyWorkloads = false;
    bool useShadowCascadeViews = false;
    CLodRasterOutputKind rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;

    RG_DEFINE_PASS_INPUTS(HierarchicalCullingPassInputs, &HierarchicalCullingPassInputs::isFirstPass, &HierarchicalCullingPassInputs::maxVisibleClusters, &HierarchicalCullingPassInputs::backend, &HierarchicalCullingPassInputs::workGraphMode, &HierarchicalCullingPassInputs::workGraphReyesVisibility, &HierarchicalCullingPassInputs::renderPhase, &HierarchicalCullingPassInputs::clodOnlyWorkloads, &HierarchicalCullingPassInputs::useShadowCascadeViews, &HierarchicalCullingPassInputs::rasterOutputKind);
};

class HierarchicalCullingPass : public ComputePass, public IDynamicDeclaredResources {
public:
    HierarchicalCullingPass(
        std::string stablePassIdentifier,
        HierarchicalCullingPassInputs inputs,
        std::shared_ptr<Buffer> visibleClustersBuffer,
        std::shared_ptr<Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<Buffer> swVisibleClustersCounterBuffer,
        std::shared_ptr<Buffer> voxelRasterWorkBuffer,
        std::shared_ptr<Buffer> voxelRasterWorkCounterBuffer,
        std::shared_ptr<Buffer> skinnedVoxelRasterWorkBuffer,
        std::shared_ptr<Buffer> skinnedVoxelRasterWorkCounterBuffer,
        uint32_t voxelRasterWorkCapacity,
        std::shared_ptr<Buffer> pageJobVisibleClustersBuffer,
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
        std::shared_ptr<Buffer> reyesDiceQueueBuffer = nullptr,
        std::shared_ptr<Buffer> reyesDiceQueueCounterBuffer = nullptr,
        std::shared_ptr<Buffer> reyesDiceQueueOverflowBuffer = nullptr,
        std::shared_ptr<Buffer> reyesTessTableConfigsBuffer = nullptr,
        std::shared_ptr<Buffer> reyesTessTableVerticesBuffer = nullptr,
        std::shared_ptr<Buffer> reyesTessTableTrianglesBuffer = nullptr,
        std::shared_ptr<Buffer> reyesTelemetryBuffer = nullptr,
        uint32_t reyesDiceQueueCapacity = 0u);
    ~HierarchicalCullingPass();

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    void Cleanup() override;
    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
    std::vector<ResourceIdentifier> GetSupportedKeys() override;

private:
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

    void CreatePipelines(
        rhi::Device device,
        rhi::PipelineLayoutHandle globalRootSignature,
        rhi::WorkGraphPtr& outGraph,
        PipelineState& outCreateCommandPipeline,
        PipelineState& outClearPipeline);

    PipelineResources m_pipelineResources;
    rhi::WorkGraphPtr m_workGraph;
    PipelineState m_createCommandPipelineState;
    PipelineState m_clearPipelineState;
    std::shared_ptr<Buffer> m_visibleClustersBuffer;
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
    std::shared_ptr<Buffer> m_pageJobVisibleClustersCounterBuffer;
    std::shared_ptr<Buffer> m_workGraphComputePageJobDescriptorsBuffer;
    std::string m_workGraphComputePageJobDescriptorResourceId;
    std::shared_ptr<Buffer> m_scratchBuffer;
    std::shared_ptr<Buffer> m_histogramIndirectCommand;
    std::shared_ptr<Buffer> m_workGraphTelemetryBuffer;
    std::shared_ptr<Buffer> m_occlusionReplayBuffer;
    std::shared_ptr<Buffer> m_occlusionReplayStateBuffer;
    std::shared_ptr<Buffer> m_occlusionNodeGpuInputsBuffer;
    std::shared_ptr<Buffer> m_viewDepthSrvIndicesBuffer;
    std::shared_ptr<Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<Buffer> m_shadowPredictiveInvalidationCandidatesBuffer;
    std::shared_ptr<Buffer> m_shadowPredictiveInvalidationCandidateCountBuffer;
    std::shared_ptr<Buffer> m_shadowInvalidatedInstancesBitsetBuffer;
    std::shared_ptr<PixelBuffer> m_shadowDirtyHierarchyTexture;
    std::shared_ptr<PixelBuffer> m_shadowPageTableTexture;
    std::shared_ptr<PixelBuffer> m_shadowPhysicalPagesTexture;
    std::shared_ptr<Buffer> m_reyesDiceQueueBuffer;
    std::shared_ptr<Buffer> m_reyesDiceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_reyesDiceQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableVerticesBuffer;
    std::shared_ptr<Buffer> m_reyesTessTableTrianglesBuffer;
    std::shared_ptr<Buffer> m_reyesTelemetryBuffer;
    std::shared_ptr<ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<Buffer> m_phase1VisibleClustersCounterBuffer; // Phase 2 only: Phase 1's HW counter for write offset
    std::shared_ptr<Buffer> m_swWriteBaseCounterBuffer; // Phase 2 only: Phase 1's SW counter for top-down write offset
    std::vector<std::shared_ptr<PixelBuffer>> m_visibilityBuffers;
    std::vector<uint64_t> m_declaredDrawSetResourceIds;
    std::vector<uint64_t> m_declaredVisibilityBufferIds;
    std::vector<CLodViewRasterInfo> m_cachedViewRasterInfo;
    std::vector<CLodViewDepthSRVIndex> m_cachedViewDepthSrvIndices;
    std::vector<uint32_t> m_zeroTelemetryScratch;
    std::array<CLodNodeGpuInput, 5> m_cachedNodeGpuInputs{};
    CLodVoxelRasterQueueDescriptors m_cachedVoxelQueueDescriptors{};
    CLodWorkGraphComputePageJobDescriptors m_cachedPageJobDescriptors{};
    uint64_t m_lastDrawSetDeclarationRevision = 0u;
    uint64_t m_lastViewResourceLayoutRevision = 0u;
    bool m_hasCachedNodeGpuInputs = false;
    bool m_hasCachedVoxelQueueDescriptors = false;
    bool m_hasCachedPageJobDescriptors = false;
    bool m_hasUploadedViewDepthSrvIndices = false;
    bool m_isFirstPass = true;
    bool m_declaredResourcesChanged = true;
    unsigned int m_maxVisibleClusters = 0u;
    uint32_t m_reyesDiceQueueCapacity = 0u;
    HierarchicalCullingWorkGraphMode m_workGraphMode = HierarchicalCullingWorkGraphMode::SoftwareRasterWorkGraph;
    CLodRasterOutputKind m_rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
    RenderPhase m_renderPhase;
    bool m_clodOnlyWorkloads = false;
    bool m_useShadowCascadeViews = false;
    bool m_workGraphReyesVisibility = false;
};
