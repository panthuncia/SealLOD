#include "Render/GraphExtensions/ClusterLOD/RasterBucketCompactAndArgsPass.h"

#include <algorithm>
#include <vector>

#include <BasicTelemetry/Telemetry.h>

#include "Managers/MaterialManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCompactionRootConstants.h"

RasterBucketCompactAndArgsPass::RasterBucketCompactAndArgsPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> compactedBaseCounterBuffer,
    std::shared_ptr<org::Buffer> readBaseCounterBuffer,
    std::shared_ptr<org::Buffer> indirectCommand,
    std::shared_ptr<org::Buffer> histogramBuffer,
    std::shared_ptr<org::Buffer> offsetsBuffer,
    std::shared_ptr<org::Buffer> writeCursorBuffer,
    std::shared_ptr<org::Buffer> compactedClustersBuffer,
    std::shared_ptr<org::Buffer> compactedClusterTransformIndicesBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> sortedToUnsortedMappingBuffer,
    std::shared_ptr<org::Buffer> reyesOwnershipBitsetBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    uint64_t maxVisibleClusters,
    bool appendToExisting,
    bool readReverse,
    bool buildSoftwareRasterDispatch,
    bool runWhenComputeSWRasterEnabledOnly)
    : m_visibleClustersBuffer(std::move(visibleClustersBuffer))
    , m_visibleClusterTransformIndicesBuffer(std::move(visibleClusterTransformIndicesBuffer))
    , m_visibleClustersCounterBuffer(std::move(visibleClustersCounterBuffer))
    , m_compactedBaseCounterBuffer(std::move(compactedBaseCounterBuffer))
    , m_readBaseCounterBuffer(std::move(readBaseCounterBuffer))
    , m_indirectCommand(std::move(indirectCommand))
    , m_histogramBuffer(std::move(histogramBuffer))
    , m_offsetsBuffer(std::move(offsetsBuffer))
    , m_writeCursorBuffer(std::move(writeCursorBuffer))
    , m_compactedClustersBuffer(std::move(compactedClustersBuffer))
    , m_compactedClusterTransformIndicesBuffer(std::move(compactedClusterTransformIndicesBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_sortedToUnsortedMappingBuffer(std::move(sortedToUnsortedMappingBuffer))
    , m_reyesOwnershipBitsetBuffer(std::move(reyesOwnershipBitsetBuffer))
    , m_telemetryBuffer(std::move(telemetryBuffer))
    , m_maxVisibleClusters(maxVisibleClusters)
    , m_appendToExisting(appendToExisting)
    , m_readReverse(readReverse)
    , m_buildSoftwareRasterDispatch(buildSoftwareRasterDispatch)
    , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly)
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/clodUtil.hlsl",
        L"CompactClustersAndBuildIndirectArgsCS",
        {},
        "CLod_RasterBucketsCompactAndArgsPSO");
    m_clearPipeline = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain",
        {},
        "CLod_RasterBucketsClearUintPSO");

    rhi::IndirectArg args[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 2 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    rhi::CommandSignaturePtr commandSignature;
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterBucketsHistogramIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        commandSignature);
    m_compactionCommandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(commandSignature));
}

RasterBucketCompactAndArgsBindings RasterBucketCompactAndArgsPass::Declare(org::PassBuilder& builder) {
    RasterBucketCompactAndArgsBindings bindings{
        builder.BindShaderResource(m_visibleClustersBuffer),
        builder.BindShaderResource(m_visibleClusterTransformIndicesBuffer),
        builder.BindShaderResource(m_visibleClustersCounterBuffer),
        builder.BindShaderResource(m_compactedBaseCounterBuffer)};
    builder.WithShaderResource(
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer)
        .WithUnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    bindings.indirectCommand = builder.BindIndirectArguments(m_indirectCommand);
    bindings.histogram = builder.BindShaderResource(m_histogramBuffer);
    bindings.offsets = builder.BindShaderResource(m_offsetsBuffer);
    bindings.writeCursor = builder.BindUnorderedAccess(m_writeCursorBuffer);
    bindings.compactedClusters = builder.BindUnorderedAccess(m_compactedClustersBuffer);
    bindings.compactedTransforms = builder.BindUnorderedAccess(m_compactedClusterTransformIndicesBuffer);
    bindings.indirectArgs = builder.BindUnorderedAccess(m_indirectArgsBuffer);
    bindings.sortedMapping = builder.BindUnorderedAccess(m_sortedToUnsortedMappingBuffer);
    if (m_reyesOwnershipBitsetBuffer) {
        bindings.reyesOwnership = builder.BindShaderResource(m_reyesOwnershipBitsetBuffer);
    }
    if (m_readBaseCounterBuffer) {
        bindings.readBaseCount = builder.BindShaderResource(m_readBaseCounterBuffer);
    }
    if (m_telemetryBuffer) {
        bindings.telemetry = builder.BindUnorderedAccess(m_telemetryBuffer);
    }

    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    bindings.numBuckets = m_numBuckets;
    bindings.maxVisibleClusters = static_cast<uint32_t>(m_maxVisibleClusters);
    bindings.enabled = m_enabled && m_numBuckets != 0u;
    bindings.appendToExisting = m_appendToExisting;
    bindings.readReverse = m_readReverse;
    bindings.buildSoftwareRasterDispatch = m_buildSoftwareRasterDispatch;
    bindings.hasReadBaseCount = static_cast<bool>(m_readBaseCounterBuffer);
    bindings.hasReyesOwnership = static_cast<bool>(m_reyesOwnershipBitsetBuffer);
    bindings.hasTelemetry = static_cast<bool>(m_telemetryBuffer);
    bindings.telemetryEnabled = IsCLodWorkGraphTelemetryEnabled();
    return bindings;
}


