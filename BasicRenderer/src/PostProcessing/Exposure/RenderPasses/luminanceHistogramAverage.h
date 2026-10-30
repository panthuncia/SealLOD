#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/luminanceHistogramAverageRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct LuminanceHistogramAverageBindings {
    org::PipelineState pipeline;
};
struct LuminanceHistogramAverageFrame {
    float deltaTime = 0.0f;
    uint32_t pixelCount = 0;
};

class LuminanceHistogramAveragePass : public org::TypedRenderGraphPass<LuminanceHistogramAveragePass, LuminanceHistogramAverageFrame, LuminanceHistogramAverageBindings> {
public:
    LuminanceHistogramAveragePass() {
        CreateComputePSO();
    }

    LuminanceHistogramAverageBindings Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.UnorderedAccess(Builtin::PostProcessing::LuminanceHistogram, Builtin::PostProcessing::AdaptedLuminance, "FFX::LPMConstants");
        return {m_pso};
    }

    void Initialize() {
		// Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    LuminanceHistogramAverageFrame Prepare(const LuminanceHistogramAverageBindings&, const org::PassPrepareContext& preparation) const {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        return {context->deltaTime, context->renderResolution.x * context->renderResolution.y};
    }

    static void Record(const LuminanceHistogramAverageBindings& bindings, const LuminanceHistogramAverageFrame& frame,
        org::PassRecordContext& context) {
        br::render::MiscConstants constants{};
        constants[MIN_LOG_LUMINANCE] = as_uint(0.001f);
        constants[LOG_LUMINANCE_RANGE] = as_uint(log2(10.0f) - log2(0.1f));
        constants[TIME_COEFFICIENT] = as_uint(frame.deltaTime);
        constants[NUM_PIXELS] = as_uint(static_cast<float>(frame.pixelCount));
        br::render::DispatchCompute(context, bindings.pipeline, constants, 1);
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
			L"shaders/PostProcessing/LuminanceHistogramAverage.hlsl",
			L"CSMain",
			{},
			"LuminanceHistogramAverageCS");
    }
};
