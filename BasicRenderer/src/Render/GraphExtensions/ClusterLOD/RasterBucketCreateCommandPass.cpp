#include "Render/GraphExtensions/ClusterLOD/RasterBucketCreateCommandPass.h"

#include "Managers/MaterialManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"

RasterBucketCreateCommandPass::RasterBucketCreateCommandPass(
    std::shared_ptr<Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<Buffer> histogramIndirectCommand,
    std::shared_ptr<Buffer> occlusionReplayStateBuffer,
    std::shared_ptr<Buffer> occlusionNodeGpuInputsBuffer,
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

void RasterBucketCreateCommandPass::DeclareResourceUsages(ComputePassBuilder* builder) {
    builder->WithShaderResource(m_visibleClustersCounterBuffer)
        .WithUnorderedAccess(m_histogramIndirectCommand);
    if (m_patchReplayNodeInputs) {
        builder->WithShaderResource(m_occlusionReplayStateBuffer)
            .WithUnorderedAccess(m_occlusionNodeGpuInputsBuffer);
    }
    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void RasterBucketCreateCommandPass::Setup() {}

PassReturn RasterBucketCreateCommandPass::Execute(PassExecutionContext& executionContext) {
    if (m_runWhenComputeSWRasterEnabledOnly && !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
        return {};
    }

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t rc[NumMiscUintRootConstants] = {};
    rc[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_visibleClustersCounterBuffer->GetSRVInfo(0).slot.index;
    rc[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = m_histogramIndirectCommand->GetUAVShaderVisibleInfo(0).slot.index;
    rc[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] = m_patchReplayNodeInputs ? m_occlusionReplayStateBuffer->GetSRVInfo(0).slot.index : 0xFFFFFFFFu;
    rc[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] = m_patchReplayNodeInputs ? m_occlusionNodeGpuInputsBuffer->GetUAVShaderVisibleInfo(0).slot.index : 0xFFFFFFFFu;
    rc[CLOD_CREATE_NUM_RASTER_BUCKETS] = context.materialManager->GetRasterBucketCount();
    rc[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = m_visibleClustersCapacity;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rc);

    commandList.Dispatch(1, 1, 1);
    return {};
}

void RasterBucketCreateCommandPass::Update(const UpdateExecutionContext& executionContext) {
    (void)executionContext;
}

void RasterBucketCreateCommandPass::Cleanup() {}
