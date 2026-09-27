#include "Transparency/AVBOIT/RenderPasses/AVBOITOccupancyRemapPass.h"

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
    builder.WithUnorderedAccess(m_occupancySliceMaskTexture);
    return {builder.BindShaderResource(m_configBuffer), builder.BindShaderResource(m_depthWarpLUTBuffer), builder.BindUnorderedAccess(m_occupancyTexture)};
}

br::render::PreparedComputeDispatch AVBOITOccupancyRemapPass::Prepare(const AVBOITOccupancyRemapBindings& bindings, const org::PassPrepareContext& preparation) const {
    br::render::PreparedComputeDispatch data{};
    if (!m_configBuffer || !m_occupancyTexture || !m_occupancySliceMaskTexture || !m_depthWarpLUTBuffer) {
        return {};
    }

    const auto* renderContext = preparation.preparationData->Get<UpdateContext>();
    auto& context = *renderContext;

    data.resourceHeap = context.textureDescriptorHeap.GetHandle();
    data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
    auto program = preparation.CaptureProgramBinding(m_pso);
    data.program = program.program;
    data.descriptorIndices = std::move(program.descriptorIndices);

    auto& misc = data.constants;
    misc[CLOD_AVBOIT_VBOIT_DEPTH_WARP_CONFIG_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.config, {org::BindlessViewKind::ShaderResource}).index;
    misc[CLOD_AVBOIT_VBOIT_DEPTH_WARP_LUT_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.lut, {org::BindlessViewKind::ShaderResource}).index;

    const auto& occupancy = preparation.Describe(bindings.occupancy);
    const uint32_t groupCountX = (occupancy.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (occupancy.texture.height + 7u) / 8u;
    if (groupCountX == 0u || groupCountY == 0u) {
        return {};
    }

    data.groupsX = groupCountX; data.groupsY = groupCountY; data.groupsZ = 1u;
    return data;
}

void AVBOITOccupancyRemapPass::Record(const AVBOITOccupancyRemapBindings&, const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
