#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

namespace org { class Buffer; }

enum class ReyesReplayMergeKind
{
    Split,
    Dice
};

struct ReyesReplayMergeBindings {
    org::ResourceBindingToken source, sourceCounter, dest, destCounter, destOverflow, indirectArgs, telemetry;
    uint32_t capacity = 0;
};

class ReyesReplayMergePass final : public org::TypedRenderGraphPass<ReyesReplayMergePass,
    br::render::PreparedComputeIndirect, ReyesReplayMergeBindings> {
public:
    ReyesReplayMergePass(
        ReyesReplayMergeKind kind,
        std::shared_ptr<org::Buffer> sourceQueueBuffer,
        std::shared_ptr<org::Buffer> sourceQueueCounterBuffer,
        std::shared_ptr<org::Buffer> destQueueBuffer,
        std::shared_ptr<org::Buffer> destQueueCounterBuffer,
        std::shared_ptr<org::Buffer> destQueueOverflowBuffer,
        std::shared_ptr<org::Buffer> indirectArgsBuffer,
        std::shared_ptr<org::Buffer> telemetryBuffer,
        uint32_t destQueueCapacity);

    ReyesReplayMergeBindings Declare(org::PassBuilder& builder);
    void Update(const org::UpdateExecutionContext& executionContext) override;
    void InvocationRevision(const org::PassPrepareContext&, std::vector<uint64_t>&) const;
    br::render::PreparedComputeIndirect Prepare(const ReyesReplayMergeBindings&,
        const org::PassPrepareContext& preparation) const;
    static void Record(const ReyesReplayMergeBindings&, const br::render::PreparedComputeIndirect&, org::PassRecordContext&);

private:
    ReyesReplayMergeKind m_kind = ReyesReplayMergeKind::Split;
    std::shared_ptr<org::Buffer> m_sourceQueueBuffer;
    std::shared_ptr<org::Buffer> m_sourceQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_destQueueBuffer;
    std::shared_ptr<org::Buffer> m_destQueueCounterBuffer;
    std::shared_ptr<org::Buffer> m_destQueueOverflowBuffer;
    std::shared_ptr<org::Buffer> m_indirectArgsBuffer;
    std::shared_ptr<org::Buffer> m_telemetryBuffer;
    uint32_t m_destQueueCapacity = 0u;
    org::PipelineState m_pso;
    std::shared_ptr<rhi::CommandSignaturePtr> m_commandSignature;
};
