//
// Created by matth on 6/25/2024.
//

#include <BasicRenderer/Renderer.h>

#include <chrono>
#include "Runtime/Publication/PersistentRendererPublication.h"
#include "OpenRenderGraph/GraphInstance.h"
#include <BasicRenderer/Assets/ClusterLODTypes.h>

#define _USE_MATH_DEFINES
#include <optional>
#include <math.h>
#include <atlbase.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <array>
#include <stacktrace>
#include <thread>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <typeindex>
#include <utility>

#include <rhi_debug.h>
#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>
#include "Utilities/Utilities.h"
#include "Runtime/Device/DeviceManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Pipeline/PipelineState/PipelineLibrary.h"
#include "Runtime/GraphIntegration/FrameGraphExtensions.h"
#include "OpenRenderGraph/GraphHost.h"
#include "Runtime/Resources/ResourceManager.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Runtime/Frame/RendererFrameInputs.h"
#include <BasicRenderer/Streaming/RendererStateRequestService.h>
#include "Diagnostics/Telemetry/FrameTaskGraphTelemetry.h"
#include "Materials/TextureStreaming/TextureImageTableArtifacts.h"
#include "BasicRenderer/Streaming/ViewStateArtifacts.h"
#include <BasicRenderer/Streaming/PoseState.h>
#include "BasicRenderer/Streaming/LightStateArtifacts.h"
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Render/PassBuilders.h"
#include "BasicRenderer/Extensions/Buffers/DynamicBuffer.h"
#include "RenderPasses/Base/RenderPass.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Runtime/GraphIntegration/FrameBuild.h"
#include "Runtime/Renderer/FrameBuildWorker.h"
#include "BasicRenderer/Assets/MaterialTextureStreaming.h"
#include "Lighting/Environment/RenderPasses/SkyboxRenderPass.h"
#include "Lighting/PathTracing/RenderPasses/PathTracePass.h"
#include "Lighting/Stochastic/StochasticLightingExtension.h"
#include "Camera/Lens/RenderPasses/LensSelfTestPass.h"
#include "Camera/Lens/RenderPasses/LensFocusPass.h"
#include "Camera/Lens/RenderPasses/LensFlarePasses.h"
#include "Lighting/Environment/RenderPasses/EnvironmentFilterPass.h"
#include "Diagnostics/RenderPasses/DebugSpheresPass.h"
#include "Diagnostics/RenderPasses/DebugSkeletonPass.h"
#include "PostProcessing/Downsampling/RenderPasses/Downsample.h"
#include "PostProcessing/ToneMapping/RenderPasses/Tonemapping.h"
#include "PostProcessing/Upscaling/RenderPasses/UpscalingPass.h"
#include "PostProcessing/Upscaling/RenderPasses/DilateMotionVectorsPass.h"
#include "PostProcessing/Exposure/RenderPasses/luminanceHistogram.h"
#include "PostProcessing/Exposure/RenderPasses/luminanceHistogramAverage.h"
#include "VirtualGeometry/Rasterization/RenderPasses/ClearVisibilityBufferPass.h"
#include "Diagnostics/RenderPasses/DebugResolvePass.h"
#include "Diagnostics/Menu/RenderPasses/MenuRenderPass.h"
#include "Runtime/Frame/RenderPasses/PresentPass.h"
#include "Scene/Views/RenderPasses/PrimaryCameraUploadPass.h"
#include "Resources/TextureDescription.h"
#include "Diagnostics/Menu/Menu.h"
#include "Runtime/Device/DeletionManager.h"
#include "Runtime/Device/DescriptorHeapManager.h"
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "Scene/ECS/RendererECSManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "Scene/Objects/ObjectManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Lighting/Lights/LightManager.h"
#include "Lighting/Environment/EnvironmentManager.h"
#include "Animation/Skeletons/SkeletonManager.h"
#include "Runtime/IO/ReadbackManager.h"
#include "Runtime/IO/ReadbackCaptureService.h"
#include "Scene/Views/ViewManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "Materials/MaterialManager.h"
#include "Materials/TextureStreaming/TextureStreamingManager.h"
#include "Terrain/Residency/TerrainManager.h"
#include "Assets/Textures/TextureFactory.h"
#include "Utilities/MathUtils.h"
#include <BasicScene/MovementState.h>
#include "ThirdParty/XeGTAO.h"
#include "PostProcessing/AmbientOcclusion/AmbientOcclusionSettings.h"
#include "Lighting/Environment/EnvironmentManager.h"
#if BASICRENDERER_HAS_INTEROP_VALIDATION
#include "Validation/SARPInteropValidation.h"
#endif
#include "PostProcessing/ToneMapping/TonemapTypes.h"
#include "../../../generated/BuiltinResources.h"
#include "Resources/ResourceIdentifier.h"
#include "Runtime/GraphIntegration/RenderGraphBuildHelper.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "PostProcessing/FidelityFX/FFXManager.h"
#include "Runtime/IO/DirectStorageManager.h"
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include "Runtime/GraphIntegration/IOExtension.h"
#include "VirtualGeometry/GraphIntegration/CLodExtension.h"
#include "VirtualShadows/GraphIntegration/CLodShadowVariant.h"
#include <BasicRenderer/Extensions/VirtualShadowCasterProvider.h>
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "Diagnostics/RenderPasses/DebugGridPass.h"
#include <BasicRenderer/Diagnostics/ReadbackCaptures.h>
#include "Render/Runtime/ScopedActiveGraphServices.h"
#include "Resources/Resource.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>
#include <BasicRenderer/Diagnostics/ReadbackCaptures.h>
#include "Resources/DynamicResource.h"
#include "Resources/ExternalTextureResource.h"
#include "Render/MemoryIntrospectionBackend.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"
#include "Runtime/Scheduling/TbbTaskService.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include "BasicRenderer/Pipeline/MeshDrawWorkload.h"
#include "Scene/Objects/IndirectStateArtifacts.h"
#include "Materials/Publication/MaterialStateArtifacts.h"
#include "Materials/TextureStreaming/TextureBindingArtifacts.h"
#include "VirtualGeometry/Streaming/Publication/CLodResidencyStorageArtifacts.h"
#include "Terrain/Residency/TerrainStateArtifacts.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include <BasicRenderer/Streaming/StaticSceneArtifacts.h>
#include "VirtualGeometry/Streaming/Publication/GeometryResidencyStateArtifacts.h"
#include "VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.h"
#include "Scene/Objects/ObjectBufferStateArtifacts.h"
#include "BasicRenderer/Pipeline/RasterBucketFlags.h"
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasTelemetry.h"
#include "BasicRenderer/Diagnostics/TerrainRvtTelemetry.h"

namespace {
}

Renderer::PathTracingAccumulation Renderer::GetPathTracingAccumulation() const {
    PathTracingAccumulation result;
    if (const auto state = m_pathTraceState) {
        std::scoped_lock lock(state->mutex);
        result.buffer = state->accumulation;
        result.framesSinceRestart = state->accumulatedFrames;
        result.restarts = state->accumulationRestarts;
    }
    return result;
}

std::string Renderer::HandleLensControl(const std::string& command, const std::string& requestJson) {
    if (command.starts_with("lens.flare.")) {
        if (!m_ghostFlareManager) throw std::runtime_error("the renderer has no lens flare manager");
        return m_ghostFlareManager->HandleControl(command, requestJson);
    }
    if (!m_lensManager) throw std::runtime_error("the renderer has no lens manager");
    return m_lensManager->HandleControl(command, requestJson);
}

bool Renderer::StochasticLightingRequested() const {
    return m_pipelineRecipe.Contains<br::pipeline::StochasticDirectLightingTechnique>()
        && m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>()
        && !m_pipelineRecipe.Contains<br::pipeline::PathTracingTechnique>()
        && m_clodRayTracingSystem && DeviceManager::GetInstance().GetCLodRayTracingSupported();
}

bool Renderer::StochasticLightingActive() const {
    return StochasticLightingRequested() && m_clodRayTracingSystem->IsInitialized();
}

bool Renderer::VirtualShadowMapsActive() const {
    return m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>() && !StochasticLightingActive();
}

// The consolidation point's graph work (docs/frame_graph_composition_plan.md, M3 step 9). The frame graph is built on the
// build thread (m_frameBuildWorker) with GraphHost::BuildEpochGraph while the attached graph keeps rendering, and attached
// here once it is done. At most one build runs. Requests made while it runs wait, and one follow-up build answers all of
// them once it is attached. A build superseded that way is still attached: it is newer than the running graph and answers
// the requests it started with.
void Renderer::ConsolidateFrameGraph() {
    BT_ZONE_SCOPE("Renderer::ConsolidateFrameGraph");
    // Failures of the attached graph's preparation, reported by the host (the callback logs them, RendererLifecycle).
    if (m_frameHost && !m_useGraphInstance) m_frameHost->PollEpochFailures();
    // Pipelines declared outside a build (a pass redeclared at preparation) are not awaited: drop the finished ones.
    if (auto* pipelines = PSOManager::GetInstance().GetPipelineLibrary()) {
        for (const auto& failure : pipelines->DropSettledDeclared()) spdlog::error("Renderer: {}", failure);
    }
    if (m_frameBuildWorker) {
        if (auto job = m_frameBuildWorker->TakeFinished()) FinishFrameBuild(std::move(job));
    }
    if (!currentRenderGraph && !m_frameRevisions.TopologyPending() && !FrameBuildInFlight()) RequestGraphRebuild("first build");
    // A second round only after a render-thread build whose graph was installed and then failed (its rollback follows at once).
    for (int round = 0; round < 2; ++round) {
        if (round && !m_nextFrameBuildSynchronous) return;
        StartAndRunFrameBuild();
    }
}

