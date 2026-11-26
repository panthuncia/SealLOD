#pragma once

#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Scene/Scene.h"
#include "PostProcessing/Reflections/ScreenSpaceReflectionsGenerationService.h"

// What FFX SSSR takes per run: the camera and the descriptor heaps. Update writes them each run and Record reads them: the
// SDK dispatch is recorded at execution (M6 step 5), so a kept recording of Shading records it again at each submission, with
// the frame data it was prepared with.
struct ScreenSpaceReflectionsRunValues {
    Components::Camera camera;
    rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
};

struct ScreenSpaceReflectionsFrameData {
    std::shared_ptr<const br::render::ScreenSpaceReflectionsGenerationService> service;
    std::shared_ptr<const ScreenSpaceReflectionsRunValues> run;
    std::shared_ptr<org::PixelBuffer> hdr, depth, normals, motion, environment, brdf, output;
    bool operator==(const ScreenSpaceReflectionsFrameData&) const = default;
};

class ScreenSpaceReflectionsPass
    : public org::TypedRenderGraphPass<ScreenSpaceReflectionsPass, ScreenSpaceReflectionsFrameData> {
public:
    ScreenSpaceReflectionsPass()
        : m_service(br::render::ScreenSpaceReflectionsGenerationService::Create()) {}

    void Declare(org::PassBuilder& declaration) {
        auto* builder = &declaration;
        builder->PreferQueue(org::QueueKind::Compute).AutomaticQueueAssignment();
        // The SDK takes this run's camera and constants: recorded at execution, in a batch of its own.
        declaration.RecordAtExecution();
        builder->LegacyInterop(Builtin::Color::HDRColorTarget,
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

        builder->InternalTransition(
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

    void Update(const org::UpdateExecutionContext& execution) override {
        const auto* context = execution.hostData ? execution.hostData->Get<UpdateContext>() : nullptr;
        if (!context) return;
        m_run->camera = context->primaryCamera;
        m_run->resourceHeap = context->textureDescriptorHeap.GetHandle();
        m_run->samplerHeap = context->samplerDescriptorHeap.GetHandle();
    }
    // Recorded at each run, so only the service it evaluates through names its recording.
    void Revision(std::vector<uint64_t>& out) const { out.push_back(reinterpret_cast<uintptr_t>(m_service.get())); }

    ScreenSpaceReflectionsFrameData Prepare(const org::PassPrepareContext&) {
        return {.service=m_service, .run=m_run,
            .hdr=m_pHDRTarget, .depth=m_pDepthTexture, .normals=m_pNormals,
            .motion=m_pMotionVectors, .environment=m_pEnvironmentCubemap,
            .brdf=m_pBRDFLUT, .output=m_pSSSROutput};
    }
    static void Record(const ScreenSpaceReflectionsFrameData& data, org::PassRecordContext& recording) {
        recording.Commands().SetDescriptorHeaps(data.run->resourceHeap, data.run->samplerHeap);
        data.service->Evaluate(
            recording.Commands(), data.run->camera, data.hdr.get(), data.depth.get(),
            data.normals.get(), data.motion.get(),
            data.environment.get(), data.brdf.get(), data.output.get());
        recording.RestoreDescriptorHeaps();
    }

private:

    std::shared_ptr<org::PixelBuffer> m_pHDRTarget, m_pMotionVectors, m_pDepthTexture,
        m_pNormals, m_pEnvironmentCubemap, m_pBRDFLUT, m_pSSSROutput;
    std::shared_ptr<const br::render::ScreenSpaceReflectionsGenerationService> m_service;
    std::shared_ptr<ScreenSpaceReflectionsRunValues> m_run = std::make_shared<ScreenSpaceReflectionsRunValues>();
};
