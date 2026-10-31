#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketCompactAndArgsPass.h"
#include "Render/PassBuilders.h"

#include <algorithm>

#include <BasicTelemetry/Telemetry.h>
#include <BasicTelemetry/Tracy.h>

#include "Materials/MaterialManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
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
    RasterBucketCompactAndArgsBindings bindings{m_clearPipeline, m_pso, m_compactionCommandSignature,
        builder.ShaderResource(m_visibleClustersBuffer),
        builder.ShaderResource(m_visibleClusterTransformIndicesBuffer),
        builder.ShaderResource(m_visibleClustersCounterBuffer),
        builder.ShaderResource(m_compactedBaseCounterBuffer)};
    builder.ShaderResource(
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer);
    builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    bindings.indirectCommand = builder.IndirectArguments(m_indirectCommand);
    bindings.histogram = builder.ShaderResource(m_histogramBuffer);
    bindings.offsets = builder.ShaderResource(m_offsetsBuffer);
    bindings.writeCursor = builder.UnorderedAccess(m_writeCursorBuffer);
    bindings.compactedClusters = builder.UnorderedAccess(m_compactedClustersBuffer);
    bindings.compactedTransforms = builder.UnorderedAccess(m_compactedClusterTransformIndicesBuffer);
    bindings.indirectArgs = builder.UnorderedAccess(m_indirectArgsBuffer);
    bindings.sortedMapping = builder.UnorderedAccess(m_sortedToUnsortedMappingBuffer);
    if (m_reyesOwnershipBitsetBuffer) {
        bindings.reyesOwnership = builder.ShaderResource(m_reyesOwnershipBitsetBuffer);
    }
    if (m_readBaseCounterBuffer) {
        bindings.readBaseCount = builder.ShaderResource(m_readBaseCounterBuffer);
    }
    if (m_telemetryBuffer) {
        bindings.telemetry = builder.UnorderedAccess(m_telemetryBuffer);
    }

    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    bindings.maxVisibleClusters = static_cast<uint32_t>(m_maxVisibleClusters);
    bindings.appendToExisting = m_appendToExisting;
    bindings.readReverse = m_readReverse;
    bindings.buildSoftwareRasterDispatch = m_buildSoftwareRasterDispatch;
    bindings.hasReadBaseCount = static_cast<bool>(m_readBaseCounterBuffer);
    bindings.hasReyesOwnership = static_cast<bool>(m_reyesOwnershipBitsetBuffer);
    bindings.hasTelemetry = static_cast<bool>(m_telemetryBuffer);
    return bindings;
}


RasterBucketCompactAndArgsFrame RasterBucketCompactAndArgsPass::Prepare(
    const RasterBucketCompactAndArgsBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const uint32_t numBuckets = context->preparedRasterBucketCount;
    const bool enabled = numBuckets != 0u && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()));
    // The UAV and indirect-argument reference must resolve from the same
    // declaration snapshot. The wrapper may rotate to a newer allocation after
    // this frame has been accepted; consulting it here would report a
    // different backing than ExecuteIndirect consumes.
    const auto indirectArgsHandle =
        preparation.ResolveCapturedResource(preparation.DeclaredReference(bindings.indirectArgs)).GetHandle();
    const auto indirectArgsDescriptor = static_cast<int64_t>(preparation.Resolve(bindings.indirectArgs).index);
    BT_PLOT("CLod.RasterArgs.WriterResourceIndex", static_cast<int64_t>(indirectArgsHandle.index));
    BT_PLOT("CLod.RasterArgs.WriterResourceGeneration", static_cast<int64_t>(indirectArgsHandle.generation));
    BT_PLOT("CLod.RasterArgs.WriterDescriptorIndex", indirectArgsDescriptor);
    if (m_indirectArgsBuffer->GetName().find("HW phase1") != std::string::npos) {
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.WriterResourceIndex",
            static_cast<int64_t>(indirectArgsHandle.index));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.WriterResourceGeneration",
            static_cast<int64_t>(indirectArgsHandle.generation));
        basic_telemetry::SetGauge("BasicRenderer.CLod.Phase1Args.WriterDescriptorIndex", indirectArgsDescriptor);
        BT_PLOT("CLod.RasterArgs.PrimaryWriterResourceIndex", static_cast<int64_t>(indirectArgsHandle.index));
        BT_PLOT("CLod.RasterArgs.PrimaryWriterResourceGeneration", static_cast<int64_t>(indirectArgsHandle.generation));
        BT_PLOT("CLod.RasterArgs.PrimaryWriterDescriptorIndex", indirectArgsDescriptor);
    }
    return {numBuckets, enabled, IsCLodWorkGraphTelemetryEnabled()};
}

