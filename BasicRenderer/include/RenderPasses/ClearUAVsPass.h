#pragma once

#include "RenderPasses/Base/RenderPass.h"
#include "Render/RenderContext.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/ResourceManager.h"
#include "Scene/Scene.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Scene/Components.h"
#include "boost/container_hash/hash.hpp"

struct ClearIndirectDrawCommandUAVPassInputs {
	bool clearBlend;

	RG_DEFINE_PASS_INPUTS(ClearIndirectDrawCommandUAVPassInputs, &ClearIndirectDrawCommandUAVPassInputs::clearBlend);
};

class ClearIndirectDrawCommandUAVsPass : public RenderPass {
public:
	ClearIndirectDrawCommandUAVsPass() {}

	void DeclareResourceUsages(RenderPassBuilder* builder) override {
		auto inputs = Inputs<ClearIndirectDrawCommandUAVPassInputs>();
		m_clearBlend = inputs.clearBlend;

		auto ecsWorld = RendererECSManager::GetInstance().GetWorld();
		auto blendEntity = RendererECSManager::GetInstance().GetRenderPhaseEntity(Engine::Primary::OITAccumulationPass);
		m_nonBlendQuery = ECSResourceResolver(ecsWorld.query_builder<>()
			.with<Components::IsIndirectArguments>()
			.without<Components::ParticipatesInPass>(blendEntity)
			.build());
		builder->WithCopyDest(ECSResourceResolver(m_nonBlendQuery));
		if (m_clearBlend) {
			m_blendQuery = ECSResourceResolver(ecsWorld.query_builder<>()
				.with<Components::IsIndirectArguments>()
				.with<Components::ParticipatesInPass>(blendEntity)
				.build());
			builder->WithCopyDest(ECSResourceResolver(m_blendQuery));
		}
	}
  
	void Setup() override {

		auto& ecsWorld = RendererECSManager::GetInstance().GetWorld();
		lightQuery = ecsWorld.query_builder<Components::LightViewInfo>().cached().cache_kind(flecs::QueryCacheAll).build();

		m_nonBlendIndirectCommandBuffers = m_nonBlendQuery.ResolveAs<DynamicGloballyIndexedResource>();
		if (m_clearBlend) {
			m_blendIndirectCommandBuffers = m_blendQuery.ResolveAs<DynamicGloballyIndexedResource>();
		}
	}

	PassReturn Execute(PassExecutionContext& executionContext) override {
	    auto* renderContext = executionContext.hostData->Get<RenderContext>();
	    auto& context = *renderContext;
		// Reset and get the appropriate command list
		auto& commandList = executionContext.commandList;

		auto counterReset = ::ResourceManager::GetInstance().GetUAVCounterReset();

		// Opaque buffer
		for (auto& res : m_nonBlendIndirectCommandBuffers) {
			auto counterOffset = res->GetResource()->GetUAVCounterOffset();
			auto apiResource = res->GetAPIResource();
			commandList.CopyBufferRegion(apiResource.GetHandle(), counterOffset, counterReset.GetHandle(), 0, sizeof(UINT));
		}

		// Blend buffer
		if (!m_clearBlend) return {};
		for (auto& res : m_blendIndirectCommandBuffers) {
			auto counterOffset = res->GetResource()->GetUAVCounterOffset();
			auto apiResource = res->GetAPIResource();
			commandList.CopyBufferRegion(apiResource.GetHandle(), counterOffset, counterReset.GetHandle(), 0, sizeof(UINT));
		}

		return {};
	}

	void Cleanup() override {
		lightQuery = {};
		m_nonBlendQuery = ECSResourceResolver();
		m_blendQuery = ECSResourceResolver();
		m_nonBlendIndirectCommandBuffers.clear();
		m_blendIndirectCommandBuffers.clear();
	}

private:
	bool m_clearBlend = false;
	flecs::query<Components::LightViewInfo> lightQuery;
	ComPtr<ID3D12PipelineState> m_PSO;

	ECSResourceResolver m_nonBlendQuery;
	ECSResourceResolver m_blendQuery;

	std::vector<std::shared_ptr<DynamicGloballyIndexedResource>> m_nonBlendIndirectCommandBuffers;
	std::vector<std::shared_ptr<DynamicGloballyIndexedResource>> m_blendIndirectCommandBuffers;
};
