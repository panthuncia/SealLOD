#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/luminanceHistogramRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct LuminanceHistogramBindings {
    org::PipelineState pipeline;
};
struct LuminanceHistogramFrame {
    uint32_t groupsX = 0, groupsY = 0;
};

class LuminanceHistogramPass : public org::TypedRenderGraphPass<LuminanceHistogramPass, LuminanceHistogramFrame, LuminanceHistogramBindings> {
public:
    LuminanceHistogramPass() {
        CreateComputePSO();
    }

    LuminanceHistogramBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.ShaderResource(Builtin::Color::HDRColorTarget);
        builder.UnorderedAccess(Builtin::PostProcessing::LuminanceHistogram);
        return {m_pso};
    }

    void Initialize() {
		// Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    LuminanceHistogramFrame Prepare(const LuminanceHistogramBindings& bindings, const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        // A peer device (interop validation) binds its own build of the pipeline.
        if (preparation.backendInstance != org::BackendInstanceId::Primary) {
            PSOManager::GetInstance().ResolvePipeline(bindings.pipeline, preparation.backendInstance); // Builds the peer's pipeline once.
        }
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
    org::PipelineState m_pso;

    void CreateComputePSO()
    {
		m_pso = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
			L"shaders/PostProcessing/LuminanceHistogram.hlsl",
			L"CSMain",
		    {},
            "LuminanceHistogramPassCS");
    }
};
