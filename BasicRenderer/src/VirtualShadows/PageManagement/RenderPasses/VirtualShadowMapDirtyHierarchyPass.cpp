#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapDirtyHierarchyPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowDirtyHierarchyRootConstants.h"

VirtualShadowMapDirtyHierarchyPass::VirtualShadowMapDirtyHierarchyPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::PixelBuffer> dirtyHierarchyTexture,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_dirtyHierarchyTexture(std::move(dirtyHierarchyTexture))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowBuildDirtyHierarchyCSMain",
        {},
        "CLod.VirtualShadow.DirtyHierarchy.PSO");
}

VirtualShadowMapDirtyHierarchyBindings VirtualShadowMapDirtyHierarchyPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    std::vector<org::UavView> mipViews;
    for (uint32_t mip = 0; mip < m_dirtyHierarchyTexture->GetMipLevels(); ++mip)
        mipViews.push_back({static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull), mip});
    auto hierarchy = builder.UnorderedAccess(Subresources(m_dirtyHierarchyTexture, org::FromMip{0}), std::span<const org::UavView>{mipViews});
    return {
        m_pso,
        builder.ShaderResource(Subresources(m_pageTableTexture, org::Mip{0, 1}), {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}),
        std::move(hierarchy.views),
        builder.ShaderResource(m_clipmapInfoBuffer)};
}

VirtualShadowMapDirtyHierarchyFrame VirtualShadowMapDirtyHierarchyPass::Prepare(
    const VirtualShadowMapDirtyHierarchyBindings&, const org::PassPrepareContext&) const {
    return {CLodVirtualShadowBuildRuntimeResolutionConfig().pageTableResolution};
}

void VirtualShadowMapDirtyHierarchyPass::Record(const VirtualShadowMapDirtyHierarchyBindings& bindings,
    const VirtualShadowMapDirtyHierarchyFrame& frame, org::PassRecordContext& recording) {
    const uint32_t mipCount = static_cast<uint32_t>(bindings.hierarchy.size());
    if (mipCount == 0) return;
    recording.BindComputePipeline(bindings.pipeline);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const bool pageTable = mip == 0; const uint32_t src = pageTable ? frame.pageTableResolution : (std::max)(frame.pageTableResolution >> (mip - 1u), 1u);
        const uint32_t dst = pageTable ? src : (src > 1u ? src >> 1u : 1u);
        if (!pageTable) recording.UavBarrier();
        br::render::MiscConstants constants{};
        constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_DESCRIPTOR_INDEX] = pageTable
            ? recording.Index(bindings.pageTable)
            : recording.Index(bindings.hierarchy[mip - 1u]);
        constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_DEST_DESCRIPTOR_INDEX] = recording.Index(bindings.hierarchy[mip]);
        constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_IS_PAGE_TABLE] = pageTable ? 1u : 0u;
        constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION] = src;
        constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
        br::render::PushMiscConstants(recording, constants);
        const uint32_t groups = (dst + 7u) / 8u;
        recording.Commands().Dispatch(groups, groups, CLodVirtualShadowMaxSupportedClipmapCount);
    }
}
