#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapExpandPredictedPagesPass.h"

#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowExpandPredictedPagesRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

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
    return {builder.UnorderedAccess(m_predictiveCandidatesBuffer),
        builder.UnorderedAccess(m_predictiveCandidateCountBuffer), builder.UnorderedAccess(m_predictiveRawPagesBuffer),
        builder.UnorderedAccess(m_predictiveRawPageCountBuffer), builder.ShaderResource(m_clipmapInfoBuffer),
        builder.UnorderedAccess(m_scratchBitsetBuffer), builder.UnorderedAccess(m_statsBuffer),
        builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}), builder.UnorderedAccess(m_pageMetadataBuffer),
        builder.UnorderedAccess(m_pageViewInfoBuffer), m_physicalPageCount};
}

br::render::PreparedComputePipelineSequence VirtualShadowMapExpandPredictedPagesPass::Prepare(
    const VirtualShadowMapExpandPredictedPagesBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputePipelineSequence data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    std::array<unsigned int, NumMiscUintRootConstants> constants{};
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATES_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.candidates).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATE_COUNT_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.candidateCount).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGES_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.rawPages).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGE_COUNT_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.rawCount).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_INFO_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.clipmapInfo).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_SCRATCH_BITSET_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.scratch).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_STATS_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.stats).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_TABLE_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.pageTable).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_METADATA_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.pageMetadata).index;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PHYSICAL_PAGE_COUNT] = bindings.physicalPageCount;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    constants[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.pageViewInfo).index;
    const auto append = [&](const org::PipelineState& pso, uint32_t groups, bool barrierBefore) {
        br::render::PreparedComputePipelineSequence::Step step{};
        auto program = preparation.CaptureProgramBinding(pso);
        step.program = program.program;
        step.descriptorIndices = std::move(program.descriptorIndices);
        step.constants = constants;
        step.groupsX = groups;
        step.uavBarrierBefore = barrierBefore;
        data.steps.push_back(std::move(step));
    };
    append(m_stampContentGenerationPso, (bindings.physicalPageCount + 63u) / 64u, false);
    append(m_pso, (CLodVirtualShadowPredictiveCandidateCapacity + 63u) / 64u, true);
    append(m_resetCandidateCountPso, 1u, true);
    return data;
}

void VirtualShadowMapExpandPredictedPagesPass::Record(const VirtualShadowMapExpandPredictedPagesBindings&,
    const br::render::PreparedComputePipelineSequence& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputePipelineSequence(data, recording);
}
