#include "Render/GraphExtensions/ClusterLOD/ReyesDicePass.h"
#include "Render/InvocationRevision.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesDiceRootConstants.h"
#include "Resources/Buffers/Buffer.h"
#include "RenderPasses/PreparedComputeDispatch.h"

ReyesDicePass::ReyesDicePass(
    std::shared_ptr<Buffer> diceQueueBuffer,
    std::shared_ptr<Buffer> diceQueueCounterBuffer,
    std::shared_ptr<Buffer> diceQueueReadOffsetBuffer,
    std::shared_ptr<Buffer> tessTableConfigsBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    uint32_t maxDiceQueueEntries,
    uint32_t phaseIndex)
    : m_diceQueueBuffer(std::move(diceQueueBuffer))
    , m_diceQueueCounterBuffer(std::move(diceQueueCounterBuffer))
    , m_diceQueueReadOffsetBuffer(std::move(diceQueueReadOffsetBuffer))
    , m_tessTableConfigsBuffer(std::move(tessTableConfigsBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_maxDiceQueueEntries(maxDiceQueueEntries)
    , m_phaseIndex(phaseIndex) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesDice.hlsl",
        L"ReyesDiceCS",
        {},
        "CLod.ReyesDice.PSO");

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

ReyesDiceBindings ReyesDicePass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    ReyesDiceBindings bindings{builder.BindShaderResource(m_diceQueueBuffer),
        builder.BindShaderResource(m_diceQueueCounterBuffer)};
    if (m_diceQueueReadOffsetBuffer) {
        bindings.readOffset = builder.BindShaderResource(m_diceQueueReadOffsetBuffer);
        bindings.hasReadOffset = true;
    }
    bindings.tessConfigs = builder.BindShaderResource(m_tessTableConfigsBuffer);
    bindings.indirectArgs = builder.BindIndirectArguments(m_indirectArgsBuffer);
    bindings.telemetry = builder.BindUnorderedAccess(m_telemetryBuffer);
    bindings.capacity = m_maxDiceQueueEntries;
    bindings.phase = m_phaseIndex;
    return bindings;
}

br::render::PreparedComputeIndirect ReyesDicePass::Prepare(
    const ReyesDiceBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputeIndirect data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    data.constants[CLOD_REYES_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX] = bindings.hasReadOffset ? srv(bindings.readOffset) : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.queue);
    data.constants[CLOD_REYES_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.counter);
    data.constants[CLOD_REYES_DICE_TELEMETRY_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.telemetry, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_REYES_DICE_PHASE_INDEX] = bindings.phase; data.constants[CLOD_REYES_DICE_QUEUE_CAPACITY] = bindings.capacity;
    data.constants[CLOD_REYES_DICE_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    return data;
}

void ReyesDicePass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
}

void ReyesDicePass::Record(const ReyesDiceBindings&, const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
