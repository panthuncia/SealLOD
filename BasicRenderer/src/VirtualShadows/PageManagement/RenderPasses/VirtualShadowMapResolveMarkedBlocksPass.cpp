#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapResolveMarkedBlocksPass.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowResolveMarkedBlocksRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

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
    return {builder.ShaderResource(m_markedBlocksMaskBuffer), builder.ShaderResource(m_markedBlocksListBuffer),
        builder.ShaderResource(m_markedBlocksCountBuffer), builder.UnorderedAccess(m_allocationRequestsBuffer),
        builder.UnorderedAccess(m_allocationCountBuffer), builder.ShaderResource(m_markClipmapDataBuffer),
        builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}), builder.UnorderedAccess(m_dirtyPageFlagsBuffer),
        builder.UnorderedAccess(m_directionalPageViewInfoBuffer), builder.UnorderedAccess(m_statsBuffer),
        m_activeClipmapCount};
}

void VirtualShadowMapResolveMarkedBlocksPass::Initialize() {}

void VirtualShadowMapResolveMarkedBlocksPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    m_activeClipmapCount = (std::min)(
        static_cast<uint32_t>(SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades")()),
        CLodVirtualShadowMaxSupportedClipmapCount);
}



br::render::PreparedComputeDispatch VirtualShadowMapResolveMarkedBlocksPass::Prepare(
    const VirtualShadowMapResolveMarkedBlocksBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    auto payload = m_pso.GetPayload(); br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle(); auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MASK_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.mask).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_LIST_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.list).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_COUNT_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.count).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUESTS_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.requests).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUEST_COUNT_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.requestCount).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.pageTable).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_DIRTY_FLAGS_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.dirtyFlags).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.pageViewInfo).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_STATS_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.stats).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_ACTIVE_CLIPMAP_COUNT] = bindings.activeClipmapCount;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX] = preparation.Resolve(bindings.clipmapData).index;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MAX_REQUEST_COUNT] = config.maxAllocationRequests;
    data.groupsX = (CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u;
    return data;
}

void VirtualShadowMapResolveMarkedBlocksPass::ShutdownPass() {}

void VirtualShadowMapResolveMarkedBlocksPass::Record(const VirtualShadowMapResolveMarkedBlocksBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
