#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapComposePagesPass.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowComposeRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

VirtualShadowMapComposePagesPass::VirtualShadowMapComposePagesPass(
    std::shared_ptr<org::PixelBuffer> staticPagesTexture,
    std::shared_ptr<org::PixelBuffer> dynamicPagesTexture,
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> statsBuffer)
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

VirtualShadowMapComposePagesBindings VirtualShadowMapComposePagesPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {builder.BindShaderResource(m_staticPagesTexture), builder.BindUnorderedAccess(m_dynamicPagesTexture),
        builder.BindShaderResource(m_pageTableTexture), builder.BindShaderResource(m_pageMetadataBuffer),
        builder.BindUnorderedAccess(m_statsBuffer)};
}



br::render::PreparedComputeDispatch VirtualShadowMapComposePagesPass::Prepare(
    const VirtualShadowMapComposePagesBindings& bindings, const org::PassPrepareContext& preparation) const {
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
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_STATIC_PAGES_DESCRIPTOR_INDEX] = srv(bindings.staticPages);
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = uav(bindings.dynamicPages);
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_TABLE_DESCRIPTOR_INDEX] = srv(bindings.pageTable, static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull));
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_METADATA_DESCRIPTOR_INDEX] = srv(bindings.pageMetadata);
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PHYSICAL_PAGE_COUNT] = config.maxPhysicalPages;
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PHYSICAL_ATLAS_PAGES_WIDE] = config.physicalAtlasPagesWide;
    data.constants[CLOD_VIRTUAL_SHADOW_COMPOSE_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
    data.groupsX = config.maxPhysicalPages;
    return data;
}

void VirtualShadowMapComposePagesPass::Record(const VirtualShadowMapComposePagesBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
