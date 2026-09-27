//
// Created by matth on 6/25/2024.
//

#ifndef DX12RENDERER_H
#define DX12RENDERER_H

#include <windows.h>
#include <chrono>
#include <cstdint>
#include <directxmath.h>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <filesystem>
#include <optional>
#include <array>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <flecs.h>

#include <rhi.h>
#include <BasicRenderer/Extensions/SettingAccess.h>

#include <BasicRenderer/Scene/Components.h>
#include <BasicRenderer/Scene/RendererComponents.h>
#include <BasicRenderer/Extensions/Input/InputManager.h>
#include "BasicRenderer/Runtime/Detail/DepthHistoryService.h"
#include <BasicRenderer/Streaming/SceneAssetRequestService.h>
#include <BasicRenderer/Streaming/StaticWorkloadRequestService.h>
#include <BasicRenderer/Streaming/StaticObjectRequestService.h>
#include <BasicRenderer/Streaming/StaticGeometryRequestService.h>
#include <BasicRenderer/Streaming/StaticMaterialRequestService.h>
#include <BasicScene/MovementState.h>
#include <BasicRenderer/Extensions/BuiltinResources.h>
#include <BasicRenderer/Extensions/RenderGraphExtensionRegistration.h>
#include "BasicRenderer/Runtime/Detail/FrameTimer.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "BasicRenderer/Runtime/Detail/OpenPBRLookupResources.h"
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "BasicRenderer/Scene/SceneRenderBridge.h"
#include "BasicRenderer/Runtime/Detail/SceneSourceStateStore.h"
#include "BasicRenderer/Runtime/Detail/SceneEntityMaterializationService.h"
#include "BasicRenderer/Scene/SceneIngestionServices.h"
#include "BasicRenderer/Runtime/Detail/PoseInstanceRegistrationService.h"
#include "BasicRenderer/Runtime/Detail/SceneRenderableResidencyService.h"
#include "BasicRenderer/Pipeline/ShaderVariantRequestService.h"
#include <BasicRenderer/Pipeline/PipelineRecipe.h>
#include "BasicRenderer/Runtime/Detail/MaterialEvaluationBuildInputs.h"
#include <BasicRenderer/Extensions/ProducerPersistentState.h>
#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Diagnostics/SamplingReadinessSnapshot.h>

struct ID3D12Resource;
class Scene;
namespace org {
class DynamicResource;
class ExternalTextureResource;
class PixelBuffer;
class RenderGraph;
}
namespace br::render {
struct RendererFrameInputs;
class RendererStatePublisher;
class RendererStateRequestService;
class AsyncStateGraph;
class VersionedBufferFamily;
}
namespace br::telemetry { enum class CpuTaskDomain : uint8_t; }
class CLodStreamingSystem;
class VirtualShadowCasterRegistry;
class ObjectManager;
class LightManager;
class Environment;
class EnvironmentManager;
class SkeletonManager;
class TextureFactory;
class IndirectCommandBufferManager;
class ViewManager;
class MaterialManager;
class TerrainManager;
class MeshManager;
namespace br { class ReadbackManager; }
namespace br::render { class CLodRayTracingSystem; }

namespace org::runtime {
class IUploadPolicyService;
}

class DeferredFunctions {
public:
    // enqueue any void() callable
    void defer(std::function<void()> fn) {
        _queue.emplace_back(std::move(fn));
    }

    // invoke all, then clear
    void flush() {
        for (auto &fn : _queue)
            fn();
        _queue.clear();
    }

    bool empty() const { return _queue.empty(); }

private:
    std::vector<std::function<void()>> _queue;
};

class Renderer {
public:
    using SamplingReadinessSnapshot = br::render::SamplingReadinessSnapshot;

    Renderer();
    ~Renderer();

