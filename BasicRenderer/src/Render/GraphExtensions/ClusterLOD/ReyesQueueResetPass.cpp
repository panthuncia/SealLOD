#include "Render/GraphExtensions/ClusterLOD/ReyesQueueResetPass.h"

#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodReyesResetRootConstants.h"

ReyesQueueResetPass::ReyesQueueResetPass(
    std::shared_ptr<Buffer> fullClusterCounter,
    std::shared_ptr<Buffer> ownedClusterCounter,
    std::vector<std::shared_ptr<Buffer>> splitQueueCounters,
    std::vector<std::shared_ptr<Buffer>> splitQueueOverflowCounters,
    std::shared_ptr<Buffer> diceQueueCounter,
    std::shared_ptr<Buffer> diceQueueOverflowCounter,
    std::shared_ptr<Buffer> ownershipBitsetBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
    uint32_t phaseIndex,
    bool clearDiceQueueCounter,
    std::shared_ptr<Buffer> replaySplitQueueCounter,
    std::shared_ptr<Buffer> replaySplitQueueOverflowCounter,
    std::shared_ptr<Buffer> replayDiceQueueCounter,
    std::shared_ptr<Buffer> replayDiceQueueOverflowCounter)
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

void ReyesQueueResetPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithUnorderedAccess(m_fullClusterCounter, m_ownedClusterCounter, m_diceQueueCounter, m_diceQueueOverflowCounter, m_telemetryBuffer);
    if (m_replaySplitQueueCounter) {
        builder->WithUnorderedAccess(m_replaySplitQueueCounter);
    }
    if (m_replaySplitQueueOverflowCounter) {
        builder->WithUnorderedAccess(m_replaySplitQueueOverflowCounter);
    }
    if (m_replayDiceQueueCounter) {
        builder->WithUnorderedAccess(m_replayDiceQueueCounter);
    }
    if (m_replayDiceQueueOverflowCounter) {
        builder->WithUnorderedAccess(m_replayDiceQueueOverflowCounter);
    }
    if (m_ownershipBitsetBuffer) {
        builder->WithUnorderedAccess(m_ownershipBitsetBuffer);
    }
    for (const auto& splitQueueCounter : m_splitQueueCounters) {
        builder->WithUnorderedAccess(splitQueueCounter);
    }
    for (const auto& splitQueueOverflowCounter : m_splitQueueOverflowCounters) {
        builder->WithUnorderedAccess(splitQueueOverflowCounter);
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void ReyesQueueResetPass::Setup() {}

PassReturn ReyesQueueResetPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

    uint32_t uintRootConstants[NumMiscUintRootConstants] = {};
    uintRootConstants[CLOD_REYES_RESET_FULL_CLUSTER_COUNTER_DESCRIPTOR_INDEX] = m_fullClusterCounter->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_OWNED_CLUSTER_COUNTER_DESCRIPTOR_INDEX] = m_ownedClusterCounter->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_A_DESCRIPTOR_INDEX] = m_splitQueueCounters[0]->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_A_DESCRIPTOR_INDEX] = m_splitQueueOverflowCounters[0]->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_B_DESCRIPTOR_INDEX] = m_splitQueueCounters[1]->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_B_DESCRIPTOR_INDEX] = m_splitQueueOverflowCounters[1]->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_diceQueueCounter->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = m_diceQueueOverflowCounter->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_CLEAR_DICE_QUEUE_COUNTER] = m_clearDiceQueueCounter ? 1u : 0u;
    uintRootConstants[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_replaySplitQueueCounter
        ? m_replaySplitQueueCounter->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = m_replaySplitQueueOverflowCounter
        ? m_replaySplitQueueOverflowCounter->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX] = m_replayDiceQueueCounter
        ? m_replayDiceQueueCounter->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;
    uintRootConstants[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = m_replayDiceQueueOverflowCounter
        ? m_replayDiceQueueOverflowCounter->GetUAVShaderVisibleInfo(0).slot.index
        : 0xFFFFFFFFu;

    commandList.BindPipeline(m_clearCountersPso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_clearCountersPso.GetResourceDescriptorSlots());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);
    commandList.Dispatch(1, 1, 1);

    if (!m_ownershipBitsetBuffer || m_ownershipBitsetWordCount == 0u) {
        return {};
    }

    commandList.BindPipeline(m_clearOwnershipBitsetPso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_clearOwnershipBitsetPso.GetResourceDescriptorSlots());

    uintRootConstants[CLOD_REYES_RESET_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = m_ownershipBitsetBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    uintRootConstants[CLOD_REYES_RESET_OWNERSHIP_BITSET_WORD_COUNT] = m_ownershipBitsetWordCount;
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        uintRootConstants);

    constexpr uint32_t kThreadsPerGroup = 64u;
    const uint32_t groupCountX = (m_ownershipBitsetWordCount + kThreadsPerGroup - 1u) / kThreadsPerGroup;
    commandList.Dispatch(groupCountX, 1, 1);

    return {};
}

void ReyesQueueResetPass::Update(const UpdateExecutionContext& executionContext)
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
    BUFFER_UPLOAD(&telemetry, sizeof(CLodReyesTelemetry), rg::runtime::UploadTarget::FromShared(m_telemetryBuffer), 0);
}

void ReyesQueueResetPass::Cleanup() {}
