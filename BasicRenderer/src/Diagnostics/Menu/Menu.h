#pragma once


#include <directx/d3d12.h>
#include <rhi.h>
#include <rhi_interop_dx12.h>
#include <rhi_imgui_widgets.h>
#include <memory>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#if BASICRHI_ENABLE_VULKAN && __has_include(<imgui_impl_vulkan.h>) && __has_include(<vulkan/vulkan.h>)
#define BASICRENDERER_HAS_IMGUI_VULKAN 1
#ifndef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES 1
#endif
#include <imgui_impl_vulkan.h>
#include <volk.h>
#include <rhi_interop_vulkan.h>
#else
#define BASICRENDERER_HAS_IMGUI_VULKAN 0
#endif
#include <implot.h>
#include <functional>
#include <spdlog/spdlog.h>
#include <windows.h>
#include <filesystem>
#include <flecs.h>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "BasicRenderer/Extensions/RenderContext.h"
#include "Utilities/Utilities.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include "BasicRenderer/Assets/Import/ModelLoader.h"
#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "PostProcessing/ToneMapping/TonemapTypes.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "DebugUI/RenderGraphInspector.h"
#include "DebugUI/MemoryIntrospectionWidget.h"
#include "Resources/ReadbackRequest.h"
#include <BasicRenderer/Extensions/ResourceComponent.h>
#include "Render/MemoryIntrospectionAPI.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "VirtualGeometry/GraphIntegration/CLodExtensionComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "VirtualGeometry/RayTracing/CLodRayTracingSystem.h"
#include "BasicRenderer/Diagnostics/CLodTelemetry.h"
#include "Diagnostics/Telemetry/FrameTaskGraphTelemetry.h"
#include "Scene/ECS/RendererECSManager.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct PreparedImGuiDrawData {
    ImDrawData drawData{};
    std::vector<std::unique_ptr<ImDrawList>> lists;
    rhi::Backend backend = rhi::Backend::Null;
    rhi::DescriptorHeapHandle resourceHeap{};

    PreparedImGuiDrawData(const ImDrawData& source, rhi::Backend selectedBackend,
        rhi::DescriptorHeapHandle heap);
    PreparedImGuiDrawData(const PreparedImGuiDrawData&) = delete;
    PreparedImGuiDrawData& operator=(const PreparedImGuiDrawData&) = delete;
};

class Menu {
public:
    static Menu& GetInstance();

    void Initialize(HWND hwnd, rhi::Swapchain swapChain);
    void Render(const RenderContext& context, rhi::CommandList commandList);
    std::shared_ptr<const PreparedImGuiDrawData> PrepareDrawData(const RenderContext& context);
    static void RecordPreparedDrawData(const PreparedImGuiDrawData& data,
        rhi::CommandList commandList, rhi::DescriptorSlot rtv,
        DirectX::XMUINT2 outputResolution);
    bool HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void SetRenderGraph(org::RenderGraph* renderGraph) { m_renderGraph = renderGraph; }
    void Cleanup();

    // ImGui descriptor heap allocator for user textures (slot 0 reserved for font atlas).
    uint32_t AllocateImGuiDescriptor();
    void FreeImGuiDescriptor(uint32_t index);
    ImTextureID GetImGuiGpuDescriptorHandle(uint32_t index);
    rhi::DescriptorHeapHandle GetImGuiHeapHandle() const {
		if (!g_pd3dSrvDescHeap) {
			return {};
		}
        return g_pd3dSrvDescHeap->GetHandle();
    }

private:
    static constexpr uint32_t kImGuiHeapCapacity = 64;
    rhi::DescriptorHeapPtr g_pd3dSrvDescHeap;
    uint64_t imguiHeapGpuStart_ = 0;
    uint32_t imguiHeapIncrementSize_ = 0;
    uint32_t imguiHeapNextSlot_ = 1; // slot 0 = font atlas
    std::queue<uint32_t> imguiHeapFreeSlots_;
#if BASICRENDERER_HAS_IMGUI_VULKAN
    std::unordered_map<uint32_t, VkDescriptorSet> imguiVkTextureIds_;
#endif
    std::mutex imguiHeapMutex_;
    rhi::Backend m_imguiBackend = rhi::Backend::Null;
    bool m_imguiWin32Initialized = false;
#if BASICRENDERER_HAS_IMGUI_VULKAN
    VkFormat m_imguiVkColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfoKHR m_imguiVkRenderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
    VkDevice m_imguiVkDevice = VK_NULL_HANDLE;
    VkSampler m_imguiVkPreviewSampler = VK_NULL_HANDLE;
#endif

    Menu();

    struct SceneExplorerPendingEdit {
        bool hasPosition = false;
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        bool hasUniformScale = false;
        float uniformScale = 1.0f;
    };

    struct SceneExplorerNodeSnapshot {
        uint64_t stableId = 0;
        std::string name;
        bool hasPosition = false;
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        bool hasScale = false;
        float uniformScale = 1.0f;
        bool hasRotation = false;
        DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
        bool isRenderable = false;
        size_t meshCount = 0;
        bool skinned = false;
        std::vector<SceneExplorerNodeSnapshot> children;
    };