    void Initialize(HWND hwnd, UINT x_res, UINT y_res, br::pipeline::PipelineRecipe recipe);
    void OnResize(UINT newWidth, UINT newHeight);
    void Update(float elapsedSeconds);
	void PostUpdate();
    void Render();
    void Cleanup();
    std::shared_ptr<Scene>& GetCurrentScene();
    void SetCurrentScene(std::shared_ptr<Scene> newScene);
    InputManager& GetInputManager();
    bool HandleMenuInput(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void SetInputMode(InputMode mode);
    void SetCameraSpeed(float speed);
    // Establishes the scene-mutation boundary for hosts that update ECS state
    // outside Renderer::Update. Required when async graph preparation may have
    // outlived a Render call that returned before its normal join point.
    void WaitForAsyncPreparation();
    void SetEnvironment(std::string name);
    std::shared_ptr<Scene> AppendScene(std::shared_ptr<Scene> scene);
	bool IsInitialized() const { return m_isInitialized; }
    void SetExternalSceneMode(bool enabled);
    void SetSceneRenderOverlapEnabled(bool enabled);
    void IngestExternalSnapshot(const br::render::SceneFrameSnapshot& snapshot);
    br::render::ObjectStorageStats GetObjectManagerStats() const;
    SamplingReadinessSnapshot GetSamplingReadinessSnapshot(bool includeExpensiveDiagnostics = true) const;
    void SetDeterministicSamplingMode(bool enabled);
    bool GetDeterministicSamplingMode() const { return m_deterministicSamplingMode; }
    br::render::SceneIngestionServices& GetSceneIngestionServices() { return m_sceneIngestionServices; }
    const br::render::SceneIngestionServices& GetSceneIngestionServices() const { return m_sceneIngestionServices; }
    uint64_t GetTotalFramesRendered() const { return m_totalFramesRendered; }
    org::RenderGraph* GetRenderGraph() { return currentRenderGraph.get(); }
    const org::RenderGraph* GetRenderGraph() const { return currentRenderGraph.get(); }
    bool RequestPipelineReplacement(br::pipeline::PipelineRecipe recipe);
    void StartAsyncStateGraphTrace(br::render::AsyncStateGraphTraceConfig config = {});
    [[nodiscard]] bool AsyncStateGraphTraceActive() const;
    br::render::AsyncStateGraphTraceReport StopAsyncStateGraphTraceAndWriteReport(
        const std::filesystem::path& outputDirectory);
    void SetProducerPersistentState(std::shared_ptr<ProducerPersistentState> state) {
        if (m_isInitialized) throw std::logic_error("producer persistent state must be set before initialization");
        m_producerPersistentState = state ? std::move(state) : std::make_shared<ProducerPersistentState>();
    }
    const br::pipeline::PipelineRecipe& GetPipelineRecipe() const { return m_pipelineRecipe; }
    void SetPipelineReplacementDebugBreakHandler(std::function<void()> handler) {
        m_pipelineReplacementDebugBreakHandler = std::move(handler);
    }

private:
	bool m_isInitialized = false;
    bool m_deterministicSamplingMode = false;
    HWND m_hwnd = nullptr;
    rhi::Device m_device;

    rhi::SwapchainPtr m_swapChain;

    rhi::DescriptorHeapPtr rtvHeap;
	std::vector<rhi::ResourceHandle> renderTargets;
	std::vector<std::shared_ptr<org::ExternalTextureResource>> m_backbufferResources;
	std::shared_ptr<org::DynamicResource> m_dynamicBackbuffer;
	std::vector<std::shared_ptr<org::PixelBuffer>> m_presentationColorResources;
	std::shared_ptr<org::DynamicResource> m_dynamicPresentationColor;
    //ComPtr<ID3D12DescriptorHeap> dsvHeap;
	//std::vector<ComPtr<ID3D12Resource>> depthStencilBuffers;
	//Components::DepthMap m_depthMap;
    std::vector<rhi::CommandAllocatorPtr> m_commandAllocators;
    std::vector<rhi::CommandListPtr> m_commandLists;
    UINT rtvDescriptorSize;
    UINT dsvDescriptorSize;
    uint8_t m_frameIndex = 0;
    // Logical-frame preparation advances independently once async queue
    // prefill is enabled. m_frameIndex remains the acquired swapchain image.
    uint8_t m_preparationFrameIndex = 0;
    uint64_t m_totalFramesRendered = 0;
	uint8_t m_numFramesInFlight = 3;
    rhi::TimelinePtr m_frameFence;
    std::vector<UINT64> m_frameFenceValues; // Store fence values per frame
    UINT64 m_currentFrameFenceValue = 1; // Start at 1, waiting on 0 is meaningless

	rhi::TimelinePtr m_readbackFence;
    rhi::TimelinePtr m_copyReadbackFence;
	rhi::TimelinePtr m_legacyReadbackFence;

    InputManager inputManager;
    MovementState movementState;
    float verticalAngle = 0;
    float horizontalAngle = 0;

    std::shared_ptr<Scene> currentScene;

    std::unique_ptr<org::RenderGraph> currentRenderGraph = nullptr;
    bool m_renderGraphRuntimeInitialized = false;
    br::pipeline::PipelineRecipe m_pipelineRecipe;
    std::optional<br::pipeline::PipelineRecipe> m_pendingPipelineRecipe;
    std::optional<br::pipeline::PipelineRecipe> m_pipelineRollbackRecipe;
    mutable std::mutex m_pipelineRecipeMutex;
    bool m_pipelineExtensionsDirty = true;
    bool m_syncingPipelineTopologySettings = false;
    std::function<void()> m_pipelineReplacementDebugBreakHandler;
    bool rebuildRenderGraph = true;
    bool m_shaderReloadRequested = false;

    RenderContext m_context;
    // Most recently accepted immutable logical-frame publication. The render
    // half of the frame never exposes a pointer to mutable m_context.
    std::shared_ptr<const br::render::RendererFrameInputs> m_frameInputs;
    std::uint64_t m_lightArtifactRevision = 1;
    std::uint64_t m_lastLightSourceRevision = 0;
    std::uint64_t m_lastLightViewFamilyRevision = 0;
    std::uint64_t m_lastPoseSourceRevision = 0;
    MaterialEvaluationBuildInputs m_materialEvaluationInputs;
    // Persistent producer state survives graph rebuilds and full/producer
    // recipe switches. It is released only with the renderer/device lifetime.
    std::shared_ptr<ProducerPersistentState> m_producerPersistentState = std::make_shared<ProducerPersistentState>();

	std::string m_environmentName;
	std::unique_ptr<Environment> m_currentEnvironment = nullptr;
    std::shared_ptr<org::PixelBuffer> m_defaultEnvironmentCubemap = nullptr;
    std::shared_ptr<org::PixelBuffer> m_defaultEnvironmentPrefilteredCubemap = nullptr;
    std::shared_ptr<org::PixelBuffer> m_blueNoiseTexture = nullptr;
    OpenPBRLookupResources m_openPBRLookupResources;
    bool m_warnedUsingFallbackEnvironment = false;
    bool m_warnedNullScene = false;
    bool m_warnedMissingPrimaryCamera = false;

    // GPU resource managers
    std::unique_ptr<LightManager> m_pLightManager = nullptr;
    std::unique_ptr<MeshManager> m_pMeshManager = nullptr;
    std::unique_ptr<ObjectManager> m_pObjectManager = nullptr;
    std::unique_ptr<IndirectCommandBufferManager> m_pIndirectCommandBufferManager = nullptr;
    std::unique_ptr<ViewManager> m_pViewManager = nullptr;
    br::render::DepthHistoryPublicationService m_depthHistory;
	std::unique_ptr<EnvironmentManager> m_pEnvironmentManager = nullptr;
	br::render::EnvironmentWorkServices m_environmentWorkServices;
    std::unique_ptr<MaterialManager> m_pMaterialManager = nullptr;
	std::shared_ptr<SkeletonManager> m_pSkeletonManager = nullptr;
    std::unique_ptr<TerrainManager> m_pTerrainManager = nullptr;
    std::unique_ptr<br::ReadbackManager> m_pReadbackManager = nullptr;
    std::unique_ptr<TextureFactory> m_pTextureFactory = nullptr;
    std::shared_ptr<br::render::CLodRayTracingSystem> m_clodRayTracingSystem = nullptr;
    std::unique_ptr<br::render::AsyncStateGraph> m_asyncStateGraph;
    std::optional<br::render::AsyncStateGraphTraceConfig> m_pendingAsyncStateGraphTrace;
    std::unique_ptr<br::render::RendererStatePublisher> m_rendererStatePublisher;
    std::unique_ptr<br::render::RendererStateRequestService> m_rendererStateRequests;
    std::array<std::unique_ptr<br::render::VersionedBufferFamily>, 5> m_lightTableFamilies;
    std::array<std::unique_ptr<br::render::VersionedBufferFamily>, 4> m_poseTableFamilies;
    TaskScope m_rendererStateCommitScope;
    TaskScope m_presentationTailScope;
    ShaderVariantRequestService m_shaderVariantRequestService;

    br::render::SceneIngestionServices m_sceneIngestionServices;
    br::render::PoseInstanceRegistrationService m_poseInstanceRegistrationService;
    br::render::SceneRenderableResidencyService m_sceneRenderableResidencyService;
    br::render::SceneAssetRequestService m_sceneAssetRequestService;
	br::render::StaticWorkloadRequestService m_staticWorkloadRequestService;
	br::render::StaticObjectRequestService m_staticObjectRequestService;
	br::render::StaticGeometryRequestService m_staticGeometryRequestService;
	br::render::StaticMaterialRequestService m_staticMaterialRequestService;
    DirectX::XMUINT3 m_lightClusterSize = { 12, 12, 24 };
    FrameTimer m_frameTimer;

    void LoadPipeline(HWND hwnd, UINT x_res, UINT y_res);
    void CreateTextures();
	void TagDLSSResources(ID3D12Resource* pDepthTexture);
    void MoveForward();
    void SetupInputHandlers();
    void CreateGlobalResources();
    void CreateDefaultEnvironmentResources();
    void CreateRenderGraph();
    void ApplyPendingPipelineReplacement();
    br::pipeline::PipelineRecipe GetPipelineRecipeForMutation() const;
    void RegisterPipelineExtensions();
    void HandlePipelineReplacementFailure(const std::exception& error);
    void SetSettings();
    void SetEnvironmentInternal(std::wstring name);
	void ToggleMeshShaders(bool useMeshShaders);
    bool IsSceneReadyForFrame(bool logWarnings = true);
    flecs::entity GetValidatedPrimaryRenderCamera(bool attemptResync = true);
    void BootstrapCommittedSceneSnapshot();
    void CommitCompletedSceneSnapshot();
    void ScheduleSceneUpdateTask(float elapsedSeconds);
    bool HasCommittedSceneSnapshot() const;
    bool NeedsSceneSnapshotBootstrap() const;
    br::render::SceneOverlapStatus GetSceneOverlapStatus() const;
    void WaitForFrame(uint8_t frameIndex);
    void SignalFence(rhi::Queue commandQueue, uint8_t currentFrameIndex);
    void AdvanceFrameIndex();
    void CheckDebugMessages();
    void CreateRTVs();
    void RunGameUpdateStage(float elapsedSeconds);
    void RunAnimationUpdateStage(float elapsedSeconds);
    void RunTransformPropagationStage();
    void RunSceneBridgeSyncStage();
    void RegisterExternalSnapshotMeshes(const br::render::SceneFrameSnapshot& snapshot);
    void ClearExternalSnapshotMeshRegistrations();
    void ApplyPrimaryCameraInput(float elapsedSeconds);
    void ApplyPrimaryCameraInputToRenderBridge(float elapsedSeconds);
    void InvalidateSceneOverlapState();
    void RunRenderResourceSyncStage();
    void FlushPendingSceneExplorerEdits();
    void QueueSceneNodePositionEdit(uint64_t stableSceneID, DirectX::XMFLOAT3 position);
    void QueueSceneNodeUniformScaleEdit(uint64_t stableSceneID, float uniformScale);
    void BeginFrameTaskGraphCapture();
    void RecordFrameTaskStage(
        const char* stageName,
        br::telemetry::CpuTaskDomain domain,
        const std::chrono::steady_clock::time_point& stageStart,
        const std::chrono::steady_clock::time_point& stageEnd);
    void PublishFrameTaskGraphCapture();
    void MaybeRequestCLodVisibilityTelemetry();
    void MaybeRequestCLodVirtualShadowTelemetry();
    void MaybeRequestObjectReyesAtlasTelemetry();
    void MaybeRequestTerrainRvtTelemetry();
    void ApplyWindowResolutionPreset(WindowResolutionPreset preset);

    void StallPipeline();

	void RunBeforeNextFrame(std::function<void()> fn) {
		m_preFrameDeferredFunctions.defer(fn);
	}

    // Feature support
	bool m_dlssSupported = false;

	// Settings
	bool m_allowTearing = false;
	bool m_clusteredLighting = true;
    bool m_imageBasedLighting = true;
	bool m_gtaoEnabled = true;
	bool m_visibilityRendering = true;
	bool m_occlusionCulling = true;
    bool m_bloom = false;
    bool m_jitter = true;
	bool m_screenSpaceReflections = false;
    bool m_rayTracedReflections = false;
    bool m_warnedRayTracedReflectionsUnsupported = false;
	bool m_useMeshShaders = true;

    std::function<uint16_t()> getShadowResolution;
	std::function<void(float)> setCameraSpeed;
	std::function<float()> getCameraSpeed;
	std::function<void(bool)> setWireframeEnabled;
	std::function<bool()> getWireframeEnabled;
	std::function<void(bool)> setShadowsEnabled;
	std::function<bool()> getShadowsEnabled;
    std::function<uint16_t()> getSkyboxResolution;
	std::function<void(bool)> setImageBasedLightingEnabled;
	std::function<void(std::string)> setEnvironment;
	std::function<bool()> getMeshShadersEnabled;
    std::function<bool()> getIndirectDrawsEnabled;
	std::function<uint8_t()> getNumFramesInFlight;
    std::function<bool()> getDrawBoundingSpheres;
	std::function<bool()> getImageBasedLightingEnabled;

    std::vector<br::extensions::SettingSubscription> m_settingsSubscriptions;

    uint64_t m_lastTerrainRvtTelemetryRequestFrame = UINT64_MAX;
    bool m_terrainRvtStatsReadbackPending = false;
    bool m_terrainRvtCountersReadbackPending = false;
    bool m_loggedTerrainRvtTelemetryEnabled = false;

    DeferredFunctions m_preFrameDeferredFunctions;
    int32_t m_lastFrameTaskNodeIndex = -1;
    br::render::SceneRenderBridge m_sceneRenderBridge;
    br::render::SceneSourceStateStore m_sceneSourceStateStore;
    br::render::SceneEntityMaterializationService m_sceneEntityMaterializationService;
    bool m_sceneRenderOverlapEnabled = true;
    bool m_externalSceneMode = false;
    bool m_swapChainReady = true;
    bool m_loggedSwapChainNotReady = false;

    // Cached renderer ECS queries for RunRenderResourceSyncStage
    flecs::query<Components::Matrix, Components::RenderableObject, Components::ObjectDrawInfo, Components::MeshInstances> m_renderSyncObjectQuery;
    flecs::query<Components::Matrix, Components::Camera, Components::RenderViewRef> m_renderSyncCameraQuery;
    flecs::query<Components::Matrix, Components::Light> m_renderSyncLightQuery;
    flecs::query<> m_renderTransformUpdatedCleanupQuery;
    bool m_renderSyncQueriesBuilt = false;
    std::shared_ptr<br::render::SceneFrameSnapshot> m_completedSceneSnapshot;
    mutable std::mutex m_sceneSnapshotMutex;
    bool m_hasCommittedSceneSnapshot = false;
    std::atomic<bool> m_sceneTaskInFlight = false;
    std::atomic<bool> m_sceneTaskCompleted = false;
    std::atomic<uint64_t> m_sceneOverlapEpoch = 1;
    uint64_t m_nextSceneSnapshotSequence = 1;
    uint64_t m_lastCommittedSceneSnapshotSequence = 0;
    uint64_t m_lastCompletedSceneSnapshotSequence = 0;
    uint64_t m_lastCommittedSceneSourceFrame = 0;
    double m_lastSceneTaskDurationMs = 0.0;

    struct PendingSceneExplorerEdit {
        bool hasPosition = false;
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        bool hasUniformScale = false;
        float uniformScale = 1.0f;
    };

    std::mutex m_pendingSceneExplorerEditsMutex;
    std::unordered_map<uint64_t, PendingSceneExplorerEdit> m_pendingSceneExplorerEdits;
    struct ExternalMeshRegistration {
        std::shared_ptr<Material> material;
        MaterialCompileFlags regularEvalFlags = MaterialCompileNone;
        MaterialCompileFlags reyesEvalFlags = MaterialCompileNone;
        MaterialRasterFlags rasterFlags = MaterialRasterFlagsNone;
        bool hasDistinctReyes = false;
    };
    std::unordered_map<uint64_t, ExternalMeshRegistration> m_externalMeshRegistrations;
    std::unordered_set<uint64_t> m_externalRegisteredMeshes;
    std::unordered_set<uint64_t> m_externalRegisteredMeshInstances;

    std::shared_ptr<org::runtime::IUploadPolicyService> m_uploadPolicyService = nullptr;
    uint64_t m_lastCLodVisibilityTelemetryRequestFrame = UINT64_MAX;
    bool m_clodTelemetryReadbackPending = false;
    bool m_clodRasterArgsReadbackPending = false;
    bool m_clodVisibleCounterReadbackPending = false;
    bool m_clodVisibleRecordsReadbackPending = false;
    bool m_clodReplayStateReadbackPending = false;
    bool m_loggedCLodVisibilityTelemetryEnabled = false;
    bool m_clodVisibilityTelemetryDebugEnabledByRenderer = false;
    uint64_t m_lastCLodVirtualShadowTelemetryRequestFrame = UINT64_MAX;
    bool m_clodVirtualShadowTelemetryReadbackPending = false;
    bool m_clodVirtualShadowWorkTelemetryReadbackPending = false;
    uint32_t m_virtualShadowCasterTelemetryReadbacksPending = 0u;
    bool m_loggedCLodVirtualShadowTelemetryEnabled = false;
    uint64_t m_lastObjectReyesAtlasTelemetryRequestFrame = UINT64_MAX;
    bool m_objectReyesAtlasTelemetryPhase1ReadbackPending = false;
    bool m_objectReyesAtlasTelemetryPhase2ReadbackPending = false;
    bool m_loggedObjectReyesAtlasTelemetryEnabled = false;

    class CoreResourceProvider : public org::IResourceProvider {
	public:
        std::shared_ptr<org::PixelBuffer> m_HDRColorTarget = nullptr;
		std::shared_ptr<org::PixelBuffer> m_upscaledHDRColorTarget = nullptr;
		std::shared_ptr<org::PixelBuffer> m_gbufferDilatedMotionVectors = nullptr;

		std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override { // TODO: don't use ifs
			if (key.ToString() == Builtin::Surface::DilatedMotion)
				return m_gbufferDilatedMotionVectors;
            if (key.ToString() == Builtin::Color::HDRColorTarget)
				return m_HDRColorTarget;
            if (key.ToString() == Builtin::PostProcessing::UpscaledHDR)
				return m_upscaledHDRColorTarget;
		
			spdlog::error("CoreResourceProvider: ProvideResource called with unknown key: {}", key.ToString());
			return nullptr;
        }

        std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override {
            return nullptr;
		}

        std::vector<org::ResourceIdentifier> GetSupportedKeys() override {
			return {
                Builtin::Surface::DilatedMotion,
                Builtin::Color::HDRColorTarget,
				Builtin::PostProcessing::UpscaledHDR,
			};
        }

        std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override {
            return {};
		}

        void Cleanup() {
			m_HDRColorTarget = nullptr;
			m_upscaledHDRColorTarget = nullptr;
			m_gbufferDilatedMotionVectors = nullptr;
        }
    };
	CoreResourceProvider m_coreResourceProvider;
};

#endif //DX12RENDERER_H
