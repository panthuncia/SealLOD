#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class ReyesDicePass final : public ComputePass {
public:
    ReyesDicePass(
        std::shared_ptr<Buffer> diceQueueBuffer,
        std::shared_ptr<Buffer> diceQueueCounterBuffer,
        std::shared_ptr<Buffer> diceQueueReadOffsetBuffer,
        std::shared_ptr<Buffer> tessTableConfigsBuffer,
        std::shared_ptr<Buffer> indirectArgsBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        uint32_t maxDiceQueueEntries,
        uint32_t phaseIndex);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_diceQueueBuffer;
    std::shared_ptr<Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<Buffer> m_diceQueueReadOffsetBuffer;
    std::shared_ptr<Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<Buffer> m_indirectArgsBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    uint32_t m_maxDiceQueueEntries = 0u;
    uint32_t m_phaseIndex = 0u;
    PipelineState m_pso;
    rhi::CommandSignaturePtr m_commandSignature;
};