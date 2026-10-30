#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct VirtualShadowMapBuildMarkTilesBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken tileWork;
    org::DeclaredViewToken tileCount;
};
struct VirtualShadowMapBuildMarkTilesFrame {
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
};

class VirtualShadowMapBuildMarkTilesPass final : public org::TypedRenderGraphPass<VirtualShadowMapBuildMarkTilesPass,
    VirtualShadowMapBuildMarkTilesFrame, VirtualShadowMapBuildMarkTilesBindings> {
public:
    VirtualShadowMapBuildMarkTilesPass(
        std::shared_ptr<org::Buffer> tileWorkBuffer,
        std::shared_ptr<org::Buffer> tileCountBuffer);

    VirtualShadowMapBuildMarkTilesBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void Update(const org::UpdateExecutionContext& executionContext) override;
    VirtualShadowMapBuildMarkTilesFrame Prepare(const VirtualShadowMapBuildMarkTilesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapBuildMarkTilesBindings&,
        const VirtualShadowMapBuildMarkTilesFrame&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::Buffer> m_tileWorkBuffer;
    std::shared_ptr<org::Buffer> m_tileCountBuffer;
};
