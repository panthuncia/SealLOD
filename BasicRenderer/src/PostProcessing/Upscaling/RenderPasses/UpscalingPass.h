#pragma once

#include <functional>

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Scene/Scene.h"
#include "PostProcessing/Upscaling/UpscalingGenerationService.h"
#include "Render/PreparedPass.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "BasicRenderer/Extensions/UpscalerReactiveMask.h"

// What the upscaler's SDK takes per run: the camera (matrices, jitter), the frame's number and elapsed time. Update writes them
// each run, and Record reads them when the pass is recorded at execution, after Update in the same run: a kept recording of
// Post records this pass again at each submission with the frame data it was prepared with (M6 step 4).
struct UpscalingRunValues {
    Components::Camera camera;
    uint64_t frameNumber = 0;
    double deltaTime = 0;
};

struct UpscalingFrameData {
    std::shared_ptr<const br::render::UpscalingGenerationService> service;
    std::shared_ptr<const UpscalingRunValues> run;
    std::shared_ptr<org::PixelBuffer> hdr;
    std::shared_ptr<org::PixelBuffer> output;
    std::shared_ptr<org::PixelBuffer> depth;
    std::shared_ptr<org::PixelBuffer> motion;
    std::shared_ptr<org::PixelBuffer> diffuseAlbedo;
    std::shared_ptr<org::PixelBuffer> specularAlbedo;
    std::shared_ptr<org::PixelBuffer> normalRoughness;
    std::shared_ptr<org::PixelBuffer> specularHitDistance;
    std::shared_ptr<org::PixelBuffer> reactiveMask; // an extension's (Extensions/UpscalerReactiveMask.h), or null
    bool operator==(const UpscalingFrameData&) const = default;
};

class UpscalingPass
    : public org::TypedRenderGraphPass<UpscalingPass, UpscalingFrameData>, public org::IDynamicDeclaredResources {
public:
    explicit UpscalingPass(
        std::shared_ptr<const br::render::UpscalingGenerationService> service)
        : m_service(std::move(service)) {}

    // An extension gave the upscaler a reactive mask, replaced it or took it away: declare again.
    bool DeclaredResourcesChanged() const override { return m_reactiveMask != br::render::GetUpscalerReactiveMask(); }

    void Declare(org::PassBuilder& declaration) {
        auto* builder = &declaration;
        m_reactiveMask = br::render::GetUpscalerReactiveMask();
        // The SDK evaluation takes this run's values (UpscalingRunValues): recorded at execution, in a batch of its own.
        declaration.RecordAtExecution();
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
            builder->ShaderResource(
                Builtin::Color::HDRColorTarget,
                motionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture);
            if (m_reactiveMask) builder->ShaderResource(m_reactiveMask);
            builder->UnorderedAccess(upscaledHDR);
            return;
        }

        if (upscalingMode == UpscalingMode::None && backend == rhi::Backend::Vulkan) {
            builder->CopySource(Builtin::Color::HDRColorTarget);
            builder->CopyDestination(upscaledHDR);
            builder->ShaderResource(
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
            builder->ShaderResource(
                Builtin::Color::HDRColorTarget,
                motionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture);
            builder->UnorderedAccessClear(upscaledHDR);
            builder->InternalTransition(upscaledHDR, vulkanStreamlineExitState);
        }
        else {
            builder->LegacyInterop(
                Builtin::Color::HDRColorTarget,
                motionVectors,
                Builtin::PrimaryCamera::ProjectedDepthTexture,
                upscaledHDR)
                .InternalTransition(upscaledHDR, dx12StreamlineExitState);
            // Super Resolution's bias-current-colour hint. Ray Reconstruction and no upscaling do not take it.
            if (m_reactiveMask && upscalingMode == UpscalingMode::DLSS) builder->LegacyInterop(m_reactiveMask);
            if (upscalingMode == UpscalingMode::DLSSRayReconstruction) {
                builder->LegacyInterop(
                    Builtin::Surface::BaseColorOpacity,
                    Builtin::Surface::NormalRoughness,
                    Builtin::RayTracing::SpecularAlbedo,
                    Builtin::RayTracing::SpecularHitDistance);
            }
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
        if (m_service->Mode() == UpscalingMode::DLSSRayReconstruction) {
            m_pDiffuseAlbedo = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::Surface::BaseColorOpacity);
            m_pSpecularAlbedo = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::RayTracing::SpecularAlbedo);
            m_pNormalRoughness = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::Surface::NormalRoughness);
            m_pSpecularHitDistance = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::RayTracing::SpecularHitDistance);
        }
    }

    // This run's values (UpscalingRunValues). The frame number counts the graph's runs, as the preparation's does.
    void Update(const org::UpdateExecutionContext& execution) override {
        const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        if (context) m_runValues->camera = context->primaryCamera;
        m_runValues->frameNumber = ++m_runs;
        m_runValues->deltaTime = execution.deltaTime;
    }
    // Recorded at each run, so only the service it evaluates through names its recording.
    void Revision(std::vector<uint64_t>& out) const { out.push_back(reinterpret_cast<uintptr_t>(m_service.get())); }

    UpscalingFrameData Prepare(const org::PassPrepareContext&) {
        return UpscalingFrameData{
            .service = m_service,
            .run = m_runValues,
            .hdr = m_pHDRTarget,
            .output = m_pUpscaledHDRTarget,
            .depth = m_pDepthTexture,
            .motion = m_pMotionVectors,
            .diffuseAlbedo = m_pDiffuseAlbedo,
            .specularAlbedo = m_pSpecularAlbedo,
            .normalRoughness = m_pNormalRoughness,
            .specularHitDistance = m_pSpecularHitDistance,
            .reactiveMask = m_service->Mode() == UpscalingMode::DLSS || m_service->Mode() == UpscalingMode::FSR3 ? m_reactiveMask : nullptr,
        };
    }
    static void Record(const UpscalingFrameData& data, org::PassRecordContext& recording) {
        const UpscalingInputs inputs{
            .color = data.hdr.get(),
            .output = data.output.get(),
            .depth = data.depth.get(),
            .motionVectors = data.motion.get(),
            .diffuseAlbedo = data.diffuseAlbedo.get(),
            .specularAlbedo = data.specularAlbedo.get(),
            .normalRoughness = data.normalRoughness.get(),
            .specularHitDistance = data.specularHitDistance.get(),
            .reactiveMask = data.reactiveMask.get(),
        };
        const auto& run = *data.run;
        const bool written = data.service->Evaluate(recording.Commands(), run.camera,
            run.frameNumber, run.deltaTime, inputs);
        recording.RestoreDescriptorHeaps();
        if (!written && data.service->Backend() == rhi::Backend::D3D12 && data.output) {
            ClearOutput(recording.Commands(), *data.output);
        }
    }

