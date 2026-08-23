#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;
namespace org { class PixelBuffer; }
using org::PixelBuffer;

class VirtualShadowMapMarkPagesPass final : public ComputePass {
public:
    VirtualShadowMapMarkPagesPass(
        std::shared_ptr<Buffer> tileWorkBuffer,
        std::shared_ptr<Buffer> tileCountBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> markClipmapDataBuffer,
        std::shared_ptr<Buffer> markedBlocksMaskBuffer,
        std::shared_ptr<Buffer> markedBlocksListBuffer,
        std::shared_ptr<Buffer> markedBlocksCountBuffer,
        std::shared_ptr<Buffer> receiverSubpageMaskBuffer);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    PipelineState m_clearPso;
    PipelineState m_clearUint2Pso;
    rhi::CommandSignaturePtr m_commandSignature;
    std::shared_ptr<Buffer> m_tileWorkBuffer;
    std::shared_ptr<Buffer> m_tileCountBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_markClipmapDataBuffer;
    std::shared_ptr<Buffer> m_markedBlocksMaskBuffer;
    std::shared_ptr<Buffer> m_markedBlocksListBuffer;
    std::shared_ptr<Buffer> m_markedBlocksCountBuffer;
    std::shared_ptr<Buffer> m_receiverSubpageMaskBuffer;
    uint32_t m_activeClipmapCount = 0u;
    uint32_t m_receiverSubpageMode = 0u;
};
