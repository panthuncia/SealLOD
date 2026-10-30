#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapComposePagesBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken staticPages, dynamicPages, pageTable, pageMetadata, stats;
};

struct VirtualShadowMapComposePagesFrame {
    uint32_t pageTableResolution = 0;
    uint32_t maxPhysicalPages = 0;
    uint32_t physicalAtlasPagesWide = 0;
};

class VirtualShadowMapComposePagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapComposePagesPass,
    VirtualShadowMapComposePagesFrame, VirtualShadowMapComposePagesBindings> {
public:
    VirtualShadowMapComposePagesPass(
        std::shared_ptr<org::PixelBuffer> staticPagesTexture,
        std::shared_ptr<org::PixelBuffer> dynamicPagesTexture,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> statsBuffer);

    VirtualShadowMapComposePagesBindings Declare(org::PassBuilder& builder);
    void Initialize() {}
    VirtualShadowMapComposePagesFrame Prepare(const VirtualShadowMapComposePagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapComposePagesBindings&,
        const VirtualShadowMapComposePagesFrame&, org::PassRecordContext&);
    void ShutdownPass() {}

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_staticPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_dynamicPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
};
