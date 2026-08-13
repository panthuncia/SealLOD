#pragma once

#include "RenderPasses/Base/ComputePass.h"

class PixelBuffer;

class PerViewLinearDepthCopyPass : public ComputePass {
public:
    explicit PerViewLinearDepthCopyPass(bool writeProjectedDepth = true);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    PixelBuffer* m_pProjectedDepthTexture = nullptr;
    PixelBuffer* m_pCanonicalDeviceDepth = nullptr;
    bool m_writeProjectedDepth = true;
};
