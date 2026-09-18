#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }
using org::Buffer;

struct ReyesQueueResetBindings {
    org::ResourceBindingToken fullClusterCounter;
    org::ResourceBindingToken ownedClusterCounter;
    std::vector<org::ResourceBindingToken> splitQueueCounters;
    std::vector<org::ResourceBindingToken> splitQueueOverflowCounters;
    org::ResourceBindingToken diceQueueCounter;
    org::ResourceBindingToken diceQueueOverflowCounter;
    org::ResourceBindingToken telemetry;
    std::optional<org::ResourceBindingToken> replaySplitQueueCounter;
    std::optional<org::ResourceBindingToken> replaySplitQueueOverflowCounter;
    std::optional<org::ResourceBindingToken> replayDiceQueueCounter;
    std::optional<org::ResourceBindingToken> replayDiceQueueOverflowCounter;
    std::optional<org::ResourceBindingToken> ownershipBitset;
};

class ReyesQueueResetPass final : public org::TypedRenderGraphPass<ReyesQueueResetPass,
    br::render::PreparedComputePipelineSequence, ReyesQueueResetBindings> {
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

    ReyesQueueResetBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputePipelineSequence Prepare(
        const ReyesQueueResetBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesQueueResetBindings&,
        const br::render::PreparedComputePipelineSequence&, org::PassRecordContext&);
    void Update(const UpdateExecutionContext& executionContext) override;

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
