#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/luminanceHistogramAverageRootConstants.h"

class LuminanceHistogramAveragePass : public ComputePass {
public:
    LuminanceHistogramAveragePass() {
        CreateComputePSO();
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithUnorderedAccess(Builtin::PostProcessing::LuminanceHistogram, Builtin::PostProcessing::AdaptedLuminance, "FFX::LPMConstants");
    }

    void Setup() override {
		// Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(executionContext.GetResourceDescriptorHeap().GetHandle(),
			executionContext.GetSamplerDescriptorHeap().GetHandle());

        // Set the compute pipeline state
		commandList.BindLayout(psoManager.GetComputeRootSignature(executionContext.backendInstance).GetHandle());
		commandList.BindPipeline(psoManager.ResolvePipeline(m_pso, executionContext.backendInstance).GetHandle());

		uint32_t passConstants[NumMiscUintRootConstants] = {};
        passConstants[MIN_LOG_LUMINANCE] = as_uint(0.001f); // Minimum log luminance value
        passConstants[LOG_LUMINANCE_RANGE] = as_uint(log2(10.0f) - log2(0.1f)); // range for log luminance
        passConstants[TIME_COEFFICIENT] = as_uint(context.deltaTime);
		passConstants[NUM_PIXELS] = as_uint(static_cast<float>(context.renderResolution.x * context.renderResolution.y));

		commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, passConstants);

        BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

		// Dispatch the compute shader
        commandList.Dispatch(1, 1, 1);

        return {};
    }

    void Cleanup() override {
        // Cleanup if necessary
    }

private:
    PipelineState m_pso;

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
