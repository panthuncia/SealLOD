//
// Created by matth on 6/25/2024.
//

#include <BasicRenderer/Renderer.h>
#include "Runtime/StateGraph/AsyncStateGraph.h"
#include <spdlog/spdlog.h>
#include <rhi_debug.h>
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "Runtime/Resources/ResourceManager.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "Runtime/IO/DirectStorageManager.h"
#include "Scene/Objects/IndirectCommandBufferManager.h"
#include "Runtime/Device/DescriptorHeapManager.h"
#include "Pipeline/PipelineState/CommandSignatureManager.h"
#include "Runtime/IO/ReadbackManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Animation/Skeletons/SkeletonManager.h"
#include "Assets/Textures/TextureFactory.h"
#include "Lighting/Environment/EnvironmentManager.h"
#include "Lighting/Lights/LightManager.h"
#include "Materials/MaterialManager.h"
#include "Materials/TextureStreaming/TextureStreamingManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Diagnostics/Menu/Menu.h"
#include "PostProcessing/FidelityFX/FFXManager.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "Scene/Objects/ObjectManager.h"
#include "Scene/ECS/RendererECSManager.h"
#include "Scene/Views/ViewManager.h"
#include "Terrain/Residency/TerrainManager.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "Runtime/Device/DeletionManager.h"
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"
#include "Runtime/Publication/PersistentRendererPublication.h"
#include "Runtime/GraphIntegration/StateProducerRegistrations.h"
#include "Materials/TextureStreaming/TextureBindingArtifacts.h"
#include "Terrain/Residency/TerrainStateArtifacts.h"
#include "VirtualGeometry/Streaming/Publication/CLodResidencyStorageArtifacts.h"
#include "VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.h"
#include "Runtime/Publication/StaticStateArtifacts.h"
#include <BasicRenderer/Streaming/StaticSceneArtifacts.h>
#include "VirtualGeometry/Streaming/Publication/GeometryResidencyStateArtifacts.h"
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include "Resources/Resource.h"
#include "Runtime/Scheduling/TbbTaskService.h"
#include <BasicRenderer/Streaming/RendererStateRequestService.h>

void Renderer::SignalFence(rhi::Queue commandQueue, uint8_t frameIndexToSignal) {
    // Signal the fence
    const UINT64 nextFrameFenceValue = m_currentFrameFenceValue + 1;
	const rhi::Result signalResult = commandQueue.Signal({ m_frameFence->GetHandle(), nextFrameFenceValue });
    if (signalResult != rhi::Result::Ok) {
        spdlog::error(
            "Renderer::SignalFence failed frameIndex={} target={} current={} completed={} result={}",
            frameIndexToSignal,
            nextFrameFenceValue,
            m_currentFrameFenceValue,
            m_frameFence ? m_frameFence->GetCompletedValue() : 0u,
            rhi::ResultName(signalResult));
        return;
    }
    m_currentFrameFenceValue = nextFrameFenceValue;

    // Store the fence value for the current frame
    m_frameFenceValues[frameIndexToSignal] = m_currentFrameFenceValue;
    spdlog::debug(
        "Renderer::SignalFence queued frameIndex={} target={} completed={}",
        frameIndexToSignal,
        m_currentFrameFenceValue,
        m_frameFence ? m_frameFence->GetCompletedValue() : 0u);
}

void Renderer::AdvanceFrameIndex() {
    if (m_swapChain) {
        m_frameIndex = static_cast<uint8_t>(m_swapChain->CurrentImageIndex());
    } else {
        m_frameIndex = (m_frameIndex + 1) % m_numFramesInFlight;
    }
    m_totalFramesRendered += 1;
}

void Renderer::StallPipeline() {
    for (uint8_t i = 0; i < m_numFramesInFlight; ++i) {
        WaitForFrame(i);
    }
    auto& devices = DeviceManager::GetInstance();
    spdlog::info("Renderer::StallPipeline waiting for all initialized devices idle");
    if (rhi::Failed(devices.GetDevice().WaitIdle()))
        throw std::runtime_error("Primary device idle wait failed; GPU ownership must be retained");
    if (devices.IsMultiRHIEnabled()) {
        if (rhi::Failed(devices.GetPeerDevice().WaitIdle()))
            throw std::runtime_error("Peer device idle wait failed; GPU ownership must be retained");
    }
    spdlog::info("Renderer::StallPipeline all initialized devices idle complete");
}

