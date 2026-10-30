#include "VirtualGeometry/Reyes/RenderPasses/ReyesReplayMergePass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesReplayMergeRootConstants.h"

ReyesReplayMergePass::ReyesReplayMergePass(
    ReyesReplayMergeKind kind,
    std::shared_ptr<org::Buffer> sourceQueueBuffer,
    std::shared_ptr<org::Buffer> sourceQueueCounterBuffer,
    std::shared_ptr<org::Buffer> destQueueBuffer,
    std::shared_ptr<org::Buffer> destQueueCounterBuffer,
    std::shared_ptr<org::Buffer> destQueueOverflowBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    uint32_t destQueueCapacity)
    : m_kind(kind)
    , m_sourceQueueBuffer(std::move(sourceQueueBuffer))
    , m_sourceQueueCounterBuffer(std::move(sourceQueueCounterBuffer))
    , m_destQueueBuffer(std::move(destQueueBuffer))
    , m_destQueueCounterBuffer(std::move(destQueueCounterBuffer))
    , m_destQueueOverflowBuffer(std::move(destQueueOverflowBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_destQueueCapacity(destQueueCapacity)
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        kind == ReyesReplayMergeKind::Split ? L"MergeReyesReplaySplitQueueCSMain" : L"MergeReyesReplayDiceQueueCSMain",
        {},
        kind == ReyesReplayMergeKind::Split ? "CLod.ReyesReplayMergeSplit.PSO" : "CLod.ReyesReplayMergeDice.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    rhi::CommandSignaturePtr commandSignature;
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        commandSignature);
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(commandSignature));
}

ReyesReplayMergeBindings ReyesReplayMergePass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, m_commandSignature, builder.ShaderResource(m_sourceQueueBuffer), builder.ShaderResource(m_sourceQueueCounterBuffer),
        builder.UnorderedAccess(m_destQueueBuffer), builder.UnorderedAccess(m_destQueueCounterBuffer),
        builder.UnorderedAccess(m_destQueueOverflowBuffer), builder.IndirectArguments(m_indirectArgsBuffer),
        builder.UnorderedAccess(m_telemetryBuffer), m_destQueueCapacity};
}

void ReyesReplayMergePass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

void ReyesReplayMergePass::Record(const ReyesReplayMergeBindings& bindings, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    const auto srv = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    const auto uav = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    constants[CLOD_REYES_REPLAY_MERGE_SOURCE_DESCRIPTOR_INDEX] = srv(bindings.source);
    constants[CLOD_REYES_REPLAY_MERGE_SOURCE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.sourceCounter);
    constants[CLOD_REYES_REPLAY_MERGE_DEST_DESCRIPTOR_INDEX] = uav(bindings.dest);
    constants[CLOD_REYES_REPLAY_MERGE_DEST_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.destCounter);
    constants[CLOD_REYES_REPLAY_MERGE_DEST_OVERFLOW_DESCRIPTOR_INDEX] = uav(bindings.destOverflow);
    constants[CLOD_REYES_REPLAY_MERGE_CAPACITY] = bindings.capacity;
    constants[CLOD_REYES_REPLAY_MERGE_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    br::render::DispatchComputeIndirect(recording, bindings.pipeline, constants, bindings.commandSignature,
        bindings.indirectArgs);
}
