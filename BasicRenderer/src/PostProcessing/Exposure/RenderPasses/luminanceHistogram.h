#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/luminanceHistogramRootConstants.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

class LuminanceHistogramPass : public org::TypedRenderGraphPass<LuminanceHistogramPass, br::render::PreparedComputeDispatch> {
public:
    LuminanceHistogramPass() {
        CreateComputePSO();
    }

    void Declare(org::PassBuilder& builder) {
        builder.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder.WithShaderResource(Builtin::Color::HDRColorTarget)
            .WithUnorderedAccess(Builtin::PostProcessing::LuminanceHistogram);
    }

    void Initialize() {
		// Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }



    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation) {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        auto payload = m_pso.GetPayload();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context->textureDescriptorHeap.GetHandle();
        data.samplerHeap = context->samplerDescriptorHeap.GetHandle();
        data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
        auto program = preparation.CaptureProgramBinding(std::move(payload));
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);


        data.constants[MIN_LOG_LUMINANCE] = as_uint(0.001f);
        data.constants[INVERSE_LOG_LUM_RANGE] = as_uint(1.0f / (log2(10.0f) - log2(0.1f)));
        const auto sampledWidth = (context->renderResolution.x + 3u) / 4u;
        const auto sampledHeight = (context->renderResolution.y + 3u) / 4u;
        data.groupsX = (sampledWidth + 15u) / 16u;
        data.groupsY = (sampledHeight + 15u) / 16u;
        return data;
    }

    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
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