    uint64_t m_selectedSceneNodeStableId = 0;
    bool m_sceneExplorerSnapshotAvailable = false;
    bool m_sceneExplorerSnapshotTruncated = false;
    size_t m_sceneExplorerSnapshotNodeBudget = 0;
    SceneExplorerNodeSnapshot m_sceneExplorerRootSnapshot{};
    std::unordered_map<uint64_t, SceneExplorerPendingEdit> m_sceneExplorerPendingEdits;

	org::RenderGraph* m_renderGraph = nullptr;

    struct CLodCaptureStats {
        uint32_t visibleClusterCount = 0;
        uint32_t uniqueViews = 0;
        uint32_t uniqueInstances = 0;
        uint32_t uniqueMeshlets = 0;
        uint32_t maxClustersPerView = 0;
        uint32_t maxClustersPerInstance = 0;
        float avgClustersPerView = 0.0f;
        float avgClustersPerInstance = 0.0f;
        float dominantViewPercent = 0.0f;
        float dominantInstancePercent = 0.0f;
    };

    struct CLodWorkGraphCaptureState {
        CLodWorkGraphTelemetryCounters counters{};
        bool hasData = false;
        bool capturePending = false;
        uint64_t captureCount = 0;
        std::string status = "No captures yet.";

        bool captureStatsPending = false;
        uint64_t captureStatsId = 0;
        bool captureHasPendingCounter = false;
        bool captureHasPendingClusters = false;
        uint32_t capturePendingVisibleCount = 0;
        std::vector<VisibleCluster> capturePendingClusters;
        bool captureStatsAvailable = false;
        CLodCaptureStats captureStats{};
    };

    CLodWorkGraphCaptureState m_clodTelemetry;
    CLodWorkGraphCaptureState m_shadowClodTelemetry;
    uint64_t m_directionalShadowDebugLastSequence = 0;
    CLodDirectionalShadowDebugSnapshot m_directionalShadowDebugLatest{};

    struct CLodVirtualShadowCaptureState {
        CLodVirtualShadowStats stats{};
        CLodVirtualShadowRuntimeState runtimeState{};
        bool hasData = false;
        bool capturePending = false;
        bool captureHasPendingStats = false;
        bool captureHasPendingRuntimeState = false;
        uint64_t captureId = 0;
        uint64_t captureCount = 0;
        std::string status = "No VSM captures yet.";
    };

    CLodVirtualShadowCaptureState m_shadowVirtualShadowTelemetry;

    bool m_clodReyesTelemetryHasData = false;
    bool m_clodReyesTelemetryCapturePending = false;
    uint64_t m_clodReyesTelemetryCaptureId = 0;
    uint64_t m_clodReyesTelemetryCaptureCount = 0;
    bool m_clodReyesTelemetryHasPendingPhase1 = false;
    bool m_clodReyesTelemetryHasPendingPhase2 = false;
    CLodReyesTelemetry m_clodReyesTelemetryPendingPhase1{};
    CLodReyesTelemetry m_clodReyesTelemetryPendingPhase2{};
    CLodReyesTelemetry m_clodReyesTelemetryPhase1{};
    CLodReyesTelemetry m_clodReyesTelemetryPhase2{};
    std::string m_clodReyesTelemetryStatus = "No Reyes captures yet.";

    bool m_shadowClodReyesTelemetryHasData = false;
    bool m_shadowClodReyesTelemetryCapturePending = false;
    uint64_t m_shadowClodReyesTelemetryCaptureId = 0;
    uint64_t m_shadowClodReyesTelemetryCaptureCount = 0;
    CLodReyesTelemetry m_shadowClodReyesTelemetryPhase1{};
    std::string m_shadowClodReyesTelemetryStatus = "No shadow Reyes captures yet.";

    struct CLodStreamingOpsHistorySample {
        std::chrono::steady_clock::time_point timestamp;
        CLodStreamingOperationStats stats{};
    };

    uint64_t m_clodStreamingOpsLastSequence = 0;
    CLodStreamingOperationStats m_clodStreamingOpsLatest{};
    std::vector<CLodStreamingOpsHistorySample> m_clodStreamingOpsHistory;

    uint64_t m_frameTaskGraphLastSequence = 0;
    br::telemetry::FrameTaskGraphSnapshot m_frameTaskGraphLatest{};
    bool m_frameTaskGraphHasData = false;
    std::vector<br::telemetry::FrameTaskGraphSnapshot> m_frameTaskGraphHistory;
    int m_frameTaskGraphAverageWindow = 30;
    bool m_frameTaskGraphPaused = false;
    br::render::SceneOverlapStatus m_sceneOverlapStatus{};

