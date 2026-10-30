#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct AVBOITAdaptiveFitUpdateBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, histogram, state;
};
struct AVBOITAdaptiveFitUpdateFrame {
    uint32_t groupsX = 0, groupsY = 1;
};
class AVBOITAdaptiveFitUpdatePass final : public org::TypedRenderGraphPass<AVBOITAdaptiveFitUpdatePass, AVBOITAdaptiveFitUpdateFrame, AVBOITAdaptiveFitUpdateBindings> {
public:
    AVBOITAdaptiveFitUpdatePass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> occupancyHistogramBuffer,
        std::shared_ptr<org::Buffer> fitStateBuffer);

    AVBOITAdaptiveFitUpdateBindings Declare(org::PassBuilder& builder);
    AVBOITAdaptiveFitUpdateFrame Prepare(const AVBOITAdaptiveFitUpdateBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITAdaptiveFitUpdateBindings&, const AVBOITAdaptiveFitUpdateFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_occupancyHistogramBuffer;
    std::shared_ptr<org::Buffer> m_fitStateBuffer;
    org::PipelineState m_pso;
};
