#include "Render/GraphExtensions/ClusterLOD/RasterBucketBlockOffsetsPass.h"

#include "Managers/MaterialManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodPrefixOffsetsRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"

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
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.BindUnorderedAccess(m_offsetsBuffer), builder.BindShaderResource(m_blockSumsBuffer),
        builder.BindUnorderedAccess(m_scannedBlockSumsBuffer), builder.BindUnorderedAccess(m_totalCountBuffer),
        m_numBuckets, m_enabled};
}


br::render::PreparedComputeDispatch RasterBucketBlockOffsetsPass::Prepare(
    const RasterBucketBlockOffsetsBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const uint32_t numBuckets = context->preparedRasterBucketCount;
    const bool enabled = numBuckets != 0u && (!m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)()));
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    data.program = preparation.CaptureProgram(m_pso);
    data.descriptorIndices = CaptureResourceDescriptorIndices(m_pso.GetResourceDescriptorSlots());
    data.constants[CLOD_PREFIX_OFFSETS_NUM_BUCKETS] = numBuckets;
    data.constants[CLOD_PREFIX_OFFSETS_NUM_BLOCKS] = (numBuckets + m_blockSize - 1u) / m_blockSize;
    data.constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.offsets, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_BLOCK_SUMS_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.blockSums, {org::BindlessViewKind::ShaderResource}).index;
    data.constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_SCANNED_BLOCK_SUMS_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.scannedBlockSums, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_TOTAL_COUNT_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.totalCount, {org::BindlessViewKind::UnorderedAccess}).index;
    data.groupsX = enabled ? 1u : 0u;
    return data;
}

void RasterBucketBlockOffsetsPass::Update(const org::UpdateExecutionContext& executionContext) {
    m_enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    if (!m_enabled) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    m_numBuckets = context.preparedRasterBucketCount;
    const uint32_t numBlocks = (m_numBuckets + m_blockSize - 1) / m_blockSize;

    if (m_scannedBlockSumsBuffer->GetSize() < static_cast<size_t>(numBlocks) * sizeof(uint32_t)) {
        m_scannedBlockSumsBuffer->ResizeStructured(numBlocks);
    }
}

