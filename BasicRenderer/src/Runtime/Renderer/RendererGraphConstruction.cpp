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
#include <bit>
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
#include "Render/RenderGraph/FramePlacement.h"
#include "Runtime/Resources/ResourceManager.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Extensions/RenderContext.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Runtime/Frame/RendererFrameInputs.h"
#include "../shaders/PerPassRootConstants/lightCullingRootConstants.h"
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
// "1,3,4": a build's host epochs, for logs.
std::string DescribeHostEpochs(const std::vector<uint32_t>& hosts) {
    std::string text;
    for (const auto host : hosts) text += (text.empty() ? "" : ",") + std::to_string(host);
    return text.empty() ? std::string("none") : text;
}
}

std::vector<uint32_t> Renderer::FrameGraphHostEpochs() const {
    return m_useGraphInstance ? std::vector<uint32_t>{ br::render::kFrameHostEpoch } : br::render::FrameHostEpochs();
}

org::RenderGraph* Renderer::FrameGraph(uint32_t hostEpoch) {
    if (m_useGraphInstance) return currentRenderGraph;
    return m_frameHost ? m_frameHost->Graph(hostEpoch) : nullptr;
}

org::RenderGraph* Renderer::PresentationGraph() {
    // The last frame host epoch's graph: it has PresentationReadyPass (Overlay's) and submits last.
    const auto hosts = FrameGraphHostEpochs();
    return FrameGraph(hosts.back());
}

