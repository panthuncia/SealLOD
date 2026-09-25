#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"

namespace org { class Buffer; }

struct ReyesDiceBindings {
    org::ResourceBindingToken queue, counter, readOffset, tessConfigs, indirectArgs, telemetry;
    uint32_t capacity = 0, phase = 0;
    bool hasReadOffset = false;
};

class ReyesDicePass final : public org::TypedRenderGraphPass<ReyesDicePass,
    br::render::PreparedComputeIndirect, ReyesDiceBindings> {
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
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesDiceBindings&, const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesDiceBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);

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
