#include "Transparency/AVBOIT/RenderPasses/AVBOITResolvePass.h"
#include "Render/PassBuilders.h"

#include "Pipeline/PipelineState/PSOManager.h"
#include "BuiltinResources.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"
#include "BasicRenderer/Extensions/RenderContext.h"
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
    builder.UnorderedAccess(Builtin::Color::HDRColorTarget);
    return {m_pso, 
        builder.ShaderResource(m_configBuffer),
        builder.ShaderResource(m_accumulationTexture),
        builder.ShaderResource(m_normalizationTexture),
        builder.ShaderResource(m_shadingExtinctionTexture) };
}

AVBOITResolveFrame AVBOITResolvePass::Prepare(
    const AVBOITResolveBindings& bindings, const org::PassPrepareContext& preparation) const {
    if (!m_configBuffer || !m_accumulationTexture || !m_normalizationTexture || !m_shadingExtinctionTexture) {
        return {};
    }

    const auto& accumulation = preparation.Describe(bindings.accumulation);
    const uint32_t groupCountX = (accumulation.texture.width + 7u) / 8u;
    const uint32_t groupCountY = (accumulation.texture.height + 7u) / 8u;
    return {groupCountX, groupCountY};
}

void AVBOITResolvePass::Record(const AVBOITResolveBindings& bindings, const AVBOITResolveFrame& frame, org::PassRecordContext& context) {
    if (!frame.groupsX || !frame.groupsY) return;
    br::render::MiscConstants constants{};
    constants[CLOD_AVBOIT_VBOIT_RESOLVE_CONFIG_DESCRIPTOR_INDEX] = context.Index(bindings.config);
    constants[CLOD_AVBOIT_VBOIT_RESOLVE_ACCUMULATION_DESCRIPTOR_INDEX] = context.Index(bindings.accumulation);
    constants[CLOD_AVBOIT_VBOIT_RESOLVE_NORMALIZATION_DESCRIPTOR_INDEX] = context.Index(bindings.normalization);
    constants[CLOD_AVBOIT_VBOIT_RESOLVE_SHADING_EXTINCTION_DESCRIPTOR_INDEX] = context.Index(bindings.extinction);
    br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
}
