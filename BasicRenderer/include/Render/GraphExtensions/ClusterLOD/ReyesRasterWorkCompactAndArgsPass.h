#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class ReyesRasterWorkCompactAndArgsPass final : public ComputePass {
public:
    ReyesRasterWorkCompactAndArgsPass(
        std::shared_ptr<Buffer> rasterWorkBuffer,
        std::shared_ptr<Buffer> rasterWorkCounterBuffer,
        std::shared_ptr<Buffer> indirectCommand,
        std::shared_ptr<Buffer> histogramBuffer,
        std::shared_ptr<Buffer> offsetsBuffer,
        std::shared_ptr<Buffer> writeCursorBuffer,
        std::shared_ptr<Buffer> compactedRasterWorkIndicesBuffer,
        std::shared_ptr<Buffer> packedRasterWorkGroupsBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    PipelineState m_packPipeline;
    PipelineState m_finalizePackPipeline;
    PipelineState m_clearPipeline;
    rhi::CommandSignaturePtr m_compactionCommandSignature;

    std::shared_ptr<Buffer> m_rasterWorkBuffer;
    std::shared_ptr<Buffer> m_rasterWorkCounterBuffer;
    std::shared_ptr<Buffer> m_indirectCommand;
    std::shared_ptr<Buffer> m_histogramBuffer;
    std::shared_ptr<Buffer> m_offsetsBuffer;
    std::shared_ptr<Buffer> m_writeCursorBuffer;
    std::shared_ptr<Buffer> m_compactedRasterWorkIndicesBuffer;
    std::shared_ptr<Buffer> m_packedRasterWorkGroupsBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
};
