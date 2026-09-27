#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapDeduplicatePredictedPagesPass.h"

#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowDeduplicatePredictedPagesRootConstants.h"
#include "Render/ShaderAPI.h"

VirtualShadowMapDeduplicatePredictedPagesPass::VirtualShadowMapDeduplicatePredictedPagesPass(
    std::shared_ptr<org::Buffer> predictiveRawPagesBuffer,
    std::shared_ptr<org::Buffer> predictiveRawPageCountBuffer,
    std::shared_ptr<org::Buffer> predictedScratchBitsetBuffer,
    std::shared_ptr<org::Buffer> predictedPagesBuffer,
    std::shared_ptr<org::Buffer> predictedPageCountBuffer,
    std::shared_ptr<org::Buffer> statsBuffer,
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> dirtyFlagsBuffer,
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

VirtualShadowMapDeduplicatePredictedPagesBindings VirtualShadowMapDeduplicatePredictedPagesPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    declaration.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {declaration.BindShaderResource(m_predictiveRawPagesBuffer),
        declaration.BindShaderResource(m_predictiveRawPageCountBuffer),
        declaration.BindUnorderedAccess(m_predictedScratchBitsetBuffer),
        declaration.BindUnorderedAccess(m_predictedPagesBuffer),
        declaration.BindUnorderedAccess(m_predictedPageCountBuffer),
        declaration.BindUnorderedAccess(m_statsBuffer), declaration.BindUnorderedAccess(m_pageTableTexture),
        declaration.BindUnorderedAccess(m_pageMetadataBuffer), declaration.BindUnorderedAccess(m_dirtyFlagsBuffer),
        m_physicalPageCount};
}



br::render::PreparedComputePipelineSequence VirtualShadowMapDeduplicatePredictedPagesPass::Prepare(
    const VirtualShadowMapDeduplicatePredictedPagesBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    br::render::PreparedComputePipelineSequence data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.steps.resize(2);
    auto clear = preparation.CaptureProgramBinding(m_clearStatePso);
    data.steps[0].program = clear.program;
    data.steps[0].descriptorIndices = std::move(clear.descriptorIndices);
    auto deduplicate = preparation.CaptureProgramBinding(m_deduplicatePso);
    data.steps[1].program = deduplicate.program;
    data.steps[1].descriptorIndices = std::move(deduplicate.descriptorIndices);
    auto& c = data.steps[0].constants;
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGES_DESCRIPTOR_INDEX] = srv(bindings.rawPages);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGE_COUNT_DESCRIPTOR_INDEX] = srv(bindings.rawCount);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_SCRATCH_BITSET_DESCRIPTOR_INDEX] = uav(bindings.scratch);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGES_DESCRIPTOR_INDEX] = uav(bindings.pages);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGE_COUNT_DESCRIPTOR_INDEX] = uav(bindings.pageCount);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_METADATA_DESCRIPTOR_INDEX] = uav(bindings.pageMetadata);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_DIRTY_FLAGS_DESCRIPTOR_INDEX] = uav(bindings.dirtyFlags);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PHYSICAL_PAGE_COUNT] = bindings.physicalPageCount;
    data.steps[0].groupsX = (CLodVirtualShadowFallbackDependencyHashCapacity + 63u) / 64u;
    data.steps[1].groupsX = (CLodVirtualShadowPredictiveRawPageCapacity + 63u) / 64u;
    data.steps[1].constants = c;
    data.steps[0].uavBarrierAfter = data.steps[1].uavBarrierAfter = true;
    return data;
}

void VirtualShadowMapDeduplicatePredictedPagesPass::Record(const VirtualShadowMapDeduplicatePredictedPagesBindings&,
    const br::render::PreparedComputePipelineSequence& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputePipelineSequence(data, recording);
}
