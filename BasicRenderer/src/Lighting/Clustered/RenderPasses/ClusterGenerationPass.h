#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct ClusterGenerationBindings {
	org::PipelineState pipeline;
};
struct ClusterGenerationFrame {
	uint32_t groupsX = 0, groupsY = 0, groupsZ = 0;
};

class ClusterGenerationPass : public org::TypedRenderGraphPass<ClusterGenerationPass, ClusterGenerationFrame, ClusterGenerationBindings> {
public:
	ClusterGenerationPass() { CreatePSO(); }

	~ClusterGenerationPass() {
	}

	ClusterGenerationBindings Declare(org::PassBuilder& builder) {
		builder.ShaderResource(Builtin::CameraBuffer);
		builder.UnorderedAccess(Builtin::Light::ClusterBuffer);
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
		builder.PreferQueue(org::QueueKind::Compute);
		return {m_PSO};
	}

	ClusterGenerationFrame Prepare(const ClusterGenerationBindings&, const org::PassPrepareContext& preparation) const {
		const auto* update = preparation.preparationData->Get<UpdateContext>();
		const auto* render = preparation.preparationData->Get<RenderContext>();
		if (!update && !render) throw std::logic_error("ClusterGenerationPass requires frame context");
		const auto size = update ? update->lightClusterSize : render->lightClusterSize;
		return {size.x, size.y, size.z};
	}
	static void Record(const ClusterGenerationBindings& bindings, const ClusterGenerationFrame& frame, org::PassRecordContext& context) {
		br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY, frame.groupsZ);
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
