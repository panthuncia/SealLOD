#pragma once

#include <memory>

#include <rhi.h>

#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

namespace org { class Buffer; }

enum class ReyesReplayMergeKind
{
    Split,
    Dice
};

struct ReyesReplayMergeBindings {
    org::PipelineState pipeline;
    std::shared_ptr<const rhi::CommandSignaturePtr> commandSignature;
    org::DeclaredViewToken source;
    org::DeclaredViewToken sourceCounter;
    org::DeclaredViewToken dest;
    org::DeclaredViewToken destCounter;
    org::DeclaredViewToken destOverflow;
    org::ResourceBindingToken indirectArgs;
    org::DeclaredViewToken telemetry;
    uint32_t capacity = 0;
};

class ReyesReplayMergePass final : public org::TypedRenderGraphPass<ReyesReplayMergePass,
    org::EmptyPassFrameData, ReyesReplayMergeBindings> {
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
    static void Record(const ReyesReplayMergeBindings& bindings, org::PassRecordContext& recording);

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
