#include "Render/GraphExtensions/ClusterLOD/AVBOITResolvePass.h"

#include "Managers/Singletons/PSOManager.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "Render/RenderContext.h"
#include "../shaders/PerPassRootConstants/clodAVBOITResolveRootConstants.h"

AVBOITResolvePass::AVBOITResolvePass(
    std::shared_ptr<org::Buffer> configBuffer,
    std::shared_ptr<org::PixelBuffer> accumulationTexture,
    std::shared_ptr<org::PixelBuffer> normalizationTexture,
    std::shared_ptr<org::PixelBuffer> shadingExtinctionTexture)
    : m_configBuffer(std::move(configBuffer))
    , m_accumulationTexture(std::move(accumulationTexture))
    , m_normalizationTexture(std::move(normalizationTexture))
    , m_shadingExtinctionTexture(std::move(shadingExtinctionTexture))
{
    m_pso = PSOManager::GetInstance().MakeComputePipeline(
        PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
        L"shaders/ClusterLOD/AVBOITResolve.hlsl",
        L"CLodAVBOITResolveCS",
        {},
        "CLod.AVBOITResolve.PSO");
}

AVBOITResolveBindings AVBOITResolvePass::Declare(org::PassBuilder& builder)
{
    builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
    builder.WithUnorderedAccess(Builtin::Color::HDRColorTarget);
    return {
        builder.BindShaderResource(m_configBuffer),
        builder.BindShaderResource(m_accumulationTexture),
        builder.BindShaderResource(m_normalizationTexture),
        builder.BindShaderResource(m_shadingExtinctionTexture) };
}

br::render::PreparedComputeDispatch AVBOITResolvePass::Prepare(
    const AVBOITResolveBindings& bindings, const org::PassPrepareContext& preparation) const {
    br::render::PreparedComputeDispatch data{};
    if (!m_configBuffer || !m_accumulationTexture || !m_normalizationTexture || !m_shadingExtinctionTexture) {
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
    misc[CLOD_AVBOIT_VBOIT_RESOLVE_CONFIG_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.config, {org::BindlessViewKind::ShaderResource}).index;
    misc[CLOD_AVBOIT_VBOIT_RESOLVE_ACCUMULATION_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.accumulation, {org::BindlessViewKind::ShaderResource}).index;
    misc[CLOD_AVBOIT_VBOIT_RESOLVE_NORMALIZATION_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.normalization, {org::BindlessViewKind::ShaderResource}).index;
    misc[CLOD_AVBOIT_VBOIT_RESOLVE_SHADING_EXTINCTION_DESCRIPTOR_INDEX] =
        preparation.ResolveView(bindings.extinction, {org::BindlessViewKind::ShaderResource}).index;

    const auto& accumulation = preparation.Describe(bindings.accumulation);
    const uint32_t groupCountX = (accumulation.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (accumulation.texture.height + 7u) / 8u;
    data.groupsX = groupCountX; data.groupsY = groupCountY; data.groupsZ = 1u;
    return data;
}

void AVBOITResolvePass::Record(const AVBOITResolveBindings&,
    const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
    br::render::RecordPreparedComputeDispatch(data, recording);
}
