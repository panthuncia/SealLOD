#include "VirtualShadows/PageManagement/RenderPasses/VirtualShadowMapBuildActiveBlocksPass.h"
#include "Render/PassBuilders.h"

#include "BuiltinResources.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "BasicRenderer/Assets/Texture.h"
#include "../shaders/PerPassRootConstants/clodVirtualShadowBuildActiveBlocksRootConstants.h"

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
    builder.ConstantBuffer(Builtin::PerFrameBuffer);
    return {m_pso, builder.ShaderResource(m_pageTableTexture, {static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)}),
        builder.ShaderResource(m_clipmapInfoBuffer),
        builder.UnorderedAccess(m_activeBlockMetadataBuffer), m_dynamicPages};
}

void VirtualShadowMapBuildActiveBlocksPass::Record(const VirtualShadowMapBuildActiveBlocksBindings& bindings,
    org::PassRecordContext& recording) {
    br::render::MiscConstants constants{};
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX] = recording.Index(bindings.pageTable);
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_CLIPMAP_INFO_DESCRIPTOR_INDEX] = recording.Index(bindings.clipmapInfo);
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_OUTPUT_DESCRIPTOR_INDEX] = recording.Index(bindings.output);
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_COUNT] = CLodVirtualShadowMaxMarkedBlockCount;
    constants[CLOD_VSM_BUILD_ACTIVE_BLOCKS_DYNAMIC] = bindings.dynamicPages ? 1u : 0u;
    br::render::DispatchCompute(recording, bindings.pipeline, constants, (CLodVirtualShadowMaxMarkedBlockCount + 63u) / 64u);
}
