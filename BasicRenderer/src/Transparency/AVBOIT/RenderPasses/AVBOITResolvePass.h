#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITResolveBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, accumulation, normalization, extinction;
};
struct AVBOITResolveFrame {
    uint32_t groupsX = 0, groupsY = 1;
};

class AVBOITResolvePass final : public org::TypedRenderGraphPass<AVBOITResolvePass,
    AVBOITResolveFrame, AVBOITResolveBindings> {
public:
    AVBOITResolvePass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> accumulationTexture,
        std::shared_ptr<org::PixelBuffer> normalizationTexture,
        std::shared_ptr<org::PixelBuffer> shadingExtinctionTexture);

    AVBOITResolveBindings Declare(org::PassBuilder& builder);
    AVBOITResolveFrame Prepare(const AVBOITResolveBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const AVBOITResolveBindings&, const AVBOITResolveFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_accumulationTexture;
    std::shared_ptr<org::PixelBuffer> m_normalizationTexture;
    std::shared_ptr<org::PixelBuffer> m_shadingExtinctionTexture;
    org::PipelineState m_pso;
};
