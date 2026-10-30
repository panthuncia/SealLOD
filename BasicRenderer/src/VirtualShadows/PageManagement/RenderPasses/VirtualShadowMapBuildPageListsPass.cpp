#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildPageListsPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildPageListsRootConstants.h"

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
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, builder.ShaderResource(m_pageTableTexture, {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}), builder.ShaderResource(m_pageMetadataBuffer),
        builder.ShaderResource(m_allocationCountBuffer), builder.UnorderedAccess(m_freePhysicalPagesBuffer),
        builder.UnorderedAccess(m_reusablePhysicalPagesBuffer), builder.UnorderedAccess(m_pageListHeaderBuffer)};
}

void VirtualShadowMapBuildPageListsPass::Initialize() {}

VirtualShadowMapBuildPageListsFrame VirtualShadowMapBuildPageListsPass::Prepare(
    const VirtualShadowMapBuildPageListsBindings&, const org::PassPrepareContext&) const {
    const auto config = CLodVirtualShadowBuildRuntimeResolutionConfig();
    return {config.maxPhysicalPages, config.pageTableResolution};
}

void VirtualShadowMapBuildPageListsPass::ShutdownPass() {}

void VirtualShadowMapBuildPageListsPass::Record(const VirtualShadowMapBuildPageListsBindings& bindings,
    const VirtualShadowMapBuildPageListsFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_METADATA_DESCRIPTOR_INDEX] = recording.Index(bindings.pageMetadata);
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_FREE_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.freePages);
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_REUSABLE_PAGES_DESCRIPTOR_INDEX] = recording.Index(bindings.reusablePages);
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_HEADER_DESCRIPTOR_INDEX] = recording.Index(bindings.header);
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PHYSICAL_PAGE_COUNT] = frame.maxPhysicalPages;
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_ALLOCATION_COUNT_DESCRIPTOR_INDEX] = recording.Index(bindings.allocationCount);
    br::render::DispatchCompute(recording, bindings.pipeline, constants, 1);
}
