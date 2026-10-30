#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapBuildActiveBlocksBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken pageTable;
    org::DeclaredViewToken clipmapInfo;
    org::DeclaredViewToken output;
    bool dynamicPages = false;
};

class VirtualShadowMapBuildActiveBlocksPass final : public org::TypedRenderGraphPass<VirtualShadowMapBuildActiveBlocksPass,
    org::EmptyPassFrameData, VirtualShadowMapBuildActiveBlocksBindings> {
public:
    VirtualShadowMapBuildActiveBlocksPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> activeBlockMetadataBuffer,
        bool dynamicPages = false);

    VirtualShadowMapBuildActiveBlocksBindings Declare(org::PassBuilder& builder);
    void Initialize() {}
    static void Record(const VirtualShadowMapBuildActiveBlocksBindings&, org::PassRecordContext&);
    void ShutdownPass() {}

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_activeBlockMetadataBuffer;
    bool m_dynamicPages = false;
};
