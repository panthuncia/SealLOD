#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITSparseClearBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, occupancy;
};
struct AVBOITSparseClearFrame {
    uint32_t groupsX = 0, groupsY = 1;
};
class AVBOITSparseClearPass final : public org::TypedRenderGraphPass<AVBOITSparseClearPass, AVBOITSparseClearFrame, AVBOITSparseClearBindings> {
public:
    AVBOITSparseClearPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> occupancyTexture,
        std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<org::PixelBuffer> scalarExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> chromaticExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture);

    AVBOITSparseClearBindings Declare(org::PassBuilder& builder);
    AVBOITSparseClearFrame Prepare(const AVBOITSparseClearBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITSparseClearBindings&, const AVBOITSparseClearFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_occupancyTexture;
    std::shared_ptr<org::PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<org::PixelBuffer> m_scalarExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_chromaticExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_zeroTransmittanceSliceTexture;
    org::PipelineState m_pso;
};
