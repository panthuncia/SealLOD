#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITResolveBindings {
    org::ResourceBindingToken config, accumulation, normalization, extinction;
};

class AVBOITResolvePass final : public org::TypedRenderGraphPass<AVBOITResolvePass,
    br::render::PreparedComputeDispatch, AVBOITResolveBindings> {
public:
    AVBOITResolvePass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> accumulationTexture,
        std::shared_ptr<org::PixelBuffer> normalizationTexture,
        std::shared_ptr<org::PixelBuffer> shadingExtinctionTexture);

    AVBOITResolveBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatch Prepare(const AVBOITResolveBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const AVBOITResolveBindings&,
        const br::render::PreparedComputeDispatch&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_accumulationTexture;
    std::shared_ptr<org::PixelBuffer> m_normalizationTexture;
    std::shared_ptr<org::PixelBuffer> m_shadingExtinctionTexture;
    org::PipelineState m_pso;
};
