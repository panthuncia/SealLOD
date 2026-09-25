#include "Render/GraphExtensions/ClusterLOD/ReyesQueueResetPass.h"
#include "Render/InvocationRevision.h"

#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodReyesResetRootConstants.h"

ReyesQueueResetPass::ReyesQueueResetPass(
    std::shared_ptr<org::Buffer> fullClusterCounter,
    std::shared_ptr<org::Buffer> ownedClusterCounter,
    std::vector<std::shared_ptr<org::Buffer>> splitQueueCounters,
    std::vector<std::shared_ptr<org::Buffer>> splitQueueOverflowCounters,
    std::shared_ptr<org::Buffer> diceQueueCounter,
    std::shared_ptr<org::Buffer> diceQueueOverflowCounter,
    std::shared_ptr<org::Buffer> ownershipBitsetBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    uint32_t phaseIndex,
    bool clearDiceQueueCounter,
    std::shared_ptr<org::Buffer> replaySplitQueueCounter,
    std::shared_ptr<org::Buffer> replaySplitQueueOverflowCounter,
    std::shared_ptr<org::Buffer> replayDiceQueueCounter,
    std::shared_ptr<org::Buffer> replayDiceQueueOverflowCounter)
    : m_fullClusterCounter(std::move(fullClusterCounter))
    , m_ownedClusterCounter(std::move(ownedClusterCounter))
    , m_splitQueueCounters(std::move(splitQueueCounters))
    , m_splitQueueOverflowCounters(std::move(splitQueueOverflowCounters))
    , m_diceQueueCounter(std::move(diceQueueCounter))
    , m_diceQueueOverflowCounter(std::move(diceQueueOverflowCounter))
    , m_ownershipBitsetBuffer(std::move(ownershipBitsetBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_replaySplitQueueCounter(std::move(replaySplitQueueCounter))
    , m_replaySplitQueueOverflowCounter(std::move(replaySplitQueueOverflowCounter))
    , m_replayDiceQueueCounter(std::move(replayDiceQueueCounter))
    , m_replayDiceQueueOverflowCounter(std::move(replayDiceQueueOverflowCounter))
    , m_phaseIndex(phaseIndex) {
    m_clearDiceQueueCounter = clearDiceQueueCounter;
    m_clearCountersPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearReyesQueueCountersCSMain",
        {},
        "CLod.ReyesQueueReset.ClearCounters.PSO");

    if (m_ownershipBitsetBuffer) {
        m_ownershipBitsetWordCount = static_cast<uint32_t>(m_ownershipBitsetBuffer->GetSize() / sizeof(uint32_t));
        m_clearOwnershipBitsetPso = PSOManager::GetInstance().MakeComputePipeline(
            PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
            L"Shaders/ClusterLOD/clodUtil.hlsl",
            L"ClearReyesOwnershipBitsetCSMain",
            {},
            "CLod.ReyesQueueReset.ClearOwnershipBitset.PSO");
    }
}

ReyesQueueResetBindings ReyesQueueResetPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    ReyesQueueResetBindings bindings{
        builder->BindUnorderedAccess(m_fullClusterCounter),
        builder->BindUnorderedAccess(m_ownedClusterCounter),
        {}, {},
        builder->BindUnorderedAccess(m_diceQueueCounter),
        builder->BindUnorderedAccess(m_diceQueueOverflowCounter),
        builder->BindUnorderedAccess(m_telemetryBuffer)};
    if (m_replaySplitQueueCounter) {
        bindings.replaySplitQueueCounter = builder->BindUnorderedAccess(m_replaySplitQueueCounter);
    }
    if (m_replaySplitQueueOverflowCounter) {
        bindings.replaySplitQueueOverflowCounter = builder->BindUnorderedAccess(m_replaySplitQueueOverflowCounter);
    }
    if (m_replayDiceQueueCounter) {
        bindings.replayDiceQueueCounter = builder->BindUnorderedAccess(m_replayDiceQueueCounter);
    }
    if (m_replayDiceQueueOverflowCounter) {
        bindings.replayDiceQueueOverflowCounter = builder->BindUnorderedAccess(m_replayDiceQueueOverflowCounter);
    }
    if (m_ownershipBitsetBuffer) {
        bindings.ownershipBitset = builder->BindUnorderedAccess(m_ownershipBitsetBuffer);
    }
    for (const auto& splitQueueCounter : m_splitQueueCounters) {
        bindings.splitQueueCounters.push_back(builder->BindUnorderedAccess(splitQueueCounter));
    }
    for (const auto& splitQueueOverflowCounter : m_splitQueueOverflowCounters) {
        bindings.splitQueueOverflowCounters.push_back(builder->BindUnorderedAccess(splitQueueOverflowCounter));
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

void ReyesQueueResetPass::Initialize() {}

br::render::PreparedComputePipelineSequence ReyesQueueResetPass::Prepare(
    const ReyesQueueResetBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputePipelineSequence data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.steps.reserve(2);
    auto& counters = data.steps.emplace_back();
    auto program = preparation.CaptureProgramBinding(m_clearCountersPso);
    counters.program = program.program;
    counters.descriptorIndices = std::move(program.descriptorIndices);
    counters.groupsX = 1;
    auto& c = counters.constants;
    const auto uavIndex = [&](org::ResourceBindingToken token) {
        return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index;
    };
    c[CLOD_REYES_RESET_FULL_CLUSTER_COUNTER_DESCRIPTOR_INDEX] = uavIndex(bindings.fullClusterCounter);
    c[CLOD_REYES_RESET_OWNED_CLUSTER_COUNTER_DESCRIPTOR_INDEX] = uavIndex(bindings.ownedClusterCounter);
    c[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_A_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueCounters.at(0));
    c[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_A_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueOverflowCounters.at(0));
    c[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_B_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueCounters.at(1));
    c[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_B_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueOverflowCounters.at(1));
    c[CLOD_REYES_RESET_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = uavIndex(bindings.diceQueueCounter);
    c[CLOD_REYES_RESET_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = uavIndex(bindings.diceQueueOverflowCounter);
    c[CLOD_REYES_RESET_CLEAR_DICE_QUEUE_COUNTER] = m_clearDiceQueueCounter ? 1u : 0u;
    c[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = bindings.replaySplitQueueCounter ? uavIndex(*bindings.replaySplitQueueCounter) : 0xFFFFFFFFu;
    c[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = bindings.replaySplitQueueOverflowCounter ? uavIndex(*bindings.replaySplitQueueOverflowCounter) : 0xFFFFFFFFu;
    c[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = bindings.replayDiceQueueCounter ? uavIndex(*bindings.replayDiceQueueCounter) : 0xFFFFFFFFu;
    c[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = bindings.replayDiceQueueOverflowCounter ? uavIndex(*bindings.replayDiceQueueOverflowCounter) : 0xFFFFFFFFu;
    if (bindings.ownershipBitset && m_ownershipBitsetWordCount) {
        br::render::PreparedComputePipelineSequence::Step bitset{};
        auto bitsetProgram = preparation.CaptureProgramBinding(m_clearOwnershipBitsetPso);
        bitset.program = bitsetProgram.program;
        bitset.descriptorIndices = std::move(bitsetProgram.descriptorIndices);
        c[CLOD_REYES_RESET_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = uavIndex(*bindings.ownershipBitset);
        c[CLOD_REYES_RESET_OWNERSHIP_BITSET_WORD_COUNT] = m_ownershipBitsetWordCount;
        bitset.constants = c;
        bitset.groupsX = (m_ownershipBitsetWordCount + 63u) / 64u;
        data.steps.push_back(std::move(bitset));
    }
    return data;
}

void ReyesQueueResetPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_clearCountersPso));
    out.push_back(br::render::PipelineRevision(m_clearOwnershipBitsetPso));
    out.push_back(static_cast<uint64_t>(m_clearDiceQueueCounter));
    out.push_back(static_cast<uint64_t>(m_ownershipBitsetWordCount));
}

void ReyesQueueResetPass::Record(const ReyesQueueResetBindings&,
    const br::render::PreparedComputePipelineSequence& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputePipelineSequence(data, recording);
}

void ReyesQueueResetPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    CLodReyesTelemetry telemetry{};
    telemetry.phaseIndex = m_phaseIndex;
    telemetry.configuredMaxSplitPassCount = CLodReyesMaxSplitPassCount;
    telemetry.objectReyesAtlasDebugMinMaterialSlot = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinHeightDescriptor = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinSamplerDescriptor = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinHeightValueU16 = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinPatchHeightValueU16 = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinPatchUvXU16 = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinPatchUvYU16 = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinPageUvSetCount = 0xFFFFFFFFu;
    telemetry.objectReyesAtlasDebugMinHeightUvSetIndex = 0xFFFFFFFFu;
    UploadBufferData(&telemetry, sizeof(CLodReyesTelemetry), org::runtime::UploadTarget::FromShared(m_telemetryBuffer), 0);
}
