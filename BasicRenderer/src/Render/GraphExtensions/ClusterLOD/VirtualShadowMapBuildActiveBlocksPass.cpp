#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildActiveBlocksPass.h"

#include "BuiltinResources.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildActiveBlocksRootConstants.h"

VirtualShadowMapBuildActiveBlocksPass::VirtualShadowMapBuildActiveBlocksPass(
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> clipmapInfoBuffer,
    std::shared_ptr<Buffer> activeBlockMetadataBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_activeBlockMetadataBuffer(std::move(activeBlockMetadataBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowBuildActiveBlocksCSMain",
        {},
        "CLod.VirtualShadow.BuildActiveBlocks.PSO");
}

void VirtualShadowMapBuildActiveBlocksPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(m_pageTableTexture, m_clipmapInfoBuffer)
        .WithUnorderedAccess(m_activeBlockMetadataBuffer)
        .WithConstantBuffer(Builtin::PerFrameBuffer);
}

PassReturn VirtualShadowMapBuildActiveBlocksPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t constants[NumMiscUintRootConstants] = {};
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX] =
        m_pageTableTexture->GetSRVInfo(SRVViewType::Texture2DArrayFull, 0).slot.index;
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_CLIPMAP_INFO_DESCRIPTOR_INDEX] =
        m_clipmapInfoBuffer->GetSRVInfo(0).slot.index;
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_OUTPUT_DESCRIPTOR_INDEX] =
        m_activeBlockMetadataBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_COUNT] = CLodVirtualShadowMaxMarkedBlockCount;
    commandList.PushConstants(
        rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0,
        NumMiscUintRootConstants, constants);
    commandList.Dispatch((CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u, 1u, 1u);
    return {};
}
