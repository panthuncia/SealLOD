#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct ReyesQueueResetBindings {
    org::DeclaredViewToken fullClusterCounter;
    org::DeclaredViewToken ownedClusterCounter;
    std::vector<org::DeclaredViewToken> splitQueueCounters;
    std::vector<org::DeclaredViewToken> splitQueueOverflowCounters;
    org::DeclaredViewToken diceQueueCounter;
    org::DeclaredViewToken diceQueueOverflowCounter;
    org::DeclaredViewToken telemetry;
    std::optional<org::DeclaredViewToken> replaySplitQueueCounter;
    std::optional<org::DeclaredViewToken> replaySplitQueueOverflowCounter;
    std::optional<org::DeclaredViewToken> replayDiceQueueCounter;
    std::optional<org::DeclaredViewToken> replayDiceQueueOverflowCounter;
    std::optional<org::DeclaredViewToken> ownershipBitset;
};

class ReyesQueueResetPass final : public org::TypedRenderGraphPass<ReyesQueueResetPass,
    br::render::PreparedComputePipelineSequence, ReyesQueueResetBindings> {
public:
    ReyesQueueResetPass(
        std::shared_ptr<org::Buffer> fullClusterCounter,
        std::shared_ptr<org::Buffer> ownedClusterCounter,
        std::vector<std::shared_ptr<org::Buffer>> splitQueueCounters,
        std::vector<std::shared_ptr<org::Buffer>> splitQueueOverflowCounters,
        std::shared_ptr<org::Buffer> diceQueueCounter,
        std::shared_ptr<org::Buffer> diceQueueOverflowCounter,
        std::shared_ptr<org::Buffer> ownershipBitsetBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        uint32_t phaseIndex,
        bool clearDiceQueueCounter = true,
        std::shared_ptr<org::Buffer> replaySplitQueueCounter = nullptr,
        std::shared_ptr<org::Buffer> replaySplitQueueOverflowCounter = nullptr,
        std::shared_ptr<org::Buffer> replayDiceQueueCounter = nullptr,
        std::shared_ptr<org::Buffer> replayDiceQueueOverflowCounter = nullptr);

    ReyesQueueResetBindings Declare(org::PassBuilder& builder);
    void Initialize();
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputePipelineSequence Prepare(
        const ReyesQueueResetBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesQueueResetBindings&,
        const br::render::PreparedComputePipelineSequence&, org::PassRecordContext&);
    void Update(const org::UpdateExecutionContext& executionContext) override;

private:
    std::shared_ptr<org::Buffer> m_fullClusterCounter;
    std::shared_ptr<org::Buffer> m_ownedClusterCounter;
    std::vector<std::shared_ptr<org::Buffer>> m_splitQueueCounters;
    std::vector<std::shared_ptr<org::Buffer>> m_splitQueueOverflowCounters;
    std::shared_ptr<org::Buffer> m_diceQueueCounter;
    std::shared_ptr<org::Buffer> m_diceQueueOverflowCounter;
    std::shared_ptr<org::Buffer> m_ownershipBitsetBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    std::shared_ptr<org::Buffer> m_replaySplitQueueCounter;
    std::shared_ptr<org::Buffer> m_replaySplitQueueOverflowCounter;
    std::shared_ptr<org::Buffer> m_replayDiceQueueCounter;
    std::shared_ptr<org::Buffer> m_replayDiceQueueOverflowCounter;
    uint32_t m_phaseIndex = 0u;
    bool m_clearDiceQueueCounter = true;
    uint32_t m_ownershipBitsetWordCount = 0u;
    org::PipelineState m_clearCountersPso;
    org::PipelineState m_clearOwnershipBitsetPso;
};
