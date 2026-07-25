#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapAllocatePagesPass.h"

#include "BuiltinResources.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowAllocateRootConstants.h"

VirtualShadowMapAllocatePagesPass::VirtualShadowMapAllocatePagesPass(
    std::shared_ptr<Buffer> allocationRequestsBuffer,
    std::shared_ptr<Buffer> allocationCountBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> clipmapInfoBuffer,
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<Buffer> freePhysicalPagesBuffer,
    std::shared_ptr<Buffer> reusablePhysicalPagesBuffer,
    std::shared_ptr<Buffer> directionalPageViewInfoBuffer,
    std::shared_ptr<Buffer> pageListHeaderBuffer,
    std::shared_ptr<Buffer> statsBuffer)
    : m_allocationRequestsBuffer(std::move(allocationRequestsBuffer))
    , m_allocationCountBuffer(std::move(allocationCountBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_freePhysicalPagesBuffer(std::move(freePhysicalPagesBuffer))
    , m_reusablePhysicalPagesBuffer(std::move(reusablePhysicalPagesBuffer))
    , m_directionalPageViewInfoBuffer(std::move(directionalPageViewInfoBuffer))
    , m_pageListHeaderBuffer(std::move(pageListHeaderBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowAllocatePagesCSMain",
        {},
        "CLod.VirtualShadow.AllocatePages.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void VirtualShadowMapAllocatePagesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            Builtin::CameraBuffer,
            m_allocationRequestsBuffer,
            m_allocationCountBuffer,
            m_clipmapInfoBuffer,
            m_freePhysicalPagesBuffer,
            m_reusablePhysicalPagesBuffer,
            m_pageListHeaderBuffer)
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(m_pageTableTexture, m_pageMetadataBuffer, m_dirtyPageFlagsBuffer, m_directionalPageViewInfoBuffer, m_statsBuffer);

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void VirtualShadowMapAllocatePagesPass::Setup() {}

PassReturn VirtualShadowMapAllocatePagesPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());
    const CLodVirtualShadowResolutionConfig virtualShadowConfig = CLodVirtualShadowBuildRuntimeResolutionConfig();

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUESTS_DESCRIPTOR_INDEX] = m_allocationRequestsBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUEST_COUNT_DESCRIPTOR_INDEX] = m_allocationCountBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_DESCRIPTOR_INDEX] = m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_METADATA_DESCRIPTOR_INDEX] = m_pageMetadataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_DIRTY_FLAGS_DESCRIPTOR_INDEX] = m_dirtyPageFlagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_FREE_PAGES_DESCRIPTOR_INDEX] = m_freePhysicalPagesBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REUSABLE_PAGES_DESCRIPTOR_INDEX] = m_reusablePhysicalPagesBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_LIST_HEADER_DESCRIPTOR_INDEX] = m_pageListHeaderBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_INFO_DESCRIPTOR_INDEX] = m_clipmapInfoBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PHYSICAL_PAGE_COUNT] = virtualShadowConfig.maxPhysicalPages;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = m_directionalPageViewInfoBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_RENDER_BUDGET] =
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowPageRenderBudgetSettingName)();
    rootConstants[CLOD_VIRTUAL_SHADOW_ALLOCATE_STATS_DESCRIPTOR_INDEX] = m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    commandList.ExecuteIndirect(m_commandSignature->GetHandle(), m_indirectArgsBuffer->GetAPIResource().GetHandle(), 0, {}, 0, 1);

    return {};
}

void VirtualShadowMapAllocatePagesPass::Cleanup() {}