void Renderer::StartAndRunFrameBuild() {
    if (!m_frameRevisions.TopologyPending() || FrameBuildInFlight()) return;

    auto job = StartFrameBuild(m_frameRevisions.Consume());
    if (!job) return;
    // Requests the start itself made (setters it runs) are answered by this build.
    if (auto during = m_frameRevisions.Consume(); !during.Empty()) {
        job->reason += "; during its start: " + during.Describe();
        job->requests.Merge(std::move(during));
    }
    // On the render thread: the first build (nothing renders meanwhile), the in-place graph, the synchronous fallback, and the
    // build after a GPU idle wait (resize, upscaler restart: the attached graph's resources are gone).
    const bool onRenderThread = !m_frameBuildWorker || !currentRenderGraph || m_nextFrameBuildSynchronous;
    m_nextFrameBuildSynchronous = false;
    if (!onRenderThread) {
        m_graphBuildStats.inFlightSinceFrame = m_totalFramesRendered;
        m_frameBuildWorker->Start(std::move(job));
        return;
    }
    job->onRenderThread = true;
    ++m_graphBuildStats.onRenderThread;
    try {
        RunFrameBuild(*job);
    } catch (const std::exception& error) {
        job->error = error.what();
    }
    // Requests made during a build on this thread are answered by it, as before builds moved off it.
    if (auto during = m_frameRevisions.Consume(); !during.Empty()) {
        job->reason += "; during the build: " + during.Describe();
        job->requests.Merge(std::move(during));
    }
    FinishFrameBuild(std::move(job));
}

bool Renderer::FrameBuildInFlight() const {
    return m_frameBuildWorker && !m_frameBuildWorker->Idle();
}

void Renderer::DiscardFrameBuildInFlight(std::string_view why) {
    if (!m_frameBuildWorker) return;
    auto job = m_frameBuildWorker->Wait();
    if (!job) return;
    // Built from inputs that are gone now. Nothing of it was published; its graph never ran.
    job->build->outputs.resourceEntityWrites.clear();
    job->graph.reset();
    ++m_graphBuildStats.discarded;
    spdlog::info("Renderer: frame graph build {} dropped before its attach ({}); its requests are asked again: {}", job->serial, why,
        job->reason);
    m_frameRevisions.Restore(std::move(job->requests));
}

std::shared_ptr<br::render::FrameBuildJob> Renderer::StartFrameBuild(br::render::FrameRevisions::Pending requests) {
    BT_ZONE_SCOPE("Renderer::StartFrameBuild");
    const auto begin = std::chrono::steady_clock::now();
    if (!IsSceneReadyForFrame()) {
        m_frameRevisions.Restore(std::move(requests));
        return nullptr;
    }

    auto primaryCameraEntity = GetValidatedPrimaryRenderCamera(true);
    if (!primaryCameraEntity) {
        spdlog::warn("Renderer: primary camera bridge is not ready during render graph creation. Deferring rebuild.");
        m_frameRevisions.Restore(std::move(requests));
        return nullptr;
    }

    // No GPU wait here (M3 step 8, docs/frame_graph_composition_plan.md). The host keeps the old graph until the work submitted
    // so far completes (GraphHost::ReleaseRetiredGraphs), so its last frames overlap the build and the new graph's first frame.
    // Deferred deletions and descriptor releases retire by the runtime's frontier, which answers points on a retired graph's
    // timelines as reached; after-graph readback captures do the same (FrameReadbackCaptures::ProcessCompleted).
    // A graph rebuilt in place (BASICRENDERER_FRAME_GRAPH_INSTANCE) has no old graph to hold its frames' objects: it still waits.
    if (m_useGraphInstance) {
        BT_ZONE_SCOPE("Renderer::StartFrameBuild::StallPipeline");
        StallPipeline();
        if (m_readbackCaptures) m_readbackCaptures->ProcessCompleted();
        org::DescriptorHeapManager::GetInstance().DrainDeferredReleasesAfterDeviceIdle();
        PSOManager::GetInstance().DrainRetiredLivePipelinesAfterDeviceIdle();
        org::DeletionManager::GetInstance().DrainAll();
    }

        // The runtime's upload, upload policy and descriptor services were initialized with the runtime
        // (RendererLifecycle). They are not initialized again here: that would drop their pending work.
        if (m_pSkeletonManager)
            m_pSkeletonManager->SetUploadService(m_orgRuntime->Uploads());
        ::ResourceManager::GetInstance().SetUploadService(m_orgRuntime->Uploads());

        if (!m_renderGraphRuntimeInitialized) {

        if (auto* textureFactory = m_pTextureFactory.get()) {
            textureFactory->SetUploadService(m_orgRuntime->Uploads());
            textureFactory->SetReadbackService(m_readbackCaptures);
        }
        if (m_pEnvironmentManager)
            m_pEnvironmentManager->SetUploadService(m_orgRuntime->Uploads());
        if (m_pMaterialManager) {
			m_pMaterialManager->SetRendererStateServices(
				m_rendererStateRequests.get(), m_orgRuntime->Uploads());
			m_pMaterialManager->SetDescriptorService(m_orgRuntime->Descriptors());
		}
		if (m_pObjectManager)
			m_pObjectManager->SetRendererStateServices(
				m_rendererStateRequests.get(), m_orgRuntime->Uploads(),
				m_numFramesInFlight);
		if (m_pMeshManager)
			m_pMeshManager->SetRendererStateServices(
				m_rendererStateRequests.get(), m_orgRuntime->Uploads(),
				m_numFramesInFlight);
		if (m_pIndirectCommandBufferManager)
			m_pIndirectCommandBufferManager->SetRendererStateServices(
				m_rendererStateRequests.get(), m_orgRuntime->Uploads());
		if (m_sceneIngestionServices.execution.generation) {
			m_sceneIngestionServices.execution.generation->uploads.store(
				m_orgRuntime->Uploads(), std::memory_order_release);
			m_sceneIngestionServices.execution.generation->descriptors.store(
				m_orgRuntime->Descriptors(), std::memory_order_release);
		}
		if (m_pTerrainManager)
			m_pTerrainManager->SetRendererStateServices(
				m_rendererStateRequests.get(), m_orgRuntime->Uploads(),
				m_orgRuntime->Descriptors());


        RendererECSManager::GetInstance().CreateRenderPhaseEntity(Engine::Primary::CLodTransparentPass);
		m_renderGraphRuntimeInitialized = true;
    }

    auto job = std::make_shared<br::render::FrameBuildJob>();
    job->build = std::make_shared<br::render::FrameBuild>();
    job->requests = std::move(requests);
    job->reason = job->requests.Empty() ? std::string("renderer graph rebuild") : job->requests.Describe();
    job->serial = ++m_frameBuildSerial;
    auto& frameBuild = *job->build;
    br::settings_detail::t_writesUnderSnapshot.clear();
    {
        br::render::ScopedFrameBuild frameBuildScope(frameBuild);
        // The BVH is made first when anything traces it: that decides StochasticLightingActive, which the setting below and
        // the build's choice of extensions read.
        frameBuild.inputs.rayTracingReady = PrepareFrameRayTracing();
        // Passes built below read this to know whether the shadow map's resources exist.
        SettingsManager::GetInstance().getSettingSetter<bool>("virtualShadowMapsActive")(VirtualShadowMapsActive());
        // The build reads one snapshot of the settings (M3, docs/frame_graph_composition_plan.md): what it builds is consistent,
        // and it runs on the build thread while the render thread keeps writing the live values.
        auto settings = SettingsManager::GetInstance().Snapshot();
        SettingsSnapshotScope settingsScope(settings);
        MakeFrameBuild(frameBuild, primaryCameraEntity);
        frameBuild.inputs.settings = std::move(settings);
        // The host's factories are fixed per id; only a change of the recipe's extension ids changes them. Owner thread, and
        // never while a build runs: a build takes the host's extension list when it starts.
        if (!m_useGraphInstance) RegisterFrameHostExtensions(frameBuild.inputs.extensionIds);
    }
    if (auto& writes = br::settings_detail::t_writesUnderSnapshot; !writes.empty()) {
        std::sort(writes.begin(), writes.end());
        writes.erase(std::unique(writes.begin(), writes.end()), writes.end());
        std::string names;
        for (const auto& name : writes) names += (names.empty() ? "" : ", ") + name;
        spdlog::warn("Renderer: the frame build's inputs wrote settings (its snapshot does not see these): {}", names);
        writes.clear();
    }
    job->hostRequestsAtStart = m_useGraphInstance ? 0 : m_frameHost->RebuildRequests(kFrameEpoch);
    // Published-state resolvers capture against a manifest lease, as each frame's preparation does (RendererFrameUpdate).
    job->captureContext = std::make_shared<const org::ResolverCaptureContext>(m_context.publishedManifestLease,
        m_context.publishedRendererState ? m_context.publishedRendererState->bindingBundle : nullptr);
    job->started = std::chrono::steady_clock::now();
    job->prepareMs = std::chrono::duration<double, std::milli>(job->started - begin).count();
    ++m_graphBuildStats.started;
    m_graphBuildStats.lastPrepareMs = job->prepareMs;
    return job;
}

