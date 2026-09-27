#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITSparseClearBindings { org::ResourceBindingToken config, occupancy; };
class AVBOITSparseClearPass final : public org::TypedRenderGraphPass<AVBOITSparseClearPass, br::render::PreparedComputeDispatch, AVBOITSparseClearBindings> {
public:
    AVBOITSparseClearPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> occupancyTexture,
        std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<org::PixelBuffer> scalarExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> chromaticExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture);

    AVBOITSparseClearBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatch Prepare(const AVBOITSparseClearBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITSparseClearBindings&, const br::render::PreparedComputeDispatch&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_occupancyTexture;
    std::shared_ptr<org::PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<org::PixelBuffer> m_scalarExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_chromaticExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_zeroTransmittanceSliceTexture;
    org::PipelineState m_pso;
};
