#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapBuildMarkTilesPass.h"

#include "BuiltinResources.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildMarkTilesRootConstants.h"

VirtualShadowMapBuildMarkTilesPass::VirtualShadowMapBuildMarkTilesPass(
    std::shared_ptr<Buffer> tileWorkBuffer,
    std::shared_ptr<Buffer> tileCountBuffer)
    : m_tileWorkBuffer(std::move(tileWorkBuffer))
    , m_tileCountBuffer(std::move(tileCountBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowBuildMarkTilesCSMain",
        {},
        "CLod.VirtualShadow.BuildMarkTiles.PSO");
}

void VirtualShadowMapBuildMarkTilesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, Mip{ 0, 1 }))
        .WithUnorderedAccess(m_tileWorkBuffer, m_tileCountBuffer);
}

void VirtualShadowMapBuildMarkTilesPass::Setup() {}

void VirtualShadowMapBuildMarkTilesPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    const uint32_t zero = 0u;
    BUFFER_UPLOAD(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_tileCountBuffer), 0);
}

PassReturn VirtualShadowMapBuildMarkTilesPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_TILE_WORK_DESCRIPTOR_INDEX] = m_tileWorkBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_TILE_COUNT_DESCRIPTOR_INDEX] = m_tileCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_SCREEN_WIDTH] = context.renderResolution.x;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_SCREEN_HEIGHT] = context.renderResolution.y;
    rootConstants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_MAX_TILE_COUNT] = CLodVirtualShadowMaxMarkTileCount;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    const uint32_t groupCountX = (context.renderResolution.x + CLodVirtualShadowMarkTileSize - 1u) / CLodVirtualShadowMarkTileSize;
    const uint32_t groupCountY = (context.renderResolution.y + CLodVirtualShadowMarkTileSize - 1u) / CLodVirtualShadowMarkTileSize;
    commandList.Dispatch(groupCountX, groupCountY, 1u);

    return {};
}

void VirtualShadowMapBuildMarkTilesPass::Cleanup() {}