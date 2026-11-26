#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketBlockOffsetsPass.h"
#include "Render/PassBuilders.h"

#include "Materials/MaterialManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodPrefixOffsetsRootConstants.h"

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

RasterBucketBlockOffsetsPass::RasterBucketBlockOffsetsPass(
    std::shared_ptr<org::Buffer> offsetsBuffer,
    std::shared_ptr<org::Buffer> blockSumsBuffer,
    std::shared_ptr<org::Buffer> scannedBlockSumsBuffer,
    std::shared_ptr<org::Buffer> totalCountBuffer,
    bool runWhenComputeSWRasterEnabledOnly)
    : m_offsetsBuffer(std::move(offsetsBuffer))
    , m_blockSumsBuffer(std::move(blockSumsBuffer))
    , m_scannedBlockSumsBuffer(std::move(scannedBlockSumsBuffer))
    , m_totalCountBuffer(std::move(totalCountBuffer))
    , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly) {
}

RasterBucketBlockOffsetsBindings RasterBucketBlockOffsetsPass::Declare(org::PassBuilder& builder) {
    // Sized for the build's bucket count (br::render::BuildUpdateContext) as the first Update would size them: the graph then
    // places them at that size, so a recording of the new graph made ahead (staged, M6 step 9) still binds them at its first
    // run.
    if (const auto* seed = br::render::BuildUpdateContext(); seed && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)())))
        SizeBuffers(seed->preparedRasterBucketCount);
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    RasterBucketBlockOffsetsBindings bindings{br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/clodUtil.hlsl",
                L"RasterBucketsBlockOffsetsCS", "CLod_RasterBucketsBlockOffsetsPSO")),
        builder.UnorderedAccess(m_offsetsBuffer), builder.ShaderResource(m_blockSumsBuffer),
        builder.UnorderedAccess(m_scannedBlockSumsBuffer), builder.UnorderedAccess(m_totalCountBuffer)};
    m_pipelines.Clear();
    m_pipelines.Add(bindings.pipeline);
    return bindings;
}

RasterBucketBlockOffsetsFrame RasterBucketBlockOffsetsPass::Prepare(
    const RasterBucketBlockOffsetsBindings&, const org::PassPrepareContext& preparation) const {
    (void)preparation;
    return m_taken;
}

void RasterBucketBlockOffsetsPass::TakeFrameValues(const org::UpdateExecutionContext& execution) {
    bool enabled = false;
    const uint32_t numBuckets = TakeRasterBucketCount(execution, m_runWhenComputeSWRasterEnabledOnly, enabled);
    m_taken = {numBuckets, (numBuckets + m_blockSize - 1u) / m_blockSize, enabled ? 1u : 0u};
}

void RasterBucketBlockOffsetsPass::Revision(std::vector<uint64_t>& out) const {
    m_pipelines.Append(out);
    out.push_back(m_taken.numBuckets | (uint64_t{m_taken.numBlocks} << 32));
    out.push_back(m_taken.groups);
}

void RasterBucketBlockOffsetsPass::Record(const RasterBucketBlockOffsetsBindings& bindings,
    const RasterBucketBlockOffsetsFrame& frame, org::PassRecordContext& context) {
    br::render::MiscConstants constants{};
    constants[CLOD_PREFIX_OFFSETS_NUM_BUCKETS] = frame.numBuckets;
    constants[CLOD_PREFIX_OFFSETS_NUM_BLOCKS] = frame.numBlocks;
    constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX] = context.Index(bindings.offsets);
    constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_BLOCK_SUMS_DESCRIPTOR_INDEX] = context.Index(bindings.blockSums);
    constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_SCANNED_BLOCK_SUMS_DESCRIPTOR_INDEX] = context.Index(bindings.scannedBlockSums);
    constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_TOTAL_COUNT_DESCRIPTOR_INDEX] = context.Index(bindings.totalCount);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groups);
}

void RasterBucketBlockOffsetsPass::Update(const org::UpdateExecutionContext& executionContext) {
    m_enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    if (!m_enabled) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    SizeBuffers(context.preparedRasterBucketCount);
}

void RasterBucketBlockOffsetsPass::SizeBuffers(uint32_t numBuckets) {
    const uint32_t numBlocks = (numBuckets + m_blockSize - 1) / m_blockSize;
    if (m_scannedBlockSumsBuffer->GetSize() < static_cast<size_t>(numBlocks) * sizeof(uint32_t)) {
        m_scannedBlockSumsBuffer->ResizeStructured(numBlocks);
    }
}

