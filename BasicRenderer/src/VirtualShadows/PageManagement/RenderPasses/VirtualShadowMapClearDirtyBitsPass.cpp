#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapClearDirtyBitsPass.h"
#include "Render/PassBuilders.h"

#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"

#include "../shaders/PerPassRootConstants/clodVirtualShadowClearDirtyBitsRootConstants.h"

VirtualShadowMapClearDirtyBitsPass::VirtualShadowMapClearDirtyBitsPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> allocationRequestsBuffer,
    std::shared_ptr<org::Buffer> allocationCountBuffer,
    std::shared_ptr<org::Buffer> indirectArgsBuffer,
    std::shared_ptr<org::Buffer> dirtyFlagsBuffer,
    std::shared_ptr<org::Buffer> statsBuffer)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_dirtyFlagsBuffer(std::move(dirtyFlagsBuffer))
    , m_statsBuffer(std::move(statsBuffer))
{
    (void)allocationRequestsBuffer;
    (void)allocationCountBuffer;
    (void)indirectArgsBuffer;

    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowClearDirtyBitsCSMain",
        {},
        "CLod.VirtualShadow.ClearDirtyBits.PSO");
}

VirtualShadowMapClearDirtyBitsBindings VirtualShadowMapClearDirtyBitsPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {
        m_pso,
        builder.UnorderedAccess(m_pageTableTexture, {static_cast<uint32_t>(org::UAVViewType::Texture2DArrayFull)}),
        builder.UnorderedAccess(m_dirtyFlagsBuffer),
        builder.UnorderedAccess(m_statsBuffer)};
}

void VirtualShadowMapClearDirtyBitsPass::Initialize()
{
}

VirtualShadowMapClearDirtyBitsFrame VirtualShadowMapClearDirtyBitsPass::Prepare(
    const VirtualShadowMapClearDirtyBitsBindings&, const org::PassPrepareContext&) const {
    return {CLodVirtualShadowBuildRuntimeResolutionConfig().pageTableResolution};
}

void VirtualShadowMapClearDirtyBitsPass::ShutdownPass()
{
}

void VirtualShadowMapClearDirtyBitsPass::Record(const VirtualShadowMapClearDirtyBitsBindings& bindings,
    const VirtualShadowMapClearDirtyBitsFrame& frame, org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_PAGE_TABLE_RESOLUTION] = frame.pageTableResolution;
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_STATS_DESCRIPTOR_INDEX] = recording.Index(bindings.stats);
    constants[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_DIRTY_FLAGS_DESCRIPTOR_INDEX] = recording.Index(bindings.dirtyFlags);
    const uint32_t groups = (frame.pageTableResolution + 7u) / 8u;
    br::render::DispatchCompute(recording, bindings.pipeline, constants, groups, groups, CLodVirtualShadowMaxSupportedClipmapCount);
}
