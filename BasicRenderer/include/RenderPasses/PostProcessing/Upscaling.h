#pragma once

#include <functional>

#include "RenderPasses/Base/RenderPass.h"
#include "Scene/Scene.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/UpscalingManager.h"

class UpscalingPass : public RenderPass {
public:
    UpscalingPass() {
        m_renderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
		m_outputRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    }

    void DeclareResourceUsages(RenderPassBuilder* builder) override {
        ResourceIdentifierAndRange upscaledHDR(Builtin::PostProcessing::UpscaledHDR, {});
        const UpscalingMode upscalingMode = UpscalingManager::GetInstance().GetCurrentUpscalingMode();
        const rhi::Backend backend = DeviceManager::GetInstance().GetBackend();

        if (upscalingMode == UpscalingMode::FSR3) {
            builder->WithShaderResource(
                Builtin::Color::HDRColorTarget,
                Builtin::GBuffer::MotionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture)
                .WithUnorderedAccess(upscaledHDR);
            return;
        }

        if (upscalingMode == UpscalingMode::None && backend == rhi::Backend::Vulkan) {
            builder->WithCopySource(Builtin::Color::HDRColorTarget)
                .WithCopyDest(upscaledHDR)
                .WithShaderResource(
                    Builtin::GBuffer::MotionVectors,
                    Builtin::PrimaryCamera::ProjectedDepthTexture);
            return;
        }

        ResourceState vulkanStreamlineExitState{
            .access = rhi::ResourceAccessType::UnorderedAccess | rhi::ResourceAccessType::UnorderedAccessClear,
            .layout = rhi::ResourceLayout::UnorderedAccess,
            .sync = rhi::ResourceSyncState::AllShading | rhi::ResourceSyncState::ClearUnorderedAccessView };
        ResourceState dx12StreamlineExitState{
            .access = rhi::ResourceAccessType::Common,
            .layout = rhi::ResourceLayout::Common,
            .sync = rhi::ResourceSyncState::All };

        // TODO: Remove these backend-specific workarounds when ORG can model combined non-conflicting usages on one resource.
        if (backend == rhi::Backend::Vulkan) {
            builder->WithShaderResource(
                Builtin::Color::HDRColorTarget,
                Builtin::GBuffer::MotionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture)
                .WithUnorderedAccessClear(upscaledHDR)
                .WithInternalTransition(upscaledHDR, vulkanStreamlineExitState);
        }
        else {
            builder->WithLegacyInterop(
                Builtin::Color::HDRColorTarget,
                Builtin::GBuffer::MotionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture,
                upscaledHDR)
                .WithInternalTransition(upscaledHDR, dx12StreamlineExitState);
        }
    }

    void Setup() override {
        m_pHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::Color::HDRColorTarget);
        m_pMotionVectors = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::GBuffer::MotionVectors);
		m_pDepthTexture = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PrimaryCamera::ProjectedDepthTexture);
		m_pUpscaledHDRTarget = m_resourceRegistryView->RequestPtr<PixelBuffer>(Builtin::PostProcessing::UpscaledHDR);
    }

    PassReturn Execute(PassExecutionContext& executionContext) override {
        auto* renderContext = executionContext.hostData->Get<RenderContext>();
        auto& context = *renderContext;
        UpscalingManager::GetInstance().Evaluate(executionContext.commandList, &context.primaryCamera, context.frameNumber, context.deltaTime, m_pHDRTarget, m_pUpscaledHDRTarget, m_pDepthTexture, m_pMotionVectors);
        return {};
    }

    void Cleanup() override {
        // Cleanup the render pass
    }

private:

    PixelBuffer* m_pHDRTarget;
    PixelBuffer* m_pMotionVectors;
	PixelBuffer* m_pDepthTexture;
	PixelBuffer* m_pUpscaledHDRTarget;

    DirectX::XMUINT2 m_renderRes;
    DirectX::XMUINT2 m_outputRes;

};