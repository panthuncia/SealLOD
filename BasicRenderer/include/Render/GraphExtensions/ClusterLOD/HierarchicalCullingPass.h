#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <boost/container_hash/hash.hpp>
#include <rhi.h>

#include "BuiltinRenderPasses.h"
#include "Managers/Singletons/PSOManager.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodViewTables.h"
#include "Render/PreparedTablePublisher.h"
#include "Render/RenderPhase.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeCommands.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }
namespace org { class ResourceGroup; }

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

struct HierarchicalCullingBindings {
    org::ResourceBindingToken visible, transforms, visibleCounter, swCounter, histogram, telemetry;
    org::ResourceBindingToken replay, replayState, nodeInputs;
    org::ResourceBindingToken shadowPageTable, shadowPhysicalPages, shadowActiveMetadata;
    org::ResourceBindingToken shadowDynamicPages, shadowDynamicMetadata, shadowDirty;
    org::ResourceBindingToken invalidatedInstances, predictiveCandidates, predictiveCount;
    org::ResourceBindingToken phase1Counter, swWriteBase;
    std::array<org::ResourceBindingToken, 4> voxelQueues{};
    std::array<org::ResourceBindingToken, 3> pageJobQueues{};
    org::ResourceBindingToken reyesDice, reyesDiceCounter, reyesOverflow;
    org::ResourceBindingToken reyesConfigs, reyesVertices, reyesTriangles, reyesTelemetry;
    bool hasSw = false, hasViewRasterInfo = false, hasViewDepth = false, hasVirtualShadow = false;
    bool hasShadowDirty = false;
    bool hasShadowRaster = false, hasReyes = false, hasInvalidated = false, hasPredictive = false;
    bool hasPhase1 = false, hasSwWriteBase = false, hasVoxelQueues = false, hasPageJobQueues = false;
};

struct HierarchicalCullingInvocation {
    bool initializeBacking = false;
    br::render::PreparedWorkGraphCpuDispatch cpuDispatch;
};

class HierarchicalCullingPass
    : public org::TypedRenderGraphPass<HierarchicalCullingPass,
          HierarchicalCullingInvocation, HierarchicalCullingBindings, br::render::PreparedComputeCommandSequence>
    , public org::IDynamicDeclaredResources {
public:
    HierarchicalCullingPass(
        std::string stablePassIdentifier,
        HierarchicalCullingPassInputs inputs,
        std::shared_ptr<org::Buffer> visibleClustersBuffer,
        std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
        std::shared_ptr<org::Buffer> swVisibleClustersCounterBuffer,
        std::shared_ptr<org::Buffer> voxelRasterWorkBuffer,
        std::shared_ptr<org::Buffer> voxelRasterWorkCounterBuffer,
        std::shared_ptr<org::Buffer> skinnedVoxelRasterWorkBuffer,
        std::shared_ptr<org::Buffer> skinnedVoxelRasterWorkCounterBuffer,
        uint32_t voxelRasterWorkCapacity,
        std::shared_ptr<org::Buffer> pageJobVisibleClustersBuffer,
        std::shared_ptr<org::Buffer> pageJobVisibleClusterTransformIndicesBuffer,
        std::shared_ptr<org::Buffer> pageJobVisibleClustersCounterBuffer,
        std::shared_ptr<org::Buffer> histogramIndirectCommand,
        std::shared_ptr<org::Buffer> workGraphTelemetryBuffer,
        std::shared_ptr<org::Buffer> occlusionReplayBuffer,
        std::shared_ptr<org::Buffer> occlusionReplayStateBuffer,
        std::shared_ptr<org::Buffer> occlusionNodeGpuInputsBuffer,
        std::shared_ptr<org::Buffer> viewRasterInfoBuffer,
        std::shared_ptr<org::PixelBuffer> shadowDirtyHierarchyTexture = nullptr,
        std::shared_ptr<org::ResourceGroup> slabResourceGroup = nullptr,
        std::shared_ptr<org::Buffer> phase1VisibleClustersCounterBuffer = nullptr,
        std::shared_ptr<org::Buffer> swWriteBaseCounterBuffer = nullptr,
        std::shared_ptr<org::Buffer> shadowPredictiveInvalidationCandidatesBuffer = nullptr,
        std::shared_ptr<org::Buffer> shadowPredictiveInvalidationCandidateCountBuffer = nullptr,
        std::shared_ptr<org::Buffer> shadowInvalidatedInstancesBitsetBuffer = nullptr,
        std::shared_ptr<org::PixelBuffer> shadowPageTableTexture = nullptr,
        std::shared_ptr<org::PixelBuffer> shadowPhysicalPagesTexture = nullptr,
        std::shared_ptr<org::Buffer> shadowActiveBlockMetadataBuffer = nullptr,
        std::shared_ptr<org::PixelBuffer> shadowDynamicPhysicalPagesTexture = nullptr,
        std::shared_ptr<org::Buffer> shadowDynamicActiveBlockMetadataBuffer = nullptr,
        std::shared_ptr<org::Buffer> reyesDiceQueueBuffer = nullptr,
        std::shared_ptr<org::Buffer> reyesDiceQueueCounterBuffer = nullptr,
        std::shared_ptr<org::Buffer> reyesDiceQueueOverflowBuffer = nullptr,
        std::shared_ptr<org::Buffer> reyesTessTableConfigsBuffer = nullptr,
        std::shared_ptr<org::Buffer> reyesTessTableVerticesBuffer = nullptr,
        std::shared_ptr<org::Buffer> reyesTessTableTrianglesBuffer = nullptr,
        std::shared_ptr<org::Buffer> reyesTelemetryBuffer = nullptr,
        uint32_t reyesDiceQueueCapacity = 0u);
    ~HierarchicalCullingPass();

    HierarchicalCullingBindings Declare(org::PassBuilder& builder);
    void Initialize();
    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext&) const;
    br::render::PreparedComputeCommandSequence BuildRecipe(const HierarchicalCullingBindings&,
        const org::PassPrepareContext& preparation) const;
    HierarchicalCullingInvocation PrepareInvocation(const br::render::PreparedComputeCommandSequence&,
        const HierarchicalCullingBindings&, const org::PassPrepareContext&) const;
    static void Record(const br::render::PreparedComputeCommandSequence& recipe, const HierarchicalCullingInvocation& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeCommands(recipe, recording, &data.cpuDispatch, data.initializeBacking);
    }
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override;
    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
    static size_t ReloadAllWorkGraphs();