    bool m_clodAlphaTelemetryHasData = false;
    bool m_clodAlphaTelemetryCapturePending = false;
    uint64_t m_clodAlphaTelemetryCaptureId = 0;
    bool m_clodAlphaTelemetryHasPendingNodeCount = false;
    bool m_clodAlphaTelemetryHasPendingOverflow = false;
    bool m_clodAlphaTelemetryHasPendingStats = false;
    uint32_t m_clodAlphaTelemetryPendingNodeCount = 0;
    uint32_t m_clodAlphaTelemetryPendingOverflow = 0;
    uint32_t m_clodAlphaNodeCount = 0;
    uint32_t m_clodAlphaOverflowCount = 0;
    CLodDeepVisibilityStats m_clodAlphaTelemetryPendingStats{};
    CLodDeepVisibilityStats m_clodAlphaStats{};
    std::string m_clodAlphaTelemetryStatus = "No alpha captures yet.";

    flecs::query<const Components::Resource> m_telemetryQuery;
    flecs::query<const Components::Resource> m_shadowTelemetryQuery;
    flecs::query<const Components::Resource> m_reyesTelemetryPhase1Query;
    flecs::query<const Components::Resource> m_reyesTelemetryPhase2Query;
    flecs::query<const Components::Resource> m_shadowReyesTelemetryPhase1Query;
    flecs::query<const Components::Resource> m_visibleClustersQuery;
    flecs::query<const Components::Resource> m_visibleCounterQuery;
    flecs::query<const Components::Resource> m_shadowVisibleClustersQuery;
    flecs::query<const Components::Resource> m_shadowVisibleCounterQuery;
    flecs::query<const Components::Resource> m_shadowVirtualShadowStatsQuery;
    flecs::query<const Components::Resource> m_shadowVirtualShadowRuntimeStateQuery;
    flecs::query<const Components::Resource> m_alphaDeepVisibilityCounterQuery;
    flecs::query<const Components::Resource> m_alphaDeepVisibilityOverflowQuery;
    flecs::query<const Components::Resource> m_alphaDeepVisibilityStatsQuery;

    int FindFileIndex(const std::vector<std::string>& hdrFiles, const std::string& existingFile);
    void DrawCLodTelemetryWindow();
    void DrawFrameTaskGraphWindow();
    void DrawAutoAliasPlannerWindow();
    void TryFinalizeCLodCaptureStats(CLodWorkGraphCaptureState& captureState, uint64_t captureId, const char* captureLabel);
    void TryFinalizeCLodVirtualShadowCapture(uint64_t captureId);
    void TryFinalizeCLodReyesTelemetryCapture(uint64_t captureId);
    void TryFinalizeCLodAlphaTelemetryCapture(uint64_t captureId);

    void DrawEnvironmentsDropdown();
	void DrawOutputTypeDropdown();
    void DrawWindowResolutionCombo();
    void DrawCLodLodHeightModeCombo();
    void DrawUpscalingCombo();
    void DrawUpscalingQualityCombo();
    void DrawTonemapTypeDropdown();
    void DrawBrowseButton(const std::wstring& targetDirectory);
    void DrawLoadModelButton();
    SceneExplorerNodeSnapshot BuildSceneExplorerSnapshot(flecs::entity node, size_t& remainingNodes, bool& truncated);
    const SceneExplorerNodeSnapshot* FindSceneExplorerSnapshotNode(const SceneExplorerNodeSnapshot& node, uint64_t stableId) const;
    SceneExplorerNodeSnapshot* FindSceneExplorerSnapshotNode(SceneExplorerNodeSnapshot& node, uint64_t stableId);
    void RefreshSceneExplorerSnapshot(size_t maxNodes);
    void OverlayPendingSceneExplorerEdits();
    void QueueSceneNodePositionChange(uint64_t stableId, const DirectX::XMFLOAT3& position);
    void QueueSceneNodeUniformScaleChange(uint64_t stableId, float uniformScale);
    void DisplaySceneNode(const SceneExplorerNodeSnapshot& node, bool isOnlyChild);
    void DisplaySceneGraph();
    void DisplaySelectedNode();
    void DrawPassTimingWindow();

    std::chrono::steady_clock::time_point m_startTime = std::chrono::steady_clock::now();

	bool m_meshShadersSupported = false;
    bool m_menuEnabled = true;
    
    std::filesystem::path environmentsDir;

    std::string environmentName;
    std::vector<std::string> hdrFiles;

	std::function<std::string()> getEnvironmentName;
	std::function<void(std::string)> setEnvironment;

	bool imageBasedLightingEnabled = false;
    std::function<bool()> getImageBasedLightingEnabled;
    std::function<void(bool)> setImageBasedLightingEnabled;

	bool punctualLightingEnabled = false;
	std::function<bool()> getPunctualLightingEnabled;
	std::function<void(bool)> setPunctualLightingEnabled;

    bool shadowsEnabled = false;
	std::function<bool()> getShadowsEnabled;
	std::function<void(bool)> setShadowsEnabled;

	std::function<void(unsigned int)> setOutputType;
    std::function<void(unsigned int)> setTonemapType;
	std::function<unsigned int()> getTonemapType;

