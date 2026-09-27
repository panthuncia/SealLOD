#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapClearPagesBindings {
    org::ResourceBindingToken staticPages, dynamicPages, dirtyFlags, pageTable;
    org::ResourceBindingToken pageMetadata, clipmapInfo, pageViewInfo, stats;
    bool dynamicContentFilter = false;
};

class VirtualShadowMapClearPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapClearPagesPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapClearPagesBindings> {
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
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapClearPagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapClearPagesBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
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
