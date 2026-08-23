#pragma once

#include <memory>

#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class AVBOITDepthWarpPass final : public ComputePass {
public:
    AVBOITDepthWarpPass(
        std::shared_ptr<Buffer> configBuffer,
        std::shared_ptr<Buffer> occupancyHistogramBuffer,
        std::shared_ptr<Buffer> depthWarpLUTBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_configBuffer;
    std::shared_ptr<Buffer> m_occupancyHistogramBuffer;
    std::shared_ptr<Buffer> m_depthWarpLUTBuffer;
    PipelineState m_pso;
};