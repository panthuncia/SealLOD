#include "VirtualGeometry/Rasterization/RenderPasses/RasterBucketBlockOffsetsPass.h"
#include "Render/PassBuilders.h"

#include "Materials/MaterialManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodPrefixOffsetsRootConstants.h"

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
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"RasterBucketsBlockOffsetsCS",
        {},
        "CLod_RasterBucketsBlockOffsetsPSO");
}

RasterBucketBlockOffsetsBindings RasterBucketBlockOffsetsPass::Declare(org::PassBuilder& builder) {
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, builder.UnorderedAccess(m_offsetsBuffer), builder.ShaderResource(m_blockSumsBuffer),
        builder.UnorderedAccess(m_scannedBlockSumsBuffer), builder.UnorderedAccess(m_totalCountBuffer)};
}

RasterBucketBlockOffsetsFrame RasterBucketBlockOffsetsPass::Prepare(
    const RasterBucketBlockOffsetsBindings&, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const uint32_t numBuckets = context->preparedRasterBucketCount;
    const bool enabled = numBuckets != 0u && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()));
    return {numBuckets, (numBuckets + m_blockSize - 1u) / m_blockSize, enabled ? 1u : 0u};
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
    const uint32_t numBuckets = context.preparedRasterBucketCount;
    const uint32_t numBlocks = (numBuckets + m_blockSize - 1) / m_blockSize;

    if (m_scannedBlockSumsBuffer->GetSize() < static_cast<size_t>(numBlocks) * sizeof(uint32_t)) {
        m_scannedBlockSumsBuffer->ResizeStructured(numBlocks);
    }
}

