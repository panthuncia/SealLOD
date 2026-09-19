#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapBuildActiveBlocksBindings {
    org::ResourceBindingToken pageTable;
    org::ResourceBindingToken clipmapInfo;
    org::ResourceBindingToken output;
    bool dynamicPages = false;
};

class VirtualShadowMapBuildActiveBlocksPass final : public org::TypedRenderGraphPass<VirtualShadowMapBuildActiveBlocksPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapBuildActiveBlocksBindings> {
public:
    VirtualShadowMapBuildActiveBlocksPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> activeBlockMetadataBuffer,
        bool dynamicPages = false);

    VirtualShadowMapBuildActiveBlocksBindings Declare(org::PassBuilder& builder);
    void Initialize() {}
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapBuildActiveBlocksBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapBuildActiveBlocksBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass() {}

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_activeBlockMetadataBuffer;
    bool m_dynamicPages = false;
};
