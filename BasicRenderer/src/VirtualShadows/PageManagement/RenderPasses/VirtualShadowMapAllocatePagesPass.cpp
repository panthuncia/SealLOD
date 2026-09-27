#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapAllocatePagesPass.h"

#include "BuiltinResources.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowAllocateRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

VirtualShadowMapAllocatePagesPass::VirtualShadowMapAllocatePagesPass(
    std::shared_ptr<org::Buffer> allocationRequestsBuffer,
    std::shared_ptr<org::Buffer> allocationCountBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<org::Buffer> freePhysicalPagesBuffer,
    std::shared_ptr<org::Buffer> reusablePhysicalPagesBuffer,
    std::shared_ptr<org::Buffer> pageListHeaderBuffer,
    std::shared_ptr<org::Buffer> statsBuffer)
    : m_allocationRequestsBuffer(std::move(allocationRequestsBuffer))
    , m_allocationCountBuffer(std::move(allocationCountBuffer))
    , m_indirectArgsBuffer(std::move(indirectArgsBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_freePhysicalPagesBuffer(std::move(freePhysicalPagesBuffer))
    , m_reusablePhysicalPagesBuffer(std::move(reusablePhysicalPagesBuffer))
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
    rhi::CommandSignaturePtr commandSignature;
    device.CreateCommandSignature(
        rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatchArgs, 1), sizeof(CLodReyesDispatchIndirectCommand) },
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        commandSignature);
    m_commandSignature = std::make_shared<rhi::CommandSignaturePtr>(std::move(commandSignature));
}

VirtualShadowMapAllocatePagesBindings VirtualShadowMapAllocatePagesPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.BindShaderResource(m_allocationRequestsBuffer), builder.BindShaderResource(m_allocationCountBuffer),
        builder.BindIndirectArguments(m_indirectArgsBuffer), builder.BindShaderResource(m_clipmapInfoBuffer),
        builder.BindUnorderedAccess(m_pageTableTexture), builder.BindUnorderedAccess(m_pageMetadataBuffer),
        builder.BindUnorderedAccess(m_dirtyPageFlagsBuffer), builder.BindShaderResource(m_freePhysicalPagesBuffer),
        builder.BindShaderResource(m_reusablePhysicalPagesBuffer), builder.BindShaderResource(m_pageListHeaderBuffer),
        builder.BindUnorderedAccess(m_statsBuffer),
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowPageRenderBudgetSettingName)()};
}

br::render::PreparedComputeIndirect VirtualShadowMapAllocatePagesPass::Prepare(
    const VirtualShadowMapAllocatePagesBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    br::render::PreparedComputeIndirect data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.commandSignature = preparation.CaptureCommandSignature(m_commandSignature);
    data.argumentsReference = preparation.CaptureResource(bindings.indirectArgs);
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUESTS_DESCRIPTOR_INDEX] = srv(bindings.requests);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUEST_COUNT_DESCRIPTOR_INDEX] = srv(bindings.requestCount);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_METADATA_DESCRIPTOR_INDEX] = uav(bindings.pageMetadata);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_DIRTY_FLAGS_DESCRIPTOR_INDEX] = uav(bindings.dirtyFlags);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_FREE_PAGES_DESCRIPTOR_INDEX] = srv(bindings.freePages);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REUSABLE_PAGES_DESCRIPTOR_INDEX] = srv(bindings.reusablePages);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_LIST_HEADER_DESCRIPTOR_INDEX] = srv(bindings.header);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PHYSICAL_PAGE_COUNT] = config.maxPhysicalPages;
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_RENDER_BUDGET] = bindings.pageRenderBudget;
    data.constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
    return data;
}

void VirtualShadowMapAllocatePagesPass::Record(const VirtualShadowMapAllocatePagesBindings&,
    const br::render::PreparedComputeIndirect& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeIndirect(data, recording);
}
