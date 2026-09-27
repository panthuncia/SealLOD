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
    return {builder.BindShaderResource(m_markedBlocksMaskBuffer), builder.BindShaderResource(m_markedBlocksListBuffer),
        builder.BindShaderResource(m_markedBlocksCountBuffer), builder.BindUnorderedAccess(m_allocationRequestsBuffer),
        builder.BindUnorderedAccess(m_allocationCountBuffer), builder.BindShaderResource(m_markClipmapDataBuffer),
        builder.BindUnorderedAccess(m_pageTableTexture), builder.BindUnorderedAccess(m_dirtyPageFlagsBuffer),
        builder.BindUnorderedAccess(m_directionalPageViewInfoBuffer), builder.BindUnorderedAccess(m_statsBuffer),
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
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MASK_DESCRIPTOR_INDEX] = srv(bindings.mask);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_LIST_DESCRIPTOR_INDEX] = srv(bindings.list);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_COUNT_DESCRIPTOR_INDEX] = srv(bindings.count);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUESTS_DESCRIPTOR_INDEX] = uav(bindings.requests);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUEST_COUNT_DESCRIPTOR_INDEX] = uav(bindings.requestCount);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_DIRTY_FLAGS_DESCRIPTOR_INDEX] = uav(bindings.dirtyFlags);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = uav(bindings.pageViewInfo);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_ACTIVE_CLIPMAP_COUNT] = bindings.activeClipmapCount;
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX] = srv(bindings.clipmapData);
    data.constants[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MAX_REQUEST_COUNT] = config.maxAllocationRequests;
    data.groupsX = (CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u;
    return data;
}

void VirtualShadowMapResolveMarkedBlocksPass::ShutdownPass() {}

void VirtualShadowMapResolveMarkedBlocksPass::Record(const VirtualShadowMapResolveMarkedBlocksBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
