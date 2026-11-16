#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

#include "BuiltinResources.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Lighting/Stochastic/StochasticLightingResources.h"
#include "../shaders/PerPassRootConstants/stochasticLightingRootConstants.h"

namespace br::render {

enum class StochasticDenoiseStage : uint8_t {
    Temporal, // Demodulate, reproject, rectify, accumulate; writes the history.
    Spatial, // Edge-stopped sparse filter of the history; remodulates into StochasticLighting::Diffuse and ::Specular.
};

struct StochasticDenoiseFrame {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t frame = 0u;
    uint32_t flags = 0u;
    uint32_t parity = 0u;
    uint32_t downsample = 1u; // The noisy inputs' (docs/stochastic_lighting.md "Reduced resolution").
};

struct StochasticDenoiseBindings {
    org::PipelineState pipeline;
    StochasticDenoiseStage stage = StochasticDenoiseStage::Temporal;
    org::DeclaredViewToken noisyDiffuse, noisySpecular, variance, outputDiffuse, outputSpecular;
    std::array<org::DeclaredViewToken, 2> historyDiffuse{}, historySpecular{}, historyMoments{}, historyChange{}, surfaceHistory{};
};

// Stochastic lighting's denoiser (docs/stochastic_lighting.md "Denoiser"), shaders/Stochastic/stochasticDenoise.hlsl.
class StochasticDenoisePass
    : public org::TypedRenderGraphPass<StochasticDenoisePass, StochasticDenoiseFrame, StochasticDenoiseBindings> {
public:
    StochasticDenoisePass(std::shared_ptr<StochasticLightingResources> resources, StochasticDenoiseStage stage)
        : m_resources(std::move(resources)), m_stage(stage), m_state(std::make_shared<State>()) {
    }

    StochasticDenoiseBindings Declare(org::PassBuilder& builder) {
        StochasticDenoiseBindings bindings{};
        bindings.stage = m_stage;
        const bool temporal = m_stage == StochasticDenoiseStage::Temporal;
        bindings.pipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/Stochastic/stochasticDenoise.hlsl",
            temporal ? L"StochasticDenoiseTemporalCS" : L"StochasticDenoiseSpatialCS",
            temporal ? "Stochastic.DenoiseTemporal.PSO" : "Stochastic.DenoiseSpatial.PSO"));
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::Surface::BaseColorOpacity, Builtin::Surface::NormalRoughness,
            Builtin::Surface::SpecularAo);
        builder.ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }));
        // History pairs are read and written by parity, so both of each are declared for unordered access.
        for (uint32_t index = 0; index < 2u; ++index) {
            bindings.historyDiffuse[index] = builder.UnorderedAccess(m_resources->historyDiffuse[index]).View();
            bindings.historySpecular[index] = builder.UnorderedAccess(m_resources->historySpecular[index]).View();
        }
        bindings.variance = builder.UnorderedAccess(m_resources->variance).View();
        // The spatial pass reads the noisy input only for its paired diagnostic output (DENOISE_DEBUG_PAIR).
        bindings.noisyDiffuse = builder.ShaderResource(m_resources->noisyDiffuse).View();
        bindings.noisySpecular = builder.ShaderResource(m_resources->noisySpecular).View();
        if (temporal) {
            builder.ShaderResource(Builtin::Surface::Motion);
            for (uint32_t index = 0; index < 2u; ++index) {
                bindings.historyMoments[index] = builder.UnorderedAccess(m_resources->historyMoments[index]).View();
                bindings.historyChange[index] = builder.UnorderedAccess(m_resources->historyChange[index]).View();
                bindings.surfaceHistory[index] = builder.UnorderedAccess(m_resources->surfaceHistory[index]).View();
            }
        }
        else {
            bindings.outputDiffuse = builder.UnorderedAccess(Builtin::StochasticLighting::Diffuse).View();
            bindings.outputSpecular = builder.UnorderedAccess(Builtin::StochasticLighting::Specular).View();
        }
        builder.PreferQueue(org::QueueKind::Compute);
        return bindings;
    }

    StochasticDenoiseFrame Prepare(const StochasticDenoiseBindings&, const org::PassPrepareContext& preparation) const {
        StochasticDenoiseFrame frame{};
        const auto* update = preparation.preparationData->Get<UpdateContext>();
        if (!update) {
            return frame;
        }
        frame.width = update->renderResolution.x;
        frame.height = update->renderResolution.y;
        frame.frame = static_cast<uint32_t>(update->frameNumber);
        frame.parity = static_cast<uint32_t>(update->frameNumber & 1u);
        frame.downsample = m_resources->downsample;
        // Last frame's history is this pass's only if it ran the frame before (not on its first frame after a graph build).
        std::scoped_lock lock(m_state->mutex);
        if (m_state->lastFrame + 1u == update->frameNumber) {
            frame.flags |= STOCHASTIC_FLAG_HISTORY_VALID;
        }
        m_state->lastFrame = update->frameNumber;
        return frame;
    }

    static void Record(const StochasticDenoiseBindings& bindings, const StochasticDenoiseFrame& frame, org::PassRecordContext& recording) {
        if (frame.width == 0u || frame.height == 0u) {
            return;
        }
        const uint32_t current = frame.parity;
        const uint32_t previous = frame.parity ^ 1u;
        br::render::MiscConstants constants{};
        constants[STOCHASTIC_FRAME] = frame.frame;
        constants[STOCHASTIC_FLAGS] = frame.flags;
        constants[STOCHASTIC_DOWNSAMPLE_FACTOR] = frame.downsample;
        constants[STOCHASTIC_DENOISE_HISTORY_DIFFUSE_UAV] = recording.Index(bindings.historyDiffuse[current]);
        constants[STOCHASTIC_DENOISE_HISTORY_SPECULAR_UAV] = recording.Index(bindings.historySpecular[current]);
        constants[STOCHASTIC_DENOISE_VARIANCE_UAV] = recording.Index(bindings.variance);
        constants[STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV] = recording.Index(bindings.noisyDiffuse);
        constants[STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV] = recording.Index(bindings.noisySpecular);
        if (bindings.stage == StochasticDenoiseStage::Temporal) {
            constants[STOCHASTIC_DENOISE_HISTORY_MOMENTS_UAV] = recording.Index(bindings.historyMoments[current]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_DIFFUSE_UAV] = recording.Index(bindings.historyDiffuse[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_SPECULAR_UAV] = recording.Index(bindings.historySpecular[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_MOMENTS_UAV] = recording.Index(bindings.historyMoments[previous]);
            constants[STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV] = recording.Index(bindings.historyChange[current]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV] = recording.Index(bindings.historyChange[previous]);
            constants[STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV] = recording.Index(bindings.surfaceHistory[previous]);
        }
        else {
            constants[STOCHASTIC_DIFFUSE_UAV] = recording.Index(bindings.outputDiffuse);
            constants[STOCHASTIC_SPECULAR_UAV] = recording.Index(bindings.outputSpecular);
        }
        recording.BindComputePipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        recording.Commands().Dispatch((frame.width + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE,
            (frame.height + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE, 1u);
    }

private:
    struct State {
        std::mutex mutex;
        uint64_t lastFrame = UINT64_MAX;
    };
    std::shared_ptr<StochasticLightingResources> m_resources;
    StochasticDenoiseStage m_stage = StochasticDenoiseStage::Temporal;
    std::shared_ptr<State> m_state;
};

} // namespace br::render