// Which host epochs a build of these requests makes graphs for (M4 step 5). A request names host epochs (HostEpochBit; all for
// FrameRevisions::kAllEpochs); an epoch with no graph yet is always built. An epoch that received forwarded passes from the
// Visibility host epoch's graph (its extensions' later-epoch passes, ForwardedPasses) is built together with it, both ways.
// A build of several host epochs is attached as one staged set (FinishFrameBuild), so every epoch a change needs is in it.
std::vector<uint32_t> Renderer::SelectBuildHostEpochs(uint32_t mask, bool shadowVariant) {
    const auto hosts = FrameGraphHostEpochs();
    if (m_useGraphInstance) return hosts;
    uint32_t wanted = 0;
    for (const auto host : hosts)
        if ((mask & br::render::HostEpochBit(host)) || !m_frameHost->Graph(host)) wanted |= br::render::HostEpochBit(host);
    const uint32_t visibility = br::render::HostEpochBit(br::render::kFrameHostEpoch);
    if (wanted & visibility) wanted |= m_frameForwardMask;
    if (wanted & m_frameForwardMask) wanted |= visibility;
    // A Visibility build with the CLod shadow variant, or replacing a binding that had it, makes new CLod streaming buffers
    // (CLodExtension::BuildStreamingBinding), which Shading's graph (the streaming tail, the alpha variant) must be built with.
    // Both come from the build's own inputs (the makers MakeFrameExtensions made), so the two graphs go in one set.
    const uint32_t shading = br::render::HostEpochBit(br::render::HostEpochOf(br::render::kShadingEpoch));
    if ((wanted & visibility) && shading != visibility) {
        const bool attachedShadow = m_frameInterface && m_frameInterface->clodStreaming && m_frameInterface->clodStreaming->HasVirtualShadowResources();
        if (shadowVariant || attachedShadow) wanted |= shading;
    }
    std::vector<uint32_t> selected;
    for (const auto host : hosts)
        if (wanted & br::render::HostEpochBit(host)) selected.push_back(host);
    return selected;
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
    // The resolution revision (M5 step 4): window sizes and upscaler settings recorded since the last consumption, taken once a
    // frame here and only with no build in flight. It requests the build that answers it, started below.
    ApplyResolutionRequest();
    if (!currentRenderGraph && !m_frameRevisions.TopologyPending() && !FrameBuildInFlight()) RequestGraphRebuild("first build");
    // A graph built in place is checked once it has a schedule. It runs already, so a failure is only reported.
    if (m_frameEpochCheckDeferred && currentRenderGraph && m_frameBuild) {
        bool checked = true;
        for (const auto host : br::render::FrameHostEpochs()) {
            auto* graph = FrameGraph(host);
            const auto found = m_epochBuilds.find(host);
            const auto& build = found != m_epochBuilds.end() ? found->second : m_frameBuild;
            if (!graph || !build) continue;
            if (const auto failure = CheckFrameEpochInterface(*graph, *build, m_frameBuildSerialAttached, m_useGraphInstance ? br::render::kNoFrameEpoch : host)) {
                if (!failure->empty()) spdlog::error("Renderer: the attached frame graph of host epoch {}: {}", host, *failure);
            } else {
                checked = false;
            }
        }
        if (checked) {
            m_frameEpochCheckDeferred = false;
            CheckFrameInterfaceAcrossGraphs(m_frameBuildSerialAttached);
        }
    }
    // With frame placement, a graph whose placements are in a pool generation another graph grew is built again. A graph placed
    // from an older frame placement takes the current one in place, at its next preparation (ORG), so it is not rebuilt.
    if (m_framePlacement && currentRenderGraph && !m_frameRevisions.TopologyPending() && !FrameBuildInFlight()) {
        if (const auto stale = m_frameHost->StaleAliasPlacementEpochs(); !stale.empty()) {
            std::string epochs;
            uint32_t mask = 0;
            for (const auto epoch : stale) {
                epochs += (epochs.empty() ? "" : ",") + std::to_string(epoch);
                mask |= br::render::HostEpochBit(epoch);
            }
            RequestGraphRebuild("stale alias placements in epoch(s) " + epochs, mask);
        }
    }
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
    for (auto& outputs : job->build->outputs) outputs.resourceEntityWrites.clear();
    job->graphs.clear();
    ++m_graphBuildStats.discarded;
    // Its requests are asked again below; a resolution build's are answered by the next build.
    if (job->resolution) {
        if (m_resolutionStats.resolutionBuildsInFlight) --m_resolutionStats.resolutionBuildsInFlight;
        m_resolutionBuildRequested = true;
    }
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
        StallPipeline("a graph rebuilt in place (BASICRENDERER_FRAME_GRAPH_INSTANCE)");
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
    if (m_resolutionBuildRequested) {
        m_resolutionBuildRequested = false;
        job->resolution = true;
        auto& resolution = m_resolutionStats;
        ++resolution.resolutionBuilds;
        ++resolution.resolutionBuildsInFlight;
        resolution.maxResolutionBuildsInFlight = (std::max)(resolution.maxResolutionBuildsInFlight, resolution.resolutionBuildsInFlight);
    }
    auto& frameBuild = *job->build;
    frameBuild.inputs.knownPassEpochs = m_knownPassEpochs;
    frameBuild.passEpochs->SetKnown(m_knownPassEpochs);
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
        // The sizes come from the upscaler configuration the build captures (M5 step 4), as CreateTextures' do: a build never mixes
        // two sizes, whatever the live settings say (a pipe write of renderResolution, a request recorded since).
        {
            const auto resolution = BuildResolution();
            frameBuild.inputs.renderResolution = resolution.render;
            frameBuild.inputs.outputResolution = resolution.output;
            SettingsManager::GetInstance().OverrideInSnapshot(*settings, "renderResolution", resolution.render);
            SettingsManager::GetInstance().OverrideInSnapshot(*settings, "outputResolution", resolution.output);
        }
        SettingsSnapshotScope settingsScope(settings);
        MakeFrameBuild(frameBuild, primaryCameraEntity);
        frameBuild.inputs.settings = std::move(settings);
        // The host's factories are fixed per id; only a change of the recipe's extension ids changes them. Owner thread, and
        // never while a build runs: a build takes the host's extension list when it starts.
        if (!m_useGraphInstance) RegisterFrameHostExtensions(frameBuild.inputs.extensionIds, frameBuild.inputs.extensionEpochs);
    }
    // The host epochs to build, once the inputs say which extensions the build makes (the CLod shadow variant). Nothing above
    // reads them: the declarations that do run in the build.
    frameBuild.SetHostEpochs(SelectBuildHostEpochs(job->requests.topologyEpochs, frameBuild.inputs.extensionMakers.contains("CLodShadow")));
    if (auto& writes = br::settings_detail::t_writesUnderSnapshot; !writes.empty()) {
        std::sort(writes.begin(), writes.end());
        writes.erase(std::unique(writes.begin(), writes.end()), writes.end());
        std::string names;
        for (const auto& name : writes) names += (names.empty() ? "" : ", ") + name;
        spdlog::warn("Renderer: the frame build's inputs wrote settings (its snapshot does not see these): {}", names);
        writes.clear();
    }
    for (const auto host : frameBuild.hostEpochs) job->hostRequestsAtStart.push_back(m_useGraphInstance ? 0 : m_frameHost->RebuildRequests(host));
    // Published-state resolvers capture against a manifest lease, as each frame's preparation does (RendererFrameUpdate).
    job->captureContext = std::make_shared<const org::ResolverCaptureContext>(m_context.publishedManifestLease,
        m_context.publishedRendererState ? m_context.publishedRendererState->bindingBundle : nullptr);
    // Staged recordings (M6 step 6): the frame the kept epochs' copies are recorded for, the build's update values with the last
    // frame's render values. BASICRENDERER_STAGED_RECORDINGS=0 records none ahead.
    static const bool stagedRecordings = [] {
        const char* value = std::getenv("BASICRENDERER_STAGED_RECORDINGS");
        return !(value && value[0] == '0');
    }();
    const auto& lastUpdate = frameBuild.inputs.lastUpdate;
    if (stagedRecordings && !m_useGraphInstance && lastUpdate && m_lastFrameRender && lastUpdate->frameNumber == m_lastFrameRender->frameNumber
        && lastUpdate->frameSlot == m_lastFrameRender->frameSlot) {
        // A build for new sizes (a resize, an upscaler change): its first frames have the build's render and output sizes, and the
        // light grid of that render size.
        auto update = std::make_shared<UpdateContext>(*lastUpdate);
        auto render = std::make_shared<RenderContext>(*m_lastFrameRender);
        // The views as the first frame after the attach has them (MakeBuildUpdateContext: a resize's attach gives the primary view
        // the build's depth maps, visibility buffer and sizes), in both contexts: passes take their values from either.
        render->viewFamily = update->viewFamily;
        if (frameBuild.inputs.camera) {
            const auto& built = frameBuild.inputs.camera->info;
            for (auto* camera : { &update->primaryCamera, &render->primaryCamera }) {
                auto& info = camera->info;
                info.numDepthMips = built.numDepthMips;
                info.depthResX = built.depthResX;
                info.depthResY = built.depthResY;
                info.lodResY = built.lodResY;
                info.uvScaleToNextPowerOfTwo = built.uvScaleToNextPowerOfTwo;
            }
        }
        if (frameBuild.inputs.depth.linearDepthMap) render->primaryDepthMap = frameBuild.inputs.depth;
        if (const auto& sizes = frameBuild.inputs.resolution; sizes && sizes->render.x && sizes->render.y) {
            update->renderResolution = render->renderResolution = sizes->render;
            update->outputResolution = render->outputResolution = sizes->output;
            auto& settings = SettingsManager::GetInstance();
            const LightGridShape grid = MakeLightGridShape(sizes->render.x, sizes->render.y,
                settings.getSettingGetter<uint32_t>(LightGridTileSizeSettingName)(), settings.getSettingGetter<uint32_t>(LightGridSlicesSettingName)());
            update->lightClusterSize = render->lightClusterSize = { grid.tilesX, grid.tilesY, grid.slices };
        }
        // Runs after the first: a view whose depth map is new (a resize's) has its history from the second run on, as the frames'
        // own selections give it (DepthHistoryPublicationService::Select). Only whether there is one is read ahead (the depth tables).
        auto laterUpdate = std::make_shared<UpdateContext>(*update);
        auto laterRender = std::make_shared<RenderContext>(*render);
        if (update->viewFamily) {
            auto family = std::make_shared<br::render::PreparedViewFamilyState>(*update->viewFamily);
            for (auto& view : family->views)
                if (view.linearDepthMap && !view.depthHistory)
                    view.depthHistory = { view.linearDepthMap, view.linearDepthMap->GetBackingGeneration(), 0, (std::max<uint64_t>)(update->frameNumber, 1), nullptr };
            laterUpdate->viewFamily = laterRender->viewFamily = std::move(family);
        }
        update->stagedRun = 0;
        laterUpdate->stagedRun = 1;
        job->stagedLaterFrame = std::make_shared<const br::render::RendererFrameInputs>(std::move(laterUpdate), std::move(laterRender),
            br::render::PrimaryCameraFrameUpload{});
        job->stagedFrame = std::make_shared<const br::render::RendererFrameInputs>(std::move(update), std::move(render), br::render::PrimaryCameraFrameUpload{});
        job->stagedDeltaTime = lastUpdate->deltaTime;
        if (const auto& objects = frameBuild.inputs.resolution) {
            const auto swapchain = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
            job->drainsAtAttach = objects->recreateSwapchain || objects->output.x != swapchain.x || objects->output.y != swapchain.y;
            job->swapsPrimaryDepth = objects->primaryDepth.has_value();
        }
    }
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
                if (auto extension = maker->second()) {
                    currentRenderGraph->RegisterExtension(std::make_unique<br::render::EpochTaggedExtension>(std::move(extension),
                        frameBuild.inputs.ExtensionEpoch(id), frameBuild.passEpochs), id);
                }
            }
            currentRenderGraph->RegisterExtension(std::make_unique<br::render::DeclarationGraphExtension>(
                [this](org::RenderGraph& graph) { DeclareFramePasses(graph); }), "BasicRenderer::Passes");
            org::runtime::ScopedActiveGraphServices activeGraphServices(currentRenderGraph->GetUploadService(), currentRenderGraph->GetDescriptorService());
            m_frameInstance->Rebuild([](org::RenderGraph& graph) { graph.PrepareExtensionsForBuild(); });
        } else if (job.onRenderThread && !currentRenderGraph) {
            // The first build, on the render thread: the host builds its graphs in place (its own build point). Nothing ran yet,
            // so there is no set to keep whole.
            spdlog::info("Renderer: frame graph build {} on the render thread (host epochs {}); why: {}", job.serial,
                DescribeHostEpochs(frameBuild.hostEpochs), job.reason);
            RendererECSManager::ScopedBuildWorldAccessCheck worldAccessCheck;
            const auto failuresBefore = m_graphBuildStats.epochFailures;
            for (const auto host : frameBuild.hostEpochs) m_frameHost->RequestRebuild(host, job.reason);
            m_frameHost->BuildIfRequested();
            m_frameHost->PollEpochFailures();
            if (m_graphBuildStats.epochFailures != failuresBefore) job.error = m_graphBuildStats.lastEpochFailure;
            built = job.error.empty();
            job.installed = built;
        } else {
            // New graphs, detached, one per host epoch of the build, in execution order: an earlier epoch's graph forwards its
            // extensions' later-epoch passes to the later graph's build (ForwardedPasses). Each graph's extensions' PrepareForBuild
            // run the renderer's declarations (DeclareFrameProviders, DeclareFramePasses) for its own epochs, then the structural
            // build, Setup and the first schedule. The attached graphs are not touched.
            // A build after a GPU idle wait (resize, upscaler restart, path tracing on or off) runs this on the render thread,
            // before any frame: it is attached as one staged set like any other (M4 step 8), not built in place epoch by epoch.
            spdlog::info("Renderer: frame graph build {} on the {} thread (host epochs {}); why: {}", job.serial,
                job.onRenderThread ? "render" : "build", DescribeHostEpochs(frameBuild.hostEpochs), job.reason);
            RendererECSManager::ScopedBuildWorldAccessCheck worldAccessCheck;
            for (const auto host : frameBuild.hostEpochs) {
                auto graph = m_frameHost->BuildEpochGraph(host, job.reason, job.captureContext);
                const bool ok = graph && graph->Built();
                job.graphs.push_back({ host, std::move(graph) });
                if (!ok) {
                    built = false;
                    break;
                }
            }
        }
        if (built) {
            // Every pass has a frame epoch, and what crosses epochs is logged (docs/frame_graph_composition_plan.md, M4).
            const std::vector<uint32_t> hosts = m_useGraphInstance ? std::vector<uint32_t>{ br::render::kFrameHostEpoch } : frameBuild.hostEpochs;
            for (const auto host : hosts) {
                org::RenderGraph* graph = m_useGraphInstance ? currentRenderGraph : job.installed ? m_frameHost->Graph(host) : nullptr;
                if (!m_useGraphInstance && !job.installed)
                    for (auto& entry : job.graphs) if (entry.hostEpoch == host && entry.graph) graph = entry.graph->Graph();
                if (!graph) continue;
                if (auto failure = CheckFrameEpochInterface(*graph, *job.build, job.serial, m_useGraphInstance ? br::render::kNoFrameEpoch : host)) {
                    if (!failure->empty()) {
                        job.error = std::move(*failure);
                        built = false;
                        break;
                    }
                } else {
                    // Built in place: its schedule is selected at its first preparation, so it is checked after its first frame.
                    job.epochCheckDeferred = true;
                }
            }
        }
        // The graphs' transients are placed here, on the build's thread, from one staging of the host's frame placement (ORG
        // GraphHost::PlaceEpochGraphs): the attach then only swaps them in. A graph whose placement would move a resource the
        // running graphs bind (a new version that moves a shared one) places at its attach, as before; its version's heaps are
        // allocated here all the same. Before the pipeline wait, so it overlaps the compiles.
        if (built && !m_useGraphInstance && !job.installed && !job.graphs.empty()) {
            BT_ZONE_SCOPE("Renderer::RunFrameBuild::PlaceTransients");
            std::vector<org::GraphHost::EpochGraph*> graphs;
            for (auto& entry : job.graphs) if (entry.graph) graphs.push_back(entry.graph.get());
            const auto placeBegin = std::chrono::steady_clock::now();
            job.graphsPlacedAtBuild = m_frameHost->PlaceEpochGraphs(graphs);
            job.placementMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - placeBegin).count();
            for (auto& entry : job.graphs) {
                if (entry.graph && !entry.graph->Built()) {
                    job.error = "host epoch " + std::to_string(entry.hostEpoch) + "'s graph: " + entry.graph->Error();
                    built = false;
                }
            }
        }
        job.pipelinesDeclared = declarations.Declared();
        if (built) {
            BT_ZONE_SCOPE("Renderer::RunFrameBuild::AwaitPipelines");
            const auto waitBegin = std::chrono::steady_clock::now();
            job.pipelineFailures = declarations.Await();
            job.pipelineWaitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitBegin).count();
        }
        // Staged recordings (M6 step 6, ORG GraphHost::RecordStagedCopies): each kept epoch's copies are recorded here, on the
        // build's thread, once its pipelines are in, so the first frames after the attach submit kept recordings instead of
        // recording on the render thread. The host records nothing for an epoch whose passes do not all report revisions
        // (Visibility, until it converts) and says why. Each copy's first run confirms its recording against its own frame data.
        if (built && job.stagedFrame && !m_useGraphInstance && !job.installed && job.pipelineFailures.empty()) {
            BT_ZONE_SCOPE("Renderer::RunFrameBuild::StagedRecordings");
            // By run (M7 step 2) when the attach resizes the swapchain: the first run's frame, then the later runs', each copy
            // recorded for the run that takes it (the drain frees every copy, so the new graph's runs take them in order). Every
            // copy (4 with three frames in flight), except Visibility's two: they cover the attach frame and the next, and the
            // later copies record at their first runs (BASICRENDERER_STAGED_RUNS=N stages N runs of every epoch, by run, always).
            static const uint32_t stagedRunsOverride = [] {
                const char* value = std::getenv("BASICRENDERER_STAGED_RUNS");
                return value && value[0] ? static_cast<uint32_t>(std::max(1, std::atoi(value))) : 0u;
            }();
            constexpr uint32_t kEveryCopy = 8u;
            std::vector<org::UpdateExecutionContext> runs(kEveryCopy);
            for (uint32_t run = 0; run < kEveryCopy; ++run) {
                auto& frame = runs[run];
                const auto& host = run == 0 || !job.stagedLaterFrame ? job.stagedFrame : job.stagedLaterFrame;
                frame.resolverCaptureContext = job.captureContext;
                frame.hostData = host.get();
                frame.ownedHostData = host;
                frame.deltaTime = job.stagedDeltaTime;
            }
            for (auto& entry : job.graphs) {
                if (!entry.graph || !entry.graph->Built()) continue;
                if (job.swapsPrimaryDepth && entry.hostEpoch == br::render::HostEpochOf(br::render::kPostEpoch)
                    && entry.hostEpoch != br::render::HostEpochOf(br::render::kVisibilityEpoch)) {
                    org::GraphHost::StagedRecordings skipped;
                    skipped.why = "not recorded: the attach changes the depth map group its depth history pass declares";
                    job.staged.push_back({ entry.hostEpoch, skipped });
                    continue;
                }
                // Without a swapchain resize the copies the new graph's first runs take are still in flight at its attach, so its
                // runs take them in turn from wherever the turn is (GraphHost::TakeSyncCopy): every copy is recorded for the first
                // run, the one that matters most (the attach frame), as before.
                if (!job.drainsAtAttach && !stagedRunsOverride) {
                    job.staged.push_back({ entry.hostEpoch, m_frameHost->RecordStagedCopies(*entry.graph, runs.front()) });
                    continue;
                }
                uint32_t count = kEveryCopy;
                if (stagedRunsOverride) count = stagedRunsOverride;
                else if (entry.hostEpoch == br::render::HostEpochOf(br::render::kVisibilityEpoch)) count = 2u;
                job.staged.push_back({ entry.hostEpoch, m_frameHost->RecordStagedCopies(*entry.graph,
                    std::span<const org::UpdateExecutionContext>(runs.data(), (std::min<size_t>)(count, runs.size()))) });
            }
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
    if (job->resolution && m_resolutionStats.resolutionBuildsInFlight) --m_resolutionStats.resolutionBuildsInFlight;
    // Read only while the build ran; it holds the last frame's published state.
    frameBuild.inputs.lastUpdate.reset();
    if (!job->settingsWritten.empty()) {
        std::string names;
        for (const auto& name : job->settingsWritten) names += (names.empty() ? "" : ", ") + name;
        spdlog::warn("Renderer: frame graph build {} wrote settings (its snapshot does not see these): {}", job->serial, names);
    }
    std::string failure = job->error;
    if (failure.empty() && !m_useGraphInstance && !job->installed) {
        if (job->graphs.size() != frameBuild.hostEpochs.size()) failure = "no graph was made for every host epoch";
        for (auto& entry : job->graphs) {
            if (!(entry.graph && entry.graph->Built())) {
                failure = "host epoch " + std::to_string(entry.hostEpoch) + "'s graph: " + (entry.graph ? entry.graph->Error() : std::string("no graph was made"));
                break;
            }
        }
    }
    if (failure.empty() && !job->pipelineFailures.empty()) {
        failure = std::to_string(job->pipelineFailures.size()) + " pipeline(s) failed to build";
        for (const auto& pipelineFailure : job->pipelineFailures) failure += "; " + pipelineFailure;
    }
    bool askAgain = false;
    if (failure.empty() && !m_useGraphInstance && !job->installed && !m_frameHost->Graph(kUploadEpoch)) {
        // The first build: the host's upload epoch (its upload pass alone) is built and attached first. The host points the
        // upload service at the registry of the graph it prepares (GraphHost::ExecuteFrame), and the frame's after this one.
        auto uploads = m_frameHost->BuildEpochGraph(kUploadEpoch, "first build");
        const auto attached = uploads && uploads->Built() ? m_frameHost->AttachEpochGraph(uploads) : org::GraphHost::AttachResult{};
        if (!attached.attached)
            failure = "the upload epoch's graph: " + (uploads && !uploads->Built() ? uploads->Error() : attached.reason);
    }
    // The host epochs this build's graphs run in from now on.
    std::vector<uint32_t> attachedHosts;
    uint32_t refusedMask = 0;
    if (failure.empty() && !m_useGraphInstance && !job->installed) {
        // The build's graphs are one staged set (M4 step 7): each is staged (GraphHost::StageEpochGraph checks it and runs its
        // shared setup), then all are swapped in by one AttachStagedEpochGraphs at this consolidation point. A refusal of any
        // drops the whole set, so no frame runs some epochs of this build with others of an older one. Each old graph stops and
        // is released once its submitted work completes (no device wait). An epoch with no graph yet is attached at once by its
        // staging: it ran nothing, so nothing old mixes with it.
        std::vector<uint32_t> staged;
        const auto refuse = [&](uint32_t host, const org::GraphHost::AttachRefusal refusal, const std::string& reason) {
            const std::string why = std::string("the host refused host epoch ") + std::to_string(host) + "'s graph ("
                + org::GraphHost::AttachRefusalName(refusal) + "): " + reason;
            // The host changed under the build: a new build sees the change.
            const bool hostChanged = refusal == org::GraphHost::AttachRefusal::HostConfiguration
                || refusal == org::GraphHost::AttachRefusal::EpochConfiguration
                || refusal == org::GraphHost::AttachRefusal::NewerAttached;
            if (attachedHosts.empty()) {
                failure = why;
                askAgain = hostChanged;
                return;
            }
            // Only epochs that had no graph were attached (at their staging). They keep the new graphs. The refused set's
            // epochs keep theirs and are built again at once.
            spdlog::error("Renderer: frame graph build {}: {}; the epochs that had no graph keep the new ones", job->serial, why);
            for (const auto staging : staged) refusedMask |= br::render::HostEpochBit(staging);
            refusedMask |= br::render::HostEpochBit(host);
            ++m_graphBuildStats.failed;
            m_graphBuildStats.lastError = why;
        };
        bool refused = false;
        for (auto& entry : job->graphs) {
            const auto result = m_frameHost->StageEpochGraph(entry.graph);
            if (result.staged) {
                staged.push_back(entry.hostEpoch);
                continue;
            }
            if (result.attached) {
                attachedHosts.push_back(entry.hostEpoch);
                continue;
            }
            refuse(entry.hostEpoch, result.refusal, result.reason);
            refused = true;
            break;
        }
        if (!refused) {
            const auto set = m_frameHost->AttachStagedEpochGraphs(staged);
            if (set.attached) {
                attachedHosts.insert(attachedHosts.end(), staged.begin(), staged.end());
                if (staged.size() > 1) ++m_graphBuildStats.stagedSets;
            } else {
                refuse(set.epoch, set.refusal, set.reason);
                refused = true;
            }
        }
        if (refused) {
            const auto dropped = m_frameHost->DropStagedEpochGraphs(staged);
            ++m_graphBuildStats.refusedSets;
            spdlog::warn("Renderer: frame graph build {}: its staged set of host epochs {} was refused; {} staged graph(s) dropped, none attached",
                job->serial, DescribeHostEpochs(staged), dropped);
        }
        // In the build's host epoch order.
        std::vector<uint32_t> ordered;
        for (const auto host : frameBuild.hostEpochs)
            if (std::find(attachedHosts.begin(), attachedHosts.end(), host) != attachedHosts.end()) ordered.push_back(host);
        attachedHosts = std::move(ordered);
    } else if (failure.empty()) {
        attachedHosts = m_useGraphInstance ? std::vector<uint32_t>{ br::render::kFrameHostEpoch } : frameBuild.hostEpochs;
    }
    const auto latencyMs = std::chrono::duration<double, std::milli>(begin - job->started).count();
    if (!failure.empty()) {
        // Nothing of a failed build is published: its extension instances go with its graphs, and the attached graphs keep
        // their directory entries, streaming binding and casters.
        for (auto& outputs : frameBuild.outputs) outputs.resourceEntityWrites.clear();
        job->graphs.clear();
        ++m_graphBuildStats.failed;
        m_graphBuildStats.lastError = failure;
        // A graph the host's own build point installed is in place already: it cannot be kept from running.
        const bool keep = currentRenderGraph && !m_useGraphInstance && !job->installed;
        spdlog::error("Renderer: frame graph build {} failed ({}): {}; why it was built: {}", job->serial,
            keep ? "the attached graphs keep rendering" : "there is no graph to keep", failure, job->reason);
        if (job->installed) {
            // Its pipelines failed after the host installed it: the rollback's build follows at once, on this thread.
            currentRenderGraph = m_frameHost->Graph(br::render::kFrameHostEpoch);
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
        currentRenderGraph = m_frameHost->Graph(br::render::kFrameHostEpoch);
        if (!currentRenderGraph) throw std::runtime_error("the frame graph host has no graph after its attach");
    }
    m_frameBuild = job->build;
    m_frameBuildSerialAttached = job->serial;
    if (!m_useGraphInstance) RecordAttachedBuildSet(job->serial, attachedHosts);
    m_frameEpochCheckDeferred = job->epochCheckDeferred;
    m_graphBuildSettings = frameBuild.inputs.settings;
    for (const auto host : attachedHosts) {
        m_epochBuilds[host] = job->build;
        ++m_epochGraphBuilds[host];
    }
    // The passes' epochs, for the next builds' readback capture anchors: this build's for its host epochs, the earlier
    // builds' for the others.
    {
        auto known = std::make_shared<std::unordered_map<std::string, uint32_t>>();
        if (m_knownPassEpochs)
            for (const auto& [pass, epoch] : *m_knownPassEpochs)
                if (std::find(attachedHosts.begin(), attachedHosts.end(), br::render::HostEpochOf(epoch)) == attachedHosts.end())
                    known->emplace(pass, epoch);
        for (const auto& [pass, epoch] : frameBuild.passEpochs->Tagged()) known->insert_or_assign(pass, epoch);
        m_knownPassEpochs = std::move(known);
    }
    // An epoch that received another's passes is built with it from now on (StartFrameBuild).
    if (std::find(attachedHosts.begin(), attachedHosts.end(), br::render::kFrameHostEpoch) != attachedHosts.end())
        m_frameForwardMask = frameBuild.forwarded ? frameBuild.forwarded->hostEpochMask : 0u;
    PublishFrameBuild(frameBuild);
    for (const auto host : attachedHosts) {
        if (auto* graph = FrameGraph(host)) {
            graph->GetMemorySnapshotProvider().SetProvider(
                org::memory::CreateECSMemorySnapshotProvider(RendererECSManager::GetInstance().GetWorld()));
        }
    }
    Menu::GetInstance().SetRenderGraph(currentRenderGraph);
    {
        std::vector<std::pair<uint32_t, org::RenderGraph*>> graphs;
        for (const auto host : FrameGraphHostEpochs()) graphs.emplace_back(host, FrameGraph(host));
        Menu::GetInstance().SetEpochGraphs(std::move(graphs));
    }
    // The recipe replacement this build carried is in place.
    m_pipelineRollbackRecipe.reset();
    ConfirmPipelineReplacement();

    ++m_graphBuilds;
    m_lastGraphBuildMs = job->buildMs;
    m_lastGraphBuildFrame = m_totalFramesRendered;
    m_lastGraphBuildReasons = job->reason;
    m_lastGraphBuildHostEpochs = attachedHosts;
    auto& stats = m_graphBuildStats;
    stats.lastAttachMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    m_framesSinceFrameGraphAttach = 0;
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
    // The epochs the host refused: built again.
    if (refusedMask) RequestGraphRebuild("refused at the attach of build " + std::to_string(job->serial), refusedMask);
    // The host's own requests (an extension added, recording timestamps switched) made after the build started.
    if (!m_useGraphInstance) {
        for (size_t index = 0; index < frameBuild.hostEpochs.size() && index < job->hostRequestsAtStart.size(); ++index) {
            const auto host = frameBuild.hostEpochs[index];
            if (m_frameHost->RebuildRequests(host) != job->hostRequestsAtStart[index] && m_frameHost->RebuildRequested(host))
                RequestGraphRebuild("frame graph host: " + m_frameHost->RebuildReasons(host), br::render::HostEpochBit(host));
        }
    }
    spdlog::info("Renderer: frame graph build {} attached (host epochs {}); built in {:.1f} ms {} ({:.1f} ms of it waiting for {} pipelines), {:.1f} ms after "
        "its start, {} frames rendered meanwhile; render thread {:.1f} ms before it and {:.1f} ms at its attach; transients of {} of {} graphs placed "
        "at the build ({:.1f} ms)",
        job->serial, DescribeHostEpochs(attachedHosts), job->buildMs, job->onRenderThread ? "on the render thread" : "on the build thread", job->pipelineWaitMs,
        job->pipelinesDeclared, latencyMs, stats.lastFrames, job->prepareMs, stats.lastAttachMs, job->graphsPlacedAtBuild, job->graphs.size(),
        job->placementMs);
    // Staged recordings (M6 step 6): per epoch, the copies recorded on the build's thread, and why the rest were not.
    if (!job->staged.empty()) {
        std::string staged;
        for (const auto& entry : job->staged) {
            const auto& result = entry.result;
            staged += fmt::format("{}{}: {} of {} in {:.2f} ms{}", staged.empty() ? "" : "; ", entry.hostEpoch, result.recorded, result.copies,
                result.milliseconds, result.why.empty() ? std::string() : " (" + result.why + ")");
        }
        spdlog::info("Renderer: frame graph build {} staged recordings by host epoch: {}", job->serial, staged);
    } else if (!job->stagedFrame && !m_useGraphInstance) {
        spdlog::info("Renderer: frame graph build {} recorded nothing ahead (no frame stands in for its first ones)", job->serial);
    }
    // What crosses the attached graphs, by identity (deferred for a graph built in place: it has no schedule yet).
    if (!m_frameEpochCheckDeferred && !m_useGraphInstance) CheckFrameInterfaceAcrossGraphs(job->serial);
}

// The update values a build's passes declare from at Setup (FrameBuildInputs::lastUpdate): the last frame's, with the views
// as the first frame after the attach will have them. Null when a view's resources changed since that frame (a resize, a
// view made or destroyed): the passes then declare as the frame's Update finds them, as before.
static std::shared_ptr<const UpdateContext> MakeBuildUpdateContextFrom(const UpdateContext* lastFrame, ViewManager& views,
    const br::render::FrameBuildInputs& in) {
    if (!lastFrame || !lastFrame->viewFamily) return nullptr;
    const auto& last = *lastFrame;
    auto family = std::make_shared<br::render::PreparedViewFamilyState>(*last.viewFamily);
    size_t liveViews = 0;
    views.ForEachView([&](uint64_t) { ++liveViews; });
    if (family->views.size() != liveViews) return nullptr;
    for (auto& view : family->views) {
        const auto* live = views.Get(view.id);
        if (!live) return nullptr;
        // The primary view's depth maps are the build's (below): an attach since the last frame (a resize's, M5 step 5) changed the
        // live ones, but the build does not depend on them.
        const bool primaryDepthFromBuild = view.id == in.primaryViewID && in.depth.linearDepthMap;
        if (!primaryDepthFromBuild && live->gpu.linearDepthMap != view.linearDepthMap) return nullptr;
        if (view.id == in.primaryViewID && in.frameInterface.visibilityBuffer) {
            view.visibilityBuffer = in.frameInterface.visibilityBuffer;
            view.deepVisibilityHeadPointers = in.frameInterface.visibilityHeadPointers;
        } else if (live->gpu.visibilityBuffer != view.visibilityBuffer) {
            return nullptr;
        }
        // A build for a new render size: the primary view gets its depth maps and sizes at the attach (M5 step 5), and a new
        // linear depth map starts with no history.
        if (primaryDepthFromBuild && view.linearDepthMap != in.depth.linearDepthMap) {
            view.linearDepthMap = in.depth.linearDepthMap;
            view.depthHistory = {};
            if (in.camera) {
                view.cameraInfo.numDepthMips = in.camera->info.numDepthMips;
                view.cameraInfo.depthResX = in.camera->info.depthResX;
                view.cameraInfo.depthResY = in.camera->info.depthResY;
                view.cameraInfo.lodResY = in.camera->info.lodResY;
                view.cameraInfo.uvScaleToNextPowerOfTwo = in.camera->info.uvScaleToNextPowerOfTwo;
            }
        }
    }
    auto update = std::make_shared<UpdateContext>(last);
    update->viewFamily = std::move(family);
    return update;
}

std::shared_ptr<const UpdateContext> Renderer::MakeBuildUpdateContext(const br::render::FrameBuildInputs& in) {
    if (!m_pViewManager) return nullptr;
    auto update = MakeBuildUpdateContextFrom(m_lastFrameUpdate.get(), *m_pViewManager, in);
    if (!update && m_lastFrameUpdate)
        spdlog::info("Renderer: the views changed since the last frame; the frame graph build's passes declare at their first Update");
    return update;
}

void Renderer::MakeFrameBuild(br::render::FrameBuild& build, flecs::entity primaryCamera) {
    BT_ZONE_SCOPE("Renderer::MakeFrameBuild");
    auto& in = build.inputs;
    using namespace br::pipeline;

    in.primaryViewID = primaryCamera.get<Components::RenderViewRef>().viewID;
    // The objects of the build's sizes (M5 step 5): the newest, which the attached graphs have unless a resolution request made new
    // ones since. Their depth maps, until the view takes them at this build's attach (SwitchResolutionObjects).
    in.resolution = m_buildResolutionObjects;
    in.depth = in.resolution && in.resolution->primaryDepth ? *in.resolution->primaryDepth : primaryCamera.get<Components::DepthMap>();
    if (const auto* camera = primaryCamera.try_get<Components::Camera>()) in.camera = *camera;
    if (in.camera && in.resolution && in.resolution->primaryDepth) {
        // The sizes the camera has from the attach on (as BuildRendererCamera makes them).
        auto& info = in.camera->info;
        const auto size = in.resolution->render;
        info.numDepthMips = NumMips(size.x, size.y);
        info.depthResX = size.x;
        info.depthResY = size.y;
        info.lodResY = in.resolution->primaryLodHeight ? in.resolution->primaryLodHeight : size.y;
        info.uvScaleToNextPowerOfTwo = { static_cast<float>(size.x) / static_cast<float>(in.depth.linearDepthMap->GetInternalWidth()),
            static_cast<float>(size.y) / static_cast<float>(in.depth.linearDepthMap->GetInternalHeight()) };
    }
    in.cameraBuffer = m_pViewManager->GetCameraBuffer();
    in.cullingCameraBuffer = m_pViewManager->GetCullingCameraBuffer();
    // The renderer's cross-epoch objects, made before the views the build's passes declare from (in.lastUpdate).
    UpdateFrameInterface(in.primaryViewID);
    in.frameInterface = *m_frameInterface;
    in.lastUpdate = MakeBuildUpdateContext(in);

    if (m_pipelineRecipe.Contains<BrdfIntegrationTechnique>()) in.brdfLut = EnsureBrdfLut();
    // Post and Overlay share these (M6 step 3): the LPM constants Post's exposure writes, and the bloom texture of the build's
    // output size. Made here, on the render thread, before any build of theirs.
    if (!m_lpmConstants) m_lpmConstants = TonemappingPass::CreateLpmConstants();
    in.lpmConstants = m_lpmConstants;
    if (in.resolution && !in.resolution->bloom) in.resolution->bloom = CreateBloomTexture(in.resolution->output);

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

    // The latest upscaler configuration (M5 step 3): the graphs built now declare its mode and evaluate with it.
    if (m_pipelineRecipe.Contains<UpscalingTechnique>()) in.upscaling = br::render::UpscalingGenerationService::CaptureCurrent();
    in.upscalingMode = in.upscaling ? in.upscaling->Mode() : UpscalingManager::GetInstance().GetCurrentUpscalingMode();

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

    build.Outputs(br::render::kVisibilityEpoch).terrainRvtResources = m_producerPersistentState->terrainRvtResources;
    build.Outputs(br::render::kVisibilityEpoch).terrainRvtGeneration = m_producerPersistentState->terrainRvtGeneration;

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

void Renderer::PublishResourceEntityWrites(br::render::FrameBuild& build) {
    // Every declared epoch's, in frame epoch order: the directory below drops what none of them published again.
    std::vector<br::render::ResourceEntityWrite> writes;
    std::vector<uint32_t> writeHosts;
    for (const auto epoch : build.epochs) {
        auto& epochWrites = build.Outputs(epoch).resourceEntityWrites;
        writeHosts.insert(writeHosts.end(), epochWrites.size(), br::render::HostEpochOf(epoch));
        std::move(epochWrites.begin(), epochWrites.end(), std::back_inserter(writes));
        epochWrites.clear();
    }
    // The directory's resources are the build's extension instances', which are new every build. An entry of an epoch this
    // build rebuilt that it does not publish again belongs to a graph that is retiring: it leaves the directory now, so
    // first-match readers never see the old graph's buffers (a variant the new graph does not have, say). Entries of epochs
    // the build did not rebuild stay: their graphs keep running.
    std::unordered_set<std::string> republished;
    for (const auto& write : writes) {
        if (!write.directoryKey.empty()) republished.insert(write.directoryKey);
    }
    const auto rebuilt = [&](uint32_t host) {
        return std::find(build.hostEpochs.begin(), build.hostEpochs.end(), host) != build.hostEpochs.end() || build.hostEpochs.empty();
    };
    for (auto it = m_publishedResourceEntities.begin(); it != m_publishedResourceEntities.end();) {
        if (republished.contains(it->first) || !rebuilt(it->second.hostEpoch)) {
            ++it;
            continue;
        }
        if (auto previous = it->second.resource.lock()) {
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
    for (size_t index = 0; index < writes.size(); ++index) {
        auto& write = writes[index];
        if (!write.directoryKey.empty()) {
            // A resource replaced under the same name leaves the directory, so its first-match readers see the new one.
            auto& published = m_publishedResourceEntities[write.directoryKey];
            if (auto previous = published.resource.lock(); previous && previous != write.resource) {
                if (auto entity = previous->GetECSEntity(); entity && entity.has<Components::Resource>())
                    entity.remove<Components::Resource>();
            }
            published.resource = write.resource;
            published.hostEpoch = writeHosts[index];
        }
        if (auto entity = write.resource->GetECSEntity()) write.write(world, entity);
    }
    writes.clear();
}

void Renderer::PublishFrameBuild(br::render::FrameBuild& build) {
    BT_ZONE_SCOPE("Renderer::PublishFrameBuild");
    PublishResourceEntityWrites(build);
    // The objects of the build's sizes become current (M5 step 5): the swapchain, the presentation surfaces, the primary view's
    // depth maps and the resolution settings, when they are not the attached graphs' already.
    SwitchResolutionObjects(build.inputs.resolution);
    // Per-frame values (the render context's resolutions, the light grid shape, jitter) follow the attached graphs' sizes.
    m_attachedResolution = FrameResolution{ build.inputs.renderResolution, build.inputs.outputResolution };
    // The upscaler configuration the build's graphs evaluate with is current from their attach (M5 step 3): jitter and the status
    // follow it. Waits for its preparation if that is still running (the build usually outlasts it).
    if (build.inputs.upscaling) UpscalingManager::GetInstance().MakeCurrent(build.inputs.upscaling->Configuration());
    // Each epoch's outputs, from the epoch that owns them (FrameBuildOutputs). An epoch the build did not declare keeps what
    // it published before.
    if (build.Declares(br::render::kVisibilityEpoch)) {
        auto& out = build.Outputs(br::render::kVisibilityEpoch);
        if (out.virtualShadowCasters && !out.virtualShadowCasters->Empty()) {
            spdlog::info("Renderer: the frame graph has {} virtual shadow caster(s)", out.virtualShadowCasters->Size());
        }
        // CLod streaming now runs with the new graph's binding; without CLod in the graph it stops.
        if (const auto& streaming = m_producerPersistentState->clodStreaming) streaming->AttachGraph(out.clodStreaming);
        // Shading's graph (the alpha variant, the streaming tail) works with the binding's buffers. A Visibility rebuild shares
        // the attached ones (CLodExtension::BuildStreamingBinding); when it made new ones (the shadow variant's lists are per
        // build) and Shading's graph was not built with them, Shading is rebuilt: until then its tail submits nothing.
        const auto previous = m_frameInterface ? m_frameInterface->clodStreaming : nullptr;
        // SelectBuildHostEpochs puts Shading in such a build's set, so this is a fallback that should not run.
        if (out.clodStreaming && previous && !out.clodStreaming->SharesBuffersWith(*previous) && !build.Declares(br::render::kShadingEpoch)
            && br::render::HostEpochOf(br::render::kShadingEpoch) != br::render::HostEpochOf(br::render::kVisibilityEpoch)) {
            spdlog::warn("Renderer: a Visibility build replaced the CLod streaming buffers without a Shading graph in its set; Shading is "
                "built again, and its streaming tail submits nothing until then");
            RequestGraphRebuild("CLod streaming buffers replaced by a Visibility build", br::render::HostEpochMaskOf({ br::render::kShadingEpoch }));
        }
        // The CLod variants of later epochs' builds write their feedback into it (CLodExtension::BuildStreamingBinding).
        if (m_frameInterface) m_frameInterface->clodStreaming = out.clodStreaming;
        m_materialEvaluationInputs = out.materialEvaluationInputs;
        if (out.visibilityBuffer)
            m_pViewManager->AttachVisibilityBuffer(build.inputs.primaryViewID, out.visibilityBuffer, build.inputs.frameInterface.visibilityHeadPointers);
        // Unless a terrain RVT setting invalidated the cache while the build ran: the next build makes it again.
        if (out.terrainRvt && out.terrainRvtGeneration == m_producerPersistentState->terrainRvtGeneration)
            m_producerPersistentState->terrainRvtResources = out.terrainRvtResources;
    }
    if (build.Declares(br::render::kShadingEpoch)) m_pathTraceState = build.Outputs(br::render::kShadingEpoch).pathTraceState;
}

std::string Renderer::DumpFrameGraphMemory() {
    std::string reply = "{\"frame\":" + std::to_string(m_totalFramesRendered) + ",\"frame_placement\":" + (m_framePlacement ? "true" : "false");
    if (currentRenderGraph) {
        currentRenderGraph->WriteVramUsageDebugDumpNow(static_cast<uint32_t>(m_totalFramesRendered));
        reply += ",\"vram_dump\":true";
    }
    if (m_frameHost && m_framePlacement) {
        m_frameHost->LogFramePlacement(true);
        if (const auto version = m_frameHost->LatestFramePlacement()) {
            reply += ",\"frame_placement_version\":" + std::to_string(version->Id());
            reply += ",\"frame_placement_packed_mb\":" + std::to_string(version->Bytes() / (1024.0 * 1024.0));
        }
        // What the placement's heaps hold now: current ones, and replaced ones still alive.
        double current = 0.0, replaced = 0.0;
        for (const auto& heap : m_frameHost->FramePlacementHeaps())
            (heap.current ? current : replaced) += static_cast<double>(heap.capacity) / (1024.0 * 1024.0);
        reply += ",\"frame_placement_heaps_mb\":" + std::to_string(current) + ",\"frame_placement_replaced_heaps_mb\":" + std::to_string(replaced);
        reply += ",\"stale_epochs\":" + std::to_string(m_frameHost->StaleAliasPlacementEpochs().size());
        // Each host epoch's graph: the version it placed from and how many candidates it placed from it (M4 step 5).
        std::string epochs;
        for (const auto host : FrameGraphHostEpochs()) {
            const auto use = m_frameHost->FramePlacementInUse(host);
            epochs += (epochs.empty() ? "" : ",") + std::string("{\"epoch\":") + std::to_string(host) + ",\"planned\":"
                + (use.planned ? "true" : "false") + ",\"version\":" + std::to_string(use.version) + ",\"placed\":"
                + std::to_string(use.placed) + "}";
        }
        reply += ",\"epochs\":[" + epochs + "]";
    }
    return reply + "}";
}

void Renderer::UpdateFrameInterface(uint64_t primaryViewID) {
    if (!m_frameInterface) m_frameInterface = std::make_shared<br::render::FrameInterface>();
    auto& frameInterface = *m_frameInterface;
    if (!m_pipelineRecipe.Contains<br::pipeline::CanonicalSurfaceResourcesTechnique>()) {
        // No graph declares a canonical surface: nothing to keep (the view keeps the one it has, as before).
        frameInterface.visibilityBuffer.reset();
        frameInterface.visibilityHeadPointers.reset();
        frameInterface.primaryViewID = 0;
        frameInterface.registrations.clear();
        frameInterface.width = frameInterface.height = 0;
        frameInterface.rayReconstructionGuides = false;
        return;
    }
    const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    // The cross-epoch transients every epoch's graph registers (M4 step 5): made again only for another render resolution,
    // and the guides when Ray Reconstruction comes or goes.
    const bool guides = UpscalingManager::GetInstance().GetCurrentUpscalingMode() == UpscalingMode::DLSSRayReconstruction;
    if (frameInterface.registrations.empty() || frameInterface.width != resolution.x || frameInterface.height != resolution.y
        || frameInterface.rayReconstructionGuides != guides) {
        frameInterface.registrations.clear();
        const auto keep = [&](std::string_view id, std::shared_ptr<org::Resource> resource) {
            frameInterface.registrations.push_back({ std::string(id), std::move(resource) });
        };
        CreateCanonicalSurfaceResources(keep);
        CreateDebugVisualizationResources(keep);
        if (guides) br::render::UpscalingGenerationService::MakeRayReconstructionGuides(keep);
        // Shading's inputs from Visibility (or Shading's own, by name in every graph): one object for every build, so a
        // Visibility rebuild leaves Shading's graph valid and frame placement matches them across the two.
        const auto texture = [&](uint32_t channels, rhi::Format format, const char* name, uint32_t arraySize = 0) {
            org::TextureDescription desc;
            desc.channels = channels;
            desc.format = format;
            desc.hasSRV = desc.hasUAV = true;
            if (arraySize) {
                desc.hasSRV = false;
                desc.isArray = true;
                desc.arraySize = arraySize;
            }
            desc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
            auto made = org::PixelBuffer::CreateSharedUnmaterialized(desc);
            made->SetName(name);
            return made;
        };
        frameInterface.sunShadowMask = texture(1, rhi::Format::R8_UNorm, "CLod RT sun shadow");
        frameInterface.stochasticDiffuse = texture(4, rhi::Format::R16G16B16A16_Float, "Stochastic lighting diffuse");
        frameInterface.stochasticSpecular = texture(4, rhi::Format::R16G16B16A16_Float, "Stochastic lighting specular");
        // VSM flicker telemetry: the deferred pass reprojects each pixel's shadow outcome against the previous frame's
        // (ping-pong slices).
        frameInterface.vsmReceiverHistory = texture(4, rhi::Format::R32G32B32A32_UInt, "CLod VSM receiver history", 2);
        frameInterface.width = resolution.x;
        frameInterface.height = resolution.y;
        frameInterface.rayReconstructionGuides = guides;
        spdlog::info("Renderer: frame interface: {} cross-epoch transients for {}x{}{}", frameInterface.registrations.size(),
            resolution.x, resolution.y, guides ? " (with the Ray Reconstruction guides)" : "");
    }
    if (const auto& current = frameInterface.visibilityBuffer; current && frameInterface.primaryViewID == primaryViewID
        && current->GetWidth() == resolution.x && current->GetHeight() == resolution.y)
        return;
    // A transient of the graphs that use it (allowAlias): one object for all of them, placed by each graph's alias plan.
    org::TextureDescription desc;
    desc.channels = 2;
    desc.format = rhi::Format::R32G32_UInt;
    desc.hasRTV = desc.hasSRV = desc.hasUAV = desc.hasNonShaderVisibleUAV = true;
    desc.allowAlias = true;
    desc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
    frameInterface.visibilityBuffer = org::PixelBuffer::CreateSharedUnmaterialized(desc);
    frameInterface.visibilityBuffer->SetName("Visibility Buffer");
    org::memory::SetResourceUsageHint(*frameInterface.visibilityBuffer, "Canonical surface visibility");
    frameInterface.visibilityHeadPointers = m_pViewManager->MakeCLodDeepVisibilityHeadPointers(primaryViewID, *frameInterface.visibilityBuffer);
    frameInterface.primaryViewID = primaryViewID;
    spdlog::info("Renderer: frame interface: visibility buffer {}x{} for view {}", resolution.x, resolution.y, primaryViewID);
}

std::optional<std::string> Renderer::CheckFrameEpochInterface(org::RenderGraph& graph, const br::render::FrameBuild& build, uint64_t serial,
    uint32_t hostEpoch) {
    BT_ZONE_SCOPE("Renderer::CheckFrameEpochInterface");
    const auto begin = std::chrono::steady_clock::now();
    auto trace = graph.TraceSelectedSchedule(hostEpoch == br::render::kNoFrameEpoch ? br::render::kFrameHostEpoch : hostEpoch);
    if (!trace) trace = graph.TraceSelectedSchedule(UINT32_MAX);
    if (!trace) return std::nullopt;
    {
        // For the across-graphs check at the attach. A graph's placement does not change what it uses, only where.
        std::scoped_lock lock(m_graphTraceMutex);
        m_graphTraces[&graph] = trace;
    }
    const auto check = br::render::CheckFrameEpochs(*trace, build);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    // The crossing set, by identity and epochs: logged in full when it changes (or every build, BASICRENDERER_CHECK_EPOCH_INTERFACE=1).
    // Inside one graph (Visibility and Shading share one since step 5) a crossing is the graph's own transient.
    std::string key;
    for (const auto& crossing : check.crossing) {
        key += crossing.name;
        for (const auto epoch : crossing.epochs) key += char('0' + epoch);
        key += ';';
    }
    static const bool always = br::runtime::renderer_settings::ReadTruthyEnvironmentFlag("BASICRENDERER_CHECK_EPOCH_INTERFACE");
    auto& lastKey = m_lastFrameEpochCheckKeys[hostEpoch];
    const bool changed = always || key != lastKey || !check.untagged.empty();
    lastKey = std::move(key);
    spdlog::info("Renderer: frame graph build {} host epoch {} epochs ({:.1f} ms): {}", serial,
        hostEpoch == br::render::kNoFrameEpoch ? std::string("(all)") : std::to_string(hostEpoch), ms, check.Describe(changed ? 200 : 0));
    if (check.untagged.empty()) return std::string{};
    std::string failure = std::to_string(check.untagged.size()) + " pass(es) without a frame epoch:";
    for (const auto& pass : check.untagged) failure += " " + pass;
    return failure;
}

// The epoch interface across the host epochs' graphs (M4 step 5): every resource the attached graphs' schedules use, by identity.
// One used by two graphs is a cross-epoch resource: one object, which frame placement places over its span in the frame (or a
// persistent one). The same name used by two graphs as different objects is a resource each epoch made for itself: the later
// epoch does not see the earlier one's contents. Logged in full when the set changes; render thread, after an attach.
void Renderer::CheckFrameInterfaceAcrossGraphs(uint64_t serial) {
    BT_ZONE_SCOPE("Renderer::CheckFrameInterfaceAcrossGraphs");
    const auto hosts = FrameGraphHostEpochs();
    if (hosts.size() < 2) return;
    const auto begin = std::chrono::steady_clock::now();
    struct Use {
        std::string name;
        uint64_t bytes = 0;
        bool aliasCandidate = false;
        uint32_t hostMask = 0;
    };
    std::unordered_map<uint64_t, Use> byObject;
    std::unordered_map<std::string, std::unordered_map<uint64_t, uint32_t>> byName;  // name -> object -> host mask
    for (const auto host : hosts) {
        auto* graph = FrameGraph(host);
        if (!graph) return;
        std::shared_ptr<const org::ScheduleTrace> trace;
        {
            std::scoped_lock lock(m_graphTraceMutex);
            if (const auto found = m_graphTraces.find(graph); found != m_graphTraces.end()) trace = found->second;
        }
        if (!trace) trace = graph->TraceSelectedSchedule(host);
        if (!trace) trace = graph->TraceSelectedSchedule(UINT32_MAX);
        if (!trace) {
            spdlog::info("Renderer: frame graph build {} across host epochs: host epoch {}'s graph has no schedule yet", serial, host);
            return;
        }
        std::vector<bool> used(trace->resources.size(), false);
        for (const auto& pass : trace->passes)
            for (const auto& access : pass.accesses)
                if (access.resource < used.size()) used[access.resource] = true;
        for (size_t index = 0; index < trace->resources.size(); ++index) {
            if (!used[index]) continue;
            const auto& resource = trace->resources[index];
            if (!resource.objectId) continue;
            auto& use = byObject[resource.objectId];
            use.name = resource.name;
            use.bytes = (std::max)(use.bytes, resource.bytes);
            use.aliasCandidate = use.aliasCandidate || resource.aliasCandidate;
            use.hostMask |= br::render::HostEpochBit(host);
            byName[resource.name][resource.objectId] |= br::render::HostEpochBit(host);
        }
    }
    {
        // The traces of graphs no host epoch runs any more go.
        std::scoped_lock lock(m_graphTraceMutex);
        std::erase_if(m_graphTraces, [&](const auto& entry) {
            return std::none_of(hosts.begin(), hosts.end(), [&](uint32_t host) { return FrameGraph(host) == entry.first; });
        });
    }
    std::unordered_set<uint64_t> interfaceIds;
    if (m_frameInterface)
        for (const auto* object : m_frameInterface->Objects()) interfaceIds.insert(object->GetGlobalResourceID());
    const auto hostsOf = [&](uint32_t mask) {
        std::string text;
        for (const auto host : hosts) if (mask & br::render::HostEpochBit(host)) text += (text.empty() ? "" : "+") + std::to_string(host);
        return text;
    };
    struct Row { std::string name; uint64_t bytes; uint32_t mask; bool interfaceObject; };
    std::vector<Row> transients;
    size_t persistent = 0;
    uint64_t transientBytes = 0;
    for (const auto& [id, use] : byObject) {
        if (std::popcount(use.hostMask) < 2) continue;
        if (!use.aliasCandidate) {
            ++persistent;
            continue;
        }
        transients.push_back({ use.name, use.bytes, use.hostMask, interfaceIds.contains(id) });
        transientBytes += use.bytes;
    }
    std::sort(transients.begin(), transients.end(), [](const Row& a, const Row& b) { return a.bytes != b.bytes ? a.bytes > b.bytes : a.name < b.name; });
    // Names two graphs use as different alias candidates.
    std::vector<std::string> split;
    for (const auto& [name, objects] : byName) {
        if (objects.size() < 2) continue;
        uint32_t union_ = 0;
        for (const auto& [object, mask] : objects)
            if (byObject[object].aliasCandidate) union_ |= mask;
        bool separate = false;
        for (const auto& [object, mask] : objects)
            if (byObject[object].aliasCandidate && (union_ & ~mask)) separate = true;
        if (separate) split.push_back(name + " (" + std::to_string(objects.size()) + " objects, host epochs " + hostsOf(union_) + ")");
    }
    std::sort(split.begin(), split.end());
    std::string key;
    for (const auto& row : transients) key += row.name + hostsOf(row.mask) + ";";
    for (const auto& name : split) key += "!" + name;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    static const bool always = br::runtime::renderer_settings::ReadTruthyEnvironmentFlag("BASICRENDERER_CHECK_EPOCH_INTERFACE");
    const bool changed = always || key != m_lastFrameInterfaceCheckKey;
    m_lastFrameInterfaceCheckKey = std::move(key);
    std::string text = fmt::format("{} transient(s) {:.1f} MB cross the host epochs' graphs (one object each), {} persistent; "
        "{} name(s) are different objects in different graphs", transients.size(), transientBytes / 1048576.0, persistent, split.size());
    if (changed) {
        for (const auto& row : transients)
            text += fmt::format("\n  '{}' {:.2f} MB host epochs {}{}", row.name, row.bytes / 1048576.0, hostsOf(row.mask),
                row.interfaceObject ? " (frame interface)" : "");
        for (const auto& name : split) text += "\n  different objects: '" + name + "'";
    }
    spdlog::info("Renderer: frame graph build {} across host epochs ({:.1f} ms): {}", serial, ms, text);
    if (!split.empty() && changed)
        spdlog::warn("Renderer: {} resource name(s) are separate objects in different host epochs' graphs (no contents cross): see above", split.size());
}

void Renderer::DeclareFrameProviders(org::RenderGraph& graph) {
    BT_ZONE_SCOPE("Renderer::DeclareFrameProviders");
    auto* frameBuild = br::render::CurrentFrameBuild();
    if (!frameBuild) throw std::logic_error("DeclareFrameProviders: no frame build in progress");
    // Testing a failed build (M5 step 8): BASICRENDERER_FAIL_FRAME_BUILD="2,5" fails the 2nd and 5th frame builds of the process in
    // their Shading graph (or their first graph without one), as a declaration error would.
    {
        static const std::vector<uint64_t> failing = [] {
            std::vector<uint64_t> builds;
            if (const char* value = std::getenv("BASICRENDERER_FAIL_FRAME_BUILD"))
                for (const char* p = value; *p;) {
                    char* end = nullptr;
                    const auto n = std::strtoull(p, &end, 10);
                    if (end == p) break;
                    builds.push_back(n);
                    p = *end ? end + 1 : end;
                }
            return builds;
        }();
        if (!failing.empty()) {
            // A build is its FrameBuild with its settings snapshot (an address alone may come back for a later build).
            static std::pair<const void*, const void*> last{};
            static uint64_t count = 0;
            if (const std::pair<const void*, const void*> current{frameBuild, frameBuild->inputs.settings.get()}; current != last) {
                last = current;
                ++count;
            }
            const auto& hosts = frameBuild->hostEpochs;
            const uint32_t shading = br::render::HostEpochOf(br::render::kShadingEpoch);
            const uint32_t target = std::find(hosts.begin(), hosts.end(), shading) != hosts.end() ? shading : (hosts.empty() ? 0u : hosts.front());
            if (std::find(failing.begin(), failing.end(), count) != failing.end() && graph.PassEpochFilter() == target)
                throw std::runtime_error("BASICRENDERER_FAIL_FRAME_BUILD: frame build " + std::to_string(count) + " fails here (test)");
        }
    }
    // The managers' copies MakeFrameBuild took (the graph keeps pointers to them: the providers' extension holds them).
    for (const auto& provider : frameBuild->inputs.providers) graph.RegisterProvider(provider.get());
    // The renderer's cross-epoch objects, the same in every epoch's graph (FrameInterface), before any extension declares.
    const auto& frameInterface = frameBuild->inputs.frameInterface;
    if (frameInterface.visibilityBuffer) graph.RegisterResource(Builtin::PrimaryCamera::VisibilityTexture, frameInterface.visibilityBuffer);
    for (const auto& registration : frameInterface.registrations) graph.RegisterResource(registration.id, registration.resource);
    // Registered only for the options that use them: passes read their presence.
    const auto& in = frameBuild->inputs;
    if (in.rayTracedSunShadow && frameInterface.sunShadowMask) graph.RegisterResource(Builtin::RayTracing::SunShadow, frameInterface.sunShadowMask);
    if (in.stochasticLightingActive && frameInterface.stochasticDiffuse) {
        graph.RegisterResource(Builtin::StochasticLighting::Diffuse, frameInterface.stochasticDiffuse);
        graph.RegisterResource(Builtin::StochasticLighting::Specular, frameInterface.stochasticSpecular);
    }
    if (!br::runtime::renderer_settings::CLodVirtualShadowFrameTelemetryPath().empty() && frameInterface.vsmReceiverHistory)
        graph.RegisterResource(Builtin::Shadows::CLodReceiverHistory, frameInterface.vsmReceiverHistory);
    // The directional virtual shadow map, which Visibility's shadow variant writes and Shading's receivers (deferred shading, the
    // alpha variant) read: the renderer's objects, so a graph of another epoch binds what Visibility's graph writes even when it
    // is rebuilt alone. Visibility's own graph (and the in-place graph, which has the variant) takes them from the variant.
    const uint32_t hostEpoch = graph.PassEpochFilter();
    if (in.directionalVsm && hostEpoch != br::render::kNoFrameEpoch && hostEpoch != br::render::HostEpochOf(br::render::kVisibilityEpoch)) {
        const auto& vsm = *in.directionalVsm;
        const std::pair<org::ResourceIdentifier, const std::shared_ptr<org::Resource>*> receivers[] = {
            {Builtin::Shadows::CLodPageTable, &vsm.pageTable}, {Builtin::Shadows::CLodPhysicalPages, &vsm.physicalPages},
            {Builtin::Shadows::CLodClipmapInfo, &vsm.clipmapInfo}, {Builtin::Shadows::CLodCompactMainCamera, &vsm.mainCamera},
            {Builtin::Shadows::CLodCompactShadowCameras, &vsm.shadowCameras},
            {Builtin::Shadows::CLodDirectionalPageViewInfo, &vsm.pageViewInfo}, {Builtin::Shadows::CLodPageMetadata, &vsm.pageMetadata},
            {Builtin::Shadows::CLodStats, &vsm.stats}};
        for (const auto& [id, resource] : receivers)
            if (*resource) graph.RegisterResource(id, *resource);
    }
}

namespace br::render {
// What the renderer's pass declarations of one build share (Renderer::DeclareFramePasses and its epochs' functions).
struct FramePassDeclaration {
    org::RenderGraph& graph;
    FrameBuild& build;
    const FrameBuildInputs& in;
    const br::pipeline::PipelineRecipe& recipe;
};
} // namespace br::render

// Which frame epoch a technique's passes belong to (docs/frame_graph_composition_plan.md, M4; the M4 design's section 2):
// Visibility ends with material evaluation. The frame resources, the BRDF table, the environment and the ray tracing scene
// are registrations every epoch reads; until the frame is split they are declared once, with Visibility. DebugOutput has
// passes in two epochs (Post's resolve, Overlay's skeletons and spheres). Present is Overlay's: it makes the frame's last
// backbuffer access, so it ends the last epoch.
static uint32_t FrameEpochOfTechnique(br::pipeline::TechniqueId id) {
    using enum br::pipeline::TechniqueId;
    switch (id) {
    case FrameResources: case BrdfIntegration: case Environment: case ClusterLod: case ClusterLodShadow: case ClusterLodVoxel:
    case CanonicalSurfaceResources: case VisibilityMaterialBinning: case TerrainRvt: case TerrainRegionMaterialEvaluation:
    case MaterialEvaluation:
        return br::render::kVisibilityEpoch;
    case ClusterLodAlpha: case Gtao: case ClusteredLighting: case PrimaryLighting: case Reflections: case PathTracing:
    case StochasticDirectLighting:
        return br::render::kShadingEpoch;
    case Exposure: case Upscaling: case Bloom: case DepthHistory:
        return br::render::kPostEpoch;
    // Tonemapping and the debug resolve write PresentationColor, the frame slot's surface, which another object binds each frame. In
    // Overlay, which records every frame anyway, Post's recording does not depend on it (M6 step 3, decision D3).
    case Tonemapping: case DebugOutput: case DebugUi: case Present:
        return br::render::kOverlayEpoch;
    }
    return br::render::kNoFrameEpoch;
}

// The techniques whose declarations only register what every epoch's passes read. Each graph makes them (DeclareFramePasses).
static bool IsSharedRegistrationTechnique(br::pipeline::TechniqueId id) {
    using enum br::pipeline::TechniqueId;
    return id == FrameResources || id == BrdfIntegration || id == Environment || id == ClusterLod || id == ClusterLodShadow
        || id == ClusterLodVoxel;
}

// The host epochs a recipe replacement changes (M4 step 5): the epochs of the techniques it adds, removes or configures again
// (an entry whose technique object differs), and of the extensions whose ids or epochs differ. A shared registration (made in
// every graph) names every epoch; path tracing also changes whether the lens flares trace (Post), and bloom whether
// tonemapping (Overlay) reads its texture.
uint32_t Renderer::RecipeChangeHostEpochs(const br::pipeline::PipelineRecipe& before, const br::pipeline::PipelineRecipe& after) {
    uint32_t mask = 0;
    const auto touch = [&](br::pipeline::TechniqueId id) {
        using enum br::pipeline::TechniqueId;
        // Registrations every graph makes (the CLod techniques' are their extensions', Visibility's).
        if (id == FrameResources || id == BrdfIntegration || id == Environment) {
            mask = br::render::FrameRevisions::kAllEpochs;
            return;
        }
        const auto epoch = FrameEpochOfTechnique(id);
        if (br::render::IsFrameEpoch(epoch)) mask |= br::render::HostEpochMaskOf({ epoch });
        else mask = br::render::FrameRevisions::kAllEpochs;
        if (id == PathTracing) mask |= br::render::HostEpochMaskOf({ br::render::kPostEpoch });
        if (id == Bloom) mask |= br::render::HostEpochMaskOf({ br::render::kOverlayEpoch });
    };
    const auto find = [](const br::pipeline::PipelineRecipe& recipe, br::pipeline::TechniqueId id) -> const br::pipeline::TechniqueEntry* {
        for (const auto& entry : recipe.Techniques()) if (entry.id == id) return &entry;
        return nullptr;
    };
    for (const auto& entry : before.Techniques()) {
        const auto* other = find(after, entry.id);
        if (!other || other->technique != entry.technique) touch(entry.id);
    }
    for (const auto& entry : after.Techniques())
        if (!find(before, entry.id)) touch(entry.id);
    // The order of the techniques is the declaration order inside each epoch.
    std::vector<br::pipeline::TechniqueId> beforeOrder, afterOrder;
    for (const auto& entry : before.Techniques()) if (find(after, entry.id)) beforeOrder.push_back(entry.id);
    for (const auto& entry : after.Techniques()) if (find(before, entry.id)) afterOrder.push_back(entry.id);
    if (beforeOrder != afterOrder) mask = br::render::FrameRevisions::kAllEpochs;
    const auto extensionsOf = [](const br::pipeline::PipelineRecipe& recipe) {
        std::vector<std::pair<std::string, uint32_t>> extensions;
        for (const auto& [id, factory] : recipe.Extensions()) extensions.emplace_back(id, recipe.ExtensionEpoch(id));
        return extensions;
    };
    const auto beforeExtensions = extensionsOf(before), afterExtensions = extensionsOf(after);
    if (beforeExtensions != afterExtensions) {
        for (const auto* list : { &beforeExtensions, &afterExtensions })
            for (const auto& [id, epoch] : *list)
                if (std::find(beforeExtensions.begin(), beforeExtensions.end(), std::pair{ id, epoch }) == beforeExtensions.end()
                    || std::find(afterExtensions.begin(), afterExtensions.end(), std::pair{ id, epoch }) == afterExtensions.end())
                    mask |= br::render::IsFrameEpoch(epoch) ? br::render::HostEpochMaskOf({ epoch }) : br::render::FrameRevisions::kAllEpochs;
    }
    // Nothing found (bindings, say): every epoch, as before.
    return mask ? mask : br::render::FrameRevisions::kAllEpochs;
}

// Whether the debug output is the skeleton view, which replaces the debug resolve.
static bool DebugSkeletonsShown() {
    return SettingsManager::GetInstance().getSettingGetter<unsigned int>("outputType")() ==
        static_cast<unsigned int>(OutputType::SKELETONS) && DeviceManager::GetInstance().GetMeshShadersSupported();
}

void Renderer::DeclareVisibilityPasses(const br::render::FramePassDeclaration& d, br::pipeline::TechniqueId id) {
    BT_ZONE_SCOPE("Renderer::DeclareVisibilityPasses");
    auto& graph = d.graph;
    const auto& in = d.in;
    const auto& recipe = d.recipe;
    auto& out = d.build.Outputs(br::render::kVisibilityEpoch);
    auto& materialEvaluationInputs = out.materialEvaluationInputs;
    const auto& depth = in.depth;
    const std::shared_ptr<org::PixelBuffer>& depthTexture = depth.depthMap;
    const bool terrainRvtEnabled = recipe.Contains<br::pipeline::TerrainRvtTechnique>();
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
        graph.RegisterResource(Builtin::PresentationColor, in.resolution ? in.resolution->dynamicPresentationColor : m_dynamicPresentationColor);
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
    case ClusterLodShadow:
    case ClusterLodVoxel:
        break; // These techniques own ordered graph extensions.
    case CanonicalSurfaceResources: {
        const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
        // The visibility buffer, the canonical surface textures and records and the debug visualization are the renderer's
        // frame interface, registered in every epoch's graph (DeclareFrameProviders).
        const auto& visibilityBuffer = in.frameInterface.visibilityBuffer;  // MakeFrameBuild (the renderer's FrameInterface)
        if (!visibilityBuffer) throw std::logic_error("DeclareFramePasses: the frame build has no visibility buffer");
        // The sun shadow mask, stochastic lighting's radiance and the VSM receiver history are the interface's too, registered
        // for the options that use them (DeclareFrameProviders).
        out.visibilityBuffer = visibilityBuffer;  // attached to the primary view after the build (PublishFrameBuild)
        if (in.frameInterface.registrations.empty()) throw std::logic_error("DeclareFramePasses: the frame build has no canonical surface");
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
    default:
        break;
    }
}

void Renderer::DeclareShadingPasses(const br::render::FramePassDeclaration& d, br::pipeline::TechniqueId id) {
    BT_ZONE_SCOPE("Renderer::DeclareShadingPasses");
    auto& graph = d.graph;
    const auto& in = d.in;
    const auto& recipe = d.recipe;
    auto& out = d.build.Outputs(br::render::kShadingEpoch);
    using enum br::pipeline::TechniqueId;
    switch (id) {
    case ClusterLodAlpha:
        break; // These techniques own ordered graph extensions.
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
    case StochasticDirectLighting:
        break; // Its passes come from a graph extension (RegisterPipelineExtensions).
    default:
        break;
    }
}

void Renderer::DeclarePostPasses(const br::render::FramePassDeclaration& d, br::pipeline::TechniqueId id) {
    BT_ZONE_SCOPE("Renderer::DeclarePostPasses");
    auto& graph = d.graph;
    const auto& in = d.in;
    const auto& recipe = d.recipe;
    using enum br::pipeline::TechniqueId;
    switch (id) {
    case Exposure: {
        auto adapted = CreateIndexedStructuredBuffer(1, sizeof(float), true, false);
        adapted->SetName("Adapted Luminance");
        org::memory::SetResourceUsageHint(*adapted, "Post-Processing resources");
        graph.RegisterResource(Builtin::PostProcessing::AdaptedLuminance, adapted);
        auto histogram = CreateIndexedStructuredBuffer(256, sizeof(uint32_t), true, false);
        histogram->SetName("Luminance Histogram Buffer");
        org::memory::SetResourceUsageHint(*histogram, "Post-Processing resources");
        graph.RegisterResource(Builtin::PostProcessing::LuminanceHistogram, histogram);
        // The luminance average writes the tonemapping curve into the constants Overlay's tonemapping reads.
        if (in.lpmConstants) graph.RegisterResource(org::ResourceIdentifier{ "FFX::LPMConstants" }, in.lpmConstants);
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
        // Ray Reconstruction's guides are the frame interface's (the path tracer writes them in Shading).
        if (in.upscalingMode == UpscalingMode::DLSSRayReconstruction && !in.frameInterface.rayReconstructionGuides)
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
        // The output size's texture, which Overlay's tonemapping reads.
		BuildBloomPipeline(&graph, in.resolution ? in.resolution->bloom : nullptr);
        break;
    // Tonemapping and the debug resolve are Overlay's (DeclareOverlayPasses).
    case DepthHistory:
        BuildLinearDepthHistoryCopyPass(&graph, &m_depthHistory);
        break;
    default:
        break;
    }
}

void Renderer::DeclareOverlayPasses(const br::render::FramePassDeclaration& d, br::pipeline::TechniqueId id) {
    BT_ZONE_SCOPE("Renderer::DeclareOverlayPasses");
    auto& graph = d.graph;
    const auto& in = d.in;
    const auto& recipe = d.recipe;
    using enum br::pipeline::TechniqueId;
    switch (id) {
    case Tonemapping: {
        // In Overlay since M6 step 3 (D3): it writes PresentationColor. It reads Post's upscaled colour, the LPM constants Post's
        // exposure wrote, and Post's bloom texture: the same objects in both graphs (the core provider's, FrameBuildInputs's,
        // ResolutionObjects's).
        const bool bloom = recipe.Contains<br::pipeline::BloomTechnique>();
        if (in.lpmConstants) graph.RegisterResource(org::ResourceIdentifier{ "FFX::LPMConstants" }, in.lpmConstants);
        if (bloom) {
            if (!in.resolution || !in.resolution->bloom)
                throw std::logic_error("DeclareOverlayPasses: tonemapping reads bloom, but the build has no bloom texture of its output size");
            graph.RegisterResource(Builtin::PostProcessing::BloomTexture, in.resolution->bloom);
        }
        graph.BuildPass<TonemappingPass>("TonemappingPass", bloom, in.lpmConstants);
        graph.SetPassTechnique("TonemappingPass", "Post Process::Tonemapping");
        break;
    }
    case DebugUi:
        graph.BuildPass<MenuRenderPass>("MenuRenderPass");
        graph.SetPassTechnique("MenuRenderPass", "Debug::UI");
        break;
    case Present:
        graph.BuildPass<PresentationReadyPass>("PresentationReadyPass");
        graph.SetPassTechnique("PresentationReadyPass", "Frame::Present");
        graph.SetPresentPass("PresentationReadyPass");
        break;
    case DebugOutput:
        // The debug resolve (in Overlay since M6 step 3, as tonemapping), or the skeleton view that replaces it, then the bounding
        // spheres.
        if (!DebugSkeletonsShown()) {
            graph.BuildPass<DebugResolvePass>("DebugResolvePass");
            graph.SetPassTechnique("DebugResolvePass", "Debug::Visualization");
        }
        if (DebugSkeletonsShown()) {
            graph.BuildPass<DebugSkeletonPass>("DebugSkeletonPass", in.debugSnapshots);
            graph.SetPassTechnique("DebugSkeletonPass", "Debug::Visualization");
        }
        if (in.drawBoundingSpheres) {
            graph.BuildPass<DebugSpherePass>("DebugSpherePass", in.debugSnapshots);
            graph.SetPassTechnique("DebugSpherePass", "Debug::Visualization");
        }
        break;
    default:
        break;
    }
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
    // The recipe the build was started with (the render thread may have replaced the renderer's since).
    if (!in.recipe) throw std::logic_error("DeclareFramePasses: the frame build has no recipe");
    const br::pipeline::PipelineRecipe& recipe = *in.recipe;
    const br::render::FramePassDeclaration declaration{ graph, *frameBuild, in, recipe };
    auto& passEpochs = *frameBuild->passEpochs;
    // The host epoch this graph is built for (M4 step 5): it declares only the frame epochs that run in it. kNoFrameEpoch (the
    // in-place graph): every epoch.
    const uint32_t hostEpoch = graph.PassEpochFilter();
    // Runs one frame epoch's declarations, with the epoch current (ScopedDeclarationEpoch), and tags the passes they built
    // with it. An epoch the build does not declare, or that another host epoch's graph holds, is skipped.
    const auto declareIn = [&](uint32_t epoch, const auto& declare) {
        if (!frameBuild->Declares(epoch)) return;
        if (hostEpoch != br::render::kNoFrameEpoch && br::render::HostEpochOf(epoch) != hostEpoch) return;
        const size_t first = graph.PassBuilderCount();
        {
            br::render::ScopedDeclarationEpoch scope(epoch);
            declare();
        }
        for (const auto& name : graph.PassBuilderNamesFrom(first)) passEpochs.Tag(name, epoch);
    };

    declareIn(br::render::kVisibilityEpoch, [&] {
        // The primary camera is high-frequency frame data, not renderer-state
        // publication. Insert its graphics-queue upload before all pipeline passes;
        // their camera SRV declarations establish the required RAW dependencies.
        graph.BuildPass<br::render::PrimaryCameraUploadPass>(
            "PrimaryCameraUploadPass", in.cameraBuffer,
            in.cullingCameraBuffer, m_numFramesInFlight);

        auto& materialEvaluationInputs = frameBuild->Outputs(br::render::kVisibilityEpoch).materialEvaluationInputs;
        materialEvaluationInputs = {};
        materialEvaluationInputs.pipelines = &PSOManager::GetInstance();
        materialEvaluationInputs.commandSignatures = &CommandSignatureManager::GetInstance();
        materialEvaluationInputs.clodSlabResources = in.clodSlabs;
        // The opaque CLod variant put its buffers in the build's Visibility outputs in its PrepareForBuild, which runs before this.
        const auto& clodPrimary = frameBuild->Outputs(br::render::kVisibilityEpoch).clodPrimary;
        materialEvaluationInputs.visibleClusters = clodPrimary.visibleClusters;
        materialEvaluationInputs.visibleClusterCapacity = clodPrimary.visibleClusterCapacity;
        materialEvaluationInputs.visibleClusterCounter = clodPrimary.visibleClusterCounter;
        materialEvaluationInputs.visibleClusterTransformIndices = clodPrimary.visibleClusterTransformIndices;
        materialEvaluationInputs.reyesDiceQueue = clodPrimary.reyesDiceQueue;
        materialEvaluationInputs.reyesTessTableConfigs = clodPrimary.reyesTessTableConfigs;
        materialEvaluationInputs.reyesTessTableVertices = clodPrimary.reyesTessTableVertices;
        materialEvaluationInputs.reyesTessTableTriangles = clodPrimary.reyesTessTableTriangles;
    });

    // In recipe order, so the passes are declared in the order they always were: each technique's declarations by its
    // epoch's function.
    br::pipeline::PipelineBuildContext buildContext(
        graph,
        recipe.Bindings(),
        [&](br::pipeline::TechniqueId id, const br::pipeline::TechniqueOptions&) {
            // Registrations every epoch's passes read (the frame resources, the BRDF table, the environment, the ray
            // tracing scene): made in every graph. They build no pass.
            if (IsSharedRegistrationTechnique(id)) {
                DeclareVisibilityPasses(declaration, id);
                return;
            }
            switch (FrameEpochOfTechnique(id)) {
            case br::render::kVisibilityEpoch:
                declareIn(br::render::kVisibilityEpoch, [&] { DeclareVisibilityPasses(declaration, id); });
                break;
            case br::render::kShadingEpoch:
                declareIn(br::render::kShadingEpoch, [&] { DeclareShadingPasses(declaration, id); });
                break;
            case br::render::kPostEpoch:
                declareIn(br::render::kPostEpoch, [&] { DeclarePostPasses(declaration, id); });
                break;
            case br::render::kOverlayEpoch:
                declareIn(br::render::kOverlayEpoch, [&] { DeclareOverlayPasses(declaration, id); });
                break;
            default:
                throw std::logic_error("DeclareFramePasses: technique " + std::to_string(static_cast<uint32_t>(id)) + " has no frame epoch");
            }
        });

    {
    BT_ZONE_SCOPE("Renderer::DeclareFramePasses::BuildTechniques");
	for (const auto& entry : recipe.Techniques()) {
		entry.technique->Build(buildContext);
		probeGraphBuildPhase(("DeclareFramePasses after technique " + std::to_string(static_cast<uint32_t>(entry.id))).c_str());
	}
    }
    // Terrain RVT's persistent atlases and tables, which Visibility's RVT passes register and Shading's ray traced passes
    // (reflections, the path tracer) sample: the same objects in a graph without Visibility. This build's Visibility outputs
    // hold the renderer's (MakeFrameBuild), with what a Visibility declaration of this build added.
    if (hostEpoch != br::render::kNoFrameEpoch && hostEpoch != br::render::HostEpochOf(br::render::kVisibilityEpoch)
        && recipe.Contains<br::pipeline::TerrainRvtTechnique>()) {
        for (const auto& [name, resource] : frameBuild->Outputs(br::render::kVisibilityEpoch).terrainRvtResources)
            graph.RegisterResource(name, resource);
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
    // The size the swapchain got, which a Vulkan surface decides (its extent is the window's): the output resolution, so the first
    // present is no mode change (M5 step 8, decision D3).
    if (uint32_t width = 0, height = 0; m_swapChain && m_swapChain->Extent(width, height) && width && height && (width != x_res || height != y_res)) {
        spdlog::info("Renderer: the swapchain is {}x{}, not the requested {}x{}; that is the output resolution", width, height, x_res, y_res);
        x_res = width;
        y_res = height;
        SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("outputResolution")({ x_res, y_res });
    }

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
    // Each extension's frame epoch (the M4 design's section 2). The IO extension's only work is its after-execution
    // readback; readback captures take the epoch of the pass each follows.
    auto& epochs = in.extensionEpochs;
    // Its readback copies the texture streaming feedback, which material evaluation (Visibility) and the CLod alpha variant
    // (Shading) write: Shading's graph, after which it runs.
    ids.push_back("BuiltinIO");
    epochs["BuiltinIO"] = br::render::kShadingEpoch;
    makers["BuiltinIO"] = [io = m_frameIoService]() -> ExtensionPtr { return std::make_unique<RenderGraphIOExtension>(io); };

    auto& persistent = *m_producerPersistentState;
    if (!persistent.clodStreaming) {
        persistent.clodStreaming = std::make_shared<CLodStreamingSystem>();
        persistent.clodStreaming->SetGeometryStorage(&m_pMeshManager->GetCLodGeometryStorage());
    }
    if (!persistent.virtualShadowInvalidations) persistent.virtualShadowInvalidations = std::make_shared<VirtualShadowInvalidationQueue>();
    // This build's casters, over the persistent queue: a caster extension joins it when it is made.
    auto& visibilityOutputs = build.Outputs(br::render::kVisibilityEpoch);
    visibilityOutputs.virtualShadowCasters = std::make_shared<VirtualShadowCasterRegistry>(persistent.virtualShadowInvalidations);

    // Recipe extensions may register resources consumed by technique extensions (ProceduralWind visibility is consumed by CLod
    // shadow traversal). Register producers first; structural insertion constraints are resolved only after every extension
    // has been gathered, so this does not require their target passes to have been materialized yet.
    for (const auto& [id, factory] : m_pipelineRecipe.Extensions()) {
        ids.push_back(id);
        epochs[id] = m_pipelineRecipe.ExtensionEpoch(id);
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
                if (!current || !current->Outputs(br::render::kVisibilityEpoch).virtualShadowCasters) {
                    throw std::logic_error("Pipeline extension " + id + " is a shadow caster made outside a frame build");
                }
                current->Outputs(br::render::kVisibilityEpoch).virtualShadowCasters->Register(*caster);
            }
            return made;
        };
    }

    // The technique extensions, in a fixed order: a technique the recipe lacks has no maker.
    for (const char* id : { "CLodOpaque", "CLodRayTracing", "CLodAlpha", "CLodShadow", "StochasticLighting" }) ids.push_back(id);
    epochs["CLodOpaque"] = epochs["CLodRayTracing"] = epochs["CLodShadow"] = br::render::kVisibilityEpoch;
    epochs["CLodAlpha"] = epochs["StochasticLighting"] = br::render::kShadingEpoch;
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
                // The other epochs' graphs register the same objects for their receivers (DeclareFrameProviders).
                in.directionalVsm = directionalVsm;
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
                .virtualShadowCasters = visibilityOutputs.virtualShadowCasters,
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
                    .virtualShadowCasters = visibilityOutputs.virtualShadowCasters,
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
    // CLod streaming's frame tail, in the last graph that writes the feedback: Shading's (the opaque variant, Visibility's, owns
    // streaming; the alpha variant, Shading's, writes feedback too), after every CLod variant's passes of its graph.
    ids.push_back("CLodStreamingTail");
    epochs["CLodStreamingTail"] = br::render::kShadingEpoch;
    if (makers.contains("CLodOpaque")) {
        makers["CLodStreamingTail"] = [streaming]() -> ExtensionPtr { return std::make_unique<CLodStreamingTailExtension>(streaming); };
    }
    // Readback captures after a named pass are copy passes this extension places after their anchors. The capture sites
    // persist in m_readbackCaptures; each graph gets its own extension over them, and keeps the captures whose anchor is one of
    // its passes (EpochTaggedExtension). Last, so the anchors among the other extensions' passes are tagged when it gathers.
    ids.push_back("ReadbackCaptures");
    epochs["ReadbackCaptures"] = br::render::kNoFrameEpoch;
    if (m_readbackCaptures) {
        makers["ReadbackCaptures"] = [captures = m_readbackCaptures]() -> ExtensionPtr { return captures->CreateExtension(); };
    }
}

void Renderer::RegisterFrameHostExtensions(const std::vector<std::string>& extensionIds,
    const std::unordered_map<std::string, uint32_t>& extensionEpochs) {
    if (!m_frameHost) throw std::logic_error("RegisterFrameHostExtensions: no frame graph host");
    // Build order: the renderer's providers, then the pipeline extensions (their PrepareForBuild reads the providers and
    // registers what the renderer's passes read), then the renderer's passes.
    std::vector<std::string> ids;
    ids.reserve(extensionIds.size() + 2u);
    ids.push_back("BasicRenderer::Providers");
    ids.insert(ids.end(), extensionIds.begin(), extensionIds.end());
    ids.push_back("BasicRenderer::Passes");
    // The host epochs each extension is made into, from its frame epoch. The renderer's providers and passes and the readback
    // captures (kNoFrameEpoch: each graph keeps the captures after its own passes) are made into every frame host epoch.
    const auto hostEpochsOf = [&](const std::string& id) -> std::vector<uint32_t> {
        if (id == "BasicRenderer::Providers" || id == "BasicRenderer::Passes") return br::render::FrameHostEpochs();
        const auto found = extensionEpochs.find(id);
        const uint32_t epoch = found == extensionEpochs.end() ? br::render::kVisibilityEpoch : found->second;
        if (!br::render::IsFrameEpoch(epoch)) return br::render::FrameHostEpochs();
        return { br::render::HostEpochOf(epoch) };
    };
    std::vector<std::vector<uint32_t>> hostEpochs;
    for (const auto& id : ids) hostEpochs.push_back(hostEpochsOf(id));
    if (ids == m_frameHostExtensionIds && hostEpochs == m_frameHostExtensionEpochs) return;

    for (const auto& id : m_frameHostExtensionIds) m_frameHost->RemoveExtension(id);
    m_frameHostExtensionIds.clear();
    m_frameHostExtensionEpochs.clear();
    const auto add = [&, this](std::string id, org::GraphHost::ExtensionFactory factory) {
        // Frame epochs only: the upload epoch's graph is the host's upload pass alone.
        auto epochs = hostEpochsOf(id);
        m_frameHost->AddExtension(id, std::move(factory), epochs);
        m_frameHostExtensionIds.push_back(std::move(id));
        m_frameHostExtensionEpochs.push_back(std::move(epochs));
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
            auto made = maker->second();
            if (!made) return nullptr;
            // Its passes are tagged with its frame epoch as it gathers them.
            return std::make_unique<br::render::EpochTaggedExtension>(std::move(made), build->inputs.ExtensionEpoch(id), build->passEpochs);
        });
    }
    // It also gathers the passes an earlier host epoch's graph of the build forwarded to its graph.
    add("BasicRenderer::Passes", [this] {
        return std::make_unique<br::render::DeclarationGraphExtension>([this](org::RenderGraph& graph) { DeclareFramePasses(graph); },
            br::render::DeclarationGraphExtension::Declare{}, br::render::GatherForwardedPasses);
    });
}
