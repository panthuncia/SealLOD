#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapNonRasterableHierarchyPass.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowDirtyHierarchyRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

VirtualShadowMapNonRasterableHierarchyPass::VirtualShadowMapNonRasterableHierarchyPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::PixelBuffer> nonRasterableHierarchyTexture,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_nonRasterableHierarchyTexture(std::move(nonRasterableHierarchyTexture))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowBuildNonRasterableHierarchyCSMain",
        {},
        "CLod.VirtualShadow.NonRasterableHierarchy.PSO");
}

VirtualShadowMapNonRasterableHierarchyBindings VirtualShadowMapNonRasterableHierarchyPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {
        builder.BindShaderResource(Subresources(m_pageTableTexture, org::Mip{0, 1})),
        builder.BindUnorderedAccess(Subresources(m_nonRasterableHierarchyTexture, org::FromMip{0})),
        builder.BindShaderResource(m_clipmapInfoBuffer)};
}

br::render::PreparedComputeDispatchSequence VirtualShadowMapNonRasterableHierarchyPass::Prepare(
    const VirtualShadowMapNonRasterableHierarchyBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    br::render::PreparedComputeDispatchSequence data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle(); data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle(); auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    const uint32_t mipCount = preparation.Describe(bindings.hierarchy).texture.mipLevels; data.steps.reserve(mipCount);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const bool pageTable = mip == 0; const uint32_t src = pageTable ? config.pageTableResolution : (std::max)(config.pageTableResolution >> (mip - 1u), 1u);
        const uint32_t dst = pageTable ? src : (src > 1u ? src >> 1u : 1u); br::render::PreparedComputeDispatchSequence::Step step{};
        step.uavBarrierBefore = !pageTable;
        step.constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_DESCRIPTOR_INDEX] = pageTable
            ? preparation.ResolveView(bindings.pageTable, {org::BindlessViewKind::ShaderResource,
                static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}).index
            : preparation.ResolveView(bindings.hierarchy, {org::BindlessViewKind::UnorderedAccess,
                static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull), mip - 1u}).index;
        step.constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_DEST_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.hierarchy,
            {org::BindlessViewKind::UnorderedAccess, static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull), mip}).index;
        step.constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_IS_PAGE_TABLE] = pageTable ? 1u : 0u;
        step.constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION] = src;
        step.constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_COUNT] = CLodVirtualShadowMaxSupportedClipmapCount;
        step.constants[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_INFO_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.clipmapInfo,
            {org::BindlessViewKind::ShaderResource}).index;
        step.groupsX = (dst + 7u) / 8u; step.groupsY = step.groupsX; step.groupsZ = CLodVirtualShadowMaxSupportedClipmapCount; data.steps.push_back(std::move(step));
    }
    return data;
}

void VirtualShadowMapNonRasterableHierarchyPass::Record(const VirtualShadowMapNonRasterableHierarchyBindings&,
    const br::render::PreparedComputeDispatchSequence& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatchSequence(data, recording);
}
