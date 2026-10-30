#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "../shaders/PerPassRootConstants/lightCullingRootConstants.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

struct LightCullingBindings {
	org::PipelineState pipeline;
};
struct LightCullingFrame {
	uint32_t lightPagePoolSize = 0;
	uint32_t groups = 0;
};

class LightCullingPass : public org::TypedRenderGraphPass<LightCullingPass, LightCullingFrame, LightCullingBindings> {
public:
	LightCullingPass() { CreatePSO(); }

	~LightCullingPass() {
	}

	LightCullingBindings Declare(org::PassBuilder& builder) {
		builder.ShaderResource(Builtin::CameraBuffer, Builtin::Light::ActiveLightIndices, Builtin::Light::InfoBuffer);
		builder.UnorderedAccess(Builtin::Light::ClusterBuffer, Builtin::Light::PagesBuffer, Builtin::Light::PagesCounter);
		builder.ConstantBuffer(Builtin::PerFrameBuffer);
		builder.PreferQueue(org::QueueKind::Compute);
		return {m_PSO};
	}

	void Initialize() {

		m_lightPagesCounterHandle = m_resourceRegistryView->RequestHandle(Builtin::Light::PagesCounter);
		m_pLightPagesCounter = m_resourceRegistryView->Resolve<org::Buffer>(m_lightPagesCounterHandle);
	}

	LightCullingFrame Prepare(const LightCullingBindings&, const org::PassPrepareContext& preparation) const {
		const auto* update = preparation.preparationData->Get<UpdateContext>();
		const auto* render = preparation.preparationData->Get<RenderContext>();
		if (!update && !render) throw std::logic_error("LightCullingPass requires frame context");
		LightCullingFrame frame{};
		const auto state = update ? update->publishedRendererState : render->publishedRendererState;
		const auto lights = state
			? state->lights.payload.Get<br::render::PublishedLightTableState>() : nullptr;
		frame.lightPagePoolSize = lights
			? lights->lightPagePoolSize
			: (update ? update->LightPagePoolSize() : render->LightPagePoolSize());
		const auto clusterSize = update ? update->lightClusterSize : render->lightClusterSize;
		frame.groups = (clusterSize.x * clusterSize.y * clusterSize.z + 127u) / 128u;
		return frame;
	}
	static void Record(const LightCullingBindings& bindings, const LightCullingFrame& frame, org::PassRecordContext& context) {
		br::render::MiscConstants constants{};
		constants[LIGHT_PAGES_POOL_SIZE] = frame.lightPagePoolSize;
		br::render::DispatchCompute(context, bindings.pipeline, constants, frame.groups);
	}

	void Update(const org::UpdateExecutionContext& context) override {
		// Reset UAV counter
		uint32_t zero = 0;
		UploadBufferData(&zero, sizeof(uint32_t), org::runtime::UploadTarget::FromHandle(m_lightPagesCounterHandle), 0);
	}

private:

	org::Buffer* m_pLightPagesCounter = nullptr;
	org::ResourceRegistry::RegistryHandle m_lightPagesCounterHandle;

	void CreatePSO() {
		m_PSO = PSOManager::GetInstance().MakeComputePipeline(
			PSOManager::GetInstance().GetComputeRootSignature().GetHandle(),
			L"shaders/lightCulling.hlsl",
			L"CSMain",
			{},
			"Light Culling CS");
	}

	org::PipelineState m_PSO;
};