RasterBucketCompactAndArgsPreparedData RasterBucketCompactAndArgsPass::Prepare(
    const RasterBucketCompactAndArgsBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const uint32_t numBuckets = context->preparedRasterBucketCount;
    const bool enabled = numBuckets != 0u && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()));
    PreparedData data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    data.clearProgram = preparation.CaptureProgram(m_clearPipeline);
    data.compactProgram = preparation.CaptureProgram(m_pso);
    preparation.Retain(m_compactionCommandSignature);
    data.commandSignature = (*m_compactionCommandSignature)->GetHandle();
    data.indirectCommand = preparation.CaptureResource(bindings.indirectCommand);
    data.cursorResource = preparation.CaptureResource(bindings.writeCursor);
    data.clearDescriptorIndices = CaptureResourceDescriptorIndices(m_clearPipeline.GetResourceDescriptorSlots());
    data.compactDescriptorIndices = CaptureResourceDescriptorIndices(m_pso.GetResourceDescriptorSlots());
    data.clearConstants.resize(NumMiscUintRootConstants);
    data.compactConstants.resize(NumMiscUintRootConstants);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    const auto indirectArgsResource = preparation.CaptureResource(bindings.indirectArgs);
    const auto indirectArgsHandle = preparation.ResolveCapturedResource(indirectArgsResource).GetHandle();
    BT_PLOT("CLod.RasterArgs.WriterResourceIndex", static_cast<int64_t>(indirectArgsHandle.index));
    BT_PLOT("CLod.RasterArgs.WriterResourceGeneration", static_cast<int64_t>(indirectArgsHandle.generation));
    data.enabled = enabled;
    data.clearGroups = (numBuckets + 63u) / 64u;
    data.clearConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.writeCursor);
    data.clearConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = numBuckets;
    auto& c = data.compactConstants;
    c[CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX] = bindings.appendToExisting && bindings.hasReadBaseCount ? srv(bindings.readBaseCount) : 0xFFFFFFFFu;
    c[CLOD_COMPACTION_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = srv(bindings.visibleClusters);
    c[CLOD_COMPACTION_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = srv(bindings.visibleTransforms);
    c[CLOD_COMPACTION_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.visibleCount);
    c[CLOD_COMPACTION_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = srv(bindings.histogram);
    c[CLOD_COMPACTION_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX] = srv(bindings.offsets);
    c[CLOD_COMPACTION_RASTER_BUCKETS_WRITE_CURSOR_DESCRIPTOR_INDEX] = uav(bindings.writeCursor);
    c[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = uav(bindings.compactedClusters);
    c[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = uav(bindings.compactedTransforms);
    // The UAV and indirect-argument reference must resolve from the same
    // declaration snapshot. The wrapper may rotate to a newer allocation after
    // this frame has been accepted; consulting it here would write commands to
    // a different backing than ExecuteIndirect consumes.
    c[CLOD_COMPACTION_RASTER_BUCKETS_INDIRECT_ARGS_DESCRIPTOR_INDEX] = uav(bindings.indirectArgs);
    BT_PLOT("CLod.RasterArgs.WriterDescriptorIndex", static_cast<int64_t>(
        c[CLOD_COMPACTION_RASTER_BUCKETS_INDIRECT_ARGS_DESCRIPTOR_INDEX]));
    if (m_indirectArgsBuffer->GetName().find("HW phase1") != std::string::npos) {
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.WriterResourceIndex",
            static_cast<int64_t>(indirectArgsHandle.index));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.WriterResourceGeneration",
            static_cast<int64_t>(indirectArgsHandle.generation));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.WriterDescriptorIndex",
            static_cast<int64_t>(c[CLOD_COMPACTION_RASTER_BUCKETS_INDIRECT_ARGS_DESCRIPTOR_INDEX]));
        BT_PLOT("CLod.RasterArgs.PrimaryWriterResourceIndex", static_cast<int64_t>(indirectArgsHandle.index));
        BT_PLOT("CLod.RasterArgs.PrimaryWriterResourceGeneration", static_cast<int64_t>(indirectArgsHandle.generation));
        BT_PLOT("CLod.RasterArgs.PrimaryWriterDescriptorIndex", static_cast<int64_t>(
            c[CLOD_COMPACTION_RASTER_BUCKETS_INDIRECT_ARGS_DESCRIPTOR_INDEX]));
    }
    c[CLOD_COMPACTION_APPEND_BASE_COUNTER_DESCRIPTOR_INDEX] = srv(bindings.compactedBaseCount);
    c[CLOD_COMPACTION_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] = uav(bindings.sortedMapping);
    c[CLOD_COMPACTION_REYES_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = bindings.hasReyesOwnership ? srv(bindings.reyesOwnership) : 0xFFFFFFFFu;
    c[CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX] = bindings.hasTelemetry && IsCLodWorkGraphTelemetryEnabled() ? uav(bindings.telemetry) : 0xFFFFFFFFu;
    c[CLOD_COMPACTION_NUM_RASTER_BUCKETS] = numBuckets | (bindings.appendToExisting ? 0x80000000u : 0u);
    c[CLOD_COMPACTION_READ_MODE_FLAGS] = (bindings.readReverse ? CLOD_COMPACTION_READ_FLAG_REVERSED : 0u)
        | (bindings.buildSoftwareRasterDispatch ? CLOD_COMPACTION_READ_FLAG_BUILD_SW_DISPATCH : 0u)
        | (bindings.hasReyesOwnership ? CLOD_COMPACTION_READ_FLAG_SKIP_REYES_OWNED : 0u);
    c[CLOD_COMPACTION_READ_CAPACITY] = bindings.maxVisibleClusters;
    return data;
}

void RasterBucketCompactAndArgsPass::Record(const RasterBucketCompactAndArgsBindings&,
    const PreparedData& data, org::PassRecordContext& recording) {
    if (!data.enabled) return;
    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
    commands.BindLayout(data.layout);
    auto bindIndices = [&](const std::vector<unsigned int>& indices) {
        if (!indices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(indices.size()), indices.data());
    };
    commands.BindPipeline(recording.Resolve(data.clearProgram));
    bindIndices(data.clearDescriptorIndices);
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, data.clearConstants.data());
    commands.Dispatch(data.clearGroups, 1, 1);
    rhi::BufferBarrier barrier{};
    barrier.buffer = recording.Resolve(data.cursorResource).GetHandle();
    barrier.beforeAccess = barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    barrier.beforeSync = barrier.afterSync = rhi::ResourceSyncState::ComputeShading;
    rhi::BarrierBatch barriers{}; barriers.buffers = {&barrier}; commands.Barriers(barriers);
    commands.BindPipeline(recording.Resolve(data.compactProgram));
    bindIndices(data.compactDescriptorIndices);
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, data.compactConstants.data());
    commands.ExecuteIndirect(data.commandSignature,
        recording.Resolve(data.indirectCommand).GetHandle(), 0, {}, 0, 1);
}