void Renderer::RunFrameBuild(br::render::FrameBuildJob& job) {
    BT_ZONE_SCOPE("Renderer::RunFrameBuild");
    const auto begin = std::chrono::steady_clock::now();
    auto& frameBuild = *job.build;
    // Extensions reach the build through br::render::CurrentFrameBuild(): the host's factories make them in it (the build's
    // makers), and they record their ECS writes into it.
    br::render::ScopedFrameBuild frameBuildScope(frameBuild);
    br::settings_detail::t_writesUnderSnapshot.clear();
    {
        SettingsSnapshotScope settingsScope(frameBuild.inputs.settings);
        // The pipelines this build declares, awaited here, on the build's thread (R7): not the render thread's, and not
        // another build's.
        br::PipelineDeclarationScope declarations(PSOManager::GetInstance().GetPipelineLibrary());
        bool built = true;
        if (m_useGraphInstance) {
            // One graph, rebuilt in place on the render thread: it takes this build's new extension instances.
            RendererECSManager::ScopedBuildWorldAccessCheck worldAccessCheck;
            currentRenderGraph->ClearExtensions();
            currentRenderGraph->RegisterExtension(std::make_unique<br::render::DeclarationGraphExtension>(
                [this](org::RenderGraph& graph) { DeclareFrameProviders(graph); }), "BasicRenderer::Providers");
            for (const auto& id : frameBuild.inputs.extensionIds) {
                const auto maker = frameBuild.inputs.extensionMakers.find(id);
                if (maker == frameBuild.inputs.extensionMakers.end() || !maker->second) continue;
                if (auto extension = maker->second()) currentRenderGraph->RegisterExtension(std::move(extension), id);
            }
            currentRenderGraph->RegisterExtension(std::make_unique<br::render::DeclarationGraphExtension>(
                [this](org::RenderGraph& graph) { DeclareFramePasses(graph); }), "BasicRenderer::Passes");
            org::runtime::ScopedActiveGraphServices activeGraphServices(currentRenderGraph->GetUploadService(), currentRenderGraph->GetDescriptorService());
            m_frameInstance->Rebuild([](org::RenderGraph& graph) { graph.PrepareExtensionsForBuild(); });
        } else if (job.onRenderThread) {
            // On the render thread the host builds in place of the old graph (its own build point), as before builds moved off
            // it. A detached build would do the lowering and the first schedule here and the alias placement again at the
            // first preparation: about twice the render-thread time for nothing, since nothing renders meanwhile.
            spdlog::info("Renderer: frame graph build {} on the render thread; why: {}", job.serial, job.reason);
            RendererECSManager::ScopedBuildWorldAccessCheck worldAccessCheck;
            const auto failuresBefore = m_graphBuildStats.epochFailures;
            m_frameHost->RequestRebuild(kFrameEpoch, job.reason);
            m_frameHost->BuildIfRequested();
            m_frameHost->PollEpochFailures();
            if (m_graphBuildStats.epochFailures != failuresBefore) job.error = m_graphBuildStats.lastEpochFailure;
            built = job.error.empty();
            job.installed = built;
        } else {
            // A new graph, detached: its extensions' PrepareForBuild run the renderer's declarations (DeclareFrameProviders,
            // DeclareFramePasses), then the structural build, Setup and the first schedule. The attached graph is not touched.
            spdlog::info("Renderer: frame graph build {} on the build thread; why: {}", job.serial, job.reason);
            RendererECSManager::ScopedBuildWorldAccessCheck worldAccessCheck;
            job.graph = m_frameHost->BuildEpochGraph(kFrameEpoch, job.reason, job.captureContext);
            built = job.graph && job.graph->Built();
        }
        job.pipelinesDeclared = declarations.Declared();
        if (built) {
            BT_ZONE_SCOPE("Renderer::RunFrameBuild::AwaitPipelines");
            const auto waitBegin = std::chrono::steady_clock::now();
            job.pipelineFailures = declarations.Await();
            job.pipelineWaitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitBegin).count();
        }
    }
    if (auto& writes = br::settings_detail::t_writesUnderSnapshot; !writes.empty()) {
        std::sort(writes.begin(), writes.end());
        writes.erase(std::unique(writes.begin(), writes.end()), writes.end());
        job.settingsWritten = std::move(writes);
        writes.clear();
    }
    job.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void Renderer::FinishFrameBuild(std::shared_ptr<br::render::FrameBuildJob> job) {
    BT_ZONE_SCOPE("Renderer::FinishFrameBuild");
    const auto begin = std::chrono::steady_clock::now();
    auto& frameBuild = *job->build;
    if (!job->settingsWritten.empty()) {
        std::string names;
        for (const auto& name : job->settingsWritten) names += (names.empty() ? "" : ", ") + name;
        spdlog::warn("Renderer: frame graph build {} wrote settings (its snapshot does not see these): {}", job->serial, names);
    }
    std::string failure = job->error;
    if (failure.empty() && !m_useGraphInstance && !job->installed && !(job->graph && job->graph->Built()))
        failure = job->graph ? job->graph->Error() : std::string("no graph was made");
    if (failure.empty() && !job->pipelineFailures.empty()) {
        failure = std::to_string(job->pipelineFailures.size()) + " pipeline(s) failed to build";
        for (const auto& pipelineFailure : job->pipelineFailures) failure += "; " + pipelineFailure;
    }
    bool askAgain = false;
    if (failure.empty() && !m_useGraphInstance && !job->installed && !m_frameHost->Graph(kUploadEpoch)) {
        // The first build: the host's upload epoch (its upload pass alone) is built and attached first. The host's upload
        // service resolves registry handles against the registry of the graph attached last, which must be the frame's.
        auto uploads = m_frameHost->BuildEpochGraph(kUploadEpoch, "first build");
        const auto attached = uploads && uploads->Built() ? m_frameHost->AttachEpochGraph(uploads) : org::GraphHost::AttachResult{};
        if (!attached.attached)
            failure = "the upload epoch's graph: " + (uploads && !uploads->Built() ? uploads->Error() : attached.reason);
    }
    if (failure.empty() && !m_useGraphInstance && !job->installed) {
        // The swap: the old graph stops and is released once its submitted work completes (no device wait).
        const auto attached = m_frameHost->AttachEpochGraph(job->graph);
        if (!attached.attached) {
            failure = std::string("the host refused it (") + org::GraphHost::AttachRefusalName(attached.refusal) + "): " + attached.reason;
            // The host changed under the build: a new build sees the change.
            askAgain = attached.refusal == org::GraphHost::AttachRefusal::HostConfiguration
                || attached.refusal == org::GraphHost::AttachRefusal::EpochConfiguration
                || attached.refusal == org::GraphHost::AttachRefusal::NewerAttached;
        }
    }
    const auto latencyMs = std::chrono::duration<double, std::milli>(begin - job->started).count();
    if (!failure.empty()) {
        // Nothing of a failed build is published: its extension instances go with its graph, and the attached graph keeps
        // its directory entries, streaming binding and casters.
        frameBuild.outputs.resourceEntityWrites.clear();
        job->graph.reset();
        ++m_graphBuildStats.failed;
        m_graphBuildStats.lastError = failure;
        // A graph the host's own build point installed is in place already: it cannot be kept from running.
        const bool keep = currentRenderGraph && !m_useGraphInstance && !job->installed;
        spdlog::error("Renderer: frame graph build {} failed ({}): {}; why it was built: {}", job->serial,
            keep ? "the attached graph keeps rendering" : "there is no graph to keep", failure, job->reason);
        if (job->installed) {
            // Its pipelines failed after the host installed it: the rollback's build follows at once, on this thread.
            currentRenderGraph = m_frameHost->Graph(kFrameEpoch);
            m_nextFrameBuildSynchronous = true;
        }
        if (m_pipelineRollbackRecipe) {
            // A recipe replacement failed: back to the recipe the attached graph was built from, and its settings.
            HandlePipelineReplacementFailure(std::runtime_error(failure));
            return;
        }
        if (!keep) throw std::runtime_error("the frame graph failed to build: " + failure);
        if (askAgain) m_frameRevisions.Restore(std::move(job->requests));
        return;
    }
    if (!m_useGraphInstance) {
        currentRenderGraph = m_frameHost->Graph(kFrameEpoch);
        if (!currentRenderGraph) throw std::runtime_error("the frame graph host has no graph after its attach");
    }
    m_frameBuild = job->build;
    m_graphBuildSettings = frameBuild.inputs.settings;
    PublishFrameBuild(frameBuild);
    currentRenderGraph->GetMemorySnapshotProvider().SetProvider(
        org::memory::CreateECSMemorySnapshotProvider(RendererECSManager::GetInstance().GetWorld()));
    Menu::GetInstance().SetRenderGraph(currentRenderGraph);
    // The recipe replacement this build carried is in place.
    m_pipelineRollbackRecipe.reset();
    ConfirmPipelineReplacement();

    ++m_graphBuilds;
    m_lastGraphBuildMs = job->buildMs;
    m_lastGraphBuildFrame = m_totalFramesRendered;
    m_lastGraphBuildReasons = job->reason;
    auto& stats = m_graphBuildStats;
    stats.lastAttachMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    stats.lastPipelineWaitMs = job->pipelineWaitMs;
    stats.lastPipelinesDeclared = job->pipelinesDeclared;
    stats.lastLatencyMs = latencyMs;
    stats.lastFrames = job->onRenderThread ? 0 : m_totalFramesRendered - stats.inFlightSinceFrame;
    stats.maxRenderThreadMs = (std::max)(stats.maxRenderThreadMs,
        job->prepareMs + stats.lastAttachMs + (job->onRenderThread ? job->buildMs : 0.0));
    // Requests made while it ran are not answered by it: the follow-up build starts now (ConsolidateFrameGraph).
    if (m_frameRevisions.TopologyPending()) {
        ++stats.superseded;
        m_lastGraphBuildReasons += "; waiting for the next build: " + m_frameRevisions.Peek().Describe();
    }
    // The host's own requests (an extension added, recording timestamps switched) made after the build started.
    if (!m_useGraphInstance && m_frameHost->RebuildRequests(kFrameEpoch) != job->hostRequestsAtStart && m_frameHost->RebuildRequested(kFrameEpoch))
        RequestGraphRebuild("frame graph host: " + m_frameHost->RebuildReasons(kFrameEpoch));
    spdlog::info("Renderer: frame graph build {} attached; built in {:.1f} ms {} ({:.1f} ms of it waiting for {} pipelines), {:.1f} ms after "
        "its start, {} frames rendered meanwhile; render thread {:.1f} ms before it and {:.1f} ms at its attach",
        job->serial, job->buildMs, job->onRenderThread ? "on the render thread" : "on the build thread", job->pipelineWaitMs,
        job->pipelinesDeclared, latencyMs, stats.lastFrames, job->prepareMs, stats.lastAttachMs);
}

