#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapExpandPredictedPagesPass.h"

#include "BuiltinResources.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowExpandPredictedPagesRootConstants.h"

VirtualShadowMapExpandPredictedPagesPass::VirtualShadowMapExpandPredictedPagesPass(
    std::shared_ptr<Buffer> predictiveCandidatesBuffer,
    std::shared_ptr<Buffer> predictiveCandidateCountBuffer,
    std::shared_ptr<Buffer> predictiveRawPagesBuffer,
    std::shared_ptr<Buffer> predictiveRawPageCountBuffer,
    std::shared_ptr<Buffer> clipmapInfoBuffer,
    std::shared_ptr<Buffer> scratchBitsetBuffer,
    std::shared_ptr<Buffer> statsBuffer,
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> pageViewInfoBuffer,
    uint32_t physicalPageCount)
    : m_predictiveCandidatesBuffer(std::move(predictiveCandidatesBuffer))
    , m_predictiveCandidateCountBuffer(std::move(predictiveCandidateCountBuffer))
    , m_predictiveRawPagesBuffer(std::move(predictiveRawPagesBuffer))
    , m_predictiveRawPageCountBuffer(std::move(predictiveRawPageCountBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_scratchBitsetBuffer(std::move(scratchBitsetBuffer))
    , m_statsBuffer(std::move(statsBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_pageViewInfoBuffer(std::move(pageViewInfoBuffer))
    , m_physicalPageCount(physicalPageCount)
{
    m_stampContentGenerationPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowStampRenderedPageGenerationsCSMain",
        {},
        "CLod.VirtualShadow.StampRenderedPageGenerations.PSO");
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowExpandPredictedPagesCSMain",
        {},
        "CLod.VirtualShadow.ExpandPredictedPages.PSO");
    m_resetCandidateCountPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowResetFallbackCandidateCountCSMain",
        {},
        "CLod.VirtualShadow.ResetFallbackCandidateCount.PSO");
}

void VirtualShadowMapExpandPredictedPagesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            Builtin::Shadows::CLodCompactShadowCameras,
            Builtin::CameraBuffer,
            m_clipmapInfoBuffer)
        .WithUnorderedAccess(
            m_predictiveCandidatesBuffer,
            m_predictiveCandidateCountBuffer,
            m_predictiveRawPagesBuffer,
            m_predictiveRawPageCountBuffer,
            m_scratchBitsetBuffer,
            m_statsBuffer,
            m_pageTableTexture,
            m_pageMetadataBuffer,
            m_pageViewInfoBuffer);

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void VirtualShadowMapExpandPredictedPagesPass::Setup() {}

PassReturn VirtualShadowMapExpandPredictedPagesPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATES_DESCRIPTOR_INDEX] = m_predictiveCandidatesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATE_COUNT_DESCRIPTOR_INDEX] = m_predictiveCandidateCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGES_DESCRIPTOR_INDEX] = m_predictiveRawPagesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGE_COUNT_DESCRIPTOR_INDEX] = m_predictiveRawPageCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_clipmapInfoBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_SCRATCH_BITSET_DESCRIPTOR_INDEX] = m_scratchBitsetBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_STATS_DESCRIPTOR_INDEX] = m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_TABLE_DESCRIPTOR_INDEX] =
        m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_METADATA_DESCRIPTOR_INDEX] =
        m_pageMetadataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PHYSICAL_PAGE_COUNT] = m_physicalPageCount;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_COUNT] =
        CLodVirtualShadowMaxSupportedClipmapCount;
    rootConstants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] =
        m_pageViewInfoBuffer->GetUAVShaderVisibleInfo(0).slot.index;

    commandList.BindPipeline(m_stampContentGenerationPso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_stampContentGenerationPso.GetResourceDescriptorSlots());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);
    commandList.Dispatch((m_physicalPageCount + 63u) / 64u, 1u, 1u);

    rhi::GlobalBarrier globalBarrier{};
    globalBarrier.beforeSync = rhi::ResourceSyncState::ComputeShading;
    globalBarrier.afterSync = rhi::ResourceSyncState::ComputeShading;
    globalBarrier.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    globalBarrier.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    rhi::BarrierBatch barrierBatch{};
    barrierBatch.globals = rhi::Span<rhi::GlobalBarrier>(&globalBarrier, 1);
    commandList.Barriers(barrierBatch);

    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    commandList.Dispatch(
        (CLodVirtualShadowPredictiveCandidateCapacity + 63u) / 64u,
        1u,
        1u);
    commandList.Barriers(barrierBatch);

    commandList.BindPipeline(
        m_resetCandidateCountPso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(
        commandList,
        m_resetCandidateCountPso.GetResourceDescriptorSlots());
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);
    commandList.Dispatch(1u, 1u, 1u);
    return {};
}

void VirtualShadowMapExpandPredictedPagesPass::Cleanup() {}
