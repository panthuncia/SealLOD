#pragma once

#include <unordered_map>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "RenderPasses/PreparedComputeDispatch.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"

class PrimaryDepthCopyPass : public org::TypedRenderGraphPass<PrimaryDepthCopyPass, br::render::PreparedComputeDispatch> {
public:
	PrimaryDepthCopyPass() {
		CreatePSO();
	}

	void Declare(org::PassBuilder& declaration) {
        declaration.PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        auto* builder = &declaration;
		builder->WithShaderResource(
			Builtin::PrimaryCamera::VisibilityTexture)
			.WithUnorderedAccess(Builtin::PrimaryCamera::LinearDepthMap);
		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
	}

    br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation) {
        const auto& context = *preparation.preparationData->Get<UpdateContext>();
        br::render::PreparedComputeDispatch data{};
        data.resourceHeap = context.textureDescriptorHeap.GetHandle();
        data.samplerHeap = context.samplerDescriptorHeap.GetHandle();
        auto program = preparation.CaptureProgramBinding(m_pso);
        data.program = program.program;
        data.descriptorIndices = std::move(program.descriptorIndices);
        data.groupsX = (context.renderResolution.x + 7u) / 8u;
        data.groupsY = (context.renderResolution.y + 7u) / 8u;
        return data;
    }

    static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
        br::render::RecordPreparedComputeDispatch(data, recording);
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
