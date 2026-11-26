#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/luminanceHistogramAverageRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "BasicRenderer/Extensions/PassRevision.h"

struct LuminanceHistogramAverageBindings {
    br::PipelineState pipeline;
};
// The adaptation's elapsed time is the per-frame buffer's deltaTime (M6 step 4), so a kept recording of the pass adapts each
// frame too.
struct LuminanceHistogramAverageFrame {
    uint32_t pixelCount = 0;
    bool operator==(const LuminanceHistogramAverageFrame&) const = default;
};

class LuminanceHistogramAveragePass : public org::TypedRenderGraphPass<LuminanceHistogramAveragePass, LuminanceHistogramAverageFrame, LuminanceHistogramAverageBindings> {
public:
    LuminanceHistogramAverageBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.UnorderedAccess(Builtin::PostProcessing::LuminanceHistogram, Builtin::PostProcessing::AdaptedLuminance, "FFX::LPMConstants");
        builder.ConstantBuffer(Builtin::PerFrameBuffer);
        m_pipeline = br::render::DeclarePipeline(br::render::ComputePipeline(
            L"shaders/PostProcessing/LuminanceHistogramAverage.hlsl", L"CSMain", "LuminanceHistogramAverageCS"));
        return {m_pipeline};
    }

    // Staged recordings (M6 step 6): the values the revision names, taken without side effects (RenderGraphPass::TakeFrameValues).
    void TakeFrameValues(const org::UpdateExecutionContext& execution) override {
        if (const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr) m_renderResolution = context->renderResolution;
    }
    void Revision(std::vector<uint64_t>& out) const {
        br::render::AppendPipelineRevision(out, m_pipeline);
        br::render::AppendRevision(out, m_renderResolution.x);
        br::render::AppendRevision(out, m_renderResolution.y);
    }

    void Initialize() {
		// Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    LuminanceHistogramAverageFrame Prepare(const LuminanceHistogramAverageBindings&, const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        return {context->renderResolution.x * context->renderResolution.y};
    }

    static void Record(const LuminanceHistogramAverageBindings& bindings, const LuminanceHistogramAverageFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[MIN_LOG_LUMINANCE] = as_uint(0.001f);
        constants[LOG_LUMINANCE_RANGE] = as_uint(log2(10.0f) - log2(0.1f));
        constants[NUM_PIXELS] = as_uint(static_cast<float>(frame.pixelCount));
        br::render::DispatchCompute(context, bindings.pipeline, constants, 1);
    }

    void ShutdownPass() {
        // Cleanup if necessary
    }

private:
    br::PipelineState m_pipeline;
    DirectX::XMUINT2 m_renderResolution{};
};
