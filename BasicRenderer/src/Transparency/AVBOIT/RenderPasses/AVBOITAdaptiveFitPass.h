#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct AVBOITAdaptiveFitBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, state;
};
struct AVBOITAdaptiveFitFrame {
    uint32_t groupsX = 0, groupsY = 1;
};

class AVBOITAdaptiveFitPass final : public org::TypedRenderGraphPass<AVBOITAdaptiveFitPass,
    AVBOITAdaptiveFitFrame, AVBOITAdaptiveFitBindings> {
public:
    AVBOITAdaptiveFitPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::Buffer> fitStateBuffer);

    AVBOITAdaptiveFitBindings Declare(org::PassBuilder& builder);
    AVBOITAdaptiveFitFrame Prepare(const AVBOITAdaptiveFitBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const AVBOITAdaptiveFitBindings&, const AVBOITAdaptiveFitFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::Buffer> m_fitStateBuffer;
    org::PipelineState m_pso;
};