    bool meshShaderEnabled = false;
    bool indirectDrawsWereEnabled = false;
    std::function<bool()> getMeshShaderEnabled;
	std::function<void(bool)> setMeshShaderEnabled;

	bool indirectDrawsEnabled = false;
	std::function<bool()> getIndirectDrawsEnabled;
	std::function<void(bool)> setIndirectDrawsEnabled;

	bool occlusionCulling = true;
	std::function<bool()> getOcclusionCullingEnabled;
	std::function<void(bool)> setOcclusionCullingEnabled;

    bool m_clodFrustumCulling = true;
    std::function<bool()> getCLodFrustumCulling;
    std::function<void(bool)> setCLodFrustumCulling;

    CLodCullingBackend m_clodCullingBackend = CLodCullingBackend::WorkGraph;
    std::function<CLodCullingBackend()> getCLodCullingBackend;
    std::function<void(CLodCullingBackend)> setCLodCullingBackend;

    uint32_t m_clodPureComputePhase2ExpansionFactor = CLodPureComputePhase2ExpansionFactorDefault;
    std::function<uint32_t()> getCLodPureComputePhase2ExpansionFactor;
    std::function<void(uint32_t)> setCLodPureComputePhase2ExpansionFactor;

    CLodSoftwareRasterMode m_clodSoftwareRasterMode = CLodSoftwareRasterMode::Disabled;
    std::function<CLodSoftwareRasterMode()> getCLodSoftwareRasterMode;
    std::function<void(CLodSoftwareRasterMode)> setCLodSoftwareRasterMode;

    CLodVSMRasterMode m_clodVSMRasterMode = CLodVSMRasterMode::PageJob;
    std::function<CLodVSMRasterMode()> getCLodVSMRasterMode;
    std::function<void(CLodVSMRasterMode)> setCLodVSMRasterMode;

    CLodTransparencyMode m_clodTransparencyMode = CLodTransparencyMode::LinkedListDeepVisibility;
    std::function<CLodTransparencyMode()> getCLodTransparencyMode;
    std::function<void(CLodTransparencyMode)> setCLodTransparencyMode;

    bool m_clodDisableReyesRasterization = false;
    std::function<bool()> getCLodDisableReyesRasterization;
    std::function<void(bool)> setCLodDisableReyesRasterization;

    bool m_clodReyesGeometricNormal = true;
    std::function<bool()> getCLodReyesGeometricNormal;
    std::function<void(bool)> setCLodReyesGeometricNormal;

    float m_clodReyesObjectNormalMapBlend = CLodReyesObjectNormalMapBlendDefault;
    std::function<float()> getCLodReyesObjectNormalMapBlend;
    std::function<void(float)> setCLodReyesObjectNormalMapBlend;

    float m_clodReyesTerrainNormalBlend = CLodReyesTerrainNormalBlendDefault;
    std::function<float()> getCLodReyesTerrainNormalBlend;
    std::function<void(float)> setCLodReyesTerrainNormalBlend;

    int m_clodReyesTerrainNormalMipBias = static_cast<int>(CLodReyesTerrainNormalMipBiasDefault);
    std::function<uint32_t()> getCLodReyesTerrainNormalMipBias;
    std::function<void(uint32_t)> setCLodReyesTerrainNormalMipBias;

    float m_clodReyesDiceRatePixels = CLodReyesDiceRatePixelsDefault;
    std::function<float()> getCLodReyesDiceRatePixels;
    std::function<void(float)> setCLodReyesDiceRatePixels;

    bool m_clodReyesUseAabbOcclusion = false;
    std::function<bool()> getCLodReyesUseAabbOcclusion;
    std::function<void(bool)> setCLodReyesUseAabbOcclusion;

    bool m_clodDisableVirtualShadowPageCaching = false;
    std::function<bool()> getCLodDisableVirtualShadowPageCaching;
    std::function<void(bool)> setCLodDisableVirtualShadowPageCaching;

    bool m_clodEnablePageJobVSM = false;
    std::function<bool()> getCLodEnablePageJobVSM;
    std::function<void(bool)> setCLodEnablePageJobVSM;

    float m_clodReyesShadowCoarseTargetPagesPerTriangle = CLodReyesShadowCoarseTargetPagesPerTriangleDefault;
    std::function<float()> getCLodReyesShadowCoarseTargetPagesPerTriangle;
    std::function<void(float)> setCLodReyesShadowCoarseTargetPagesPerTriangle;

    uint32_t m_clodPageJobDiameterThreshold = 64u;
    std::function<uint32_t()> getCLodPageJobDiameterThreshold;
    std::function<void(uint32_t)> setCLodPageJobDiameterThreshold;

    float m_clodPageJobSparseRatio = 0.5f;
    std::function<float()> getCLodPageJobSparseRatio;
    std::function<void(float)> setCLodPageJobSparseRatio;

