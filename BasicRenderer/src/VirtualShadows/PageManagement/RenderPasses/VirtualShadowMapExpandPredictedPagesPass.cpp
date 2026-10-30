#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapExpandPredictedPagesPass.h"
#include "Render/PassBuilders.h"

#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowExpandPredictedPagesRootConstants.h"

VirtualShadowMapExpandPredictedPagesPass::VirtualShadowMapExpandPredictedPagesPass(
    std::shared_ptr<org::Buffer> predictiveCandidatesBuffer,
    std::shared_ptr<org::Buffer> predictiveCandidateCountBuffer,
    std::shared_ptr<org::Buffer> predictiveRawPagesBuffer,
    std::shared_ptr<org::Buffer> predictiveRawPageCountBuffer,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::Buffer> scratchBitsetBuffer,
    std::shared_ptr<org::Buffer> statsBuffer,
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> pageViewInfoBuffer,
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
    spdlog::info("VirtualShadowMapExpandPredictedPagesPass: stamp pipeline begin");
    m_stampContentGenerationPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowStampRenderedPageGenerationsCSMain",
        {},
        "CLod.VirtualShadow.StampRenderedPageGenerations.PSO");
    spdlog::info("VirtualShadowMapExpandPredictedPagesPass: stamp pipeline complete; expand pipeline begin");
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowExpandPredictedPagesCSMain",
        {},
        "CLod.VirtualShadow.ExpandPredictedPages.PSO");
    spdlog::info("VirtualShadowMapExpandPredictedPagesPass: expand pipeline complete; reset pipeline begin");
    m_resetCandidateCountPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowResetFallbackCandidateCountCSMain",
        {},
        "CLod.VirtualShadow.ResetFallbackCandidateCount.PSO");
    spdlog::info("VirtualShadowMapExpandPredictedPagesPass: reset pipeline complete");
}

VirtualShadowMapExpandPredictedPagesBindings VirtualShadowMapExpandPredictedPagesPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.ShaderResource(
            Builtin::Shadows::CLodCompactShadowCameras,
            Builtin::CameraBuffer);

    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_stampContentGenerationPso, m_pso, m_resetCandidateCountPso, builder.UnorderedAccess(m_predictiveCandidatesBuffer),
        builder.UnorderedAccess(m_predictiveCandidateCountBuffer), builder.UnorderedAccess(m_predictiveRawPagesBuffer),
        builder.UnorderedAccess(m_predictiveRawPageCountBuffer), builder.ShaderResource(m_clipmapInfoBuffer),
        builder.UnorderedAccess(m_scratchBitsetBuffer), builder.UnorderedAccess(m_statsBuffer),
        builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}), builder.UnorderedAccess(m_pageMetadataBuffer),
        builder.UnorderedAccess(m_pageViewInfoBuffer), m_physicalPageCount};
}

void VirtualShadowMapExpandPredictedPagesPass::Record(const VirtualShadowMapExpandPredictedPagesBindings& bindings,
    org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATES_DESCRIPTOR_INDEX] = recording.Index(bindings.candidates);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATE_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.candidateCount);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.rawPages);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGE_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.rawCount);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_SCRATCH_BITSET_DESCRIPTOR_INDEX] = recording.Index(bindings.scratch);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PHYSICAL_PAGE_COUNT] = bindings.physicalPageCount;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.pageViewInfo);
    br::render::DispatchCompute(recording, bindings.stampContentGenerationPipeline, constants, (bindings.physicalPageCount + 63u) / 64u);
    recording.UavBarrier();
    br::render::DispatchCompute(recording, bindings.expandPipeline, constants, (CLodVirtualShadowPredictiveCandidateCapacity + 63u) / 64u);
    recording.UavBarrier();
    br::render::DispatchCompute(recording, bindings.resetCandidateCountPipeline, constants, 1u);
}