void Renderer::MakeFrameBuild(br::render::FrameBuild& build, flecs::entity primaryCamera) {
    BT_ZONE_SCOPE("Renderer::MakeFrameBuild");
    auto& in = build.inputs;
    using namespace br::pipeline;

    in.primaryViewID = primaryCamera.get<Components::RenderViewRef>().viewID;
    in.depth = primaryCamera.get<Components::DepthMap>();
    if (const auto* camera = primaryCamera.try_get<Components::Camera>()) in.camera = *camera;
    in.cameraBuffer = m_pViewManager->GetCameraBuffer();
    in.cullingCameraBuffer = m_pViewManager->GetCullingCameraBuffer();

    if (m_pipelineRecipe.Contains<BrdfIntegrationTechnique>()) in.brdfLut = EnsureBrdfLut();

    if (m_pipelineRecipe.Contains<EnvironmentTechnique>()) {
        const auto& bindings = m_pipelineRecipe.Bindings();
        if (bindings.Contains(Builtin::Environment::CurrentCubemap)) m_defaultEnvironmentCubemap.reset();
        if (bindings.Contains(Builtin::Environment::CurrentPrefilteredCubemap)) m_defaultEnvironmentPrefilteredCubemap.reset();
        if ((!bindings.Contains(Builtin::Environment::CurrentCubemap) && !m_defaultEnvironmentCubemap) ||
            (!bindings.Contains(Builtin::Environment::CurrentPrefilteredCubemap) && !m_defaultEnvironmentPrefilteredCubemap)) {
            CreateDefaultEnvironmentResources();
        }
        in.environmentCubemap = m_defaultEnvironmentCubemap;
        in.environmentPrefilteredCubemap = m_defaultEnvironmentPrefilteredCubemap;
        if (m_currentEnvironment && m_currentEnvironment->GetEnvironmentCubemap() &&
            m_currentEnvironment->GetEnvironmentCubemap()->ImagePtr() &&
            m_currentEnvironment->GetEnvironmentPrefilteredCubemap()) {
            in.environmentCubemap = m_currentEnvironment->GetEnvironmentCubemap()->ImagePtr();
            in.environmentPrefilteredCubemap = m_currentEnvironment->GetEnvironmentPrefilteredCubemap();
            m_warnedUsingFallbackEnvironment = false;
        }
        else if (!m_warnedUsingFallbackEnvironment) {
            spdlog::warn("Renderer: no valid environment is active. Using fallback blank cubemaps.");
            m_warnedUsingFallbackEnvironment = true;
        }
        // A recipe-bound environment declares the state it arrives in; its tracker starts there.
        for (const auto resourceId : { Builtin::Environment::CurrentCubemap, Builtin::Environment::CurrentPrefilteredCubemap }) {
            const auto* binding = bindings.Find(resourceId);
            if (!binding) continue;
            const auto& contract = binding->contract;
            if (contract.initialAccess != rhi::ResourceAccessType::None ||
                contract.initialLayout != rhi::ResourceLayout::Undefined ||
                contract.initialSync != rhi::ResourceSyncState::None) {
                if (auto* tracker = binding->resource->GetStateTracker()) {
                    tracker->Reset(org::RangeSpec{}, org::ResourceState{ contract.initialAccess, contract.initialLayout, contract.initialSync });
                }
            }
        }
    }
    in.hasEnvironment = m_currentEnvironment != nullptr;

    // in.rayTracingReady: PrepareFrameRayTracing, before the settings snapshot.
    const bool rayTracingSupported = DeviceManager::GetInstance().GetCLodRayTracingSupported();
    in.stochasticLightingActive = StochasticLightingActive();
    // Stochastic lighting traces the sun itself.
    in.rayTracedSunShadow = m_rayTracedShadows && rayTracingSupported && !in.stochasticLightingActive;
    in.rayTracedReflections = m_rayTracedReflections && rayTracingSupported;
    in.screenSpaceReflections = m_screenSpaceReflections;
    in.lensFlareRayQuery = m_pipelineRecipe.Contains<PathTracingTechnique>() && m_clodRayTracingSystem
        && m_clodRayTracingSystem->IsInitialized() && rayTracingSupported;

    in.upscalingMode = UpscalingManager::GetInstance().GetCurrentUpscalingMode();
    if (m_pipelineRecipe.Contains<UpscalingTechnique>()) in.upscaling = br::render::UpscalingGenerationService::CaptureCurrent();

    if (m_pipelineRecipe.Contains<DebugOutputTechnique>()) {
        if (!m_debugSceneSnapshots) {
            m_debugSceneSnapshots = br::render::DebugSceneSnapshotService::Create(RendererECSManager::GetInstance().GetWorld());
        }
        in.debugSnapshots = m_debugSceneSnapshots;
    }
    in.drawBoundingSpheres = getDrawBoundingSpheres ? getDrawBoundingSpheres() : false;

    if (m_pipelineRecipe.Contains<ClusterLodShadowTechnique>()) {
        if (!m_vsmTransformChangedQuery) {
            m_vsmTransformChangedQuery = std::make_shared<flecs::query<const Components::ObjectDrawInfo>>(
                RendererECSManager::GetInstance().GetWorld().query_builder<const Components::ObjectDrawInfo>()
                    .with<Components::Active>()
                    .with<Components::RenderTransformUpdated>()
                    .without<Components::SkipShadowPass>()
                    .build());
        }
        in.vsmTransformChangedQuery = m_vsmTransformChangedQuery;
    }

    build.outputs.terrainRvtResources = m_producerPersistentState->terrainRvtResources;
    build.outputs.terrainRvtGeneration = m_producerPersistentState->terrainRvtGeneration;

    // What the build reads of the renderer's own state, copied: the render thread keeps changing it while the build runs.
    in.recipe = std::make_shared<const PipelineRecipe>(m_pipelineRecipe);
    in.clodSlabs = m_pMeshManager ? m_pMeshManager->GetCLodSlabResourceGroup() : nullptr;
    {
        BT_ZONE_SCOPE("Renderer::MakeFrameBuild::Providers");
        // The managers' maps change on this thread every frame. ORG asks a provider for everything it lists when the build
        // registers it, so a copy taken now gives the build what registering the live manager would have.
        org::IResourceProvider* providers[] = { m_pMeshManager.get(), m_pObjectManager.get(), m_pViewManager.get(),
            m_pLightManager.get(), m_pEnvironmentManager.get(), m_pMaterialManager.get(), m_pTerrainManager.get(),
            m_pSkeletonManager.get(), &m_coreResourceProvider, m_lensManager.get(), m_ghostFlareManager.get() };
        for (auto* provider : providers) {
            if (provider) in.providers.push_back(br::render::SnapshotResourceProvider(*provider));
        }
    }

    MakeFrameExtensions(build);
}

bool Renderer::PrepareFrameRayTracing() {
    using namespace br::pipeline;
    if (!m_pipelineRecipe.Contains<ClusterLodTechnique>()) return false;
    const bool pathTraced = m_pipelineRecipe.Contains<PathTracingTechnique>();
    const bool wantsRayTracing = pathTraced || m_rayTracedPrimaryVisibilityMode != 0u || m_rayTracedReflections || m_rayTracedShadows
        || StochasticLightingRequested();
    if (!wantsRayTracing) return false;
    auto& deviceManager = DeviceManager::GetInstance();
    bool ready = false;
    if (m_clodRayTracingSystem && deviceManager.GetCLodRayTracingSupported()) {
        // Only the path tracer's alpha test is the one micromaps are baked against. Read when the BVH is first made.
        m_clodRayTracingSystem->RequestOpacityMicromaps(
            pathTraced && SettingsManager::GetInstance().getSettingGetter<bool>("pathTracingOpacityMicromaps")());
        ready = m_clodRayTracingSystem->EnsureInitialized(deviceManager.GetDevice(), deviceManager.GetRayTracingFeatures());
    }
    if (!ready) spdlog::warn("CLod ray tracing requested but unavailable on this device; rendering with raster and shadow maps.");
    return ready;
}

void Renderer::PublishResourceEntityWrites(br::render::FrameBuildOutputs& outputs) {
    auto& writes = outputs.resourceEntityWrites;
    // The directory's resources are the build's extension instances', which are new every build. An entry this build does
    // not publish again belongs to a graph that is retiring: it leaves the directory now, so first-match readers never see
    // the old graph's buffers (a variant the new graph does not have, say).
    std::unordered_set<std::string> republished;
    for (const auto& write : writes) {
        if (!write.directoryKey.empty()) republished.insert(write.directoryKey);
    }
    for (auto it = m_publishedResourceEntities.begin(); it != m_publishedResourceEntities.end();) {
        if (republished.contains(it->first)) {
            ++it;
            continue;
        }
        if (auto previous = it->second.lock()) {
            if (auto entity = previous->GetECSEntity(); entity && entity.has<Components::Resource>()) entity.remove<Components::Resource>();
        }
        it = m_publishedResourceEntities.erase(it);
    }
    if (writes.empty()) return;
    auto& ecs = RendererECSManager::GetInstance();
    // A resource made off the render thread has a deferred entity until the deferred world operations run. (Its handle
    // reads as empty until then, so this cannot tell it from a resource with no entity at all.)
    const auto deferredEntities = std::count_if(writes.begin(), writes.end(), [](const br::render::ResourceEntityWrite& write) {
        return !write.resource->GetECSEntity();
    });
    if (deferredEntities) {
        spdlog::info("Renderer: {} of the build's {} resource entity writes wait for deferred entities; flushing them", deferredEntities,
            writes.size());
        ecs.FlushDeferredWorldOperations();
    }
    auto& world = ecs.GetWorld();
    for (auto& write : writes) {
        if (!write.directoryKey.empty()) {
            // A resource replaced under the same name leaves the directory, so its first-match readers see the new one.
            auto& published = m_publishedResourceEntities[write.directoryKey];
            if (auto previous = published.lock(); previous && previous != write.resource) {
                if (auto entity = previous->GetECSEntity(); entity && entity.has<Components::Resource>())
                    entity.remove<Components::Resource>();
            }
            published = write.resource;
        }
        if (auto entity = write.resource->GetECSEntity()) write.write(world, entity);
    }
    writes.clear();
}

