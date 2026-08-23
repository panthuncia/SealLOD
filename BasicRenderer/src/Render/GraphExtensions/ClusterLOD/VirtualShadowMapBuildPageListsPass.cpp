#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildPageListsPass.h"

#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildPageListsRootConstants.h"

VirtualShadowMapBuildPageListsPass::VirtualShadowMapBuildPageListsPass(
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> allocationCountBuffer,
    std::shared_ptr<Buffer> freePhysicalPagesBuffer,
    std::shared_ptr<Buffer> reusablePhysicalPagesBuffer,
    std::shared_ptr<Buffer> pageListHeaderBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_allocationCountBuffer(std::move(allocationCountBuffer))
    , m_freePhysicalPagesBuffer(std::move(freePhysicalPagesBuffer))
    , m_reusablePhysicalPagesBuffer(std::move(reusablePhysicalPagesBuffer))
    , m_pageListHeaderBuffer(std::move(pageListHeaderBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowBuildPageListsCSMain",
        {},
        "CLod.VirtualShadow.BuildPageLists.PSO");
}

void VirtualShadowMapBuildPageListsPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(m_pageTableTexture, m_pageMetadataBuffer, m_allocationCountBuffer)
        .WithUnorderedAccess(
            m_freePhysicalPagesBuffer,
            m_reusablePhysicalPagesBuffer,
            m_pageListHeaderBuffer);

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void VirtualShadowMapBuildPageListsPass::Setup() {}

PassReturn VirtualShadowMapBuildPageListsPass::Execute(PassExecutionContext& executionContext)
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
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_DESCRIPTOR_INDEX] = m_pageTableTexture->GetSRVInfo(SRVViewType::Texture2DArrayFull, 0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_METADATA_DESCRIPTOR_INDEX] = m_pageMetadataBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_FREE_PAGES_DESCRIPTOR_INDEX] = m_freePhysicalPagesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_REUSABLE_PAGES_DESCRIPTOR_INDEX] = m_reusablePhysicalPagesBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_HEADER_DESCRIPTOR_INDEX] = m_pageListHeaderBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PHYSICAL_PAGE_COUNT] = virtualShadowConfig.maxPhysicalPages;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_ALLOCATION_COUNT_DESCRIPTOR_INDEX] =
        m_allocationCountBuffer->GetSRVInfo(0).slot.index;

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

void VirtualShadowMapBuildPageListsPass::Cleanup() {}
