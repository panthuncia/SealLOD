#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

enum class ReyesReplayMergeKind
{
    Split,
    Dice
};

class ReyesReplayMergePass final : public ComputePass {
public:
    ReyesReplayMergePass(
        ReyesReplayMergeKind kind,
        std::shared_ptr<Buffer> sourceQueueBuffer,
        std::shared_ptr<Buffer> sourceQueueCounterBuffer,
        std::shared_ptr<Buffer> destQueueBuffer,
        std::shared_ptr<Buffer> destQueueCounterBuffer,
        std::shared_ptr<Buffer> destQueueOverflowBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        uint32_t destQueueCapacity);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    void Update(const UpdateExecutionContext& executionContext) override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    ReyesReplayMergeKind m_kind = ReyesReplayMergeKind::Split;
    std::shared_ptr<Buffer> m_sourceQueueBuffer;
    std::shared_ptr<Buffer> m_sourceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_destQueueBuffer;
    std::shared_ptr<Buffer> m_destQueueCounterBuffer;
    std::shared_ptr<Buffer> m_destQueueOverflowBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    uint32_t m_destQueueCapacity = 0u;
    PipelineState m_pso;
    rhi::CommandSignaturePtr m_commandSignature;
};