private:
    void ReloadWorkGraph();
    struct ObjectCullRecord
    {
        uint32_t viewDataIndex;
        uint32_t activeDrawSetIndicesSRVIndex;
        uint32_t activeDrawCount;
        uint32_t drawRecordVisibilityGenerationSRVIndex;
        uint32_t drawRecordVisibilityGenerationCount;
        uint32_t dispatchGridX;
        uint32_t dispatchGridY;
        uint32_t dispatchGridZ;
    };

    void CreatePipelines(
        rhi::Device device,
        rhi::PipelineLayoutHandle globalRootSignature,
        rhi::WorkGraphPtr& outGraph,
        org::PipelineState& outCreateCommandPipeline,
        org::PipelineState& outClearPipeline);

    org::PipelineResources m_pipelineResources;
    // Work-graph generations are immutable shared owners so prepared packets
    // may safely outlive a live-reload replacement on the pass object.
    std::shared_ptr<rhi::WorkGraphPtr> m_workGraph;
    org::PipelineState m_createCommandPipelineState;
    org::PipelineState m_clearPipelineState;
    std::shared_ptr<org::Buffer> m_visibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_visibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_visibleClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_swVisibleClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_voxelRasterWorkBuffer;
    std::shared_ptr<org::Buffer> m_voxelRasterWorkCounterBuffer;
    std::shared_ptr<org::Buffer> m_skinnedVoxelRasterWorkBuffer;
    std::shared_ptr<org::Buffer> m_skinnedVoxelRasterWorkCounterBuffer;
    std::shared_ptr<org::Buffer> m_voxelRasterQueueDescriptorsBuffer;
    std::string m_voxelRasterQueueDescriptorResourceId;
    uint32_t m_voxelRasterWorkCapacity = 0u;
    std::shared_ptr<org::Buffer> m_pageJobVisibleClustersBuffer;
    std::shared_ptr<org::Buffer> m_pageJobVisibleClusterTransformIndicesBuffer;
    std::shared_ptr<org::Buffer> m_pageJobVisibleClustersCounterBuffer;
    std::shared_ptr<org::Buffer> m_workGraphComputePageJobDescriptorsBuffer;
    std::string m_workGraphComputePageJobDescriptorResourceId;
    std::shared_ptr<org::Buffer> m_scratchBuffer;
    std::shared_ptr<org::Buffer> m_histogramIndirectCommand;
    std::shared_ptr<org::Buffer> m_workGraphTelemetryBuffer;
    std::shared_ptr<org::Buffer> m_occlusionReplayBuffer;
    std::shared_ptr<org::Buffer> m_occlusionReplayStateBuffer;
    std::shared_ptr<org::Buffer> m_occlusionNodeGpuInputsBuffer;
    std::shared_ptr<org::Buffer> m_viewRasterInfoBuffer;
    std::shared_ptr<org::Buffer> m_shadowPredictiveInvalidationCandidatesBuffer;
    std::shared_ptr<org::Buffer> m_shadowPredictiveInvalidationCandidateCountBuffer;
    std::shared_ptr<org::Buffer> m_shadowInvalidatedInstancesBitsetBuffer;
    std::shared_ptr<org::PixelBuffer> m_shadowDirtyHierarchyTexture;
    std::shared_ptr<org::PixelBuffer> m_shadowPageTableTexture;
    std::shared_ptr<org::PixelBuffer> m_shadowPhysicalPagesTexture;
    std::shared_ptr<org::Buffer> m_shadowActiveBlockMetadataBuffer;
    std::shared_ptr<org::PixelBuffer> m_shadowDynamicPhysicalPagesTexture;
    std::shared_ptr<org::Buffer> m_shadowDynamicActiveBlockMetadataBuffer;
    std::shared_ptr<org::Buffer> m_reyesDiceQueueBuffer;
    std::shared_ptr<org::Buffer> m_reyesDiceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_reyesDiceQueueOverflowBuffer;
    std::shared_ptr<org::Buffer> m_reyesTessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_reyesTessTableVerticesBuffer;
    std::shared_ptr<org::Buffer> m_reyesTessTableTrianglesBuffer;
    std::shared_ptr<org::Buffer> m_reyesTelemetryBuffer;
    std::shared_ptr<org::ResourceGroup> m_slabResourceGroup;
    std::shared_ptr<org::Buffer> m_phase1VisibleClustersCounterBuffer; // Phase 2 only: Phase 1's HW counter for write offset
    std::shared_ptr<org::Buffer> m_swWriteBaseCounterBuffer; // Phase 2 only: Phase 1's SW counter for top-down write offset
    std::vector<std::shared_ptr<org::PixelBuffer>> m_visibilityBuffers;
    std::vector<uint64_t> m_declaredDrawSetResourceIds;
    std::vector<uint64_t> m_declaredVisibilityBufferIds;
    std::vector<CLodViewRasterInfo> m_cachedViewRasterInfo; // Descriptor-free rows shared with page-job passes.
    // Tables this pass's shaders read. They embed descriptors, so they are
    // published during preparation from the frame's bindings.
    CLodViewRasterInfoTable ViewRasterInfoTable(const org::PassPrepareContext&) const;
    CLodViewDepthTable ViewDepthTable(const org::PassPrepareContext&) const;
    org::PreparedTablePublisher m_viewRasterInfoTable{"CLod Culling View Raster Info"};
    org::PreparedTablePublisher m_viewDepthTable{"CLod Culling View Depth SRV Indices"};
    std::vector<uint32_t> m_zeroTelemetryScratch;
    CLodVoxelRasterQueueDescriptors m_cachedVoxelQueueDescriptors{};
    CLodWorkGraphComputePageJobDescriptors m_cachedPageJobDescriptors{};
    uint64_t m_lastDrawSetDeclarationRevision = 0u;
    uint64_t m_lastViewResourceLayoutRevision = 0u;
    bool m_hasCachedVoxelQueueDescriptors = false;
    bool m_hasCachedPageJobDescriptors = false;
    struct WorkGraphInitializationState { std::atomic_bool initialized{false}; };
    std::shared_ptr<WorkGraphInitializationState> m_workGraphInitialization =
        std::make_shared<WorkGraphInitializationState>();
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
