#pragma once

#include <memory>

#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class AVBOITEarlyDepthBuildPass final : public ComputePass {
public:
    AVBOITEarlyDepthBuildPass(
        std::shared_ptr<Buffer> configBuffer,
        std::shared_ptr<PixelBuffer> zeroTransmittanceSliceTexture,
        std::shared_ptr<Buffer> tileCommandsBuffer,
        std::shared_ptr<Buffer> tileCountBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_configBuffer;
    std::shared_ptr<PixelBuffer> m_zeroTransmittanceSliceTexture;
    std::shared_ptr<Buffer> m_tileCommandsBuffer;
    std::shared_ptr<Buffer> m_tileCountBuffer;
    PipelineState m_pso;
};