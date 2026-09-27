#include "VirtualGeometry/Reyes/RenderPasses/ReyesSeedPatchesPass.h"
#include "Runtime/StateGraph/InvocationRevision.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodReyesSeedRootConstants.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

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
        builder.WithShaderResource(ResourceGroupResolver(m_slabResourceGroup));
    }
    return {builder.BindShaderResource(m_visibleClustersBuffer), builder.BindShaderResource(m_ownedClustersBuffer),
        builder.BindShaderResource(m_ownedClustersCounterBuffer), builder.BindUnorderedAccess(m_splitQueueBuffer),
        builder.BindUnorderedAccess(m_splitQueueCounterBuffer), builder.BindUnorderedAccess(m_splitQueueOverflowBuffer),
        builder.BindIndirectArguments(m_indirectArgsBuffer), m_maxSplitQueueEntries, m_phaseIndex};
}

br::render::PreparedComputeIndirect ReyesSeedPatchesPass::Prepare(
    const ReyesSeedPatchesBindings& bindings, const org::PassPrepareContext& preparation) const {
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
    data.constants[CLOD_REYES_SEED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.visible);
    data.constants[CLOD_REYES_SEED_OWNED_CLUSTERS_DESCRIPTOR_INDEX] = srv(bindings.owned);
    data.constants[CLOD_REYES_SEED_OWNED_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.ownedCounter);
    data.constants[CLOD_REYES_SEED_OUTPUT_SPLIT_QUEUE_DESCRIPTOR_INDEX] = uav(bindings.splitQueue);
    data.constants[CLOD_REYES_SEED_OUTPUT_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX] = uav(bindings.splitCounter);
    data.constants[CLOD_REYES_SEED_OUTPUT_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX] = uav(bindings.splitOverflow);
    data.constants[CLOD_REYES_SEED_QUEUE_CAPACITY] = bindings.capacity; data.constants[CLOD_REYES_SEED_PHASE_INDEX] = bindings.phase;
    return data;
}

void ReyesSeedPatchesPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

void ReyesSeedPatchesPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::OwnerRevision(m_commandSignature));
}

void ReyesSeedPatchesPass::Record(const ReyesSeedPatchesBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
