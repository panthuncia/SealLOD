#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapComposePagesPass.h"

#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowComposeRootConstants.h"

VirtualShadowMapComposePagesPass::VirtualShadowMapComposePagesPass(
    std::shared_ptr<PixelBuffer> staticPagesTexture,
    std::shared_ptr<PixelBuffer> dynamicPagesTexture,
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> pageMetadataBuffer,
    std::shared_ptr<Buffer> statsBuffer)
    : m_staticPagesTexture(std::move(staticPagesTexture))
    , m_dynamicPagesTexture(std::move(dynamicPagesTexture))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowComposePhysicalPagesCSMain",
        { { L"CLOD_VSM_TWO_LAYER_COMPOSE_VERSION", L"2" } },
        "CLod.VirtualShadow.ComposePhysicalPages.PSO");
}

void VirtualShadowMapComposePagesPass::DeclareResourceUsages(
    ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            m_staticPagesTexture,
            m_pageTableTexture,
            m_pageMetadataBuffer)
        .WithUnorderedAccess(m_dynamicPagesTexture, m_statsBuffer);
}

PassReturn VirtualShadowMapComposePagesPass::Execute(
    PassExecutionContext& executionContext)
{
    auto& context = *executionContext.hostData->Get<RenderContext>();
    auto& commandList = executionContext.commandList;
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();

    commandList.SetDescriptorHeaps(
        context.textureDescriptorHeap.GetHandle(),
        context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(
        commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t constants[NumMiscUintRootConstants] = {};
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_STATIC_PAGES_DESCRIPTOR_INDEX] =
        m_staticPagesTexture->GetSRVInfo(0).slot.index;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_DYNAMIC_PAGES_DESCRIPTOR_INDEX] =
        m_dynamicPagesTexture->GetUAVShaderVisibleInfo(0).slot.index;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_TABLE_DESCRIPTOR_INDEX] =
        m_pageTableTexture->GetSRVInfo(SRVViewType::Texture2DArrayFull, 0).slot.index;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_METADATA_DESCRIPTOR_INDEX] =
        m_pageMetadataBuffer->GetSRVInfo(0).slot.index;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_TABLE_RESOLUTION] =
        config.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PHYSICAL_PAGE_COUNT] =
        config.maxPhysicalPages;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PHYSICAL_ATLAS_PAGES_WIDE] =
        config.physicalAtlasPagesWide;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_STATS_DESCRIPTOR_INDEX] =
        m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    commandList.PushConstants(
        rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, constants);
    commandList.Dispatch(config.maxPhysicalPages, 1u, 1u);
    return {};
}
