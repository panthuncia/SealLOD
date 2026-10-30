#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct AVBOITDepthWarpBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, histogram, lut;
};
struct AVBOITDepthWarpFrame {
    uint32_t groupsX = 0, groupsY = 1;
};
class AVBOITDepthWarpPass final : public org::TypedRenderGraphPass<AVBOITDepthWarpPass, AVBOITDepthWarpFrame, AVBOITDepthWarpBindings> {
public:
    AVBOITDepthWarpPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> occupancyHistogramBuffer,
        std::shared_ptr<org::Buffer> depthWarpLUTBuffer);

    AVBOITDepthWarpBindings Declare(org::PassBuilder& builder);
    AVBOITDepthWarpFrame Prepare(const AVBOITDepthWarpBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITDepthWarpBindings&, const AVBOITDepthWarpFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_occupancyHistogramBuffer;
    std::shared_ptr<org::Buffer> m_depthWarpLUTBuffer;
    org::PipelineState m_pso;
};
