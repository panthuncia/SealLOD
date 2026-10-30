#include "VirtualGeometry/Reyes/RenderPasses/ReyesDicePass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesDiceRootConstants.h"
#include "Resources/Buffers/Buffer.h"

ReyesDicePass::ReyesDicePass(
    std::shared_ptr<org::Buffer> diceQueueBuffer,
    std::shared_ptr<org::Buffer> diceQueueCounterBuffer,
    std::shared_ptr<org::Buffer> diceQueueReadOffsetBuffer,
    std::shared_ptr<org::Buffer> tessTableConfigsBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
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
    ReyesDiceBindings bindings{m_pso, m_commandSignature, builder.ShaderResource(m_diceQueueBuffer),
        builder.ShaderResource(m_diceQueueCounterBuffer)};
    if (m_diceQueueReadOffsetBuffer) {
        bindings.readOffset = builder.ShaderResource(m_diceQueueReadOffsetBuffer);
        bindings.hasReadOffset = true;
    }
    bindings.tessConfigs = builder.ShaderResource(m_tessTableConfigsBuffer);
    bindings.indirectArgs = builder.IndirectArguments(m_indirectArgsBuffer);
    bindings.telemetry = builder.UnorderedAccess(m_telemetryBuffer);
    bindings.capacity = m_maxDiceQueueEntries;
    bindings.phase = m_phaseIndex;
    return bindings;
}

void ReyesDicePass::Record(const ReyesDiceBindings& bindings, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    const auto srv = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    constants[CLOD_REYES_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX] = bindings.hasReadOffset ? srv(bindings.readOffset) : 0xFFFFFFFFu;
    constants[CLOD_REYES_DICE_QUEUE_DESCRIPTOR_INDEX] = srv(bindings.queue);
    constants[CLOD_REYES_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.counter);
    constants[CLOD_REYES_DICE_TELEMETRY_DESCRIPTOR_INDEX] = recording.Index(bindings.telemetry);
    constants[CLOD_REYES_DICE_PHASE_INDEX] = bindings.phase;
    constants[CLOD_REYES_DICE_QUEUE_CAPACITY] = bindings.capacity;
    constants[CLOD_REYES_DICE_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX] = srv(bindings.tessConfigs);
    br::render::DispatchComputeIndirect(recording, bindings.pipeline, constants, bindings.commandSignature,
        bindings.indirectArgs);
}
