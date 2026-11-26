#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketBlockScanPass.h"
#include "Render/PassBuilders.h"

#include "Materials/MaterialManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodPrefixScanRootConstants.h"

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

RasterBucketBlockScanPass::RasterBucketBlockScanPass(
    std::shared_ptr<org::Buffer> histogramBuffer,
    std::shared_ptr<org::Buffer> offsetsBuffer,
    std::shared_ptr<org::Buffer> blockSumsBuffer,
    bool runWhenComputeSWRasterEnabledOnly)
    : m_histogramBuffer(std::move(histogramBuffer))
    , m_offsetsBuffer(std::move(offsetsBuffer))
    , m_blockSumsBuffer(std::move(blockSumsBuffer))
    , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly) {
}

RasterBucketBlockScanBindings RasterBucketBlockScanPass::Declare(org::PassBuilder& builder) {
    // Sized for the build's bucket count (br::render::BuildUpdateContext) as the first Update would size them: the graph then
    // places them at that size, so a recording of the new graph made ahead (staged, M6 step 9) still binds them at its first
    // run.
    if (const auto* seed = br::render::BuildUpdateContext(); seed && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())))
        SizeBuffers(seed->preparedRasterBucketCount);
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    RasterBucketBlockScanBindings bindings{br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/clodUtil.hlsl",
                L"RasterBucketsBlockScanCS", "CLod_RasterBucketsBlockScanPSO")),
        builder.ShaderResource(m_histogramBuffer), builder.UnorderedAccess(m_offsetsBuffer),
        builder.UnorderedAccess(m_blockSumsBuffer)};
    m_pipelines.Clear();
    m_pipelines.Add(bindings.pipeline);
    return bindings;
}

RasterBucketBlockScanFrame RasterBucketBlockScanPass::Prepare(
    const RasterBucketBlockScanBindings&, const org::PassPrepareContext& preparation) const {
    (void)preparation;
    return m_taken;
}

void RasterBucketBlockScanPass::TakeFrameValues(const org::UpdateExecutionContext& execution) {
    bool enabled = false;
    const uint32_t numBuckets = TakeRasterBucketCount(execution, m_runWhenComputeSWRasterEnabledOnly, enabled);
    m_taken = {numBuckets, enabled ? (numBuckets + m_blockSize - 1u) / m_blockSize : 0u};
}

void RasterBucketBlockScanPass::Revision(std::vector<uint64_t>& out) const {
    m_pipelines.Append(out);
    out.push_back(m_taken.numBuckets | (uint64_t{m_taken.groups} << 32));
}

void RasterBucketBlockScanPass::Record(const RasterBucketBlockScanBindings& bindings,
    const RasterBucketBlockScanFrame& frame, org::PassRecordContext& context) {
    br::render::MiscConstants constants{};
    constants[UintRootConstant0] = frame.numBuckets;
    constants[CLOD_PREFIX_SCAN_NUM_BUCKETS] = frame.numBuckets;
    constants[CLOD_PREFIX_SCAN_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    constants[CLOD_PREFIX_SCAN_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX] = context.Index(bindings.offsets);
    constants[CLOD_PREFIX_SCAN_RASTER_BUCKETS_BLOCK_SUMS_DESCRIPTOR_INDEX] = context.Index(bindings.blockSums);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groups);
}

void RasterBucketBlockScanPass::Update(const org::UpdateExecutionContext& executionContext) {
    m_enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    if (!m_enabled) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    SizeBuffers(context.preparedRasterBucketCount);
}

void RasterBucketBlockScanPass::SizeBuffers(uint32_t numBuckets) {
    const uint32_t numBlocks = (numBuckets + m_blockSize - 1) / m_blockSize;
    if (m_offsetsBuffer->GetSize() < static_cast<size_t>(numBuckets) * sizeof(uint32_t)) {
        m_offsetsBuffer->ResizeStructured(numBuckets);
    }
    if (m_blockSumsBuffer->GetSize() < static_cast<size_t>(numBlocks) * sizeof(uint32_t)) {
        m_blockSumsBuffer->ResizeStructured(numBlocks);
    }
}

