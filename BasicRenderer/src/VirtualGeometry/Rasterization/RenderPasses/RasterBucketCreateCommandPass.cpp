#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketCreateCommandPass.h"

#include "Materials/MaterialManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

RasterBucketCreateCommandPass::RasterBucketCreateCommandPass(
    std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> histogramIndirectCommand,
    std::shared_ptr<org::Buffer> occlusionReplayStateBuffer,
    std::shared_ptr<org::Buffer> occlusionNodeGpuInputsBuffer,
    uint32_t visibleClustersCapacity,
    bool runWhenComputeSWRasterEnabledOnly,
    bool patchReplayNodeInputs)
    : m_visibleClustersCounterBuffer(std::move(visibleClustersCounterBuffer))
    , m_histogramIndirectCommand(std::move(histogramIndirectCommand))
    , m_occlusionReplayStateBuffer(std::move(occlusionReplayStateBuffer))
    , m_occlusionNodeGpuInputsBuffer(std::move(occlusionNodeGpuInputsBuffer))
    , m_visibleClustersCapacity(visibleClustersCapacity)
    , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly)
    , m_patchReplayNodeInputs(patchReplayNodeInputs) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CreateRasterBucketsHistogramCommandCSMain",
        {},
        "CLod_RasterBucketsCreateCommandPSO");
}

RasterBucketCreateCommandBindings RasterBucketCreateCommandPass::Declare(org::PassBuilder& builder) {
    RasterBucketCreateCommandBindings bindings{builder.BindShaderResource(m_visibleClustersCounterBuffer),
        builder.BindUnorderedAccess(m_histogramIndirectCommand)};
    if (m_patchReplayNodeInputs) {
        bindings.replayState = builder.BindShaderResource(m_occlusionReplayStateBuffer);
        bindings.nodeInputs = builder.BindUnorderedAccess(m_occlusionNodeGpuInputsBuffer);
    }
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    bindings.numBuckets = m_numBuckets;
    bindings.visibleCapacity = m_visibleClustersCapacity;
    bindings.enabled = m_enabled;
    bindings.patchReplay = m_patchReplayNodeInputs;
    return bindings;
}


br::render::PreparedComputeDispatch RasterBucketCreateCommandPass::Prepare(
    const RasterBucketCreateCommandBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const uint32_t numBuckets = context->preparedRasterBucketCount;
    const bool enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    data.program = preparation.CaptureProgram(m_pso);
    data.descriptorIndices = CaptureResourceDescriptorIndices(m_pso.GetResourceDescriptorSlots());
    data.constants[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.visibleCount, {org::BindlessViewKind::ShaderResource}).index;
    data.constants[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.indirectCommand, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = bindings.patchReplay ? preparation.ResolveView(bindings.replayState, {org::BindlessViewKind::ShaderResource}).index : 0xFFFFFFFFu;
    data.constants[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = bindings.patchReplay ? preparation.ResolveView(bindings.nodeInputs, {org::BindlessViewKind::UnorderedAccess}).index : 0xFFFFFFFFu;
    data.constants[CLOD_CREATE_NUM_RASTER_BUCKETS] = numBuckets;
    data.constants[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = bindings.visibleCapacity;
    data.groupsX = enabled ? 1u : 0u;
    return data;
}

void RasterBucketCreateCommandPass::Update(const org::UpdateExecutionContext& executionContext) {
    const auto* context = executionContext.hostData->Get<UpdateContext>();
    m_numBuckets = context->preparedRasterBucketCount;
    m_enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
}

