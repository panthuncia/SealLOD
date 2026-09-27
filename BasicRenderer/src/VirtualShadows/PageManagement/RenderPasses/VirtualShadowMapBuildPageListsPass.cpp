#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildPageListsPass.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildPageListsRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

VirtualShadowMapBuildPageListsPass::VirtualShadowMapBuildPageListsPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> allocationCountBuffer,
    std::shared_ptr<org::Buffer> freePhysicalPagesBuffer,
    std::shared_ptr<org::Buffer> reusablePhysicalPagesBuffer,
    std::shared_ptr<org::Buffer> pageListHeaderBuffer)
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

VirtualShadowMapBuildPageListsBindings VirtualShadowMapBuildPageListsPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.BindShaderResource(m_pageTableTexture), builder.BindShaderResource(m_pageMetadataBuffer),
        builder.BindShaderResource(m_allocationCountBuffer), builder.BindUnorderedAccess(m_freePhysicalPagesBuffer),
        builder.BindUnorderedAccess(m_reusablePhysicalPagesBuffer), builder.BindUnorderedAccess(m_pageListHeaderBuffer)};
}

void VirtualShadowMapBuildPageListsPass::Initialize() {}



br::render::PreparedComputeDispatch VirtualShadowMapBuildPageListsPass::Prepare(
    const VirtualShadowMapBuildPageListsBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    auto payload = m_pso.GetPayload();
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource, variant}).index; };
    const auto uav = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess}).index; };
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_DESCRIPTOR_INDEX] = srv(bindings.pageTable, static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull));
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_METADATA_DESCRIPTOR_INDEX] = srv(bindings.pageMetadata);
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_FREE_PAGES_DESCRIPTOR_INDEX] = uav(bindings.freePages);
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_REUSABLE_PAGES_DESCRIPTOR_INDEX] = uav(bindings.reusablePages);
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_HEADER_DESCRIPTOR_INDEX] = uav(bindings.header);
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PHYSICAL_PAGE_COUNT] = config.maxPhysicalPages;
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_ALLOCATION_COUNT_DESCRIPTOR_INDEX] = srv(bindings.allocationCount);
    data.groupsX = 1;
    return data;
}

void VirtualShadowMapBuildPageListsPass::ShutdownPass() {}

void VirtualShadowMapBuildPageListsPass::Record(const VirtualShadowMapBuildPageListsBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