    uint32_t m_clodPageJobMaxPagesPerCluster = 32u;
    std::function<uint32_t()> getCLodPageJobMaxPagesPerCluster;
    std::function<void(uint32_t)> setCLodPageJobMaxPagesPerCluster;

    uint32_t m_clodPageJobRecordCapacity = CLodPageJobDefaultRecordCapacity;
    std::function<uint32_t()> getCLodPageJobRecordCapacity;
    std::function<void(uint32_t)> setCLodPageJobRecordCapacity;

    bool m_clodPageJobForceAll = false;
    std::function<bool()> getCLodPageJobForceAll;
    std::function<void(bool)> setCLodPageJobForceAll;

    uint32_t m_clodForceTraversalDepthRoot = CLodForceTraversalDepthRootDisabled;
    std::function<uint32_t()> getCLodForceTraversalDepthRoot;
    std::function<void(uint32_t)> setCLodForceTraversalDepthRoot;

    uint32_t m_clodVisibleClusterCapacity = CLodDefaultVisibleClusterCapacity;
    std::function<uint32_t()> getCLodVisibleClusterCapacity;
    std::function<void(uint32_t)> setCLodVisibleClusterCapacity;

    uint32_t m_clodDirectionalVirtualShadowMaxBackingResolution = CLodVirtualShadowDefaultBackingResolution;
    std::function<uint32_t()> getCLodDirectionalVirtualShadowMaxBackingResolution;
    std::function<void(uint32_t)> setCLodDirectionalVirtualShadowMaxBackingResolution;

    uint32_t m_clodDirectionalVirtualShadowMaxPhysicalPages = CLodVirtualShadowDefaultPhysicalPageCount;
    std::function<uint32_t()> getCLodDirectionalVirtualShadowMaxPhysicalPages;
    std::function<void(uint32_t)> setCLodDirectionalVirtualShadowMaxPhysicalPages;

    float m_clodDirectionalVirtualShadowLodBias = CLodVirtualShadowDefaultDirectionalLodBias;
    std::function<float()> getCLodDirectionalVirtualShadowLodBias;
    std::function<void(float)> setCLodDirectionalVirtualShadowLodBias;

    bool m_clodDirectionalVirtualShadowAutoLodBias = true;
    std::function<bool()> getCLodDirectionalVirtualShadowAutoLodBias;
    std::function<void(bool)> setCLodDirectionalVirtualShadowAutoLodBias;

    float m_clodDirectionalVirtualShadowAutoLodBiasScale = 1.0f;
    std::function<float()> getCLodDirectionalVirtualShadowAutoLodBiasScale;
    std::function<void(float)> setCLodDirectionalVirtualShadowAutoLodBiasScale;

    bool m_clodDirectionalVirtualShadowPredictiveLodInvalidation = false;
    std::function<bool()> getCLodDirectionalVirtualShadowPredictiveLodInvalidation;
    std::function<void(bool)> setCLodDirectionalVirtualShadowPredictiveLodInvalidation;

    float m_clodDirectionalVirtualShadowSourceAngleDegrees = CLodVirtualShadowDefaultDirectionalSourceAngleDegrees;
    std::function<float()> getCLodDirectionalVirtualShadowSourceAngleDegrees;
    std::function<void(float)> setCLodDirectionalVirtualShadowSourceAngleDegrees;

    uint32_t m_clodDirectionalVirtualShadowSmrtRayCountDirectional = CLodVirtualShadowDefaultSmrtRayCountDirectional;
    std::function<uint32_t()> getCLodDirectionalVirtualShadowSmrtRayCountDirectional;
    std::function<void(uint32_t)> setCLodDirectionalVirtualShadowSmrtRayCountDirectional;

    uint32_t m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = CLodVirtualShadowDefaultSmrtSamplesPerRayDirectional;
    std::function<uint32_t()> getCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional;
    std::function<void(uint32_t)> setCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional;

    float m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees = CLodVirtualShadowDefaultSmrtMaxRayAngleFromLightDegrees;
    std::function<float()> getCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees;
    std::function<void(float)> setCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees;

    float m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional = CLodVirtualShadowDefaultSmrtRayLengthScaleDirectional;
    std::function<float()> getCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional;
    std::function<void(float)> setCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional;

    float m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld = CLodVirtualShadowDefaultSmrtMaxTraceDistanceWorld;
    std::function<float()> getCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld;
    std::function<void(float)> setCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld;

    bool m_clodDirectionalVirtualShadowReceiverTraceEnabled = CLodVirtualShadowDefaultReceiverTraceEnabled;
    std::function<bool()> getCLodDirectionalVirtualShadowReceiverTraceEnabled;
    std::function<void(bool)> setCLodDirectionalVirtualShadowReceiverTraceEnabled;

    uint32_t m_clodDirectionalVirtualShadowReceiverTraceSampleCount = CLodVirtualShadowDefaultReceiverTraceSampleCount;
    std::function<uint32_t()> getCLodDirectionalVirtualShadowReceiverTraceSampleCount;
    std::function<void(uint32_t)> setCLodDirectionalVirtualShadowReceiverTraceSampleCount;

