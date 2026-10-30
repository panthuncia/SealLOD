#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapClearPagesPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowClearRootConstants.h"

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
    builder.ShaderResource(Builtin::CameraBuffer);
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, builder.UnorderedAccess(m_staticPagesTexture), builder.UnorderedAccess(m_dynamicPagesTexture),
        builder.UnorderedAccess(m_dirtyPageFlagsBuffer), builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}),
        builder.UnorderedAccess(m_pageMetadataBuffer), builder.ShaderResource(m_clipmapInfoBuffer),
        builder.UnorderedAccess(m_pageViewInfoBuffer), builder.UnorderedAccess(m_statsBuffer),
        SettingsManager::GetInstance().getSettingGetter<bool>(CLodDirectionalVirtualShadowDynamicContentFilterSettingName)()};
}

void VirtualShadowMapClearPagesPass::Initialize()
{
}

VirtualShadowMapClearPagesFrame VirtualShadowMapClearPagesPass::Prepare(
    const VirtualShadowMapClearPagesBindings&, const org::PassPrepareContext&) const
{
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    return {config.pageTableResolution, config.maxPhysicalPages, config.physicalAtlasPagesWide};
}

void VirtualShadowMapClearPagesPass::ShutdownPass()
{
}

void VirtualShadowMapClearPagesPass::Record(const VirtualShadowMapClearPagesBindings& bindings,
    const VirtualShadowMapClearPagesFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_STATIC_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.staticPages);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_DYNAMIC_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.dynamicPages);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_FLAGS_DESCRIPTOR_INDEX] = recording.Index(bindings.dirtyFlags);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_PAGE_COUNT] = frame.maxPhysicalPages;
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_ATLAS_PAGES_WIDE] = frame.physicalAtlasPagesWide;
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_VIEW_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.pageViewInfo);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_DYNAMIC_CONTENT_FILTER_ENABLED] = bindings.dynamicContentFilter ? 1u : 0u;
    br::render::DispatchCompute(recording, bindings.pipeline, constants, frame.maxPhysicalPages);
}