void RasterBucketCompactAndArgsPass::Update(const org::UpdateExecutionContext& executionContext) {
    const bool nextEnabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    m_declaredResourcesChanged = nextEnabled != m_enabled;
    m_enabled = nextEnabled;
    if (!m_enabled) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const uint32_t nextBucketCount = context.preparedRasterBucketCount;
    m_declaredResourcesChanged |= nextBucketCount != m_numBuckets;
    m_numBuckets = nextBucketCount;

    if (m_writeCursorBuffer->GetSize() < static_cast<size_t>(m_numBuckets) * sizeof(uint32_t)) {
        m_writeCursorBuffer->ResizeStructured(m_numBuckets);
        m_declaredResourcesChanged = true;
    }
    if (m_indirectArgsBuffer->GetSize() < static_cast<size_t>(m_numBuckets) * sizeof(RasterizeClustersCommand)) {
        m_indirectArgsBuffer->ResizeStructured(m_numBuckets);
        m_declaredResourcesChanged = true;
    }
    BT_PLOT("CLod.RasterArgs.UpdateBucketCount", static_cast<int64_t>(m_numBuckets));
    BT_PLOT("CLod.RasterArgs.UpdateBackingBytes", static_cast<int64_t>(m_indirectArgsBuffer->GetSize()));

}

bool RasterBucketCompactAndArgsPass::DeclaredResourcesChanged() const {
    return m_declaredResourcesChanged;
}