void Renderer::Cleanup() {
    spdlog::info("In cleanup");
    auto retiringDescriptors = currentRenderGraph ? currentRenderGraph->RetainDescriptorService() : nullptr;
	auto retiringIngestionGeneration = m_sceneIngestionServices.execution.generation;
    if (currentRenderGraph) currentRenderGraph->StopFrameProduction();
    // Wait for all GPU frames to complete
	spdlog::info("Stalling pipeline for cleanup");
	StallPipeline();
	// Extensions own graph resources and bridge objects.  Do not let them
	// release those objects until work on both API devices has completed.
    if (currentRenderGraph) {
        currentRenderGraph->ShutdownExtensions();
    }
	if (currentScene) {
		currentScene->Deactivate();
	}
	ClearExternalSnapshotMeshRegistrations();
	m_sceneRenderBridge.Clear(GetSceneIngestionServices());
	m_sceneIngestionServices = {};
	m_renderSyncObjectQuery = {};
	m_renderSyncCameraQuery = {};
	m_renderSyncLightQuery = {};
	m_renderTransformUpdatedCleanupQuery = {};
	m_renderSyncQueriesBuilt = false;
	if (m_pMaterialManager) {
		m_pMaterialManager->ShutdownTextureStreaming();
	}
	spdlog::info("Cleaning up resources");
    // Desired-state jobs borrow the request service. Its Stop flag cannot be
    // read safely after destruction: join the producer before closing/freeing
    // that service (ASAN caught SubmitLatest racing the old reset order).
    if (m_pIndirectCommandBufferManager) {
        m_pIndirectCommandBufferManager->Shutdown();
    }
    // Close the renderer-state request boundary before any upload/descriptor
    // service it can target is destroyed. CancelAndWait also prevents a late
    // producer completion from publishing into manager teardown.
    m_sceneIngestionServices.execution.stateRequests = nullptr;
	if (m_pMeshManager) m_pMeshManager->SetRendererStateRequestService(nullptr);
    if (m_rendererStateCommitScope.Valid()) {
        m_rendererStateCommitScope.CancelAndWait();
        m_rendererStateCommitScope = {};
    }
    if (m_presentationTailScope.Valid()) {
        m_presentationTailScope.CancelAndWait();
        m_presentationTailScope = {};
    }
    if (m_rendererStateRequests) {
        m_rendererStateRequests->Stop();
        m_rendererStateRequests.reset();
    }
    if (m_asyncStateGraph) {
        m_asyncStateGraph->Shutdown();
        m_asyncStateGraph.reset();
    }
	// Static-scene clients share this generation cell with the renderer. Clear it
	// only after every request producer has joined, so late admission observes a
	// closed service boundary and old backend generations can be destroyed.
	if (retiringIngestionGeneration) {
		retiringIngestionGeneration->uploads.store({}, std::memory_order_release);
		retiringIngestionGeneration->descriptors.store({}, std::memory_order_release);
	}
    if (m_rendererStatePublisher) {
        m_context.publishedRendererState.reset();
        m_context.publishedManifestLease.reset();
        br::render::PublishedStateSource::SetProcessSource({});
        m_rendererStatePublisher->Shutdown();
        m_rendererStatePublisher.reset();
    }
    // Version families retain their latest immutable GPU backing to seed a
    // successor request.  Producer shutdown above guarantees there can be no
    // more requests, so release those device-bound roots before descriptor and
    // allocator teardown rather than waiting for Renderer destruction.
    for (auto& family : m_lightTableFamilies) family.reset();
    for (auto& family : m_poseTableFamilies) family.reset();
    if (currentRenderGraph) {
        // Publication and texture-streaming producers have now stopped. Cover
        // submissions made after the first wait before cleaning their services.
        StallPipeline();
        if (auto* uploadService = currentRenderGraph->GetUploadService()) {
            uploadService->Cleanup();
        }
        if (m_uploadPolicyService) {
            m_uploadPolicyService->Cleanup();
        }
        if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
            readbackService->Cleanup();
        }
    }
    if (currentRenderGraph) {
        currentRenderGraph->ShutdownTaskWorkers();
    }
    // Readback reservations are graph users. Join or cancel those users before
    // closing admission and detaching the shared request state.
    if (m_pReadbackManager) {
        m_pReadbackManager->Cleanup();
    }
    org::SetAsyncBufferBackingResizeScheduler({});
    TaskSchedulerManager::GetInstance().Cleanup();
    ::ResourceManager::GetInstance().Cleanup();
    m_coreResourceProvider.Cleanup();
    currentRenderGraph.reset();
    // Cleanup tears down the device. Preserve the state container so a module
    // owner can replay CPU scene metadata, but discard all device-bound state.
    m_producerPersistentState->InvalidateTerrainRvt();
    m_producerPersistentState->directionalVsm.InvalidateGpuState();
    m_producerPersistentState->virtualShadowCasters.reset();
    m_producerPersistentState->clodStreaming.reset();
    org::runtime::SetActiveUploadPolicyService(nullptr);
    m_uploadPolicyService.reset();
    m_renderGraphRuntimeInitialized = false;
    m_currentEnvironment.reset();
    m_defaultEnvironmentCubemap.reset();
    m_defaultEnvironmentPrefilteredCubemap.reset();
	currentScene.reset();
	m_sceneEntityMaterializationService.Configure(nullptr, nullptr, nullptr, nullptr);
	m_poseInstanceRegistrationService.Configure(nullptr);
	m_sceneRenderableResidencyService.Configure(nullptr, nullptr);
	m_pIndirectCommandBufferManager.reset();
	m_depthHistory.Clear();
	m_pViewManager.reset();
	m_pLightManager.reset();
	// The object manager reads the mesh manager's geometry sequence on commit.
	if (m_pObjectManager) m_pObjectManager->SetGeometryCoverageSource({});
	m_pMeshManager.reset();
	m_pObjectManager.reset();
    m_pMaterialManager.reset();
    m_pEnvironmentManager.reset();
	m_pTerrainManager.reset();
	m_pSkeletonManager.reset();
    m_pReadbackManager.reset();
    m_pTextureFactory.reset();
    m_clodRayTracingSystem.reset();
    m_context = {};
    m_frameInputs.reset();
    m_materialEvaluationInputs = {};
    m_openPBRLookupResources = {};
    m_blueNoiseTexture.reset();
	ReleaseSharedProcessingPlaceholderTextures();
    m_dynamicBackbuffer.reset();
    m_dynamicPresentationColor.reset();
    m_presentationColorResources.clear();
    m_backbufferResources.clear();
    renderTargets.clear();
    m_commandLists.clear();
    m_commandAllocators.clear();
    m_frameFence.Reset();
    m_readbackFence.Reset();
    m_copyReadbackFence.Reset();
    m_legacyReadbackFence.Reset();
    m_frameFenceValues.clear();
    rtvHeap.Reset();
    m_settingsSubscriptions.clear();
    m_warnedUsingFallbackEnvironment = false;
    m_warnedNullScene = false;
    m_warnedMissingPrimaryCamera = false;
	spdlog::info("Cleaning up singletons");
    Material::DestroyDefaultMaterial();
    Menu::GetInstance().Cleanup();
    CommandSignatureManager::GetInstance().Cleanup();
    PSOManager::GetInstance().Cleanup();
	FFXManager::GetInstance().Shutdown();
	UpscalingManager::GetInstance().Shutdown();
    RendererECSManager::GetInstance().FlushDeferredWorldOperations();
    org::TrackedEntityToken::ResetHooks();
    org::Resource::ResetEntityHooks();
    // The ingestion source store borrows the renderer ECS world. Release that
    // association before the singleton destroys the world so late availability
    // checks cannot observe a dangling source boundary.
    m_sceneSourceStateStore.Reset();
    RendererECSManager::GetInstance().Cleanup();
	// ECS components and manager destructors can retire resources. Keep the
	// runtime descriptor/backing services alive through their final release.
    if (retiringDescriptors) retiringDescriptors->Cleanup();
    retiringDescriptors.reset();
	org::RenderGraph::ShutdownRuntime();
	spdlog::info("Cleaning up swap chain");
    m_swapChain.Reset();
	spdlog::info("Cleaning up device manager");
    DirectStorageManager::GetInstance().Cleanup();
    DeviceManager::GetInstance().Cleanup();
	spdlog::info("Cleanup complete");
}

