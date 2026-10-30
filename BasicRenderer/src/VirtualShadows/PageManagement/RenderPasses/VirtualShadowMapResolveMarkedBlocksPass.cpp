#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapResolveMarkedBlocksPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowResolveMarkedBlocksRootConstants.h"

VirtualShadowMapResolveMarkedBlocksPass::VirtualShadowMapResolveMarkedBlocksPass(
    std::shared_ptr<org::Buffer> markedBlocksMaskBuffer,
    std::shared_ptr<org::Buffer> markedBlocksListBuffer,
    std::shared_ptr<org::Buffer> markedBlocksCountBuffer,
    std::shared_ptr<org::Buffer> allocationRequestsBuffer,
    std::shared_ptr<org::Buffer> allocationCountBuffer,
    std::shared_ptr<org::Buffer> markClipmapDataBuffer,
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<org::Buffer> directionalPageViewInfoBuffer,
    std::shared_ptr<org::Buffer> statsBuffer)
    : m_markedBlocksMaskBuffer(std::move(markedBlocksMaskBuffer))
    , m_markedBlocksListBuffer(std::move(markedBlocksListBuffer))
    , m_markedBlocksCountBuffer(std::move(markedBlocksCountBuffer))
    , m_allocationRequestsBuffer(std::move(allocationRequestsBuffer))
    , m_allocationCountBuffer(std::move(allocationCountBuffer))
    , m_markClipmapDataBuffer(std::move(markClipmapDataBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_directionalPageViewInfoBuffer(std::move(directionalPageViewInfoBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowResolveMarkedBlocksCSMain",
        {},
        "CLod.VirtualShadow.ResolveMarkedBlocks.PSO");
}

VirtualShadowMapResolveMarkedBlocksBindings VirtualShadowMapResolveMarkedBlocksPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {m_pso, builder.ShaderResource(m_markedBlocksMaskBuffer), builder.ShaderResource(m_markedBlocksListBuffer),
        builder.ShaderResource(m_markedBlocksCountBuffer), builder.UnorderedAccess(m_allocationRequestsBuffer),
        builder.UnorderedAccess(m_allocationCountBuffer), builder.ShaderResource(m_markClipmapDataBuffer),
        builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}), builder.UnorderedAccess(m_dirtyPageFlagsBuffer),
        builder.UnorderedAccess(m_directionalPageViewInfoBuffer), builder.UnorderedAccess(m_statsBuffer)};
}

void VirtualShadowMapResolveMarkedBlocksPass::Initialize() {}

void VirtualShadowMapResolveMarkedBlocksPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    m_activeClipmapCount = (std::min)(
        static_cast<uint32_t>(SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades")()),
        CLodVirtualShadowMaxSupportedClipmapCount);
}

VirtualShadowMapResolveMarkedBlocksFrame VirtualShadowMapResolveMarkedBlocksPass::Prepare(
    const VirtualShadowMapResolveMarkedBlocksBindings&, const org::PassPrepareContext&) const {
    return {m_activeClipmapCount, CLodVirtualShadowBuildRuntimeResolutionConfig().maxAllocationRequests};
}

void VirtualShadowMapResolveMarkedBlocksPass::ShutdownPass() {}

void VirtualShadowMapResolveMarkedBlocksPass::Record(const VirtualShadowMapResolveMarkedBlocksBindings& bindings,
    const VirtualShadowMapResolveMarkedBlocksFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MASK_DESCRIPTOR_INDEX] = recording.Index(bindings.mask);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_LIST_DESCRIPTOR_INDEX] = recording.Index(bindings.list);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.count);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUESTS_DESCRIPTOR_INDEX] = recording.Index(bindings.requests);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUEST_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.requestCount);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_DIRTY_FLAGS_DESCRIPTOR_INDEX] = recording.Index(bindings.dirtyFlags);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.pageViewInfo);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_ACTIVE_CLIPMAP_COUNT] = frame.activeClipmapCount;
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapData);
    constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MAX_REQUEST_COUNT] = frame.maxAllocationRequests;
    br::render::DispatchCompute(recording, bindings.pipeline, constants, (CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u);
}
