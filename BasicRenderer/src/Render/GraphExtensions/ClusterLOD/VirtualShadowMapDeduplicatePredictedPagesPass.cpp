#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapDeduplicatePredictedPagesPass.h"

#include "BuiltinResources.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowDeduplicatePredictedPagesRootConstants.h"

VirtualShadowMapDeduplicatePredictedPagesPass::VirtualShadowMapDeduplicatePredictedPagesPass(
    std::shared_ptr<Buffer> predictiveRawPagesBuffer,
    std::shared_ptr<Buffer> predictiveRawPageCountBuffer,
    std::shared_ptr<Buffer> predictedScratchBitsetBuffer,
    std::shared_ptr<Buffer> predictedPagesBuffer,
    std::shared_ptr<Buffer> predictedPageCountBuffer,
    std::shared_ptr<Buffer> statsBuffer,
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> dirtyFlagsBuffer,
    uint32_t physicalPageCount)
    : m_predictiveRawPagesBuffer(std::move(predictiveRawPagesBuffer))
    , m_predictiveRawPageCountBuffer(std::move(predictiveRawPageCountBuffer))
    , m_predictedScratchBitsetBuffer(std::move(predictedScratchBitsetBuffer))
    , m_predictedPagesBuffer(std::move(predictedPagesBuffer))
    , m_predictedPageCountBuffer(std::move(predictedPageCountBuffer))
    , m_statsBuffer(std::move(statsBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_dirtyFlagsBuffer(std::move(dirtyFlagsBuffer))
    , m_physicalPageCount(physicalPageCount)
{
    m_clearStatePso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowClearPredictedPageDedupStateCSMain",
        {},
        "CLod.VirtualShadow.ClearPredictedPageDedupState.PSO");

    m_deduplicatePso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowDeduplicatePredictedPagesCSMain",
        {},
        "CLod.VirtualShadow.DeduplicatePredictedPages.PSO");
}

void VirtualShadowMapDeduplicatePredictedPagesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_predictiveRawPagesBuffer,
            m_predictiveRawPageCountBuffer)
        .WithConstantBuffer(Builtin::PerFrameBuffer)
        .WithUnorderedAccess(
            m_predictedScratchBitsetBuffer,
            m_predictedPagesBuffer,
            m_predictedPageCountBuffer,
            m_statsBuffer,
            m_pageTableTexture,
            m_pageMetadataBuffer,
            m_dirtyFlagsBuffer);
}

void VirtualShadowMapDeduplicatePredictedPagesPass::Setup() {}

PassReturn VirtualShadowMapDeduplicatePredictedPagesPass::Execute(PassExecutionContext& executionContext)
{
    auto& commandList = executionContext.commandList;

    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    BindResourceDescriptorIndices(commandList, m_clearStatePso.GetResourceDescriptorSlots());

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGES_DESCRIPTOR_INDEX] = m_predictiveRawPagesBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGE_COUNT_DESCRIPTOR_INDEX] = m_predictiveRawPageCountBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_SCRATCH_BITSET_DESCRIPTOR_INDEX] = m_predictedScratchBitsetBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGES_DESCRIPTOR_INDEX] = m_predictedPagesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGE_COUNT_DESCRIPTOR_INDEX] = m_predictedPageCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_STATS_DESCRIPTOR_INDEX] = m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_TABLE_DESCRIPTOR_INDEX] =
        m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_METADATA_DESCRIPTOR_INDEX] =
        m_pageMetadataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_DIRTY_FLAGS_DESCRIPTOR_INDEX] =
        m_dirtyFlagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PHYSICAL_PAGE_COUNT] =
        m_physicalPageCount;

    commandList.BindPipeline(m_clearStatePso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    constexpr uint32_t kThreadsPerGroup = 64u;
    commandList.Dispatch((CLodVirtualShadowFallbackDependencyHashCapacity + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1u, 1u);

    rhi::GlobalBarrier globalBarrier{};
    globalBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
    globalBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;
    globalBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    globalBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    rhi::BarrierBatch barrierBatch{};
    barrierBatch.globals = rhi::Span<rhi::GlobalBarrier>(&globalBarrier, 1);
    commandList.Barriers(barrierBatch);

    BindResourceDescriptorIndices(commandList, m_deduplicatePso.GetResourceDescriptorSlots());
    commandList.BindPipeline(m_deduplicatePso.GetAPIPipelineState().GetHandle());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);
    commandList.Dispatch((CLodVirtualShadowPredictiveRawPageCapacity + kThreadsPerGroup - 1u) / kThreadsPerGroup, 1u, 1u);
    commandList.Barriers(barrierBatch);

    return {};
}

void VirtualShadowMapDeduplicatePredictedPagesPass::Cleanup() {}
