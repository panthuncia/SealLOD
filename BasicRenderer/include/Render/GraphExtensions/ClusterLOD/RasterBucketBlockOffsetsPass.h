#pragma once

#include <memory>

#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class RasterBucketBlockOffsetsPass : public ComputePass {
public:
    RasterBucketBlockOffsetsPass(
        std::shared_ptr<Buffer> offsetsBuffer,
        std::shared_ptr<Buffer> blockSumsBuffer,
        std::shared_ptr<Buffer> scannedBlockSumsBuffer,
        std::shared_ptr<Buffer> totalCountBuffer,
        bool runWhenComputeSWRasterEnabledOnly = false);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    uint32_t m_blockSize = 1024;
    std::shared_ptr<Buffer> m_offsetsBuffer;
    std::shared_ptr<Buffer> m_blockSumsBuffer;
    std::shared_ptr<Buffer> m_scannedBlockSumsBuffer;
    std::shared_ptr<Buffer> m_totalCountBuffer;
    bool m_runWhenComputeSWRasterEnabledOnly = false;
};
