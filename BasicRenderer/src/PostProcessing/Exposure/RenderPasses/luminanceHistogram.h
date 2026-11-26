#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/luminanceHistogramRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include <BasicRenderer/Extensions/PipelineDescs.h>
#include "BasicRenderer/Extensions/PassRevision.h"

struct LuminanceHistogramBindings {
    br::PipelineState pipeline;
};
struct LuminanceHistogramFrame {
    uint32_t groupsX = 0, groupsY = 0;
    bool operator==(const LuminanceHistogramFrame&) const = default;
};

class LuminanceHistogramPass : public org::TypedRenderGraphPass<LuminanceHistogramPass, LuminanceHistogramFrame, LuminanceHistogramBindings> {
public:
    LuminanceHistogramBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.ShaderResource(Builtin::Color::HDRColorTarget);
        builder.UnorderedAccess(Builtin::PostProcessing::LuminanceHistogram);
        m_pipeline = br::render::DeclarePipeline(br::ComputePipelineDesc{
            .shader = {L"shaders/PostProcessing/LuminanceHistogram.hlsl", L"CSMain"},
            .name = "LuminanceHistogramPassCS"});
        return {m_pipeline};
    }

    // Kept recordings (M6 step 4): the dispatch follows the render resolution, which Update takes from the frame.
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



    LuminanceHistogramFrame Prepare(const LuminanceHistogramBindings& bindings, const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        const auto sampledWidth = (context->renderResolution.x + 3u) / 4u;
        const auto sampledHeight = (context->renderResolution.y + 3u) / 4u;
        return {(sampledWidth + 15u) / 16u, (sampledHeight + 15u) / 16u};
    }

    static void Record(const LuminanceHistogramBindings& bindings, const LuminanceHistogramFrame& frame, org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[MIN_LOG_LUMINANCE] = as_uint(0.001f);
        constants[INVERSE_LOG_LUM_RANGE] = as_uint(1.0f / (log2(10.0f) - log2(0.1f)));
        br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groupsX, frame.groupsY);
    }

    void ShutdownPass() {
        // Cleanup if necessary
    }

private:
    br::PipelineState m_pipeline;
    DirectX::XMUINT2 m_renderResolution{};
};
