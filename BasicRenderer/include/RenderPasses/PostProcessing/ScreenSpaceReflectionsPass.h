#pragma once

#include "RenderPasses/Base/RenderPass.h"
#include "Render/RenderContext.h"
#include "Scene/Scene.h"
#include "Managers/Singletons/FFXManager.h"

class ScreenSpaceReflectionsPass : public ComputePass {
public:
    ScreenSpaceReflectionsPass() {
    }

    void DeclareResourceUsages(ComputePassBuilder* builder) override {
        builder->WithLegacyInterop(Builtin::Color::HDRColorTarget,
            Builtin::GBuffer::MotionVectors,
            Builtin::PrimaryCamera::DepthTexture,
            Builtin::GBuffer::Normals,
            Builtin::GBuffer::MetallicRoughness,
            Builtin::GBuffer::MotionVectors,
            Builtin::Environment::CurrentPrefilteredCubemap,
            Builtin::BRDFLUT,
            Builtin::PostProcessing::ScreenSpaceReflections);

        ResourceState outState{
        .access = rhi::ResourceAccessType::Common, 
        .layout = rhi::ResourceLayout::Common, 
        .sync = rhi::ResourceSyncState::All};

		ResourceIdentifierAndRange outResource(Builtin::PostProcessing::ScreenSpaceReflections, {});

        builder->WithInternalTransition(
            outResource,
            outState);
    }

    void Setup() override {
        m_pHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Color::HDRColorTarget);
        m_pMotionVectors = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GBuffer::MotionVectors);
        m_pDepthTexture = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PrimaryCamera::DepthTexture);
		m_pNormals = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GBuffer::Normals);
        m_pMetallicRoughness = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GBuffer::MetallicRoughness);
		m_pMotionVectors = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GBuffer::MotionVectors);
		m_pEnvironmentCubemap = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Environment::CurrentPrefilteredCubemap);
        m_pBRDFLUT = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::BRDFLUT);
		m_pSSSROutput = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PostProcessing::ScreenSpaceReflections);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;

        executionContext.commandList.SetDescriptorHeaps(context.textureDescriptorHeap.GetHandle(), context.samplerDescriptorHeap.GetHandle());

        // TODO: Do we need a clear here? Need transitions if so.
        //rhi::UavClearInfo clearInfo{};
        //clearInfo.cpuVisible = m_pSSSROutput->GetUAVNonShaderVisibleInfo(0).slot;
        //clearInfo.shaderVisible = m_pSSSROutput->GetUAVShaderVisibleInfo(0).slot;
        //clearInfo.resource = m_pSSSROutput->GetAPIResource();

        //rhi::UavClearFloat clearValue{};
        //clearValue.v[0] = 0.0f;
        //clearValue.v[1] = 0.0f;
        //clearValue.v[2] = 0.0f;
        //clearValue.v[3] = 0.0f;

        //executionContext.commandList.ClearUavFloat(clearInfo, clearValue);


        FFXManager::GetInstance().EvaluateSSSR(
            executionContext.commandList,
			&context.primaryCamera,
            m_pHDRTarget,
            m_pDepthTexture,
			m_pNormals,
			m_pMetallicRoughness,
			m_pMotionVectors,
            m_pEnvironmentCubemap,
			m_pBRDFLUT,
            m_pSSSROutput
		);
        return {};
    }

    void Cleanup() override {
        // Cleanup the render pass
    }

private:

    PixelBuffer* m_pHDRTarget;
    PixelBuffer* m_pMotionVectors;
    PixelBuffer* m_pDepthTexture;
	PixelBuffer* m_pNormals;
	PixelBuffer* m_pMetallicRoughness;
	PixelBuffer* m_pEnvironmentCubemap;
	PixelBuffer* m_pBRDFLUT;
	PixelBuffer* m_pSSSROutput;
};