    float m_clodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld = CLodVirtualShadowDefaultReceiverTraceMaxDistanceWorld;
    std::function<float()> getCLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld;
    std::function<void(float)> setCLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorld;

    float m_clodDirectionalVirtualShadowReceiverTraceUncertaintyScale = CLodVirtualShadowDefaultReceiverTraceUncertaintyScale;
    std::function<float()> getCLodDirectionalVirtualShadowReceiverTraceUncertaintyScale;
    std::function<void(float)> setCLodDirectionalVirtualShadowReceiverTraceUncertaintyScale;

    float m_clodDirectionalVirtualShadowReceiverTraceDepthSafetyScale = CLodVirtualShadowDefaultReceiverTraceDepthSafetyScale;
    std::function<float()> getCLodDirectionalVirtualShadowReceiverTraceDepthSafetyScale;
    std::function<void(float)> setCLodDirectionalVirtualShadowReceiverTraceDepthSafetyScale;

    uint8_t m_numDirectionalLightCascades = 0u;
    std::function<uint8_t()> getNumDirectionalLightCascades;
    std::function<void(uint8_t)> setNumDirectionalLightCascades;

    float m_directionalShadowDistanceLowerBound = 0.0f;
    std::function<float()> getDirectionalShadowDistanceLowerBound;
    std::function<void(float)> setDirectionalShadowDistanceLowerBound;

    bool wireframeEnabled = false;
	std::function<bool()> getWireframeEnabled;
	std::function<void(bool)> setWireframeEnabled;

    std::function<flecs::entity ()> getSceneRoot;
    std::function<void(uint64_t, DirectX::XMFLOAT3)> queueSceneNodePositionEdit;
    std::function<void(uint64_t, float)> queueSceneNodeUniformScaleEdit;

    bool allowTearing = false;
	std::function<bool()> getAllowTearing;
    std::function<void(bool)> setAllowTearing;

    bool drawBoundingSpheres = false;
	std::function<bool()> getDrawBoundingSpheres;
	std::function<void(bool)> setDrawBoundingSpheres;

    bool clusteredLighting = true;
	std::function<bool()> getClusteredLightingEnabled;
	std::function<void(bool)> setClusteredLightingEnabled;

	bool m_visibilityRenderingEnabled = true;
	std::function<bool()> getVisibilityRenderingEnabled;
	std::function<void(bool)> setVisibilityRenderingEnabled;
    bool m_terrainRegionMaterialEvaluationEnabled = false;
    std::function<bool()> getTerrainRegionMaterialEvaluationEnabled;
    std::function<void(bool)> setTerrainRegionMaterialEvaluationEnabled;
    bool m_terrainRvtEnabled = false;
    std::function<bool()> getTerrainRvtEnabled;
    std::function<void(bool)> setTerrainRvtEnabled;
    bool m_forceDirectTerrainRvtFallback = false;
    std::function<bool()> getForceDirectTerrainRvtFallback;
    std::function<void(bool)> setForceDirectTerrainRvtFallback;
    bool m_terrainRvtTelemetryDebug = false;
    std::function<bool()> getTerrainRvtTelemetryDebug;
    std::function<void(bool)> setTerrainRvtTelemetryDebug;
    int m_terrainRvtDebugView = 0;
    std::function<uint32_t()> getTerrainRvtDebugView;
    std::function<void(uint32_t)> setTerrainRvtDebugView;
    int m_terrainRvtPageSize = 128;
    std::function<uint32_t()> getTerrainRvtPageSize;
    std::function<void(uint32_t)> setTerrainRvtPageSize;
    int m_terrainRvtBorderTexels = 4;
    std::function<uint32_t()> getTerrainRvtBorderTexels;
    std::function<void(uint32_t)> setTerrainRvtBorderTexels;
    int m_terrainRvtMipCount = 14;
    std::function<uint32_t()> getTerrainRvtMipCount;
    std::function<void(uint32_t)> setTerrainRvtMipCount;
    float m_terrainRvtMipOffset = 0.0f;
    std::function<float()> getTerrainRvtMipOffset;
    std::function<void(float)> setTerrainRvtMipOffset;
    float m_terrainRvtSourceTexelsPerWorld = 24.0f;
    std::function<float()> getTerrainRvtSourceTexelsPerWorld;
    std::function<void(float)> setTerrainRvtSourceTexelsPerWorld;
    int m_terrainRvtPhysicalAtlasPagesWide = 32;
    std::function<uint32_t()> getTerrainRvtPhysicalAtlasPagesWide;
    std::function<void(uint32_t)> setTerrainRvtPhysicalAtlasPagesWide;
    int m_terrainRvtPhysicalAtlasPagesHigh = 32;
    std::function<uint32_t()> getTerrainRvtPhysicalAtlasPagesHigh;
    std::function<void(uint32_t)> setTerrainRvtPhysicalAtlasPagesHigh;
    int m_terrainRvtPhysicalAtlasPoolCount = 1;
    std::function<uint32_t()> getTerrainRvtPhysicalAtlasPoolCount;
    std::function<void(uint32_t)> setTerrainRvtPhysicalAtlasPoolCount;

