#pragma once

#include <unordered_map>

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"

class PrimaryDepthCopyPass : public ComputePass {
public:
	PrimaryDepthCopyPass() {
		CreatePSO();
	}

	void DeclareResourceUsages(ComputePassBuilder* builder) override {
		builder->WithShaderResource(
			Builtin::PrimaryCamera::VisibilityTexture)
			.WithUnorderedAccess(Builtin::PrimaryCamera::LinearDepthMap);
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
	}

	void Setup() override {
	}

	PassReturn Execute(PassExecutionContext& executionContext) override {
	    auto* renderContext = executionContext.hostData->Get<RenderContext>();
	    auto& context = *renderContext;
		auto& psoManager = PSOManager::GetInstance();
		auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(),
			context.samplerDescriptorHeap.GetHandle());

		commandList.BindLayout(psoManager.GetComputeRootSignature().GetHandle());

		commandList.BindPipeline(m_pso.GetAPIPipelineState().GetHandle());

		BindResourceDescriptorIndices(commandList, m_pso.GetResourceDescriptorSlots());

		uint32_t w = context.renderResolution.x;
		uint32_t h = context.renderResolution.y;
		const uint32_t groupSizeX = 8;
		const uint32_t groupSizeY = 8;
		uint32_t groupsX = (w + groupSizeX - 1) / groupSizeX;
		uint32_t groupsY = (h + groupSizeY - 1) / groupSizeY;

		commandList.Dispatch(groupsX, groupsY, 1);
		return {};
	}

	void Cleanup() override {
		// Cleanup the render pass
	}

private:

	PipelineState m_pso;

	void CreatePSO() {
		m_pso = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
			L"shaders/canonicalSurface.hlsl",
			L"PrimaryDepthCopyCS",
			{},
			"PrimaryDepthCopyPSO"
		);
	}
};