void Renderer::Initialize(
    HWND hwnd,
    UINT x_res,
    UINT y_res,
    br::pipeline::PipelineRecipe recipe) {
    const auto validation = recipe.Validate();
    if (!validation.valid) {
        std::string message = "Invalid renderer pipeline recipe:";
        for (const auto& error : validation.errors) {
            message += "\n - " + error;
        }
        throw std::invalid_argument(message);
    }
    m_pipelineRecipe = std::move(recipe);
    m_pipelineExtensionsDirty = true;
    m_gtaoEnabled = m_pipelineRecipe.Contains<br::pipeline::GtaoTechnique>();
    m_clusteredLighting = m_pipelineRecipe.Contains<br::pipeline::ClusteredLightingTechnique>();
    m_bloom = m_pipelineRecipe.Contains<br::pipeline::BloomTechnique>();
#if defined(_DEBUG)
    if (!m_pipelineReplacementDebugBreakHandler) {
        m_pipelineReplacementDebugBreakHandler = [] { __debugbreak(); };
    }
#endif
    org::BufferBase::ScopedBackingMutation initializationBackingMutation;
    m_hwnd = hwnd;

    auto& settingsManager = SettingsManager::GetInstance();
    const bool enableStreamline = !br::runtime::renderer_settings::IsStreamlineDisabledByEnvironment();
    const bool enableDirectStorage = !br::runtime::renderer_settings::IsDirectStorageDisabledByEnvironment();
    settingsManager.registerSetting<uint8_t>("numFramesInFlight", m_numFramesInFlight);
    getNumFramesInFlight = settingsManager.getSettingGetter<uint8_t>("numFramesInFlight");
    settingsManager.registerSetting<rhi::Backend>("rhiBackend", rhi::Backend::D3D12);
    settingsManager.registerSetting<DirectX::XMUINT2>("renderResolution", { x_res, y_res });
    settingsManager.registerSetting<DirectX::XMUINT2>("outputResolution", { x_res, y_res });
    settingsManager.registerSetting<WindowResolutionPreset>(
        WindowResolutionPresetSettingName,
        FindClosestWindowResolutionPreset(x_res, y_res));
    settingsManager.registerSetting<UpscalingMode>(
        "upscalingMode",
        enableStreamline ? UpscalingMode::DLSS : UpscalingMode::None);
    settingsManager.registerSetting<UpscaleQualityMode>("upscalingQualityMode", UpscaleQualityMode::DLAA);
    settingsManager.registerSetting<bool>("enableDilatedMotionVectors", true);
    settingsManager.registerSetting<bool>("enableVisibilityRendering", m_visibilityRendering);
    settingsManager.registerSetting<bool>("enableStreamline", enableStreamline);
    settingsManager.registerSetting<bool>("enableDirectStorage", enableDirectStorage);
    settingsManager.registerSetting<bool>("enableReShape", br::runtime::renderer_settings::DefaultEnableReShapeForBuild());
    settingsManager.registerSetting<bool>("reshapeSynchronousRecording", false);
    settingsManager.registerSetting<bool>("reshapeTexelAddressing", true);
    settingsManager.registerSetting<uint64_t>("reshapeGlobalFeatureMask", 0ull);
    settingsManager.registerSetting<bool>(
        "renderGraphBatchTraceEnabled",
        br::runtime::renderer_settings::ReadTruthyEnvironmentFlag("BASICRENDERER_RENDER_GRAPH_BATCH_TRACE"));
    settingsManager.registerSetting<bool>("renderGraphLightweightCompileSummaryEnabled", false);
    settingsManager.registerSetting<int>("experimentalAsyncCompileMode", 0);
    settingsManager.registerSetting<int>("experimentalCompileConcurrency", 2);
    if (const auto* mode = std::getenv("SARP_ASYNC_COMPILE_MODE")) {
        const std::string_view value(mode);
        const int parsed = value == "Async" || value == "async" || value == "2" ? 2 : 0;
        settingsManager.getSettingSetter<int>("experimentalAsyncCompileMode")(parsed);
        spdlog::info("Experimental async compile mode requested through environment: '{}' ({})",
            value, parsed);
    }
    LoadPipeline(hwnd, x_res, y_res);
    DirectStorageManager::GetInstance().Initialize();
    ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), "after LoadPipeline");
    UpscalingManager::GetInstance().InitSL();
    SetSettings();
    SyncOpenRenderGraphSettings(m_numFramesInFlight);
    RendererECSManager::GetInstance().Initialize();
    org::TrackedEntityToken::Hooks trackedEntityHooks{};
    trackedEntityHooks.createEntity = [](flecs::entity existing) {
            auto& ecsManager = RendererECSManager::GetInstance();
            if (!ecsManager.IsAlive()) {
                return org::TrackedEntityToken{};
            }

            org::TrackedEntityToken token = org::TrackedEntityToken::CreateDeferred();
            const flecs::entity_t existingId = existing.id();
            auto deferredState = token.deferredState;
            ecsManager.EnqueueDeferredWorldOperation([deferredState, existingId](flecs::world& world) mutable {
                std::vector<std::function<void(flecs::entity)>> pendingOps;
                bool destroyRequested = false;
                flecs::entity entity = existingId != 0 ? flecs::entity{ world, existingId } : flecs::entity{};
                if (!entity.is_alive()) {
                    entity = world.entity();
                }

                if (!org::TrackedEntityToken::ResolveDeferredState(
                    deferredState,
                    world,
                    entity.id(),
                    pendingOps,
                    destroyRequested)) {
                    if (entity.is_alive()) {
                        entity.destruct();
                    }
                    org::TrackedEntityToken::MarkDeferredStateDestroyed(deferredState);
                    return;
                }

                for (auto& op : pendingOps) {
                    op(entity);
                }

                if (destroyRequested && entity.is_alive()) {
                    entity.destruct();
                    org::TrackedEntityToken::MarkDeferredStateDestroyed(deferredState);
                }
            });
            return std::move(token);
        };
    trackedEntityHooks.isRuntimeAlive = []() {
        return RendererECSManager::GetInstance().IsAlive();
    };
    trackedEntityHooks.isMainThread = []() {
        return RendererECSManager::GetInstance().IsMainThread();
    };
    trackedEntityHooks.enqueueAttachBundle = [](flecs::entity_t id, org::EntityComponentBundle bundle) {
        auto& ecsManager = RendererECSManager::GetInstance();
        if (!ecsManager.IsAlive()) {
            return;
        }

        ecsManager.EnqueueDeferredWorldOperation([id, bundle = std::move(bundle)](flecs::world& world) mutable {
            flecs::entity entity{ world, id };
            if (entity.is_alive()) {
                bundle.ApplyTo(entity);
            }
        });
    };
    trackedEntityHooks.destroyEntity = [](flecs::world& world, flecs::entity_t id) {
        auto& ecsManager = RendererECSManager::GetInstance();
        if (!ecsManager.IsAlive()) {
            return;
        }

        if (!ecsManager.IsMainThread()) {
            ecsManager.EnqueueDeferredWorldOperation([id](flecs::world& deferredWorld) {
                flecs::entity entity{ deferredWorld, id };
                if (entity.is_alive()) {
                    entity.destruct();
                }
            });
            return;
        }

        flecs::entity entity{ world, id };
        if (entity.is_alive()) {
            entity.destruct();
        }
    };
    org::TrackedEntityToken::SetHooks(std::move(trackedEntityHooks));

    org::Resource::ECSEntityHooks resourceEntityHooks{};
    resourceEntityHooks.createEntity = []() -> org::Resource::ECSEntityHandle {
		auto& ecsManager = RendererECSManager::GetInstance();
		if (!ecsManager.IsAlive()) {
			return {};
		}

		if (!ecsManager.IsMainThread()) {
			auto handle = org::Resource::ECSEntityHandle::CreateDeferred();
                auto deferredState = handle.deferredState;
                ecsManager.EnqueueDeferredWorldOperation([deferredState](flecs::world& world) mutable {
					flecs::entity entity = world.entity();
                    org::Resource::ECSEntityHandle deferredHandle;
                    deferredHandle.deferredState = deferredState;
                    if (!deferredHandle.Resolve(world, entity.id())) {
                        deferredHandle.MarkDestroyed();
					}
				});
                return std::move(handle);
        }

        auto& world = RendererECSManager::GetInstance().GetWorld();
        auto entity = world.entity();
        org::Resource::ECSEntityHandle handle{};
        handle.world = &world;
        handle.id = entity.id();
        return handle;
    };
    resourceEntityHooks.destroyEntity = [](const org::Resource::ECSEntityHandle& handle) {
        auto& ecsManager = RendererECSManager::GetInstance();
        if (!ecsManager.IsAlive()) {
            return;
        }

        if (ecsManager.IsMainThread()) {
            flecs::world* world = nullptr;
            flecs::entity_t id = 0;
            if (handle.TryGetResolved(world, id)) {
                flecs::entity entity{ *world, id };
                if (entity.is_alive()) {
                    entity.destruct();
                }
            }
            handle.MarkDestroyed();
            return;
        }

        handle.RequestDestroy();
            auto deferredState = handle.deferredState;
            ecsManager.EnqueueDeferredWorldOperation([deferredState](flecs::world&) mutable {
                org::Resource::ECSEntityHandle deferredHandle;
                deferredHandle.deferredState = deferredState;
				flecs::world* world = nullptr;
				flecs::entity_t id = 0;
                if (!deferredHandle.TryGetResolved(world, id)) {
					return;
				}

				flecs::entity entity{ *world, id };
				if (entity.is_alive()) {
					entity.destruct();
				}
                deferredHandle.MarkDestroyed();
			});
	};
    resourceEntityHooks.isRuntimeAlive = []() {
        return RendererECSManager::GetInstance().IsAlive();
    };
    org::Resource::SetEntityHooks(std::move(resourceEntityHooks));

    if (!currentRenderGraph) {
		currentRenderGraph = std::make_unique<org::RenderGraph>(DeviceManager::GetInstance().GetDevice(), DeviceManager::GetInstance().GetBackend());
		if (DeviceManager::GetInstance().IsMultiRHIEnabled()) {
			currentRenderGraph->RegisterBackendDevice(DeviceManager::GetInstance().GetPeerBackend(), DeviceManager::GetInstance().GetPeerDevice());
		}
    }

    if (auto* uploadService = currentRenderGraph->GetUploadService()) {
            uploadService->Initialize();
            uploadService->SetOwnerThread();
    }
    if (!m_uploadPolicyService) {
        m_uploadPolicyService = org::runtime::CreateDefaultUploadPolicyService(currentRenderGraph->RetainUploadService());
    }
    if (m_uploadPolicyService) {
        m_uploadPolicyService->Initialize();
        org::runtime::SetActiveUploadPolicyService(m_uploadPolicyService.get());
    }
    if (auto* descriptorService = currentRenderGraph->GetDescriptorService()) {
        descriptorService->Initialize();
    }
    ::ResourceManager::GetInstance().Initialize(currentRenderGraph->RetainUploadService());
    TaskSchedulerManager::GetInstance().Initialize();
    m_rendererStateCommitScope = TaskSchedulerManager::GetInstance().CreateScope(
        "RendererStateCommitCleanup");
    m_presentationTailScope = TaskSchedulerManager::GetInstance().CreateScope(
        "RendererPresentationTail");
    m_asyncStateGraph = std::make_unique<br::render::AsyncStateGraph>(
        TaskSchedulerManager::GetInstance(), "RendererStateGraph");
    // The renderer thread observes published leases only; any graph lock it
    // takes is a design regression and is counted (SARP.AsyncStateGraph.OwnerThreadLocks).
    m_asyncStateGraph->SetOwnerThread();
    if (m_pendingAsyncStateGraphTrace) {
        m_asyncStateGraph->StartTrace(*m_pendingAsyncStateGraphTrace);
    }
    m_rendererStatePublisher = std::make_unique<br::render::RendererStatePublisher>(m_numFramesInFlight);
    if (const auto* persistent = std::getenv("SARP_PERSISTENT_PUBLICATIONS"); persistent && persistent[0] == '1') {
        m_rendererStatePublisher->SetExecutablePreparation(br::render::PreparePersistentRendererPublication);
        spdlog::info("Persistent material/geometry publication preparation enabled; pass execution remains transitional");
    }
    m_rendererStatePublisher->SetPreparationScheduler([scope = m_rendererStateCommitScope](std::function<void()>&& work) {
        return TaskSchedulerManager::GetInstance().Submit(scope, TaskLane::Background, TaskDomain::RendererState,
            "PublicationBindingBundles", [work = std::move(work)](const br::TaskContext& context) mutable {
                auto operation = std::move(work);
                if (!context.StopRequested()) operation();
            });
    });
    br::render::PublishedStateSource::SetProcessSource(m_rendererStatePublisher->ResourceSource());
    m_rendererStateRequests = std::make_unique<br::render::RendererStateRequestService>(
        *m_asyncStateGraph, *m_rendererStatePublisher);
    br::render::RegisterIndirectStateProducer(*m_asyncStateGraph);
    br::render::RegisterMaterialStateProducer(*m_asyncStateGraph);
	br::render::RegisterTextureBindingProducer(*m_asyncStateGraph);
	br::render::RegisterTextureDisplayGateProducer(*m_asyncStateGraph);
	br::render::RegisterTerrainStateProducer(*m_asyncStateGraph);
    br::render::RegisterVersionedGpuBufferProducer(*m_asyncStateGraph);
	br::render::RegisterTextureImageTableProducer(*m_asyncStateGraph);
    br::render::RegisterObjectBufferStateProducer(*m_asyncStateGraph);
    br::render::RegisterGeometryCoverageGateProducer(*m_asyncStateGraph);
    br::render::RegisterCLodResidencyStorageProducers(*m_asyncStateGraph);
    br::render::RegisterGeometryBufferStateProducer(*m_asyncStateGraph);
    br::render::RegisterStaticStateProducers(*m_asyncStateGraph);
    br::render::RegisterGeometryResidencyStateProducer(*m_asyncStateGraph);
	br::render::RegisterViewStateProducer(*m_asyncStateGraph);
	br::render::RegisterPoseStateProducer(*m_asyncStateGraph);
	br::render::RegisterLightStateProducer(*m_asyncStateGraph);
    m_asyncStateGraph->SetReadyCallback([this](const br::render::ArtifactSnapshot& artifact) {
        if (m_rendererStateRequests) m_rendererStateRequests->OnArtifactReady(artifact);
    });
    m_rendererStatePublisher->SetCandidateRejectedCallback([this](std::uint64_t epoch) {
        if (m_rendererStateRequests) m_rendererStateRequests->OnCandidateRejected(epoch);
    });
    org::SetAsyncBufferBackingResizeScheduler([](std::string taskName, std::function<void()>&& task) {
        return TaskSchedulerManager::GetInstance().Submit(
            TaskLane::Background, TaskDomain::Cleanup, taskName, std::move(task));
    });
    currentRenderGraph->SetTaskService(std::make_shared<br::TbbTaskService>());
    spdlog::info("Renderer initialization: initializing PSO manager");
    PSOManager::GetInstance().initialize();
    spdlog::info("Renderer initialization: initializing deletion manager");
    org::DeletionManager::GetInstance().Initialize();
	spdlog::info("Renderer initialization: initializing command signatures");
	CommandSignatureManager::GetInstance().Initialize();
    spdlog::info("Renderer initialization: command signatures initialized");
    ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), "after PSO and command signatures");
    spdlog::info("Renderer initialization: initializing menu");
    Menu::GetInstance().Initialize(hwnd, m_swapChain.Get());
    spdlog::info("Renderer initialization: menu initialized");
    if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
        readbackService->Initialize(m_readbackFence.Get(), m_copyReadbackFence.Get());
    }
    m_pReadbackManager = std::make_unique<br::ReadbackManager>();
    
    m_pReadbackManager->Initialize(m_legacyReadbackFence.Get());
    if (auto* statisticsService = currentRenderGraph->GetStatisticsService()) {
        statisticsService->Initialize();
    }

    spdlog::info("Renderer initialization: initializing upscaling managers");
    UpscalingManager::GetInstance().InitFFX(); // Needs device and must precede Setup for FSR queries.
    UpscalingManager::GetInstance().Setup();
    FFXManager::GetInstance().InitFFX();
    ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), "after UpscalingManager::Setup");

    CreateTextures();
    ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), "after CreateTextures");

    // Initialize GPU resource managers
    m_pLightManager = LightManager::CreateUnique();
    m_pMeshManager = MeshManager::CreateUnique();
	m_pMeshManager->SetRendererStateServices(
		m_rendererStateRequests.get(),
		currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr,
		m_numFramesInFlight);
	m_pObjectManager = ObjectManager::CreateUnique();
	m_pObjectManager->SetRendererStateServices(
		m_rendererStateRequests.get(),
		currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr,
		m_numFramesInFlight);
	// Static draw-record roots publish only with a Geometry root covering the
	// mesh templates they reference (see ObjectManager::SetGeometryCoverageSource).
	m_pObjectManager->SetGeometryCoverageSource([meshes = m_pMeshManager.get()] {
		return meshes->GeometryMutationSequence();
	});
	m_pIndirectCommandBufferManager = IndirectCommandBufferManager::CreateUnique();
	m_pViewManager = ViewManager::CreateUnique();
	m_pEnvironmentManager = EnvironmentManager::CreateUnique(currentRenderGraph->RetainUploadService());
	m_pEnvironmentManager->SetWorkServices(m_environmentWorkServices);
    CreateDefaultEnvironmentResources();
    m_pEnvironmentManager->SetRequestReadbackFn([this](std::shared_ptr<org::PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback, bool cubemap) {
        if (!m_pReadbackManager) {
            return;
        }

        m_pReadbackManager->RequestReadback(std::move(texture), std::move(outputFile), std::move(callback), cubemap);
    });
	m_pMaterialManager = MaterialManager::CreateUnique();
	br::render::RegisterMaterialRowProducer(*m_asyncStateGraph);
	br::render::RegisterMaterialUsageBatchProducer(*m_asyncStateGraph);
    m_pMaterialManager->SetRendererStateServices(
        m_rendererStateRequests.get(),
        currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr);
    m_pMaterialManager->SetDescriptorService(currentRenderGraph->RetainDescriptorService());
    m_pMaterialManager->SetRequestTextureReadbackFn(
        [this](std::shared_ptr<org::PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback) {
            if (m_pMaterialManager &&
                m_pMaterialManager->RequestExternalMaterialTextureReadback(
                    texture, outputFile, callback)) {
                return;
            }
            if (!m_pReadbackManager) {
                return;
            }
            m_pReadbackManager->RequestReadback(
                std::move(texture),
                std::move(outputFile),
                std::move(callback),
                false);
        });
    m_pTerrainManager = TerrainManager::CreateUnique();
	m_pTerrainManager->SetRendererStateServices(
		m_rendererStateRequests.get(),
		currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr,
		currentRenderGraph ? currentRenderGraph->RetainDescriptorService() : nullptr);
	//ResourceManager::GetInstance().SetEnvironmentBufferDescriptorIndex(m_pEnvironmentManager->GetEnvironmentBufferSRVDescriptorIndex());
	m_pLightManager->SetShadowViewService(m_pViewManager.get());
	m_pViewManager->SetEvents({
        .onCreated = [this](const View& view) {
            m_pIndirectCommandBufferManager->CreateBuffersForView(
                view.id, view.flags.primaryCamera);
        },
        .onDestroyed = [this](uint64_t viewID) {
            m_pIndirectCommandBufferManager->UnregisterBuffers(viewID);
        },
    });
    m_pMeshManager->SetViewManager(m_pViewManager.get());
	m_pIndirectCommandBufferManager->AttachActiveDrawSource(*m_pObjectManager);
	const auto transientWindMatrixCapacity = std::clamp(
		SettingsManager::GetInstance().getSettingGetter<std::uint32_t>(
			ProceduralWindTransientBoneCapacitySettingName)(),
		1024u, 1048576u);
	m_pSkeletonManager = SkeletonManager::CreateShared(
		currentRenderGraph->RetainUploadService(), transientWindMatrixCapacity);
	m_pMeshManager->SetSkeletonManager(m_pSkeletonManager.get());
	m_poseInstanceRegistrationService.Configure(m_pSkeletonManager.get());
	m_sceneRenderableResidencyService.Configure(m_pMeshManager.get(), m_pMaterialManager.get());
    const auto makeFrameTableFamily = [](std::uint64_t id, const char* name,
        std::uint32_t stride, br::render::PublishedFragmentKind owner,
        std::uint64_t variant, bool unorderedAccess = false) {
        return std::make_unique<br::render::VersionedBufferFamily>(
            br::render::VersionedBufferFamily::Config{
                { br::render::ArtifactKind::BufferVersion, id, 0 }, name, stride,
                unorderedAccess, false, owner,
                br::render::PublishedResourceUsage::ShaderResource, variant, false });
    };
    constexpr std::array<std::uint64_t, 5> lightVariants{
        br::render::LightInfoTableVariant, br::render::LightSpotViewTableVariant,
        br::render::LightPointViewTableVariant, br::render::LightDirectionalViewTableVariant,
        br::render::LightActiveIndexTableVariant };
    constexpr std::array<std::uint32_t, 5> lightStrides{
        sizeof(LightInfo), sizeof(std::uint32_t), sizeof(std::uint32_t), sizeof(std::uint32_t),
        sizeof(std::uint32_t) };
    for (std::size_t i = 0; i < m_lightTableFamilies.size(); ++i)
        m_lightTableFamilies[i] = makeFrameTableFamily(110 + i, "LightFrameTable", lightStrides[i],
            br::render::PublishedFragmentKind::Lights, lightVariants[i]);
    constexpr std::array<std::uint64_t, 4> poseVariants{
        br::render::PoseInverseBindTableVariant, br::render::PoseBoneTransformTableVariant,
        br::render::PoseInverseSkinTableVariant, br::render::PoseInstanceInfoTableVariant };
    constexpr std::array<std::uint32_t, 4> poseStrides{
        sizeof(DirectX::XMMATRIX), sizeof(DirectX::XMMATRIX), sizeof(DirectX::XMMATRIX),
        sizeof(SkinningInstanceGPUInfo) };
    // Inverse-bind matrices are immutable pose source data. The remaining
    // tables are GPU-written frame outputs (including procedural wind) and
    // cannot be republished from their CPU shadows; they stay owned by the
    // palette service until slot-local palette outputs are introduced.
    m_poseTableFamilies[0] = makeFrameTableFamily(120, "PoseInverseBindTable", poseStrides[0],
        br::render::PublishedFragmentKind::Poses, poseVariants[0]);
    m_pTextureFactory = TextureFactory::CreateUnique(currentRenderGraph->RetainUploadService());
    m_clodRayTracingSystem = std::make_shared<br::render::CLodRayTracingSystem>(
        currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr);
    if (currentRenderGraph) {
        m_pTextureFactory->SetReadbackService(currentRenderGraph->GetReadbackServiceOwner());
    }
	if (m_pMaterialManager) {
		m_pMaterialManager->InitializeTextureStreaming(*m_pTextureFactory, m_numFramesInFlight);
	}

    CreateGlobalResources();
    ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), "after CreateGlobalResources");

    m_sceneAssetRequestService.Configure(
        m_pTextureFactory.get(), m_pTerrainManager.get(), m_pMaterialManager.get());
	m_staticWorkloadRequestService.Configure(m_pIndirectCommandBufferManager.get());
	m_staticObjectRequestService.Configure(m_pObjectManager.get());
	m_staticGeometryRequestService.Configure(m_pMeshManager.get());
    m_staticMaterialRequestService.Configure(m_pMaterialManager.get());
    m_sceneSourceStateStore.Configure(
        RendererECSManager::GetInstance().GetWorld(),
        RendererECSManager::GetInstance().GetRenderPhaseEntities());
    m_sceneEntityMaterializationService.Configure(
        m_pObjectManager.get(), m_pViewManager.get(), m_pLightManager.get(),
        std::addressof(m_sceneRenderableResidencyService));
    m_sceneIngestionServices = {
        .source = std::addressof(m_sceneSourceStateStore),
        .sceneEntities = std::addressof(m_sceneEntityMaterializationService),
        .poseInstances = std::addressof(m_poseInstanceRegistrationService),
        .renderables = std::addressof(m_sceneRenderableResidencyService),
        .shaderVariants = std::addressof(m_shaderVariantRequestService),
        .execution = {
            .stateRequests = m_rendererStateRequests.get(),
			.generation = std::make_shared<br::render::ArtifactExecutionAccess::Generation>(
				currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr,
				currentRenderGraph ? currentRenderGraph->RetainDescriptorService() : nullptr) },
		.sceneAssetRequests = std::addressof(m_sceneAssetRequestService),
		.geometryRequests = std::addressof(m_staticGeometryRequestService),
		.materialRequests = std::addressof(m_staticMaterialRequestService),
		.objectRequests = std::addressof(m_staticObjectRequestService),
		.workloadRequests = std::addressof(m_staticWorkloadRequestService)
    };
    m_pIndirectCommandBufferManager->SetRendererStateServices(
        m_rendererStateRequests.get(),
        currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr);

    m_warnedNullScene = false;
    m_warnedMissingPrimaryCamera = false;
    m_warnedUsingFallbackEnvironment = false;

	m_isInitialized = true;
}


