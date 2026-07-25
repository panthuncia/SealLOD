//
// Created by matth on 6/25/2024.
//

#ifndef DX12RENDERER_H
#define DX12RENDERER_H

#include <windows.h>
#include <d3d12.h>
#include <chrono>
#include <directxmath.h>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <flecs.h>

#include <rhi.h>

#include <sl.h>

#include "Scene/Scene.h"
#include "Managers/InputManager.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Managers/ViewManager.h"
#include "Managers/LightManager.h"
#include "Managers/MeshManager.h"
#include "Managers/ObjectManager.h"
#include "Managers/IndirectCommandBufferManager.h"
#include "Managers/EnvironmentManager.h"
#include "Managers/MaterialManager.h"
#include "Managers/SkeletonManager.h"
#include "Managers/TerrainManager.h"
#include "Managers/ReadbackManager.h"
#include "Factories/TextureFactory.h"
#include "Scene/MovementState.h"
#include "Telemetry/FrameTaskGraphTelemetry.h"
#include "Render/BuiltinResources.h"
#include "Render/GraphExtensions/RenderGraphExtensionRegistration.h"
#include "Utilities/Timer.h"
#include "Render/RenderContext.h"
#include "Render/RendererSettings.h"
#include "Render/OpenPBRLookupResources.h"
#include "Render/SceneRenderBridge.h"
#include "Render/GraphExtensions/ClusterLOD/CLodRayTracingSystem.h"
#include "Render/ShaderVariantRequestService.h"
#include "Render/Pipeline/PipelineRecipe.h"

class DynamicResource;
class ExternalTextureResource;

using namespace Microsoft::WRL;

namespace rg::runtime {
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
    struct SamplingReadinessSnapshot {
        bool sceneTaskInFlight = false;
        bool hasCommittedSceneSnapshot = false;
        uint64_t committedSceneSnapshotSequence = 0;
        uint64_t pendingSceneSnapshotSequence = 0;
        uint32_t pendingTextureReloads = 0;
        uint32_t fullResolutionTextures = 0;
        uint32_t residentClodGroups = 0;
        uint32_t queuedClodRequests = 0;
        uint32_t inFlightClodGroups = 0;
        uint32_t completedClodResults = 0;
        uint32_t pendingDirectStorageLaunches = 0;
        uint32_t pendingDirectStorageUploads = 0;
        uint32_t ioTasks = 0;
        uint32_t backgroundTasks = 0;
        uint32_t shaderCompileTasks = 0;
        uint64_t deferredRetireQueueDepth = 0;
        uint64_t drawRecordsAllocated = 0;
    };

    Renderer() = default;

    void Initialize(HWND hwnd, UINT x_res, UINT y_res, br::pipeline::PipelineRecipe recipe);
    void OnResize(UINT newWidth, UINT newHeight);
    void Update(float elapsedSeconds);
	void PostUpdate();
    void Render();
    void Cleanup();
    std::shared_ptr<Scene>& GetCurrentScene();
    void SetCurrentScene(std::shared_ptr<Scene> newScene);
    InputManager& GetInputManager();
    void SetInputMode(InputMode mode);
    void SetCameraSpeed(float speed);
    void SetEnvironment(std::string name);
    std::shared_ptr<Scene> AppendScene(std::shared_ptr<Scene> scene);
	bool IsInitialized() const { return m_isInitialized; }
    void SetExternalSceneMode(bool enabled);
    void SetSceneRenderOverlapEnabled(bool enabled);
    void IngestExternalSnapshot(const br::render::SceneFrameSnapshot& snapshot);
    ObjectManager::Stats GetObjectManagerStats() const;
    SamplingReadinessSnapshot GetSamplingReadinessSnapshot() const;
    void SetDeterministicSamplingMode(bool enabled);
    bool GetDeterministicSamplingMode() const { return m_deterministicSamplingMode; }
    ManagerInterface& GetManagerInterface() { return m_managerInterface; }
    const ManagerInterface& GetManagerInterface() const { return m_managerInterface; }
    uint64_t GetTotalFramesRendered() const { return m_totalFramesRendered; }
    RenderGraph* GetRenderGraph() { return currentRenderGraph.get(); }
    const RenderGraph* GetRenderGraph() const { return currentRenderGraph.get(); }
    bool RequestPipelineReplacement(br::pipeline::PipelineRecipe recipe);
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
	std::vector<std::shared_ptr<ExternalTextureResource>> m_backbufferResources;
	std::shared_ptr<DynamicResource> m_dynamicBackbuffer;
    //ComPtr<ID3D12DescriptorHeap> dsvHeap;
	//std::vector<ComPtr<ID3D12Resource>> depthStencilBuffers;
	//Components::DepthMap m_depthMap;
    std::vector<rhi::CommandAllocatorPtr> m_commandAllocators;
    std::vector<rhi::CommandListPtr> m_commandLists;
    UINT rtvDescriptorSize;
    UINT dsvDescriptorSize;
    uint8_t m_frameIndex = 0;
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

