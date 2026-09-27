#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct VirtualShadowMapFreeWrappedPagesBindings {
    org::ResourceBindingToken pageTable, pageMetadata, clipmapInfo, stats;
};

class VirtualShadowMapFreeWrappedPagesPass final : public org::TypedRenderGraphPass<VirtualShadowMapFreeWrappedPagesPass,
    br::render::PreparedComputeDispatch, VirtualShadowMapFreeWrappedPagesBindings> {
public:
    VirtualShadowMapFreeWrappedPagesPass(
        std::shared_ptr<org::PixelBuffer> pageTableTexture,
        std::shared_ptr<org::Buffer> pageMetadataBuffer,
        std::shared_ptr<org::Buffer> clipmapInfoBuffer,
        std::shared_ptr<org::Buffer> statsBuffer);

    VirtualShadowMapFreeWrappedPagesBindings Declare(org::PassBuilder& builder);
    void Initialize();
    br::render::PreparedComputeDispatch Prepare(const VirtualShadowMapFreeWrappedPagesBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const VirtualShadowMapFreeWrappedPagesBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);
    void ShutdownPass();

private:
    org::PipelineState m_pso;
    std::shared_ptr<org::PixelBuffer> m_pageTableTexture;
    std::shared_ptr<org::Buffer> m_pageMetadataBuffer;
    std::shared_ptr<org::Buffer> m_clipmapInfoBuffer;
    std::shared_ptr<org::Buffer> m_statsBuffer;
};