void Renderer::WaitForFrame(uint8_t currentFrameIndex) {
	// Wait until the GPU has completed commands up to this fence point.
	auto device = DeviceManager::GetInstance().GetDevice();
	auto completedValue = m_frameFence->GetCompletedValue();
    const UINT64 targetValue = m_frameFenceValues[currentFrameIndex];
    if (completedValue < targetValue) {
        spdlog::trace(
            "Renderer::WaitForFrame waiting frameIndex={} target={} completed={}",
            currentFrameIndex,
            targetValue,
            completedValue);
        uint32_t waitTimeouts = 0u;
        while (completedValue < targetValue) {
            const bool nvperfCaptureServiced = br::telemetry::nvperf::ServicePendingGpuOperations();
            const rhi::Result waitResult = m_frameFence->HostWait(targetValue, nvperfCaptureServiced ? 100 : 1000);
            completedValue = m_frameFence->GetCompletedValue();
            if (waitResult != rhi::Result::WaitTimeout) {
                if (waitResult != rhi::Result::Ok) {
                    spdlog::warn(
                        "Renderer::WaitForFrame wait failed frameIndex={} target={} completed={} result={}",
                        currentFrameIndex,
                        targetValue,
                        completedValue,
                        rhi::ResultName(waitResult));
                }
                break;
            }
            ++waitTimeouts;
            if (waitTimeouts == 5u || waitTimeouts == 30u || (waitTimeouts % 60u) == 0u) {
                spdlog::warn(
                    "Renderer::WaitForFrame timed out frameIndex={} target={} completed={} waitTimeouts={}",
                    currentFrameIndex,
                    targetValue,
                    completedValue,
                    waitTimeouts);
            }
        }
        spdlog::debug(
            "Renderer::WaitForFrame completed frameIndex={} target={} completed={}",
            currentFrameIndex,
            targetValue,
            m_frameFence->GetCompletedValue());
    }
}

Renderer::~Renderer() {
    // Resources unregister from this process-global service during member
    // destruction. Clear the slot before the service member itself is
    // destroyed so exception unwinding cannot call through a dangling pointer.
    org::runtime::SetActiveUploadPolicyService(nullptr);
}

Renderer::Renderer() = default;