void Renderer::PublishFrameBuild(br::render::FrameBuild& build) {
    BT_ZONE_SCOPE("Renderer::PublishFrameBuild");
    auto& out = build.outputs;
    PublishResourceEntityWrites(out);
    if (out.virtualShadowCasters && !out.virtualShadowCasters->Empty()) {
        spdlog::info("Renderer: the frame graph has {} virtual shadow caster(s)", out.virtualShadowCasters->Size());
    }
    // CLod streaming now runs with the new graph's binding; without CLod in the graph it stops.
    if (const auto& streaming = m_producerPersistentState->clodStreaming) streaming->AttachGraph(out.clodStreaming);
    m_materialEvaluationInputs = out.materialEvaluationInputs;
    m_pathTraceState = out.pathTraceState;
    if (out.visibilityBuffer) m_pViewManager->AttachVisibilityBuffer(build.inputs.primaryViewID, out.visibilityBuffer);
    // Unless a terrain RVT setting invalidated the cache while the build ran: the next build makes it again.
    if (out.terrainRvt && out.terrainRvtGeneration == m_producerPersistentState->terrainRvtGeneration)
        m_producerPersistentState->terrainRvtResources = out.terrainRvtResources;
}

void Renderer::DeclareFrameProviders(org::RenderGraph& graph) {
    BT_ZONE_SCOPE("Renderer::DeclareFrameProviders");
    auto* frameBuild = br::render::CurrentFrameBuild();
    if (!frameBuild) throw std::logic_error("DeclareFrameProviders: no frame build in progress");
    // The managers' copies MakeFrameBuild took (the graph keeps pointers to them: the providers' extension holds them).
    for (const auto& provider : frameBuild->inputs.providers) graph.RegisterProvider(provider.get());
}

