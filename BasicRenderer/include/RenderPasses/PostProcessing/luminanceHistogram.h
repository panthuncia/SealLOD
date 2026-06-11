#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Utilities/Utilities.h"
#include "../shaders/PerPassRootConstants/luminanceHistogramRootConstants.h"

class LuminanceHistogramPass : public ComputePass {
public:
    LuminanceHistogramPass() {
        CreateComputePSO();
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithShaderResource(Builtin::Color::HDRColorTarget)
            .WithUnorderedAccess(Builtin::PostProcessing::LuminanceHistogram);
    }

    void Setup() override {
		// Removed redundant Register calls now covered by declared-resource auto descriptor registration
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        auto& psoManager = PSOManager::GetInstance();
        auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

        // Set the compute pipeline state
		commandList.BindLayout(psoManager.GetComputeRootSignature().GetHandle());
		commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());

        uint32_t passConstants[NumMiscUintRootConstants] = {};
        passConstants[MIN_LOG_LUMINANCE] = as_uint(0.001f); // Minimum log luminance value
        passConstants[INVERSE_LOG_LUM_RANGE] = as_uint(1.0f / (log2(10.0f) - log2(0.1f))); // Inverse range for log luminance

        commandList.PushConstants(rhi::ShaderStage::Compute, 0, MiscUintRootSignatureIndex, 0, NumMiscUintRootConstants, passConstants);

		BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

        // Dispatch
        // In luminance histogram each thread group handles a 16x16 block
        unsigned int x = (context.renderResolution.x + 16 - 1) / 16;
        unsigned int y = (context.renderResolution.y + 16 - 1) / 16;
        commandList.Dispatch(x, y, 1);

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
			L"shaders/PostProcessing/LuminanceHistogram.hlsl",
			L"CSMain",
		    {}, 
            "LuminanceHistogramPassCS");
    }
};
