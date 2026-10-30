#include "VirtualGeometry/Reyes/RenderPasses/ReyesCreateDispatchArgsPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodReyesCreateDispatchArgsRootConstants.h"

ReyesCreateDispatchArgsPass::ReyesCreateDispatchArgsPass(
    std::shared_ptr<org::Buffer> sourceCounterBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> sourceBaseCounterBuffer,
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
    ReyesCreateDispatchArgsBindings bindings{m_pso, builder.ShaderResource(m_sourceCounterBuffer),
        builder.UnorderedAccess(m_indirectArgsBuffer)};
    if (m_sourceBaseCounterBuffer) {
        bindings.sourceBaseCounter = builder.ShaderResource(m_sourceBaseCounterBuffer);
        bindings.hasSourceBaseCounter = true;
    }

    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    bindings.threadsPerGroup = m_threadsPerGroup;
    bindings.maxWorkItemCount = m_maxWorkItemCount;
    return bindings;
}

void ReyesCreateDispatchArgsPass::Initialize()
{
}



void ReyesCreateDispatchArgsPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
}

void ReyesCreateDispatchArgsPass::ShutdownPass()
{
}

void ReyesCreateDispatchArgsPass::Record(const ReyesCreateDispatchArgsBindings& bindings,
    org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_REYES_CREATE_DISPATCH_ARGS_SOURCE_COUNTER_DESCRIPTOR_INDEX] = recording.Index(bindings.sourceCounter);
    constants[CLOD_REYES_CREATE_DISPATCH_ARGS_OUTPUT_DESCRIPTOR_INDEX] = recording.Index(bindings.indirectArgs);
    constants[CLOD_REYES_CREATE_DISPATCH_ARGS_THREADS_PER_GROUP] = bindings.threadsPerGroup;
    constants[CLOD_REYES_CREATE_DISPATCH_ARGS_SOURCE_BASE_COUNTER_DESCRIPTOR_INDEX] = bindings.hasSourceBaseCounter
        ? recording.Index(bindings.sourceBaseCounter) : 0xFFFFFFFFu;
    constants[CLOD_REYES_CREATE_DISPATCH_ARGS_MAX_WORK_ITEM_COUNT] = bindings.maxWorkItemCount;
    br::render::DispatchCompute(recording, bindings.pipeline, constants, 1);
}
