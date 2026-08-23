#include "Render/GraphExtensions/ClusterLOD/ReyesReplayMergePass.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "BuiltinResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesReplayMergeRootConstants.h"

ReyesReplayMergePass::ReyesReplayMergePass(
    ReyesReplayMergeKind kind,
    std::shared_ptr<Buffer> sourceQueueBuffer,
    std::shared_ptr<Buffer> sourceQueueCounterBuffer,
    std::shared_ptr<Buffer> destQueueBuffer,
    std::shared_ptr<Buffer> destQueueCounterBuffer,
    std::shared_ptr<Buffer> destQueueOverflowBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
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
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void ReyesReplayMergePass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(m_sourceQueueBuffer, m_sourceQueueCounterBuffer)
        .WithUnorderedAccess(m_destQueueBuffer, m_destQueueCounterBuffer, m_destQueueOverflowBuffer, m_telemetryBuffer)
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithConstantBuffer(Builtin::PerFrameBuffer);
}

void ReyesReplayMergePass::Setup() {}

void ReyesReplayMergePass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

PassReturn ReyesReplayMergePass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_REPLAY_MERGE_SOURCE_DESCRIPTOR_INDEX] = m_sourceQueueBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_REPLAY_MERGE_SOURCE_COUNTER_DESCRIPTOR_INDEX] = m_sourceQueueCounterBuffer->GetSRVInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_REPLAY_MERGE_DEST_DESCRIPTOR_INDEX] = m_destQueueBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_REPLAY_MERGE_DEST_COUNTER_DESCRIPTOR_INDEX] = m_destQueueCounterBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_REPLAY_MERGE_DEST_OVERFLOW_DESCRIPTOR_INDEX] = m_destQueueOverflowBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_REPLAY_MERGE_CAPACITY] = m_destQueueCapacity;
    uintRootConstants[CLOD_REYES_REPLAY_MERGE_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);

    commandList.ExecuteIndirect(m_commandSignature->GetHandle(), m_indirectArgsBuffer->GetAPIResource().GetHandle(), 0, {}, 0, 1);
    return {};
}

void ReyesReplayMergePass::Cleanup() {}
