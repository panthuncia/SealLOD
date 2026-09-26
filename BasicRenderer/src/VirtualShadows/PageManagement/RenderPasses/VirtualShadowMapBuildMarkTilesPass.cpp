#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildMarkTilesPass.h"

#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/UploadTypes.h"
#include "Resources/Buffers/Buffer.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildMarkTilesRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

VirtualShadowMapBuildMarkTilesPass::VirtualShadowMapBuildMarkTilesPass(
    std::shared_ptr<org::Buffer> tileWorkBuffer,
    std::shared_ptr<org::Buffer> tileCountBuffer)
    : m_tileWorkBuffer(std::move(tileWorkBuffer))
    , m_tileCountBuffer(std::move(tileCountBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowBuildMarkTilesCSMain",
        {},
        "CLod.VirtualShadow.BuildMarkTiles.PSO");
}

VirtualShadowMapBuildMarkTilesBindings VirtualShadowMapBuildMarkTilesPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }));
    return {builder.BindUnorderedAccess(m_tileWorkBuffer), builder.BindUnorderedAccess(m_tileCountBuffer)};
}

void VirtualShadowMapBuildMarkTilesPass::Initialize() {}

void VirtualShadowMapBuildMarkTilesPass::Update(const org::UpdateExecutionContext& executionContext)
{
    (void)executionContext;
    const uint32_t zero = 0u;
    UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromShared(m_tileCountBuffer), 0);
}



br::render::PreparedComputeDispatch VirtualShadowMapBuildMarkTilesPass::Prepare(
    const VirtualShadowMapBuildMarkTilesBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    auto payload = m_pso.GetPayload();
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_TILE_WORK_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.tileWork,
        {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_TILE_COUNT_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.tileCount,
        {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_SCREEN_WIDTH] = context->renderResolution.x;
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_SCREEN_HEIGHT] = context->renderResolution.y;
    data.constants[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_MAX_TILE_COUNT] = CLodVirtualShadowMaxMarkTileCount;
    data.groupsX = (context->renderResolution.x + CLodVirtualShadowMarkTileSize - 1u) / CLodVirtualShadowMarkTileSize;
    data.groupsY = (context->renderResolution.y + CLodVirtualShadowMarkTileSize - 1u) / CLodVirtualShadowMarkTileSize;
    return data;
}

void VirtualShadowMapBuildMarkTilesPass::ShutdownPass() {}

void VirtualShadowMapBuildMarkTilesPass::Record(const VirtualShadowMapBuildMarkTilesBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
