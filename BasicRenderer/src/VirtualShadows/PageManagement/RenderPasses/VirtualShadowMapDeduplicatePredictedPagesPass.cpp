#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapDeduplicatePredictedPagesPass.h"
#include "Render/PassBuilders.h"

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
    declaration.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_clearStatePso, m_deduplicatePso, declaration.ShaderResource(m_predictiveRawPagesBuffer),
        declaration.ShaderResource(m_predictiveRawPageCountBuffer),
        declaration.UnorderedAccess(m_predictedScratchBitsetBuffer),
        declaration.UnorderedAccess(m_predictedPagesBuffer),
        declaration.UnorderedAccess(m_predictedPageCountBuffer),
        declaration.UnorderedAccess(m_statsBuffer), declaration.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}),
        declaration.UnorderedAccess(m_pageMetadataBuffer), declaration.UnorderedAccess(m_dirtyFlagsBuffer),
        m_physicalPageCount};
}

void VirtualShadowMapDeduplicatePredictedPagesPass::Record(const VirtualShadowMapDeduplicatePredictedPagesBindings& bindings,
    org::PassRecordContext& recording) {
    br::render::MiscConstants c{};
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.rawPages);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGE_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.rawCount);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_SCRATCH_BITSET_DESCRIPTOR_INDEX] = recording.Index(bindings.scratch);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.pages);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGE_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.pageCount);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_DIRTY_FLAGS_DESCRIPTOR_INDEX] = recording.Index(bindings.dirtyFlags);
    c[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PHYSICAL_PAGE_COUNT] = bindings.physicalPageCount;
    br::render::DispatchCompute(recording, bindings.clearStatePipeline, c, (CLodVirtualShadowFallbackDependencyHashCapacity + 63u) / 64u);
    recording.UavBarrier();
    br::render::DispatchCompute(recording, bindings.deduplicatePipeline, c, (CLodVirtualShadowPredictiveRawPageCapacity + 63u) / 64u);
    recording.UavBarrier();
}
