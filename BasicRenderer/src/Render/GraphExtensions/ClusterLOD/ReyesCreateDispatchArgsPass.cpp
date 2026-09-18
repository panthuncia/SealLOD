#include "Render/GraphExtensions/ClusterLOD/ReyesCreateDispatchArgsPass.h"
#include "Render/InvocationRevision.h"

#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodReyesCreateDispatchArgsRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"

ReyesCreateDispatchArgsPass::ReyesCreateDispatchArgsPass(
    std::shared_ptr<Buffer> sourceCounterBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> sourceBaseCounterBuffer,
    uint32_t threadsPerGroup,
    uint32_t maxWorkItemCount)
    : m_sourceCounterBuffer(std::move(sourceCounterBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_sourceBaseCounterBuffer(std::move(sourceBaseCounterBuffer))
    , m_threadsPerGroup(threadsPerGroup)
    , m_maxWorkItemCount(maxWorkItemCount)
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"BuildReyesDispatchArgsCSMain",
        {},
        "CLod.ReyesCreateDispatchArgs.PSO");
}

ReyesCreateDispatchArgsBindings ReyesCreateDispatchArgsPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    ReyesCreateDispatchArgsBindings bindings{builder.BindShaderResource(m_sourceCounterBuffer),
        builder.BindUnorderedAccess(m_indirectArgsBuffer)};
    if (m_sourceBaseCounterBuffer) {
        bindings.sourceBaseCounter = builder.BindShaderResource(m_sourceBaseCounterBuffer);
        bindings.hasSourceBaseCounter = true;
    }

    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    bindings.threadsPerGroup = m_threadsPerGroup;
    bindings.maxWorkItemCount = m_maxWorkItemCount;
    return bindings;
}

void ReyesCreateDispatchArgsPass::Initialize()
{
}



void ReyesCreateDispatchArgsPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

void ReyesCreateDispatchArgsPass::ShutdownPass()
{
}

br::render::PreparedComputeDispatch ReyesCreateDispatchArgsPass::Prepare(
    const ReyesCreateDispatchArgsBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    auto payload = m_pso.GetPayload();
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    data.constants[CLOD_REYES_CREATE_DISPATCH_ARGS_SOURCE_COUNTER_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.sourceCounter, {org::BindlessViewKind::ShaderResource}).index;
    data.constants[CLOD_REYES_CREATE_DISPATCH_ARGS_OUTPUT_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.indirectArgs, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_REYES_CREATE_DISPATCH_ARGS_THREADS_PER_GROUP] = bindings.threadsPerGroup;
    data.constants[CLOD_REYES_CREATE_DISPATCH_ARGS_SOURCE_BASE_COUNTER_DESCRIPTOR_INDEX] = bindings.hasSourceBaseCounter
        ? preparation.ResolveView(bindings.sourceBaseCounter, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
    data.constants[CLOD_REYES_CREATE_DISPATCH_ARGS_MAX_WORK_ITEM_COUNT] = bindings.maxWorkItemCount;
    data.groupsX = 1;
    return data;
}

void ReyesCreateDispatchArgsPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(static_cast<uint64_t>(br::render::HandleRevision(PSOManager::GetInstance().GetComputeRootSignature().GetHandle())));
}

void ReyesCreateDispatchArgsPass::Record(const ReyesCreateDispatchArgsBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
