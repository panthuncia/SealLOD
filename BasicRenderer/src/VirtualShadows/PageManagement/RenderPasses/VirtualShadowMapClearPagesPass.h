#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapClearPagesBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken staticPages, dynamicPages, dirtyFlags, pageTable;
    org::DeclaredViewToken pageMetadata, clipmapInfo, pageViewInfo, stats;
    bool dynamicContentFilter = false;
};

struct VirtualShadowMapClearPagesFrame {
    uint32_t pageTableResolution = 0;
    uint32_t maxPhysicalPages = 0;
    uint32_t physicalAtlasPagesWide = 0;
};

class VirtualShadowMapClearPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapClearPagesPass,
    VirtualShadowMapClearPagesFrame, VirtualShadowMapClearPagesBindings> {
public:
    VirtualShadowMapClearPagesPass(
        std::shared_ptr<org::PixelBuffer> staticPagesTexture,
        std::shared_ptr<org::PixelBuffer> dynamicPagesTexture,
        std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> pageViewInfoBuffer,
        std::shared_ptr<org::Buffer> statsBuffer);

    VirtualShadowMapClearPagesBindings Declare(org::PassBuilder& builder);
    void Initialize();
    VirtualShadowMapClearPagesFrame Prepare(const VirtualShadowMapClearPagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapClearPagesBindings&,
        const VirtualShadowMapClearPagesFrame&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_staticPagesTexture;
    std::shared_ptr<org::PixelBuffer> m_dynamicPagesTexture;
    std::shared_ptr<org::Buffer> m_dirtyPageFlagsBuffer;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_pageViewInfoBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
};
