#pragma once

#include <cstdint>
#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITIntegrateBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, state, occupancy;
};
struct AVBOITIntegrateFrame {
    uint32_t groupsX = 0, groupsY = 1;
};

class AVBOITIntegratePass final : public org::TypedRenderGraphPass<AVBOITIntegratePass,
    AVBOITIntegrateFrame, AVBOITIntegrateBindings> {
public:
    AVBOITIntegratePass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> fitStateBuffer,
        std::shared_ptr<org::PixelBuffer> occupancyTexture,
        std::shared_ptr<org::PixelBuffer> coverageTexture,
        std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
        std::shared_ptr<org::PixelBuffer> scalarExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> chromaticExtinctionTexture,
        std::shared_ptr<org::PixelBuffer> integratedTransmittanceTexture,
        std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture);

    AVBOITIntegrateBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    AVBOITIntegrateFrame Prepare(const AVBOITIntegrateBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const AVBOITIntegrateBindings&, const AVBOITIntegrateFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_fitStateBuffer;
    std::shared_ptr<org::PixelBuffer> m_occupancyTexture;
    std::shared_ptr<org::PixelBuffer> m_coverageTexture;
    std::shared_ptr<org::PixelBuffer> m_occupancySliceMaskTexture;
    std::shared_ptr<org::PixelBuffer> m_scalarExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_chromaticExtinctionTexture;
    std::shared_ptr<org::PixelBuffer> m_integratedTransmittanceTexture;
    std::shared_ptr<org::PixelBuffer> m_zeroTransmittanceSliceTexture;
    org::PipelineState m_pso;
};
