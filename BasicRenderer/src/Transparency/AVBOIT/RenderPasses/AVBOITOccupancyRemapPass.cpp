#include "Transparency/AVBOIT/RenderPasses/AVBOITOccupancyRemapPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITDepthWarpRootConstants.h"

AVBOITOccupancyRemapPass::AVBOITOccupancyRemapPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::PixelBuffer> occupancyTexture,
    std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
    std::shared_ptr<org::Buffer> depthWarpLUTBuffer)
    : m_configBuffer(std::move(configBuffer))
    , m_occupancyTexture(std::move(occupancyTexture))
    , m_occupancySliceMaskTexture(std::move(occupancySliceMaskTexture))
    , m_depthWarpLUTBuffer(std::move(depthWarpLUTBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITOccupancyRemap.hlsl",
        L"CLodAVBOITOccupancyRemapCS",
        {},
        "CLod.AVBOITOccupancyRemap.PSO");
}

AVBOITOccupancyRemapBindings AVBOITOccupancyRemapPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.UnorderedAccess(m_occupancySliceMaskTexture);
    return {m_pso, builder.ShaderResource(m_configBuffer), builder.ShaderResource(m_depthWarpLUTBuffer), builder.UnorderedAccess(m_occupancyTexture)};
}

AVBOITOccupancyRemapFrame AVBOITOccupancyRemapPass::Prepare(const AVBOITOccupancyRemapBindings& bindings, const org::PassPrepareContext& preparation) const {
    if (!m_configBuffer || !m_occupancyTexture || !m_occupancySliceMaskTexture || !m_depthWarpLUTBuffer) {
        return {};
    }

    const auto& occupancy = preparation.Describe(bindings.occupancy);
    const uint32_t groupCountX = (occupancy.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (occupancy.texture.height + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    return {groupCountX, groupCountY};
}

void AVBOITOccupancyRemapPass::Record(const AVBOITOccupancyRemapBindings& bindings, const AVBOITOccupancyRemapFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_DEPTH_WARP_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    constants[CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_DESCRIPTOR_INDEX] = context.Index(bindings.lut);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
