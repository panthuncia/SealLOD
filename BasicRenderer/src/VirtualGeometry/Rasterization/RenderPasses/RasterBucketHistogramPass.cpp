#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketHistogramPass.h"
#include "Render/PassBuilders.h"

#include <stdexcept>
#include <vector>

#include "Materials/MaterialManager.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodHistogramRootConstants.h"

namespace {
// The frame's raster bucket count; `enabled` is false without buckets or while the pass is gated off (software raster in
// compute only).
uint32_t TakeRasterBucketCount(const org::UpdateExecutionContext& execution, bool computeSoftwareRasterOnly, bool& enabled) {
    const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
    const uint32_t numBuckets = context ? context->preparedRasterBucketCount : 0u;
    enabled = numBuckets != 0u && (!computeSoftwareRasterOnly || CLodSoftwareRasterUsesCompute(
        SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()));
    return numBuckets;
}
}

RasterBucketHistogramPass::RasterBucketHistogramPass(
    std::shared_ptr<org::Buffer> visibleClustersBuffer,
    std::shared_ptr<org::Buffer> visibleClustersCounterBuffer,
    std::shared_ptr<org::Buffer> histogramIndirectCommand,
    std::shared_ptr<org::Buffer> histogramBuffer,
    std::shared_ptr<org::Buffer> reyesOwnershipBitsetBuffer,
    std::shared_ptr<org::Buffer> telemetryBuffer,
    std::shared_ptr<org::Buffer> readBaseCounterBuffer,
    bool readReverse,
    uint32_t visibleClustersCapacity,
    bool runWhenComputeSWRasterEnabledOnly) {
    if (visibleClustersCapacity == 0u) {
        throw std::invalid_argument("RasterBucketHistogramPass requires a non-zero visible-cluster capacity");
    }
    m_visibleClustersBuffer = std::move(visibleClustersBuffer);
    m_visibleClustersCounterBuffer = std::move(visibleClustersCounterBuffer);
    m_histogramIndirectCommand = std::move(histogramIndirectCommand);
    m_histogramBuffer = std::move(histogramBuffer);
    m_reyesOwnershipBitsetBuffer = std::move(reyesOwnershipBitsetBuffer);
    m_telemetryBuffer = std::move(telemetryBuffer);
    m_readBaseCounterBuffer = std::move(readBaseCounterBuffer);
    m_readReverse = readReverse;
    m_visibleClustersCapacity = visibleClustersCapacity;
    m_runWhenComputeSWRasterEnabledOnly = runWhenComputeSWRasterEnabledOnly;
}

RasterBucketHistogramPass::~RasterBucketHistogramPass() = default;

RasterBucketHistogramBindings RasterBucketHistogramPass::Declare(org::PassBuilder& builder) {
    // Sized for the build's bucket count (br::render::BuildUpdateContext) as the first Update would size them: the graph then
    // places them at that size, so a recording of the new graph made ahead (staged, M6 step 9) still binds them at its first
    // run.
    if (const auto* seed = br::render::BuildUpdateContext(); seed && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())))
        SizeBuffers(seed->preparedRasterBucketCount);
    RasterBucketHistogramBindings bindings{
        br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/clodUtil.hlsl",
            L"ClearUintStructuredBufferCSMain", "ClearUintStructuredBufferCSMain")),
        br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/clodUtil.hlsl",
            L"ClusterRasterBucketsHistogramCSMain", "ClusterRasterBucketsHistogramCSMain")),
        br::render::DeclareCommandSignature(br::render::ConstantsDispatchCommandSignature(2,
            sizeof(RasterBucketsHistogramIndirectCommand), "CLod_RasterBucketsHistogram.Signature")),
        builder.ShaderResource(m_visibleClustersBuffer), builder.ShaderResource(m_visibleClustersCounterBuffer),
        builder.IndirectArguments(m_histogramIndirectCommand), builder.UnorderedAccess(m_histogramBuffer)};
    builder.ShaderResource(
            Builtin::PerMeshBuffer, Builtin::CLod::MaterialPalettes, Builtin::CLod::AssemblyMaterialBases,
            Builtin::PerMeshInstanceBuffer,
            Builtin::InstanceDrawRecordBuffer,
            Builtin::PerInstanceTransformBuffer,
            Builtin::PerMaterialDataBuffer,
            Builtin::Material::TextureStreamingMetadataBuffer);
    builder.UnorderedAccess(Builtin::Material::TextureStreamingFeedbackBuffer);
    if (m_reyesOwnershipBitsetBuffer) {
        bindings.reyesOwnership = builder.ShaderResource(m_reyesOwnershipBitsetBuffer);
    }
    if (m_telemetryBuffer) {
        bindings.telemetry = builder.UnorderedAccess(m_telemetryBuffer);
    }
    if (m_readBaseCounterBuffer) {
        bindings.readBaseCounter = builder.ShaderResource(m_readBaseCounterBuffer);
    }
    if (m_sideBandBuffer) {
        bindings.sideBand = builder.ShaderResource(m_sideBandBuffer);
        bindings.hasSideBand = true;
    }

    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    bindings.visibleCapacity = m_visibleClustersCapacity;
    bindings.readReverse = m_readReverse;
    bindings.hasReyesOwnership = static_cast<bool>(m_reyesOwnershipBitsetBuffer);
    bindings.hasTelemetry = static_cast<bool>(m_telemetryBuffer);
    bindings.hasReadBaseCounter = static_cast<bool>(m_readBaseCounterBuffer);
    m_pipelines.Clear();
    m_pipelines.Add(bindings.clearPipeline);
    m_pipelines.Add(bindings.histogramPipeline);
    return bindings;
}

