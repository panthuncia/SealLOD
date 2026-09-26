#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

class ClusterGenerationPass : public org::TypedRenderGraphPass<ClusterGenerationPass, br::render::PreparedComputeDispatch> {
public:
	ClusterGenerationPass() { CreatePSO(); }

	~ClusterGenerationPass() {
	}

	void Declare(org::PassBuilder& builder) {
		builder.WithShaderResource(Builtin::CameraBuffer)
			.WithUnorderedAccess(Builtin::Light::ClusterBuffer);
		builder.WithConstantBuffer(Builtin::PerFrameBuffer)
			.PreferQueue(org::QueueKind::Compute);
	}

	br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation) {
		const auto* update = preparation.preparationData->Get<UpdateContext>();
		const auto* render = preparation.preparationData->Get<RenderContext>();
		if (!update && !render) throw std::logic_error("ClusterGenerationPass requires frame context");
		br::render::PreparedComputeDispatch data{};
		data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
		auto program = CaptureProgramBinding(preparation, m_PSO);
		data.program = program.program;
		data.descriptorIndices = std::move(program.descriptorIndices);
		const auto size = update ? update->lightClusterSize : render->lightClusterSize;
		data.groupsX = size.x; data.groupsY = size.y; data.groupsZ = size.z;
		return data;
	}
	static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
		br::render::RecordPreparedComputeDispatch(data, recording);
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

	org::PipelineState m_PSO;
};
