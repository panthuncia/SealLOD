#pragma once

#include <memory>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }

struct AVBOITEarlyDepthBuildBindings { org::ResourceBindingToken config, zeroSlice, commands, count; };
class AVBOITEarlyDepthBuildPass final : public org::TypedRenderGraphPass<AVBOITEarlyDepthBuildPass, br::render::PreparedComputeDispatch, AVBOITEarlyDepthBuildBindings> {
public:
    AVBOITEarlyDepthBuildPass(
        std::shared_ptr<org::Buffer> configBuffer,
        std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture,
        std::shared_ptr<org::Buffer> tileCommandsBuffer,
        std::shared_ptr<org::Buffer> tileCountBuffer);

    AVBOITEarlyDepthBuildBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    br::render::PreparedComputeDispatch Prepare(const AVBOITEarlyDepthBuildBindings&, const org::PassPrepareContext&) const;
    static void Record(const AVBOITEarlyDepthBuildBindings&, const br::render::PreparedComputeDispatch&, org::PassRecordContext&);

private:
    std::shared_ptr<org::Buffer> m_configBuffer;
    std::shared_ptr<org::PixelBuffer> m_zeroTransmittanceSliceTexture;
    std::shared_ptr<org::Buffer> m_tileCommandsBuffer;
    std::shared_ptr<org::Buffer> m_tileCountBuffer;
    org::PipelineState m_pso;
};
