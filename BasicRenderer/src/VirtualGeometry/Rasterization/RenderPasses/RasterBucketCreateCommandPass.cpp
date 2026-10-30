#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketCreateCommandPass.h"
#include "Render/PassBuilders.h"

#include "Materials/MaterialManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodCreateCommandRootConstants.h"

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
    RasterBucketCreateCommandBindings bindings{m_pso, builder.ShaderResource(m_visibleClustersCounterBuffer),
        builder.UnorderedAccess(m_histogramIndirectCommand)};
    if (m_patchReplayNodeInputs) {
        bindings.replayState = builder.ShaderResource(m_occlusionReplayStateBuffer);
        bindings.nodeInputs = builder.UnorderedAccess(m_occlusionNodeGpuInputsBuffer);
    }
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    bindings.visibleCapacity = m_visibleClustersCapacity;
    bindings.patchReplay = m_patchReplayNodeInputs;
    return bindings;
}

RasterBucketCreateCommandFrame RasterBucketCreateCommandPass::Prepare(
    const RasterBucketCreateCommandBindings&, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const bool enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    return {context->preparedRasterBucketCount, enabled ? 1u : 0u};
}

void RasterBucketCreateCommandPass::Record(const RasterBucketCreateCommandBindings& bindings,
    const RasterBucketCreateCommandFrame& frame, org::PassRecordContext& context) {
    br::render::MiscConstants constants{};
    constants[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleCount);
    constants[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX] = context.Index(bindings.indirectCommand);
    constants[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX] =
        bindings.patchReplay ? context.Index(bindings.replayState) : 0xFFFFFFFFu;
    constants[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX] =
        bindings.patchReplay ? context.Index(bindings.nodeInputs) : 0xFFFFFFFFu;
    constants[CLOD_CREATE_NUM_RASTER_BUCKETS] = frame.numBuckets;
    constants[CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY] = bindings.visibleCapacity;
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groups);
}
