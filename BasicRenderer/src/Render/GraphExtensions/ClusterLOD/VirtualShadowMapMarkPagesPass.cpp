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
#include "Render/ShaderAPI.h"

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
    builder->WithShaderResource(Builtin::Shadows::CLodCompactMainCamera);
    VirtualShadowMapMarkPagesBindings bindings{
        builder->BindShaderResource(m_tileWorkBuffer), builder->BindShaderResource(m_tileCountBuffer),
        builder->BindIndirectArguments(m_indirectArgsBuffer), builder->BindShaderResource(m_markClipmapDataBuffer),
        builder->BindUnorderedAccess(m_markedBlocksMaskBuffer), builder->BindUnorderedAccess(m_markedBlocksListBuffer),
        builder->BindUnorderedAccess(m_markedBlocksCountBuffer), {}, m_activeClipmapCount, m_receiverSubpageMode};
    if (m_receiverSubpageMaskBuffer) {
        bindings.receiverMask = builder->BindUnorderedAccess(m_receiverSubpageMaskBuffer);
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
    VirtualShadowMarkFrameData data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    auto clear = preparation.CaptureProgramBinding(m_clearPso);
    data.clearProgram = clear.program;
    data.clearIndices = std::move(clear.descriptorIndices);
    auto clearUint2 = preparation.CaptureProgramBinding(m_clearUint2Pso);
    data.clearUint2Program = clearUint2.program;
    data.clearUint2Indices = std::move(clearUint2.descriptorIndices);
    auto mark = preparation.CaptureProgramBinding(m_pso);
    data.markProgram = mark.program;
    data.markIndices = std::move(mark.descriptorIndices);
    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    data.indirectArguments = preparation.CaptureResource(bindings.indirectArgs);
    data.clearMask[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.mask);
    data.clearMask[CLOD_CLEAR_UINT_BUFFER_COUNT] = CLodVirtualShadowMaxMarkedBlockCount;
    data.clearReceiver = data.clearMask;
    if (bindings.receiverSubpageMode != CLodVirtualShadowReceiverSubpageModeOff) {
        if (!bindings.receiverMask) throw std::logic_error("receiver subpage mode requires a declared mask");
        data.clearReceiver[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(*bindings.receiverMask);
        data.clearReceiver[CLOD_CLEAR_UINT_BUFFER_COUNT] = CLodVirtualShadowMaxReceiverPageCount;
        data.receiverGroups = (CLodVirtualShadowMaxReceiverPageCount + 63u) / 64u;
        data.receiverUint2 = bindings.receiverSubpageMode == CLodVirtualShadowReceiverSubpageMode8x8;
    }
    data.clearCount = data.clearMask;
    data.clearCount[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX] = uav(bindings.count);
    data.clearCount[CLOD_CLEAR_UINT_BUFFER_COUNT] = 1u;
    data.barrierResources[0] = preparation.CaptureResource(bindings.mask);
    data.barrierResources[1] = preparation.CaptureResource(bindings.count);
    if (data.receiverGroups) {
        data.barrierResources[2] = preparation.CaptureResource(*bindings.receiverMask);
        data.barrierCount = 3;
    }
    auto& c = data.mark;
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_WORK_DESCRIPTOR_INDEX] = srv(bindings.tileWork);
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_COUNT_DESCRIPTOR_INDEX] = srv(bindings.tileCount);
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_WIDTH] = context->renderResolution.x;
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_HEIGHT] = context->renderResolution.y;
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_ACTIVE_CLIPMAP_COUNT] = bindings.activeClipmapCount;
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX] = srv(bindings.clipmapData);
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_MASK_DESCRIPTOR_INDEX] = uav(bindings.mask);
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_LIST_DESCRIPTOR_INDEX] = uav(bindings.list);
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_COUNT_DESCRIPTOR_INDEX] = uav(bindings.count);
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_RECEIVER_MASK_DESCRIPTOR_INDEX] = bindings.receiverMask ? uav(*bindings.receiverMask) : 0u;
    c[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_RECEIVER_MASK_ENABLED] = bindings.receiverSubpageMode;
    return data;
}

void VirtualShadowMapMarkPagesPass::Record(const VirtualShadowMapMarkPagesBindings&,
    const VirtualShadowMarkFrameData& data, org::PassRecordContext& recording)
{
    auto& commands = recording.Commands();
    commands.SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
    const auto bind = [&](const std::vector<unsigned int>& indices) {
        if (!indices.empty()) commands.PushConstants(rhi::ShaderStage::Compute, 0,
            org::shaderapi::kResourceDescriptorIndicesRootParameter, 0,
            static_cast<uint32_t>(indices.size()), indices.data());
    };
    commands.BindLayout(recording.ResolveLayout(data.clearProgram)); commands.BindPipeline(recording.Resolve(data.clearProgram)); bind(data.clearIndices);
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, data.clearMask.data());
    commands.Dispatch((CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u, 1u, 1u);
    if (data.receiverGroups) {
        const auto program = data.receiverUint2 ? data.clearUint2Program : data.clearProgram;
        commands.BindLayout(recording.ResolveLayout(program));
        commands.BindPipeline(recording.Resolve(program));
        bind(data.receiverUint2 ? data.clearUint2Indices : data.clearIndices);
        commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, data.clearReceiver.data());
        commands.Dispatch(data.receiverGroups, 1u, 1u);
        commands.BindLayout(recording.ResolveLayout(data.clearProgram)); commands.BindPipeline(recording.Resolve(data.clearProgram)); bind(data.clearIndices);
    }
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, data.clearCount.data());
    commands.Dispatch(1u, 1u, 1u);
    std::array<rhi::BufferBarrier, 3> barriers{};
    for (uint32_t i = 0; i < data.barrierCount; ++i) {
        barriers[i].buffer = recording.Resolve(data.barrierResources[i]).GetHandle();
        barriers[i].beforeAccess = barriers[i].afterAccess = rhi::ResourceAccessType::UnorderedAccess;
        barriers[i].beforeSync = barriers[i].afterSync = rhi::ResourceSyncState::ComputeShading;
    }
    rhi::BarrierBatch batch{}; batch.buffers = {barriers.data(), data.barrierCount}; commands.Barriers(batch);
    commands.BindLayout(recording.ResolveLayout(data.markProgram)); commands.BindPipeline(recording.Resolve(data.markProgram)); bind(data.markIndices);
    commands.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, data.mark.data());
    commands.ExecuteIndirect(data.commandSignature, recording.Resolve(data.indirectArguments).GetHandle(), 0, {}, 0, 1);
}
