#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct VirtualShadowMapBuildMarkTilesBindings {
    org::ResourceBindingToken tileWork;
    org::ResourceBindingToken tileCount;
};

class VirtualShadowMapBuildMarkTilesPass final : public org::TypedRenderGraphPass<VirtualShadowMapBuildMarkTilesPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapBuildMarkTilesBindings> {
public:
    VirtualShadowMapBuildMarkTilesPass(
        std::shared_ptr<org::Buffer> tileWorkBuffer,
        std::shared_ptr<org::Buffer> tileCountBuffer);

    VirtualShadowMapBuildMarkTilesBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext& executionContext) override;
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapBuildMarkTilesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapBuildMarkTilesBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::Buffer> m_tileWorkBuffer;
    std::shared_ptr<org::Buffer> m_tileCountBuffer;
};
