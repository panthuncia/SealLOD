#include "Render/GraphExtensions/ClusterLOD/RasterBucketCompactAndArgsPass.h"

#include <algorithm>
#include <vector>

#include "Managers/MaterialManager.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodCompactionRootConstants.h"

RasterBucketCompactAndArgsPass::RasterBucketCompactAndArgsPass(
    std::shared_ptr<Buffer> visibleClustersBuffer,
    std::shared_ptr<Buffer> visibleClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<Buffer> compactedBaseCounterBuffer,
    std::shared_ptr<Buffer> readBaseCounterBuffer,
    std::shared_ptr<Buffer> indirectCommand,
    std::shared_ptr<Buffer> histogramBuffer,
    std::shared_ptr<Buffer> offsetsBuffer,
    std::shared_ptr<Buffer> writeCursorBuffer,
    std::shared_ptr<Buffer> compactedClustersBuffer,
    std::shared_ptr<Buffer> compactedClusterTransformIndicesBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> sortedToUnsortedMappingBuffer,
    std::shared_ptr<Buffer> reyesOwnershipBitsetBuffer,
    std::shared_ptr<Buffer> telemetryBuffer,
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
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 2), sizeof(RasterBucketsHistogramIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_compactionCommandSignature);
}

void RasterBucketCompactAndArgsPass::DeclareResourceUsages(ComputePassBuilder* builder) {
    builder->WithShaderResource(
            m_visibleClustersBuffer,
            m_visibleClusterTransformIndicesBuffer,
            m_visibleClustersCounterBuffer,
            m_compactedBaseCounterBuffer,
            m_histogramBuffer,
            m_offsetsBuffer,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::SkeletonResources::SkinningInstanceInfo,
            Builtin::PerMeshBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer)
        .WithUnorderedAccess(
            Builtin::Material::TextureStreamingFeedbackBuffer,
            m_writeCursorBuffer,
            m_compactedClustersBuffer,
            m_compactedClusterTransformIndicesBuffer,
            m_indirectArgsBuffer,
            m_sortedToUnsortedMappingBuffer)
        .WithIndirectArguments(m_indirectCommand);
    if (m_reyesOwnershipBitsetBuffer) {
        builder->WithShaderResource(m_reyesOwnershipBitsetBuffer);
    }
    if (m_readBaseCounterBuffer) {
        builder->WithShaderResource(m_readBaseCounterBuffer);
    }
    if (m_telemetryBuffer) {
        builder->WithUnorderedAccess(m_telemetryBuffer);
    }

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void RasterBucketCompactAndArgsPass::Setup() {
}

PassReturn RasterBucketCompactAndArgsPass::Execute(PassExecutionContext& executionContext) {
    if (m_runWhenComputeSWRasterEnabledOnly && !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
        return {};
    }

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;
    auto& pm = PSOManager::GetInstance();

    auto numBuckets = context.materialManager->GetRasterBucketCount();
    if (numBuckets == 0u) {
        return {};
    }
    const uint32_t kThreads = 64;
    const uint64_t maxItems = std::max<uint64_t>(m_maxVisibleClusters, numBuckets);
    const uint32_t groups = static_cast<uint32_t>((maxItems + kThreads - 1u) / kThreads);
    (void)groups;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(pm.GetComputeRootSignature().GetHandle());

    BindResourceDescriptorIndices(commandList, m_clearPipeline.GetResourceDescriptorSlots());
    commandList.BindPipeline(m_clearPipeline.GetAPIPipelineState().GetHandle());

    uint32_t clearRootConstants[NumMiscUintRootConstants] = {};
    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = m_writeCursorBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = numBuckets;
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        clearRootConstants);
    commandList.Dispatch((numBuckets + 63u) / 64u, 1u, 1u);

    rhi::BufferBarrier writeCursorBarrier{};
    writeCursorBarrier.buffer = m_writeCursorBuffer->GetAPIResource().GetHandle();
    writeCursorBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    writeCursorBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    writeCursorBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
    writeCursorBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;

    rhi::BarrierBatch barrierBatch{};
    barrierBatch.buffers = { &writeCursorBarrier };
    commandList.Barriers(barrierBatch);

    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    unsigned int rc[NumMiscUintRootConstants] = {};
    rc[CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    rc[CLOD_COMPACTION_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = m_visibleClustersBuffer->GetSRVInfo(0).slot.index;
    rc[CLOD_COMPACTION_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = m_visibleClusterTransformIndicesBuffer->GetSRVInfo(0).slot.index;
    rc[CLOD_COMPACTION_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = m_visibleClustersCounterBuffer->GetSRVInfo(0).slot.index;
    rc[CLOD_COMPACTION_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = m_histogramBuffer->GetSRVInfo(0).slot.index;
    rc[CLOD_COMPACTION_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX] = m_offsetsBuffer->GetSRVInfo(0).slot.index;
    rc[CLOD_COMPACTION_RASTER_BUCKETS_WRITE_CURSOR_DESCRIPTOR_INDEX] = m_writeCursorBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rc[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX] = m_compactedClustersBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rc[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX] = m_compactedClusterTransformIndicesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rc[CLOD_COMPACTION_RASTER_BUCKETS_INDIRECT_ARGS_DESCRIPTOR_INDEX] = m_indirectArgsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rc[CLOD_COMPACTION_APPEND_BASE_COUNTER_DESCRIPTOR_INDEX] = m_compactedBaseCounterBuffer->GetSRVInfo(0).slot.index;
    rc[CLOD_COMPACTION_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX] = m_sortedToUnsortedMappingBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rc[CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX] = 0xFFFFFFFFu;
    if (m_reyesOwnershipBitsetBuffer) {
        rc[CLOD_COMPACTION_REYES_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] = m_reyesOwnershipBitsetBuffer->GetSRVInfo(0).slot.index;
    }
    if (m_telemetryBuffer && IsCLodWorkGraphTelemetryEnabled()) {
        rc[CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX] = m_telemetryBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    }
    rc[CLOD_COMPACTION_NUM_RASTER_BUCKETS] = numBuckets | (m_appendToExisting ? 0x80000000u : 0u);
    if (m_appendToExisting && m_readBaseCounterBuffer) {
        rc[CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX] = m_readBaseCounterBuffer->GetSRVInfo(0).slot.index;
    }
    rc[CLOD_COMPACTION_READ_MODE_FLAGS] =
        (m_readReverse ? CLOD_COMPACTION_READ_FLAG_REVERSED : 0u) |
        (m_buildSoftwareRasterDispatch ? CLOD_COMPACTION_READ_FLAG_BUILD_SW_DISPATCH : 0u) |
        (m_reyesOwnershipBitsetBuffer ? CLOD_COMPACTION_READ_FLAG_SKIP_REYES_OWNED : 0u);
    rc[CLOD_COMPACTION_READ_CAPACITY] = static_cast<uint32_t>(m_maxVisibleClusters);
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rc);

    commandList.ExecuteIndirect(
        m_compactionCommandSignature->GetHandle(),
        m_indirectCommand->GetAPIResource().GetHandle(),
        0,
        {},
        0,
        1);

    return {};
}

void RasterBucketCompactAndArgsPass::Update(const UpdateExecutionContext& executionContext) {
    if (m_runWhenComputeSWRasterEnabledOnly && !CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    auto numBuckets = context.materialManager->GetRasterBucketCount();

    if (m_writeCursorBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(uint32_t)) {
        m_writeCursorBuffer->ResizeStructured(numBuckets);
    }
    if (m_indirectArgsBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(RasterizeClustersCommand)) {
        m_indirectArgsBuffer->ResizeStructured(numBuckets);
    }

}

void RasterBucketCompactAndArgsPass::Cleanup() {}
