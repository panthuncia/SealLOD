#pragma once

// Hierarchical cluster culling in plain compute: object culling, then BVH
// traversal one level per indirect dispatch, then dense cluster culling. See
// ComputeCullingCommands.h for the command structure.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/PreparedTablePublisher.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "VirtualGeometry/Culling/RenderPasses/ComputeCullingCommands.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include "BasicRenderer/Extensions/PassRevision.h"

class ComputeCullingPass
    : public org::TypedRenderGraphPass<ComputeCullingPass, ComputeCullingFrame, ComputeCullingBindings>
    , public org::IDynamicDeclaredResources {
public:
    ComputeCullingPass(CullingPassConfig config, CullingPassResources resources);
    ~ComputeCullingPass() override;

    ComputeCullingBindings Declare(org::PassBuilder& builder);
    void Initialize() {}
    ComputeCullingFrame Prepare(const ComputeCullingBindings&, const org::PassPrepareContext&) const;
    static void Record(const ComputeCullingBindings& bindings, const ComputeCullingFrame& frame,
        org::PassRecordContext& context) {
        RecordComputeCulling(bindings, frame, context);
    }
    void Update(const org::UpdateExecutionContext& executionContext) override;
    bool DeclaredResourcesChanged() const override { return m_declaredResourcesChanged; }
    // Kept recordings (M6 step 8): the culling settings, the bucket count and what the first pass's workloads come from
    // (the published indirect workloads and the views), taken from the host data before Update
    // (RenderGraphPass::TakeFrameValues) and read by Prepare. Revision names them, the pipelines and what Update took
    // (traversal depth, view tables, the DynamicWind bounds cache generation, which Update advances each run while the cache
    // exists, so such a pass records every run).
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override;
    void Revision(std::vector<uint64_t>& out) const;
    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override;

private:
    void CreateWindCache();
    void CreateFrontiers();
    ComputeCullingPrograms DeclarePrograms(org::PassBuilder& builder) const;
    void ValidatePrograms(const ComputeCullingPrograms& programs, org::BackendInstanceId device) const;
    ComputeCullingShadowViews DeclareVirtualShadow(org::PassBuilder& builder) const;
    void UpdateTraversalDepth(const UpdateContext& context);
    // Takes this frame's views into the view tables; returns whether what the pass declares changed.
    bool UpdateViewTables(const UpdateContext& context);
    br::render::CullingUploader Uploader() const;

    CullingPassConfig m_config;
    CullingPassResources m_resources;
    br::render::CullingVoxelQueueTable m_voxelQueueTable;
    mutable std::atomic<const org::PipelineStatePayload*> m_validatedPrograms{nullptr};
    ComputeCullingFrontiers m_frontiers;
    ComputeCullingWindCache m_windCache;
    uint32_t m_sizedFrontierCapacity = 0u;
    // The deepest BVH level resident geometry reaches; one traversal step each.
    uint32_t m_traversalDepth = 0u;

    // Tables the shaders read. They embed descriptors, so they are published
    // during preparation from the frame's bindings.
    CLodDeclaredViewRasterTable m_viewRasterInfoLayout;
    CLodDeclaredViewDepthTable m_viewDepthLayout;
    org::PreparedTablePublisher m_viewRasterInfoTable{"CLod Dispatch Culling View Raster Info"};
    org::PreparedTablePublisher m_viewDepthTable{"CLod Dispatch Culling View Depth SRV Indices"};

    std::vector<uint32_t> m_zeroTelemetryScratch;
    bool m_declaredResourcesChanged = true;

    ComputeCullingPrograms m_programs;
    struct Taken {
        br::render::CullingSettings settings;
        uint32_t cullingFlags = 0u;
        uint32_t expansionFactor = 1u;
        uint32_t rasterBucketCount = 0u;
        // The first pass's workload inputs: the published indirect workloads (immutable per payload) and the views.
        const void* indirectWorkloads = nullptr;
        uint64_t viewsHash = 0u;
        bool hasContext = false;
    };
    Taken m_taken;
};
