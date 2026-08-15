#pragma once

#include <memory>

#include "Managers/Singletons/PSOManager.h"
#include "RenderPasses/Base/RenderPass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class AVBOITEarlyDepthPass final : public RenderPass {
public:
    AVBOITEarlyDepthPass(
        std::shared_ptr<Buffer> configBuffer,
        std::shared_ptr<Buffer> tileCommandsBuffer,
        std::shared_ptr<Buffer> tileCountBuffer,
        std::shared_ptr<PixelBuffer> earlyDepthTexture);

    void DeclareResourceUsages(RenderPassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_configBuffer;
    std::shared_ptr<Buffer> m_tileCommandsBuffer;
    std::shared_ptr<Buffer> m_tileCountBuffer;
    std::shared_ptr<PixelBuffer> m_earlyDepthTexture;
    PipelineState m_pso;
    rhi::CommandSignaturePtr m_commandSignature;
};