    bool m_terrainStochasticSamplingEnabled = true;
    std::function<bool()> getTerrainStochasticSamplingEnabled;
    std::function<void(bool)> setTerrainStochasticSamplingEnabled;
    bool m_terrainStochasticDiffuseSamplingEnabled = true;
    std::function<bool()> getTerrainStochasticDiffuseSamplingEnabled;
    std::function<void(bool)> setTerrainStochasticDiffuseSamplingEnabled;
    bool m_terrainStochasticNormalSamplingEnabled = true;
    std::function<bool()> getTerrainStochasticNormalSamplingEnabled;
    std::function<void(bool)> setTerrainStochasticNormalSamplingEnabled;
    bool m_terrainStochasticDerivativeNormalSamplingEnabled = true;
    std::function<bool()> getTerrainStochasticDerivativeNormalSamplingEnabled;
    std::function<void(bool)> setTerrainStochasticDerivativeNormalSamplingEnabled;
    float m_terrainStochasticBlendCurve = 0.65f;
    std::function<float()> getTerrainStochasticBlendCurve;
    std::function<void(float)> setTerrainStochasticBlendCurve;
    bool m_terrainGaussianStochasticSamplingEnabled = false;
    std::function<bool()> getTerrainGaussianStochasticSamplingEnabled;
    std::function<void(bool)> setTerrainGaussianStochasticSamplingEnabled;
    bool m_parallaxOcclusionMappingEnabled = true;
    std::function<bool()> getParallaxOcclusionMappingEnabled;
    std::function<void(bool)> setParallaxOcclusionMappingEnabled;
    bool m_terrainParallaxOcclusionMappingEnabled = true;
    std::function<bool()> getTerrainParallaxOcclusionMappingEnabled;
    std::function<void(bool)> setTerrainParallaxOcclusionMappingEnabled;
    bool m_terrainReyesDisplacementEnabled = true;
    std::function<bool()> getTerrainReyesDisplacementEnabled;
    std::function<void(bool)> setTerrainReyesDisplacementEnabled;
    float m_terrainReyesDisplacementScale = 1.0f;
    std::function<float()> getTerrainReyesDisplacementScale;
    std::function<void(float)> setTerrainReyesDisplacementScale;
    float m_objectReyesDisplacementScale = 1.0f;
    std::function<float()> getObjectReyesDisplacementScale;
    std::function<void(float)> setObjectReyesDisplacementScale;
    float m_proceduralWindDisplacementScale = 1.0f;
    std::function<float()> getProceduralWindDisplacementScale;
    std::function<void(float)> setProceduralWindDisplacementScale;
    float m_proceduralWindGrassDisplacementScale = 1.0f;
    std::function<float()> getProceduralWindGrassDisplacementScale;
    std::function<void(float)> setProceduralWindGrassDisplacementScale;
    float m_proceduralWindGrassOscillationScale = 1.0f;
    std::function<float()> getProceduralWindGrassOscillationScale;
    std::function<void(float)> setProceduralWindGrassOscillationScale;
    float m_proceduralWindGrassFlutterFrequency = 1.0f;
    std::function<float()> getProceduralWindGrassFlutterFrequency;
    std::function<void(float)> setProceduralWindGrassFlutterFrequency;
    float m_proceduralWindEffectDistance = 10000.0f;
    std::function<void(float)> setProceduralWindEffectDistance;
    std::function<void(float)> setProceduralWindInnerRadius;
    float m_terrainParallaxHeightScale = 0.03f;
    std::function<float()> getTerrainParallaxHeightScale;
    std::function<void(float)> setTerrainParallaxHeightScale;
    float m_objectParallaxHeightScale = 1.0f;
    std::function<float()> getObjectParallaxHeightScale;
    std::function<void(float)> setObjectParallaxHeightScale;
    uint32_t m_terrainParallaxMaxSteps = 16u;
    std::function<uint32_t()> getTerrainParallaxMaxSteps;
    std::function<void(uint32_t)> setTerrainParallaxMaxSteps;
    float m_terrainParallaxFadeStartDistance = 2048.0f;
    std::function<float()> getTerrainParallaxFadeStartDistance;
    std::function<void(float)> setTerrainParallaxFadeStartDistance;
    float m_terrainParallaxFadeEndDistance = 8192.0f;
    std::function<float()> getTerrainParallaxFadeEndDistance;
    std::function<void(float)> setTerrainParallaxFadeEndDistance;

	bool m_gtaoEnabled = true;
	std::function<bool()> getGTAOEnabled;
	std::function<void(bool)> setGTAOEnabled;

	bool m_bloomEnabled = true;
	std::function<bool()> getBloomEnabled;
	std::function<void(bool)> setBloomEnabled;

