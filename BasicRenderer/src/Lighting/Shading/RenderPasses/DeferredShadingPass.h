#pragma once

#include <functional>
#include <chrono>
#include <atomic>
#include <spdlog/spdlog.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/RecordHelpers.h"

// The deferred pipeline follows the global PSO flags, so it is chosen per frame.
struct DeferredShadingFrame {
	org::PipelineState pipeline;
	std::array<uint32_t, 4> lighting{};
	uint32_t groupsX = 0, groupsY = 0;
};

class DeferredShadingPass : public org::TypedRenderGraphPass<DeferredShadingPass, DeferredShadingFrame> {
public:
	explicit DeferredShadingPass(bool skyboxEnabled = false)
		: m_skyboxEnabled(skyboxEnabled) {
		auto& settingsManager = SettingsManager::GetInstance();
		m_imageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting")();
		m_punctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting")();
		m_shadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows")();
		m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();
		m_clusteredLightingEnabled = settingsManager.getSettingGetter<bool>("enableClusteredLighting")();
	}

	void Declare(org::PassBuilder& builder) {
		builder.ShaderResource(Builtin::CameraBuffer,
			Builtin::Environment::PrefilteredCubemapsGroup,
			Builtin::Light::ActiveLightIndices,
			Builtin::Light::InfoBuffer,
			Builtin::Light::PointLightCubemapBuffer,
			Builtin::Light::DirectionalLightCascadeBuffer,
			Builtin::Light::SpotLightMatrixBuffer,
			Builtin::Environment::InfoBuffer,
			Builtin::PerMaterialOpenPBRDataBuffer,
			Builtin::Surface::BaseColorOpacity,
			Builtin::Surface::NormalRoughness,
			Builtin::Surface::SpecularAo,
			Builtin::Surface::Emissive,
			Builtin::Surface::Identity,
			Builtin::Surface::Payload0,
			Builtin::Surface::Payload1,
			Builtin::Surface::Records,
			Builtin::Environment::CurrentCubemap,
			Builtin::OpenPBR::FuzzLTC,
			Builtin::OpenPBR::IdealMetalEnergyComplement,
			Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
			Builtin::Noise::BlueNoise2D);
		builder.ShaderResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
			org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
		builder.ShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }));
		builder.UnorderedAccess(Builtin::Color::HDRColorTarget,
				Builtin::DebugVisualization,
				Builtin::Surface::Motion);

			if (m_shadowsEnabled) {
				builder.ShaderResource(Builtin::Shadows::CLodClipmapInfo,
					Builtin::Shadows::CLodCompactMainCamera,
					Builtin::Shadows::CLodCompactShadowCameras,
					Builtin::Shadows::CLodDirectionalPageViewInfo,
					Builtin::Shadows::CLodPageMetadata,
					Builtin::Shadows::CLodPhysicalPages);
				builder.ShaderResource(Builtin::Shadows::CLodPageTable,
					org::SrvView{static_cast<uint32_t>(org::SRVViewType::Texture2DArrayFull)});
				builder.UnorderedAccess(Builtin::Shadows::CLodStats);
			}

		if (m_clusteredLightingEnabled) {
			builder.ShaderResource(Builtin::Light::ClusterBuffer, Builtin::Light::PagesBuffer);
		}

		if (m_gtaoEnabled) {
			builder.ShaderResource(Builtin::GTAO::OutputAOTerm);
		}

		builder.ConstantBuffer(Builtin::PerFrameBuffer);
		builder.PreferQueue(org::QueueKind::Compute);
	}

	void Initialize() {}

	DeferredShadingFrame Prepare(const org::PassPrepareContext& preparation) {
		const auto* update = preparation.preparationData->Get<UpdateContext>();
		const auto* render = preparation.preparationData->Get<RenderContext>();
		if (!update && !render) throw std::logic_error("DeferredShadingPass requires frame context");
		const auto globalFlags = update ? update->globalPSOFlags : render->globalPSOFlags;
		const auto resolution = update ? update->renderResolution : render->renderResolution;
		const auto& lighting = update ? update->lighting : render->lighting;
		DeferredShadingFrame frame{};
		frame.pipeline = DeferredPipeline(globalFlags);
		frame.lighting = {lighting.shadowsEnabled, lighting.punctualLightingEnabled, lighting.gtaoEnabled, m_skyboxEnabled};
		frame.groupsX = (resolution.x + 7u) / 8u; frame.groupsY = (resolution.y + 7u) / 8u;
		return frame;
	}
	static void Record(const DeferredShadingFrame& frame, org::PassRecordContext& context) {
		br::render::MiscConstants constants{};
		for (uint32_t index = 0; index < frame.lighting.size(); ++index)
			constants[MiscEnableShadows + index] = frame.lighting[index];
		br::render::DispatchCompute(context, frame.pipeline, constants, frame.groupsX, frame.groupsY);
	}

private:
	const org::PipelineState& DeferredPipeline(UINT globalFlags) const {
		auto& manager = PSOManager::GetInstance();
		const auto generation = manager.PipelineCacheGeneration();
		if (m_cachedPipeline && m_cachedPipelineFlags == globalFlags && m_cachedPipelineGeneration == generation)
			return *m_cachedPipeline;
		m_cachedPipeline = &manager.GetDeferredPSO(globalFlags);
		m_cachedPipelineFlags = globalFlags;
		m_cachedPipelineGeneration = generation;
		return *m_cachedPipeline;
	}
	mutable const org::PipelineState* m_cachedPipeline = nullptr;
	mutable UINT m_cachedPipelineFlags = 0;
	mutable uint64_t m_cachedPipelineGeneration = 0;

	bool m_imageBasedLightingEnabled = true;
	bool m_punctualLightingEnabled = true;
	bool m_shadowsEnabled = true;

	bool m_gtaoEnabled = true;
	bool m_clusteredLightingEnabled = true;
	bool m_skyboxEnabled = false;
};
