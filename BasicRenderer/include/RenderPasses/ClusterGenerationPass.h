#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"

class ClusterGenerationPass : public ComputePass {
public:
	ClusterGenerationPass() {
		getClusterSize = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT3>("lightClusterSize");
		CreatePSO();
	}

	~ClusterGenerationPass() {
	}

	void DeclareResourceUsages(ComputePassBuilder* builder) override {
		builder->WithShaderResource(Builtin::CameraBuffer)
			.WithUnorderedAccess(Builtin::Light::ClusterBuffer);
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
	}

	void Setup() override {
	}

	PassReturn Execute(PassExecutionContext& executionContext) override {
	    auto* renderContext = executionContext.hostData->Get<RenderContext>();
	    auto& context = *renderContext;
		auto& commandList = executionContext.commandList;

		// Set the descriptor heaps
		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

		commandList.BindLayout(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
		commandList.BindPipeline(m_PSO.GetAPIPipelineState().GetHandle());

		BindResourceDescriptorIndices(commandList, m_PSO.GetResourceDescriptorSlots());

		auto clusterSize = getClusterSize();
		commandList.Dispatch(clusterSize.x, clusterSize.y, clusterSize.z);
		return {};
	}

	void Cleanup() override {

	}

private:

	void CreatePSO() {
		m_PSO = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
			L"shaders/clustering.hlsl",
			L"CSMain",
			{},
			"Light cluster generation CS");
	}

	std::function<DirectX::XMUINT3()> getClusterSize;
	PipelineState m_PSO;
};