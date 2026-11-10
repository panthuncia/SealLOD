//
// Created by matth on 6/25/2024.
//

#ifndef DX12RENDERER_H
#define DX12RENDERER_H

#include <windows.h>
#include "Interfaces/IResourceProvider.h"
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
#include <BasicRenderer/Pipeline/UpscalingTypes.h>
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
class CLodVirtualShadowFrameTelemetryState;
struct PathTraceState;
namespace br::lens { class LensManager; }
namespace org {
class DynamicResource;
class ExternalTextureResource;
class PixelBuffer;
class RenderGraph;
class GraphInstance;
class Execution;
class Runtime;
}
namespace br::render {
class RendererFrameInputs;
class TextureJobGraph;
class EnvironmentJobGraph;
class RendererStatePublisher;
class RendererStateRequestService;
class AsyncStateGraph;
class VersionedBufferFamily;
class VersionedBufferRuntime;
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

    // Starts Streamline's device-independent setup (about a second) in the background, for a later Initialize on
    // this thread. Does nothing when BASICRENDERER_DISABLE_STREAMLINE is set.
    static void PrewarmStreamline();
    void Initialize(HWND hwnd, UINT x_res, UINT y_res, br::pipeline::PipelineRecipe recipe);
    void OnResize(UINT newWidth, UINT newHeight);
    void Update(float elapsedSeconds);
    void Render();
    void Cleanup();
    std::shared_ptr<Scene>& GetCurrentScene();
    void SetCurrentScene(std::shared_ptr<Scene> newScene);
    InputManager& GetInputManager();
    bool HandleMenuInput(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void SetInputMode(InputMode mode);
    void SetCameraSpeed(float speed);
    void SetEnvironment(std::string name);
	bool IsInitialized() const { return m_isInitialized; }
    void SetExternalSceneMode(bool enabled);
    void SetSceneRenderOverlapEnabled(bool enabled);
    void IngestExternalSnapshot(const br::render::SceneFrameSnapshot& snapshot);
    br::render::ObjectStorageStats GetObjectManagerStats() const;
    SamplingReadinessSnapshot GetSamplingReadinessSnapshot(bool includeExpensiveDiagnostics = true) const;
    // Shader pipelines requested and not yet built: on-demand variants (material evaluation, CLod raster) and the graph's pass
    // pipelines, queued ones included. A pass or material whose pipeline is pending draws nothing yet.
    uint32_t GetPendingShaderCompileCount() const;
    void SetDeterministicSamplingMode(bool enabled);
    bool GetDeterministicSamplingMode() const { return m_deterministicSamplingMode; }
    br::render::SceneIngestionServices& GetSceneIngestionServices() { return m_sceneIngestionServices; }
    const br::render::SceneIngestionServices& GetSceneIngestionServices() const { return m_sceneIngestionServices; }
    uint64_t GetTotalFramesRendered() const { return m_totalFramesRendered; }
    org::RenderGraph* GetRenderGraph() { return currentRenderGraph; }
    // The path tracer's accumulation, for capturing a reference image. buffer is null unless the current graph path traces
    // with pathTracingAccumulate on. framesSinceRestart counts frames prepared since the last restart, so it can lead the
    // buffer's alpha (the sample count the GPU wrote) by the frames in flight; restarts counts restarts since graph build.
    struct PathTracingAccumulation {
        std::shared_ptr<org::PixelBuffer> buffer; // RGBA32F: rgb the running average, a the sample count
        uint64_t framesSinceRestart = 0;
        uint64_t restarts = 0;
    };
    PathTracingAccumulation GetPathTracingAccumulation() const;
    // The physical camera lens's control commands (lens.get, lens.set, lens.selftest, lens.selftest.status): a JSON
    // request, a JSON reply. Throws std::exception on a bad request. See SARP's docs/lens_simulation.md.
    std::string HandleLensControl(const std::string& command, const std::string& requestJson);
    // upscaler.get: the upscaling mode requested, the one the current graph runs and what its last evaluation ran, as JSON.
    std::string DescribeUpscaler() const;
    // upscaler.simulate_failure: see UpscalingManager::SimulateFailures.
    void SimulateUpscalerFailures(uint32_t outOfMemoryFrames, uint32_t settingsFailures);
    // Reads the primary view's visibility and visible clusters from one frame, so a tool can say what
    // drew each pixel. done runs on a readback thread. Returns false, without calling it, if the frame
    // has no such resources yet. Each call copies both resources, so this is not for every frame.
    struct CLodInspectionFrame {
        uint64_t frame = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint64_t> visibility;  // width * height keys
        std::vector<std::byte> visibleClusters;
        std::shared_ptr<const std::vector<std::byte>> drawRecords;
        std::shared_ptr<const std::vector<std::byte>> clodOffsets;
        std::shared_ptr<const std::vector<std::byte>> meshMetadata;
        // Filled when available; a pick (Diagnostics/CLodPixelPick.h) uses them to reach a pixel's mesh and material.
        std::shared_ptr<const std::vector<std::byte>> perMeshes;      // PerMeshCB rows
        std::shared_ptr<const std::vector<std::byte>> meshTemplates;  // PerMeshInstanceCB rows
        std::shared_ptr<const std::vector<std::byte>> groups;         // ClusterLODGroup rows (GPU copy: partIndex)
        std::shared_ptr<const std::vector<std::byte>> materialPalettes;     // CLodMaterialPaletteEntry rows
        std::shared_ptr<const std::vector<std::byte>> assemblyMaterialBases; // uint per assembly transform
        std::vector<std::byte> visibleClusterSideBand;  // uint2 per visible cluster: assembly transform, palette index
        std::vector<std::byte> reyesDiceQueue;          // CLodReyesDiceQueueEntry rows, for patch keys
        std::uint32_t reyesPatchIndexBase = 0xFFFFFFFFu; // key clusters at or above this are dice-queue entries
        std::vector<std::byte> surfaceRecords;          // SARPSurfaceRecordV1 per pixel (32 bytes, row-major)
        std::vector<std::string> missingReadbacks;      // optional readbacks that had not arrived when it was handed over
        bool pathTraced = false;  // read after PathTracePass: surface records only, no visibility or clusters
    };
    // A pending inspection. A readback of a resource its anchor pass does not touch never arrives, so a caller that can
    // live without the optional ones hands over what has arrived.
    class CLodInspectionRequest {
    public:
        virtual ~CLodInspectionRequest() = default;
        // Delivers the frame now when the visibility and clusters arrived. False when they have not, or it already went.
        virtual bool FinishWithAvailable() = 0;
        virtual std::string Missing() const = 0;
    };
    std::shared_ptr<CLodInspectionRequest> StartCLodInspectionFrame(std::function<void(CLodInspectionFrame&&)> done);
    bool RequestCLodInspectionFrame(std::function<void(CLodInspectionFrame&&)> done) { return StartCLodInspectionFrame(std::move(done)) != nullptr; }
    const org::RenderGraph* GetRenderGraph() const { return currentRenderGraph; }
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
    double m_lastWaitForFrameMs = 0.0; // The frame's WaitForFrame, for the hitch log in FrameMaintenance.
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

    // The ORG runtime every graph runs on: created before the first graph, destroyed after the last.
    std::shared_ptr<org::Runtime> m_orgRuntime;
    // GPU mip generation and BC7 compression, in a graph of their own next to the frame graph.
    std::unique_ptr<br::render::TextureJobGraph> m_textureJobs;
    std::unique_ptr<br::render::EnvironmentJobGraph> m_environmentJobs;
    // The frame graph, adopted by an instance that BR declares (Rebuild) and drives (Prepare, then Execution::Submit).
    std::unique_ptr<org::GraphInstance> m_frameGraph;
    org::RenderGraph* currentRenderGraph = nullptr;  // m_frameGraph's
    std::unique_ptr<org::Execution> m_preparedFrame;  // prepared by Update, submitted by Render
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
	std::unique_ptr<Environment> m_currentEnvironment;
    std::shared_ptr<org::PixelBuffer> m_defaultEnvironmentCubemap = nullptr;
    std::shared_ptr<org::PixelBuffer> m_defaultEnvironmentPrefilteredCubemap = nullptr;
    std::shared_ptr<org::PixelBuffer> m_blueNoiseTexture = nullptr;
    std::shared_ptr<org::PixelBuffer> m_brdfLut;
    OpenPBRLookupResources m_openPBRLookupResources;
    bool m_warnedUsingFallbackEnvironment = false;
    bool m_warnedNullScene = false;
    bool m_warnedMissingPrimaryCamera = false;

    // GPU resource managers
    std::unique_ptr<LightManager> m_pLightManager;
    std::unique_ptr<MeshManager> m_pMeshManager;
    std::unique_ptr<ObjectManager> m_pObjectManager;
    std::unique_ptr<IndirectCommandBufferManager> m_pIndirectCommandBufferManager;
    std::unique_ptr<ViewManager> m_pViewManager;
    br::render::DepthHistoryPublicationService m_depthHistory;
	std::unique_ptr<EnvironmentManager> m_pEnvironmentManager;
	br::render::EnvironmentWorkServices m_environmentWorkServices;
    std::unique_ptr<MaterialManager> m_pMaterialManager;
	std::shared_ptr<SkeletonManager> m_pSkeletonManager = nullptr;
    std::unique_ptr<TerrainManager> m_pTerrainManager;
    std::unique_ptr<br::ReadbackManager> m_pReadbackManager;
    std::unique_ptr<TextureFactory> m_pTextureFactory;
    std::shared_ptr<br::render::CLodRayTracingSystem> m_clodRayTracingSystem = nullptr;
    // 0 raster, 1 raytraced, 2 raster with agreement against rays counted. See "rayTracedPrimaryVisibilityMode".
    uint32_t m_rayTracedPrimaryVisibilityMode = 0u;
    bool m_rayTracedShadows = false;
    // The renderer's versioned-buffer retirement clock and pools (advanced by frame-slot and graph retirement).
    std::shared_ptr<br::render::VersionedBufferRuntime> m_versionedBuffers;
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
    // The BRDF integration LUT, rendered on first use by a one-shot job graph.
    std::shared_ptr<org::PixelBuffer> EnsureBrdfLut();
    void CreateRenderGraph();
    void ApplyPendingPipelineReplacement();
    br::pipeline::PipelineRecipe GetPipelineRecipeForMutation() const;
    void RegisterPipelineExtensions();
    void HandlePipelineReplacementFailure(const std::exception& error);
    // After a graph built with the current recipe: drops what only a failed replacement could have needed.
    void ConfirmPipelineReplacement();
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
    void MaybeRequestCLodVirtualShadowFrameTelemetry();
    void MaybeRequestObjectReyesAtlasTelemetry();
    void MaybeRequestTerrainRvtTelemetry();
    void ApplyWindowResolutionPreset(WindowResolutionPreset preset);
    // Sets the upscaler up again and rebuilds the graph when UpscalingManager asks: a Ray Reconstruction retry.
    void ReconfigureUpscalerIfRequested();

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
    bool m_pathTracing = false;
    std::shared_ptr<PathTraceState> m_pathTraceState; // The current graph's, when it path traces.
    std::shared_ptr<br::lens::LensManager> m_lensManager; // The path tracer's physical lens.
    // What switching path tracing off goes back to. A successful replacement clears the rollback copy, so it can't serve.
    std::optional<br::pipeline::PipelineRecipe> m_rasterRecipeBeforePathTracing;
    // The upscaler raster used, for when path tracing (which selects DLSS Ray Reconstruction) is switched off.
    std::optional<UpscalingMode> m_upscalingModeBeforePathTracing;
    // Path tracing denoises one sample per pixel with DLSS Ray Reconstruction (falling back to DLSS SR without it), unless
    // it's accumulating; leaving it restores the upscaler from before.
    void SelectPathTracingUpscaler(bool pathTracing);
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
    std::shared_ptr<CLodVirtualShadowFrameTelemetryState> m_clodVirtualShadowFrameTelemetry;
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
