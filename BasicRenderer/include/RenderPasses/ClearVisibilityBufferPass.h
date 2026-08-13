#pragma once

#include <functional>

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/RenderContext.h"
#include "Render/OutputTypes.h"
#include "Resources/PixelBuffer.h"

class ClearVisibilityBufferPass : public RenderPass {
public:
	ClearVisibilityBufferPass() {
		m_getOutputType = SettingsManager::GetInstance().getSettingGetter<unsigned int>("outputType");
	}

	void DeclareResourceUsages(RenderPassBuilder* builder) override {
		builder->WithUnorderedAccessClear(Builtin::PrimaryCamera::VisibilityTexture,
			Builtin::Surface::BaseColorOpacity,
			Builtin::Surface::NormalRoughness,
			Builtin::Surface::SpecularAo,
			Builtin::Surface::Emissive,
			Builtin::Surface::Motion,
			Builtin::Surface::Payload0,
			Builtin::Surface::Payload1,
			Builtin::Surface::Identity,
			Builtin::DebugVisualization);
		builder->WithDepthStencilClear(Builtin::PrimaryCamera::DepthTexture);
	}

	void Setup() override {
		m_visibilityBuffer = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PrimaryCamera::VisibilityTexture);
		m_baseColorOpacity = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::BaseColorOpacity);
		m_normalRoughness = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::NormalRoughness);
		m_specularAo = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::SpecularAo);
		m_emissive = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::Emissive);
		m_motion = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::Motion);
		m_payload0 = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::Payload0);
		m_payload1 = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::Payload1);
		m_surfaceIdentity = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Surface::Identity);
		m_depthTexture = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PrimaryCamera::DepthTexture);
		m_debugVisualization = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::DebugVisualization);
	}

	PassReturn Execute(PassExecutionContext& executionContext) override {
		auto* renderContext = executionContext.hostData->Get<RenderContext>();
		auto& context = *renderContext;
		auto& psoManager = PSOManager::GetInstance();
		auto& commandList = executionContext.commandList;

		commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(),
			context.samplerDescriptorHeap.GetHandle());

		rhi::UavClearInfo clearInfo{};
		clearInfo.cpuVisible = m_visibilityBuffer->GetUAVNonShaderVisibleInfo(0).slot;
		clearInfo.shaderVisible = m_visibilityBuffer->GetUAVShaderVisibleInfo(0).slot;
		clearInfo.resource = m_visibilityBuffer->GetAPIResource();

		// Visibility buffer clear value: 0xFFFFFFFF, 0xFFFFFFFF
		rhi::UavClearUint clearValue{};
		clearValue.v[0] = 0xFFFFFFFF;
		clearValue.v[1] = 0xFFFFFFFF;

		commandList.ClearUavUint(clearInfo, clearValue);

		// Canonical surface producers overwrite this sentinel for every shaded
		// pixel. Clearing it prevents missing producers from exposing stale or
		// uninitialized record indices to deferred shading.
		clearInfo.cpuVisible = m_surfaceIdentity->GetUAVNonShaderVisibleInfo(0).slot;
		clearInfo.shaderVisible = m_surfaceIdentity->GetUAVShaderVisibleInfo(0).slot;
		clearInfo.resource = m_surfaceIdentity->GetAPIResource();
		commandList.ClearUavUint(clearInfo, clearValue);

		// Everything else: 0
		rhi::UavClearFloat clearValueFloat{};
		clearValueFloat.v[0] = 0;
		clearValueFloat.v[1] = 0;

		auto clearResource = [&](GloballyIndexedResource* resource) {
			if (resource) {
				rhi::UavClearInfo info{};
				info.cpuVisible = resource->GetUAVNonShaderVisibleInfo(0).slot;
				info.shaderVisible = resource->GetUAVShaderVisibleInfo(0).slot;
				info.resource = resource->GetAPIResource();
				commandList.ClearUavFloat(info, clearValueFloat);
			}
			};

		auto clearDepth = [&](GloballyIndexedResource* resource) {
			if (resource) {
				commandList.ClearDepthStencilView(
					resource->GetDSVInfo(0).slot,
					true,
					false,
					1.0f,
					0
				);
			}
			};

		clearResource(m_baseColorOpacity);
		clearResource(m_normalRoughness);
		clearResource(m_specularAo);
		clearResource(m_emissive);
		clearResource(m_motion);
		clearResource(m_payload0);
		clearResource(m_payload1);
		clearDepth(m_depthTexture); // same

		// Clear debug visualization texture to sentinel (0xFFFFFFFF)
		{
			rhi::UavClearInfo debugClearInfo{};
			debugClearInfo.cpuVisible = m_debugVisualization->GetUAVNonShaderVisibleInfo(0).slot;
			debugClearInfo.shaderVisible = m_debugVisualization->GetUAVShaderVisibleInfo(0).slot;
			debugClearInfo.resource = m_debugVisualization->GetAPIResource();
			rhi::UavClearUint debugClearValue{};
			debugClearValue.v[0] = 0xFFFFFFFF;
			debugClearValue.v[1] = 0xFFFFFFFF;
			commandList.ClearUavUint(debugClearInfo, debugClearValue);
		}
		return {};
	}

	void Cleanup() override {
		// Cleanup the render pass
	}

private:
	GloballyIndexedResource* m_visibilityBuffer;
	GloballyIndexedResource* m_baseColorOpacity;
	GloballyIndexedResource* m_normalRoughness;
	GloballyIndexedResource* m_specularAo;
	GloballyIndexedResource* m_emissive;
	GloballyIndexedResource* m_motion;
	GloballyIndexedResource* m_payload0;
	GloballyIndexedResource* m_payload1;
	GloballyIndexedResource* m_surfaceIdentity;
	GloballyIndexedResource* m_depthTexture;
	GloballyIndexedResource* m_debugVisualization;
	std::function<unsigned int()> m_getOutputType;
};
