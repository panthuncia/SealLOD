#include "VirtualGeometry/Reyes/RenderPasses/ReyesRasterWorkHistogramPass.h"
#include "Runtime/StateGraph/InvocationRevision.h"

#include "Materials/MaterialManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeBarrier.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodReyesRasterWorkBucketRootConstants.h"

ReyesRasterWorkHistogramPass::ReyesRasterWorkHistogramPass(
    std::shared_ptr<org::Buffer> rasterWorkBuffer,
    std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<org::Buffer> histogramIndirectCommand,
    std::shared_ptr<org::Buffer> histogramBuffer)
    : m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_rasterWorkCounterBuffer(std::move(rasterWorkCounterBuffer))
    , m_histogramIndirectCommand(std::move(histogramIndirectCommand))
    , m_histogramBuffer(std::move(histogramBuffer)) {
    CreatePipelines(
        DeviceManager::GetInstance().GetDevice(),
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_histogramPipeline,
        m_clearPipeline);

    rhi::IndirectArg histogramArgs[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 2 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    m_histogramCommandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(histogramArgs, 2), sizeof(RasterBucketsHistogramIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        *m_histogramCommandSignature);
}

ReyesRasterWorkHistogramBindings ReyesRasterWorkHistogramPass::Declare(org::PassBuilder& declaration) {
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    declaration.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {declaration.BindShaderResource(m_rasterWorkBuffer), declaration.BindShaderResource(m_rasterWorkCounterBuffer),
        declaration.BindIndirectArguments(m_histogramIndirectCommand), declaration.BindUnorderedAccess(m_histogramBuffer), m_numBuckets};
}

ReyesHistogramFrameData ReyesRasterWorkHistogramPass::Prepare(
    const ReyesRasterWorkHistogramBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    ReyesHistogramFrameData data{};
    const auto numRasterBuckets = bindings.numBuckets;
    if (numRasterBuckets == 0u) return data;
    const auto capture = [&](auto& dispatch, const org::PipelineState& pipeline) {
        dispatch.resourceHeap = context.textureDescriptorHeap.GetHandle();
        dispatch.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto binding = preparation.CaptureProgramBinding(pipeline);
        dispatch.program = binding.program;
        dispatch.descriptorIndices = std::move(binding.descriptorIndices);
    };
    capture(data.clear, m_clearPipeline);
    data.clear.constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.histogram, {org::BindlessViewKind::UnorderedAccess}).index;
    data.clear.constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
    data.clear.constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = numRasterBuckets;
    data.clear.groupsX = (numRasterBuckets + 63u) / 64u;
    capture(data.histogram, m_histogramPipeline);
    data.histogram.commandSignature = preparation.CaptureCommandSignature(m_histogramCommandSignature);
    data.histogram.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    data.histogramBarrier = preparation.CaptureResource(bindings.histogram);
    data.histogram.constants[CLOD_REYES_RASTER_BUCKET_WORK_BUFFER_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.work, {org::BindlessViewKind::ShaderResource}).index;
    data.histogram.constants[CLOD_REYES_RASTER_BUCKET_WORK_COUNTER_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.counter, {org::BindlessViewKind::ShaderResource}).index;
    data.histogram.constants[CLOD_REYES_RASTER_BUCKET_HISTOGRAM_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.histogram, {org::BindlessViewKind::UnorderedAccess}).index;
    return data;
}

void ReyesRasterWorkHistogramPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_clearPipeline));
    out.push_back(br::render::PipelineRevision(m_histogramPipeline));
    out.push_back(br::render::OwnerRevision(m_histogramCommandSignature));
}

void ReyesRasterWorkHistogramPass::Record(const ReyesRasterWorkHistogramBindings&,
    const ReyesHistogramFrameData& data, org::PassRecordContext& recording) {
    if (data.clear.groupsX == 0) return;
    br::render::RecordPreparedComputeDispatch(data.clear, recording);
    br::render::RecordPreparedComputeUavBarrier(data.histogramBarrier, recording);
    br::render::RecordPreparedComputeIndirect(data.histogram, recording);
}

void ReyesRasterWorkHistogramPass::Update(const org::UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const auto numRasterBuckets = context.preparedRasterBucketCount;
    m_numBuckets = numRasterBuckets;

    if (m_histogramBuffer->GetSize() < static_cast<size_t>(numRasterBuckets) * sizeof(uint32_t)) {
        m_histogramBuffer->ResizeStructured(numRasterBuckets);
    }
}

void ReyesRasterWorkHistogramPass::CreatePipelines(
    rhi::Device device,
    rhi::PipelineLayoutHandle globalRootSignature,
    org::PipelineState& outHistogramPipeline,
    org::PipelineState& outClearPipeline)
{
    (void)device;
    outHistogramPipeline = PSOManager::GetInstance().MakeComputePipeline(
        globalRootSignature,
        L"Shaders/ClusterLOD/reyesRasterWorkBuckets.hlsl",
        L"HistogramReyesRasterWorkBucketsCS",
        {},
        "CLod.ReyesRasterWorkHistogram.PSO");
    outClearPipeline = PSOManager::GetInstance().MakeComputePipeline(
        globalRootSignature,
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain",
        {},
        "CLod.ReyesRasterWorkHistogramClear.PSO");
}
