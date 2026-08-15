#pragma once

#include <memory>

#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class RasterBucketBlockScanPass : public ComputePass {
public:
    RasterBucketBlockScanPass(
        std::shared_ptr<Buffer> histogramBuffer,
        std::shared_ptr<Buffer> offsetsBuffer,
        std::shared_ptr<Buffer> blockSumsBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    uint32_t m_blockSize = 1024;
    std::shared_ptr<Buffer> m_histogramBuffer;
    std::shared_ptr<Buffer> m_offsetsBuffer;
    std::shared_ptr<Buffer> m_blockSumsBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
