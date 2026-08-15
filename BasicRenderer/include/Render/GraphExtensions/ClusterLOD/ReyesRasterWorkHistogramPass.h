#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class ReyesRasterWorkHistogramPass final : public ComputePass {
public:
    ReyesRasterWorkHistogramPass(
        std::shared_ptr<Buffer> rasterWorkBuffer,
        std::shared_ptr<Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<Buffer> histogramIndirectCommand,
        std::shared_ptr<Buffer> histogramBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    void CreatePipelines(
        rhi::Device device,
        rhi::PipelineLayoutHandle globalRootSignature,
        PipelineState& outHistogramPipeline,
        PipelineState& outClearPipeline);

    PipelineState m_histogramPipeline;
    PipelineState m_clearPipeline;
    rhi::CommandSignaturePtr m_histogramCommandSignature;
    std::shared_ptr<Buffer> m_rasterWorkBuffer;
    std::shared_ptr<Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_histogramIndirectCommand;
    std::shared_ptr<Buffer> m_histogramBuffer;
};
