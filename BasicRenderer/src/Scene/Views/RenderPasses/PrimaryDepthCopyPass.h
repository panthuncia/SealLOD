#pragma once

#include <unordered_map>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"

struct PrimaryDepthCopyBindings {
	org::PipelineState pipeline;
};
struct PrimaryDepthCopyFrame {
	uint32_t groupsX = 0, groupsY = 0;
};

class PrimaryDepthCopyPass : public org::TypedRenderGraphPass<PrimaryDepthCopyPass, PrimaryDepthCopyFrame, PrimaryDepthCopyBindings> {
public:
	PrimaryDepthCopyPass() {
		CreatePSO();
	}

	PrimaryDepthCopyBindings Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
		builder->ShaderResource(Builtin::PrimaryCamera::VisibilityTexture);
		builder->UnorderedAccess(Builtin::PrimaryCamera::LinearDepthMap);
		builder->ConstantBuffer(Builtin::PerFrameBuffer);
		return {m_pso};
	}

    PrimaryDepthCopyFrame Prepare(const PrimaryDepthCopyBindings&, const org::PassPrepareContext& preparation) const {
        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        return {(context.renderResolution.x + 7u) / 8u, (context.renderResolution.y + 7u) / 8u};
    }

    static void Record(const PrimaryDepthCopyBindings& bindings, const PrimaryDepthCopyFrame& frame, org::PassRecordContext& context) {
        br::render::DispatchCompute(context, bindings.pipeline, {}, frame.groupsX, frame.groupsY);
    }

private:

	org::PipelineState m_pso;

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
