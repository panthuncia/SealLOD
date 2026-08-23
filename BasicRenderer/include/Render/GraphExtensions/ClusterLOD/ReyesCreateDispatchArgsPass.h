#pragma once

#include <memory>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class ReyesCreateDispatchArgsPass final : public ComputePass {
public:
    ReyesCreateDispatchArgsPass(
        std::shared_ptr<Buffer> sourceCounterBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> sourceBaseCounterBuffer = nullptr,
        uint32_t threadsPerGroup = 64u,
        uint32_t maxWorkItemCount = 0xFFFFFFFFu);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    PipelineState m_pso;
    std::shared_ptr<Buffer> m_sourceCounterBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_sourceBaseCounterBuffer;
    uint32_t m_threadsPerGroup = 64u;
    uint32_t m_maxWorkItemCount = 0xFFFFFFFFu;
};