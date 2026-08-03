#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapMarkPagesPass.h"

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "BuiltinResources.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowMarkBlocksRootConstants.h"

VirtualShadowMapMarkPagesPass::VirtualShadowMapMarkPagesPass(
    std::shared_ptr<Buffer> tileWorkBuffer,
    std::shared_ptr<Buffer> tileCountBuffer,
    std::shared_ptr<Buffer> indirectArgsBuffer,
    std::shared_ptr<Buffer> markClipmapDataBuffer,
    std::shared_ptr<Buffer> markedBlocksMaskBuffer,
    std::shared_ptr<Buffer> markedBlocksListBuffer,
    std::shared_ptr<Buffer> markedBlocksCountBuffer,
    std::shared_ptr<Buffer> receiverSubpageMaskBuffer)
    : m_tileWorkBuffer(std::move(tileWorkBuffer))
    , m_tileCountBuffer(std::move(tileCountBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_markClipmapDataBuffer(std::move(markClipmapDataBuffer))
    , m_markedBlocksMaskBuffer(std::move(markedBlocksMaskBuffer))
    , m_markedBlocksListBuffer(std::move(markedBlocksListBuffer))
    , m_markedBlocksCountBuffer(std::move(markedBlocksCountBuffer))
    , m_receiverSubpageMaskBuffer(std::move(receiverSubpageMaskBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowMarkBlocksCSMain",
        {},
        "CLod.VirtualShadow.MarkPages.PSO");

    m_clearPso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUintStructuredBufferCSMain",
        {},
        "CLod.VirtualShadow.MarkBlocks.Clear.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        m_commandSignature);
}

void VirtualShadowMapMarkPagesPass::DeclareResourceUsages(ComputePassBuilder* builder)
{
    builder->WithShaderResource(
            Builtin::Shadows::CLodCompactMainCamera,
            m_tileWorkBuffer,
            m_tileCountBuffer,
            m_markClipmapDataBuffer
    )
        .WithIndirectArguments(m_indirectArgsBuffer)
        .WithUnorderedAccess(
            m_markedBlocksMaskBuffer,
            m_markedBlocksListBuffer,
            m_markedBlocksCountBuffer,
            m_receiverSubpageMaskBuffer);
}

void VirtualShadowMapMarkPagesPass::Setup() {}

void VirtualShadowMapMarkPagesPass::Update(const UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    m_activeClipmapCount = (std::min)(
        static_cast<uint32_t>(SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades")()),
    CLodVirtualShadowMaxSupportedClipmapCount);
    m_receiverSubpageMaskEnabled =
        SettingsManager::GetInstance().getSettingGetter<bool>(
            CLodDirectionalVirtualShadowReceiverSubpageMaskSettingName)();
}

PassReturn VirtualShadowMapMarkPagesPass::Execute(PassExecutionContext& executionContext)
{
    auto* renderContext = executionContext.hostData->Get<RenderContext>();
    auto& context = *renderContext;
    auto& commandList = executionContext.commandList;

    commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());
    commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());

    BindResourceDescriptorIndices(commandList, m_clearPso.GetResourceDescriptorSlots());
    commandList.BindPipeline(m_clearPso.GetAPIPipelineState().GetHandle());

    uint32_t clearRootConstants[NumMiscUintRootConstants] = {};
    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = m_markedBlocksMaskBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_VALUE] = 0u;
    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = CLodVirtualShadowMaxMarkedBlockCount;
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        clearRootConstants);
    commandList.Dispatch((CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u, 1u, 1u);

    if (m_receiverSubpageMaskEnabled) {
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] =
            m_receiverSubpageMaskBuffer->GetUAVShaderVisibleInfo(0).slot.index;
        clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] =
            CLodVirtualShadowMaxReceiverPageCount;
        commandList.PushConstants(
            rhi::ShaderStage::Compute,
            0,
            MiscUintRootSignatureIndex,
            0,
            NumMiscUintRootConstants,
            clearRootConstants);
        commandList.Dispatch(
            (CLodVirtualShadowMaxReceiverPageCount + 63u) / 64u,
            1u,
            1u);
    }

    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = m_markedBlocksCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    clearRootConstants[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
    commandList.PushConstants(
        rhi::ShaderStage::Compute,
        0,
        MiscUintRootSignatureIndex,
        0,
        NumMiscUintRootConstants,
        clearRootConstants);
    commandList.Dispatch(1u, 1u, 1u);

    rhi::BufferBarrier clearBarriers[3] = {};
    clearBarriers[0].buffer = m_markedBlocksMaskBuffer->GetAPIResource().GetHandle();
    clearBarriers[0].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    clearBarriers[0].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    clearBarriers[0].beforeSync = rhi::ResourceSyncState::ComputeShading;
    clearBarriers[0].afterSync = rhi::ResourceSyncState::ComputeShading;
    clearBarriers[1].buffer = m_markedBlocksCountBuffer->GetAPIResource().GetHandle();
    clearBarriers[1].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
    clearBarriers[1].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
    clearBarriers[1].beforeSync = rhi::ResourceSyncState::ComputeShading;
    clearBarriers[1].afterSync = rhi::ResourceSyncState::ComputeShading;
    uint32_t clearBarrierCount = 2u;
    if (m_receiverSubpageMaskEnabled) {
        clearBarriers[2].buffer =
            m_receiverSubpageMaskBuffer->GetAPIResource().GetHandle();
        clearBarriers[2].beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
        clearBarriers[2].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        clearBarriers[2].beforeSync = rhi::ResourceSyncState::ComputeShading;
        clearBarriers[2].afterSync = rhi::ResourceSyncState::ComputeShading;
        clearBarrierCount = 3u;
    }
    rhi::BarrierBatch barrierBatch{};
    barrierBatch.buffers =
        rhi::Span<rhi::BufferBarrier>(clearBarriers, clearBarrierCount);
    commandList.Barriers(barrierBatch);

    commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());
    BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

    uint32_t rootConstants[NumMiscUintRootConstants] = {};
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_WORK_DESCRIPTOR_INDEX] = m_tileWorkBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_COUNT_DESCRIPTOR_INDEX] = m_tileCountBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_WIDTH] = context.renderResolution.x;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_HEIGHT] = context.renderResolution.y;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_ACTIVE_CLIPMAP_COUNT] = m_activeClipmapCount;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX] = m_markClipmapDataBuffer->GetSRVInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_MASK_DESCRIPTOR_INDEX] = m_markedBlocksMaskBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_LIST_DESCRIPTOR_INDEX] = m_markedBlocksListBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_COUNT_DESCRIPTOR_INDEX] = m_markedBlocksCountBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_RECEIVER_MASK_DESCRIPTOR_INDEX] =
        m_receiverSubpageMaskBuffer->GetUAVShaderVisibleInfo(0).slot.index;
    rootConstants[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_RECEIVER_MASK_ENABLED] =
        m_receiverSubpageMaskEnabled ? 1u : 0u;

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

void VirtualShadowMapMarkPagesPass::Cleanup() {}