void RasterBucketCompactAndArgsPass::Record(const RasterBucketCompactAndArgsBindings& bindings,
    const RasterBucketCompactAndArgsFrame& frame, org::PassRecordContext& context) {
    if (!frame.enabled) return;
    br::render::MiscConstants clear{};
    clear[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.writeCursor);
    clear[CLOD_CLEAR_UINT_BUFFER_COUNT] = frame.numBuckets;
    br::render::DispatchCompute(context, bindings.clearPipeline, clear, (frame.numBuckets + 63u) / 64u);
    context.BufferBarriers({bindings.writeCursor.Resource()},
        rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);

    br::render::MiscConstants c{};
    c[CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX] =
        bindings.appendToExisting && bindings.hasReadBaseCount ? context.Index(bindings.readBaseCount) : 0xFFFFFFFFu;
    c[CLOD_COMPACTION_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleClusters);
    c[CLOD_COMPACTION_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = context.Index(bindings.visibleTransforms);
    c[CLOD_COMPACTION_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleCount);
    c[CLOD_COMPACTION_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    c[CLOD_COMPACTION_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX] = context.Index(bindings.offsets);
    c[CLOD_COMPACTION_RASTER_BUCKETS_WRITE_CURSOR_DESCRIPTOR_INDEX] = context.Index(bindings.writeCursor);
    c[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = context.Index(bindings.compactedClusters);
    c[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = context.Index(bindings.compactedTransforms);
    c[CLOD_COMPACTION_RASTER_BUCKETS_INDIRECT_ARGS_DESCRIPTOR_INDEX] = context.Index(bindings.indirectArgs);
    c[CLOD_COMPACTION_APPEND_BASE_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.compactedBaseCount);
    c[CLOD_COMPACTION_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] = context.Index(bindings.sortedMapping);
    c[CLOD_COMPACTION_REYES_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] =
        bindings.hasReyesOwnership ? context.Index(bindings.reyesOwnership) : 0xFFFFFFFFu;
    c[CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX] =
        bindings.hasTelemetry && frame.telemetryEnabled ? context.Index(bindings.telemetry) : 0xFFFFFFFFu;
    c[CLOD_COMPACTION_NUM_RASTER_BUCKETS] = frame.numBuckets | (bindings.appendToExisting ? 0x80000000u : 0u);
    c[CLOD_COMPACTION_READ_MODE_FLAGS] = (bindings.readReverse ? CLOD_COMPACTION_READ_FLAG_REVERSED : 0u)
        | (bindings.buildSoftwareRasterDispatch ? CLOD_COMPACTION_READ_FLAG_BUILD_SW_DISPATCH : 0u)
        | (bindings.hasReyesOwnership ? CLOD_COMPACTION_READ_FLAG_SKIP_REYES_OWNED : 0u);
    c[CLOD_COMPACTION_READ_CAPACITY] = bindings.maxVisibleClusters;
    br::render::DispatchComputeIndirect(context, bindings.compactPipeline, c, bindings.commandSignature,
        bindings.indirectCommand);
}

void RasterBucketCompactAndArgsPass::Update(const org::UpdateExecutionContext& executionContext) {
    const bool nextEnabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    m_enabled = nextEnabled;
    if (!m_enabled) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    const uint32_t nextBucketCount = context.preparedRasterBucketCount;
    m_numBuckets = nextBucketCount;

    // A resize swaps the buffers' backings. The declaration is unchanged, so the graph rebinds them without a rebuild.
    if (m_writeCursorBuffer->GetSize() < static_cast<size_t>(m_numBuckets) * sizeof(uint32_t)) {
        m_writeCursorBuffer->ResizeStructured(m_numBuckets);
    }
    if (m_indirectArgsBuffer->GetSize() < static_cast<size_t>(m_numBuckets) * sizeof(RasterizeClustersCommand)) {
        m_indirectArgsBuffer->ResizeStructured(m_numBuckets);
    }
    BT_PLOT("CLod.RasterArgs.UpdateBucketCount", static_cast<int64_t>(m_numBuckets));
    BT_PLOT("CLod.RasterArgs.UpdateBackingBytes", static_cast<int64_t>(m_indirectArgsBuffer->GetSize()));

}


