#include "Render/GraphExtensions/ClusterLOD/AVBOITAdaptiveFitPass.h"

#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
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
    return {builder.BindUnorderedAccess(m_configBuffer), builder.BindShaderResource(m_fitStateBuffer)};
}

br::render::PreparedComputeDispatch AVBOITAdaptiveFitPass::Prepare(
    const AVBOITAdaptiveFitBindings& bindings, const org::PassPrepareContext& preparation) const {
    br::render::PreparedComputeDispatch data{};
    if (!m_configBuffer || !m_fitStateBuffer) {
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
        preparation.ResolveView(bindings.config, {org::BindlessViewKind::UnorderedAccess}).index;
    misc[CLOD_AVBOIT_VBOIT_ADAPTIVE_FIT_STATE_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.state, {org::BindlessViewKind::ShaderResource}).index;

    data.groupsX = 1u; data.groupsY = 1u; data.groupsZ = 1u;
    return data;
}

void AVBOITAdaptiveFitPass::Record(const AVBOITAdaptiveFitBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
