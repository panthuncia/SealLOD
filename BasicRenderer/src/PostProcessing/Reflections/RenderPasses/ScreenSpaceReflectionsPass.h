#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"
#include "PostProcessing/Reflections/ScreenSpaceReflectionsGenerationService.h"

struct ScreenSpaceReflectionsFrameData {
    std::shared_ptr<const br::render::ScreenSpaceReflectionsGenerationService> service;
    Components::Camera camera;
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
    std::shared_ptr<org::PixelBuffer> hdr, depth, normals, motion, environment, brdf, output;
};

class ScreenSpaceReflectionsPass
    : public org::TypedRenderGraphPass<ScreenSpaceReflectionsPass, ScreenSpaceReflectionsFrameData> {
public:
    ScreenSpaceReflectionsPass()
        : m_service(br::render::ScreenSpaceReflectionsGenerationService::Create()) {}

    void Declare(org::PassBuilder& declaration) {
        auto* builder = &declaration;
        builder->PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        builder->WithLegacyInterop(Builtin::Color::HDRColorTarget,
            Builtin::Surface::Motion,
            Builtin::PrimaryCamera::DepthTexture,
            Builtin::Surface::NormalRoughness,
            Builtin::Surface::Motion,
            Builtin::Environment::CurrentPrefilteredCubemap,
            Builtin::BRDFLUT,
            Builtin::PostProcessing::ScreenSpaceReflections);

        org::ResourceState outState{
        .access = rhi::ResourceAccessType::Common, 
        .layout = rhi::ResourceLayout::Common, 
        .sync = rhi::ResourceSyncState::All};

		org::ResourceIdentifierAndRange outResource(Builtin::PostProcessing::ScreenSpaceReflections, {});

        builder->WithInternalTransition(
            outResource,
            outState);
    }

    void Initialize() {
        m_pHDRTarget = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::Color::HDRColorTarget);
        m_pDepthTexture = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::PrimaryCamera::DepthTexture);
		m_pNormals = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::Surface::NormalRoughness);
		m_pMotionVectors = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::Surface::Motion);
		m_pEnvironmentCubemap = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::Environment::CurrentPrefilteredCubemap);
        m_pBRDFLUT = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::BRDFLUT);
		m_pSSSROutput = m_resourceRegistryView->RequestSharedAs<org::PixelBuffer>(Builtin::PostProcessing::ScreenSpaceReflections);
    }

    ScreenSpaceReflectionsFrameData Prepare(const org::PassPrepareContext& preparation) {
        const auto* context = preparation.preparationData->Get<UpdateContext>();
        return {.service=m_service, .camera=context->primaryCamera,
            .resourceHeap=context->textureDescriptorHeap.GetHandle(),
            .samplerHeap=context->samplerDescriptorHeap.GetHandle(),
            .hdr=m_pHDRTarget, .depth=m_pDepthTexture, .normals=m_pNormals,
            .motion=m_pMotionVectors, .environment=m_pEnvironmentCubemap,
            .brdf=m_pBRDFLUT, .output=m_pSSSROutput};
    }
    static void Record(const ScreenSpaceReflectionsFrameData& data, org::PassRecordContext& recording) {
        recording.Commands().SetDescriptorHeaps(data.resourceHeap, data.samplerHeap);
        data.service->Evaluate(
            recording.Commands(), data.camera, data.hdr.get(), data.depth.get(),
            data.normals.get(), data.motion.get(),
            data.environment.get(), data.brdf.get(), data.output.get());
    }

private:

    std::shared_ptr<org::PixelBuffer> m_pHDRTarget, m_pMotionVectors, m_pDepthTexture,
        m_pNormals, m_pEnvironmentCubemap, m_pBRDFLUT, m_pSSSROutput;
    std::shared_ptr<const br::render::ScreenSpaceReflectionsGenerationService> m_service;
};
