#include "Render/GraphExtensions/ClusterLOD/RasterBucketBlockScanPass.h"

#include "Managers/MaterialManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "../shaders/PerPassRootConstants/clodPrefixScanRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"

RasterBucketBlockScanPass::RasterBucketBlockScanPass(
    std::shared_ptr<org::Buffer> histogramBuffer,
    std::shared_ptr<org::Buffer> offsetsBuffer,
    std::shared_ptr<org::Buffer> blockSumsBuffer,
    bool runWhenComputeSWRasterEnabledOnly)
    : m_histogramBuffer(std::move(histogramBuffer))
    , m_offsetsBuffer(std::move(offsetsBuffer))
    , m_blockSumsBuffer(std::move(blockSumsBuffer))
    , m_runWhenComputeSWRasterEnabledOnly(runWhenComputeSWRasterEnabledOnly) {
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"RasterBucketsBlockScanCS",
        {},
        "CLod_RasterBucketsBlockScanPSO");
}

RasterBucketBlockScanBindings RasterBucketBlockScanPass::Declare(org::PassBuilder& builder) {
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.BindShaderResource(m_histogramBuffer), builder.BindUnorderedAccess(m_offsetsBuffer),
        builder.BindUnorderedAccess(m_blockSumsBuffer), m_numBuckets, m_enabled};
}


br::render::PreparedComputeDispatch RasterBucketBlockScanPass::Prepare(
    const RasterBucketBlockScanBindings& bindings, const org::PassPrepareContext& preparation) const {
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
    data.constants[UintRootConstant0] = numBuckets;
    data.constants[CLOD_PREFIX_SCAN_NUM_BUCKETS] = numBuckets;
    data.constants[CLOD_PREFIX_SCAN_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.histogram, {org::BindlessViewKind::ShaderResource}).index;
    data.constants[CLOD_PREFIX_SCAN_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.offsets, {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_PREFIX_SCAN_RASTER_BUCKETS_BLOCK_SUMS_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.blockSums, {org::BindlessViewKind::UnorderedAccess}).index;
    data.groupsX = enabled ? (numBuckets + m_blockSize - 1u) / m_blockSize : 0u;
    return data;
}

void RasterBucketBlockScanPass::Update(const org::UpdateExecutionContext& executionContext) {
    m_enabled = !m_runWhenComputeSWRasterEnabledOnly ||
        CLodSoftwareRasterUsesCompute(SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName)());
    if (!m_enabled) {
        return;
    }

    auto* updateContext = executionContext.hostData->Get<UpdateContext>();
    auto& context = *updateContext;
    m_numBuckets = context.preparedRasterBucketCount;
    const uint32_t numBlocks = (m_numBuckets + m_blockSize - 1) / m_blockSize;

    if (m_offsetsBuffer->GetSize() < static_cast<size_t>(m_numBuckets) * sizeof(uint32_t)) {
        m_offsetsBuffer->ResizeStructured(m_numBuckets);
    }
    if (m_blockSumsBuffer->GetSize() < static_cast<size_t>(numBlocks) * sizeof(uint32_t)) {
        m_blockSumsBuffer->ResizeStructured(numBlocks);
    }
}