void Renderer::DeclareFramePasses(org::RenderGraph& graph) {
    BT_ZONE_SCOPE("Renderer::DeclareFramePasses");
    const auto probeGraphBuildPhase = [&](const char* phase) {
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    };
    auto* frameBuild = br::render::CurrentFrameBuild();
    if (!frameBuild) throw std::logic_error("DeclareFramePasses: no frame build in progress");
    // The build reads only what MakeFrameBuild took on the render thread, and writes only its outputs.
    const br::render::FrameBuildInputs& in = frameBuild->inputs;
    br::render::FrameBuildOutputs& out = frameBuild->outputs;
    // The recipe the build was started with (the render thread may have replaced the renderer's since).
    if (!in.recipe) throw std::logic_error("DeclareFramePasses: the frame build has no recipe");
    const br::pipeline::PipelineRecipe& recipe = *in.recipe;

    // The primary camera is high-frequency frame data, not renderer-state
    // publication. Insert its graphics-queue upload before all pipeline passes;
    // their camera SRV declarations establish the required RAW dependencies.
    graph.BuildPass<br::render::PrimaryCameraUploadPass>(
        "PrimaryCameraUploadPass", in.cameraBuffer,
        in.cullingCameraBuffer, m_numFramesInFlight);

    const auto& depth = in.depth;
    std::shared_ptr<org::PixelBuffer> depthTexture = depth.depthMap;

    const bool terrainRvtEnabled = recipe.Contains<br::pipeline::TerrainRvtTechnique>();
    auto& materialEvaluationInputs = out.materialEvaluationInputs;
    materialEvaluationInputs = {};
    materialEvaluationInputs.pipelines = &PSOManager::GetInstance();
    materialEvaluationInputs.commandSignatures = &CommandSignatureManager::GetInstance();
    materialEvaluationInputs.clodSlabResources = in.clodSlabs;
    // The opaque CLod variant put its buffers in the build's outputs in its PrepareForBuild, which runs before this.
    const auto& clodPrimary = out.clodPrimary;
    materialEvaluationInputs.visibleClusters = clodPrimary.visibleClusters;
    materialEvaluationInputs.visibleClusterCapacity = clodPrimary.visibleClusterCapacity;
    materialEvaluationInputs.visibleClusterCounter = clodPrimary.visibleClusterCounter;
    materialEvaluationInputs.visibleClusterTransformIndices = clodPrimary.visibleClusterTransformIndices;
    materialEvaluationInputs.reyesDiceQueue = clodPrimary.reyesDiceQueue;
    materialEvaluationInputs.reyesTessTableConfigs = clodPrimary.reyesTessTableConfigs;
    materialEvaluationInputs.reyesTessTableVertices = clodPrimary.reyesTessTableVertices;
    materialEvaluationInputs.reyesTessTableTriangles = clodPrimary.reyesTessTableTriangles;
    br::pipeline::PipelineBuildContext buildContext(
        graph,
        recipe.Bindings(),
        [&](br::pipeline::TechniqueId id, const br::pipeline::TechniqueOptions&) {
            using enum br::pipeline::TechniqueId;
            switch (id) {
            case FrameResources:
                graph.RegisterResource(Builtin::PrimaryCamera::DepthTexture, depthTexture);
                graph.RegisterResource(Builtin::PrimaryCamera::LinearDepthMap, depth.linearDepthMap);
                graph.RegisterResource(
                    Builtin::PrimaryCamera::ProjectedDepthTexture,
                    depth.projectedDepthMap);
                graph.RegisterResource(Builtin::Backbuffer, m_dynamicBackbuffer);
                graph.SetSwapchainResource(m_dynamicBackbuffer);
                graph.RegisterResource(Builtin::PresentationColor, m_dynamicPresentationColor);
                graph.RegisterResource(Builtin::PerFrameBuffer, ::ResourceManager::GetInstance().GetPerFrameBuffer());
                break;
            case BrdfIntegration:
                RegisterBRDFLUT(&graph, in.brdfLut);
                break;
            case Environment: {
                // A recipe binding (SARP's live environment) wins; otherwise the cubemaps MakeFrameBuild chose.
                const auto registerEnvironment = [&](org::ResourceIdentifier resourceId, std::shared_ptr<org::Resource> chosen) {
                    if (const auto* binding = recipe.Bindings().Find(resourceId)) graph.RegisterResource(resourceId, binding->resource);
                    else graph.RegisterResource(resourceId, std::move(chosen));
                };
                registerEnvironment(Builtin::Environment::CurrentCubemap, in.environmentCubemap);
                registerEnvironment(Builtin::Environment::CurrentPrefilteredCubemap, in.environmentPrefilteredCubemap);
                if (m_blueNoiseTexture) graph.RegisterResource(Builtin::Noise::BlueNoise2D, m_blueNoiseTexture);
                if (m_openPBRLookupResources.idealDielectricEnergyComplement) graph.RegisterResource(Builtin::OpenPBR::IdealDielectricEnergyComplement, m_openPBRLookupResources.idealDielectricEnergyComplement);
                if (m_openPBRLookupResources.idealDielectricAverageEnergyComplement) graph.RegisterResource(Builtin::OpenPBR::IdealDielectricAverageEnergyComplement, m_openPBRLookupResources.idealDielectricAverageEnergyComplement);
                if (m_openPBRLookupResources.idealDielectricReflectionRatio) graph.RegisterResource(Builtin::OpenPBR::IdealDielectricReflectionRatio, m_openPBRLookupResources.idealDielectricReflectionRatio);
                if (m_openPBRLookupResources.opaqueDielectricEnergyComplement) graph.RegisterResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement, m_openPBRLookupResources.opaqueDielectricEnergyComplement);
                if (m_openPBRLookupResources.opaqueDielectricAverageEnergyComplement) graph.RegisterResource(Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement, m_openPBRLookupResources.opaqueDielectricAverageEnergyComplement);
                if (m_openPBRLookupResources.idealMetalEnergyComplement) graph.RegisterResource(Builtin::OpenPBR::IdealMetalEnergyComplement, m_openPBRLookupResources.idealMetalEnergyComplement);
                if (m_openPBRLookupResources.idealMetalAverageEnergyComplement) graph.RegisterResource(Builtin::OpenPBR::IdealMetalAverageEnergyComplement, m_openPBRLookupResources.idealMetalAverageEnergyComplement);
                if (m_openPBRLookupResources.fuzzLTC) graph.RegisterResource(Builtin::OpenPBR::FuzzLTC, m_openPBRLookupResources.fuzzLTC);
                break;
            }
            case ClusterLod: {
                // What passes which trace read of the ray tracing scene (CLodRayTracingSystem::DeclareTracing) they declare by
                // name, so it is registered here, before any pass declares. MakeFrameBuild initialized the BVH. The extensions
                // below set up the rest of ray tracing.
                if (in.rayTracingReady) m_clodRayTracingSystem->RegisterGraphResources(graph);
                break; // It and the techniques below own ordered graph extensions.
            }
            case ClusterLodAlpha:
            case ClusterLodShadow:
            case ClusterLodVoxel:
                break; // These techniques own ordered graph extensions.
            case CanonicalSurfaceResources: {
                const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
                org::TextureDescription desc;
                desc.channels = 2;
                desc.format = rhi::Format::R32G32_UInt;
                desc.hasRTV = desc.hasSRV = desc.hasUAV = desc.hasNonShaderVisibleUAV = true;
                desc.allowAlias = true;
                desc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
                auto visibilityBuffer = org::PixelBuffer::CreateSharedUnmaterialized(desc);
                visibilityBuffer->SetName("Visibility Buffer");
                org::memory::SetResourceUsageHint(*visibilityBuffer, "Canonical surface visibility");
                graph.RegisterResource(Builtin::PrimaryCamera::VisibilityTexture, visibilityBuffer);
                // Stochastic lighting traces the sun itself.
                if (in.rayTracedSunShadow) {
                    org::TextureDescription sunDesc;
                    sunDesc.channels = 1;
                    sunDesc.format = rhi::Format::R8_UNorm;
                    sunDesc.hasSRV = sunDesc.hasUAV = true;
                    sunDesc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
                    auto sunMask = org::PixelBuffer::CreateSharedUnmaterialized(sunDesc);
                    sunMask->SetName("CLod RT sun shadow");
                    graph.RegisterResource(Builtin::RayTracing::SunShadow, sunMask);
                }
                if (in.stochasticLightingActive) {
                    // Spot and point lights' diffuse and specular radiance, which the deferred pass adds.
                    for (const auto& [id, name] : { std::pair{ Builtin::StochasticLighting::Diffuse, "Stochastic lighting diffuse" },
                             std::pair{ Builtin::StochasticLighting::Specular, "Stochastic lighting specular" } }) {
                        org::TextureDescription lightingDesc;
                        lightingDesc.channels = 4;
                        lightingDesc.format = rhi::Format::R16G16B16A16_Float;
                        lightingDesc.hasSRV = lightingDesc.hasUAV = true;
                        lightingDesc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
                        auto lighting = org::PixelBuffer::CreateSharedUnmaterialized(lightingDesc);
                        lighting->SetName(name);
                        graph.RegisterResource(id, lighting);
                    }
                }
                if (!br::runtime::renderer_settings::CLodVirtualShadowFrameTelemetryPath().empty()) {
                    // VSM flicker telemetry: the deferred pass reprojects each pixel's
                    // shadow outcome against the previous frame's (ping-pong slices).
                    org::TextureDescription historyDesc;
                    historyDesc.channels = 4;
                    historyDesc.format = rhi::Format::R32G32B32A32_UInt;
                    historyDesc.hasUAV = true;
                    historyDesc.isArray = true;
                    historyDesc.arraySize = 2;
                    historyDesc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
                    auto history = org::PixelBuffer::CreateSharedUnmaterialized(historyDesc);
                    history->SetName("CLod VSM receiver history");
                    graph.RegisterResource(Builtin::Shadows::CLodReceiverHistory, history);
                }
                out.visibilityBuffer = visibilityBuffer;  // attached to the primary view after the build (PublishFrameBuild)
                CreateCanonicalSurfaceResources(&graph);
                CreateDebugVisualizationResources(&graph);
                graph.BuildPass<ClearVisibilityBufferPass>("ClearVisibilityBufferPass");
                graph.SetPassTechnique("ClearVisibilityBufferPass", "Primary Visibility::Canonical Surface Construction");
                break;
            }
            case VisibilityMaterialBinning:
                RegisterVisUtilResources(&graph, false);
                BuildVisibilityMaterialBinningPipeline(&graph, materialEvaluationInputs);
                break;
            case TerrainRvt:
                out.terrainRvt = true;
                RegisterVisUtilResources(&graph, true, false, &out.terrainRvtResources);
                BuildTerrainRvtPipeline(&graph);
                break;
            case TerrainRegionMaterialEvaluation:
                BuildTerrainRegionMaterialEvaluationPipeline(&graph, materialEvaluationInputs);
                break;
            case MaterialEvaluation:
                BuildMaterialEvaluationPipeline(&graph, materialEvaluationInputs, terrainRvtEnabled);
                break;
            case Gtao:
                // The screen-space AO slot: ambientOcclusionTechnique picks what fills it.
                if (br::ambient_occlusion::ReadAmbientOcclusionTechnique() == br::ambient_occlusion::AmbientOcclusionTechnique::XeGTAO) {
                    RegisterGTAOResources(&graph);
                    BuildGTAOPipeline(&graph, in.camera ? &*in.camera : nullptr);
                } else {
                    RegisterCORTAOResources(&graph);
                    BuildCORTAOPipeline(&graph);
                }
                break;
            case ClusteredLighting:
                // Forward runs only when a transparency pass reads them (AVBOIT shading, deep visibility resolve).
                BuildLightClusteringPipeline(&graph, recipe.Contains<br::pipeline::ClusterLodAlphaTechnique>());
                break;
            case PrimaryLighting:
                BuildPrimaryPass(&graph, nullptr,
                    in.hasEnvironment || recipe.Bindings().Contains(Builtin::Environment::CurrentCubemap), in.stochasticLightingActive);
                break;
            case Reflections: {
                if (in.rayTracedReflections) BuildRayTracedReflectionPasses(&graph, in.clodSlabs, recipe.Contains<br::pipeline::TerrainRvtTechnique>());
                else if (in.screenSpaceReflections) BuildSSRPasses(&graph);
                break;
            }
            case PathTracing: {
                auto pathTraceState = MakePathTraceState();
                out.pathTraceState = pathTraceState;
                graph.BuildPass<PathTraceArgsPass>("PathTraceArgsPass", pathTraceState);
                if (m_lensManager) {
                    // The lens's focus for this frame's camera rays (manual, or a step toward the last autofocus measurement).
                    graph.BuildPass<LensFocusPass>("LensFocusPass", m_lensManager);
                    graph.SetPassTechnique("LensFocusPass", "Ray Tracing::Path Tracing");
                }
                graph.SetPassTechnique("PathTraceArgsPass", "Ray Tracing::Path Tracing");
                graph.BuildPass<PathTracePass>("PathTracePass", pathTraceState, m_clodRayTracingSystem, in.clodSlabs,
                    recipe.Contains<br::pipeline::TerrainRvtTechnique>(),
                    in.upscalingMode == UpscalingMode::DLSSRayReconstruction, m_lensManager);
                graph.SetPassTechnique("PathTracePass", "Ray Tracing::Path Tracing");
                if (m_lensManager) {
                    // The primary hits' depth over the autofocus region, for the next frame's focus.
                    graph.BuildPass<LensAutofocusPass>("LensAutofocusPass", m_lensManager);
                    graph.SetPassTechnique("LensAutofocusPass", "Ray Tracing::Path Tracing");
                    graph.BuildPass<LensSelfTestPass>("LensSelfTestPass", m_lensManager);
                    graph.SetPassTechnique("LensSelfTestPass", "Ray Tracing::Path Tracing");
                }
                break;
            }
            case Exposure: {
                auto adapted = CreateIndexedStructuredBuffer(1, sizeof(float), true, false);
                adapted->SetName("Adapted Luminance");
                org::memory::SetResourceUsageHint(*adapted, "Post-Processing resources");
                graph.RegisterResource(Builtin::PostProcessing::AdaptedLuminance, adapted);
                auto histogram = CreateIndexedStructuredBuffer(256, sizeof(uint32_t), true, false);
                histogram->SetName("Luminance Histogram Buffer");
                org::memory::SetResourceUsageHint(*histogram, "Post-Processing resources");
                graph.RegisterResource(Builtin::PostProcessing::LuminanceHistogram, histogram);
				auto& histogramBuilder = graph.BuildPass<LuminanceHistogramPass>("luminanceHistogramPass");
#if BASICRENDERER_HAS_INTEROP_VALIDATION
				br::validation::SARPInteropValidation::ApplyPassPolicy(
					"luminanceHistogramPass", histogramBuilder, DeviceManager::GetInstance().GetPeerBackend());
#endif
                graph.SetPassTechnique("luminanceHistogramPass", "Post Process::Exposure");
                graph.BuildPass<LuminanceHistogramAveragePass>("LuminanceAveragePass");
                graph.SetPassTechnique("LuminanceAveragePass", "Post Process::Exposure");
                break;
            }
            case Upscaling:
                br::render::UpscalingGenerationService::RegisterRayReconstructionGuides(graph);
                if (in.upscalingMode == UpscalingMode::DLSS &&
                    SettingsManager::GetInstance().getSettingGetter<bool>("enableDilatedMotionVectors")()) {
                    graph.BuildPass<DilateMotionVectorsPass>("DilateMotionVectorsPass");
                    graph.SetPassTechnique("DilateMotionVectorsPass", "Post Process::Upscaling");
                }
                graph.BuildPass<UpscalingPass>("UpscalingPass", in.upscaling);
                graph.SetPassTechnique("UpscalingPass", "Post Process::Upscaling");
                // The physical lens's ghost flares, added to the upscaled colour (Ray Reconstruction never sees them) before
                // bloom and tonemapping. Path traced, with a BVH, their sources' visibility is by ray query.
                if (m_ghostFlareManager && SettingsManager::GetInstance().getSettingGetter<bool>("lensFlareEnabled")()) {
                    const auto output = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
                    const bool rayQuery = in.lensFlareRayQuery;
                    BuildLensFlarePasses(&graph, m_ghostFlareManager, rayQuery ? m_clodRayTracingSystem : nullptr,
                        rayQuery ? in.clodSlabs : nullptr, output.x, output.y, 2u);
                }
                break;
            case Bloom:
				BuildBloomPipeline(&graph);
                break;
            case Tonemapping:
                graph.BuildPass<TonemappingPass>(
                    "TonemappingPass",
                    recipe.Contains<br::pipeline::BloomTechnique>());
                graph.SetPassTechnique("TonemappingPass", "Post Process::Tonemapping");
                break;
            case DebugOutput: {
                const auto& debugSnapshots = in.debugSnapshots;
                const bool skeletons = SettingsManager::GetInstance().getSettingGetter<unsigned int>("outputType")() ==
                    static_cast<unsigned int>(OutputType::SKELETONS) && DeviceManager::GetInstance().GetMeshShadersSupported();
                if (skeletons) {
                    graph.BuildPass<DebugSkeletonPass>("DebugSkeletonPass", debugSnapshots);
                    graph.SetPassTechnique("DebugSkeletonPass", "Debug::Visualization");
                }
                else {
                    graph.BuildPass<DebugResolvePass>("DebugResolvePass");
                    graph.SetPassTechnique("DebugResolvePass", "Debug::Visualization");
                }
                if (in.drawBoundingSpheres) {
                    graph.BuildPass<DebugSpherePass>("DebugSpherePass", debugSnapshots);
                    graph.SetPassTechnique("DebugSpherePass", "Debug::Visualization");
                }
                break;
            }
            case DebugUi:
                graph.BuildPass<MenuRenderPass>("MenuRenderPass");
                graph.SetPassTechnique("MenuRenderPass", "Debug::UI");
                break;
            case DepthHistory:
                BuildLinearDepthHistoryCopyPass(&graph, &m_depthHistory);
                break;
            case StochasticDirectLighting:
                break; // Its passes come from a graph extension (RegisterPipelineExtensions).
            case Present:
                graph.BuildPass<PresentationReadyPass>("PresentationReadyPass");
                graph.SetPassTechnique("PresentationReadyPass", "Frame::Present");
                graph.SetPresentPass("PresentationReadyPass");
                break;
            }
        });

    {
    BT_ZONE_SCOPE("Renderer::DeclareFramePasses::BuildTechniques");
	for (const auto& entry : recipe.Techniques()) {
		entry.technique->Build(buildContext);
		probeGraphBuildPhase(("DeclareFramePasses after technique " + std::to_string(static_cast<uint32_t>(entry.id))).c_str());
	}
    }

    probeGraphBuildPhase("DeclareFramePasses before CompileStructural");
