#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITEarlyDepthBuildBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken config, zeroSlice, commands, count;
};
struct AVBOITEarlyDepthBuildFrame {
    uint32_t groupsX = 0, groupsY = 1;
};
class AVBOITEarlyDepthBuildPass final : public org::TypedRenderGraphPass<AVBOITEarlyDepthBuildPass, AVBOITEarlyDepthBuildFrame, AVBOITEarlyDepthBuildBindings> {
public:
    AVBOITEarlyDepthBuildPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture,
        std::shared_ptr<org::Buffer> tileCommandsBuffer,
        std::shared_ptr<org::Buffer> tileCountBuffer);

    AVBOITEarlyDepthBuildBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    AVBOITEarlyDepthBuildFrame Prepare(const AVBOITEarlyDepthBuildBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITEarlyDepthBuildBindings&, const AVBOITEarlyDepthBuildFrame&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_zeroTransmittanceSliceTexture;
    std::shared_ptr<org::Buffer> m_tileCommandsBuffer;
    std::shared_ptr<org::Buffer> m_tileCountBuffer;
    org::PipelineState m_pso;
};