RasterBucketHistogramFrame RasterBucketHistogramPass::Prepare(
    const RasterBucketHistogramBindings&, const org::PassPrepareContext& preparation) const {
    (void)preparation;
    return m_taken;
}

void RasterBucketHistogramPass::TakeFrameValues(const org::UpdateExecutionContext& execution) {
    bool enabled = false;
    const uint32_t numBuckets = TakeRasterBucketCount(execution, m_runWhenComputeSWRasterEnabledOnly, enabled);
    m_taken = {numBuckets, enabled, IsCLodWorkGraphTelemetryEnabled()};
}

void RasterBucketHistogramPass::Revision(std::vector<uint64_t>& out) const {
    m_pipelines.Append(out);
    out.push_back(m_taken.numBuckets | (uint64_t{m_taken.enabled} << 32) | (uint64_t{m_taken.telemetryEnabled} << 33));
}

void RasterBucketHistogramPass::Record(const RasterBucketHistogramBindings& bindings,
    const RasterBucketHistogramFrame& frame, org::PassRecordContext& context) {
    if (!frame.enabled) return;
    br::render::MiscConstants clear{};
    clear[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    clear[CLOD_CLEAR_UINT_BUFFER_COUNT] = frame.numBuckets;
    br::render::DispatchCompute(context, bindings.clearPipeline, clear, (frame.numBuckets + 63u) / 64u);
    context.BufferBarriers({bindings.histogram.Resource()},
        rhi::ResourceAccessType::UnorderedAccess, rhi::ResourceAccessType::UnorderedAccess);

    br::render::MiscConstants c{};
    c[CLOD_HISTOGRAM_READ_BASE_COUNTER_DESCRIPTOR_INDEX] =
        bindings.hasReadBaseCounter ? context.Index(bindings.readBaseCounter) : 0xFFFFFFFFu;
    c[CLOD_HISTOGRAM_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleClusters);
    c[CLOD_HISTOGRAM_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX] = context.Index(bindings.visibleCount);
    c[CLOD_HISTOGRAM_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    c[CLOD_HISTOGRAM_TELEMETRY_DESCRIPTOR_INDEX] =
        bindings.hasTelemetry && frame.telemetryEnabled ? context.Index(bindings.telemetry) : 0xFFFFFFFFu;
    c[CLOD_HISTOGRAM_REYES_OWNERSHIP_BITSET_DESCRIPTOR_INDEX] =
        bindings.hasReyesOwnership ? context.Index(bindings.reyesOwnership) : 0xFFFFFFFFu;
    c[CLOD_HISTOGRAM_NUM_RASTER_BUCKETS] = frame.numBuckets;
    c[CLOD_HISTOGRAM_VISIBLE_CLUSTER_SIDE_BAND_DESCRIPTOR_INDEX] =
        bindings.hasSideBand ? context.Index(bindings.sideBand) : 0xFFFFFFFFu;
    c[CLOD_HISTOGRAM_READ_MODE_FLAGS] = (bindings.readReverse ? CLOD_HISTOGRAM_READ_FLAG_REVERSED : 0u)
        | (bindings.hasReyesOwnership ? CLOD_HISTOGRAM_READ_FLAG_SKIP_REYES_OWNED : 0u);
    c[CLOD_HISTOGRAM_READ_CAPACITY] = bindings.visibleCapacity;
    br::render::DispatchComputeIndirect(context, bindings.histogramPipeline, c, bindings.commandSignature,
        bindings.indirectArguments);
}

void RasterBucketHistogramPass::Update(const org::UpdateExecutionContext& executionContext) {
    m_enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    if (!m_enabled) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;

    SizeBuffers(context.preparedRasterBucketCount);
}

void RasterBucketHistogramPass::SizeBuffers(uint32_t numBuckets) {
    if (m_histogramBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(uint32_t)) {
        m_histogramBuffer->ResizeStructured(numBuckets);
    }
}
