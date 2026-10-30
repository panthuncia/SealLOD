#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapComposePagesPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowComposeRootConstants.h"

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
    return {m_pso, builder.ShaderResource(m_staticPagesTexture), builder.UnorderedAccess(m_dynamicPagesTexture),
        builder.ShaderResource(m_pageTableTexture, {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}), builder.ShaderResource(m_pageMetadataBuffer),
        builder.UnorderedAccess(m_statsBuffer)};
}

VirtualShadowMapComposePagesFrame VirtualShadowMapComposePagesPass::Prepare(
    const VirtualShadowMapComposePagesBindings&, const org::PassPrepareContext&) const {
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    return {config.pageTableResolution, config.maxPhysicalPages, config.physicalAtlasPagesWide};
}

void VirtualShadowMapComposePagesPass::Record(const VirtualShadowMapComposePagesBindings& bindings,
    const VirtualShadowMapComposePagesFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_STATIC_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.staticPages);
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.dynamicPages);
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PHYSICAL_PAGE_COUNT] = frame.maxPhysicalPages;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_PHYSICAL_ATLAS_PAGES_WIDE] = frame.physicalAtlasPagesWide;
    constants[CLOD_VIRTUAL_SHADOW_COMPOSE_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    br::render::DispatchCompute(recording, bindings.pipeline, constants, frame.maxPhysicalPages);
}
