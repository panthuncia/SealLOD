#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct AVBOITDepthWarpBindings { org::ResourceBindingToken config, histogram, lut; };
class AVBOITDepthWarpPass final : public org::TypedRenderGraphPass<AVBOITDepthWarpPass, br::render::PreparedComputeDispatch, AVBOITDepthWarpBindings> {
public:
    AVBOITDepthWarpPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> occupancyHistogramBuffer,
        std::shared_ptr<org::Buffer> depthWarpLUTBuffer);

    AVBOITDepthWarpBindings Declare(org::PassBuilder& builder);
    br::render::PreparedComputeDispatch Prepare(const AVBOITDepthWarpBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITDepthWarpBindings&, const br::render::PreparedComputeDispatch&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_occupancyHistogramBuffer;
    std::shared_ptr<org::Buffer> m_depthWarpLUTBuffer;
    org::PipelineState m_pso;
};
