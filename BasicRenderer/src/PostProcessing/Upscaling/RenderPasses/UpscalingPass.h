#pragma once

#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Scene/Scene.h"
#include "PostProcessing/Upscaling/UpscalingGenerationService.h"
#include "Render/PreparedPass.h"

struct UpscalingFrameData {
    std::shared_ptr<const br::render::UpscalingGenerationService> service;
    Components::Camera camera;
    uint64_t frameNumber = 0;
    double deltaTime = 0;
    std::shared_ptr<org::PixelBuffer> hdr;
    std::shared_ptr<org::PixelBuffer> output;
    std::shared_ptr<org::PixelBuffer> depth;
    std::shared_ptr<org::PixelBuffer> motion;
};

class UpscalingPass
    : public org::TypedRenderGraphPass<UpscalingPass, UpscalingFrameData> {
public:
    explicit UpscalingPass(
        std::shared_ptr<const br::render::UpscalingGenerationService> service)
        : m_service(std::move(service)) {}

    void Declare(org::PassBuilder& declaration) {
        auto* builder = &declaration;
        // Upscalers produce only the full-resolution image in mip 0. Keep the
        // interop contract explicit so it remains correct if this output is
        // replaced by an external resource with additional subresources.
        const auto upscaledHDR = Subresources(
            Builtin::PostProcessing::UpscaledHDR,
            org::Mip{ 0, 1 });
        const UpscalingMode upscalingMode = m_service->Mode();
        const rhi::Backend backend = m_service->Backend();
        const bool useDilatedMotionVectors = m_service->UsesDilatedMotionVectors();
        const auto motionVectors = useDilatedMotionVectors
            ? Builtin::Surface::DilatedMotion
            : Builtin::Surface::Motion;

        if (upscalingMode == UpscalingMode::FSR3) {
            builder->WithShaderResource(
                Builtin::Color::HDRColorTarget,
                motionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture)
                .WithUnorderedAccess(upscaledHDR);
            return;
        }

        if (upscalingMode == UpscalingMode::None && backend == rhi::Backend::Vulkan) {
            builder->WithCopySource(Builtin::Color::HDRColorTarget)
                .WithCopyDest(upscaledHDR)
                .WithShaderResource(
                    Builtin::Surface::Motion,
                    Builtin::PrimaryCamera::ProjectedDepthTexture);
            return;
        }

        org::ResourceState vulkanStreamlineExitState{
            .access = rhi::ResourceAccessType::UnorderedAccess | rhi::ResourceAccessType::UnorderedAccessClear,
            .layout = rhi::ResourceLayout::UnorderedAccess,
            .sync = rhi::ResourceSyncState::AllShading | rhi::ResourceSyncState::ClearUnorderedAccessView };
        org::ResourceState dx12StreamlineExitState{
            .access = rhi::ResourceAccessType::Common,
            .layout = rhi::ResourceLayout::Common,
            .sync = rhi::ResourceSyncState::All };

        // TODO: Remove these backend-specific workarounds when ORG can model combined non-conflicting usages on one resource.
        if (backend == rhi::Backend::Vulkan) {
            builder->WithShaderResource(
                Builtin::Color::HDRColorTarget,
                motionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture)
                .WithUnorderedAccessClear(upscaledHDR)
                .WithInternalTransition(upscaledHDR, vulkanStreamlineExitState);
        }
        else {
            builder->WithLegacyInterop(
                Builtin::Color::HDRColorTarget,
                motionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture,
                upscaledHDR)
                .WithInternalTransition(upscaledHDR, dx12StreamlineExitState);
        }
    }

    void Initialize() {
        m_pHDRTarget = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::Color::HDRColorTarget);
        const bool useDilatedMotionVectors = m_service->UsesDilatedMotionVectors();
        const auto motionVectors = useDilatedMotionVectors
            ? Builtin::Surface::DilatedMotion
            : Builtin::Surface::Motion;
        m_pMotionVectors = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(motionVectors);
		m_pDepthTexture = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::PrimaryCamera::ProjectedDepthTexture);
		m_pUpscaledHDRTarget = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::PostProcessing::UpscaledHDR);
    }

    UpscalingFrameData Prepare(const org::PassPrepareContext& preparation) {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        return UpscalingFrameData{
            .service = m_service,
            .camera = context->primaryCamera,
            .frameNumber = preparation.frameNumber,
            .deltaTime = preparation.deltaTime,
            .hdr = m_pHDRTarget,
            .output = m_pUpscaledHDRTarget,
            .depth = m_pDepthTexture,
            .motion = m_pMotionVectors,
        };
    }
    static void Record(const UpscalingFrameData& data, org::PassRecordContext& recording) {
        data.service->Evaluate(recording.Commands(), data.camera,
            data.frameNumber, data.deltaTime, data.hdr.get(), data.output.get(),
            data.depth.get(), data.motion.get());
    }

private:

    std::shared_ptr<org::PixelBuffer> m_pHDRTarget;
    std::shared_ptr<org::PixelBuffer> m_pMotionVectors;
	std::shared_ptr<org::PixelBuffer> m_pDepthTexture;
	std::shared_ptr<org::PixelBuffer> m_pUpscaledHDRTarget;

    std::shared_ptr<const br::render::UpscalingGenerationService> m_service;

};
