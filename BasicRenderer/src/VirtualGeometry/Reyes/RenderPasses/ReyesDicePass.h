#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

struct ReyesDiceBindings {
    org::PipelineState pipeline;
    std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
    org::DeclaredViewToken queue;
    org::DeclaredViewToken counter;
    org::DeclaredViewToken readOffset;
    org::DeclaredViewToken tessConfigs;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken telemetry;
    uint32_t capacity = 0, phase = 0;
    bool hasReadOffset = false;
};

class ReyesDicePass final : public org::TypedRenderGraphPass<ReyesDicePass,
    org::EmptyPassFrameData, ReyesDiceBindings> {
public:
    ReyesDicePass(
        std::shared_ptr<org::Buffer> diceQueueBuffer,
        std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
        std::shared_ptr<org::Buffer> diceQueueReadOffsetBuffer,
        std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        uint32_t maxDiceQueueEntries,
        uint32_t phaseIndex);

    ReyesDiceBindings Declare(org::PassBuilder& builder);
    static void Record(const ReyesDiceBindings& bindings, org::PassRecordContext& recording);

private:
    std::shared_ptr<org::Buffer> m_diceQueueBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_diceQueueReadOffsetBuffer;
    std::shared_ptr<org::Buffer> m_tessTableConfigsBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    uint32_t m_maxDiceQueueEntries = 0u;
    uint32_t m_phaseIndex = 0u;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
