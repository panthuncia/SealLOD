#include "Transparency/AVBOIT/RenderPasses/AVBOITSparseClearPass.h"
#include "Render/PassBuilders.h"

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
    builder.UnorderedAccess(
            m_occupancySliceMaskTexture,
            m_scalarExtinctionTexture,
            m_chromaticExtinctionTexture,
            m_zeroTransmittanceSliceTexture);
    return {m_pso, builder.ShaderResource(m_configBuffer), builder.UnorderedAccess(m_occupancyTexture)};
}

AVBOITSparseClearFrame AVBOITSparseClearPass::Prepare(const AVBOITSparseClearBindings& bindings, const org::PassPrepareContext& preparation) const {
    if (!m_configBuffer || !m_occupancyTexture || !m_occupancySliceMaskTexture ||
        !m_scalarExtinctionTexture || !m_chromaticExtinctionTexture || !m_zeroTransmittanceSliceTexture) {
        return {};
    }

    const auto& occupancy = preparation.Describe(bindings.occupancy);
    const uint32_t groupCountX = (occupancy.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (occupancy.texture.height + 7u) / 8u;
    return {groupCountX, groupCountY};
}

void AVBOITSparseClearPass::Record(const AVBOITSparseClearBindings& bindings, const AVBOITSparseClearFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_INTEGRATE_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
