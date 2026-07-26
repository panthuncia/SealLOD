#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapClearDirtyBitsPass.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Texture.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowClearDirtyBitsRootConstants.h"

VirtualShadowMapClearDirtyBitsPass::VirtualShadowMapClearDirtyBitsPass(
    std::shared_ptr<PixelBuffer> pageTableTexture,
    std::shared_ptr<Buffer> allocationRequestsBuffer,
    std::shared_ptr<Buffer> allocationCountBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> dirtyFlagsBuffer,
    std::shared_ptr<Buffer> statsBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_dirtyFlagsBuffer(std::move(dirtyFlagsBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    (void)allocationRequestsBuffer;
    (void)allocationCountBuffer;
    (void)indirectArgsBuffer;

    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowClearDirtyBitsCSMain",
        {},
        "CLod.VirtualShadow.ClearDirtyBits.PSO");
}

void VirtualShadowMapClearDirtyBitsPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithUnorderedAccess(
        m_pageTableTexture,
        m_dirtyFlagsBuffer,
        m_statsBuffer);

    builder->WithConstantBuffer(Builtin::PerFrameBuffer);
}

void VirtualShadowMapClearDirtyBitsPass::Setup()
{
}

PassReturn VirtualShadowMapClearDirtyBitsPass::Execute(PassExecutionContext& executionContext)
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
    rootConstants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_PAGE_TABLE_DESCRIPTOR_INDEX] = m_pageTableTexture->GetUAVShaderVisibleInfo(UAVViewType::Texture2DArrayFull, 0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_PAGE_TABLE_RESOLUTION] = virtualShadowConfig.pageTableResolution;
    rootConstants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_STATS_DESCRIPTOR_INDEX] = m_statsBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_COMPLETE_EMPTY_ADMITTED_PAGES] = 1u;
    rootConstants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_DIRTY_FLAGS_DESCRIPTOR_INDEX] =
        m_dirtyFlagsBuffer->GetUAVShaderVisibleInfo(0).slot.index;

    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        rootConstants);

    constexpr uint32_t threadsPerDimension = 8u;
    const uint32_t groupCountX = (virtualShadowConfig.pageTableResolution + threadsPerDimension - 1u) / threadsPerDimension;
    const uint32_t groupCountY = (virtualShadowConfig.pageTableResolution + threadsPerDimension - 1u) / threadsPerDimension;
    commandList.Dispatch(groupCountX, groupCountY, CLodVirtualShadowMaxSupportedClipmapCount);
    return {};
}

void VirtualShadowMapClearDirtyBitsPass::Cleanup()
{
}
