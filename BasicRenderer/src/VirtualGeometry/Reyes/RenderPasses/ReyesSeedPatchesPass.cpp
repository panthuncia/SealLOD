#include "VirtualGeometry/Reyes/RenderPasses/ReyesSeedPatchesPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesSeedRootConstants.h"
#include "Resources/Buffers/Buffer.h"

ReyesSeedPatchesPass::ReyesSeedPatchesPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> ownedClustersBuffer,
    std::shared_ptr<org::Buffer> ownedClustersCounterBuffer,
    std::shared_ptr<org::Buffer> splitQueueBuffer,
    std::shared_ptr<org::Buffer> splitQueueCounterBuffer,
    std::shared_ptr<org::Buffer> splitQueueOverflowBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::ResourceGroup> slabResourceGroup,
    uint32_t maxSplitQueueEntries,
    uint32_t phaseIndex)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_ownedClustersBuffer(std::move(ownedClustersBuffer))
    , m_ownedClustersCounterBuffer(std::move(ownedClustersCounterBuffer))
    , m_splitQueueBuffer(std::move(splitQueueBuffer))
    , m_splitQueueCounterBuffer(std::move(splitQueueCounterBuffer))
    , m_splitQueueOverflowBuffer(std::move(splitQueueOverflowBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_slabResourceGroup(std::move(slabResourceGroup))
    , m_maxSplitQueueEntries(maxSplitQueueEntries)
    , m_phaseIndex(phaseIndex) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesSeedPatches.hlsl",
        L"ReyesSeedPatchesCS",
        {},
        "CLod.ReyesSeedPatches.PSO");

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

ReyesSeedPatchesBindings ReyesSeedPatchesPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    if (m_slabResourceGroup) {
        builder.ShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    return {m_pso, m_commandSignature, builder.ShaderResource(m_visibleClustersBuffer), builder.ShaderResource(m_ownedClustersBuffer),
        builder.ShaderResource(m_ownedClustersCounterBuffer), builder.UnorderedAccess(m_splitQueueBuffer),
        builder.UnorderedAccess(m_splitQueueCounterBuffer), builder.UnorderedAccess(m_splitQueueOverflowBuffer),
        builder.IndirectArguments(m_indirectArgsBuffer), m_maxSplitQueueEntries, m_phaseIndex};
}

void ReyesSeedPatchesPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

void ReyesSeedPatchesPass::Record(const ReyesSeedPatchesBindings& bindings, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    const auto srv = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    const auto uav = [&](const org::DeclaredViewToken& token) { return recording.Index(token); };
    constants[CLOD_REYES_SEED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.visible);
    constants[CLOD_REYES_SEED_OWNED_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.owned);
    constants[CLOD_REYES_SEED_OWNED_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.ownedCounter);
    constants[CLOD_REYES_SEED_OUTPUT_SPLIT_QUEUE_DESCRIPTOR_INDEX] = uav(bindings.splitQueue);
    constants[CLOD_REYES_SEED_OUTPUT_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.splitCounter);
    constants[CLOD_REYES_SEED_OUTPUT_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = uav(bindings.splitOverflow);
    constants[CLOD_REYES_SEED_QUEUE_CAPACITY] = bindings.capacity;
    constants[CLOD_REYES_SEED_PHASE_INDEX] = bindings.phase;
    br::render::DispatchComputeIndirect(recording, bindings.pipeline, constants, bindings.commandSignature,
        bindings.indirectArgs);
}
