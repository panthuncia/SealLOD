#include "Transparency/AVBOIT/RenderPasses/AVBOITAdaptiveFitPass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Resources/Buffers/Buffer.h"

#include "../shaders/PerPassRootConstants/clodAVBOITAdaptiveFitRootConstants.h"

AVBOITAdaptiveFitPass::AVBOITAdaptiveFitPass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::Buffer> fitStateBuffer)
    : m_configBuffer(std::move(configBuffer))
    , m_fitStateBuffer(std::move(fitStateBuffer))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITAdaptiveFit.hlsl",
        L"CLodAVBOITAdaptiveFitCS",
        {},
        "CLod.AVBOITAdaptiveFit.PSO");
}

AVBOITAdaptiveFitBindings AVBOITAdaptiveFitPass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    return {m_pso, builder.UnorderedAccess(m_configBuffer), builder.ShaderResource(m_fitStateBuffer)};
}

AVBOITAdaptiveFitFrame AVBOITAdaptiveFitPass::Prepare(
    const AVBOITAdaptiveFitBindings&, const org::PassPrepareContext&) const {
    if (!m_configBuffer || !m_fitStateBuffer) {
        return {};
    }

    return {1u, 1u};
}

void AVBOITAdaptiveFitPass::Record(const AVBOITAdaptiveFitBindings& bindings, const AVBOITAdaptiveFitFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    constants[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_STATE_DESCRIPTOR_INDEX] = context.Index(bindings.state);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
