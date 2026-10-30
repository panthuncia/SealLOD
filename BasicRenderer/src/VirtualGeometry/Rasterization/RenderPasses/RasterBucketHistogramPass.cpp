#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketHistogramPass.h"
#include "Render/PassBuilders.h"

#include <stdexcept>
#include <vector>

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
#include "../shaders/PerPassRootConstants/clodHistogramRootConstants.h"

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
    CreatePipelines(
        DeviceManager::GetInstance().GetDevice(),
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_histogramPipeline,
        m_clearPipeline);

    rhi::IndirectArg rasterizeClustersArgs[] = {
        {.kind = rhi::IndirectArgKind::Constant, .u = {.rootConstants = { IndirectCommandSignatureRootSignatureIndex, 0, 2 } } },
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    rhi::CommandSignaturePtr histogramCommandSignature;
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(rasterizeClustersArgs, 2), sizeof(RasterBucketsHistogramIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(), histogramCommandSignature);
    m_histogramCommandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(histogramCommandSignature));

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
    RasterBucketHistogramBindings bindings{m_clearPipeline, m_histogramPipeline, m_histogramCommandSignature,
        builder.ShaderResource(m_visibleClustersBuffer), builder.ShaderResource(m_visibleClustersCounterBuffer),
        builder.IndirectArguments(m_histogramIndirectCommand), builder.UnorderedAccess(m_histogramBuffer)};
    builder.ShaderResource(
            Builtin::PerMeshBuffer,
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

    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    bindings.visibleCapacity = m_visibleClustersCapacity;
    bindings.readReverse = m_readReverse;
    bindings.hasReyesOwnership = static_cast<bool>(m_reyesOwnershipBitsetBuffer);
    bindings.hasTelemetry = static_cast<bool>(m_telemetryBuffer);
    bindings.hasReadBaseCounter = static_cast<bool>(m_readBaseCounterBuffer);
    return bindings;
}

RasterBucketHistogramFrame RasterBucketHistogramPass::Prepare(
    const RasterBucketHistogramBindings&, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const uint32_t numBuckets = context->preparedRasterBucketCount;
    const bool enabled = numBuckets != 0u && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()));
    return {numBuckets, enabled, IsCLodWorkGraphTelemetryEnabled()};
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

    const uint32_t numBuckets = context.preparedRasterBucketCount;

    if (m_histogramBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(uint32_t)) {
        m_histogramBuffer->ResizeStructured(numBuckets);
    }

}

void RasterBucketHistogramPass::CreatePipelines(
    rhi::Device device,
    rhi::PipelineLayoutHandle globalRootSignature,
    org::PipelineState& outHistogramPipeline,
    org::PipelineState& outClearPipeline)
{
    (void)device;
    outHistogramPipeline = PSOManager::GetInstance().MakeComputePipeline(
        globalRootSignature,
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClusterRasterBucketsHistogramCSMain");
    outClearPipeline = PSOManager::GetInstance().MakeComputePipeline(
        globalRootSignature,
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain");
}
