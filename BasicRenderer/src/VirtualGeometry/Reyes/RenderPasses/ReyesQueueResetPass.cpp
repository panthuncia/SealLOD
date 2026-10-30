#include "VirtualGeometry/Reyes/RenderPasses/ReyesQueueResetPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
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
        m_clearCountersPso,
        m_clearOwnershipBitsetPso,
        builder->UnorderedAccess(m_fullClusterCounter),
        builder->UnorderedAccess(m_ownedClusterCounter),
        {}, {},
        builder->UnorderedAccess(m_diceQueueCounter),
        builder->UnorderedAccess(m_diceQueueOverflowCounter),
        builder->UnorderedAccess(m_telemetryBuffer)};
    if (m_replaySplitQueueCounter) {
        bindings.replaySplitQueueCounter = builder->UnorderedAccess(m_replaySplitQueueCounter);
    }
    if (m_replaySplitQueueOverflowCounter) {
        bindings.replaySplitQueueOverflowCounter = builder->UnorderedAccess(m_replaySplitQueueOverflowCounter);
    }
    if (m_replayDiceQueueCounter) {
        bindings.replayDiceQueueCounter = builder->UnorderedAccess(m_replayDiceQueueCounter);
    }
    if (m_replayDiceQueueOverflowCounter) {
        bindings.replayDiceQueueOverflowCounter = builder->UnorderedAccess(m_replayDiceQueueOverflowCounter);
    }
    if (m_ownershipBitsetBuffer) {
        bindings.ownershipBitset = builder->UnorderedAccess(m_ownershipBitsetBuffer);
    }
    for (const auto& splitQueueCounter : m_splitQueueCounters) {
        bindings.splitQueueCounters.push_back(builder->UnorderedAccess(splitQueueCounter));
    }
    for (const auto& splitQueueOverflowCounter : m_splitQueueOverflowCounters) {
        bindings.splitQueueOverflowCounters.push_back(builder->UnorderedAccess(splitQueueOverflowCounter));
    }

    bindings.ownershipBitsetWordCount = m_ownershipBitsetWordCount;
    bindings.clearDiceQueueCounter = m_clearDiceQueueCounter;

    builder->ConstantBuffer(Builtin::PerFrameBuffer);
    return bindings;
}

void ReyesQueueResetPass::Initialize() {}

void ReyesQueueResetPass::Record(const ReyesQueueResetBindings& bindings, org::PassRecordContext& recording)
{
    // Both dispatches share one constant block; the counter clear sees the bitset fields too.
    br::render::MiscConstants c{};
    const auto uavIndex = [&](const org::DeclaredViewToken& token) {
        return recording.Index(token);
    };
    c[CLOD_REYES_RESET_FULL_CLUSTER_COUNTER_DESCRIPTOR_INDEX] = uavIndex(bindings.fullClusterCounter);
    c[CLOD_REYES_RESET_OWNED_CLUSTER_COUNTER_DESCRIPTOR_INDEX] = uavIndex(bindings.ownedClusterCounter);
    c[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_A_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueCounters.at(0));
    c[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_A_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueOverflowCounters.at(0));
    c[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_B_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueCounters.at(1));
    c[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_B_DESCRIPTOR_INDEX] = uavIndex(bindings.splitQueueOverflowCounters.at(1));
    c[CLOD_REYES_RESET_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = uavIndex(bindings.diceQueueCounter);
    c[CLOD_REYES_RESET_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = uavIndex(bindings.diceQueueOverflowCounter);
    c[CLOD_REYES_RESET_CLEAR_DICE_QUEUE_COUNTER] = bindings.clearDiceQueueCounter ? 1u : 0u;
    c[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = bindings.replaySplitQueueCounter ? uavIndex(*bindings.replaySplitQueueCounter) : 0xFFFFFFFFu;
    c[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = bindings.replaySplitQueueOverflowCounter ? uavIndex(*bindings.replaySplitQueueOverflowCounter) : 0xFFFFFFFFu;
    c[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = bindings.replayDiceQueueCounter ? uavIndex(*bindings.replayDiceQueueCounter) : 0xFFFFFFFFu;
    c[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = bindings.replayDiceQueueOverflowCounter ? uavIndex(*bindings.replayDiceQueueOverflowCounter) : 0xFFFFFFFFu;
    const bool clearBitset = bindings.ownershipBitset && bindings.ownershipBitsetWordCount;
    if (clearBitset) {
        c[CLOD_REYES_RESET_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = uavIndex(*bindings.ownershipBitset);
        c[CLOD_REYES_RESET_OWNERSHIP_BITSET_WORD_COUNT] = bindings.ownershipBitsetWordCount;
    }
    br::render::DispatchCompute(recording, bindings.clearCountersPipeline, c, 1);
    if (clearBitset) {
        br::render::DispatchCompute(recording, bindings.clearOwnershipBitsetPipeline, c,
            (bindings.ownershipBitsetWordCount + 63u) / 64u);
    }
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
