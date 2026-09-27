#include "VirtualGeometry/Reyes/RenderPasses/ReyesRasterWorkCompactAndArgsPass.h"
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

ReyesRasterWorkCompactAndArgsPass::ReyesRasterWorkCompactAndArgsPass(
    std::shared_ptr<org::Buffer> rasterWorkBuffer,
    std::shared_ptr<org::Buffer> rasterWorkCounterBuffer,
    std::shared_ptr<org::Buffer> indirectCommand,
    std::shared_ptr<org::Buffer> histogramBuffer,
    std::shared_ptr<org::Buffer> offsetsBuffer,
    std::shared_ptr<org::Buffer> writeCursorBuffer,
    std::shared_ptr<org::Buffer> compactedRasterWorkIndicesBuffer,
    std::shared_ptr<org::Buffer> packedRasterWorkGroupsBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer)
    : m_rasterWorkBuffer(std::move(rasterWorkBuffer))
    , m_rasterWorkCounterBuffer(std::move(rasterWorkCounterBuffer))
    , m_indirectCommand(std::move(indirectCommand))
    , m_histogramBuffer(std::move(histogramBuffer))
    , m_offsetsBuffer(std::move(offsetsBuffer))
    , m_writeCursorBuffer(std::move(writeCursorBuffer))
    , m_compactedRasterWorkIndicesBuffer(std::move(compactedRasterWorkIndicesBuffer))
    , m_packedRasterWorkGroupsBuffer(std::move(packedRasterWorkGroupsBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer)) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesRasterWorkBuckets.hlsl",
        L"CompactReyesRasterWorkCS",
        {},
        "CLod.ReyesRasterWorkCompactAndArgs.PSO");
    m_packPipeline = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesRasterWorkBuckets.hlsl",
        L"EmitPackedReyesRasterWorkGroupsCS",
        {},
        "CLod.ReyesRasterWorkEmitPackedGroups.PSO");
    m_finalizePackPipeline = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/reyesRasterWorkBuckets.hlsl",
        L"PackReyesRasterWorkGroupsAndBuildIndirectArgsCS",
        {},
        "CLod.ReyesRasterWorkFinalizePackedGroups.PSO");
    m_clearPipeline = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain",
        {},
        "CLod.ReyesRasterWorkCompactClear.PSO");

    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 2 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    m_compactionCommandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterBucketsHistogramIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        *m_compactionCommandSignature);
}

ReyesRasterWorkCompactBindings ReyesRasterWorkCompactAndArgsPass::Declare(org::PassBuilder& declaration) {
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    declaration.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {declaration.BindShaderResource(m_rasterWorkBuffer), declaration.BindShaderResource(m_rasterWorkCounterBuffer),
        declaration.BindIndirectArguments(m_indirectCommand), declaration.BindUnorderedAccess(m_histogramBuffer),
        declaration.BindShaderResource(m_offsetsBuffer), declaration.BindUnorderedAccess(m_writeCursorBuffer),
        declaration.BindUnorderedAccess(m_compactedRasterWorkIndicesBuffer), declaration.BindUnorderedAccess(m_packedRasterWorkGroupsBuffer),
        declaration.BindUnorderedAccess(m_indirectArgsBuffer), m_numBuckets};
}

ReyesCompactFrameData ReyesRasterWorkCompactAndArgsPass::Prepare(
    const ReyesRasterWorkCompactBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto& context = *preparation.preparationData->Get<UpdateContext>();
    ReyesCompactFrameData data{};
    const auto numBuckets = bindings.numBuckets;
    if (numBuckets == 0u) return data;
    const auto capture = [&](auto& dispatch, const org::PipelineState& pipeline) {
        dispatch.resourceHeap = context.textureDescriptorHeap.GetHandle();
        dispatch.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto binding = preparation.CaptureProgramBinding(pipeline);
        dispatch.program = binding.program;
        dispatch.descriptorIndices = std::move(binding.descriptorIndices);
    };
    capture(data.clear, m_clearPipeline);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    data.clear.constants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.cursor);
    data.clear.constants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
    data.clear.constants[CLOD_CLEAR_UINT_BUFFER_COUNT] = numBuckets;
    data.clear.groupsX = (numBuckets + 63u) / 64u;
    capture(data.compact, m_pso);
    data.compact.commandSignature = preparation.CaptureCommandSignature(m_compactionCommandSignature);
    data.compact.argumentsReference = preparation.CaptureResource(bindings.indirectCommand);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_WORK_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.work);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_WORK_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.counter);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_HISTOGRAM_DESCRIPTOR_INDEX] = uav(bindings.histogram);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_OFFSETS_DESCRIPTOR_INDEX] = srv(bindings.offsets);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_WRITE_CURSOR_DESCRIPTOR_INDEX] = uav(bindings.cursor);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_COMPACTED_WORK_INDICES_DESCRIPTOR_INDEX] = uav(bindings.compacted);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_INDIRECT_ARGS_DESCRIPTOR_INDEX] = uav(bindings.indirectArgs);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_PACKED_WORK_GROUPS_DESCRIPTOR_INDEX] = uav(bindings.packed);
    data.compact.constants[CLOD_REYES_RASTER_BUCKET_NUM_BUCKETS] = numBuckets;
    data.pack = data.compact;
    capture(data.pack, m_packPipeline);
    capture(data.finalize, m_finalizePackPipeline);
    data.finalize.constants = data.compact.constants;
    data.finalize.groupsX = (numBuckets + 63u) / 64u;
    data.cursorBarrier = preparation.CaptureResource(bindings.cursor);
    data.compactedBarrier = preparation.CaptureResource(bindings.compacted);
    data.packedBarrier = preparation.CaptureResource(bindings.packed);
    return data;
}

void ReyesRasterWorkCompactAndArgsPass::InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
    br::render::AppendFrameHeapRevision(preparation, out);
    out.push_back(br::render::PipelineRevision(m_clearPipeline));
    out.push_back(br::render::PipelineRevision(m_pso));
    out.push_back(br::render::PipelineRevision(m_packPipeline));
    out.push_back(br::render::PipelineRevision(m_finalizePackPipeline));
    out.push_back(br::render::OwnerRevision(m_compactionCommandSignature));
}

void ReyesRasterWorkCompactAndArgsPass::Record(const ReyesRasterWorkCompactBindings&,
    const ReyesCompactFrameData& data, org::PassRecordContext& recording) {
    if (data.clear.groupsX == 0) return;
    br::render::RecordPreparedComputeDispatch(data.clear, recording);
    br::render::RecordPreparedComputeUavBarrier(data.cursorBarrier, recording);
    br::render::RecordPreparedComputeIndirect(data.compact, recording);
    br::render::RecordPreparedComputeUavBarrier(data.compactedBarrier, recording);
    br::render::RecordPreparedComputeIndirect(data.pack, recording);
    br::render::RecordPreparedComputeUavBarrier(data.packedBarrier, recording);
    br::render::RecordPreparedComputeDispatch(data.finalize, recording);
}

void ReyesRasterWorkCompactAndArgsPass::Update(const org::UpdateExecutionContext& executionContext) {
    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    m_numBuckets = context.preparedRasterBucketCount;

    if (m_writeCursorBuffer->GetSize() < static_cast<size_t>(m_numBuckets) * sizeof(uint32_t)) {
        m_writeCursorBuffer->ResizeStructured(m_numBuckets);
    }
    if (m_indirectArgsBuffer->GetSize() < static_cast<size_t>(m_numBuckets) * sizeof(RasterizeClustersCommand)) {
        m_indirectArgsBuffer->ResizeStructured(m_numBuckets);
    }
}
