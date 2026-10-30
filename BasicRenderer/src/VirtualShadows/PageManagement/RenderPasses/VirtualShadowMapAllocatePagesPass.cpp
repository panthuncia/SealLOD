#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapAllocatePagesPass.h"
#include "Render/PassBuilders.h"

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
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, m_commandSignature, builder.ShaderResource(m_allocationRequestsBuffer), builder.ShaderResource(m_allocationCountBuffer),
        builder.IndirectArguments(m_indirectArgsBuffer), builder.ShaderResource(m_clipmapInfoBuffer),
        builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}), builder.UnorderedAccess(m_pageMetadataBuffer),
        builder.UnorderedAccess(m_dirtyPageFlagsBuffer), builder.ShaderResource(m_freePhysicalPagesBuffer),
        builder.ShaderResource(m_reusablePhysicalPagesBuffer), builder.ShaderResource(m_pageListHeaderBuffer),
        builder.UnorderedAccess(m_statsBuffer),
        SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowPageRenderBudgetSettingName)()};
}

VirtualShadowMapAllocatePagesFrame VirtualShadowMapAllocatePagesPass::Prepare(
    const VirtualShadowMapAllocatePagesBindings&, const org::PassPrepareContext&) const {
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    return {config.pageTableResolution, config.maxPhysicalPages};
}

void VirtualShadowMapAllocatePagesPass::Record(const VirtualShadowMapAllocatePagesBindings& bindings,
    const VirtualShadowMapAllocatePagesFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUESTS_DESCRIPTOR_INDEX] = recording.Index(bindings.requests);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUEST_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.requestCount);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_DIRTY_FLAGS_DESCRIPTOR_INDEX] = recording.Index(bindings.dirtyFlags);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_FREE_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.freePages);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_REUSABLE_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.reusablePages);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_LIST_HEADER_DESCRIPTOR_INDEX] = recording.Index(bindings.header);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PHYSICAL_PAGE_COUNT] = frame.maxPhysicalPages;
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_RENDER_BUDGET] = bindings.pageRenderBudget;
    constants[CLOD_VIRTUAL_SHADOW_ALLOCATE_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    br::render::DispatchComputeIndirect(recording, bindings.pipeline, constants, bindings.commandSignature,
        bindings.indirectArgs);
}
