#pragma once

#include "RenderPasses/Base/RenderPass.h"
#include "Render/RenderContext.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/ResourceManager.h"
#include "Scene/Scene.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Scene/Components.h"
#include "boost/container_hash/hash.hpp"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "Materials/TechniqueDescriptor.h"

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

		br::render::PublishedResourceQuery nonBlend{};
		nonBlend.owner = br::render::PublishedFragmentKind::IndirectWorkloads;
		nonBlend.usage = br::render::PublishedResourceUsage::IndirectArguments;
		nonBlend.forbiddenVariantMask = static_cast<std::uint64_t>(MaterialCompileBlend);
		m_nonBlendQuery = PublishedStateResourceResolver(br::render::PublishedStateSource::ProcessSource(), nonBlend);
		builder->WithCopyDest(m_nonBlendQuery);
		if (m_clearBlend) {
			br::render::PublishedResourceQuery blend = nonBlend;
			blend.forbiddenVariantMask = 0;
			blend.requiredVariantMask = static_cast<std::uint64_t>(MaterialCompileBlend);
			m_blendQuery = PublishedStateResourceResolver(br::render::PublishedStateSource::ProcessSource(), blend);
			builder->WithCopyDest(m_blendQuery);
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
		m_nonBlendQuery = PublishedStateResourceResolver();
		m_blendQuery = PublishedStateResourceResolver();
		m_nonBlendIndirectCommandBuffers.clear();
		m_blendIndirectCommandBuffers.clear();
	}

private:
	bool m_clearBlend = false;
	flecs::query<Components::LightViewInfo> lightQuery;
	ComPtr<ID3D12PipelineState> m_PSO;

	PublishedStateResourceResolver m_nonBlendQuery;
	PublishedStateResourceResolver m_blendQuery;

	std::vector<std::shared_ptr<DynamicGloballyIndexedResource>> m_nonBlendIndirectCommandBuffers;
	std::vector<std::shared_ptr<DynamicGloballyIndexedResource>> m_blendIndirectCommandBuffers;
};
