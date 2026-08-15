#pragma once

#include <memory>
#include <vector>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/ComputePass.h"

namespace org { class Buffer; }
using org::Buffer;

class ReyesQueueResetPass final : public ComputePass {
public:
    ReyesQueueResetPass(
        std::shared_ptr<Buffer> fullClusterCounter,
        std::shared_ptr<Buffer> ownedClusterCounter,
        std::vector<std::shared_ptr<Buffer>> splitQueueCounters,
        std::vector<std::shared_ptr<Buffer>> splitQueueOverflowCounters,
        std::shared_ptr<Buffer> diceQueueCounter,
        std::shared_ptr<Buffer> diceQueueOverflowCounter,
        std::shared_ptr<Buffer> ownershipBitsetBuffer,
        std::shared_ptr<Buffer> telemetryBuffer,
        uint32_t phaseIndex,
        bool clearDiceQueueCounter = true,
        std::shared_ptr<Buffer> replaySplitQueueCounter = nullptr,
        std::shared_ptr<Buffer> replaySplitQueueOverflowCounter = nullptr,
        std::shared_ptr<Buffer> replayDiceQueueCounter = nullptr,
        std::shared_ptr<Buffer> replayDiceQueueOverflowCounter = nullptr);

    void DeclareResourceUsages(ComputePassBuilder* builder) override;
    void Setup() override;
    PassReturn Execute(PassExecutionContext& executionContext) override;
    void Update(const UpdateExecutionContext& executionContext) override;
    void Cleanup() override;

private:
    std::shared_ptr<Buffer> m_fullClusterCounter;
    std::shared_ptr<Buffer> m_ownedClusterCounter;
    std::vector<std::shared_ptr<Buffer>> m_splitQueueCounters;
    std::vector<std::shared_ptr<Buffer>> m_splitQueueOverflowCounters;
    std::shared_ptr<Buffer> m_diceQueueCounter;
    std::shared_ptr<Buffer> m_diceQueueOverflowCounter;
    std::shared_ptr<Buffer> m_ownershipBitsetBuffer;
    std::shared_ptr<Buffer> m_telemetryBuffer;
    std::shared_ptr<Buffer> m_replaySplitQueueCounter;
    std::shared_ptr<Buffer> m_replaySplitQueueOverflowCounter;
    std::shared_ptr<Buffer> m_replayDiceQueueCounter;
    std::shared_ptr<Buffer> m_replayDiceQueueOverflowCounter;
    uint32_t m_phaseIndex = 0u;
    bool m_clearDiceQueueCounter = true;
    uint32_t m_ownershipBitsetWordCount = 0u;
    PipelineState m_clearCountersPso;
    PipelineState m_clearOwnershipBitsetPso;
};
