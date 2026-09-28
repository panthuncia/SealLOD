#include "Transparency/AVBOIT/RenderPasses/AVBOITAdaptiveFitUpdatePass.h"

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
    return {builder.ShaderResource(m_configBuffer), builder.ShaderResource(m_occupancyHistogramBuffer), builder.UnorderedAccess(m_fitStateBuffer)};
}

br::render::PreparedComputeDispatch AVBOITAdaptiveFitUpdatePass::Prepare(const AVBOITAdaptiveFitUpdateBindings& bindings, const org::PassPrepareContext& preparation) const {
    br::render::PreparedComputeDispatch data{};
    if (!m_configBuffer || !m_occupancyHistogramBuffer || !m_fitStateBuffer) {
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
    misc[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_CONFIG_DESCRIPTOR_INDEX] =
        preparation.Resolve(bindings.config).index;
    misc[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_STATE_DESCRIPTOR_INDEX] =
        preparation.Resolve(bindings.state).index;
    misc[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_HISTOGRAM_DESCRIPTOR_INDEX] =
        preparation.Resolve(bindings.histogram).index;

    data.groupsX = 1u; data.groupsY = 1u; data.groupsZ = 1u;
    return data;
}

void AVBOITAdaptiveFitUpdatePass::Record(const AVBOITAdaptiveFitUpdateBindings&, const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
