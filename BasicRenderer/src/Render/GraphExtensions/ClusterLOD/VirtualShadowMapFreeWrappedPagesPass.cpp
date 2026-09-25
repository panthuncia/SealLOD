#include "Render/GraphExtensions/ClusterLOD/VirtualShadowMapFreeWrappedPagesPass.h"

#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowFreeWrappedRootConstants.h"
#include "RenderPasses/PreparedComputeDispatch.h"

VirtualShadowMapFreeWrappedPagesPass::VirtualShadowMapFreeWrappedPagesPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> pageMetadataBuffer,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::Buffer> statsBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_pageMetadataBuffer(std::move(pageMetadataBuffer))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowFreeWrappedPagesCSMain",
        {},
        "CLod.VirtualShadow.FreeWrappedPages.PSO");
}

VirtualShadowMapFreeWrappedPagesBindings VirtualShadowMapFreeWrappedPagesPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {builder.BindUnorderedAccess(m_pageTableTexture), builder.BindUnorderedAccess(m_pageMetadataBuffer),
        builder.BindShaderResource(m_clipmapInfoBuffer), builder.BindUnorderedAccess(m_statsBuffer)};
}

void VirtualShadowMapFreeWrappedPagesPass::Initialize() {}



br::render::PreparedComputeDispatch VirtualShadowMapFreeWrappedPagesPass::Prepare(
    const VirtualShadowMapFreeWrappedPagesBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    auto payload = m_pso.GetPayload();
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle(); auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const auto srv = [&](org::ResourceBindingToken token) { return preparation.ResolveView(token, {org::BindlessViewKind::ShaderResource}).index; };
    const auto uav = [&](org::ResourceBindingToken token, uint32_t variant = UINT32_MAX) { return preparation.ResolveView(token, {org::BindlessViewKind::UnorderedAccess, variant}).index; };
    data.constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_DESCRIPTOR_INDEX] = uav(bindings.pageTable, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull));
    data.constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_METADATA_DESCRIPTOR_INDEX] = uav(bindings.pageMetadata);
    data.constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_CLIPMAP_INFO_DESCRIPTOR_INDEX] = srv(bindings.clipmapInfo);
    data.constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_STATS_DESCRIPTOR_INDEX] = uav(bindings.stats);
    data.constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_RESOLUTION] = config.pageTableResolution;
    data.constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    data.constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PHYSICAL_PAGE_COUNT] = config.maxPhysicalPages;
    data.groupsX = (config.pageTableResolution + 7u) / 8u; data.groupsY = data.groupsX; data.groupsZ = CLodVirtualShadowMaxSupportedClipmapCount;
    return data;
}

void VirtualShadowMapFreeWrappedPagesPass::ShutdownPass() {}

void VirtualShadowMapFreeWrappedPagesPass::Record(const VirtualShadowMapFreeWrappedPagesBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
