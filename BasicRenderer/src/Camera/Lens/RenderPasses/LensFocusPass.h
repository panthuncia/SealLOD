#pragma once

#include <algorithm>
#include <bit>
#include <memory>

#include "BuiltinResources.h"
#include "Camera/Lens/LensManager.h"
#include "Render/PassBuilders.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/PipelineDescs.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/RenderContext.h"

// The physical lens's per-frame focus (lensFocus.hlsl, SARP's docs/lens_simulation.md "Focus and autofocus"). LensFocusPass runs
// before the path tracer and sets this frame's focus in Builtin::RayTracing::LensFocus: the manual distance, or a step of the
// autofocus spring toward what LensAutofocusPass measured after the previous frame's path trace. Both are one small dispatch,
// only while a lens is published. Nothing comes back to the CPU in the loop; LensManager reads the state back every few frames
// for lens.get and the menu.

struct LensFocusFrameData {
    br::lens::LensFocusParameters parameters;
    float deltaTime = 0.0f;
};

struct LensFocusBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken focus;
};

class LensFocusPass : public org::TypedRenderGraphPass<LensFocusPass, LensFocusFrameData, LensFocusBindings> {
public:
    explicit LensFocusPass(std::shared_ptr<br::lens::LensManager> lens) : m_lens(std::move(lens)) {}

    LensFocusBindings Declare(org::PassBuilder& builder) {
        LensFocusBindings bindings{};
        bindings.pipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/lensFocus.hlsl", L"LensFocusUpdateCS", "Lens.Focus.PSO"));
        // The camera, for the frame's sensor frames (its projection and the previous one).
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::RayTracing::LensData);
        bindings.focus = builder.UnorderedAccess(Builtin::RayTracing::LensFocus).View();
        return bindings;
    }

    LensFocusFrameData Prepare(const LensFocusBindings&, const org::PassPrepareContext& preparation) const {
        LensFocusFrameData frame{};
        frame.parameters = m_lens->FocusParameters();
        const auto* context = preparation.preparationData ? preparation.preparationData->Get<UpdateContext>() : nullptr;
        // A frame time of 0 (the statistical sampler's deterministic frames) still moves the spring, at 60 Hz.
        const float dt = context ? context->deltaTime : 0.0f;
        frame.deltaTime = dt > 0.0f ? std::min(dt, 0.1f) : 1.0f / 60.0f;
        return frame;
    }

    static void Record(const LensFocusBindings& bindings, const LensFocusFrameData& frame, org::PassRecordContext& recording) {
        if (!frame.parameters.active) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.focus);
        constants[1] = frame.parameters.mode;
        constants[2] = std::bit_cast<uint32_t>(frame.parameters.manualShiftMm);
        constants[3] = std::bit_cast<uint32_t>(frame.deltaTime);
        constants[4] = std::bit_cast<uint32_t>(frame.parameters.rate);
        constants[5] = std::bit_cast<uint32_t>(frame.parameters.deadBandMm);
        constants[6] = frame.parameters.identity;
        constants[7] = std::bit_cast<uint32_t>(frame.parameters.epsilonMm);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, 1u, 1u);
    }

private:
    std::shared_ptr<br::lens::LensManager> m_lens;
};

struct LensAutofocusBindings {
    org::PipelineState pipeline;
    org::DeclaredViewToken focus, depth;
};

// After the path tracer: the primary hits' depth over the autofocus region, as the next frame's target. It measures in manual
// mode too, so lens.get reports what autofocus would pick.
class LensAutofocusPass : public org::TypedRenderGraphPass<LensAutofocusPass, br::lens::LensFocusParameters, LensAutofocusBindings> {
public:
    explicit LensAutofocusPass(std::shared_ptr<br::lens::LensManager> lens) : m_lens(std::move(lens)) {}

    LensAutofocusBindings Declare(org::PassBuilder& builder) {
        LensAutofocusBindings bindings{};
        bindings.pipeline = builder.Pipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/lensFocus.hlsl", L"LensAutofocusCS", "Lens.Autofocus.PSO"));
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::RayTracing::LensData);
        bindings.depth = builder.ShaderResource(Builtin::PrimaryCamera::ProjectedDepthTexture);
        bindings.focus = builder.UnorderedAccess(Builtin::RayTracing::LensFocus).View();
        return bindings;
    }

    br::lens::LensFocusParameters Prepare(const LensAutofocusBindings&, const org::PassPrepareContext&) const { return m_lens->FocusParameters(); }

    static void Record(const LensAutofocusBindings& bindings, const br::lens::LensFocusParameters& parameters, org::PassRecordContext& recording) {
        if (!parameters.active) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.focus);
        constants[1] = recording.Index(bindings.depth);
        constants[2] = std::bit_cast<uint32_t>(parameters.pointX);
        constants[3] = std::bit_cast<uint32_t>(parameters.pointY);
        constants[4] = std::bit_cast<uint32_t>(parameters.region);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, 1u, 1u);
    }

private:
    std::shared_ptr<br::lens::LensManager> m_lens;
};
