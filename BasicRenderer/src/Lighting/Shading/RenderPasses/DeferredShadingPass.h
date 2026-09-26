#pragma once

#include <functional>
#include "Runtime/StateGraph/InvocationRevision.h"
#include <chrono>
#include <atomic>
#include <spdlog/spdlog.h>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"
#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeDispatch.h"

class DeferredShadingPass : public org::TypedRenderGraphPass<DeferredShadingPass, br::render::PreparedComputeDispatch> {
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
		builder.WithShaderResource(Builtin::CameraBuffer,
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
			Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
			Builtin::Noise::BlueNoise2D)
			.WithShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, org::Mip{ 0, 1 }))
			.WithUnorderedAccess(Builtin::Color::HDRColorTarget,
				Builtin::DebugVisualization,
				Builtin::Surface::Motion);

			if (m_shadowsEnabled) {
				builder.WithShaderResource(Builtin::Shadows::CLodClipmapInfo,
					Builtin::Shadows::CLodCompactMainCamera,
					Builtin::Shadows::CLodCompactShadowCameras,
					Builtin::Shadows::CLodDirectionalPageViewInfo,
					Builtin::Shadows::CLodPageMetadata,
					Builtin::Shadows::CLodPageTable,
					Builtin::Shadows::CLodPhysicalPages)
					.WithUnorderedAccess(Builtin::Shadows::CLodStats);
			}

		if (m_clusteredLightingEnabled) {
			builder.WithShaderResource(Builtin::Light::ClusterBuffer, Builtin::Light::PagesBuffer);
		}

		if (m_gtaoEnabled) {
			builder.WithShaderResource(Builtin::GTAO::OutputAOTerm);
		}

		builder.WithConstantBuffer(Builtin::PerFrameBuffer)
			.PreferQueue(org::QueueKind::Compute);
	}

	void Initialize() {
		RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::OpenPBR::OpaqueDielectricEnergyComplement);
		if (m_shadowsEnabled) {
			RegisterSRV(org::SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
		}
	}

	br::render::PreparedComputeDispatch Prepare(const org::PassPrepareContext& preparation) {
		const auto* update = preparation.preparationData->Get<UpdateContext>();
		const auto* render = preparation.preparationData->Get<RenderContext>();
		if (!update && !render) throw std::logic_error("DeferredShadingPass requires frame context");
		const auto globalFlags = update ? update->globalPSOFlags : render->globalPSOFlags;
		const auto resolution = update ? update->renderResolution : render->renderResolution;
		auto& pso = DeferredPipeline(globalFlags);
		br::render::PreparedComputeDispatch data{};
		data.layout = PSOManager::GetInstance().GetComputeRootSignature().GetHandle();
		auto program = CaptureProgramBinding(preparation, pso);
		data.program = program.program;
		data.descriptorIndices = std::move(program.descriptorIndices);
		const auto& lighting = update ? update->lighting : render->lighting;
		data.constants[MiscEnableShadows] = lighting.shadowsEnabled;
		data.constants[MiscEnableShadows + 1] = lighting.punctualLightingEnabled;
		data.constants[MiscEnableShadows + 2] = lighting.gtaoEnabled;
		data.constants[MiscEnableShadows + 3] = m_skyboxEnabled;
		data.groupsX = (resolution.x + 7u) / 8u; data.groupsY = (resolution.y + 7u) / 8u;
		return data;
	}
	void InvocationRevision(const org::PassPrepareContext& preparation, std::vector<uint64_t>& out) const {
		const auto* update = preparation.preparationData->Get<UpdateContext>();
		const auto* render = preparation.preparationData->Get<RenderContext>();
		if (!update && !render) return;
		const auto globalFlags = update ? update->globalPSOFlags : render->globalPSOFlags;
		const auto resolution = update ? update->renderResolution : render->renderResolution;
		const auto& lighting = update ? update->lighting : render->lighting;
		const auto pipeline = br::render::PipelineRevision(DeferredPipeline(globalFlags));
		const auto layout = br::render::HandleRevision(PSOManager::GetInstance().GetComputeRootSignature().GetHandle());
		out.insert(out.end(), {static_cast<uint64_t>(globalFlags), static_cast<uint64_t>(resolution.x), static_cast<uint64_t>(resolution.y),
			static_cast<uint64_t>(lighting.shadowsEnabled), static_cast<uint64_t>(lighting.punctualLightingEnabled),
			static_cast<uint64_t>(lighting.gtaoEnabled), static_cast<uint64_t>(m_skyboxEnabled), pipeline, layout});
	}
	static void Record(const br::render::PreparedComputeDispatch& data, org::PassRecordContext& recording) {
		br::render::RecordPreparedComputeDispatch(data, recording);
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
