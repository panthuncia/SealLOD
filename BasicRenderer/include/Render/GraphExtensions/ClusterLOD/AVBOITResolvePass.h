#pragma once

#include <memory>

#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class AVBOITResolvePass final : public ComputePass {
public:
    AVBOITResolvePass(
        std::shared_ptr<Buffer> configBuffer,
        std::shared_ptr<PixelBuffer> accumulationTexture,
        std::shared_ptr<PixelBuffer> normalizationTexture,
        std::shared_ptr<PixelBuffer> shadingExtinctionTexture);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_configBuffer;
    std::shared_ptr<PixelBuffer> m_accumulationTexture;
    std::shared_ptr<PixelBuffer> m_normalizationTexture;
    std::shared_ptr<PixelBuffer> m_shadingExtinctionTexture;
    PipelineState m_pso;
};