	bool m_screenSpaceReflectionsEnabled = true;
	std::function<bool()> getScreenSpaceReflectionsEnabled;
	std::function<void(bool)> setScreenSpaceReflectionsEnabled;

    bool m_rayTracedReflectionsEnabled = false;
    std::function<bool()> getRayTracedReflectionsEnabled;
    std::function<void(bool)> setRayTracedReflectionsEnabled;

    bool m_jitterEnabled = true;
    std::function<bool()> getJitterEnabled;
    std::function<void(bool)> setJitterEnabled;

    bool m_rememberCameraPose = false;
    std::function<bool()> getRememberCameraPose;
    std::function<void(bool)> setRememberCameraPose;

    bool m_collectPassStatistics = true;
    std::function<bool()> getCollectPassStatistics;
    std::function<void(bool)> setCollectPassStatistics;
	bool m_collectPipelineStatistics = false;
	std::function<bool()> getCollectPipelineStatistics;
    std::function<void(bool)> setCollectPipelineStatistics;

	UpscalingMode m_currentUpscalingMode = UpscalingMode::None;
	std::function<UpscalingMode()> getUpscalingMode;
	std::function<void(UpscalingMode)> setUpscalingMode;
	bool m_dilatedMotionVectorsEnabled = true;
	std::function<bool()> getDilatedMotionVectorsEnabled;
	std::function<void(bool)> setDilatedMotionVectorsEnabled;

	UpscaleQualityMode m_currentUpscalingQualityMode = UpscaleQualityMode::Balanced;
	std::function<UpscaleQualityMode()> getUpscalingQualityMode;
    std::function<void(UpscaleQualityMode)> setUpscalingQualityMode;

    WindowResolutionPreset m_currentWindowResolutionPreset = WindowResolutionPreset::P1080;
    std::function<WindowResolutionPreset()> getWindowResolutionPreset;
    std::function<void(WindowResolutionPreset)> setWindowResolutionPreset;

    CLodLodHeightMode m_currentCLodLodHeightMode = CLodLodHeightMode::OutputHeight;
    std::function<CLodLodHeightMode()> getCLodLodHeightMode;
    std::function<void(CLodLodHeightMode)> setCLodLodHeightMode;

	bool m_useAsyncCompute = true;
	std::function<bool()> getUseAsyncCompute;
    std::function<void(bool)> setUseAsyncCompute;

	bool m_heavyDebug = false;
	std::function<bool()> getHeavyDebug;
	std::function<void(bool)> setHeavyDebug;

    bool m_renderGraphBatchTraceEnabled = false;
    std::function<bool()> getRenderGraphBatchTraceEnabled;
    std::function<void(bool)> setRenderGraphBatchTraceEnabled;

	bool m_renderGraphLightweightCompileSummaryEnabled = false;
	std::function<bool()> getRenderGraphLightweightCompileSummaryEnabled;
	std::function<void(bool)> setRenderGraphLightweightCompileSummaryEnabled;

    bool m_reshapeTexelAddressing = true;
    std::function<bool()> getReshapeTexelAddressing;
    std::function<void(bool)> setReshapeTexelAddressing;

    org::AutoAliasMode m_autoAliasMode = org::AutoAliasMode::Balanced;
    std::function<org::AutoAliasMode()> getAutoAliasMode;
    std::function<void(org::AutoAliasMode)> setAutoAliasMode;

    org::AutoAliasPackingStrategy m_autoAliasPackingStrategy = org::AutoAliasPackingStrategy::GreedySweepLine;
    std::function<org::AutoAliasPackingStrategy()> getAutoAliasPackingStrategy;
    std::function<void(org::AutoAliasPackingStrategy)> setAutoAliasPackingStrategy;

    bool m_autoAliasLogExclusionReasons = false;
    std::function<bool()> getAutoAliasLogExclusionReasons;
    std::function<void(bool)> setAutoAliasLogExclusionReasons;
    std::function<void(bool)> setAutoAliasBuildDebugData;

    uint32_t m_autoAliasPoolRetireIdleFrames = 120;
    std::function<uint32_t()> getAutoAliasPoolRetireIdleFrames;
    std::function<void(uint32_t)> setAutoAliasPoolRetireIdleFrames;

    uint32_t m_clodStreamingCpuUploadBudgetRequests = 64;
    std::function<uint32_t()> getCLodStreamingCpuUploadBudgetRequests;
    std::function<void(uint32_t)> setCLodStreamingCpuUploadBudgetRequests;

    bool m_clodStreamingEnableDirectStorage = true;
    std::function<bool()> getCLodStreamingEnableDirectStorage;
    std::function<void(bool)> setCLodStreamingEnableDirectStorage;

    float m_autoAliasPoolGrowthHeadroom = 1.5f;
    std::function<float()> getAutoAliasPoolGrowthHeadroom;
    std::function<void(float)> setAutoAliasPoolGrowthHeadroom;

	std::function<std::shared_ptr<Scene>(std::shared_ptr<Scene>)> appendScene;
	std::vector<SettingsManager::Subscription> m_settingSubscriptions;
};

