#include "Transparency/AVBOIT/RenderPasses/AVBOITSparseClearPass.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITIntegrateRootConstants.h"

AVBOITSparseClearPass::AVBOITSparseClearPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::PixelBuffer> occupancyTexture,
    std::shared_ptr<org::PixelBuffer> occupancySliceMaskTexture,
    std::shared_ptr<org::PixelBuffer> scalarExtinctionTexture,
    std::shared_ptr<org::PixelBuffer> chromaticExtinctionTexture,
    std::shared_ptr<org::PixelBuffer> zeroTransmittanceSliceTexture)
    : m_configBuffer(std::move(configBuffer))
    , m_occupancyTexture(std::move(occupancyTexture))
    , m_occupancySliceMaskTexture(std::move(occupancySliceMaskTexture))
    , m_scalarExtinctionTexture(std::move(scalarExtinctionTexture))
    , m_chromaticExtinctionTexture(std::move(chromaticExtinctionTexture))
    , m_zeroTransmittanceSliceTexture(std::move(zeroTransmittanceSliceTexture))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITSparseClear.hlsl",
        L"CLodAVBOITSparseClearCS",
        {},
        "CLod.AVBOITSparseClear.PSO");
}

AVBOITSparseClearBindings AVBOITSparseClearPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithUnorderedAccess(
            m_occupancySliceMaskTexture,
            m_scalarExtinctionTexture,
            m_chromaticExtinctionTexture,
            m_zeroTransmittanceSliceTexture);
    return {builder.BindShaderResource(m_configBuffer), builder.BindUnorderedAccess(m_occupancyTexture)};
}

br::render::PreparedComputeDispatch AVBOITSparseClearPass::Prepare(const AVBOITSparseClearBindings& bindings, const org::PassPrepareContext& preparation) const {
    br::render::PreparedComputeDispatch data{};
    if (!m_configBuffer || !m_occupancyTexture || !m_occupancySliceMaskTexture ||
        !m_scalarExtinctionTexture || !m_chromaticExtinctionTexture || !m_zeroTransmittanceSliceTexture) {
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
    misc[CLOD_AVBOIT_VBOIT_INTEGRATE_CONFIG_DESCRIPTOR_INDEX] = preparation.ResolveView(bindings.config, {org::BindlessViewKind::ShaderResource}).index;

    const auto& occupancy = preparation.Describe(bindings.occupancy);
    const uint32_t groupCountX = (occupancy.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (occupancy.texture.height + 7u) / 8u;
    data.groupsX = groupCountX; data.groupsY = groupCountY; data.groupsZ = 1u;
    return data;
}

void AVBOITSparseClearPass::Record(const AVBOITSparseClearBindings&, const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
