#include "Render/GraphExtensions/ClusterLOD/ReyesReplayMergePass.h"
#include "Render/InvocationRevision.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "BuiltinResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesReplayMergeRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"

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
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.BindShaderResource(m_sourceQueueBuffer), builder.BindShaderResource(m_sourceQueueCounterBuffer),
        builder.BindUnorderedAccess(m_destQueueBuffer), builder.BindUnorderedAccess(m_destQueueCounterBuffer),
        builder.BindUnorderedAccess(m_destQueueOverflowBuffer), builder.BindIndirectArguments(m_indirectArgsBuffer),
        builder.BindUnorderedAccess(m_telemetryBuffer), m_destQueueCapacity};
}

void ReyesReplayMergePass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

br::render::PreparedComputeIndirect ReyesReplayMergePass::Prepare(
    const ReyesReplayMergeBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputeIndirect data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    data.constants[CLOD_REYES_REPLAY_MERGE_SOURCE_DESCRIPTOR_INDEX] = srv(bindings.source);
    data.constants[CLOD_REYES_REPLAY_MERGE_SOURCE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.sourceCounter);
    data.constants[CLOD_REYES_REPLAY_MERGE_DEST_DESCRIPTOR_INDEX] = uav(bindings.dest);
    data.constants[CLOD_REYES_REPLAY_MERGE_DEST_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.destCounter);
    data.constants[CLOD_REYES_REPLAY_MERGE_DEST_OVERFLOW_DESCRIPTOR_INDEX] = uav(bindings.destOverflow);
    data.constants[CLOD_REYES_REPLAY_MERGE_CAPACITY] = bindings.capacity;
    data.constants[CLOD_REYES_REPLAY_MERGE_TELEMETRY_DESCRIPTOR_INDEX] = uav(bindings.telemetry);
    return data;
}

void ReyesReplayMergePass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
}

void ReyesReplayMergePass::Record(const ReyesReplayMergeBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