private:
    // DLSS didn't run (UpscalingManager::HandleFailedEvaluation) and nothing could stand in for it: the output is aliased, so
    // black rather than whatever the memory held. After RestoreDescriptorHeaps, since Streamline may have bound its own. The
    // output is declared as legacy interop, left in the common state.
    static void ClearOutput(rhi::CommandList& commands, org::PixelBuffer& output) {
        rhi::TextureBarrier barrier{};
        barrier.texture = output.GetAPIResource().GetHandle();
        barrier.range = { 0, 1, 0, 1 };
        barrier.beforeSync = rhi::ResourceSyncState::All;
        barrier.afterSync = rhi::ResourceSyncState::ClearUnorderedAccessView;
        barrier.beforeAccess = rhi::ResourceAccessType::Common;
        barrier.afterAccess = rhi::ResourceAccessType::UnorderedAccessClear;
        barrier.beforeLayout = rhi::ResourceLayout::Common;
        barrier.afterLayout = rhi::ResourceLayout::UnorderedAccess;
        rhi::BarrierBatch batch{};
        batch.textures = { &barrier };
        commands.Barriers(batch);
        rhi::UavClearInfo info{};
        info.resource = output.GetAPIResource();
        info.shaderVisible = output.GetUAVShaderVisibleInfo(0).slot;
        info.cpuVisible = output.GetUAVNonShaderVisibleInfo(0).slot;
        commands.ClearUavFloat(info, {});
        std::swap(barrier.beforeSync, barrier.afterSync);
        barrier.afterSync = rhi::ResourceSyncState::All;
        std::swap(barrier.beforeAccess, barrier.afterAccess);
        std::swap(barrier.beforeLayout, barrier.afterLayout);
        commands.Barriers(batch);
    }

    std::shared_ptr<org::PixelBuffer> m_pHDRTarget;
    std::shared_ptr<org::PixelBuffer> m_pMotionVectors;
	std::shared_ptr<org::PixelBuffer> m_pDepthTexture;
	std::shared_ptr<org::PixelBuffer> m_pUpscaledHDRTarget;
    std::shared_ptr<org::PixelBuffer> m_pDiffuseAlbedo;
    std::shared_ptr<org::PixelBuffer> m_pSpecularAlbedo;
    std::shared_ptr<org::PixelBuffer> m_pNormalRoughness;
    std::shared_ptr<org::PixelBuffer> m_pSpecularHitDistance;
    std::shared_ptr<org::PixelBuffer> m_reactiveMask; // the one this declaration names

    std::shared_ptr<const br::render::UpscalingGenerationService> m_service;
    std::shared_ptr<UpscalingRunValues> m_runValues = std::make_shared<UpscalingRunValues>();
    uint64_t m_runs = 0;

};