#if BASICRENDERER_HAS_INTEROP_VALIDATION
    if (DeviceManager::GetInstance().IsMultiRHIEnabled())
        br::validation::SARPInteropValidation::ApplyNamedPasses(graph, DeviceManager::GetInstance().GetPeerBackend());
#endif

    graph.SetStructuralMaterializeCheckpointCallback([](std::string_view passName) {
        if (!passName.starts_with("CLodShadow::")) {
            return;
        }

        std::string phase = "Frame graph build after structural pass ";
        phase += passName;
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    });

    graph.SetStructuralMaterializeResourceCheckpointCallback([](std::string_view passName, std::string_view resourceName) {
        if (passName != "CLodShadow::VirtualShadowSetupPass") {
            return;
        }

        std::string phase = "Frame graph build after structural resource ";
        phase += passName;
        phase += " :: ";
        phase += resourceName;
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    });

    //graph.SetMinimumAutomaticSchedulingQueues(QueueKind::Compute, 3);

}


void Renderer::LoadPipeline(HWND hwnd, UINT x_res, UINT y_res) {
    UINT dxgiFactoryFlags = 0;
	RECT clientRect{};
	if (hwnd && GetClientRect(hwnd, &clientRect)) {
		const UINT clientWidth = static_cast<UINT>((std::max)(clientRect.right - clientRect.left, 0L));
		const UINT clientHeight = static_cast<UINT>((std::max)(clientRect.bottom - clientRect.top, 0L));
		if (clientWidth != 0 && clientHeight != 0 && (clientWidth != x_res || clientHeight != y_res)) {
			spdlog::info(
				"Renderer: using actual client extent {}x{} for initial swapchain instead of requested {}x{}",
				clientWidth,
				clientHeight,
				x_res,
				y_res);
			x_res = clientWidth;
			y_res = clientHeight;
			SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("outputResolution")({ x_res, y_res });
		}
	}

    DeviceManager::GetInstance().Initialize();

	auto device = DeviceManager::GetInstance().GetDevice();

    auto result = device.CreateSwapchain(hwnd, x_res, y_res, rhi::Format::R8G8B8A8_UNorm, m_numFramesInFlight, m_allowTearing, m_swapChain);

    UpscalingManager::GetInstance().InitializeAdapter();

    // Create RTV descriptor heap
	rhi::DescriptorHeapDesc rtvHeapDesc = {};
    rtvHeapDesc.capacity = m_numFramesInFlight;
    rtvHeapDesc.type = rhi::DescriptorHeapType::RTV;
	rtvHeapDesc.shaderVisible = false;
	rtvHeapDesc.debugName = "RTV Descriptor Heap";
    result = device.CreateDescriptorHeap(rtvHeapDesc, rtvHeap);

    rtvDescriptorSize = device.GetDescriptorHandleIncrementSize(rhi::DescriptorHeapType::RTV);

    // Create frame resources
    renderTargets.resize(m_numFramesInFlight);
    for (UINT n = 0; n < m_numFramesInFlight; n++) {        
        renderTargets[n] = m_swapChain->Image(n);
    }

    // Wrap swapchain images for render-graph tracking
    m_backbufferResources.resize(m_numFramesInFlight);
    for (UINT n = 0; n < m_numFramesInFlight; n++) {
        m_backbufferResources[n] = std::make_shared<org::ExternalTextureResource>(
            renderTargets[n], x_res, y_res, rhi::Format::R8G8B8A8_UNorm);
        m_backbufferResources[n]->SetName("Backbuffer " + std::to_string(n));
    }
    m_dynamicBackbuffer = std::make_shared<org::DynamicResource>(m_backbufferResources[0]);
    m_dynamicBackbuffer->SetName("Backbuffer");

    CreateRTVs();
    m_swapChainReady = true;
    m_loggedSwapChainNotReady = false;

    // Create command allocator

	m_commandAllocators.resize(m_numFramesInFlight);
	m_commandLists.resize(m_numFramesInFlight);
    for (int i = 0; i < m_numFramesInFlight; i++) {
        rhi::CommandAllocatorPtr commandAllocator;
        rhi::CommandListPtr commandList;
        result = device.CreateCommandAllocator(rhi::QueueKind::Graphics, commandAllocator);
        result = device.CreateCommandList(rhi::QueueKind::Graphics, commandAllocator.Get(), commandList);
		m_commandAllocators[i] = std::move(commandAllocator);
		m_commandLists[i] = std::move(commandList);
        m_commandLists[i]->End();
    }

    // Create per-frame fence information
	m_frameFenceValues.resize(m_numFramesInFlight);
	for (int i = 0; i < m_numFramesInFlight; i++) {
		m_frameFenceValues[i] = 0;
	}

    m_frameIndex = static_cast<uint8_t>(m_swapChain->CurrentImageIndex());
    m_preparationFrameIndex = m_frameIndex;
    if (m_dynamicBackbuffer && m_frameIndex < m_backbufferResources.size()) {
        m_dynamicBackbuffer->SetResource(m_backbufferResources[m_frameIndex]);
    }

    result = device.CreateTimeline(m_frameFence);
    result = device.CreateTimeline(m_readbackFence);
    result = device.CreateTimeline(m_copyReadbackFence);
    result = device.CreateTimeline(m_legacyReadbackFence);
}


