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
#include "BasicRenderer/Extensions/PassRevision.h"

// The physical lens's per-frame focus (lensFocus.hlsl, SARP's docs/lens_simulation.md "Focus and autofocus"). LensFocusPass runs
// before the path tracer and sets this frame's focus in Builtin::RayTracing::LensFocus: the manual distance, or a step of the
// autofocus spring toward what LensAutofocusPass measured after the previous frame's path trace. Both are one small dispatch,
// only while a lens is published. Nothing comes back to the CPU in the loop; LensManager reads the state back every few frames
// for lens.get and the menu.

// What Record bakes: the focus parameters, taken by LensFocusPass::Update each run. The frame time is the per-frame buffer's
// (lensFocus.hlsl), so the pass is recorded ahead and kept (M6 step 9c), and the revision names the parameters.
struct LensFocusFrameData {
    br::lens::LensFocusParameters parameters{};
    bool operator==(const LensFocusFrameData& other) const {
        const auto& a = parameters;
        const auto& b = other.parameters;
        return a.active == b.active && a.mode == b.mode && a.identity == b.identity && a.manualShiftMm == b.manualShiftMm
            && a.rate == b.rate && a.deadBandMm == b.deadBandMm && a.epsilonMm == b.epsilonMm;
    }
};

struct LensFocusBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken focus;
};

class LensFocusPass : public org::TypedRenderGraphPass<LensFocusPass, LensFocusFrameData, LensFocusBindings> {
public:
    explicit LensFocusPass(std::shared_ptr<br::lens::LensManager> lens) : m_lens(std::move(lens)) {}

    LensFocusBindings Declare(org::PassBuilder& builder) {
        LensFocusBindings bindings{};
        bindings.pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/lensFocus.hlsl", L"LensFocusUpdateCS", "Lens.Focus.PSO"));
        // The camera, for the frame's sensor frames (its projection and the previous one).
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::RayTracing::LensData);
        bindings.focus = builder.UnorderedAccess(Builtin::RayTracing::LensFocus).View();
        m_pipeline = bindings.pipeline;
        return bindings;
    }

    // FocusParameters counts the focus pass's runs (LensManager's readback gate), so it is asked here, once a run.
    void Update(const org::UpdateExecutionContext&) override { m_frame.parameters = m_lens->FocusParameters(); }
    // Staged recordings: the parameters without counting a run.
    void TakeFrameValues(const org::UpdateExecutionContext&) override { m_frame.parameters = m_lens->PeekFocusParameters(); }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        const auto& p = m_frame.parameters;
        out.push_back(uint64_t(p.active) | (uint64_t(p.mode) << 1u) | (uint64_t(p.identity) << 32u));
        for (const float value : { p.manualShiftMm, p.rate, p.deadBandMm, p.epsilonMm }) br::render::AppendRevision(out, value);
    }

    LensFocusFrameData Prepare(const LensFocusBindings&, const org::PassPrepareContext&) const { return m_frame; }

    static void Record(const LensFocusBindings& bindings, const LensFocusFrameData& data, org::PassRecordContext& recording) {
        const auto& parameters = data.parameters;
        if (!parameters.active) return;
        br::render::MiscConstants constants{};
        constants[0] = recording.Index(bindings.focus);
        constants[1] = parameters.mode;
        constants[2] = std::bit_cast<uint32_t>(parameters.manualShiftMm);
        constants[4] = std::bit_cast<uint32_t>(parameters.rate);
        constants[5] = std::bit_cast<uint32_t>(parameters.deadBandMm);
        constants[6] = parameters.identity;
        constants[7] = std::bit_cast<uint32_t>(parameters.epsilonMm);
        br::render::DispatchCompute(recording, bindings.pipeline, constants, 1u, 1u);
    }

private:
    std::shared_ptr<br::lens::LensManager> m_lens;
    br::PipelineState m_pipeline;
    LensFocusFrameData m_frame{};
};

struct LensAutofocusBindings {
    br::PipelineState pipeline;
    org::DeclaredViewToken focus, depth;
};

// After the path tracer: the primary hits' depth over the autofocus region, as the next frame's target. It measures in manual
// mode too, so lens.get reports what autofocus would pick.
class LensAutofocusPass : public org::TypedRenderGraphPass<LensAutofocusPass, br::lens::LensFocusParameters, LensAutofocusBindings> {
public:
    explicit LensAutofocusPass(std::shared_ptr<br::lens::LensManager> lens) : m_lens(std::move(lens)) {}

    LensAutofocusBindings Declare(org::PassBuilder& builder) {
        LensAutofocusBindings bindings{};
        bindings.pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(L"Shaders/ClusterLOD/lensFocus.hlsl", L"LensAutofocusCS", "Lens.Autofocus.PSO"));
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        builder.ShaderResource(Builtin::CameraBuffer, Builtin::RayTracing::LensData);
        bindings.depth = builder.ShaderResource(Builtin::PrimaryCamera::ProjectedDepthTexture);
        bindings.focus = builder.UnorderedAccess(Builtin::RayTracing::LensFocus).View();
        m_pipeline = bindings.pipeline;
        return bindings;
    }

    // Kept recordings (M6 step 5): the parameters (settings and the published lens) are taken each run and named by the revision.
    // Staged recordings (M6 step 6): taken without counting a focus run (the focus pass's Update counts them).
    void TakeFrameValues(const org::UpdateExecutionContext&) override { m_parameters = m_lens->PeekFocusParameters(); }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        const auto& p = m_parameters;
        out.push_back(uint64_t(p.active) | (uint64_t(p.mode) << 1u) | (uint64_t(p.identity) << 32u));
        for (const float value : { p.manualShiftMm, p.rate, p.deadBandMm, p.epsilonMm, p.pointX, p.pointY, p.region })
            br::render::AppendRevision(out, value);
    }

    br::lens::LensFocusParameters Prepare(const LensAutofocusBindings&, const org::PassPrepareContext&) const { return m_parameters; }

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
    br::PipelineState m_pipeline;
    br::lens::LensFocusParameters m_parameters{};
};
