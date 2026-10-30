#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapMarkPagesPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BuiltinResources.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowMarkBlocksRootConstants.h"

VirtualShadowMapMarkPagesPass::VirtualShadowMapMarkPagesPass(
    std::shared_ptr<org::Buffer> tileWorkBuffer,
    std::shared_ptr<org::Buffer> tileCountBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> markClipmapDataBuffer,
    std::shared_ptr<org::Buffer> markedBlocksMaskBuffer,
    std::shared_ptr<org::Buffer> markedBlocksListBuffer,
    std::shared_ptr<org::Buffer> markedBlocksCountBuffer,
    std::shared_ptr<org::Buffer> receiverSubpageMaskBuffer)
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
    m_clearUint2Pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"ClearUint2StructuredBufferCSMain",
        {},
        "CLod.VirtualShadow.ReceiverMask.ClearUint2.PSO");

    rhi::IndirectArg dispatchArgs[] = {
        {.kind = rhi::IndirectArgKind::Dispatch }
    };

    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>();
    auto device = DeviceManager::GetInstance().GetDevice();
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        *m_commandSignature);
}

VirtualShadowMapMarkPagesBindings VirtualShadowMapMarkPagesPass::Declare(org::PassBuilder& declaration)
{
    declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    auto* builder = &declaration;
    builder->ShaderResource(Builtin::Shadows::CLodCompactMainCamera);
    VirtualShadowMapMarkPagesBindings bindings{
        m_pso, m_clearPso, m_clearUint2Pso, m_commandSignature,
        builder->ShaderResource(m_tileWorkBuffer), builder->ShaderResource(m_tileCountBuffer),
        builder->IndirectArguments(m_indirectArgsBuffer), builder->ShaderResource(m_markClipmapDataBuffer),
        builder->UnorderedAccess(m_markedBlocksMaskBuffer), builder->UnorderedAccess(m_markedBlocksListBuffer),
        builder->UnorderedAccess(m_markedBlocksCountBuffer), {}};
    if (m_receiverSubpageMaskBuffer) {
        bindings.receiverMask = builder->UnorderedAccess(m_receiverSubpageMaskBuffer);
    }
    return bindings;
}



void VirtualShadowMapMarkPagesPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    m_activeClipmapCount = (std::min)(
        static_cast<uint32_t>(SettingsManager::GetInstance().getSettingGetter<uint8_t>("numDirectionalLightCascades")()),
    CLodVirtualShadowMaxSupportedClipmapCount);
    m_receiverSubpageMode = m_receiverSubpageMaskBuffer
        ? SettingsManager::GetInstance().getSettingGetter<uint32_t>(
            CLodDirectionalVirtualShadowReceiverSubpageModeSettingName)()
        : CLodVirtualShadowReceiverSubpageModeOff;
}

VirtualShadowMarkFrameData VirtualShadowMapMarkPagesPass::Prepare(
    const VirtualShadowMapMarkPagesBindings& bindings, const org::PassPrepareContext& preparation) const
{
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    if (m_receiverSubpageMode != CLodVirtualShadowReceiverSubpageModeOff && !bindings.receiverMask)
        throw std::logic_error("receiver subpage mode requires a declared mask");
    return {context->renderResolution.x, context->renderResolution.y, m_activeClipmapCount, m_receiverSubpageMode};
}

void VirtualShadowMapMarkPagesPass::Record(const VirtualShadowMapMarkPagesBindings& bindings,
    const VirtualShadowMarkFrameData& frame, org::PassRecordContext& recording)
{
    auto& commands = recording.Commands();
    br::render::MiscConstants clearMask{};
    clearMask[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = recording.Index(bindings.mask);
    clearMask[CLOD_CLEAR_UINT_BUFFER_COUNT] = CLodVirtualShadowMaxMarkedBlockCount;
    const bool receiver = frame.receiverSubpageMode != CLodVirtualShadowReceiverSubpageModeOff;

    recording.BindComputePipeline(bindings.clearPipeline);
    br::render::PushMiscConstants(recording, clearMask);
    commands.Dispatch((CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u, 1u, 1u);
    if (receiver) {
        br::render::MiscConstants clearReceiver = clearMask;
        clearReceiver[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = recording.Index(*bindings.receiverMask);
        clearReceiver[CLOD_CLEAR_UINT_BUFFER_COUNT] = CLodVirtualShadowMaxReceiverPageCount;
        recording.BindComputePipeline(frame.receiverSubpageMode == CLodVirtualShadowReceiverSubpageMode8x8
            ? bindings.clearUint2Pipeline : bindings.clearPipeline);
        br::render::PushMiscConstants(recording, clearReceiver);
        commands.Dispatch((CLodVirtualShadowMaxReceiverPageCount + 63u) / 64u, 1u, 1u);
        recording.BindComputePipeline(bindings.clearPipeline);
    }
    br::render::MiscConstants clearCount = clearMask;
    clearCount[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = recording.Index(bindings.count);
    clearCount[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
    br::render::PushMiscConstants(recording, clearCount);
    commands.Dispatch(1u, 1u, 1u);

    constexpr auto uav = rhi::ResourceAccessType::UnorderedAccess;
    if (receiver) recording.BufferBarriers({bindings.mask.Resource(), bindings.count.Resource(), bindings.receiverMask->Resource()}, uav, uav);
    else recording.BufferBarriers({bindings.mask.Resource(), bindings.count.Resource()}, uav, uav);

    br::render::MiscConstants mark{};
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_WORK_DESCRIPTOR_INDEX] = recording.Index(bindings.tileWork);
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.tileCount);
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_WIDTH] = frame.screenWidth;
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_HEIGHT] = frame.screenHeight;
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_ACTIVE_CLIPMAP_COUNT] = frame.activeClipmapCount;
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapData);
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_MASK_DESCRIPTOR_INDEX] = recording.Index(bindings.mask);
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_LIST_DESCRIPTOR_INDEX] = recording.Index(bindings.list);
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.count);
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_RECEIVER_MASK_DESCRIPTOR_INDEX] = bindings.receiverMask ? recording.Index(*bindings.receiverMask) : 0u;
    mark[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_RECEIVER_MASK_ENABLED] = frame.receiverSubpageMode;
    br::render::DispatchComputeIndirect(recording, bindings.markPipeline, mark, bindings.commandSignature, bindings.indirectArgs);
}
