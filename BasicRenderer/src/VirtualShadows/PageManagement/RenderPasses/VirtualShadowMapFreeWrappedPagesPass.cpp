#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapFreeWrappedPagesPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowFreeWrappedRootConstants.h"

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
    return {m_pso, builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}), builder.UnorderedAccess(m_pageMetadataBuffer),
        builder.ShaderResource(m_clipmapInfoBuffer), builder.UnorderedAccess(m_statsBuffer)};
}

void VirtualShadowMapFreeWrappedPagesPass::Initialize() {}

VirtualShadowMapFreeWrappedPagesFrame VirtualShadowMapFreeWrappedPagesPass::Prepare(
    const VirtualShadowMapFreeWrappedPagesBindings&, const org::PassPrepareContext&) const {
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    return {config.pageTableResolution, config.maxPhysicalPages};
}

void VirtualShadowMapFreeWrappedPagesPass::ShutdownPass() {}

void VirtualShadowMapFreeWrappedPagesPass::Record(const VirtualShadowMapFreeWrappedPagesBindings& bindings,
    const VirtualShadowMapFreeWrappedPagesFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
    constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
    constants[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PHYSICAL_PAGE_COUNT] = frame.maxPhysicalPages;
    const uint32_t groups = (frame.pageTableResolution + 7u) / 8u;
    br::render::DispatchCompute(recording, bindings.pipeline, constants, groups, groups, CLodVirtualShadowMaxSupportedClipmapCount);
}
