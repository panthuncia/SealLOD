#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapComposePagesBindings {
    org::ResourceBindingToken staticPages, dynamicPages, pageTable, pageMetadata, stats;
};

class VirtualShadowMapComposePagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapComposePagesPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapComposePagesBindings> {
public:
    VirtualShadowMapComposePagesPass(
        std::shared_ptr<org::PixelBuffer> staticPagesTexture,
        std::shared_ptr<org::PixelBuffer> dynamicPagesTexture,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> statsBuffer);

    VirtualShadowMapComposePagesBindings Declare(org::PassBuilder& builder);
    void Initialize() {}
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapComposePagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapComposePagesBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass() {}

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_staticPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_dynamicPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
};
