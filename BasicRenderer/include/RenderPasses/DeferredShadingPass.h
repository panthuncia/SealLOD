#pragma once

#include <functional>

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Scene/Scene.h"
#include "Managers/Singletons/SettingsManager.h"

class DeferredShadingPass : public ComputePass {
public:
	explicit DeferredShadingPass(bool skyboxEnabled = false)
		: m_skyboxEnabled(skyboxEnabled) {
		auto& settingsManager = SettingsManager::GetInstance();
		getImageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting");
		getPunctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting");
		getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
		m_gtaoEnabled = settingsManager.getSettingGetter<bool>("enableGTAO")();
		m_clusteredLightingEnabled = settingsManager.getSettingGetter<bool>("enableClusteredLighting")();
	}

	void DeclareResourceUsages(ComputePassBuilder* builder) override {
		builder->WithShaderResource(Builtin::CameraBuffer,
			Builtin::Environment::PrefilteredCubemapsGroup,
			Builtin::Light::ActiveLightIndices,
			Builtin::Light::InfoBuffer,
			Builtin::Light::PointLightCubemapBuffer,
			Builtin::Light::DirectionalLightCascadeBuffer,
			Builtin::Light::SpotLightMatrixBuffer,
			Builtin::Environment::InfoBuffer,
			Builtin::PerMaterialOpenPBRDataBuffer,
			Builtin::Material::TextureGroup,
			Builtin::GBuffer::Normals,
			Builtin::GBuffer::Albedo,
			Builtin::GBuffer::Coat,
			Builtin::GBuffer::Emissive,
			Builtin::GBuffer::Fuzz,
			Builtin::GBuffer::MetallicRoughness,
			Builtin::Environment::CurrentCubemap,
			Builtin::OpenPBR::FuzzLTC,
			Builtin::OpenPBR::IdealMetalEnergyComplement,
			Builtin::OpenPBR::IdealMetalAverageEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricEnergyComplement,
			Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement,
			Builtin::Noise::BlueNoise2D)
			.WithShaderResource(Subresources(Builtin::PrimaryCamera::LinearDepthMap, Mip{ 0, 1 }))
			.WithUnorderedAccess(Builtin::Color::HDRColorTarget,
				Builtin::DebugVisualization,
				Builtin::GBuffer::MotionVectors);

			if (getShadowsEnabled()) {
				builder->WithShaderResource(Builtin::Shadows::CLodClipmapInfo,
					Builtin::Shadows::CLodCompactMainCamera,
					Builtin::Shadows::CLodCompactShadowCameras,
					Builtin::Shadows::CLodDirectionalPageViewInfo,
					Builtin::Shadows::CLodPageMetadata,
					Builtin::Shadows::CLodPageTable,
					Builtin::Shadows::CLodPhysicalPages)
					.WithUnorderedAccess(Builtin::Shadows::CLodStats);
			}

		if (m_clusteredLightingEnabled) {
			builder->WithShaderResource(Builtin::Light::ClusterBuffer, Builtin::Light::PagesBuffer);
		}

		if (m_gtaoEnabled) {
			builder->WithShaderResource(Builtin::GTAO::OutputAOTerm);
		}

		builder->WithConstantBuffer(Builtin::PerFrameBuffer);
	}

	void Setup() override {
		RegisterSRV(SRVViewType::Texture2DArrayFull, Builtin::OpenPBR::OpaqueDielectricEnergyComplement);
		if (getShadowsEnabled()) {
			RegisterSRV(SRVViewType::Texture2DArrayFull, Builtin::Shadows::CLodPageTable);
		}
	}

	PassReturn Execute(PassExecutionContext& executionContext) override {
	    auto* renderContext = executionContext.hostData->Get<RenderContext>();
	    auto& context = *renderContext;
		auto& psoManager = PSOManager::GetInstance();
		auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(),
			context.samplerDescriptorHeap.GetHandle());

		commandList.BindLayout(psoManager.GetComputeRootSignature().GetHandle());

		auto& pso = psoManager.GetDeferredPSO(context.globalPSOFlags);
		commandList.BindPipeline(pso.GetAPIPipelineState().GetHandle());

		BindResourceDescriptorIndices(commandList, pso.GetResourceDescriptorSlots());

		unsigned int settings[] = {
			getShadowsEnabled(),
			getPunctualLightingEnabled(),
			m_gtaoEnabled,
			m_skyboxEnabled
		};
		commandList.PushConstants(rhi::ShaderStage::Compute, 0,
			MiscUintRootSignatureIndex, MiscEnableShadows,
			4, settings);

		uint32_t w = context.renderResolution.x;
		uint32_t h = context.renderResolution.y;
		const uint32_t groupSizeX = 8;
		const uint32_t groupSizeY = 8;
		uint32_t groupsX = (w + groupSizeX - 1) / groupSizeX;
		uint32_t groupsY = (h + groupSizeY - 1) / groupSizeY;

		commandList.Dispatch(groupsX, groupsY, 1);
		return {};
	}

	void Cleanup() override {
		// Cleanup the render pass
	}

private:

	std::function<bool()> getImageBasedLightingEnabled;
	std::function<bool()> getPunctualLightingEnabled;
	std::function<bool()> getShadowsEnabled;

	bool m_gtaoEnabled = true;
	bool m_clusteredLightingEnabled = true;
	bool m_skyboxEnabled = false;
};
