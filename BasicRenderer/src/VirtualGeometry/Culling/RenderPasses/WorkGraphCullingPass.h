#pragma once

// Hierarchical cluster culling as one D3D12 work graph. Object culling, BVH
// traversal, cluster culling and, per variant, software raster and Reyes patch
// visibility run as nodes of a single graph launch per phase. Phase 1 launches
// from CPU records (one per culling workload); phase 2 launches from GPU records
// that replay what phase 1 rejected for occlusion.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rhi.h>

#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeCommands.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/PreparedTablePublisher.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "VirtualGeometry/Culling/RenderPasses/CullingPassCommon.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"

// Virtual shadow state the graph reads and writes.
struct WorkGraphVirtualShadowBindings {
    org::DeclaredViewToken pageTable, activeMetadata;
    std::optional<org::DeclaredViewToken> dirtyHierarchy, invalidatedInstances;
    struct PredictiveInvalidation { org::DeclaredViewToken candidates, count; };
    std::optional<PredictiveInvalidation> predictiveInvalidation;
    // The graph's software raster node writes shadow pages directly.
    struct PageRaster { org::DeclaredViewToken staticPages, dynamicPages, dynamicMetadata; };
    std::optional<PageRaster> pageRaster;
};

struct WorkGraphReyesBindings {
    org::DeclaredViewToken diceQueue, diceQueueCounter, diceQueueOverflow;
    org::DeclaredViewToken tessTableConfigs, tessTableVertices, tessTableTriangles, telemetry;
};

struct WorkGraphCullingBindings {
    org::DeclaredTableLayout<CLodViewRasterInfo> viewRasterInfoLayout;  // SW classification only.
    org::DeclaredTableLayout<CLodViewDepthSRVIndex> viewDepthLayout;    // Per-view depth occlusion only.
    org::DeclaredViewToken visible, transforms, visibleCounter, visibleCounterSrv, swCounter, histogram, telemetry;
    org::DeclaredViewToken replay, replayState, replayStateSrv, nodeInputs;
    std::optional<org::DeclaredViewToken> phase1Counter, phase1SwCounter;
    std::optional<std::array<org::DeclaredViewToken, 4>> voxelQueues;
    std::optional<WorkGraphVirtualShadowBindings> virtualShadow;
    std::optional<WorkGraphReyesBindings> reyes;
};

struct WorkGraphCullingInvocation {
    bool initializeBacking = false;
    br::render::PreparedWorkGraphCpuDispatch cpuDispatch;
};

class WorkGraphCullingPass
    : public org::TypedRenderGraphPass<WorkGraphCullingPass,
          WorkGraphCullingInvocation, WorkGraphCullingBindings, br::render::PreparedComputeCommandSequence>
    , public org::IDynamicDeclaredResources {
public:
    WorkGraphCullingPass(CullingPassConfig config, CullingPassResources resources);
    ~WorkGraphCullingPass();

    WorkGraphCullingBindings Declare(org::PassBuilder& builder);
    void Initialize() {}
    std::vector<uint64_t> RecipeRevision(const org::PassPrepareContext&) const;
    br::render::PreparedComputeCommandSequence BuildRecipe(const WorkGraphCullingBindings&,
        const org::PassPrepareContext& preparation) const;
    WorkGraphCullingInvocation PrepareInvocation(const br::render::PreparedComputeCommandSequence&,
        const WorkGraphCullingBindings&, const org::PassPrepareContext&) const;
    static void Record(const br::render::PreparedComputeCommandSequence& recipe, const WorkGraphCullingInvocation& data,
        org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeCommands(recipe, recording, &data.cpuDispatch, data.initializeBacking);
    }
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override { return m_declaredResourcesChanged; }
    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override;

    // Recompiles every live pass's work graph (shader hot reload).
    static size_t ReloadAllWorkGraphs();

private:
    struct ObjectCullRecord {
        uint32_t viewDataIndex;
        uint32_t activeDrawSetIndicesSRVIndex;
        uint32_t activeDrawCount;
        uint32_t drawRecordVisibilityGenerationSRVIndex;
        uint32_t drawRecordVisibilityGenerationCount;
        uint32_t dispatchGridX;
        uint32_t dispatchGridY;
        uint32_t dispatchGridZ;
    };
    struct CompiledWorkGraph {
        rhi::WorkGraphPtr graph;
        org::PipelineResources resources;
    };
    struct WorkGraphInitializationState { std::atomic_bool initialized{false}; };

    CompiledWorkGraph CompileWorkGraph() const;
    void InstallWorkGraph(CompiledWorkGraph compiled);
    void DeclareSceneInputs(org::PassBuilder& builder);
    std::optional<WorkGraphVirtualShadowBindings> DeclareVirtualShadow(org::PassBuilder& builder);
    std::optional<WorkGraphReyesBindings> DeclareReyes(org::PassBuilder& builder);
    // The root constants every node of the graph reads.
    void FillGraphConstants(uint32_t* root, const WorkGraphCullingBindings& bindings,
        const br::render::CullingSettings& settings, const org::PassPrepareContext& preparation) const;
    br::render::CullingUploader Uploader() const;

    CullingPassConfig m_config;
    CullingPassResources m_resources;
    br::render::CullingVoxelQueueTable m_voxelQueueTable;

    // Work-graph generations are immutable shared owners so prepared packets
    // may safely outlive a live-reload replacement on the pass object.
    std::shared_ptr<rhi::WorkGraphPtr> m_workGraph;
    org::PipelineResources m_workGraphResources;
    std::shared_ptr<org::Buffer> m_scratchBuffer;
    std::shared_ptr<WorkGraphInitializationState> m_workGraphInitialization =
        std::make_shared<WorkGraphInitializationState>();
    org::PipelineState m_createCommandPipelineState;
    org::PipelineState m_clearPipelineState;

    // Tables the graph reads. They embed descriptors, so they are published
    // during preparation from the frame's bindings.
    CLodDeclaredViewRasterTable m_viewRasterInfoLayout;
    CLodDeclaredViewDepthTable m_viewDepthLayout;
    org::PreparedTablePublisher m_viewRasterInfoTable{"CLod Culling View Raster Info"};
    org::PreparedTablePublisher m_viewDepthTable{"CLod Culling View Depth SRV Indices"};

    br::render::CullingDrawSetTracker m_drawSets;
    std::vector<uint32_t> m_zeroTelemetryScratch;
    bool m_declaredResourcesChanged = true;
};
