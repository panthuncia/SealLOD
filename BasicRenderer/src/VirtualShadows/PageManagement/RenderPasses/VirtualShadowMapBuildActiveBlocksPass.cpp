#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildActiveBlocksPass.h"

#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildActiveBlocksRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

VirtualShadowMapBuildActiveBlocksPass::VirtualShadowMapBuildActiveBlocksPass(
    std::shared_ptr<org::PixelBuffer> pageTableTexture,
    std::shared_ptr<org::Buffer> clipmapInfoBuffer,
    std::shared_ptr<org::Buffer> activeBlockMetadataBuffer,
    bool dynamicPages)
    : m_pageTableTexture(std::move(pageTableTexture))
    , m_clipmapInfoBuffer(std::move(clipmapInfoBuffer))
    , m_activeBlockMetadataBuffer(std::move(activeBlockMetadataBuffer))
    , m_dynamicPages(dynamicPages)
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"Shaders/ClusterLOD/clodUtil.hlsl",
        L"CLodVirtualShadowBuildActiveBlocksCSMain",
        { { L"CLOD_VSM_TWO_LAYER_ACTIVE_BLOCKS_VERSION", L"2" } },
        "CLod.VirtualShadow.BuildActiveBlocks.PSO");
}

VirtualShadowMapBuildActiveBlocksBindings VirtualShadowMapBuildActiveBlocksPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithConstantBuffer(Builtin::PerFrameBuffer);
    return {builder.BindShaderResource(m_pageTableTexture),
        builder.BindShaderResource(m_clipmapInfoBuffer),
        builder.BindUnorderedAccess(m_activeBlockMetadataBuffer), m_dynamicPages};
}



br::render::PreparedComputeDispatch VirtualShadowMapBuildActiveBlocksPass::Prepare(
    const VirtualShadowMapBuildActiveBlocksBindings& bindings, const org::PassPrepareContext& preparation) const {
    const auto* context = preparation.preparationData->Get<UpdateContext>();
    auto payload = m_pso.GetPayload();
    br::render::PreparedComputeDispatch data{};
    data.resourceHeap = context->textureDescriptorHeap.GetHandle();
    data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
    data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
    auto program = preparation.CaptureProgramBinding(std::move(payload));
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);
    data.constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.pageTable,
        {org::BindlessViewKind::ShaderResource, static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}).index;
    data.constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_CLIPMAP_INFO_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.clipmapInfo,
        {org::BindlessViewKind::ShaderResource}).index;
    data.constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_OUTPUT_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.output,
        {org::BindlessViewKind::UnorderedAccess}).index;
    data.constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_COUNT] = CLodVirtualShadowMaxMarkedBlockCount;
    data.constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_DYNAMIC] = bindings.dynamicPages ? 1u : 0u;
    data.groupsX = (CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u;
    return data;
}

void VirtualShadowMapBuildActiveBlocksPass::Record(const VirtualShadowMapBuildActiveBlocksBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