    std::unique_ptr<RenderGraph> currentRenderGraph = nullptr;
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

	std::string m_environmentName;
	std::unique_ptr<Environment> m_currentEnvironment = nullptr;
    std::shared_ptr<PixelBuffer> m_defaultEnvironmentCubemap = nullptr;
    std::shared_ptr<PixelBuffer> m_defaultEnvironmentPrefilteredCubemap = nullptr;
    std::shared_ptr<PixelBuffer> m_blueNoiseTexture = nullptr;
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
	std::unique_ptr<EnvironmentManager> m_pEnvironmentManager = nullptr;
    std::unique_ptr<MaterialManager> m_pMaterialManager = nullptr;
	std::unique_ptr<SkeletonManager> m_pSkeletonManager = nullptr;
    std::unique_ptr<TerrainManager> m_pTerrainManager = nullptr;
    std::unique_ptr<br::ReadbackManager> m_pReadbackManager = nullptr;
    std::unique_ptr<TextureFactory> m_pTextureFactory = nullptr;
    std::unique_ptr<br::render::CLodRayTracingSystem> m_clodRayTracingSystem = nullptr;
    ShaderVariantRequestService m_shaderVariantRequestService;

	ManagerInterface m_managerInterface;
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

    std::vector<SettingsManager::Subscription> m_settingsSubscriptions;

    uint64_t m_lastTerrainRvtTelemetryRequestFrame = UINT64_MAX;
    bool m_terrainRvtStatsReadbackPending = false;
    bool m_terrainRvtCountersReadbackPending = false;
    bool m_loggedTerrainRvtTelemetryEnabled = false;

    DeferredFunctions m_preFrameDeferredFunctions;
    int32_t m_lastFrameTaskNodeIndex = -1;
    br::render::SceneRenderBridge m_sceneRenderBridge;
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
    std::unordered_set<uint64_t> m_externalRegisteredMeshes;
    std::unordered_set<uint64_t> m_externalRegisteredMeshInstances;

    std::shared_ptr<rg::runtime::IUploadPolicyService> m_uploadPolicyService = nullptr;
    uint64_t m_lastCLodVisibilityTelemetryRequestFrame = UINT64_MAX;
    bool m_clodTelemetryReadbackPending = false;
    bool m_clodVisibleCounterReadbackPending = false;
    bool m_loggedCLodVisibilityTelemetryEnabled = false;
    bool m_clodVisibilityTelemetryDebugEnabledByRenderer = false;
    uint64_t m_lastCLodVirtualShadowTelemetryRequestFrame = UINT64_MAX;
    bool m_clodVirtualShadowTelemetryReadbackPending = false;
    bool m_clodVirtualShadowWorkTelemetryReadbackPending = false;
    bool m_clodVirtualShadowVisibleCounterReadbackPending = false;
    bool m_loggedCLodVirtualShadowTelemetryEnabled = false;
    uint64_t m_lastObjectReyesAtlasTelemetryRequestFrame = UINT64_MAX;
    bool m_objectReyesAtlasTelemetryPhase1ReadbackPending = false;
    bool m_objectReyesAtlasTelemetryPhase2ReadbackPending = false;
    bool m_loggedObjectReyesAtlasTelemetryEnabled = false;

    class CoreResourceProvider : public IResourceProvider {
	public:
        std::shared_ptr<PixelBuffer> m_HDRColorTarget = nullptr;
		std::shared_ptr<PixelBuffer> m_upscaledHDRColorTarget = nullptr;
		std::shared_ptr<PixelBuffer> m_gbufferMotionVectors = nullptr;

		std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override { // TODO: don't use ifs
            if (key.ToString() == Builtin::GBuffer::MotionVectors)
				return m_gbufferMotionVectors;
            if (key.ToString() == Builtin::Color::HDRColorTarget)
				return m_HDRColorTarget;
            if (key.ToString() == Builtin::PostProcessing::UpscaledHDR)
				return m_upscaledHDRColorTarget;
		
			spdlog::error("CoreResourceProvider: ProvideResource called with unknown key: {}", key.ToString());
			return nullptr;
        }

        std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override {
            return nullptr;
		}

        std::vector<ResourceIdentifier> GetSupportedKeys() override {
			return {
                Builtin::GBuffer::MotionVectors,
                Builtin::Color::HDRColorTarget,
				Builtin::PostProcessing::UpscaledHDR,
			};
        }

        std::vector<ResourceIdentifier> GetSupportedResolverKeys() override {
            return {};
		}

        void Cleanup() {
			m_HDRColorTarget = nullptr;
			m_upscaledHDRColorTarget = nullptr;
			m_gbufferMotionVectors = nullptr;
        }
    };
	CoreResourceProvider m_coreResourceProvider;
};

#endif //DX12RENDERER_H
