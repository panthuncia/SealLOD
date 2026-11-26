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
#include "BasicRenderer/Extensions/PassRevision.h"
#include <BasicRenderer/Pipeline/PipelineDesc.h>
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Lighting/Stochastic/StochasticLightingResources.h"
#include "../shaders/PerPassRootConstants/stochasticLightingRootConstants.h"

namespace br::render {

enum class StochasticDenoiseStage : uint8_t {
    Temporal, // Demodulate, reproject, rectify, accumulate; writes the history.
    Spatial, // Edge-stopped sparse filter of the history; remodulates into StochasticLighting::Diffuse and ::Specular.
};

// One run's values, made by Update each run. Since M6 step 9c the passes are recorded ahead and kept: the shaders take the frame
// number and parity from the per-frame buffer (stochasticLightingRootConstants.h), and the rest is the frame data and the revision.
struct StochasticDenoiseRun {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t frame = 0u;
    uint32_t flags = 0u;
    uint32_t parity = 0u;
    uint32_t downsample = 1u; // The noisy inputs' (docs/stochastic_lighting.md "Reduced resolution").
    bool operator==(const StochasticDenoiseRun&) const = default;
};

// What the history validity depends on, and this run's values (under the mutex).
struct StochasticDenoiseState {
    std::mutex mutex;
    uint64_t lastFrame = UINT64_MAX;
    bool lastNormalize = false;
    bool lastPaired = false;
    bool lastMixture = false;
    bool lastGradient = false;
    StochasticDenoiseRun run{};
};

// What Record bakes: the run without its frame number and parity.
struct StochasticDenoiseFrame {
    StochasticDenoiseRun run{};
    bool operator==(const StochasticDenoiseFrame&) const = default;
};

struct StochasticDenoiseBindings {
    br::PipelineState pipeline;
    StochasticDenoiseStage stage = StochasticDenoiseStage::Temporal;
    org::DeclaredViewToken noisyDiffuse, noisySpecular, variance, outputDiffuse, outputSpecular, guide, signature, gradient;
    std::array<org::DeclaredViewToken, 2> historyDiffuse{}, historySpecular{}, historyMoments{}, historyChange{}, surfaceHistory{}, mixtureHistory{};
};

// Stochastic lighting's denoiser (docs/stochastic_lighting.md "Denoiser"), shaders/Stochastic/stochasticDenoise.hlsl.
class StochasticDenoisePass
    : public org::TypedRenderGraphPass<StochasticDenoisePass, StochasticDenoiseFrame, StochasticDenoiseBindings> {
public:
    StochasticDenoisePass(std::shared_ptr<StochasticLightingResources> resources, StochasticDenoiseStage stage)
        : m_resources(std::move(resources)), m_stage(stage), m_state(std::make_shared<StochasticDenoiseState>()) {
    }

    StochasticDenoiseBindings Declare(org::PassBuilder& builder) {
        StochasticDenoiseBindings bindings{};
        bindings.stage = m_stage;
        const bool temporal = m_stage == StochasticDenoiseStage::Temporal;
        auto desc = br::render::ComputePipeline(L"Shaders/Stochastic/stochasticDenoise.hlsl",
            temporal ? L"StochasticDenoiseTemporalCS" : L"StochasticDenoiseSpatialCS",
            temporal ? "Stochastic.DenoiseTemporal.PSO" : "Stochastic.DenoiseSpatial.PSO");
        // The temporal pass compiled for this graph's resolution, its groupshared region sized for it (reduced resolution:
        // 4,608 bytes instead of 9,728; docs/performance/stochastic-lighting-experiments.md "Performance pass (2026-11-17)"). The downsample is fixed
        // per graph (changing the setting rebuilds it).
        if (temporal) {
            desc.defines.push_back({ L"DENOISE_DOWNSAMPLED", m_resources->downsample > 1u ? L"1" : L"0" });
        }
        bindings.pipeline = br::render::DeclarePipeline(desc);
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
        // Guide-normalized accumulation (docs/stochastic_lighting.md "Guide-normalized accumulation"): bound whether or not
        // stochasticLightingNormalize is on, so switching it is per frame.
        bindings.guide = builder.ShaderResource(m_resources->guide).View();
        // The change history: the temporal pass's, and with the paired ratio (docs/stochastic_lighting.md "The paired ratio")
        // the spatial pass reads the unshadowed estimate's history from it. Bound in every mode, so the setting is per frame.
        for (uint32_t index = 0; index < 2u; ++index) {
            bindings.historyChange[index] = builder.UnorderedAccess(m_resources->historyChange[index]).View();
        }
        // The estimator's surface history: the temporal pass validates its reprojection against last frame's, the spatial pass
        // reads its taps' depth and normal from this frame's (DENOISE_SPATIAL_SURFACE_HISTORY, docs/stochastic_lighting.md
        // "Performance pass (2026-11-17)").
        for (uint32_t index = 0; index < 2u; ++index) {
            bindings.surfaceHistory[index] = builder.UnorderedAccess(m_resources->surfaceHistory[index]).View();
        }
        if (temporal) {
            builder.ShaderResource(Builtin::Surface::Motion);
            for (uint32_t index = 0; index < 2u; ++index) {
                bindings.historyMoments[index] = builder.UnorderedAccess(m_resources->historyMoments[index]).View();
                // Light-mixture confidence (docs/stochastic_lighting.md "Guide-normalized accumulation"): bound in every mode, so the
                // setting is per frame.
                bindings.mixtureHistory[index] = builder.UnorderedAccess(m_resources->mixtureHistory[index]).View();
            }
            bindings.signature = builder.ShaderResource(m_resources->signature).View();
            // Visibility gradients (docs/stochastic_lighting.md "Visibility gradients"): the estimator's
            // held gradient. Bound in every mode where it exists, so the setting is per frame.
            if (m_resources->gradient) {
                bindings.gradient = builder.UnorderedAccess(m_resources->gradient).View();
            }
        }
        else {
            bindings.outputDiffuse = builder.UnorderedAccess(Builtin::StochasticLighting::Diffuse).View();
            bindings.outputSpecular = builder.UnorderedAccess(Builtin::StochasticLighting::Specular).View();
        }
        builder.PreferQueue(org::QueueKind::Compute);
        m_pipeline = bindings.pipeline;
        return bindings;
    }

    void Update(const org::UpdateExecutionContext& execution) override {
        const auto* update = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        StochasticDenoiseRun frame{};
        if (update) frame = MakeRun(*update);
        std::scoped_lock lock(m_state->mutex);
        m_state->run = frame;
    }
    // The run as Update made it (Revision is asked after Update), without what the shaders read from the per-frame buffer.
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        const StochasticDenoiseFrame frame = MakeFrame();
        br::render::AppendRevisionRows(out, &frame.run, 1u);
    }

