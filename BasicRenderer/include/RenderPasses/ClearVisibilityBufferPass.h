#pragma once

#include "RenderPasses/Base/ComputePass.h"
#include "Managers/Singletons/PSOManager.h"
#include "Render/RenderContext.h"
#include "Resources/PixelBuffer.h"

class ClearVisibilityBufferPass : public RenderPass {
public:
	ClearVisibilityBufferPass() {}

	void DeclareResourceUsages(RenderPassBuilder* builder) override {
		builder->WithUnorderedAccessClear(Builtin::PrimaryCamera::VisibilityTexture,
			Builtin::GBuffer::Albedo,
			Builtin::GBuffer::Coat,
			Builtin::GBuffer::Emissive,
			Builtin::GBuffer::Fuzz,
			Builtin::GBuffer::MetallicRoughness,
			Builtin::GBuffer::Normals,
			Builtin::GBuffer::MotionVectors,
			Builtin::Color::HDRColorTarget,
			Builtin::DebugVisualization);
		builder->WithDepthStencilClear(Builtin::PrimaryCamera::DepthTexture);
		builder->WithRenderTargetClear(Subresources(Builtin::PrimaryCamera::LinearDepthMap, Mip{ 0, 1 }))
			.WithUnorderedAccessClear(Subresources(Builtin::PrimaryCamera::LinearDepthMap, FromMip{ 1 }));
	}

	void Setup() override {
		m_visibilityBuffer = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PrimaryCamera::VisibilityTexture);
		m_albedo = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GBuffer::Albedo);
		m_coat = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GBuffer::Coat);
		m_metallicRoughness = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GBuffer::MetallicRoughness);
		m_emissive = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GBuffer::Emissive);
		m_fuzz = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GBuffer::Fuzz);
		m_normals = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GBuffer::Normals);
		m_motionVectors = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::GBuffer::MotionVectors);
		m_HDRColorTarget = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::Color::HDRColorTarget);
		m_depthTexture = m_resourceRegistryView->RequestPtr<GloballyIndexedResource>(Builtin::PrimaryCamera::DepthTexture);
		m_linearDepthTexture = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PrimaryCamera::LinearDepthMap);
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

		clearResource(m_albedo);
		clearResource(m_coat);
		clearResource(m_metallicRoughness);
		clearResource(m_emissive);
		clearResource(m_fuzz);
		clearResource(m_normals);
		clearResource(m_motionVectors);
		clearResource(m_HDRColorTarget); // TODO: Only needed because of non-zero initialized memory issue- make a clear manager instead?
		clearDepth(m_depthTexture); // same
		if (m_linearDepthTexture) {
			for (unsigned int slice = 0; slice < m_linearDepthTexture->GetNumRTVSlices(); ++slice) {
				commandList.ClearRenderTargetView(
					m_linearDepthTexture->GetRTVInfo(0, slice).slot,
					m_linearDepthTexture->GetClearColor());
				for (unsigned int mip = 1; mip < m_linearDepthTexture->GetNumUAVMipLevels(); ++mip) {
					rhi::UavClearInfo clearInfo{};
					clearInfo.cpuVisible = m_linearDepthTexture->GetUAVNonShaderVisibleInfo(mip, slice).slot;
					clearInfo.shaderVisible = m_linearDepthTexture->GetUAVShaderVisibleInfo(mip, slice).slot;
					clearInfo.resource = m_linearDepthTexture->GetAPIResource();
					commandList.ClearUavFloat(clearInfo, rhi::UavClearFloat{});
				}
			}
		}

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
	GloballyIndexedResource* m_albedo;
	GloballyIndexedResource* m_coat;
	GloballyIndexedResource* m_metallicRoughness;
	GloballyIndexedResource* m_emissive;
	GloballyIndexedResource* m_fuzz;
	GloballyIndexedResource* m_normals;
	GloballyIndexedResource* m_motionVectors;
	GloballyIndexedResource* m_HDRColorTarget;
	GloballyIndexedResource* m_depthTexture;
	PixelBuffer* m_linearDepthTexture;
	GloballyIndexedResource* m_debugVisualization;
};
