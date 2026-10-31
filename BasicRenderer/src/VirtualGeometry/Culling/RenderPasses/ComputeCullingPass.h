#pragma once

// Hierarchical cluster culling in plain compute: object culling, then BVH
// traversal one level per indirect dispatch, then dense cluster culling. See
// ComputeCullingCommands.h for the command structure.

#include <cstdint>
#include <memory>
#include <vector>

#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/PreparedTablePublisher.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "VirtualGeometry/Culling/RenderPasses/ComputeCullingCommands.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"

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
    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override;

private:
    void CreateWindCache();
    void CreateFrontiers();
    void CreatePrograms();
    ComputeCullingShadowViews DeclareVirtualShadow(org::PassBuilder& builder) const;
    void UpdateTraversalDepth(const UpdateContext& context);
    br::render::CullingUploader Uploader() const;

    CullingPassConfig m_config;
    CullingPassResources m_resources;
    br::render::CullingVoxelQueueTable m_voxelQueueTable;
    ComputeCullingPrograms m_programs;
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
};