void Renderer::MakeFrameExtensions(br::render::FrameBuild& build) {
    using ExtensionPtr = std::unique_ptr<org::IRenderGraphExtension>;
    auto& in = build.inputs;
    auto& ids = in.extensionIds;
    auto& makers = in.extensionMakers;

    // The service persists; each graph gets its own extension over it.
    if (!m_frameIoService) {
        m_frameIoService = std::make_shared<br::render::RenderGraphIOService>(*m_pTextureFactory, m_orgRuntime->Uploads(), *m_pMaterialManager);
        m_frameIoService->SetAddsUploadPass(m_useGraphInstance);
    }
    ids.push_back("BuiltinIO");
    makers["BuiltinIO"] = [io = m_frameIoService]() -> ExtensionPtr { return std::make_unique<RenderGraphIOExtension>(io); };
    // Readback captures after a named pass are copy passes this extension places after their anchors. The capture sites
    // persist in m_readbackCaptures; each graph gets its own extension over them.
    ids.push_back("ReadbackCaptures");
    if (m_readbackCaptures) {
        makers["ReadbackCaptures"] = [captures = m_readbackCaptures]() -> ExtensionPtr { return captures->CreateExtension(); };
    }

    auto& persistent = *m_producerPersistentState;
    if (!persistent.clodStreaming) {
        persistent.clodStreaming = std::make_shared<CLodStreamingSystem>();
        persistent.clodStreaming->SetGeometryStorage(&m_pMeshManager->GetCLodGeometryStorage());
    }
    if (!persistent.virtualShadowInvalidations) persistent.virtualShadowInvalidations = std::make_shared<VirtualShadowInvalidationQueue>();
    // This build's casters, over the persistent queue: a caster extension joins it when it is made.
    build.outputs.virtualShadowCasters = std::make_shared<VirtualShadowCasterRegistry>(persistent.virtualShadowInvalidations);

    // Recipe extensions may register resources consumed by technique extensions (ProceduralWind visibility is consumed by CLod
    // shadow traversal). Register producers first; structural insertion constraints are resolved only after every extension
    // has been gathered, so this does not require their target passes to have been materialized yet.
    for (const auto& [id, factory] : m_pipelineRecipe.Extensions()) {
        ids.push_back(id);
        if (id == "SARPGrass" && br::runtime::renderer_settings::ReadTruthyEnvironmentFlag("BASICRENDERER_DISABLE_SARP_GRASS_EXTENSION")) {
            spdlog::info("Render-graph isolation: SARPGrass extension disabled");
            continue;
        }
        makers[id] = [factory, id]() -> ExtensionPtr {
            auto made = factory();
            if (!made) throw std::runtime_error("Pipeline extension factory returned null: " + id);
            // A virtual shadow caster (SARP grass) joins this build's caster set, which only this graph uses.
            if (auto* caster = dynamic_cast<IVirtualShadowCasterProvider*>(made.get())) {
                auto* current = br::render::CurrentFrameBuild();
                if (!current || !current->outputs.virtualShadowCasters) {
                    throw std::logic_error("Pipeline extension " + id + " is a shadow caster made outside a frame build");
                }
                current->outputs.virtualShadowCasters->Register(*caster);
            }
            return made;
        };
    }

    // The technique extensions, in a fixed order: a technique the recipe lacks has no maker.
    for (const char* id : { "CLodOpaque", "CLodRayTracing", "CLodAlpha", "CLodShadow", "StochasticLighting" }) ids.push_back(id);
    const auto streaming = persistent.clodStreaming;
    const auto slabs = m_pMeshManager ? m_pMeshManager->GetCLodSlabResourceGroup() : nullptr;
    br::pipeline::PipelineBuildContext extensionContext(
        m_pipelineRecipe.Bindings(),
        {},
        [&](br::pipeline::TechniqueId id, const br::pipeline::TechniqueOptions& optionsVariant) {
            CLodExtensionType extensionType{};
            const char* extensionId = nullptr;
            std::shared_ptr<const ProducerPersistentState::DirectionalVsmResources> directionalVsm;
            switch (id) {
            case br::pipeline::TechniqueId::ClusterLod:
                extensionType = CLodExtensionType::VisiblityBuffer;
                extensionId = "CLodOpaque";
                break;
            case br::pipeline::TechniqueId::ClusterLodAlpha:
                extensionType = CLodExtensionType::AlphaBlend;
                extensionId = "CLodAlpha";
                break;
            case br::pipeline::TechniqueId::ClusterLodShadow:
                // Stochastic lighting shadows every light by ray, the sun included, so no shadow map is built.
                if (in.stochasticLightingActive) {
                    return;
                }
                extensionType = CLodExtensionType::Shadow;
                extensionId = "CLodShadow";
                // The atlas is the renderer's, shared by every graph that binds it (R1: the largest VRAM item).
                CLodShadowVariant::EnsureDirectionalVsmAtlas(persistent);
                directionalVsm = std::make_shared<const ProducerPersistentState::DirectionalVsmResources>(persistent.directionalVsm);
                break;
            case br::pipeline::TechniqueId::StochasticDirectLighting:
                if (in.stochasticLightingActive) {
                    // Its options from the build's settings snapshot; it makes its resources in every build.
                    auto& settings = SettingsManager::GetInstance();
                    const br::render::StochasticLightingOptions stochasticOptions{
                        .reference = settings.getSettingGetter<bool>("stochasticLightingReference")(),
                        .accumulate = settings.getSettingGetter<bool>("stochasticLightingAccumulate")(),
                        .denoise = settings.getSettingGetter<bool>("stochasticLightingDenoise")(),
                        .downsample = settings.getSettingGetter<uint32_t>("stochasticLightingDownsample")() };
                    makers["StochasticLighting"] = [rayTracing = m_clodRayTracingSystem, slabs, stochasticOptions]() -> ExtensionPtr {
                        return std::make_unique<br::render::StochasticLightingExtension>(rayTracing, slabs, stochasticOptions);
                    };
                }
                return;
            default:
                return;
            }

            const auto& options = std::get<br::pipeline::ClusterLodOptions>(optionsVariant);
            const bool voxelRasterizationEnabled =
                m_pipelineRecipe.Contains<br::pipeline::ClusterLodVoxelTechnique>();
            const uint32_t voxelRasterWorkCapacity = voxelRasterizationEnabled
                ? m_pipelineRecipe.Options<br::pipeline::ClusterLodVoxelTechnique>().workRecordCapacity
                : 0u;
            const uint32_t maxClusters = std::clamp(
                SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodVisibleClusterCapacitySettingName)(),
                CLodMinVisibleClusterCapacity,
                CLodMaxVisibleClusterCapacity);

            // PrepareFrameRayTracing made the BVH if anything traces it.
            const bool pathTraced = m_pipelineRecipe.Contains<br::pipeline::PathTracingTechnique>();
            const auto rayTracingSystem = in.rayTracingReady ? m_clodRayTracingSystem : nullptr;
            // One culling for raster and the BVH (docs/clod_raytracing.md, "Unified culling"), where raster takes the
            // triangle branch with the pure compute backend and nothing replaces raster. SARP_CLOD_UNIFIED_CULLING=0 keeps
            // the separate ray tracing view.
            const bool unifiedCulling = [&] {
                if (!rayTracingSystem || id != br::pipeline::TechniqueId::ClusterLod || pathTraced || m_rayTracedPrimaryVisibilityMode != 0u) return false;
                const uint32_t forced = GetClusterLODForcedViewBranch();
                if (forced == 2u || (forced == 0u && voxelRasterizationEnabled)) return false;
                if (SettingsManager::GetInstance().getSettingGetter<CLodCullingBackend>(CLodCullingBackendSettingName)() != CLodCullingBackend::PureCompute) return false;
                const char* env = std::getenv("SARP_CLOD_UNIFIED_CULLING");
                return !(env && env[0] == '0');
            }();
            // Applied to the streaming service when a graph built with this extension is attached (PublishFrameBuild).
            uint32_t streamingActiveBranches = 0u;
            if (id == br::pipeline::TechniqueId::ClusterLod) {
                // Forked DAGs: raster views with voxel output take the voxel branch, every other view the triangle branch.
                const uint32_t forcedBranch = GetClusterLODForcedViewBranch();
                const bool rasterVoxels = forcedBranch == 2u || (forcedBranch == 0u && voxelRasterizationEnabled);
                streamingActiveBranches = rasterVoxels ? CLOD_BRANCH_VOXEL : CLOD_BRANCH_TRIANGLE;
                if (rayTracingSystem) streamingActiveBranches |= CLOD_BRANCH_TRIANGLE;
            }

            const CLodExtensionOptions variantOptions{
                .enableReyes = options.reyes == br::pipeline::ReyesMode::Enabled,
                .enableVoxelRasterization = voxelRasterizationEnabled,
                .voxelRasterWorkCapacity = voxelRasterWorkCapacity,
                .streamingSystem = streaming,
                .streamingActiveBranches = streamingActiveBranches,
                .virtualShadowCasters = build.outputs.virtualShadowCasters,
                .persistentState = m_producerPersistentState,
                .directionalVsm = directionalVsm,
                .slabResourceGroup = slabs,
                .rayTracingSystem = id == br::pipeline::TechniqueId::ClusterLod ? rayTracingSystem : nullptr,
                .rayTracedPrimaryVisibility = !pathTraced && m_rayTracedPrimaryVisibilityMode == 1u,
                .pathTraced = pathTraced,
                .rayTracedShadows = unifiedCulling && m_rayTracedShadows && !in.stochasticLightingActive,
                .unifiedRayTracingCulling = unifiedCulling };
            makers[extensionId] = [extensionType, maxClusters, variantOptions]() -> ExtensionPtr {
                return std::make_unique<CLodExtension>(extensionType, maxClusters, variantOptions);
            };

            if (id == br::pipeline::TechniqueId::ClusterLod && rayTracingSystem && !unifiedCulling) {
                const uint32_t rayTracingClusters = rayTracingSystem->GetLimits().maxClustersInBvh;
                const CLodExtensionOptions rayTracingOptions{
                    .enableReyes = false,
                    .enableVoxelRasterization = false,
                    .voxelRasterWorkCapacity = 0u,
                    .streamingSystem = streaming,
                    .virtualShadowCasters = build.outputs.virtualShadowCasters,
                    .persistentState = m_producerPersistentState,
                    .slabResourceGroup = slabs,
                    .rayTracingSystem = rayTracingSystem,
                    .rayTracedPrimaryVisibility = !pathTraced && m_rayTracedPrimaryVisibilityMode == 1u,
                    .pathTraced = pathTraced,
                    .scheduleRayTracedVisibility = !pathTraced && m_rayTracedPrimaryVisibilityMode != 0u,
                    .rayTracedVisibilityMode = m_rayTracedPrimaryVisibilityMode == 2u ? 1u : 0u,
                    .rayTracedShadows = !pathTraced && m_rayTracedShadows && !in.stochasticLightingActive };
                makers["CLodRayTracing"] = [rayTracingClusters, rayTracingOptions]() -> ExtensionPtr {
                    return std::make_unique<CLodExtension>(CLodExtensionType::RayTracing, rayTracingClusters, rayTracingOptions);
                };
            }
        });
    for (const auto& entry : m_pipelineRecipe.Techniques()) {
        entry.technique->RegisterExtensions(extensionContext);
    }
}

void Renderer::RegisterFrameHostExtensions(const std::vector<std::string>& extensionIds) {
    if (!m_frameHost) throw std::logic_error("RegisterFrameHostExtensions: no frame graph host");
    // Build order: the renderer's providers, then the pipeline extensions (their PrepareForBuild reads the providers and
    // registers what the renderer's passes read), then the renderer's passes.
    std::vector<std::string> ids;
    ids.reserve(extensionIds.size() + 2u);
    ids.push_back("BasicRenderer::Providers");
    ids.insert(ids.end(), extensionIds.begin(), extensionIds.end());
    ids.push_back("BasicRenderer::Passes");
    if (ids == m_frameHostExtensionIds) return;

    for (const auto& id : m_frameHostExtensionIds) m_frameHost->RemoveExtension(id);
    m_frameHostExtensionIds.clear();
    const auto add = [this](std::string id, org::GraphHost::ExtensionFactory factory) {
        // The frame epoch only: the upload epoch's graph is the host's upload pass alone.
        m_frameHost->AddExtension(id, std::move(factory), {kFrameEpoch});
        m_frameHostExtensionIds.push_back(std::move(id));
    };
    add("BasicRenderer::Providers", [this] {
        // The extension holds the build's provider copies: its graph keeps pointers to them.
        std::vector<std::shared_ptr<org::IResourceProvider>> providers;
        if (auto* build = br::render::CurrentFrameBuild()) providers = build->inputs.providers;
        return std::make_unique<br::render::DeclarationGraphExtension>([this, providers](org::RenderGraph& graph) {
            (void)providers;
            DeclareFrameProviders(graph);
        });
    });
    for (const auto& id : extensionIds) {
        // Each graph build calls this on the build's thread: it makes what the build's maker for the id makes, or nothing.
        add(id, [id]() -> std::unique_ptr<org::IRenderGraphExtension> {
            auto* build = br::render::CurrentFrameBuild();
            if (!build) throw std::logic_error("Frame graph extension " + id + ": made outside a frame build");
            const auto maker = build->inputs.extensionMakers.find(id);
            if (maker == build->inputs.extensionMakers.end() || !maker->second) return nullptr;
            return maker->second();
        });
    }
    add("BasicRenderer::Passes", [this] {
        return std::make_unique<br::render::DeclarationGraphExtension>([this](org::RenderGraph& graph) { DeclareFramePasses(graph); });
    });
}
