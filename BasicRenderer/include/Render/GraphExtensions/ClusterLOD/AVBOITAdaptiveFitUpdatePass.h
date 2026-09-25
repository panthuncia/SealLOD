#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct AVBOITAdaptiveFitUpdateBindings { org::ResourceBindingToken config, histogram, state; };
class AVBOITAdaptiveFitUpdatePass final : public org::TypedRenderGraphPass<AVBOITAdaptiveFitUpdatePass, br::render::PreparedComputeDispatch, AVBOITAdaptiveFitUpdateBindings> {
public:
    AVBOITAdaptiveFitUpdatePass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> occupancyHistogramBuffer,
        std::shared_ptr<org::Buffer> fitStateBuffer);

    AVBOITAdaptiveFitUpdateBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatch Prepare(const AVBOITAdaptiveFitUpdateBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITAdaptiveFitUpdateBindings&, const br::render::PreparedComputeDispatch&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_occupancyHistogramBuffer;
    std::shared_ptr<org::Buffer> m_fitStateBuffer;
    org::PipelineState m_pso;
};