    StochasticDenoiseFrame Prepare(const StochasticDenoiseBindings&, const org::PassPrepareContext&) const { return MakeFrame(); }

private:
    StochasticDenoiseFrame MakeFrame() const {
        StochasticDenoiseFrame frame{};
        {
            std::scoped_lock lock(m_state->mutex);
            frame.run = m_state->run;
        }
        frame.run.frame = 0u;
        frame.run.parity = 0u;
        return frame;
    }

    StochasticDenoiseRun MakeRun(const UpdateContext& context) const {
        const auto* update = &context;
        StochasticDenoiseRun frame{};
        frame.width = update->renderResolution.x;
        frame.height = update->renderResolution.y;
        frame.frame = static_cast<uint32_t>(update->frameNumber);
        frame.parity = static_cast<uint32_t>(update->frameNumber & 1u);
        frame.downsample = m_resources->downsample;
        const bool normalize = SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingNormalize")();
        const bool paired = normalize && SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingPaired")();
        const bool mixture = normalize && SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingMixtureConfidence")();
        const bool gradient = normalize && m_resources->gradient
            && SettingsManager::GetInstance().getSettingGetter<bool>("stochasticLightingVisibilityGradient")();
        if (normalize) {
            frame.flags |= STOCHASTIC_FLAG_NORMALIZE;
        }
        if (paired) {
            frame.flags |= STOCHASTIC_FLAG_PAIRED;
        }
        if (mixture) {
            frame.flags |= STOCHASTIC_FLAG_MIXTURE;
        }
        if (gradient) {
            frame.flags |= STOCHASTIC_FLAG_GRADIENT;
        }
        // Last frame's history is this pass's only if it ran the frame before (not on its first frame after a graph build), in
        // the same domain (a history of radiance is no history of ratios to the guide, and the reverse; without the paired
        // ratio the change history holds no unshadowed estimate; without light-mixture confidence no mixture history was kept).
        std::scoped_lock lock(m_state->mutex);
        if (m_state->lastFrame + 1u == update->frameNumber && m_state->lastNormalize == normalize && m_state->lastPaired == paired
            && m_state->lastMixture == mixture && m_state->lastGradient == gradient) {
            frame.flags |= STOCHASTIC_FLAG_HISTORY_VALID;
        }
        m_state->lastFrame = update->frameNumber;
        m_state->lastNormalize = normalize;
        m_state->lastPaired = paired;
        m_state->lastMixture = mixture;
        m_state->lastGradient = gradient;
        return frame;
    }

public:
    static void Record(const StochasticDenoiseBindings& bindings, const StochasticDenoiseFrame& data, org::PassRecordContext& recording) {
        const StochasticDenoiseRun& frame = data.run;
        if (frame.width == 0u || frame.height == 0u) {
            return;
        }
        // Each pair's [0] in this frame's slot and [1] in last frame's (the shaders swap them on odd frames). The surface
        // history's [1] goes in slot 22 (stochasticLightingRootConstants.h), and both stages bind every pair they read.
        const uint32_t current = 0u;
        const uint32_t previous = 1u;
        br::render::MiscConstants constants{};
        constants[STOCHASTIC_FLAGS] = frame.flags;
        constants[STOCHASTIC_DOWNSAMPLE_FACTOR] = frame.downsample;
        constants[STOCHASTIC_DENOISE_HISTORY_DIFFUSE_UAV] = recording.Index(bindings.historyDiffuse[current]);
        constants[STOCHASTIC_DENOISE_HISTORY_SPECULAR_UAV] = recording.Index(bindings.historySpecular[current]);
        constants[STOCHASTIC_DENOISE_VARIANCE_UAV] = recording.Index(bindings.variance);
        constants[STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV] = recording.Index(bindings.noisyDiffuse);
        constants[STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV] = recording.Index(bindings.noisySpecular);
        constants[STOCHASTIC_DENOISE_GUIDE_SRV] = recording.Index(bindings.guide);
        // The temporal pass writes this frame's; the spatial pass reads it (the paired ratio).
        constants[STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV] = recording.Index(bindings.historyChange[current]);
        if (bindings.stage == StochasticDenoiseStage::Temporal) {
            constants[STOCHASTIC_DENOISE_HISTORY_MOMENTS_UAV] = recording.Index(bindings.historyMoments[current]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_DIFFUSE_UAV] = recording.Index(bindings.historyDiffuse[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_SPECULAR_UAV] = recording.Index(bindings.historySpecular[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_MOMENTS_UAV] = recording.Index(bindings.historyMoments[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV] = recording.Index(bindings.historyChange[previous]);
            constants[STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV] = recording.Index(bindings.surfaceHistory[current]);
            constants[STOCHASTIC_DENOISE_SIGNATURE_SRV] = recording.Index(bindings.signature);
            constants[STOCHASTIC_DENOISE_MIXTURE_UAV] = recording.Index(bindings.mixtureHistory[current]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_MIXTURE_UAV] = recording.Index(bindings.mixtureHistory[previous]);
            constants[kSurfaceHistoryOddSlot] = recording.Index(bindings.surfaceHistory[previous]);
            if (bindings.gradient.layout) {
                constants[STOCHASTIC_DENOISE_GRADIENT_UAV] = recording.Index(bindings.gradient);
            }
        }
        else {
            constants[STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV] = recording.Index(bindings.surfaceHistory[current]);
            constants[kSurfaceHistoryOddSlot] = recording.Index(bindings.surfaceHistory[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_DIFFUSE_UAV] = recording.Index(bindings.historyDiffuse[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_SPECULAR_UAV] = recording.Index(bindings.historySpecular[previous]);
            constants[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV] = recording.Index(bindings.historyChange[previous]);
            constants[STOCHASTIC_DIFFUSE_UAV] = recording.Index(bindings.outputDiffuse);
            constants[STOCHASTIC_SPECULAR_UAV] = recording.Index(bindings.outputSpecular);
        }
        recording.BindComputePipeline(bindings.pipeline);
        br::render::PushMiscConstants(recording, constants);
        recording.Commands().Dispatch((frame.width + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE,
            (frame.height + STOCHASTIC_TILE_SIZE - 1u) / STOCHASTIC_TILE_SIZE, 1u);
    }

private:
    // The surface history's [1] (the shaders' STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV pair, stochasticLightingRootConstants.h).
    static constexpr uint32_t kSurfaceHistoryOddSlot = 22u;

    std::shared_ptr<StochasticLightingResources> m_resources;
    StochasticDenoiseStage m_stage = StochasticDenoiseStage::Temporal;
    std::shared_ptr<StochasticDenoiseState> m_state;
    br::PipelineState m_pipeline;
};

} // namespace br::render
