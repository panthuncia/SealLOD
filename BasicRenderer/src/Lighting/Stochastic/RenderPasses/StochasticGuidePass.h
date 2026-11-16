#pragma once

#include <cstdint>
#include <memory>

#include "BuiltinResources.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Settings/SettingsManager.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingPasses.h"
#include "Lighting/Stochastic/StochasticLightingResources.h"
#include "../shaders/PerPassRootConstants/stochasticLightingRootConstants.h"

namespace br::render {

struct StochasticGuideFrame {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t frame = 0u;
    uint32_t flags = 0u;
};

struct StochasticGuideBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken guide, signature;
};

// The guide for guide-normalized accumulation (docs/stochastic_lighting.md "Guide-normalized accumulation"), shaders/Stochastic/
// stochasticGuide.hlsl: per pixel, the unshadowed diffuse and specular luminance of every light in its light grid froxel, in
// the denoiser's demodulated domain. Runs with the denoiser, before shading, and dispatches only while
// stochasticLightingNormalize is on (per frame; the texture is bound either way, so the setting needs no graph rebuild).
class StochasticGuidePass
    : public org::TypedRenderGraphPass<StochasticGuidePass, StochasticGuideFrame, StochasticGuideBindings> {
public:
    explicit StochasticGuidePass(std::shared_ptr<StochasticLightingResources> resources)
        : m_resources(std::move(resources)) {
    }

    StochasticGuideBindings Declare(org::PassBuilder& builder) {
        StochasticGuideBindings bindings{};
        // Shader model 6.10 like the reference, whose includes (the shadow ray's) it shares; it traces nothing.
        bindings.pipeline = builder.Pipeline(CLodRtComputePipeline(L"Shaders/Stochastic/stochasticGuide.hlsl", L"StochasticGuideCS", "Stochastic.Guide.PSO"));
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::Light::InfoBuffer);
        // The primary surface and its shading, and the light grid (as selection declares them).
        builder.ShaderResource(Builtin::PerMeshBuffer, Builtin::PerMaterialDataBuffer);
        builder.ShaderResource(Builtin::Light::ActiveLightIndices,
            Builtin::Light::GridCells, Builtin::Light::GridIndices, Builtin::PerMaterialOpenPBRDataBuffer,
            Builtin::Surface::BaseColorOpacity, Builtin::Surface::NormalRoughness, Builtin::Surface::SpecularAo, Builtin::Surface::Emissive,
            Builtin::Surface::Identity, Builtin::Surface::Payload0, Builtin::Surface::Payload1, Builtin::Surface::Records,
            Builtin::OpenPBR::FuzzLTC, Builtin::OpenPBR::IdealMetalEnergyComplement, Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
            Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement);
        builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
            org::SrvView{ static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull) });
        builder.ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }));
        bindings.guide = builder.UnorderedAccess(m_resources->guide).View();
        // The light-mixture signature (docs/stochastic_lighting.md "Guide-normalized accumulation"), written while
        // stochasticLightingMixtureConfidence is on; bound either way, so that setting is per frame too.
        bindings.signature = builder.UnorderedAccess(m_resources->signature).View();
        builder.PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    StochasticGuideFrame Prepare(const StochasticGuideBindings&, const org::PassPrepareContext& preparation) const {
        StochasticGuideFrame frame{};
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        if (!update) {
            return frame;
        }
        frame.width = update->renderResolution.x;
        frame.height = update->renderResolution.y;
        frame.frame = static_cast<uint32_t>(update->frameNumber);
        if (SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingNormalize")()) {
            frame.flags |= STOCHASTIC_FLAG_NORMALIZE | STOCHASTIC_FLAG_DENOISED;
            if (SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingMixtureConfidence")()) {
                frame.flags |= STOCHASTIC_FLAG_MIXTURE;
            }
            // The paired ratio divides by its own unshadowed estimate X and multiplies by the guide, so it needs G = E[X]: the
            // exact guide. With it the shader takes the exact walk in every froxel, the far-field guide's included (its cost
            // then grows with the light count again; docs/performance/stochastic-lighting-experiments.md "Correctness pass (2026-11-17)").
            if (SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingPaired")()) {
                frame.flags |= STOCHASTIC_FLAG_PAIRED;
            }
        }
        return frame;
    }

    static void Record(const StochasticGuideBindings& bindings, const StochasticGuideFrame& frame, org::PassRecordContext& recording) {
        if (frame.width == 0u || frame.height == 0u || (frame.flags & STOCHASTIC_FLAG_NORMALIZE) == 0u) {
            return;
        }
        br::render::MiscConstants constants{};
        constants[STOCHASTIC_FRAME] = frame.frame;
        constants[STOCHASTIC_FLAGS] = frame.flags;
        constants[STOCHASTIC_GUIDE_UAV] = recording.Index(bindings.guide);
        constants[STOCHASTIC_GUIDE_SIGNATURE_UAV] = recording.Index(bindings.signature);
        recording.BindComputePipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        recording.Commands().Dispatch((frame.width + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE,
            (frame.height + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE, 1u);
    }

private:
    std::shared_ptr<StochasticLightingResources> m_resources;
};

} // namespace br::render
