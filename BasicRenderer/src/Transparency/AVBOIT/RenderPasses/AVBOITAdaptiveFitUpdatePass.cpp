#include "Transparency/AVBOIT/RenderPasses/AVBOITAdaptiveFitUpdatePass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITAdaptiveFitRootConstants.h"

AVBOITAdaptiveFitUpdatePass::AVBOITAdaptiveFitUpdatePass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::Buffer> occupancyHistogramBuffer,
    std::shared_ptr<org::Buffer> fitStateBuffer)
    : m_configBuffer(std::move(configBuffer))
    , m_occupancyHistogramBuffer(std::move(occupancyHistogramBuffer))
    , m_fitStateBuffer(std::move(fitStateBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITAdaptiveFit.hlsl",
        L"CLodAVBOITAdaptiveFitUpdateCS",
        {},
        "CLod.AVBOITAdaptiveFitUpdate.PSO");
}

AVBOITAdaptiveFitUpdateBindings AVBOITAdaptiveFitUpdatePass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {m_pso, builder.ShaderResource(m_configBuffer), builder.ShaderResource(m_occupancyHistogramBuffer), builder.UnorderedAccess(m_fitStateBuffer)};
}

AVBOITAdaptiveFitUpdateFrame AVBOITAdaptiveFitUpdatePass::Prepare(const AVBOITAdaptiveFitUpdateBindings&, const org::PassPrepareContext&) const {
    if (!m_configBuffer || !m_occupancyHistogramBuffer || !m_fitStateBuffer) {
        return {};
    }

    return {1u, 1u};
}

void AVBOITAdaptiveFitUpdatePass::Record(const AVBOITAdaptiveFitUpdateBindings& bindings, const AVBOITAdaptiveFitUpdateFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    constants[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_STATE_DESCRIPTOR_INDEX] = context.Index(bindings.state);
    constants[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_HISTOGRAM_DESCRIPTOR_INDEX] = context.Index(bindings.histogram);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
