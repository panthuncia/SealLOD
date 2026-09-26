#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapClearPagesPass.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowClearRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

VirtualShadowMapClearPagesPass::VirtualShadowMapClearPagesPass(
    std::shared_ptr<org::PixelBuffer> staticPagesTexture,
    std::shared_ptr<org::PixelBuffer> dynamicPagesTexture,
    std::shared_ptr<org::Buffer> dirtyPageFlagsBuffer,
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::Buffer> pageViewInfoBuffer,
    std::shared_ptr<org::Buffer> statsBuffer)
    : m_staticPagesTexture(std::move(staticPagesTexture))
    , m_dynamicPagesTexture(std::move(dynamicPagesTexture))
    , m_dirtyPageFlagsBuffer(std::move(dirtyPageFlagsBuffer))
    , m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_pageViewInfoBuffer(std::move(pageViewInfoBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowClearPhysicalPagesCSMain",
        { { L"CLOD_VSM_TWO_LAYER_CLEAR_VERSION", L"2" } },
        "CLod.VirtualShadow.ClearPhysicalPages.PSO");
}

VirtualShadowMapClearPagesBindings VirtualShadowMapClearPagesPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithShaderResource(Builtin::CameraBuffer).WithConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.BindUnorderedAccess(m_staticPagesTexture), builder.BindUnorderedAccess(m_dynamicPagesTexture),
        builder.BindUnorderedAccess(m_dirtyPageFlagsBuffer), builder.BindUnorderedAccess(m_pageTableTexture),
        builder.BindUnorderedAccess(m_pageMetadataBuffer), builder.BindShaderResource(m_clipmapInfoBuffer),
        builder.BindUnorderedAccess(m_pageViewInfoBuffer), builder.BindUnorderedAccess(m_statsBuffer),
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDirectionalVirtualShadowDynamicContentFilterSettingName)()};
}

void VirtualShadowMapClearPagesPass::Initialize()
{
}



br::render::PreparedComputeDispatch VirtualShadowMapClearPagesPass::Prepare(
    const VirtualShadowMapClearPagesBindings& bindings, const org::PassPrepareContext& preparation) const
{
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
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_STATIC_PAGES_DESCRIPTOR_INDEX] = uav(bindings.staticPages);
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = uav(bindings.dynamicPages);
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_FLAGS_DESCRIPTOR_INDEX] = uav(bindings.dirtyFlags);
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_METADATA_DESCRIPTOR_INDEX] = uav(bindings.pageMetadata);
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_PAGE_COUNT] = config.maxPhysicalPages;
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_ATLAS_PAGES_WIDE] = config.physicalAtlasPagesWide;
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = uav(bindings.pageViewInfo);
    data.constants[CLOD_VIRTUAL_SHADOW_CLEAR_DYNAMIC_CONTENT_FILTER_ENABLED] = bindings.dynamicContentFilter ? 1u : 0u;
    data.groupsX = config.maxPhysicalPages;
    return data;
}

void VirtualShadowMapClearPagesPass::ShutdownPass()
{
}

void VirtualShadowMapClearPagesPass::Record(const VirtualShadowMapClearPagesBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
