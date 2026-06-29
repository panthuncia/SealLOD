#pragma once


#include <directx/d3d12.h>
#include <rhi.h>
#include <rhi_interop_dx12.h>
#include <rhi_imgui_widgets.h>
#include <memory>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#if __has_include(<imgui_impl_vulkan.h>) && __has_include(<vulkan/vulkan.h>)
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
#include <queue>
#include <unordered_map>

#include "Render/RenderContext.h"
#include "Utilities/Utilities.h"
#include "Render/OutputTypes.h"
#include "Import/ModelLoader.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Render/TonemapTypes.h"
#include "Managers/Singletons/UpscalingManager.h"
#include "DebugUI/RenderGraphInspector.h"
#include "DebugUI/MemoryIntrospectionWidget.h"
#include "Resources/ReadbackRequest.h"
#include "Resources/components.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "ShaderBuffers.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodRayTracingSystem.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Telemetry/FrameTaskGraphTelemetry.h"
#include "Managers/Singletons/RendererECSManager.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static inline const char* MajorCategory(rhi::ResourceType t) {
    using RT = rhi::ResourceType;
    switch (t) {
    case RT::Buffer:                return "Buffers";
    case RT::Texture1D:             return "Textures";
    case RT::Texture2D:             return "Textures";
    case RT::Texture3D:             return "Textures";
    case RT::AccelerationStructure: return "AccelStructs";
    default:                        return "Other";
    }
}

struct PerResourceMemInfo {
    uint64_t bytes = 0;
    std::string category; // "Textures/Material", etc.
    std::string name;     // optional
};

using PerResourceMemIndex = std::unordered_map<uint64_t, PerResourceMemInfo>;

static void BuildMemorySnapshotFromRecords(
    ui::MemorySnapshot& out,
    const std::vector<rg::memory::ResourceMemoryRecord>& records,
    PerResourceMemIndex* outIndex /*= nullptr*/)
{
    out.categories.clear();
    out.resources.clear();
    out.totalBytes = 0;

    std::unordered_map<std::string, uint64_t> minorBuckets;
    minorBuckets.reserve(256);

    if (outIndex) { outIndex->clear(); outIndex->reserve(2048); }

    for (const auto& record : records) {
        const uint64_t bytes = record.bytes;
        out.totalBytes += bytes;

        const char* major = MajorCategory(record.resourceType);

        const char* usage = !record.usage.empty() ? record.usage.c_str()
            : "Unspecified";

        const std::string cat = std::string(major) + "/" + usage;

        minorBuckets[cat] += bytes;

        ui::MemoryResourceRow row{};
        row.bytes = bytes;
        row.uid = record.resourceID;

        if (!record.resourceName.empty()) row.name = record.resourceName;
        else if (record.resourceID != 0) row.name = "Resource " + std::to_string((unsigned long long)record.resourceID);
        else row.name = "Unknown resource";

        row.type = cat;
        out.resources.push_back(row);

        if (outIndex && record.resourceID != 0) {
            auto& info = (*outIndex)[record.resourceID];
            info.bytes = bytes;
            info.category = cat;
            if (!record.resourceName.empty()) info.name = record.resourceName;
        }
    }

    out.categories.reserve(minorBuckets.size());
    for (auto& [label, bytes] : minorBuckets) {
        if (bytes) out.categories.push_back({ label, bytes });
    }

    std::sort(out.categories.begin(), out.categories.end(),
        [](auto const& a, auto const& b) { return a.bytes > b.bytes; });
}

struct MemInfo {
    uint64_t bytes = 0;
    std::string name; // from ResourceIdentifier.name (string ID)
};

static void BuildIdToMemInfoIndex(
    std::unordered_map<uint64_t, MemInfo>& out,
    const std::vector<rg::memory::ResourceMemoryRecord>& records)
{
    out.clear();
    out.reserve(2048);

    for (const auto& record : records) {
        if (record.resourceID == 0) {
            continue;
        }

        MemInfo info;
        info.bytes = record.bytes;

        if (!record.identifier.empty()) {
            info.name = record.identifier;
        }

        out[record.resourceID] = std::move(info);
    }
}

class Menu {
public:
    static Menu& GetInstance();

    void Initialize(HWND hwnd, rhi::Swapchain swapChain);
    void Render(const RenderContext& context, rhi::CommandList commandList);
    bool HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void SetRenderGraph(RenderGraph* renderGraph) { m_renderGraph = renderGraph; }
    void Cleanup() {
        if (m_imguiBackend == rhi::Backend::Vulkan) {
#if BASICRENDERER_HAS_IMGUI_VULKAN
            for (auto& entry : imguiVkTextureIds_) {
                ImGui_ImplVulkan_RemoveTexture(entry.second);
            }
            imguiVkTextureIds_.clear();
#endif
        }
        if (m_imguiBackend == rhi::Backend::D3D12) {
            ImGui_ImplDX12_Shutdown();
        }
#if BASICRENDERER_HAS_IMGUI_VULKAN
        if (m_imguiBackend == rhi::Backend::Vulkan) {
            ImGui_ImplVulkan_Shutdown();
            if (m_imguiVkPreviewSampler != VK_NULL_HANDLE && m_imguiVkDevice != VK_NULL_HANDLE) {
                vkDestroySampler(m_imguiVkDevice, m_imguiVkPreviewSampler, nullptr);
            }
        }
#endif
        if (m_imguiWin32Initialized) {
            ImGui_ImplWin32_Shutdown();
        }
        m_imguiBackend = rhi::Backend::Null;
        m_imguiWin32Initialized = false;
        g_pd3dSrvDescHeap.Reset();
        imguiHeapGpuStart_ = 0;
        imguiHeapIncrementSize_ = 0;
        imguiHeapNextSlot_ = 1;
        imguiHeapFreeSlots_ = {};
#if BASICRENDERER_HAS_IMGUI_VULKAN
        imguiVkTextureIds_.clear();
        m_imguiVkPreviewSampler = VK_NULL_HANDLE;
        m_imguiVkDevice = VK_NULL_HANDLE;
#endif
		m_settingSubscriptions.clear();
		m_telemetryQuery = {};
		m_visibleClustersQuery = {};
		m_visibleCounterQuery = {};
        m_alphaDeepVisibilityCounterQuery = {};
        m_alphaDeepVisibilityOverflowQuery = {};
        m_alphaDeepVisibilityStatsQuery = {};
		m_reyesTelemetryPhase1Query = {};
		m_reyesTelemetryPhase2Query = {};
        m_shadowReyesTelemetryPhase1Query = {};
        m_shadowTelemetryQuery = {};
        m_shadowVisibleCounterQuery = {};
        m_shadowVisibleClustersQuery = {};
        m_shadowVirtualShadowStatsQuery = {};
		m_shadowVirtualShadowRuntimeStateQuery = {};
    }

    // ImGui descriptor heap allocator for user textures (slot 0 reserved for font atlas).
    uint32_t AllocateImGuiDescriptor() {
        std::lock_guard lock(imguiHeapMutex_);
        if (!imguiHeapFreeSlots_.empty()) {
            uint32_t idx = imguiHeapFreeSlots_.front();
            imguiHeapFreeSlots_.pop();
            return idx;
        }
        if (imguiHeapNextSlot_ < kImGuiHeapCapacity) {
            return imguiHeapNextSlot_++;
        }
        throw std::runtime_error("ImGui descriptor heap exhausted");
    }
    void FreeImGuiDescriptor(uint32_t index) {
        if (index == 0) return; // never free the font atlas slot
#if BASICRENDERER_HAS_IMGUI_VULKAN
        if (m_imguiBackend == rhi::Backend::Vulkan) {
            auto textureIt = imguiVkTextureIds_.find(index);
            if (textureIt != imguiVkTextureIds_.end()) {
                ImGui_ImplVulkan_RemoveTexture(textureIt->second);
                imguiVkTextureIds_.erase(textureIt);
            }
        }
#endif
        std::lock_guard lock(imguiHeapMutex_);
        imguiHeapFreeSlots_.push(index);
    }
    ImTextureID GetImGuiGpuDescriptorHandle(uint32_t index) {
        if (m_imguiBackend == rhi::Backend::D3D12) {
        return static_cast<ImTextureID>(imguiHeapGpuStart_ + static_cast<uint64_t>(index) * imguiHeapIncrementSize_);
        }
#if BASICRENDERER_HAS_IMGUI_VULKAN
        if (m_imguiBackend == rhi::Backend::Vulkan && g_pd3dSrvDescHeap && m_imguiVkPreviewSampler != VK_NULL_HANDLE) {
            auto textureIt = imguiVkTextureIds_.find(index);
            if (textureIt != imguiVkTextureIds_.end()) {
                return (ImTextureID)textureIt->second;
            }

            auto device = DeviceManager::GetInstance().GetDevice();
            const rhi::DescriptorSlot slot{ g_pd3dSrvDescHeap->GetHandle(), index };
            const VkImageView imageView = rhi::vulkan::get_image_view(device, slot);
            if (imageView == VK_NULL_HANDLE) {
                return static_cast<ImTextureID>(0);
            }

            VkDescriptorSet textureSet = ImGui_ImplVulkan_AddTexture(
                m_imguiVkPreviewSampler,
                imageView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (textureSet == VK_NULL_HANDLE) {
                return static_cast<ImTextureID>(0);
            }

            imguiVkTextureIds_[index] = textureSet;
            return (ImTextureID)textureSet;
        }
#endif
        return static_cast<ImTextureID>(0);
    }
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

    Menu() { 
        ImGui::CreateContext();
		ImPlot::CreateContext();
    };

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

	RenderGraph* m_renderGraph = nullptr;

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

    uint8_t m_numDirectionalLightCascades = 0u;
    std::function<uint8_t()> getNumDirectionalLightCascades;
    std::function<void(uint8_t)> setNumDirectionalLightCascades;

    float m_directionalShadowVerticalExtent = 0.0f;
    std::function<float()> getDirectionalShadowVerticalExtent;
    std::function<void(float)> setDirectionalShadowVerticalExtent;

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

    bool m_collectPassStatistics = true;
    std::function<bool()> getCollectPassStatistics;
    std::function<void(bool)> setCollectPassStatistics;
	bool m_collectPipelineStatistics = false;
	std::function<bool()> getCollectPipelineStatistics;
    std::function<void(bool)> setCollectPipelineStatistics;

	UpscalingMode m_currentUpscalingMode = UpscalingMode::None;
	std::function<UpscalingMode()> getUpscalingMode;
	std::function<void(UpscalingMode)> setUpscalingMode;

	UpscaleQualityMode m_currentUpscalingQualityMode = UpscaleQualityMode::Balanced;
	std::function<UpscaleQualityMode()> getUpscalingQualityMode;
    std::function<void(UpscaleQualityMode)> setUpscalingQualityMode;

	bool m_useAsyncCompute = true;
	std::function<bool()> getUseAsyncCompute;
    std::function<void(bool)> setUseAsyncCompute;

	bool m_heavyDebug = false;
	std::function<bool()> getHeavyDebug;
	std::function<void(bool)> setHeavyDebug;

	bool m_renderGraphDisableCaching = false;
	std::function<bool()> getRenderGraphDisableCaching;
	std::function<void(bool)> setRenderGraphDisableCaching;

    bool m_renderGraphBatchTraceEnabled = false;
    std::function<bool()> getRenderGraphBatchTraceEnabled;
    std::function<void(bool)> setRenderGraphBatchTraceEnabled;

	bool m_renderGraphLightweightCompileSummaryEnabled = false;
	std::function<bool()> getRenderGraphLightweightCompileSummaryEnabled;
	std::function<void(bool)> setRenderGraphLightweightCompileSummaryEnabled;

    bool m_reshapeTexelAddressing = true;
    std::function<bool()> getReshapeTexelAddressing;
    std::function<void(bool)> setReshapeTexelAddressing;

    AutoAliasMode m_autoAliasMode = AutoAliasMode::Balanced;
    std::function<AutoAliasMode()> getAutoAliasMode;
    std::function<void(AutoAliasMode)> setAutoAliasMode;

    AutoAliasPackingStrategy m_autoAliasPackingStrategy = AutoAliasPackingStrategy::GreedySweepLine;
    std::function<AutoAliasPackingStrategy()> getAutoAliasPackingStrategy;
    std::function<void(AutoAliasPackingStrategy)> setAutoAliasPackingStrategy;

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

inline Menu& Menu::GetInstance() {
    static Menu instance;
    return instance;
}

inline bool Menu::HandleInput(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return static_cast<bool>(ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam));
}

inline void Menu::Initialize(HWND hwnd, rhi::Swapchain swapChain) {
	auto numFramesInFlight = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numFramesInFlight")();

    environmentsDir = std::filesystem::path(GetExePath()) / "textures" / "environment";

	auto device = DeviceManager::GetInstance().GetDevice();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    m_imguiWin32Initialized = true;

    if (DeviceManager::GetInstance().GetBackend() == rhi::Backend::D3D12) {
		auto result = device.CreateDescriptorHeap({ rhi::DescriptorHeapType::CbvSrvUav, kImGuiHeapCapacity, true }, g_pd3dSrvDescHeap);
		if (!rhi::IsOk(result) || !g_pd3dSrvDescHeap) {
			throw std::runtime_error("Menu::Initialize failed to create ImGui descriptor heap for DX12 backend");
		}

        ImGui_ImplDX12_Init(rhi::dx12::get_device(device), 
            numFramesInFlight,
            DXGI_FORMAT_R8G8B8A8_UNORM, 
            rhi::dx12::get_descriptor_heap(g_pd3dSrvDescHeap.Get()),
            rhi::dx12::get_descriptor_heap(g_pd3dSrvDescHeap.Get())->GetCPUDescriptorHandleForHeapStart(),
            rhi::dx12::get_descriptor_heap(g_pd3dSrvDescHeap.Get())->GetGPUDescriptorHandleForHeapStart());

        // Cache GPU start and increment size for user-texture descriptor allocation.
        imguiHeapGpuStart_ = rhi::dx12::get_descriptor_heap(g_pd3dSrvDescHeap.Get())->GetGPUDescriptorHandleForHeapStart().ptr;
        imguiHeapIncrementSize_ = device.GetDescriptorHandleIncrementSize(rhi::DescriptorHeapType::CbvSrvUav);
        m_imguiBackend = rhi::Backend::D3D12;
    } else if (DeviceManager::GetInstance().GetBackend() == rhi::Backend::Vulkan) {
#if BASICRENDERER_HAS_IMGUI_VULKAN
        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = rhi::vulkan::get_device_api_version(device);
        initInfo.Instance = rhi::vulkan::get_instance(device);
        initInfo.PhysicalDevice = rhi::vulkan::get_physical_device(device);
        initInfo.Device = rhi::vulkan::get_device(device);
        initInfo.QueueFamily = rhi::vulkan::get_queue_family_index(DeviceManager::GetInstance().GetGraphicsQueue());
        initInfo.Queue = rhi::vulkan::get_queue(DeviceManager::GetInstance().GetGraphicsQueue());
        initInfo.MinImageCount = numFramesInFlight;
        initInfo.ImageCount = numFramesInFlight;
        initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.DescriptorPoolSize = kImGuiHeapCapacity;
        initInfo.UseDynamicRendering = true;
        m_imguiVkColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        m_imguiVkRenderingInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        m_imguiVkRenderingInfo.colorAttachmentCount = 1;
        m_imguiVkRenderingInfo.pColorAttachmentFormats = &m_imguiVkColorFormat;
        initInfo.PipelineRenderingCreateInfo = m_imguiVkRenderingInfo;
        initInfo.CheckVkResultFn = [](VkResult result) {
            if (result != VK_SUCCESS) {
                spdlog::error("ImGui Vulkan backend returned VkResult {}", static_cast<int>(result));
            }
        };

        if (!initInfo.Instance || !initInfo.PhysicalDevice || !initInfo.Device || !initInfo.Queue) {
            throw std::runtime_error("Menu::Initialize failed to query Vulkan native handles for ImGui");
        }
        if (!vkGetInstanceProcAddr || !vkGetDeviceProcAddr) {
            throw std::runtime_error("Menu::Initialize cannot initialize ImGui Vulkan backend because Volk has not loaded Vulkan function pointers");
        }

        struct ImGuiVulkanLoaderData {
            VkInstance instance;
            VkDevice device;
        } loaderData{ initInfo.Instance, initInfo.Device };

        if (!ImGui_ImplVulkan_LoadFunctions(initInfo.ApiVersion, [](const char* functionName, void* userData) -> PFN_vkVoidFunction {
            const auto* loaderData = static_cast<const ImGuiVulkanLoaderData*>(userData);
            if (!loaderData) {
                return nullptr;
            }
            if (std::strcmp(functionName, "vkCmdBeginRendering") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRendering);
            }
            if (std::strcmp(functionName, "vkCmdEndRendering") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRendering);
            }
            if (std::strcmp(functionName, "vkCmdBeginRenderingKHR") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdBeginRenderingKHR);
            }
            if (std::strcmp(functionName, "vkCmdEndRenderingKHR") == 0) {
                return reinterpret_cast<PFN_vkVoidFunction>(vkCmdEndRenderingKHR);
            }
            if (vkGetInstanceProcAddr) {
                if (PFN_vkVoidFunction function = vkGetInstanceProcAddr(loaderData->instance, functionName)) {
                    return function;
                }
            }
            return vkGetDeviceProcAddr ? vkGetDeviceProcAddr(loaderData->device, functionName) : nullptr;
        }, &loaderData)) {
            throw std::runtime_error("Menu::Initialize failed to load ImGui Vulkan backend functions through Volk");
        }

        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error("Menu::Initialize failed to initialize ImGui Vulkan backend");
        }
        if (!ImGui_ImplVulkan_CreateFontsTexture()) {
            throw std::runtime_error("Menu::Initialize failed to create ImGui Vulkan font texture");
        }

        auto result = device.CreateDescriptorHeap({ rhi::DescriptorHeapType::CbvSrvUav, kImGuiHeapCapacity, true }, g_pd3dSrvDescHeap);
        if (!rhi::IsOk(result) || !g_pd3dSrvDescHeap) {
            throw std::runtime_error("Menu::Initialize failed to create ImGui descriptor heap for Vulkan backend");
        }

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        samplerInfo.maxAnisotropy = 1.0f;
        m_imguiVkDevice = initInfo.Device;
        const VkResult samplerResult = vkCreateSampler(m_imguiVkDevice, &samplerInfo, nullptr, &m_imguiVkPreviewSampler);
        if (samplerResult != VK_SUCCESS) {
            throw std::runtime_error("Menu::Initialize failed to create Vulkan ImGui preview sampler");
        }

        m_imguiBackend = rhi::Backend::Vulkan;
#else
        (void)swapChain;
        spdlog::warn("Menu::Initialize: Vulkan renderer backend was selected, but imgui_impl_vulkan.h is not available in this build environment.");
#endif
    } else {
        (void)swapChain;
        spdlog::warn("Menu::Initialize: Vulkan renderer backend is not available in this workspace's ImGui integration. UI rendering will stay disabled until a Vulkan ImGui backend is added.");
    }

    ImGui_ImplWin32_EnableDpiAwareness();


    IMGUI_CHECKVERSION();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.FontGlobalScale = 1.2f;

    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

	// Helper to set an observer on a setting which updates local copies of settings
    auto observerSetting = [&](auto& localCopy, const std::string& settingName) {
        m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<std::decay_t<decltype(localCopy)>>(settingName,
            [&localCopy](const std::decay_t<decltype(localCopy)>& newValue) {
                localCopy = newValue;
            }));
		};

	getEnvironmentName = SettingsManager::GetInstance().getSettingGetter<std::string>("environmentName");
	setEnvironment = SettingsManager::GetInstance().getSettingSetter<std::string>("environmentName");

    auto& settingsManager = SettingsManager::GetInstance();
    getImageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting");
    setImageBasedLightingEnabled = settingsManager.getSettingSetter<bool>("enableImageBasedLighting");
	imageBasedLightingEnabled = getImageBasedLightingEnabled();
	observerSetting(imageBasedLightingEnabled, "enableImageBasedLighting");

	getPunctualLightingEnabled = settingsManager.getSettingGetter<bool>("enablePunctualLighting");
	setPunctualLightingEnabled = settingsManager.getSettingSetter<bool>("enablePunctualLighting");
	punctualLightingEnabled = getPunctualLightingEnabled();
	observerSetting(punctualLightingEnabled, "enablePunctualLighting");

	getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
	setShadowsEnabled = settingsManager.getSettingSetter<bool>("enableShadows");
	shadowsEnabled = getShadowsEnabled();
	observerSetting(shadowsEnabled, "enableShadows");

    hdrFiles = GetFilesInDirectoryMatchingExtension(environmentsDir.wstring(), L".hdr");
	environmentName = getEnvironmentName();
    settingsManager.addObserver<std::string>("environmentName", [this](const std::string& newValue) {
        environmentName = getEnvironmentName();
        });

	setOutputType = settingsManager.getSettingSetter<unsigned int>("outputType");
	setTonemapType = settingsManager.getSettingSetter<unsigned int>("tonemapType");
	getTonemapType = settingsManager.getSettingGetter<unsigned int>("tonemapType");

    getSceneRoot = settingsManager.getSettingGetter<std::function<flecs::entity()>>("getSceneRoot")();
    queueSceneNodePositionEdit = settingsManager.getSettingGetter<std::function<void(uint64_t, DirectX::XMFLOAT3)>>("queueSceneNodePositionEdit")();
    queueSceneNodeUniformScaleEdit = settingsManager.getSettingGetter<std::function<void(uint64_t, float)>>("queueSceneNodeUniformScaleEdit")();

	setMeshShaderEnabled = settingsManager.getSettingSetter<bool>("enableMeshShader");
	getMeshShaderEnabled = settingsManager.getSettingGetter<bool>("enableMeshShader");
	meshShaderEnabled = getMeshShaderEnabled();
	observerSetting(meshShaderEnabled, "enableMeshShader");

	setIndirectDrawsEnabled = settingsManager.getSettingSetter<bool>("enableIndirectDraws");
	getIndirectDrawsEnabled = settingsManager.getSettingGetter<bool>("enableIndirectDraws");
	indirectDrawsEnabled = getIndirectDrawsEnabled();
	observerSetting(indirectDrawsEnabled, "enableIndirectDraws");

	getOcclusionCullingEnabled = settingsManager.getSettingGetter<bool>("enableOcclusionCulling");
	setOcclusionCullingEnabled = settingsManager.getSettingSetter<bool>("enableOcclusionCulling");
	occlusionCulling = getOcclusionCullingEnabled();
	observerSetting(occlusionCulling, "enableOcclusionCulling");

    getCLodFrustumCulling = settingsManager.getSettingGetter<bool>(CLodFrustumCullingSettingName);
    setCLodFrustumCulling = settingsManager.getSettingSetter<bool>(CLodFrustumCullingSettingName);
    m_clodFrustumCulling = getCLodFrustumCulling();
    observerSetting(m_clodFrustumCulling, CLodFrustumCullingSettingName);

    getCLodCullingBackend = settingsManager.getSettingGetter<CLodCullingBackend>(CLodCullingBackendSettingName);
    setCLodCullingBackend = settingsManager.getSettingSetter<CLodCullingBackend>(CLodCullingBackendSettingName);
    m_clodCullingBackend = getCLodCullingBackend();
    observerSetting(m_clodCullingBackend, CLodCullingBackendSettingName);

    getCLodPureComputePhase2ExpansionFactor =
        settingsManager.getSettingGetter<uint32_t>(CLodPureComputePhase2ExpansionFactorSettingName);
    setCLodPureComputePhase2ExpansionFactor =
        settingsManager.getSettingSetter<uint32_t>(CLodPureComputePhase2ExpansionFactorSettingName);
    m_clodPureComputePhase2ExpansionFactor =
        CLodNormalizePureComputePhase2ExpansionFactor(getCLodPureComputePhase2ExpansionFactor());
    observerSetting(m_clodPureComputePhase2ExpansionFactor, CLodPureComputePhase2ExpansionFactorSettingName);

    getCLodSoftwareRasterMode = settingsManager.getSettingGetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName);
    setCLodSoftwareRasterMode = settingsManager.getSettingSetter<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName);
    m_clodSoftwareRasterMode = getCLodSoftwareRasterMode();
    observerSetting(m_clodSoftwareRasterMode, CLodSoftwareRasterModeSettingName);

    getCLodVSMRasterMode = settingsManager.getSettingGetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName);
    setCLodVSMRasterMode = settingsManager.getSettingSetter<CLodVSMRasterMode>(CLodVSMRasterModeSettingName);
    m_clodVSMRasterMode = getCLodVSMRasterMode();
    observerSetting(m_clodVSMRasterMode, CLodVSMRasterModeSettingName);

    getCLodTransparencyMode = settingsManager.getSettingGetter<CLodTransparencyMode>(CLodTransparencyModeSettingName);
    setCLodTransparencyMode = settingsManager.getSettingSetter<CLodTransparencyMode>(CLodTransparencyModeSettingName);
    m_clodTransparencyMode = getCLodTransparencyMode();
    observerSetting(m_clodTransparencyMode, CLodTransparencyModeSettingName);

    getCLodDisableReyesRasterization = settingsManager.getSettingGetter<bool>(CLodDisableReyesRasterizationSettingName);
    setCLodDisableReyesRasterization = settingsManager.getSettingSetter<bool>(CLodDisableReyesRasterizationSettingName);
    m_clodDisableReyesRasterization = getCLodDisableReyesRasterization();
    observerSetting(m_clodDisableReyesRasterization, CLodDisableReyesRasterizationSettingName);

    getCLodReyesGeometricNormal = settingsManager.getSettingGetter<bool>(CLodReyesGeometricNormalSettingName);
    setCLodReyesGeometricNormal = settingsManager.getSettingSetter<bool>(CLodReyesGeometricNormalSettingName);
    m_clodReyesGeometricNormal = getCLodReyesGeometricNormal();
    observerSetting(m_clodReyesGeometricNormal, CLodReyesGeometricNormalSettingName);

    getCLodReyesTerrainNormalBlend = settingsManager.getSettingGetter<float>(CLodReyesTerrainNormalBlendSettingName);
    setCLodReyesTerrainNormalBlend = settingsManager.getSettingSetter<float>(CLodReyesTerrainNormalBlendSettingName);
    m_clodReyesTerrainNormalBlend = getCLodReyesTerrainNormalBlend();
    observerSetting(m_clodReyesTerrainNormalBlend, CLodReyesTerrainNormalBlendSettingName);

    getCLodReyesTerrainNormalMipBias = settingsManager.getSettingGetter<uint32_t>(CLodReyesTerrainNormalMipBiasSettingName);
    setCLodReyesTerrainNormalMipBias = settingsManager.getSettingSetter<uint32_t>(CLodReyesTerrainNormalMipBiasSettingName);
    m_clodReyesTerrainNormalMipBias = static_cast<int>(getCLodReyesTerrainNormalMipBias());
    m_settingSubscriptions.push_back(settingsManager.addObserver<uint32_t>(
        CLodReyesTerrainNormalMipBiasSettingName,
        [this](const uint32_t& newValue) {
            m_clodReyesTerrainNormalMipBias = static_cast<int>(newValue);
        }));

    getCLodReyesDiceRatePixels = settingsManager.getSettingGetter<float>(CLodReyesDiceRatePixelsSettingName);
    setCLodReyesDiceRatePixels = settingsManager.getSettingSetter<float>(CLodReyesDiceRatePixelsSettingName);
    m_clodReyesDiceRatePixels = getCLodReyesDiceRatePixels();
    observerSetting(m_clodReyesDiceRatePixels, CLodReyesDiceRatePixelsSettingName);

    getCLodReyesUseAabbOcclusion = settingsManager.getSettingGetter<bool>(CLodReyesUseAabbOcclusionSettingName);
    setCLodReyesUseAabbOcclusion = settingsManager.getSettingSetter<bool>(CLodReyesUseAabbOcclusionSettingName);
    m_clodReyesUseAabbOcclusion = getCLodReyesUseAabbOcclusion();
    observerSetting(m_clodReyesUseAabbOcclusion, CLodReyesUseAabbOcclusionSettingName);

    getCLodDisableVirtualShadowPageCaching = settingsManager.getSettingGetter<bool>(CLodDisableVirtualShadowPageCachingSettingName);
    setCLodDisableVirtualShadowPageCaching = settingsManager.getSettingSetter<bool>(CLodDisableVirtualShadowPageCachingSettingName);
    m_clodDisableVirtualShadowPageCaching = getCLodDisableVirtualShadowPageCaching();
    observerSetting(m_clodDisableVirtualShadowPageCaching, CLodDisableVirtualShadowPageCachingSettingName);

    getCLodEnablePageJobVSM = settingsManager.getSettingGetter<bool>(CLodEnablePageJobVSMSettingName);
    setCLodEnablePageJobVSM = settingsManager.getSettingSetter<bool>(CLodEnablePageJobVSMSettingName);
    m_clodEnablePageJobVSM = getCLodEnablePageJobVSM();
    observerSetting(m_clodEnablePageJobVSM, CLodEnablePageJobVSMSettingName);

    getCLodReyesShadowCoarseTargetPagesPerTriangle = settingsManager.getSettingGetter<float>(CLodReyesShadowCoarseTargetPagesPerTriangleSettingName);
    setCLodReyesShadowCoarseTargetPagesPerTriangle = settingsManager.getSettingSetter<float>(CLodReyesShadowCoarseTargetPagesPerTriangleSettingName);
    m_clodReyesShadowCoarseTargetPagesPerTriangle = getCLodReyesShadowCoarseTargetPagesPerTriangle();
    observerSetting(m_clodReyesShadowCoarseTargetPagesPerTriangle, CLodReyesShadowCoarseTargetPagesPerTriangleSettingName);

    getCLodPageJobDiameterThreshold = settingsManager.getSettingGetter<uint32_t>(CLodPageJobDiameterThresholdSettingName);
    setCLodPageJobDiameterThreshold = settingsManager.getSettingSetter<uint32_t>(CLodPageJobDiameterThresholdSettingName);
    m_clodPageJobDiameterThreshold = getCLodPageJobDiameterThreshold();
    observerSetting(m_clodPageJobDiameterThreshold, CLodPageJobDiameterThresholdSettingName);

    getCLodPageJobSparseRatio = settingsManager.getSettingGetter<float>(CLodPageJobSparseRatioSettingName);
    setCLodPageJobSparseRatio = settingsManager.getSettingSetter<float>(CLodPageJobSparseRatioSettingName);
    m_clodPageJobSparseRatio = getCLodPageJobSparseRatio();
    observerSetting(m_clodPageJobSparseRatio, CLodPageJobSparseRatioSettingName);

    getCLodPageJobMaxPagesPerCluster = settingsManager.getSettingGetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName);
    setCLodPageJobMaxPagesPerCluster = settingsManager.getSettingSetter<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName);
    m_clodPageJobMaxPagesPerCluster = getCLodPageJobMaxPagesPerCluster();
    observerSetting(m_clodPageJobMaxPagesPerCluster, CLodPageJobMaxPagesPerClusterSettingName);

    getCLodPageJobRecordCapacity = settingsManager.getSettingGetter<uint32_t>(CLodPageJobRecordCapacitySettingName);
    setCLodPageJobRecordCapacity = settingsManager.getSettingSetter<uint32_t>(CLodPageJobRecordCapacitySettingName);
    m_clodPageJobRecordCapacity = getCLodPageJobRecordCapacity();
    observerSetting(m_clodPageJobRecordCapacity, CLodPageJobRecordCapacitySettingName);

    getCLodPageJobForceAll = settingsManager.getSettingGetter<bool>(CLodPageJobForceAllSettingName);
    setCLodPageJobForceAll = settingsManager.getSettingSetter<bool>(CLodPageJobForceAllSettingName);
    m_clodPageJobForceAll = getCLodPageJobForceAll();
    observerSetting(m_clodPageJobForceAll, CLodPageJobForceAllSettingName);

    getCLodForceTraversalDepthRoot = settingsManager.getSettingGetter<uint32_t>(CLodForceTraversalDepthRootSettingName);
    setCLodForceTraversalDepthRoot = settingsManager.getSettingSetter<uint32_t>(CLodForceTraversalDepthRootSettingName);
    m_clodForceTraversalDepthRoot = getCLodForceTraversalDepthRoot();
    observerSetting(m_clodForceTraversalDepthRoot, CLodForceTraversalDepthRootSettingName);

    getCLodVisibleClusterCapacity = settingsManager.getSettingGetter<uint32_t>(CLodVisibleClusterCapacitySettingName);
    setCLodVisibleClusterCapacity = settingsManager.getSettingSetter<uint32_t>(CLodVisibleClusterCapacitySettingName);
    m_clodVisibleClusterCapacity = getCLodVisibleClusterCapacity();
    observerSetting(m_clodVisibleClusterCapacity, CLodVisibleClusterCapacitySettingName);

    getCLodDirectionalVirtualShadowMaxBackingResolution = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName);
    setCLodDirectionalVirtualShadowMaxBackingResolution = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName);
    m_clodDirectionalVirtualShadowMaxBackingResolution = getCLodDirectionalVirtualShadowMaxBackingResolution();
    observerSetting(m_clodDirectionalVirtualShadowMaxBackingResolution, CLodDirectionalVirtualShadowMaxBackingResolutionSettingName);

    getCLodDirectionalVirtualShadowMaxPhysicalPages = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName);
    setCLodDirectionalVirtualShadowMaxPhysicalPages = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName);
    m_clodDirectionalVirtualShadowMaxPhysicalPages = getCLodDirectionalVirtualShadowMaxPhysicalPages();
    observerSetting(m_clodDirectionalVirtualShadowMaxPhysicalPages, CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName);

    getCLodDirectionalVirtualShadowLodBias = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowLodBiasSettingName);
    setCLodDirectionalVirtualShadowLodBias = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowLodBiasSettingName);
    m_clodDirectionalVirtualShadowLodBias = getCLodDirectionalVirtualShadowLodBias();
    observerSetting(m_clodDirectionalVirtualShadowLodBias, CLodDirectionalVirtualShadowLodBiasSettingName);

    getCLodDirectionalVirtualShadowAutoLodBias = settingsManager.getSettingGetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName);
    setCLodDirectionalVirtualShadowAutoLodBias = settingsManager.getSettingSetter<bool>(CLodDirectionalVirtualShadowAutoLodBiasSettingName);
    m_clodDirectionalVirtualShadowAutoLodBias = getCLodDirectionalVirtualShadowAutoLodBias();
    observerSetting(m_clodDirectionalVirtualShadowAutoLodBias, CLodDirectionalVirtualShadowAutoLodBiasSettingName);

    getCLodDirectionalVirtualShadowAutoLodBiasScale = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName);
    setCLodDirectionalVirtualShadowAutoLodBiasScale = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName);
    m_clodDirectionalVirtualShadowAutoLodBiasScale = getCLodDirectionalVirtualShadowAutoLodBiasScale();
    observerSetting(m_clodDirectionalVirtualShadowAutoLodBiasScale, CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName);

    getCLodDirectionalVirtualShadowPredictiveLodInvalidation = settingsManager.getSettingGetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName);
    setCLodDirectionalVirtualShadowPredictiveLodInvalidation = settingsManager.getSettingSetter<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName);
    m_clodDirectionalVirtualShadowPredictiveLodInvalidation = getCLodDirectionalVirtualShadowPredictiveLodInvalidation();
    observerSetting(m_clodDirectionalVirtualShadowPredictiveLodInvalidation, CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName);

    getCLodDirectionalVirtualShadowSourceAngleDegrees = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);
    setCLodDirectionalVirtualShadowSourceAngleDegrees = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);
    m_clodDirectionalVirtualShadowSourceAngleDegrees = getCLodDirectionalVirtualShadowSourceAngleDegrees();
    observerSetting(m_clodDirectionalVirtualShadowSourceAngleDegrees, CLodDirectionalVirtualShadowSourceAngleDegreesSettingName);

    getCLodDirectionalVirtualShadowSmrtRayCountDirectional = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName);
    setCLodDirectionalVirtualShadowSmrtRayCountDirectional = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName);
    m_clodDirectionalVirtualShadowSmrtRayCountDirectional = getCLodDirectionalVirtualShadowSmrtRayCountDirectional();
    observerSetting(m_clodDirectionalVirtualShadowSmrtRayCountDirectional, CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName);

    getCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = settingsManager.getSettingGetter<uint32_t>(CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName);
    setCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = settingsManager.getSettingSetter<uint32_t>(CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName);
    m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = getCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional();
    observerSetting(m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional, CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName);

    getCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName);
    setCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName);
    m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees = getCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees();
    observerSetting(m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees, CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName);

    getCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName);
    setCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName);
    m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional = getCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional();
    observerSetting(m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional, CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName);

    getCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld = settingsManager.getSettingGetter<float>(CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName);
    setCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld = settingsManager.getSettingSetter<float>(CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName);
    m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld = getCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld();
    observerSetting(m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld, CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName);

    getNumDirectionalLightCascades = settingsManager.getSettingGetter<uint8_t>("numDirectionalLightCascades");
    setNumDirectionalLightCascades = settingsManager.getSettingSetter<uint8_t>("numDirectionalLightCascades");
    m_numDirectionalLightCascades = getNumDirectionalLightCascades();
    observerSetting(m_numDirectionalLightCascades, "numDirectionalLightCascades");

    getDirectionalShadowVerticalExtent = settingsManager.getSettingGetter<float>("directionalShadowVerticalExtent");
    setDirectionalShadowVerticalExtent = settingsManager.getSettingSetter<float>("directionalShadowVerticalExtent");
    m_directionalShadowVerticalExtent = getDirectionalShadowVerticalExtent();
    observerSetting(m_directionalShadowVerticalExtent, "directionalShadowVerticalExtent");

	setWireframeEnabled = settingsManager.getSettingSetter<bool>("enableWireframe");
	getWireframeEnabled = settingsManager.getSettingGetter<bool>("enableWireframe");
	wireframeEnabled = getWireframeEnabled();
	observerSetting(wireframeEnabled, "enableWireframe");

	setAllowTearing = settingsManager.getSettingSetter<bool>("allowTearing");
	getAllowTearing = settingsManager.getSettingGetter<bool>("allowTearing");
	allowTearing = getAllowTearing();
	observerSetting(allowTearing, "allowTearing");

	setDrawBoundingSpheres = settingsManager.getSettingSetter<bool>("drawBoundingSpheres");
	getDrawBoundingSpheres = settingsManager.getSettingGetter<bool>("drawBoundingSpheres");
	drawBoundingSpheres = getDrawBoundingSpheres();
	observerSetting(drawBoundingSpheres, "drawBoundingSpheres");

	setClusteredLightingEnabled = settingsManager.getSettingSetter<bool>("enableClusteredLighting");
	getClusteredLightingEnabled = settingsManager.getSettingGetter<bool>("enableClusteredLighting");
	clusteredLighting = getClusteredLightingEnabled();
	observerSetting(clusteredLighting, "enableClusteredLighting");

	setVisibilityRenderingEnabled = settingsManager.getSettingSetter<bool>("enableVisibilityRendering");
	getVisibilityRenderingEnabled = settingsManager.getSettingGetter<bool>("enableVisibilityRendering");
	m_visibilityRenderingEnabled = getVisibilityRenderingEnabled();
	observerSetting(m_visibilityRenderingEnabled, "enableVisibilityRendering");
    setTerrainRegionMaterialEvaluationEnabled = settingsManager.getSettingSetter<bool>("enableTerrainRegionMaterialEvaluation");
    getTerrainRegionMaterialEvaluationEnabled = settingsManager.getSettingGetter<bool>("enableTerrainRegionMaterialEvaluation");
    m_terrainRegionMaterialEvaluationEnabled = getTerrainRegionMaterialEvaluationEnabled();
    observerSetting(m_terrainRegionMaterialEvaluationEnabled, "enableTerrainRegionMaterialEvaluation");
    setTerrainRvtEnabled = settingsManager.getSettingSetter<bool>("enableTerrainRvt");
    getTerrainRvtEnabled = settingsManager.getSettingGetter<bool>("enableTerrainRvt");
    m_terrainRvtEnabled = getTerrainRvtEnabled();
    observerSetting(m_terrainRvtEnabled, "enableTerrainRvt");
    setForceDirectTerrainRvtFallback = settingsManager.getSettingSetter<bool>("forceDirectTerrainRvtFallback");
    getForceDirectTerrainRvtFallback = settingsManager.getSettingGetter<bool>("forceDirectTerrainRvtFallback");
    m_forceDirectTerrainRvtFallback = getForceDirectTerrainRvtFallback();
    observerSetting(m_forceDirectTerrainRvtFallback, "forceDirectTerrainRvtFallback");
    setTerrainRvtTelemetryDebug = settingsManager.getSettingSetter<bool>("terrainRvtTelemetryDebug");
    getTerrainRvtTelemetryDebug = settingsManager.getSettingGetter<bool>("terrainRvtTelemetryDebug");
    m_terrainRvtTelemetryDebug = getTerrainRvtTelemetryDebug();
    observerSetting(m_terrainRvtTelemetryDebug, "terrainRvtTelemetryDebug");
    setTerrainRvtDebugView = settingsManager.getSettingSetter<uint32_t>("terrainRvtDebugView");
    getTerrainRvtDebugView = settingsManager.getSettingGetter<uint32_t>("terrainRvtDebugView");
    m_terrainRvtDebugView = static_cast<int>(getTerrainRvtDebugView());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtDebugView",
        [this](const uint32_t& newValue) {
            m_terrainRvtDebugView = static_cast<int>(newValue);
        }));
    setTerrainRvtPageSize = settingsManager.getSettingSetter<uint32_t>("terrainRvtPageSize");
    getTerrainRvtPageSize = settingsManager.getSettingGetter<uint32_t>("terrainRvtPageSize");
    m_terrainRvtPageSize = static_cast<int>(getTerrainRvtPageSize());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPageSize",
        [this](const uint32_t& newValue) {
            m_terrainRvtPageSize = static_cast<int>(newValue);
        }));
    setTerrainRvtBorderTexels = settingsManager.getSettingSetter<uint32_t>("terrainRvtBorderTexels");
    getTerrainRvtBorderTexels = settingsManager.getSettingGetter<uint32_t>("terrainRvtBorderTexels");
    m_terrainRvtBorderTexels = static_cast<int>(getTerrainRvtBorderTexels());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtBorderTexels",
        [this](const uint32_t& newValue) {
            m_terrainRvtBorderTexels = static_cast<int>(newValue);
        }));
    setTerrainRvtMipCount = settingsManager.getSettingSetter<uint32_t>("terrainRvtMipCount");
    getTerrainRvtMipCount = settingsManager.getSettingGetter<uint32_t>("terrainRvtMipCount");
    m_terrainRvtMipCount = static_cast<int>(getTerrainRvtMipCount());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtMipCount",
        [this](const uint32_t& newValue) {
            m_terrainRvtMipCount = static_cast<int>(newValue);
        }));
    setTerrainRvtMipOffset = settingsManager.getSettingSetter<float>("terrainRvtMipOffset");
    getTerrainRvtMipOffset = settingsManager.getSettingGetter<float>("terrainRvtMipOffset");
    m_terrainRvtMipOffset = getTerrainRvtMipOffset();
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<float>(
        "terrainRvtMipOffset",
        [this](const float& newValue) {
            m_terrainRvtMipOffset = newValue;
        }));
    setTerrainRvtSourceTexelsPerWorld = settingsManager.getSettingSetter<float>("terrainRvtSourceTexelsPerWorld");
    getTerrainRvtSourceTexelsPerWorld = settingsManager.getSettingGetter<float>("terrainRvtSourceTexelsPerWorld");
    m_terrainRvtSourceTexelsPerWorld = getTerrainRvtSourceTexelsPerWorld();
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<float>(
        "terrainRvtSourceTexelsPerWorld",
        [this](const float& newValue) {
            m_terrainRvtSourceTexelsPerWorld = newValue;
        }));
    setTerrainRvtPhysicalAtlasPagesWide = settingsManager.getSettingSetter<uint32_t>("terrainRvtPhysicalAtlasPagesWide");
    getTerrainRvtPhysicalAtlasPagesWide = settingsManager.getSettingGetter<uint32_t>("terrainRvtPhysicalAtlasPagesWide");
    m_terrainRvtPhysicalAtlasPagesWide = static_cast<int>(getTerrainRvtPhysicalAtlasPagesWide());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPhysicalAtlasPagesWide",
        [this](const uint32_t& newValue) {
            m_terrainRvtPhysicalAtlasPagesWide = static_cast<int>(newValue);
        }));
    setTerrainRvtPhysicalAtlasPagesHigh = settingsManager.getSettingSetter<uint32_t>("terrainRvtPhysicalAtlasPagesHigh");
    getTerrainRvtPhysicalAtlasPagesHigh = settingsManager.getSettingGetter<uint32_t>("terrainRvtPhysicalAtlasPagesHigh");
    m_terrainRvtPhysicalAtlasPagesHigh = static_cast<int>(getTerrainRvtPhysicalAtlasPagesHigh());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPhysicalAtlasPagesHigh",
        [this](const uint32_t& newValue) {
            m_terrainRvtPhysicalAtlasPagesHigh = static_cast<int>(newValue);
        }));
    setTerrainRvtPhysicalAtlasPoolCount = settingsManager.getSettingSetter<uint32_t>("terrainRvtPhysicalAtlasPoolCount");
    getTerrainRvtPhysicalAtlasPoolCount = settingsManager.getSettingGetter<uint32_t>("terrainRvtPhysicalAtlasPoolCount");
    m_terrainRvtPhysicalAtlasPoolCount = static_cast<int>(getTerrainRvtPhysicalAtlasPoolCount());
    m_settingSubscriptions.push_back(SettingsManager::GetInstance().addObserver<uint32_t>(
        "terrainRvtPhysicalAtlasPoolCount",
        [this](const uint32_t& newValue) {
            m_terrainRvtPhysicalAtlasPoolCount = static_cast<int>(newValue);
        }));

    setTerrainStochasticSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticSampling");
    getTerrainStochasticSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticSampling");
    m_terrainStochasticSamplingEnabled = getTerrainStochasticSamplingEnabled();
    observerSetting(m_terrainStochasticSamplingEnabled, "enableTerrainStochasticSampling");
    setTerrainStochasticDiffuseSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticDiffuseSampling");
    getTerrainStochasticDiffuseSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticDiffuseSampling");
    m_terrainStochasticDiffuseSamplingEnabled = getTerrainStochasticDiffuseSamplingEnabled();
    observerSetting(m_terrainStochasticDiffuseSamplingEnabled, "enableTerrainStochasticDiffuseSampling");
    setTerrainStochasticNormalSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticNormalSampling");
    getTerrainStochasticNormalSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticNormalSampling");
    m_terrainStochasticNormalSamplingEnabled = getTerrainStochasticNormalSamplingEnabled();
    observerSetting(m_terrainStochasticNormalSamplingEnabled, "enableTerrainStochasticNormalSampling");
    setTerrainStochasticDerivativeNormalSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainStochasticDerivativeNormalSampling");
    getTerrainStochasticDerivativeNormalSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainStochasticDerivativeNormalSampling");
    m_terrainStochasticDerivativeNormalSamplingEnabled = getTerrainStochasticDerivativeNormalSamplingEnabled();
    observerSetting(m_terrainStochasticDerivativeNormalSamplingEnabled, "enableTerrainStochasticDerivativeNormalSampling");
    setTerrainStochasticBlendCurve = settingsManager.getSettingSetter<float>("terrainStochasticBlendCurve");
    getTerrainStochasticBlendCurve = settingsManager.getSettingGetter<float>("terrainStochasticBlendCurve");
    m_terrainStochasticBlendCurve = getTerrainStochasticBlendCurve();
    observerSetting(m_terrainStochasticBlendCurve, "terrainStochasticBlendCurve");
    setTerrainGaussianStochasticSamplingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainGaussianStochasticSampling");
    getTerrainGaussianStochasticSamplingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainGaussianStochasticSampling");
    m_terrainGaussianStochasticSamplingEnabled = getTerrainGaussianStochasticSamplingEnabled();
    observerSetting(m_terrainGaussianStochasticSamplingEnabled, "enableTerrainGaussianStochasticSampling");
    setParallaxOcclusionMappingEnabled = settingsManager.getSettingSetter<bool>("enableParallaxOcclusionMapping");
    getParallaxOcclusionMappingEnabled = settingsManager.getSettingGetter<bool>("enableParallaxOcclusionMapping");
    m_parallaxOcclusionMappingEnabled = getParallaxOcclusionMappingEnabled();
    observerSetting(m_parallaxOcclusionMappingEnabled, "enableParallaxOcclusionMapping");
    setTerrainParallaxOcclusionMappingEnabled = settingsManager.getSettingSetter<bool>("enableTerrainParallaxOcclusionMapping");
    getTerrainParallaxOcclusionMappingEnabled = settingsManager.getSettingGetter<bool>("enableTerrainParallaxOcclusionMapping");
    m_terrainParallaxOcclusionMappingEnabled = getTerrainParallaxOcclusionMappingEnabled();
    observerSetting(m_terrainParallaxOcclusionMappingEnabled, "enableTerrainParallaxOcclusionMapping");
    setTerrainReyesDisplacementEnabled = settingsManager.getSettingSetter<bool>("enableTerrainReyesDisplacement");
    getTerrainReyesDisplacementEnabled = settingsManager.getSettingGetter<bool>("enableTerrainReyesDisplacement");
    m_terrainReyesDisplacementEnabled = getTerrainReyesDisplacementEnabled();
    observerSetting(m_terrainReyesDisplacementEnabled, "enableTerrainReyesDisplacement");
    setTerrainReyesDisplacementScale = settingsManager.getSettingSetter<float>("terrainReyesDisplacementGlobalScale");
    getTerrainReyesDisplacementScale = settingsManager.getSettingGetter<float>("terrainReyesDisplacementGlobalScale");
    m_terrainReyesDisplacementScale = getTerrainReyesDisplacementScale();
    observerSetting(m_terrainReyesDisplacementScale, "terrainReyesDisplacementGlobalScale");
    setObjectReyesDisplacementScale = settingsManager.getSettingSetter<float>("objectReyesDisplacementScale");
    getObjectReyesDisplacementScale = settingsManager.getSettingGetter<float>("objectReyesDisplacementScale");
    m_objectReyesDisplacementScale = getObjectReyesDisplacementScale();
    observerSetting(m_objectReyesDisplacementScale, "objectReyesDisplacementScale");
    setTerrainParallaxHeightScale = settingsManager.getSettingSetter<float>("terrainParallaxHeightScale");
    getTerrainParallaxHeightScale = settingsManager.getSettingGetter<float>("terrainParallaxHeightScale");
    m_terrainParallaxHeightScale = getTerrainParallaxHeightScale();
    observerSetting(m_terrainParallaxHeightScale, "terrainParallaxHeightScale");
    setObjectParallaxHeightScale = settingsManager.getSettingSetter<float>("objectParallaxHeightScale");
    getObjectParallaxHeightScale = settingsManager.getSettingGetter<float>("objectParallaxHeightScale");
    m_objectParallaxHeightScale = getObjectParallaxHeightScale();
    observerSetting(m_objectParallaxHeightScale, "objectParallaxHeightScale");
    setTerrainParallaxMaxSteps = settingsManager.getSettingSetter<uint32_t>("terrainParallaxMaxSteps");
    getTerrainParallaxMaxSteps = settingsManager.getSettingGetter<uint32_t>("terrainParallaxMaxSteps");
    m_terrainParallaxMaxSteps = getTerrainParallaxMaxSteps();
    observerSetting(m_terrainParallaxMaxSteps, "terrainParallaxMaxSteps");
    setTerrainParallaxFadeStartDistance = settingsManager.getSettingSetter<float>("terrainParallaxFadeStartDistance");
    getTerrainParallaxFadeStartDistance = settingsManager.getSettingGetter<float>("terrainParallaxFadeStartDistance");
    m_terrainParallaxFadeStartDistance = getTerrainParallaxFadeStartDistance();
    observerSetting(m_terrainParallaxFadeStartDistance, "terrainParallaxFadeStartDistance");
    setTerrainParallaxFadeEndDistance = settingsManager.getSettingSetter<float>("terrainParallaxFadeEndDistance");
    getTerrainParallaxFadeEndDistance = settingsManager.getSettingGetter<float>("terrainParallaxFadeEndDistance");
    m_terrainParallaxFadeEndDistance = getTerrainParallaxFadeEndDistance();
    observerSetting(m_terrainParallaxFadeEndDistance, "terrainParallaxFadeEndDistance");

	getGTAOEnabled = settingsManager.getSettingGetter<bool>("enableGTAO");
	setGTAOEnabled = settingsManager.getSettingSetter<bool>("enableGTAO");
	m_gtaoEnabled = getGTAOEnabled();
	observerSetting(m_gtaoEnabled, "enableGTAO");

	setBloomEnabled = settingsManager.getSettingSetter<bool>("enableBloom");
	getBloomEnabled = settingsManager.getSettingGetter<bool>("enableBloom");
	m_bloomEnabled = getBloomEnabled();
	observerSetting(m_bloomEnabled, "enableBloom");

	setScreenSpaceReflectionsEnabled = settingsManager.getSettingSetter<bool>("enableScreenSpaceReflections");
	getScreenSpaceReflectionsEnabled = settingsManager.getSettingGetter<bool>("enableScreenSpaceReflections");
	m_screenSpaceReflectionsEnabled = getScreenSpaceReflectionsEnabled();
	observerSetting(m_screenSpaceReflectionsEnabled, "enableScreenSpaceReflections");

    setRayTracedReflectionsEnabled = settingsManager.getSettingSetter<bool>("enableRayTracedReflections");
    getRayTracedReflectionsEnabled = settingsManager.getSettingGetter<bool>("enableRayTracedReflections");
    m_rayTracedReflectionsEnabled = getRayTracedReflectionsEnabled();
    observerSetting(m_rayTracedReflectionsEnabled, "enableRayTracedReflections");

    setJitterEnabled = settingsManager.getSettingSetter<bool>("enableJitter");
    getJitterEnabled = settingsManager.getSettingGetter<bool>("enableJitter");
    m_jitterEnabled = getJitterEnabled();
	observerSetting(m_jitterEnabled, "enableJitter");

    getCollectPassStatistics = settingsManager.getSettingGetter<bool>("collectPassStatistics");
    setCollectPassStatistics = settingsManager.getSettingSetter<bool>("collectPassStatistics");
    m_collectPassStatistics = getCollectPassStatistics();
    observerSetting(m_collectPassStatistics, "collectPassStatistics");

	getCollectPipelineStatistics = settingsManager.getSettingGetter<bool>("collectPipelineStatistics");
	setCollectPipelineStatistics = settingsManager.getSettingSetter<bool>("collectPipelineStatistics");
	m_collectPipelineStatistics = getCollectPipelineStatistics();

    getUpscalingMode = settingsManager.getSettingGetter<UpscalingMode>("upscalingMode");
    setUpscalingMode = settingsManager.getSettingSetter<UpscalingMode>("upscalingMode");
    m_currentUpscalingMode = getUpscalingMode();
	observerSetting(m_currentUpscalingMode, "upscalingMode");

	getUpscalingQualityMode = settingsManager.getSettingGetter<UpscaleQualityMode>("upscalingQualityMode");
    setUpscalingQualityMode = settingsManager.getSettingSetter<UpscaleQualityMode>("upscalingQualityMode");
    m_currentUpscalingQualityMode = getUpscalingQualityMode();
	observerSetting(m_currentUpscalingQualityMode, "upscalingQualityMode");

	getUseAsyncCompute = settingsManager.getSettingGetter<bool>("useAsyncCompute");
    setUseAsyncCompute = settingsManager.getSettingSetter<bool>("useAsyncCompute");
    m_useAsyncCompute = getUseAsyncCompute();
	observerSetting(m_useAsyncCompute, "useAsyncCompute");

	getHeavyDebug = settingsManager.getSettingGetter<bool>("heavyDebug");
	setHeavyDebug = settingsManager.getSettingSetter<bool>("heavyDebug");
	m_heavyDebug = getHeavyDebug();
	observerSetting(m_heavyDebug, "heavyDebug");

	getRenderGraphDisableCaching = settingsManager.getSettingGetter<bool>("renderGraphDisableCaching");
	setRenderGraphDisableCaching = settingsManager.getSettingSetter<bool>("renderGraphDisableCaching");
	m_renderGraphDisableCaching = getRenderGraphDisableCaching();
	observerSetting(m_renderGraphDisableCaching, "renderGraphDisableCaching");

    getRenderGraphBatchTraceEnabled = settingsManager.getSettingGetter<bool>("renderGraphBatchTraceEnabled");
    setRenderGraphBatchTraceEnabled = settingsManager.getSettingSetter<bool>("renderGraphBatchTraceEnabled");
    m_renderGraphBatchTraceEnabled = getRenderGraphBatchTraceEnabled();
    observerSetting(m_renderGraphBatchTraceEnabled, "renderGraphBatchTraceEnabled");

	getRenderGraphLightweightCompileSummaryEnabled = settingsManager.getSettingGetter<bool>("renderGraphLightweightCompileSummaryEnabled");
	setRenderGraphLightweightCompileSummaryEnabled = settingsManager.getSettingSetter<bool>("renderGraphLightweightCompileSummaryEnabled");
	m_renderGraphLightweightCompileSummaryEnabled = getRenderGraphLightweightCompileSummaryEnabled();
	observerSetting(m_renderGraphLightweightCompileSummaryEnabled, "renderGraphLightweightCompileSummaryEnabled");

    getReshapeTexelAddressing = settingsManager.getSettingGetter<bool>("reshapeTexelAddressing");
    setReshapeTexelAddressing = settingsManager.getSettingSetter<bool>("reshapeTexelAddressing");
    m_reshapeTexelAddressing = getReshapeTexelAddressing();
    observerSetting(m_reshapeTexelAddressing, "reshapeTexelAddressing");

    getAutoAliasMode = settingsManager.getSettingGetter<AutoAliasMode>("autoAliasMode");
    setAutoAliasMode = settingsManager.getSettingSetter<AutoAliasMode>("autoAliasMode");
    m_autoAliasMode = getAutoAliasMode();
    observerSetting(m_autoAliasMode, "autoAliasMode");

    getAutoAliasPackingStrategy = settingsManager.getSettingGetter<AutoAliasPackingStrategy>("autoAliasPackingStrategy");
    setAutoAliasPackingStrategy = settingsManager.getSettingSetter<AutoAliasPackingStrategy>("autoAliasPackingStrategy");
    m_autoAliasPackingStrategy = getAutoAliasPackingStrategy();
    observerSetting(m_autoAliasPackingStrategy, "autoAliasPackingStrategy");

    getAutoAliasLogExclusionReasons = settingsManager.getSettingGetter<bool>("autoAliasLogExclusionReasons");
    setAutoAliasLogExclusionReasons = settingsManager.getSettingSetter<bool>("autoAliasLogExclusionReasons");
    m_autoAliasLogExclusionReasons = getAutoAliasLogExclusionReasons();
    observerSetting(m_autoAliasLogExclusionReasons, "autoAliasLogExclusionReasons");

    setAutoAliasBuildDebugData = settingsManager.getSettingSetter<bool>("autoAliasBuildDebugData");

    getAutoAliasPoolRetireIdleFrames = settingsManager.getSettingGetter<uint32_t>("autoAliasPoolRetireIdleFrames");
    setAutoAliasPoolRetireIdleFrames = settingsManager.getSettingSetter<uint32_t>("autoAliasPoolRetireIdleFrames");
    m_autoAliasPoolRetireIdleFrames = getAutoAliasPoolRetireIdleFrames();
    observerSetting(m_autoAliasPoolRetireIdleFrames, "autoAliasPoolRetireIdleFrames");

    getCLodStreamingCpuUploadBudgetRequests = settingsManager.getSettingGetter<uint32_t>(CLodStreamingCpuUploadBudgetSettingName);
    setCLodStreamingCpuUploadBudgetRequests = settingsManager.getSettingSetter<uint32_t>(CLodStreamingCpuUploadBudgetSettingName);
    m_clodStreamingCpuUploadBudgetRequests = getCLodStreamingCpuUploadBudgetRequests();
    observerSetting(m_clodStreamingCpuUploadBudgetRequests, CLodStreamingCpuUploadBudgetSettingName);

    getCLodStreamingEnableDirectStorage = settingsManager.getSettingGetter<bool>(CLodStreamingEnableDirectStorageSettingName);
    setCLodStreamingEnableDirectStorage = settingsManager.getSettingSetter<bool>(CLodStreamingEnableDirectStorageSettingName);
    m_clodStreamingEnableDirectStorage = getCLodStreamingEnableDirectStorage();
    observerSetting(m_clodStreamingEnableDirectStorage, CLodStreamingEnableDirectStorageSettingName);

    getAutoAliasPoolGrowthHeadroom = settingsManager.getSettingGetter<float>("autoAliasPoolGrowthHeadroom");
    setAutoAliasPoolGrowthHeadroom = settingsManager.getSettingSetter<float>("autoAliasPoolGrowthHeadroom");
    m_autoAliasPoolGrowthHeadroom = getAutoAliasPoolGrowthHeadroom();
    observerSetting(m_autoAliasPoolGrowthHeadroom, "autoAliasPoolGrowthHeadroom");

	appendScene = settingsManager.getSettingGetter<std::function<std::shared_ptr<Scene>(std::shared_ptr<Scene>)>>("appendScene")();

    m_meshShadersSupported = DeviceManager::GetInstance().GetMeshShadersSupported();

    // CLod queries
    const auto visBufferTag = RendererECSManager::GetInstance().GetWorld().component<CLodExtensionVisibilityBufferTag>();
    m_telemetryQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    const auto shadowTag = RendererECSManager::GetInstance().GetWorld().component<CLodExtensionShadowTag>();
    m_shadowTelemetryQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_reyesTelemetryPhase1Query = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase1Tag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_reyesTelemetryPhase2Query = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase2Tag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_shadowReyesTelemetryPhase1Query = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase1Tag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_visibleClustersQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersBufferTag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_visibleCounterQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersCounterTag>()
        .with<CLodExtensionTypeTag>(visBufferTag)
        .build();
    m_shadowVisibleClustersQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersBufferTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_shadowVisibleCounterQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<VisibleClustersCounterTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_shadowVirtualShadowStatsQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodVirtualShadowStatsTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();
    m_shadowVirtualShadowRuntimeStateQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodVirtualShadowRuntimeStateTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build();

    const auto alphaTag = RendererECSManager::GetInstance().GetWorld().component<CLodExtensionAlphaBlendTag>();
    m_alphaDeepVisibilityCounterQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodDeepVisibilityCounterTag>()
        .with<CLodExtensionTypeTag>(alphaTag)
        .build();
    m_alphaDeepVisibilityOverflowQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodDeepVisibilityOverflowCounterTag>()
        .with<CLodExtensionTypeTag>(alphaTag)
        .build();
    m_alphaDeepVisibilityStatsQuery = RendererECSManager::GetInstance().GetWorld()
        .query_builder<const Components::Resource>()
        .with<CLodDeepVisibilityStatsTag>()
        .with<CLodExtensionTypeTag>(alphaTag)
        .build();
}

static bool PassUsesResourceAdapter(const void* passAndRes, uint64_t resourceId, int passKind) {
    auto checkRequirements = [&](const auto& resources) {
        bool found = false;
        ForEachFrameRequirement(resources, [&](const auto& req) {
            if (req.resourceHandleAndRange.resource.GetGlobalResourceID() == resourceId) {
                found = true;
            }
        });
        return found;
    };
    switch (passKind) {
    case 1: { // Compute
        auto& pr = *reinterpret_cast<const RenderGraph::ComputePassAndResources*>(passAndRes);
        return checkRequirements(pr.resources);
    }
    case 2: { // Copy
        auto& pr = *reinterpret_cast<const RenderGraph::CopyPassAndResources*>(passAndRes);
        return checkRequirements(pr.resources);
    }
    default: { // Render (0)
        auto& pr = *reinterpret_cast<const RenderGraph::RenderPassAndResources*>(passAndRes);
        return checkRequirements(pr.resources);
    }
    }
}

inline void Menu::Render(const RenderContext& context, rhi::CommandList commandList) {
    m_sceneOverlapStatus = context.sceneOverlapStatus;

    if (m_imguiBackend == rhi::Backend::D3D12) {
        ImGui_ImplDX12_NewFrame();
    }
#if BASICRENDERER_HAS_IMGUI_VULKAN
    else if (m_imguiBackend == rhi::Backend::Vulkan) {
        ImGui_ImplVulkan_NewFrame();
    }
#endif
	ImGui_ImplWin32_NewFrame();

	ImGui::NewFrame();
    static bool showRG = false;
    static bool showMemoryIntrospection = false;
    static bool showCLodTelemetry = false;
    static bool showFrameTaskGraph = false;
    static bool showAutoAliasPlanner = false;
    static bool showGpuInstrumentation = false;
    static bool showMaterialTextureStreaming = false;

    const auto formatBytes = [](uint64_t bytes) {
        const double value = static_cast<double>(bytes);
        const double KiB = 1024.0;
        const double MiB = KiB * 1024.0;
        const double GiB = MiB * 1024.0;
        const auto [divisor, suffix] =
            (value >= GiB) ? std::pair{ GiB, "GB" } :
            (value >= MiB) ? std::pair{ MiB, "MB" } :
            (value >= KiB) ? std::pair{ KiB, "KB" } :
            std::pair{ 1.0, "B" };
        return std::format("{:.2f} {}", value / divisor, suffix);
    };
    const std::optional<MaterialTextureStreamingStats> materialTextureStreamingStats =
        context.materialManager
        ? std::optional<MaterialTextureStreamingStats>(context.materialManager->GetMaterialTextureStreamingStats())
        : std::nullopt;

    const float fps = ImGui::GetIO().Framerate;
    const float msPerFrame = fps > 0.0f ? (1000.0f / fps) : 0.0f;

    SetCLodWorkGraphTelemetryEnabled(
        (m_menuEnabled && showCLodTelemetry) ||
        m_clodTelemetry.capturePending ||
        m_clodTelemetry.captureStatsPending ||
        m_shadowClodTelemetry.capturePending ||
        m_shadowClodTelemetry.captureStatsPending);

    if (!m_menuEnabled) {
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Menu", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoCollapse);
        ImGui::Checkbox("Enable Menu", &m_menuEnabled);
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", msPerFrame, fps);
        ImGui::End();

		ImGui::Render();

        if (m_imguiBackend == rhi::Backend::Null) {
            return;
        }

        if (m_imguiBackend == rhi::Backend::D3D12) {
            if (!g_pd3dSrvDescHeap) {
                return;
            }
            commandList.SetDescriptorHeaps(g_pd3dSrvDescHeap->GetHandle(), std::nullopt);
        }

		rhi::PassBeginInfo beginInfo{};
		rhi::ColorAttachment attchment{};
        attchment.loadOp = rhi::LoadOp::Load;
		attchment.rtv = { context.rtvHeap.GetHandle() , context.frameIndex }; // Index into the swapchain RTV heap
		beginInfo.colors = { &attchment };
        beginInfo.height = context.outputResolution.y;
        beginInfo.width = context.outputResolution.x;

		commandList.BeginPass(beginInfo);

        if (m_imguiBackend == rhi::Backend::D3D12) {
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), rhi::dx12::get_cmd_list(commandList));
        }
#if BASICRENDERER_HAS_IMGUI_VULKAN
        else if (m_imguiBackend == rhi::Backend::Vulkan) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), rhi::vulkan::get_cmd_list(commandList));
        }
#endif
        return;
    }

	{
		static float f = 0.0f;
		static int counter = 0;

        const ImGuiIO& io = ImGui::GetIO();
        const float margin = 12.0f;
        const float maxWindowWidth = (std::max)(360.0f, io.DisplaySize.x - margin * 2.0f);
        const float maxWindowHeight = (std::max)(240.0f, io.DisplaySize.y - margin * 2.0f);
        ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2((std::min)(520.0f, maxWindowWidth), (std::min)(720.0f, maxWindowHeight)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 240.0f), ImVec2(maxWindowWidth, maxWindowHeight));

		ImGui::Begin("Renderer Configuration", nullptr, ImGuiWindowFlags_NoCollapse);

        if (ImGui::Checkbox("Image-Based Lighting", &imageBasedLightingEnabled)) {
			setImageBasedLightingEnabled(imageBasedLightingEnabled);
        }
		if (ImGui::Checkbox("Punctual Lighting", &punctualLightingEnabled)) {
			setPunctualLightingEnabled(punctualLightingEnabled);
		}
		if (ImGui::Checkbox("Shadows", &shadowsEnabled)) {
			setShadowsEnabled(shadowsEnabled);
		}
        if (m_meshShadersSupported) {
            if (ImGui::Checkbox("Use Mesh Shaders", &meshShaderEnabled)) {
                setMeshShaderEnabled(meshShaderEnabled);
            }
        }
        else {
            ImGui::Text("Your GPU does not support mesh shaders!");
        }
        if (ImGui::Checkbox("Use Indirect Draws", &indirectDrawsEnabled)) {
            setIndirectDrawsEnabled(indirectDrawsEnabled);
        }
        if (ImGui::Checkbox("Occlusion Culling", &occlusionCulling)) {
            setOcclusionCullingEnabled(occlusionCulling);
        }
        if (ImGui::Checkbox("CLod Frustum Culling", &m_clodFrustumCulling)) {
            setCLodFrustumCulling(m_clodFrustumCulling);
        }
        int clodCullingBackendIndex = static_cast<int>(m_clodCullingBackend);
        if (ImGui::Combo("CLod Culling Backend", &clodCullingBackendIndex, CLodCullingBackendNames, CLodCullingBackendCount)) {
            clodCullingBackendIndex = std::clamp(clodCullingBackendIndex, 0, CLodCullingBackendCount - 1);
            m_clodCullingBackend = static_cast<CLodCullingBackend>(clodCullingBackendIndex);
            setCLodCullingBackend(m_clodCullingBackend);
        }
        if (m_clodCullingBackend == CLodCullingBackend::PureCompute) {
            static constexpr uint32_t kPureComputePhase2ExpansionFactors[] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u };
            static constexpr const char* kPureComputePhase2ExpansionFactorLabels[] = { "1", "2", "4", "8", "16", "32", "64" };
            static constexpr int kPureComputePhase2ExpansionFactorCount =
                static_cast<int>(sizeof(kPureComputePhase2ExpansionFactors) / sizeof(kPureComputePhase2ExpansionFactors[0]));
            m_clodPureComputePhase2ExpansionFactor =
                CLodNormalizePureComputePhase2ExpansionFactor(m_clodPureComputePhase2ExpansionFactor);
            int phase2ExpansionIndex = 0;
            for (int i = 0; i < kPureComputePhase2ExpansionFactorCount; ++i) {
                if (kPureComputePhase2ExpansionFactors[i] == m_clodPureComputePhase2ExpansionFactor) {
                    phase2ExpansionIndex = i;
                    break;
                }
            }
            if (ImGui::Combo(
                    "Pure Compute Phase-2 Bucket Size",
                    &phase2ExpansionIndex,
                    kPureComputePhase2ExpansionFactorLabels,
                    kPureComputePhase2ExpansionFactorCount)) {
                phase2ExpansionIndex = std::clamp(
                    phase2ExpansionIndex,
                    0,
                    kPureComputePhase2ExpansionFactorCount - 1);
                m_clodPureComputePhase2ExpansionFactor = kPureComputePhase2ExpansionFactors[phase2ExpansionIndex];
                setCLodPureComputePhase2ExpansionFactor(m_clodPureComputePhase2ExpansionFactor);
            }
        }
        int clodSoftwareRasterModeIndex = static_cast<int>(m_clodSoftwareRasterMode);
        if (ImGui::Combo("Visibility/Alpha SW Raster Mode", &clodSoftwareRasterModeIndex, CLodSoftwareRasterModeNames, CLodSoftwareRasterModeCount)) {
            clodSoftwareRasterModeIndex = std::clamp(clodSoftwareRasterModeIndex, 0, CLodSoftwareRasterModeCount - 1);
            m_clodSoftwareRasterMode = static_cast<CLodSoftwareRasterMode>(clodSoftwareRasterModeIndex);
            setCLodSoftwareRasterMode(m_clodSoftwareRasterMode);
        }
        int clodVSMRasterModeIndex = static_cast<int>(m_clodVSMRasterMode);
        if (ImGui::Combo("VSM Raster Mode", &clodVSMRasterModeIndex, CLodVSMRasterModeNames, CLodVSMRasterModeCount)) {
            clodVSMRasterModeIndex = std::clamp(clodVSMRasterModeIndex, 0, CLodVSMRasterModeCount - 1);
            m_clodVSMRasterMode = static_cast<CLodVSMRasterMode>(clodVSMRasterModeIndex);
            setCLodVSMRasterMode(m_clodVSMRasterMode);
        }
        int clodTransparencyModeIndex = static_cast<int>(m_clodTransparencyMode);
        if (ImGui::Combo("Transparency Mode", &clodTransparencyModeIndex, CLodTransparencyModeNames, CLodTransparencyModeCount)) {
            clodTransparencyModeIndex = std::clamp(clodTransparencyModeIndex, 0, CLodTransparencyModeCount - 1);
            m_clodTransparencyMode = static_cast<CLodTransparencyMode>(clodTransparencyModeIndex);
            setCLodTransparencyMode(m_clodTransparencyMode);
        }
        if (ImGui::Checkbox("Disable Reyes Tessellation / VSM Reyes Routing", &m_clodDisableReyesRasterization)) {
            setCLodDisableReyesRasterization(m_clodDisableReyesRasterization);
        }
        if (ImGui::Checkbox("Reyes Geometric Normal", &m_clodReyesGeometricNormal)) {
            setCLodReyesGeometricNormal(m_clodReyesGeometricNormal);
        }
        if (ImGui::SliderFloat("Reyes Terrain Normal Blend", &m_clodReyesTerrainNormalBlend, 0.0f, 1.0f, "%.2f")) {
            m_clodReyesTerrainNormalBlend = std::clamp(m_clodReyesTerrainNormalBlend, 0.0f, 1.0f);
            setCLodReyesTerrainNormalBlend(m_clodReyesTerrainNormalBlend);
        }
        if (ImGui::SliderInt("Reyes Terrain Normal Mip Bias", &m_clodReyesTerrainNormalMipBias, 0, static_cast<int>(CLodReyesTerrainNormalMipBiasMax))) {
            m_clodReyesTerrainNormalMipBias = std::clamp(
                m_clodReyesTerrainNormalMipBias,
                0,
                static_cast<int>(CLodReyesTerrainNormalMipBiasMax));
            setCLodReyesTerrainNormalMipBias(static_cast<uint32_t>(m_clodReyesTerrainNormalMipBias));
        }
        if (ImGui::SliderFloat("Reyes Dice Rate Pixels", &m_clodReyesDiceRatePixels, CLodReyesDiceRatePixelsMin, CLodReyesDiceRatePixelsMax, "%.3f")) {
            m_clodReyesDiceRatePixels = std::clamp(
                m_clodReyesDiceRatePixels,
                CLodReyesDiceRatePixelsMin,
                CLodReyesDiceRatePixelsMax);
            setCLodReyesDiceRatePixels(m_clodReyesDiceRatePixels);
        }
        if (ImGui::Checkbox("Reyes AABB Occlusion", &m_clodReyesUseAabbOcclusion)) {
            setCLodReyesUseAabbOcclusion(m_clodReyesUseAabbOcclusion);
        }
        if (ImGui::Checkbox("Disable VSM Page Caching", &m_clodDisableVirtualShadowPageCaching)) {
            setCLodDisableVirtualShadowPageCaching(m_clodDisableVirtualShadowPageCaching);
        }
        bool forceTraversalDepthRoot = m_clodForceTraversalDepthRoot != CLodForceTraversalDepthRootDisabled;
        if (ImGui::Checkbox("Force CLod Traversal Depth Root", &forceTraversalDepthRoot)) {
            m_clodForceTraversalDepthRoot = forceTraversalDepthRoot ? 0u : CLodForceTraversalDepthRootDisabled;
            setCLodForceTraversalDepthRoot(m_clodForceTraversalDepthRoot);
        }
        if (forceTraversalDepthRoot) {
            int forcedDepth = static_cast<int>(std::min<uint32_t>(m_clodForceTraversalDepthRoot, 255u));
            if (ImGui::InputInt("Forced CLod Depth Root", &forcedDepth)) {
                forcedDepth = std::max(forcedDepth, 0);
                m_clodForceTraversalDepthRoot = static_cast<uint32_t>(forcedDepth);
                setCLodForceTraversalDepthRoot(m_clodForceTraversalDepthRoot);
            }
        }
        int visibleClusterCapacityM = static_cast<int>((m_clodVisibleClusterCapacity + 999999u) / 1000000u);
        if (ImGui::SliderInt("CLod Visible Cluster Capacity (M)", &visibleClusterCapacityM, 1, 30)) {
            const uint32_t capacity = static_cast<uint32_t>(std::clamp(visibleClusterCapacityM, 1, 30)) * 1000000u;
            m_clodVisibleClusterCapacity = std::clamp(
                capacity,
                CLodMinVisibleClusterCapacity,
                CLodMaxVisibleClusterCapacity);
            setCLodVisibleClusterCapacity(m_clodVisibleClusterCapacity);
        }
        if (ImGui::SliderFloat(
                "Shadow Reyes Coarse Target Pages/Triangle",
                &m_clodReyesShadowCoarseTargetPagesPerTriangle,
                CLodReyesShadowCoarseTargetPagesPerTriangleMin,
                CLodReyesShadowCoarseTargetPagesPerTriangleMax,
                "%.2f")) {
            m_clodReyesShadowCoarseTargetPagesPerTriangle = std::clamp(
                m_clodReyesShadowCoarseTargetPagesPerTriangle,
                CLodReyesShadowCoarseTargetPagesPerTriangleMin,
                CLodReyesShadowCoarseTargetPagesPerTriangleMax);
            setCLodReyesShadowCoarseTargetPagesPerTriangle(m_clodReyesShadowCoarseTargetPagesPerTriangle);
        }
        if (m_clodVSMRasterMode == CLodVSMRasterMode::PageJob) {
            int diameterThreshold = static_cast<int>(m_clodPageJobDiameterThreshold);
            if (ImGui::SliderInt("Page-Job Diameter Threshold", &diameterThreshold, 1, 255)) {
                m_clodPageJobDiameterThreshold = static_cast<uint32_t>(std::clamp(diameterThreshold, 1, 255));
                setCLodPageJobDiameterThreshold(m_clodPageJobDiameterThreshold);
            }
            if (ImGui::SliderFloat("Page-Job Sparse Ratio", &m_clodPageJobSparseRatio, 0.0f, 1.0f, "%.2f")) {
                setCLodPageJobSparseRatio(m_clodPageJobSparseRatio);
            }
            int maxPages = static_cast<int>(m_clodPageJobMaxPagesPerCluster);
            if (ImGui::SliderInt("Page-Job Max Pages/Cluster", &maxPages, 1, 255)) {
                m_clodPageJobMaxPagesPerCluster = static_cast<uint32_t>(std::clamp(maxPages, 1, 255));
                setCLodPageJobMaxPagesPerCluster(m_clodPageJobMaxPagesPerCluster);
            }
            int pageJobRecordCapacity = static_cast<int>(m_clodPageJobRecordCapacity);
            if (ImGui::SliderInt("Page-Job Record Capacity", &pageJobRecordCapacity, 1, 8 * 1024 * 1024)) {
                m_clodPageJobRecordCapacity = static_cast<uint32_t>(std::clamp(pageJobRecordCapacity, 1, 8 * 1024 * 1024));
                setCLodPageJobRecordCapacity(m_clodPageJobRecordCapacity);
            }
            if (ImGui::Checkbox("Force All Opaque VSM -> Page-Job", &m_clodPageJobForceAll)) {
                setCLodPageJobForceAll(m_clodPageJobForceAll);
            }
        }
        int directionalLightClipmaps = static_cast<int>(m_numDirectionalLightCascades);
        if (ImGui::SliderInt("Directional VSM Clipmaps", &directionalLightClipmaps, 1, static_cast<int>(CLodVirtualShadowMaxSupportedClipmapCount))) {
            directionalLightClipmaps = std::clamp(directionalLightClipmaps, 1, static_cast<int>(CLodVirtualShadowMaxSupportedClipmapCount));
            m_numDirectionalLightCascades = static_cast<uint8_t>(directionalLightClipmaps);
            setNumDirectionalLightCascades(m_numDirectionalLightCascades);
        }
        static constexpr uint32_t kDirectionalVsmBackingResolutionOptions[] = {
            CLodVirtualShadowMinBackingResolution,
            CLodVirtualShadowMediumBackingResolution,
            CLodVirtualShadowMaxBackingResolution,
        };
        static constexpr const char* kDirectionalVsmBackingResolutionLabels[] = {
            "4K",
            "8K",
            "16K",
        };
        int directionalVsmBackingResolutionIndex = 0;
        for (int optionIndex = 0; optionIndex < static_cast<int>(std::size(kDirectionalVsmBackingResolutionOptions)); ++optionIndex) {
            if (CLodVirtualShadowSanitizeBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution) ==
                kDirectionalVsmBackingResolutionOptions[optionIndex]) {
                directionalVsmBackingResolutionIndex = optionIndex;
                break;
            }
        }
        if (ImGui::Combo(
                "Directional VSM Backing Size",
                &directionalVsmBackingResolutionIndex,
                kDirectionalVsmBackingResolutionLabels,
                static_cast<int>(std::size(kDirectionalVsmBackingResolutionLabels)))) {
            directionalVsmBackingResolutionIndex = std::clamp(
                directionalVsmBackingResolutionIndex,
                0,
                static_cast<int>(std::size(kDirectionalVsmBackingResolutionOptions)) - 1);
            m_clodDirectionalVirtualShadowMaxBackingResolution =
                kDirectionalVsmBackingResolutionOptions[directionalVsmBackingResolutionIndex];
            setCLodDirectionalVirtualShadowMaxBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution);

            const uint32_t backingMaxPhysicalPages = CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(
                m_clodDirectionalVirtualShadowMaxBackingResolution);
            if (m_clodDirectionalVirtualShadowMaxPhysicalPages > backingMaxPhysicalPages) {
                m_clodDirectionalVirtualShadowMaxPhysicalPages = backingMaxPhysicalPages;
                setCLodDirectionalVirtualShadowMaxPhysicalPages(m_clodDirectionalVirtualShadowMaxPhysicalPages);
            }
        }
        const uint32_t backingMaxPhysicalPages = CLodVirtualShadowMaxPhysicalPageCountFromBackingResolution(
            m_clodDirectionalVirtualShadowMaxBackingResolution);
        int maxPhysicalPages = static_cast<int>(m_clodDirectionalVirtualShadowMaxPhysicalPages);
        if (ImGui::SliderInt("Directional VSM Physical Pages", &maxPhysicalPages, 1, static_cast<int>(backingMaxPhysicalPages))) {
            maxPhysicalPages = std::clamp(maxPhysicalPages, 1, static_cast<int>(backingMaxPhysicalPages));
            m_clodDirectionalVirtualShadowMaxPhysicalPages = static_cast<uint32_t>(maxPhysicalPages);
            setCLodDirectionalVirtualShadowMaxPhysicalPages(m_clodDirectionalVirtualShadowMaxPhysicalPages);
        }
        if (ImGui::Checkbox("Auto Directional VSM LOD Bias", &m_clodDirectionalVirtualShadowAutoLodBias)) {
            setCLodDirectionalVirtualShadowAutoLodBias(m_clodDirectionalVirtualShadowAutoLodBias);
        }
        if (ImGui::SliderFloat("Directional VSM Manual LOD Bias", &m_clodDirectionalVirtualShadowLodBias, -4.0f, 4.0f, "%.2f")) {
            setCLodDirectionalVirtualShadowLodBias(m_clodDirectionalVirtualShadowLodBias);
        }
        if (ImGui::SliderFloat("Directional VSM Auto Bias Scale", &m_clodDirectionalVirtualShadowAutoLodBiasScale, 0.0f, 4.0f, "%.2f")) {
            m_clodDirectionalVirtualShadowAutoLodBiasScale = std::max(m_clodDirectionalVirtualShadowAutoLodBiasScale, 0.0f);
            setCLodDirectionalVirtualShadowAutoLodBiasScale(m_clodDirectionalVirtualShadowAutoLodBiasScale);
        }
        if (ImGui::Checkbox("Predictive VSM LOD Invalidation", &m_clodDirectionalVirtualShadowPredictiveLodInvalidation)) {
            setCLodDirectionalVirtualShadowPredictiveLodInvalidation(m_clodDirectionalVirtualShadowPredictiveLodInvalidation);
        }
        if (ImGui::SliderFloat("Directional VSM Source Angle", &m_clodDirectionalVirtualShadowSourceAngleDegrees, 0.0f, 10.0f, "%.2f deg")) {
            m_clodDirectionalVirtualShadowSourceAngleDegrees = std::max(m_clodDirectionalVirtualShadowSourceAngleDegrees, 0.0f);
            setCLodDirectionalVirtualShadowSourceAngleDegrees(m_clodDirectionalVirtualShadowSourceAngleDegrees);
        }
        int smrtRayCountDirectional = static_cast<int>(m_clodDirectionalVirtualShadowSmrtRayCountDirectional);
        if (ImGui::SliderInt("Directional VSM SMRT Rays", &smrtRayCountDirectional, 0, 32)) {
            m_clodDirectionalVirtualShadowSmrtRayCountDirectional = static_cast<uint32_t>(std::max(smrtRayCountDirectional, 0));
            setCLodDirectionalVirtualShadowSmrtRayCountDirectional(m_clodDirectionalVirtualShadowSmrtRayCountDirectional);
        }
        int smrtSamplesPerRayDirectional = static_cast<int>(m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional);
        if (ImGui::SliderInt("Directional VSM SMRT Samples/Ray", &smrtSamplesPerRayDirectional, 0, 16)) {
            m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional = static_cast<uint32_t>(std::max(smrtSamplesPerRayDirectional, 0));
            setCLodDirectionalVirtualShadowSmrtSamplesPerRayDirectional(m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional);
        }
        if (ImGui::SliderFloat(
                "Directional VSM SMRT Max Ray Angle",
                &m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees,
                0.0f,
                15.0f,
                "%.2f deg")) {
            m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees =
                std::max(m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees, 0.0f);
            setCLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees(
                m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees);
        }
        if (ImGui::SliderFloat(
                "Directional VSM SMRT Ray Length Scale",
                &m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional,
                0.0f,
                0.25f,
                "%.3f")) {
            m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional =
                std::max(m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional, 0.0f);
            setCLodDirectionalVirtualShadowSmrtRayLengthScaleDirectional(
                m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional);
        }
        if (ImGui::SliderFloat(
                "Directional VSM SMRT Max Trace Distance",
                &m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld,
                1.0f,
                2000.0f,
                "%.1f")) {
            m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld =
                std::max(m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld, 1.0f);
            setCLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld(
                m_clodDirectionalVirtualShadowSmrtMaxTraceDistanceWorld);
        }
        const CLodVirtualShadowResolutionConfig virtualShadowConfig =
            CLodVirtualShadowBuildRuntimeResolutionConfig();
        const float budgetDirectionalLodBias = m_clodDirectionalVirtualShadowAutoLodBias
            ? CLodVirtualShadowAutomaticDirectionalLodBiasFromBudget(
                virtualShadowConfig.maxPhysicalPages,
                m_clodDirectionalVirtualShadowAutoLodBiasScale)
            : 0.0f;
        ImGui::Text(
            "Directional VSM Virtual Space: %u x %u pages (%u texels/page, fixed 16K)",
            CLodVirtualShadowFixedVirtualPageCountPerAxis,
            CLodVirtualShadowFixedVirtualPageCountPerAxis,
            CLodVirtualShadowPhysicalPageSize);
        ImGui::Text(
            "Directional VSM Backing Cap: %u x %u texels (%u pages max)",
            CLodVirtualShadowSanitizeBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution),
            CLodVirtualShadowSanitizeBackingResolution(m_clodDirectionalVirtualShadowMaxBackingResolution),
            backingMaxPhysicalPages);
        ImGui::Text(
            "Directional VSM Physical Atlas: %u x %u pages (%u total)",
            virtualShadowConfig.physicalAtlasPagesWide,
            virtualShadowConfig.physicalAtlasPagesHigh,
            virtualShadowConfig.maxPhysicalPages);
        ImGui::Text(
            "Directional VSM Bias: manual=%.2f budgetBase=%.2f configured=%.2f",
            m_clodDirectionalVirtualShadowLodBias,
            budgetDirectionalLodBias,
            virtualShadowConfig.directionalLodBias);
        ImGui::Text(
            "Directional VSM SMRT: angle=%.2f deg rays=%u samples/ray=%u maxRayAngle=%.2f deg rayLengthScale=%.2f",
            m_clodDirectionalVirtualShadowSourceAngleDegrees,
            m_clodDirectionalVirtualShadowSmrtRayCountDirectional,
            m_clodDirectionalVirtualShadowSmrtSamplesPerRayDirectional,
            m_clodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegrees,
            m_clodDirectionalVirtualShadowSmrtRayLengthScaleDirectional);
        if (ImGui::SliderFloat("Directional Shadow Vertical Extent", &m_directionalShadowVerticalExtent, 1.0f, 1000.0f, "%.1f")) {
            m_directionalShadowVerticalExtent = std::max(m_directionalShadowVerticalExtent, 1.0f);
            setDirectionalShadowVerticalExtent(m_directionalShadowVerticalExtent);
        }
		if (ImGui::Checkbox("Wireframe", &wireframeEnabled)) {
			setWireframeEnabled(wireframeEnabled);
		}
        if (ImGui::Checkbox("Uncap Framerate", &allowTearing)) {
			setAllowTearing(allowTearing);
        }
		if (ImGui::Checkbox("Draw Bounding Spheres", &drawBoundingSpheres)) {
			setDrawBoundingSpheres(drawBoundingSpheres);
		}
        if (ImGui::Checkbox("Clustered Lighting", &clusteredLighting)) {
			setClusteredLightingEnabled(clusteredLighting);
        }
        if (ImGui::Checkbox("Visibility Rendering", &m_visibilityRenderingEnabled)) {
            setVisibilityRenderingEnabled(m_visibilityRenderingEnabled);
        }
        if (ImGui::Checkbox("Terrain Region Material Evaluation", &m_terrainRegionMaterialEvaluationEnabled)) {
            setTerrainRegionMaterialEvaluationEnabled(m_terrainRegionMaterialEvaluationEnabled);
        }
        if (ImGui::Checkbox("Terrain Runtime Virtual Texture", &m_terrainRvtEnabled)) {
            setTerrainRvtEnabled(m_terrainRvtEnabled);
        }
        if (ImGui::Checkbox("Force Terrain RVT Direct Fallback", &m_forceDirectTerrainRvtFallback)) {
            setForceDirectTerrainRvtFallback(m_forceDirectTerrainRvtFallback);
        }
        if (ImGui::Checkbox("Terrain RVT Telemetry", &m_terrainRvtTelemetryDebug)) {
            setTerrainRvtTelemetryDebug(m_terrainRvtTelemetryDebug);
        }
        if (ImGui::SliderInt("Terrain RVT Debug View", &m_terrainRvtDebugView, 0, 4)) {
            setTerrainRvtDebugView(static_cast<uint32_t>(std::max(0, m_terrainRvtDebugView)));
        }
        if (ImGui::SliderInt("Terrain RVT Page Size", &m_terrainRvtPageSize, 16, 512)) {
            setTerrainRvtPageSize(static_cast<uint32_t>(std::clamp(m_terrainRvtPageSize, 16, 512)));
        }
        if (ImGui::SliderInt("Terrain RVT Border Texels", &m_terrainRvtBorderTexels, 0, 16)) {
            setTerrainRvtBorderTexels(static_cast<uint32_t>(std::clamp(m_terrainRvtBorderTexels, 0, 16)));
        }
        if (ImGui::SliderInt("Terrain RVT Clipmaps", &m_terrainRvtMipCount, 1, 24)) {
            setTerrainRvtMipCount(static_cast<uint32_t>(std::clamp(m_terrainRvtMipCount, 1, 24)));
        }
        if (ImGui::SliderFloat("Terrain RVT Mip Offset", &m_terrainRvtMipOffset, -4.0f, 4.0f, "%.2f")) {
            setTerrainRvtMipOffset(std::clamp(m_terrainRvtMipOffset, -8.0f, 8.0f));
        }
        if (ImGui::SliderFloat("Terrain RVT Source Texels/World", &m_terrainRvtSourceTexelsPerWorld, 1.0f, 128.0f, "%.2f")) {
            setTerrainRvtSourceTexelsPerWorld(std::max(0.001f, m_terrainRvtSourceTexelsPerWorld));
        }
        if (ImGui::SliderInt("Terrain RVT Physical Atlas Pages Wide", &m_terrainRvtPhysicalAtlasPagesWide, 1, 128)) {
            setTerrainRvtPhysicalAtlasPagesWide(static_cast<uint32_t>(std::clamp(m_terrainRvtPhysicalAtlasPagesWide, 1, 128)));
        }
        if (ImGui::SliderInt("Terrain RVT Physical Atlas Pages High", &m_terrainRvtPhysicalAtlasPagesHigh, 1, 128)) {
            setTerrainRvtPhysicalAtlasPagesHigh(static_cast<uint32_t>(std::clamp(m_terrainRvtPhysicalAtlasPagesHigh, 1, 128)));
        }
        if (ImGui::SliderInt("Terrain RVT Physical Atlas Pools", &m_terrainRvtPhysicalAtlasPoolCount, 1, 8)) {
            setTerrainRvtPhysicalAtlasPoolCount(static_cast<uint32_t>(std::clamp(m_terrainRvtPhysicalAtlasPoolCount, 1, 8)));
        }
        if (ImGui::Checkbox("Terrain Stochastic Sampling", &m_terrainStochasticSamplingEnabled)) {
            setTerrainStochasticSamplingEnabled(m_terrainStochasticSamplingEnabled);
        }
        if (ImGui::Checkbox("Terrain Stochastic Diffuse", &m_terrainStochasticDiffuseSamplingEnabled)) {
            setTerrainStochasticDiffuseSamplingEnabled(m_terrainStochasticDiffuseSamplingEnabled);
        }
        if (ImGui::Checkbox("Terrain Stochastic Normals", &m_terrainStochasticNormalSamplingEnabled)) {
            setTerrainStochasticNormalSamplingEnabled(m_terrainStochasticNormalSamplingEnabled);
        }
        if (ImGui::Checkbox("Terrain Derivative Normal Blend", &m_terrainStochasticDerivativeNormalSamplingEnabled)) {
            setTerrainStochasticDerivativeNormalSamplingEnabled(m_terrainStochasticDerivativeNormalSamplingEnabled);
        }
        if (ImGui::SliderFloat("Terrain Stochastic Blend Curve", &m_terrainStochasticBlendCurve, 0.0f, 1.0f, "%.2f")) {
            setTerrainStochasticBlendCurve(m_terrainStochasticBlendCurve);
        }
        if (ImGui::Checkbox("Terrain Gaussian Stochastic Variant", &m_terrainGaussianStochasticSamplingEnabled)) {
            setTerrainGaussianStochasticSamplingEnabled(m_terrainGaussianStochasticSamplingEnabled);
        }
        if (ImGui::Checkbox("Parallax Occlusion Mapping", &m_parallaxOcclusionMappingEnabled)) {
            setParallaxOcclusionMappingEnabled(m_parallaxOcclusionMappingEnabled);
        }
        if (ImGui::Checkbox("Terrain Parallax Occlusion Mapping", &m_terrainParallaxOcclusionMappingEnabled)) {
            setTerrainParallaxOcclusionMappingEnabled(m_terrainParallaxOcclusionMappingEnabled);
        }
        if (ImGui::Checkbox("Terrain Reyes Displacement", &m_terrainReyesDisplacementEnabled)) {
            setTerrainReyesDisplacementEnabled(m_terrainReyesDisplacementEnabled);
        }
        if (ImGui::SliderFloat("Terrain Reyes Displacement Global Scale", &m_terrainReyesDisplacementScale, 0.0f, 16.0f, "%.2f")) {
            m_terrainReyesDisplacementScale = std::max(0.0f, m_terrainReyesDisplacementScale);
            setTerrainReyesDisplacementScale(m_terrainReyesDisplacementScale);
        }
        if (ImGui::SliderFloat("Object Reyes Displacement Global Scale", &m_objectReyesDisplacementScale, 0.0f, 1000.0f, "%.2f")) {
            m_objectReyesDisplacementScale = std::max(0.0f, m_objectReyesDisplacementScale);
            setObjectReyesDisplacementScale(m_objectReyesDisplacementScale);
        }
        if (ImGui::SliderFloat("Terrain Parallax Height Scale", &m_terrainParallaxHeightScale, 0.0f, 0.20f, "%.3f")) {
            setTerrainParallaxHeightScale(m_terrainParallaxHeightScale);
        }
        if (ImGui::SliderFloat("Object Parallax Height Scale", &m_objectParallaxHeightScale, 0.0f, 16.0f, "%.2f")) {
            m_objectParallaxHeightScale = std::max(0.0f, m_objectParallaxHeightScale);
            setObjectParallaxHeightScale(m_objectParallaxHeightScale);
        }
        int terrainParallaxMaxSteps = static_cast<int>(m_terrainParallaxMaxSteps);
        if (ImGui::SliderInt("Terrain Parallax Max Steps", &terrainParallaxMaxSteps, 4, 32)) {
            m_terrainParallaxMaxSteps = static_cast<uint32_t>(std::clamp(terrainParallaxMaxSteps, 4, 32));
            setTerrainParallaxMaxSteps(m_terrainParallaxMaxSteps);
        }
        if (ImGui::SliderFloat("Height Fade Start", &m_terrainParallaxFadeStartDistance, 0.0f, 32768.0f, "%.0f")) {
            m_terrainParallaxFadeStartDistance = std::max(0.0f, m_terrainParallaxFadeStartDistance);
            setTerrainParallaxFadeStartDistance(m_terrainParallaxFadeStartDistance);
        }
        if (ImGui::SliderFloat("Height Fade End", &m_terrainParallaxFadeEndDistance, 0.0f, 65536.0f, "%.0f")) {
            m_terrainParallaxFadeEndDistance = std::max(0.0f, m_terrainParallaxFadeEndDistance);
            setTerrainParallaxFadeEndDistance(m_terrainParallaxFadeEndDistance);
        }
		if (ImGui::Checkbox("Enable GTAO", &m_gtaoEnabled)) {
			setGTAOEnabled(m_gtaoEnabled);
		}
		if (ImGui::Checkbox("Enable Bloom", &m_bloomEnabled)) {
			setBloomEnabled(m_bloomEnabled);
		}
        if (ImGui::Checkbox("Enable Screen Space Reflections", &m_screenSpaceReflectionsEnabled)) {
            setScreenSpaceReflectionsEnabled(m_screenSpaceReflectionsEnabled);
		}
        const bool clodRtSupported = DeviceManager::GetInstance().GetCLodRayTracingSupported();
        if (!clodRtSupported) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Checkbox("Enable Ray Traced Reflections", &m_rayTracedReflectionsEnabled)) {
            setRayTracedReflectionsEnabled(m_rayTracedReflectionsEnabled);
        }
        if (!clodRtSupported) {
            ImGui::EndDisabled();
        }
        const RayTracingFeatureInfo& rtFeatures = DeviceManager::GetInstance().GetRayTracingFeatures();
        ImGui::TextDisabled(
            "RT: pipeline %s, AS %s, GPU RTAS %s, cluster AS %s, max cluster %u verts/%u tris",
            rtFeatures.pipeline ? "yes" : "no",
            rtFeatures.accelerationStructure ? "yes" : "no",
            rtFeatures.gpuRtasOperations ? "yes" : "no",
            rtFeatures.clusterAccelerationStructure ? "yes" : "no",
            rtFeatures.maxClusterVertices,
            rtFeatures.maxClusterTriangles);
        if (context.clodRayTracingSystem) {
            const auto& clodRtStats = context.clodRayTracingSystem->GetStats();
            ImGui::TextDisabled(
                "CLod RT: groups %u pages %u clusters %u, GPU %s, CLAS %s, BLAS %s, TLAS %s, pipeline %s, trace %s",
                clodRtStats.residentGroups,
                clodRtStats.residentPages,
                clodRtStats.buildableClusters,
                clodRtStats.gpuResourcesReady ? "yes" : "no",
                clodRtStats.clasBuildSubmitted ? "yes" : "no",
                clodRtStats.blasBuildSubmitted ? "yes" : "no",
                clodRtStats.tlasBuildSubmitted ? "yes" : "no",
                clodRtStats.rayPipelineReady ? "yes" : "no",
                clodRtStats.traceRaysSubmitted ? "yes" : "no");
        }
        if (ImGui::Checkbox("Enable Jitter", &m_jitterEnabled)) {
            setJitterEnabled(m_jitterEnabled);
        }
        if (ImGui::Checkbox("Collect Pass Statistics", &m_collectPassStatistics)) {
            setCollectPassStatistics(m_collectPassStatistics);
        }
		if (ImGui::Checkbox("Collect Pipeline Statistics", &m_collectPipelineStatistics)) {
			setCollectPipelineStatistics(m_collectPipelineStatistics);
		}
        DrawUpscalingCombo();
        DrawUpscalingQualityCombo();
        DrawTonemapTypeDropdown();

        DrawEnvironmentsDropdown();
        DrawBrowseButton(environmentsDir.wstring());
		DrawOutputTypeDropdown();
        DrawLoadModelButton();
		if (ImGui::Checkbox("Use Async Compute", &m_useAsyncCompute)) {
			setUseAsyncCompute(m_useAsyncCompute);
		}
		if (ImGui::Checkbox("Heavy Debug (1 pass/batch + GPU drain)", &m_heavyDebug)) {
			setHeavyDebug(m_heavyDebug);
		}
		if (ImGui::Checkbox("Disable Render Graph Caching", &m_renderGraphDisableCaching)) {
			setRenderGraphDisableCaching(m_renderGraphDisableCaching);
		}
        if (ImGui::Checkbox("Render Graph Batch Trace", &m_renderGraphBatchTraceEnabled)) {
            setRenderGraphBatchTraceEnabled(m_renderGraphBatchTraceEnabled);
        }
		if (ImGui::Checkbox("Render Graph Compile Summary", &m_renderGraphLightweightCompileSummaryEnabled)) {
			setRenderGraphLightweightCompileSummaryEnabled(m_renderGraphLightweightCompileSummaryEnabled);
		}
        if (ImGui::Checkbox("ReShape texel addressing (requires recreate)", &m_reshapeTexelAddressing)) {
            setReshapeTexelAddressing(m_reshapeTexelAddressing);
        }
        int clodCpuUploadBudget = static_cast<int>(std::min<uint32_t>(m_clodStreamingCpuUploadBudgetRequests, 4096u));
        if (ImGui::SliderInt("CLod CPU Upload Budget", &clodCpuUploadBudget, 1, 4096)) {
            m_clodStreamingCpuUploadBudgetRequests = static_cast<uint32_t>(std::max(clodCpuUploadBudget, 1));
            setCLodStreamingCpuUploadBudgetRequests(m_clodStreamingCpuUploadBudgetRequests);
        }
        if (ImGui::Checkbox("CLod Streaming DirectStorage", &m_clodStreamingEnableDirectStorage)) {
            setCLodStreamingEnableDirectStorage(m_clodStreamingEnableDirectStorage);
        }
        ImGui::Checkbox("Render Graph Inspector", &showRG);
        ImGui::Checkbox("Memory introspection", &showMemoryIntrospection);
        ImGui::Checkbox("CLod telemetry", &showCLodTelemetry);
        ImGui::Checkbox("CPU frame task graph", &showFrameTaskGraph);
        ImGui::Checkbox("Auto Alias Planner", &showAutoAliasPlanner);
        if (setAutoAliasBuildDebugData) {
            setAutoAliasBuildDebugData(showAutoAliasPlanner);
        }
        ImGui::Checkbox("GPU instrumentation", &showGpuInstrumentation);
        ImGui::Checkbox("Material texture streaming", &showMaterialTextureStreaming);
        std::string memoryString = "Memory usage: unavailable";
        const double KiB = 1024.0;
        const double MiB = KiB * 1024.0;
        const double GiB = MiB * 1024.0;
        if (m_renderGraph) {
            if (auto* statisticsService = m_renderGraph->GetStatisticsService()) {
                const auto memoryBudgetStats = statisticsService->GetMemoryBudgetStats();
                if (memoryBudgetStats.valid) {
                    const auto usage = static_cast<double>(memoryBudgetStats.usageBytes);

                    const auto [div, suffix] =
                        (usage >= GiB) ? std::pair{ GiB, "GB" } :
                        (usage >= MiB) ? std::pair{ MiB, "MB" } :
                        (usage >= KiB) ? std::pair{ KiB, "KB" } :
                        std::pair{ 1.0, "B" };

                    memoryString = std::format("Memory usage: {:.2f} {} / {:.2f} GB",
                        usage / div, suffix,
                        static_cast<double>(memoryBudgetStats.budgetBytes) / GiB);
                }
            }
        }

        ImGui::Text(memoryString.c_str());
        ImGui::Text("Render Resolution: %d x %d | Output Resolution: %d x %d", context.renderResolution.x, context.renderResolution.y, context.outputResolution.x, context.outputResolution.y);
        ImGui::Checkbox("Enable Menu", &m_menuEnabled);
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", msPerFrame, fps);
		ImGui::End();
	}
	if (showMemoryIntrospection) {
        static ui::MemoryIntrospectionWidget g_memWidget;

        std::vector<rg::memory::ResourceMemoryRecord> memoryRecords;
    if (m_renderGraph) {
        m_renderGraph->GetMemorySnapshotProvider().BuildSnapshot(memoryRecords);
    }

        ui::MemorySnapshot snap;
        PerResourceMemIndex memIndex;
        BuildMemorySnapshotFromRecords(snap, memoryRecords, &memIndex);

		ui::FrameGraphSnapshot fgSnap;
        if (m_renderGraph) {
            m_renderGraph->BuildMemoryIntrospectionFrameGraphSnapshot(fgSnap, memoryRecords);
        }


        ImGui::Begin("Memory Introspection", nullptr);
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsedSeconds = now - m_startTime;
        uint64_t totalBytes = snap.totalBytes;
        g_memWidget.PushFrameSample(elapsedSeconds.count(), totalBytes);
        bool open = true;
        g_memWidget.Draw(&open, &snap, &fgSnap);
		ImGui::End();
	}

    if (showMaterialTextureStreaming) {
        ImGui::Begin("Material Texture Streaming", &showMaterialTextureStreaming);
        if (materialTextureStreamingStats.has_value()) {
            const auto& stats = *materialTextureStreamingStats;
            ImGui::Text("Active material textures: %u", stats.uniqueMaterialTextureCount);
            ImGui::Text(
                "Streamable / enabled: %u / %u",
                stats.uniqueStreamableTextureCount,
                stats.uniqueStreamingEnabledTextureCount);
            ImGui::Text(
                "Full-res resident: %u total, %u streamable",
                stats.fullResolutionResidentTextureCount,
                stats.streamableFullResolutionResidentTextureCount);
            ImGui::Text("Pending reloads: %u", stats.pendingReloadTextureCount);
            ImGui::Text("Resident bytes: %s", formatBytes(stats.totalResidentBytes).c_str());
            ImGui::Text("Streamable resident bytes: %s", formatBytes(stats.streamableResidentBytes).c_str());

            if (!stats.residentTopMipHistogram.empty()) {
                std::vector<float> histogramValues;
                histogramValues.reserve(stats.residentTopMipHistogram.size());
                for (uint32_t count : stats.residentTopMipHistogram) {
                    histogramValues.push_back(static_cast<float>(count));
                }

                ImGui::SeparatorText("Resident top mip histogram");
                ImGui::PlotHistogram(
                    "##MaterialTextureResidentTopMipHistogram",
                    histogramValues.data(),
                    static_cast<int>(histogramValues.size()),
                    0,
                    nullptr,
                    0.0f,
                    *std::max_element(histogramValues.begin(), histogramValues.end()) + 1.0f,
                    ImVec2(420.0f, 180.0f));

                for (size_t mip = 0; mip < stats.residentTopMipHistogram.size(); ++mip) {
                    ImGui::Text("Top mip %zu: %u", mip, stats.residentTopMipHistogram[mip]);
                }
            }
            else {
                ImGui::TextUnformatted("No active material textures tracked.");
            }
        }
        else {
            ImGui::TextUnformatted("MaterialManager unavailable.");
        }
        ImGui::End();
    }

    {
		ImGui::Begin("Scene Graph", nullptr);
		DisplaySceneGraph();
		ImGui::End();

		DisplaySelectedNode();

        DrawPassTimingWindow();
    }
    
    if (showRG) {
		ImGui::Begin("Render Graph Inspector", nullptr);
		RGInspectorOptions opts;
        if ((m_imguiBackend == rhi::Backend::D3D12 || m_imguiBackend == rhi::Backend::Vulkan) && g_pd3dSrvDescHeap) {
            opts.imguiAllocDescriptor = [this]() { return AllocateImGuiDescriptor(); };
            opts.imguiFreeDescriptor = [this](uint32_t idx) { FreeImGuiDescriptor(idx); };
            opts.imguiGpuHandle = [this](uint32_t idx) { return GetImGuiGpuDescriptorHandle(idx); };
            opts.imguiHeapHandle = GetImGuiHeapHandle();
        }
        opts.cacheOverlayProvider = [this]() -> std::vector<RGCacheOverlayRange> {
            if (!m_renderGraph) {
                return {};
            }
            return m_renderGraph->BuildReplayCacheOverlayRanges();
        };
        RGInspector::Show(m_renderGraph->GetBatches(),
            m_renderGraph->GetQueueRegistry(),
            PassUsesResourceAdapter,
            [this](uint64_t resourceId) -> std::string {
                if (!m_renderGraph) return {};
                auto resource = m_renderGraph->GetResourceByID(resourceId);
                if (!resource) return {};
                return resource->GetName();
            },
            [this](uint64_t resourceId) -> Resource* {
                if (!m_renderGraph) return nullptr;
                auto resource = m_renderGraph->GetResourceByID(resourceId);
                return resource ? resource.get() : nullptr;
            },
            [this](const std::string& passName, Resource* resource, const RangeSpec& range, ReadbackCaptureCallback callback) {
                if (!m_renderGraph) {
                    return;
                }
                if (auto* readbackService = m_renderGraph->GetReadbackService()) {
                    readbackService->RequestReadbackCapture(passName, resource, range, std::move(callback));
                }
            },
            opts);
        ImGui::End();

    }

    if (showCLodTelemetry) {
        DrawCLodTelemetryWindow();
    }

    if (showGpuInstrumentation) {
        static rhi::debug::InstrumentationWidget g_gpuInstrumentationWidget;
        g_gpuInstrumentationWidget.Draw(DeviceManager::GetInstance().GetDevice(), &showGpuInstrumentation);
    }

    if (showFrameTaskGraph) {
        DrawFrameTaskGraphWindow();
    }

    if (showAutoAliasPlanner) {
        DrawAutoAliasPlannerWindow();
    }

	// Rendering
	ImGui::Render();

    if (m_imguiBackend == rhi::Backend::Null) {
        return;
    }

    if (m_imguiBackend == rhi::Backend::D3D12) {
        if (!g_pd3dSrvDescHeap) {
            return;
        }
        commandList.SetDescriptorHeaps(g_pd3dSrvDescHeap->GetHandle(), std::nullopt);
    }

	rhi::PassBeginInfo beginInfo{};
	rhi::ColorAttachment attchment{};
    attchment.loadOp = rhi::LoadOp::Load;
	attchment.rtv = { context.rtvHeap.GetHandle() , context.frameIndex }; // Index into the swapchain RTV heap
	beginInfo.colors = { &attchment };
    beginInfo.height = context.outputResolution.y;
    beginInfo.width = context.outputResolution.x;

	commandList.BeginPass(beginInfo);

    if (m_imguiBackend == rhi::Backend::D3D12) {
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), rhi::dx12::get_cmd_list(commandList));
    }
#if BASICRENDERER_HAS_IMGUI_VULKAN
    else if (m_imguiBackend == rhi::Backend::Vulkan) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), rhi::vulkan::get_cmd_list(commandList));
    }
#endif

}

inline int Menu::FindFileIndex(const std::vector<std::string>& inputHdrFiles, const std::string& existingFile) {
    for (unsigned int i = 0; i < inputHdrFiles.size(); ++i)
    {
        if (inputHdrFiles[i] == existingFile)
        {
            return i;
        }
    }
    return -1;
}

inline void Menu::DrawEnvironmentsDropdown() {
    static int selectedItemIndex = FindFileIndex(hdrFiles, environmentName);

    const char* previewValue = (selectedItemIndex >= 0)
        ? hdrFiles[selectedItemIndex].c_str()
        : "Select Environment";

    if (ImGui::BeginCombo("HDR Files", previewValue))
    {
        for (int i = 0; i < (int)hdrFiles.size(); ++i)
        {
            bool isSelected = (selectedItemIndex == i);
            if (ImGui::Selectable(hdrFiles[i].c_str(), isSelected))
            {
                selectedItemIndex = i;
                environmentName   = hdrFiles[i];
                setEnvironment(hdrFiles[i]);
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

inline void Menu::DrawOutputTypeDropdown() {
	static unsigned int selectedItemIndex = 0;
    if (ImGui::BeginCombo("Output Type", OutputTypeNames[selectedItemIndex].c_str())) {
		for (unsigned int i = 0; i < OutputTypeNames.size(); ++i) {
			bool isSelected = (selectedItemIndex == i);
			if (ImGui::Selectable(OutputTypeNames[i].c_str(), isSelected)) {
				selectedItemIndex = i;
				setOutputType(i);
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();

    }
}

inline void Menu::DrawUpscalingCombo()
{
    int modeIdx = static_cast<int>(m_currentUpscalingMode);

    if (ImGui::Combo("Upscaling Mode", &modeIdx, UpscalingModeNames, UpscalingModeCount))
    {
        m_currentUpscalingMode = static_cast<UpscalingMode>(modeIdx);
		setUpscalingMode(m_currentUpscalingMode);
    }
}

inline void Menu::DrawUpscalingQualityCombo()
{
    int modeIdx = static_cast<int>(m_currentUpscalingQualityMode);

    if (ImGui::Combo("Upscaling Quality", &modeIdx, UpscaleQualityModeNames, UpscaleQualityModeCount))
    {
        m_currentUpscalingQualityMode = static_cast<UpscaleQualityMode>(modeIdx);
        setUpscalingQualityMode(m_currentUpscalingQualityMode);
    }
}

inline void Menu::DrawTonemapTypeDropdown() {
    static unsigned int selectedItemIndex = 0;
	selectedItemIndex = getTonemapType();
    if (ImGui::BeginCombo("Tonemap Type", TonemapTypeNames[selectedItemIndex].c_str())) {
        for (unsigned int i = 0; i < TonemapTypeNames.size(); ++i) {
            bool isSelected = (selectedItemIndex == i);
            if (ImGui::Selectable(TonemapTypeNames[i].c_str(), isSelected)) {
                selectedItemIndex = i;
                setTonemapType(i);
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();

    }
}


inline void Menu::DrawBrowseButton(const std::wstring& targetDirectory) {
    if (ImGui::Button("Browse"))
    {
        std::wstring selectedFile;
        std::wstring customFilter = L"HDR Files\0*.hdr\0All Files\0*.*\0";
        if (OpenFileDialog(selectedFile, customFilter))
        {
            spdlog::info("Selected file: {}", ws2s(selectedFile));

            CopyFileToDirectory(selectedFile, targetDirectory);
            hdrFiles = GetFilesInDirectoryMatchingExtension(environmentsDir, L".hdr");
        }
        else
        {
            spdlog::warn("No file selected.");
        }
    }
}

inline void Menu::DrawLoadModelButton() {
    if (ImGui::Button("Load Model"))
    {
        std::wstring selectedFile;
        std::wstring customFilter = L"Scene Files\0*.glb;*.gltf;*.usd;*.usda;*.usdc;*.usdz;*.nif\0All Files\0*.*\0";
        if (OpenFileDialog(selectedFile, customFilter))
        {
			//auto exePath = GetExePath();
			//// Strip EXE path from selectedFile
			//if (selectedFile.find(exePath) == 0) {
			//	selectedFile.erase(0, exePath.length());
			//}
			//// Strip filename from selectedFile
   //         auto pathCopy = selectedFile;
			//auto lastSlash = selectedFile.find_last_of(L"\\/");
			//if (lastSlash != std::wstring::npos) {
			//	selectedFile.erase(lastSlash, selectedFile.size()-1);
			//}

            spdlog::info("Selected file: {}", ws2s(selectedFile));
			auto scene = LoadModel(ws2s(selectedFile));
			scene->GetRoot().set<Components::Name>(ws2s(getFileNameFromPath(selectedFile)));
			appendScene(scene->Clone());
        }
        else
        {
            spdlog::warn("No file selected.");
        }
    }
}

inline Menu::SceneExplorerNodeSnapshot Menu::BuildSceneExplorerSnapshot(flecs::entity node, size_t& remainingNodes, bool& truncated) {
    SceneExplorerNodeSnapshot snapshot;
    if (!node.is_alive()) {
        return snapshot;
    }

    if (remainingNodes == 0) {
        truncated = true;
        return snapshot;
    }
    --remainingNodes;

    if (const auto* stableSceneID = node.try_get<Components::StableSceneID>()) {
        snapshot.stableId = stableSceneID->value;
    } else {
        snapshot.stableId = static_cast<uint64_t>(node.id());
    }

    if (const auto* nameComponent = node.try_get<Components::Name>()) {
        snapshot.name = nameComponent->name;
    } else {
        snapshot.name = "Unnamed Node";
    }

    if (const auto* position = node.try_get<Components::Position>()) {
        snapshot.hasPosition = true;
        XMStoreFloat3(&snapshot.position, position->pos);
    }

    if (const auto* scale = node.try_get<Components::Scale>()) {
        DirectX::XMFLOAT3 scaleValue{};
        snapshot.hasScale = true;
        XMStoreFloat3(&scaleValue, scale->scale);
        snapshot.uniformScale = scaleValue.x;
    }

    if (const auto* rotation = node.try_get<Components::Rotation>()) {
        snapshot.hasRotation = true;
        XMStoreFloat4(&snapshot.rotation, rotation->rot);
    }

    snapshot.isRenderable = node.has<Components::RenderableObject>();
    if (snapshot.isRenderable) {
        if (const auto* meshInstances = node.try_get<Components::MeshInstances>()) {
            snapshot.meshCount = meshInstances->meshInstances.size();
        }
        snapshot.skinned = node.has<Components::Skinned>();
    }

    auto* world = node.world().c_ptr();
    ecs_iter_t it = ecs_children(world, node.id());
    while (ecs_children_next(&it)) {
        for (int32_t i = 0; i < it.count; ++i) {
            if (remainingNodes == 0) {
                truncated = true;
                ecs_iter_fini(&it);
                return snapshot;
            }

            snapshot.children.push_back(BuildSceneExplorerSnapshot(flecs::entity(world, it.entities[i]), remainingNodes, truncated));
            if (remainingNodes == 0) {
                truncated = true;
                ecs_iter_fini(&it);
                return snapshot;
            }
        }
    }

    return snapshot;
}

inline const Menu::SceneExplorerNodeSnapshot* Menu::FindSceneExplorerSnapshotNode(const SceneExplorerNodeSnapshot& node, uint64_t stableId) const {
    if (node.stableId == stableId) {
        return &node;
    }

    for (const auto& child : node.children) {
        if (const auto* found = FindSceneExplorerSnapshotNode(child, stableId)) {
            return found;
        }
    }

    return nullptr;
}

inline Menu::SceneExplorerNodeSnapshot* Menu::FindSceneExplorerSnapshotNode(SceneExplorerNodeSnapshot& node, uint64_t stableId) {
    if (node.stableId == stableId) {
        return &node;
    }

    for (auto& child : node.children) {
        if (auto* found = FindSceneExplorerSnapshotNode(child, stableId)) {
            return found;
        }
    }

    return nullptr;
}

inline void Menu::OverlayPendingSceneExplorerEdits() {
    if (!m_sceneExplorerSnapshotAvailable) {
        return;
    }

    constexpr float kFloatEpsilon = 1e-4f;
    for (auto it = m_sceneExplorerPendingEdits.begin(); it != m_sceneExplorerPendingEdits.end();) {
        auto* node = FindSceneExplorerSnapshotNode(m_sceneExplorerRootSnapshot, it->first);
        if (!node) {
            it = m_sceneExplorerPendingEdits.erase(it);
            continue;
        }

        bool appliedToScene = true;
        if (it->second.hasPosition) {
            appliedToScene = appliedToScene
                && node->hasPosition
                && std::fabs(node->position.x - it->second.position.x) <= kFloatEpsilon
                && std::fabs(node->position.y - it->second.position.y) <= kFloatEpsilon
                && std::fabs(node->position.z - it->second.position.z) <= kFloatEpsilon;
            node->hasPosition = true;
            node->position = it->second.position;
        }

        if (it->second.hasUniformScale) {
            appliedToScene = appliedToScene
                && node->hasScale
                && std::fabs(node->uniformScale - it->second.uniformScale) <= kFloatEpsilon;
            node->hasScale = true;
            node->uniformScale = it->second.uniformScale;
        }

        if (appliedToScene) {
            it = m_sceneExplorerPendingEdits.erase(it);
        } else {
            ++it;
        }
    }
}

inline void Menu::RefreshSceneExplorerSnapshot(size_t maxNodes) {
    if (m_sceneOverlapStatus.taskInFlight) {
        return;
    }

    auto root = getSceneRoot();
    if (!root) {
        m_sceneExplorerSnapshotAvailable = false;
        m_sceneExplorerSnapshotTruncated = false;
        m_sceneExplorerSnapshotNodeBudget = 0;
        m_selectedSceneNodeStableId = 0;
        m_sceneExplorerPendingEdits.clear();
        return;
    }

    size_t remainingNodes = std::max<size_t>(1, maxNodes);
    m_sceneExplorerSnapshotTruncated = false;
    m_sceneExplorerSnapshotNodeBudget = remainingNodes;
    m_sceneExplorerRootSnapshot = BuildSceneExplorerSnapshot(root, remainingNodes, m_sceneExplorerSnapshotTruncated);
    m_sceneExplorerSnapshotAvailable = true;
    OverlayPendingSceneExplorerEdits();

    if (m_selectedSceneNodeStableId != 0
        && FindSceneExplorerSnapshotNode(m_sceneExplorerRootSnapshot, m_selectedSceneNodeStableId) == nullptr) {
        m_selectedSceneNodeStableId = 0;
    }
}

inline void Menu::QueueSceneNodePositionChange(uint64_t stableId, const DirectX::XMFLOAT3& position) {
    auto& pendingEdit = m_sceneExplorerPendingEdits[stableId];
    pendingEdit.hasPosition = true;
    pendingEdit.position = position;
    if (queueSceneNodePositionEdit) {
        queueSceneNodePositionEdit(stableId, position);
    }
}

inline void Menu::QueueSceneNodeUniformScaleChange(uint64_t stableId, float uniformScale) {
    auto& pendingEdit = m_sceneExplorerPendingEdits[stableId];
    pendingEdit.hasUniformScale = true;
    pendingEdit.uniformScale = uniformScale;
    if (queueSceneNodeUniformScaleEdit) {
        queueSceneNodeUniformScaleEdit(stableId, uniformScale);
    }
}

inline void Menu::DisplaySceneNode(const SceneExplorerNodeSnapshot& node, bool isOnlyChild) {
    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

    if (node.stableId == m_selectedSceneNodeStableId) {
        nodeFlags |= ImGuiTreeNodeFlags_Selected;
    }

    if (isOnlyChild) {
        nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    if (node.children.empty()) {
        nodeFlags |= ImGuiTreeNodeFlags_Leaf;
    }

    void* uniqueId = reinterpret_cast<void*>(static_cast<intptr_t>(node.stableId));
    if (ImGui::TreeNodeEx(uniqueId, nodeFlags, "%s", node.name.c_str())) {
        if (ImGui::IsItemClicked()) {
            m_selectedSceneNodeStableId = node.stableId;
        }

        if (node.isRenderable) {
            ImGui::Text("Meshes: %llu", static_cast<unsigned long long>(node.meshCount));
            ImGui::Text("Has Skinned: %s", node.skinned ? "Yes" : "No");
        }

        const bool childIsOnly = node.children.size() <= 1;
        for (const auto& child : node.children) {
            DisplaySceneNode(child, childIsOnly);
        }

        ImGui::TreePop();
    } else if (ImGui::IsItemClicked()) {
        m_selectedSceneNodeStableId = node.stableId;
    }
}

inline void Menu::DisplaySceneGraph() {
    const float lineHeight = std::max(1.0f, ImGui::GetTextLineHeightWithSpacing());
    const float availableHeight = std::max(0.0f, ImGui::GetContentRegionAvail().y);
    const size_t visibleRows = static_cast<size_t>(std::ceil(availableHeight / lineHeight));
    RefreshSceneExplorerSnapshot(std::max<size_t>(1, visibleRows + 8));

    if (!m_sceneExplorerSnapshotAvailable) {
        ImGui::TextDisabled("No scene snapshot available.");
        return;
    }

    if (m_sceneExplorerSnapshotTruncated) {
        ImGui::TextDisabled(
            "Scene graph limited to %llu visible nodes.",
            static_cast<unsigned long long>(m_sceneExplorerSnapshotNodeBudget));
    }

    DisplaySceneNode(m_sceneExplorerRootSnapshot, true);
}

inline void Menu::DisplaySelectedNode() {
    if (m_selectedSceneNodeStableId == 0 || !m_sceneExplorerSnapshotAvailable) {
        return;
    }

    auto* selectedNode = FindSceneExplorerSnapshotNode(m_sceneExplorerRootSnapshot, m_selectedSceneNodeStableId);
    if (!selectedNode) {
        m_selectedSceneNodeStableId = 0;
        return;
    }

    ImGui::Begin("Selected Node Transform", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("Position:");
    if (selectedNode->hasPosition) {
        DirectX::XMFLOAT3 pos = selectedNode->position;
        if (ImGui::InputFloat3("Position", &pos.x)) {
            selectedNode->position = pos;
            QueueSceneNodePositionChange(selectedNode->stableId, pos);
        }
    } else {
        ImGui::TextDisabled("Position unavailable.");
    }

    ImGui::Text("Scale:");
    if (selectedNode->hasScale) {
        float uniformScale = selectedNode->uniformScale;
        if (ImGui::InputFloat("Scale", &uniformScale)) {
            selectedNode->uniformScale = uniformScale;
            QueueSceneNodeUniformScaleChange(selectedNode->stableId, uniformScale);
        }
    } else {
        ImGui::TextDisabled("Scale unavailable.");
    }

    if (selectedNode->hasRotation) {
        ImGui::Text(
            "Rotation (quaternion): (%.3f, %.3f, %.3f, %.3f)",
            selectedNode->rotation.x,
            selectedNode->rotation.y,
            selectedNode->rotation.z,
            selectedNode->rotation.w);
    } else {
        ImGui::TextDisabled("Rotation unavailable.");
    }

    ImGui::End();
}

inline void Menu::TryFinalizeCLodCaptureStats(CLodWorkGraphCaptureState& captureState, uint64_t captureId, const char* captureLabel) {
    if (!captureState.captureStatsPending || captureState.captureStatsId != captureId) {
        return;
    }

    if (!captureState.captureHasPendingCounter || !captureState.captureHasPendingClusters) {
        return;
    }

    const uint32_t requestedCount = captureState.capturePendingVisibleCount;
    const uint32_t availableCount = static_cast<uint32_t>(captureState.capturePendingClusters.size());
    const uint32_t decodeCount = (std::min)(requestedCount, availableCount);

    std::unordered_map<uint32_t, uint32_t> viewHistogram;
    std::unordered_map<uint32_t, uint32_t> instanceHistogram;
    std::unordered_set<uint64_t> uniqueMeshlets;

    viewHistogram.reserve(16);
    instanceHistogram.reserve(512);
    uniqueMeshlets.reserve(decodeCount > 0 ? decodeCount : 1);

    for (uint32_t i = 0; i < decodeCount; ++i) {
        const VisibleCluster& cluster = captureState.capturePendingClusters[i];
        viewHistogram[cluster.viewID]++;
        instanceHistogram[cluster.instanceID]++;
        const uint64_t key = (static_cast<uint64_t>(cluster.instanceID) << 32ull) | (static_cast<uint64_t>(cluster.groupID) << 16ull) | static_cast<uint64_t>(cluster.localMeshletIndex);
        uniqueMeshlets.insert(key);
    }

    CLodCaptureStats stats{};
    stats.visibleClusterCount = decodeCount;
    stats.uniqueViews = static_cast<uint32_t>(viewHistogram.size());
    stats.uniqueInstances = static_cast<uint32_t>(instanceHistogram.size());
    stats.uniqueMeshlets = static_cast<uint32_t>(uniqueMeshlets.size());

    for (const auto& [_, count] : viewHistogram) {
        stats.maxClustersPerView = (std::max)(stats.maxClustersPerView, count);
    }
    for (const auto& [_, count] : instanceHistogram) {
        stats.maxClustersPerInstance = (std::max)(stats.maxClustersPerInstance, count);
    }

    if (stats.uniqueViews > 0) {
        stats.avgClustersPerView = static_cast<float>(decodeCount) / static_cast<float>(stats.uniqueViews);
    }
    if (stats.uniqueInstances > 0) {
        stats.avgClustersPerInstance = static_cast<float>(decodeCount) / static_cast<float>(stats.uniqueInstances);
    }
    if (decodeCount > 0) {
        stats.dominantViewPercent = 100.0f * static_cast<float>(stats.maxClustersPerView) / static_cast<float>(decodeCount);
        stats.dominantInstancePercent = 100.0f * static_cast<float>(stats.maxClustersPerInstance) / static_cast<float>(decodeCount);
    }

    captureState.captureStats = stats;
    captureState.captureStatsAvailable = true;
    captureState.captureStatsPending = false;

    spdlog::info(
        "{} stats capture: visible={}, views={}, instances={}, uniqueMeshlets={}, maxPerView={}, maxPerInstance={}",
        captureLabel,
        stats.visibleClusterCount,
        stats.uniqueViews,
        stats.uniqueInstances,
        stats.uniqueMeshlets,
        stats.maxClustersPerView,
        stats.maxClustersPerInstance);
}

inline void Menu::TryFinalizeCLodAlphaTelemetryCapture(uint64_t captureId) {
    if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
        return;
    }

    if (!m_clodAlphaTelemetryHasPendingNodeCount ||
        !m_clodAlphaTelemetryHasPendingOverflow ||
        !m_clodAlphaTelemetryHasPendingStats) {
        return;
    }

    m_clodAlphaNodeCount = m_clodAlphaTelemetryPendingNodeCount;
    m_clodAlphaOverflowCount = m_clodAlphaTelemetryPendingOverflow;
    m_clodAlphaStats = m_clodAlphaTelemetryPendingStats;
    m_clodAlphaTelemetryHasData = true;
    m_clodAlphaTelemetryCapturePending = false;
    m_clodAlphaTelemetryStatus = "Alpha capture completed.";

    spdlog::info(
        "CLod alpha telemetry: nodes={}, overflow={}, truncatedPixels={}, truncatedNodes={}, resolvedSamples={}, maxRaw={}, maxResolved={}",
        m_clodAlphaNodeCount,
        m_clodAlphaOverflowCount,
        m_clodAlphaStats.truncatedPixelCount,
        m_clodAlphaStats.truncatedNodeCount,
        m_clodAlphaStats.totalResolvedSamples,
        m_clodAlphaStats.maxRawNodeCount,
        m_clodAlphaStats.maxResolvedSamples);
}

inline void Menu::TryFinalizeCLodReyesTelemetryCapture(uint64_t captureId) {
    if (!m_clodReyesTelemetryCapturePending || m_clodReyesTelemetryCaptureId != captureId) {
        return;
    }

    if (!m_clodReyesTelemetryHasPendingPhase1 || !m_clodReyesTelemetryHasPendingPhase2) {
        return;
    }

    m_clodReyesTelemetryPhase1 = m_clodReyesTelemetryPendingPhase1;
    m_clodReyesTelemetryPhase2 = m_clodReyesTelemetryPendingPhase2;
    m_clodReyesTelemetryHasData = true;
    m_clodReyesTelemetryCapturePending = false;
    m_clodReyesTelemetryCaptureCount++;
    m_clodReyesTelemetryStatus = "Reyes capture completed.";

    spdlog::info(
        "Reyes telemetry capture: phase1 input={} owned={} bypass={} totalDice={} splitDepth={} rasterizedPatches={} rasterizedMicros={} | phase2 input={} owned={} bypass={} totalDice={} splitDepth={} rasterizedPatches={} rasterizedMicros={}",
        m_clodReyesTelemetryPhase1.visibleClusterInputCount,
        m_clodReyesTelemetryPhase1.ownedClusterOutputCount,
        m_clodReyesTelemetryPhase1.fullClusterOutputCount,
        m_clodReyesTelemetryPhase1.immediateDiceQueueEntryCount + m_clodReyesTelemetryPhase1.finalDiceQueueEntryCount,
        m_clodReyesTelemetryPhase1.deepestSplitLevelReached,
        m_clodReyesTelemetryPhase1.patchRasterizedPatchCount,
        m_clodReyesTelemetryPhase1.patchRasterizedMicroTriangleCount,
        m_clodReyesTelemetryPhase2.visibleClusterInputCount,
        m_clodReyesTelemetryPhase2.ownedClusterOutputCount,
        m_clodReyesTelemetryPhase2.fullClusterOutputCount,
        m_clodReyesTelemetryPhase2.immediateDiceQueueEntryCount + m_clodReyesTelemetryPhase2.finalDiceQueueEntryCount,
        m_clodReyesTelemetryPhase2.deepestSplitLevelReached,
        m_clodReyesTelemetryPhase2.patchRasterizedPatchCount,
        m_clodReyesTelemetryPhase2.patchRasterizedMicroTriangleCount);
}

inline void Menu::TryFinalizeCLodVirtualShadowCapture(uint64_t captureId) {
    if (!m_shadowVirtualShadowTelemetry.capturePending || m_shadowVirtualShadowTelemetry.captureId != captureId) {
        return;
    }

    if (!m_shadowVirtualShadowTelemetry.captureHasPendingStats || !m_shadowVirtualShadowTelemetry.captureHasPendingRuntimeState) {
        return;
    }

    m_shadowVirtualShadowTelemetry.capturePending = false;
    m_shadowVirtualShadowTelemetry.hasData = true;
    m_shadowVirtualShadowTelemetry.captureCount++;
    m_shadowVirtualShadowTelemetry.status = "Capture completed.";
}

inline void Menu::DrawCLodTelemetryWindow() {
    ImGui::Begin("CLod Work Graph Telemetry", nullptr);

    Resource* clodTelemetryResource = nullptr;
    Resource* shadowClodTelemetryResource = nullptr;
    Resource* reyesTelemetryPhase1Resource = nullptr;
    Resource* reyesTelemetryPhase2Resource = nullptr;
    Resource* shadowReyesTelemetryPhase1Resource = nullptr;
    Resource* clodVisibleClustersResource = nullptr;
    Resource* clodVisibleCounterResource = nullptr;
    Resource* shadowClodVisibleClustersResource = nullptr;
    Resource* shadowClodVisibleCounterResource = nullptr;
    Resource* shadowVirtualShadowStatsResource = nullptr;
    Resource* shadowVirtualShadowRuntimeStateResource = nullptr;
    Resource* alphaNodeCounterResource = nullptr;
    Resource* alphaOverflowCounterResource = nullptr;
    Resource* alphaStatsResource = nullptr;
    {
        m_telemetryQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (clodTelemetryResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    clodTelemetryResource = resource.get();
                }
            }
            });

        m_shadowTelemetryQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowClodTelemetryResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowClodTelemetryResource = resource.get();
                }
            }
            });

        m_reyesTelemetryPhase1Query.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (reyesTelemetryPhase1Resource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    reyesTelemetryPhase1Resource = resource.get();
                }
            }
            });

        m_reyesTelemetryPhase2Query.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (reyesTelemetryPhase2Resource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    reyesTelemetryPhase2Resource = resource.get();
                }
            }
            });

        m_shadowReyesTelemetryPhase1Query.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowReyesTelemetryPhase1Resource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowReyesTelemetryPhase1Resource = resource.get();
                }
            }
            });

        m_visibleClustersQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (clodVisibleClustersResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    clodVisibleClustersResource = resource.get();
                }
            }
            });

        m_visibleCounterQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (clodVisibleCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    clodVisibleCounterResource = resource.get();
                }
            }
            });

        m_shadowVisibleClustersQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowClodVisibleClustersResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowClodVisibleClustersResource = resource.get();
                }
            }
            });

        m_shadowVisibleCounterQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowClodVisibleCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowClodVisibleCounterResource = resource.get();
                }
            }
            });

        m_shadowVirtualShadowStatsQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowVirtualShadowStatsResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowVirtualShadowStatsResource = resource.get();
                }
            }
            });

        m_shadowVirtualShadowRuntimeStateQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (shadowVirtualShadowRuntimeStateResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    shadowVirtualShadowRuntimeStateResource = resource.get();
                }
            }
            });

        m_alphaDeepVisibilityCounterQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (alphaNodeCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    alphaNodeCounterResource = resource.get();
                }
            }
            });

        m_alphaDeepVisibilityOverflowQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (alphaOverflowCounterResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    alphaOverflowCounterResource = resource.get();
                }
            }
            });

        m_alphaDeepVisibilityStatsQuery.each([&](flecs::entity, const Components::Resource& resourceComponent) {
            if (alphaStatsResource == nullptr) {
                if (auto resource = resourceComponent.resource.lock()) {
                    alphaStatsResource = resource.get();
                }
            }
            });
    }

    const bool captureStatsResourcesReady = (clodVisibleClustersResource != nullptr) && (clodVisibleCounterResource != nullptr);
    const bool shadowCaptureStatsResourcesReady = (shadowClodVisibleClustersResource != nullptr) && (shadowClodVisibleCounterResource != nullptr);
    const bool alphaCaptureResourcesReady =
        (alphaNodeCounterResource != nullptr) &&
        (alphaOverflowCounterResource != nullptr) &&
        (alphaStatsResource != nullptr);
    const bool reyesCaptureResourcesReady =
        (reyesTelemetryPhase1Resource != nullptr) &&
        (reyesTelemetryPhase2Resource != nullptr);
    const bool shadowReyesCaptureResourcesReady =
        (shadowReyesTelemetryPhase1Resource != nullptr);
    auto* readbackService = m_renderGraph ? m_renderGraph->GetReadbackService() : nullptr;
    const bool canCapture =
        (clodTelemetryResource != nullptr) &&
        (readbackService != nullptr) &&
        (!m_clodTelemetry.capturePending) &&
        (!m_clodTelemetry.captureStatsPending);
    const bool canCaptureShadow =
        (shadowClodTelemetryResource != nullptr) &&
        (readbackService != nullptr) &&
        (!m_shadowClodTelemetry.capturePending) &&
        (!m_shadowClodTelemetry.captureStatsPending);
    const bool canCaptureVirtualShadow =
        (shadowVirtualShadowStatsResource != nullptr) &&
        (shadowVirtualShadowRuntimeStateResource != nullptr) &&
        (readbackService != nullptr) &&
        (!m_shadowVirtualShadowTelemetry.capturePending);
    const bool canCaptureReyes = reyesCaptureResourcesReady && (readbackService != nullptr) && (!m_clodReyesTelemetryCapturePending);
    const bool canCaptureShadowReyes = shadowReyesCaptureResourcesReady && (readbackService != nullptr) && (!m_shadowClodReyesTelemetryCapturePending);
    const bool canCaptureAlpha = alphaCaptureResourcesReady && (readbackService != nullptr) && (!m_clodAlphaTelemetryCapturePending);

    if (!captureStatsResourcesReady) {
        ImGui::TextDisabled("Primary extended stats unavailable: visible cluster resources not found.");
    }
    if (!shadowCaptureStatsResourcesReady) {
        ImGui::TextDisabled("Shadow extended stats unavailable: visible cluster resources not found.");
    }

    if (!canCapture) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture CLod Primary Metrics")) {
        m_clodTelemetry.capturePending = true;
        m_clodTelemetry.status = "Capture requested.";

        const bool requestCaptureStats = captureStatsResourcesReady;
        if (requestCaptureStats) {
            m_clodTelemetry.captureStatsPending = true;
            m_clodTelemetry.captureStatsId++;
            m_clodTelemetry.captureHasPendingCounter = false;
            m_clodTelemetry.captureHasPendingClusters = false;
            m_clodTelemetry.capturePendingVisibleCount = 0;
            m_clodTelemetry.capturePendingClusters.clear();
        }

        if (readbackService) {
            readbackService->RequestReadbackCapture(
                "CLodOpaque::RasterizeClustersPass2",
                clodTelemetryResource,
                RangeSpec{},
                [this](ReadbackCaptureResult&& result) {
                m_clodTelemetry.capturePending = false;

                constexpr size_t telemetryBytes = sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    m_clodTelemetry.status = "Capture failed: telemetry payload too small.";
                    spdlog::warn("CLod telemetry capture payload too small ({} bytes).", result.data.size());
                    return;
                }

                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);

                m_clodTelemetry.counters = decoded;
                m_clodTelemetry.hasData = true;
                m_clodTelemetry.captureCount++;
                m_clodTelemetry.status = "Capture completed.";

                auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                    return decoded.counters[static_cast<size_t>(idx)];
                    };

                const uint32_t objectThreads = counter(CLodWorkGraphCounterIndex::ObjectCullThreads);
                const uint32_t objectActive = counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads);
                const uint32_t traverseThreads = counter(CLodWorkGraphCounterIndex::TraverseNodesThreads);
                const uint32_t traverseActive = counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads);
                const uint32_t clusterThreads = counter(CLodWorkGraphCounterIndex::ClusterCullThreads);
                const uint32_t clusterActive = counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads);
                const uint32_t visibleWrites = counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites);
                const uint32_t bucketDispatchRecords = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
                const uint32_t denseExpansionBuckets = counter(CLodWorkGraphCounterIndex::ClusterCullDenseExpansionBuckets);
                const uint32_t denseClustersDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullDenseClustersDispatched);
                const uint32_t replayNodeInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords);
                const uint32_t replayMeshletInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords);
                const uint32_t voxelLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords);
                const uint32_t voxelRejected = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords);
                const uint32_t voxelHits = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits);
                const uint32_t voxelMisses = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses);
                const uint32_t voxelRasterWork = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords);
                const uint32_t voxelRasterDropped = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped);
                const uint32_t sortHistInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
                const uint32_t sortHistVoxels = counter(CLodWorkGraphCounterIndex::RasterSortHistogramVoxelSkipped);
                const uint32_t sortHistTriangles = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors);
                const uint32_t sortCompactInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
                const uint32_t sortCompactVoxels = counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped);
                const uint32_t sortCompactTriangles = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
                const uint32_t rasterGroups = counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
                const uint32_t rasterInRange = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
                const uint32_t rasterInitFailed = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
                const uint32_t rasterOutputTris = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
                const char* clusterDispatchMode = (denseExpansionBuckets > 0u || denseClustersDispatched > 0u)
                    ? ((bucketDispatchRecords > 0u) ? "mixed" : "dense")
                    : "bucketed";

                spdlog::info(
                    "CLod WG telemetry: ObjectCull {}/{} active, Traverse {}/{} active-child, voxel(leaves={}, rejected={}, descHit={}, descMiss={}, rasterWork={}, rasterDrop={}), ClusterCull[{}] {}/{} in-range, visible writes {}, dispatch(bucket={}, denseBuckets={}, denseClusters={}), replay(node={}, meshlet={}), sort(hist input={}, hist voxels={}, hist tris={}, compact input={}, compact voxels={}, compact tris={}), raster(groups={}, inRange={}, initFail={}, outTris={})",
                    objectActive,
                    objectThreads,
                    traverseActive,
                    traverseThreads,
                    voxelLeaves,
                    voxelRejected,
                    voxelHits,
                    voxelMisses,
                    voxelRasterWork,
                    voxelRasterDropped,
                    clusterDispatchMode,
                    clusterActive,
                    clusterThreads,
                    visibleWrites,
                    bucketDispatchRecords,
                    denseExpansionBuckets,
                    denseClustersDispatched,
                    replayNodeInput,
                    replayMeshletInput,
                    sortHistInputs,
                    sortHistVoxels,
                    sortHistTriangles,
                    sortCompactInputs,
                    sortCompactVoxels,
                    sortCompactTriangles,
                    rasterGroups,
                    rasterInRange,
                    rasterInitFailed,
                    rasterOutputTris);
                });

        }

        if (requestCaptureStats) {
            const uint64_t captureId = m_clodTelemetry.captureStatsId;

            if (readbackService) {
                readbackService->RequestReadbackCapture(
                    "CLodOpaque::HierarchicalCullingPass2",
                    clodVisibleCounterResource,
                    RangeSpec{},
                    [this, captureId](ReadbackCaptureResult&& result) {
                    if (!m_clodTelemetry.captureStatsPending || m_clodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    if (result.data.size() < sizeof(uint32_t)) {
                        m_clodTelemetry.status = "Capture failed: visible counter payload too small.";
                        m_clodTelemetry.captureStatsPending = false;
                        return;
                    }

                    std::memcpy(&m_clodTelemetry.capturePendingVisibleCount, result.data.data(), sizeof(uint32_t));
                    m_clodTelemetry.captureHasPendingCounter = true;
                    TryFinalizeCLodCaptureStats(m_clodTelemetry, captureId, "CLod primary WG");
                    });

                readbackService->RequestReadbackCapture(
                    "CLodOpaque::HierarchicalCullingPass2",
                    clodVisibleClustersResource,
                    RangeSpec{},
                    [this, captureId](ReadbackCaptureResult&& result) {
                    if (!m_clodTelemetry.captureStatsPending || m_clodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    const size_t clusterBytes = PackedVisibleClusterStrideBytes;
                    const size_t count = result.data.size() / clusterBytes;
                    m_clodTelemetry.capturePendingClusters.resize(count);
                    if (count > 0) {
                        const std::byte* rawClusters = result.data.data();
                        for (size_t i = 0; i < count; ++i) {
                            m_clodTelemetry.capturePendingClusters[i] = DecodePackedVisibleCluster(rawClusters + i * clusterBytes);
                        }
                    }

                    m_clodTelemetry.captureHasPendingClusters = true;
                    TryFinalizeCLodCaptureStats(m_clodTelemetry, captureId, "CLod primary WG");
                    });
            }

            m_clodTelemetry.status = "Capture requested (extended stats).";
        }
    }
    if (!canCapture) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Primary Status: %s", m_clodTelemetry.status.c_str());

    if (!canCaptureShadow) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture CLod Shadow Metrics")) {
        m_shadowClodTelemetry.capturePending = true;
        m_shadowClodTelemetry.status = "Capture requested.";

        const bool requestCaptureStats = shadowCaptureStatsResourcesReady;
        if (requestCaptureStats) {
            m_shadowClodTelemetry.captureStatsPending = true;
            m_shadowClodTelemetry.captureStatsId++;
            m_shadowClodTelemetry.captureHasPendingCounter = false;
            m_shadowClodTelemetry.captureHasPendingClusters = false;
            m_shadowClodTelemetry.capturePendingVisibleCount = 0;
            m_shadowClodTelemetry.capturePendingClusters.clear();
        }

        if (readbackService) {
            readbackService->RequestReadbackCapture(
                "CLodShadow::RasterizeClustersPass1",
                shadowClodTelemetryResource,
                RangeSpec{},
                [this](ReadbackCaptureResult&& result) {
                m_shadowClodTelemetry.capturePending = false;

                constexpr size_t telemetryBytes = sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    m_shadowClodTelemetry.status = "Capture failed: telemetry payload too small.";
                    spdlog::warn("CLod shadow telemetry capture payload too small ({} bytes).", result.data.size());
                    return;
                }

                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);

                m_shadowClodTelemetry.counters = decoded;
                m_shadowClodTelemetry.hasData = true;
                m_shadowClodTelemetry.captureCount++;
                m_shadowClodTelemetry.status = "Capture completed.";

                auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                    return decoded.counters[static_cast<size_t>(idx)];
                    };

                const uint32_t objectThreads = counter(CLodWorkGraphCounterIndex::ObjectCullThreads);
                const uint32_t objectActive = counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads);
                const uint32_t traverseThreads = counter(CLodWorkGraphCounterIndex::TraverseNodesThreads);
                const uint32_t traverseActive = counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads);
                const uint32_t clusterThreads = counter(CLodWorkGraphCounterIndex::ClusterCullThreads);
                const uint32_t clusterActive = counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads);
                const uint32_t visibleWrites = counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites);
                const uint32_t bucketDispatchRecords = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
                const uint32_t denseExpansionBuckets = counter(CLodWorkGraphCounterIndex::ClusterCullDenseExpansionBuckets);
                const uint32_t denseClustersDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullDenseClustersDispatched);
                const uint32_t replayNodeInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords);
                const uint32_t replayMeshletInput = counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords);
                const uint32_t voxelLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords);
                const uint32_t voxelRejected = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords);
                const uint32_t voxelHits = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits);
                const uint32_t voxelMisses = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses);
                const uint32_t voxelRasterWork = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords);
                const uint32_t voxelRasterDropped = counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped);
                const uint32_t sortHistInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
                const uint32_t sortHistVoxels = counter(CLodWorkGraphCounterIndex::RasterSortHistogramVoxelSkipped);
                const uint32_t sortHistTriangles = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors);
                const uint32_t sortCompactInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
                const uint32_t sortCompactVoxels = counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped);
                const uint32_t sortCompactTriangles = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
                const uint32_t rasterGroups = counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
                const uint32_t rasterInRange = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
                const uint32_t rasterInitFailed = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
                const uint32_t rasterOutputTris = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
                const char* clusterDispatchMode = (denseExpansionBuckets > 0u || denseClustersDispatched > 0u)
                    ? ((bucketDispatchRecords > 0u) ? "mixed" : "dense")
                    : "bucketed";

                spdlog::info(
                    "CLod shadow WG telemetry: ObjectCull {}/{} active, Traverse {}/{} active-child, voxel(leaves={}, rejected={}, descHit={}, descMiss={}, rasterWork={}, rasterDrop={}), ClusterCull[{}] {}/{} in-range, visible writes {}, dispatch(bucket={}, denseBuckets={}, denseClusters={}), replay(node={}, meshlet={}), sort(hist input={}, hist voxels={}, hist tris={}, compact input={}, compact voxels={}, compact tris={}), raster(groups={}, inRange={}, initFail={}, outTris={})",
                    objectActive,
                    objectThreads,
                    traverseActive,
                    traverseThreads,
                    voxelLeaves,
                    voxelRejected,
                    voxelHits,
                    voxelMisses,
                    voxelRasterWork,
                    voxelRasterDropped,
                    clusterDispatchMode,
                    clusterActive,
                    clusterThreads,
                    visibleWrites,
                    bucketDispatchRecords,
                    denseExpansionBuckets,
                    denseClustersDispatched,
                    replayNodeInput,
                    replayMeshletInput,
                    sortHistInputs,
                    sortHistVoxels,
                    sortHistTriangles,
                    sortCompactInputs,
                    sortCompactVoxels,
                    sortCompactTriangles,
                    rasterGroups,
                    rasterInRange,
                    rasterInitFailed,
                    rasterOutputTris);
                });

        }

        if (requestCaptureStats) {
            const uint64_t captureId = m_shadowClodTelemetry.captureStatsId;

            if (readbackService) {
                readbackService->RequestReadbackCapture(
                    "CLodShadow::HierarchicalCullingPass1",
                    shadowClodVisibleCounterResource,
                    RangeSpec{},
                    [this, captureId](ReadbackCaptureResult&& result) {
                    if (!m_shadowClodTelemetry.captureStatsPending || m_shadowClodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    if (result.data.size() < sizeof(uint32_t)) {
                        m_shadowClodTelemetry.status = "Capture failed: visible counter payload too small.";
                        m_shadowClodTelemetry.captureStatsPending = false;
                        return;
                    }

                    std::memcpy(&m_shadowClodTelemetry.capturePendingVisibleCount, result.data.data(), sizeof(uint32_t));
                    m_shadowClodTelemetry.captureHasPendingCounter = true;
                    TryFinalizeCLodCaptureStats(m_shadowClodTelemetry, captureId, "CLod shadow WG");
                    });

                readbackService->RequestReadbackCapture(
                    "CLodShadow::HierarchicalCullingPass1",
                    shadowClodVisibleClustersResource,
                    RangeSpec{},
                    [this, captureId](ReadbackCaptureResult&& result) {
                    if (!m_shadowClodTelemetry.captureStatsPending || m_shadowClodTelemetry.captureStatsId != captureId) {
                        return;
                    }

                    const size_t clusterBytes = PackedVisibleClusterStrideBytes;
                    const size_t count = result.data.size() / clusterBytes;
                    m_shadowClodTelemetry.capturePendingClusters.resize(count);
                    if (count > 0) {
                        const std::byte* rawClusters = result.data.data();
                        for (size_t i = 0; i < count; ++i) {
                            m_shadowClodTelemetry.capturePendingClusters[i] = DecodePackedVisibleCluster(rawClusters + i * clusterBytes);
                        }
                    }

                    m_shadowClodTelemetry.captureHasPendingClusters = true;
                    TryFinalizeCLodCaptureStats(m_shadowClodTelemetry, captureId, "CLod shadow WG");
                    });
            }

            m_shadowClodTelemetry.status = "Capture requested (extended stats).";
        }
    }
    if (!canCaptureShadow) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Shadow Status: %s", m_shadowClodTelemetry.status.c_str());

    if (!shadowVirtualShadowStatsResource || !shadowVirtualShadowRuntimeStateResource) {
        ImGui::TextDisabled("VSM stats unavailable: required stats/runtime-state resources not found.");
    }

    if (!canCaptureVirtualShadow) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture Virtual Shadow Metrics")) {
        m_shadowVirtualShadowTelemetry.capturePending = true;
        m_shadowVirtualShadowTelemetry.captureHasPendingStats = false;
        m_shadowVirtualShadowTelemetry.captureHasPendingRuntimeState = false;
        m_shadowVirtualShadowTelemetry.captureId++;
        m_shadowVirtualShadowTelemetry.status = "Capture requested.";

        const uint64_t captureId = m_shadowVirtualShadowTelemetry.captureId;

        readbackService->RequestReadbackCapture(
            "CLodShadow::VirtualShadowGatherStatsPass",
            shadowVirtualShadowStatsResource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_shadowVirtualShadowTelemetry.capturePending || m_shadowVirtualShadowTelemetry.captureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodVirtualShadowStats)) {
                    m_shadowVirtualShadowTelemetry.capturePending = false;
                    m_shadowVirtualShadowTelemetry.status = "Capture failed: VSM stats payload too small.";
                    return;
                }

                std::memcpy(&m_shadowVirtualShadowTelemetry.stats, result.data.data(), sizeof(CLodVirtualShadowStats));
                m_shadowVirtualShadowTelemetry.captureHasPendingStats = true;
                TryFinalizeCLodVirtualShadowCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodShadow::VirtualShadowSetupPass",
            shadowVirtualShadowRuntimeStateResource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_shadowVirtualShadowTelemetry.capturePending || m_shadowVirtualShadowTelemetry.captureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodVirtualShadowRuntimeState)) {
                    m_shadowVirtualShadowTelemetry.capturePending = false;
                    m_shadowVirtualShadowTelemetry.status = "Capture failed: VSM runtime-state payload too small.";
                    return;
                }

                std::memcpy(&m_shadowVirtualShadowTelemetry.runtimeState, result.data.data(), sizeof(CLodVirtualShadowRuntimeState));
                m_shadowVirtualShadowTelemetry.captureHasPendingRuntimeState = true;
                TryFinalizeCLodVirtualShadowCapture(captureId);
            });
    }
    if (!canCaptureVirtualShadow) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("VSM Status: %s", m_shadowVirtualShadowTelemetry.status.c_str());

    if (!reyesCaptureResourcesReady) {
        ImGui::TextDisabled("Reyes metrics unavailable: phase telemetry resources not found.");
    }

    if (!canCaptureReyes) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture Reyes Metrics")) {
        m_clodReyesTelemetryCapturePending = true;
        m_clodReyesTelemetryCaptureId++;
        m_clodReyesTelemetryHasPendingPhase1 = false;
        m_clodReyesTelemetryHasPendingPhase2 = false;
        m_clodReyesTelemetryPendingPhase1 = {};
        m_clodReyesTelemetryPendingPhase2 = {};
        m_clodReyesTelemetryStatus = "Reyes capture requested.";

        const uint64_t captureId = m_clodReyesTelemetryCaptureId;
        readbackService->RequestReadbackCapture(
            "CLodOpaque::ReyesPatchRasterPass1",
            reyesTelemetryPhase1Resource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_clodReyesTelemetryCapturePending || m_clodReyesTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodReyesTelemetry)) {
                    m_clodReyesTelemetryStatus = "Reyes capture failed: phase 1 payload too small.";
                    m_clodReyesTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodReyesTelemetryPendingPhase1, result.data.data(), sizeof(CLodReyesTelemetry));
                m_clodReyesTelemetryHasPendingPhase1 = true;
                TryFinalizeCLodReyesTelemetryCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodOpaque::ReyesPatchRasterPass2",
            reyesTelemetryPhase2Resource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_clodReyesTelemetryCapturePending || m_clodReyesTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodReyesTelemetry)) {
                    m_clodReyesTelemetryStatus = "Reyes capture failed: phase 2 payload too small.";
                    m_clodReyesTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodReyesTelemetryPendingPhase2, result.data.data(), sizeof(CLodReyesTelemetry));
                m_clodReyesTelemetryHasPendingPhase2 = true;
                TryFinalizeCLodReyesTelemetryCapture(captureId);
            });
    }
    if (!canCaptureReyes) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Reyes Status: %s", m_clodReyesTelemetryStatus.c_str());

    if (!shadowReyesCaptureResourcesReady) {
        ImGui::TextDisabled("Shadow Reyes metrics unavailable: phase telemetry resource not found.");
    }

    if (!canCaptureShadowReyes) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture Shadow Reyes Metrics")) {
        m_shadowClodReyesTelemetryCapturePending = true;
        m_shadowClodReyesTelemetryCaptureId++;
        m_shadowClodReyesTelemetryPhase1 = {};
        m_shadowClodReyesTelemetryStatus = "Shadow Reyes capture requested.";

        const uint64_t captureId = m_shadowClodReyesTelemetryCaptureId;
        readbackService->RequestReadbackCapture(
            "CLodShadow::VirtualShadowClearDirtyBitsPass",
            shadowReyesTelemetryPhase1Resource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_shadowClodReyesTelemetryCapturePending || m_shadowClodReyesTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodReyesTelemetry)) {
                    m_shadowClodReyesTelemetryStatus = "Shadow Reyes capture failed: phase 1 payload too small.";
                    m_shadowClodReyesTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_shadowClodReyesTelemetryPhase1, result.data.data(), sizeof(CLodReyesTelemetry));
                m_shadowClodReyesTelemetryHasData = true;
                m_shadowClodReyesTelemetryCapturePending = false;
                m_shadowClodReyesTelemetryCaptureCount++;
                m_shadowClodReyesTelemetryStatus = "Shadow Reyes capture completed.";

                spdlog::info(
                    "Shadow Reyes telemetry capture: phase1 input={} owned={} bypass={} totalDice={} splitDepth={} rasterizedPatches={} rasterizedMicros={}",
                    m_shadowClodReyesTelemetryPhase1.visibleClusterInputCount,
                    m_shadowClodReyesTelemetryPhase1.ownedClusterOutputCount,
                    m_shadowClodReyesTelemetryPhase1.fullClusterOutputCount,
                    m_shadowClodReyesTelemetryPhase1.immediateDiceQueueEntryCount + m_shadowClodReyesTelemetryPhase1.finalDiceQueueEntryCount,
                    m_shadowClodReyesTelemetryPhase1.deepestSplitLevelReached,
                    m_shadowClodReyesTelemetryPhase1.patchRasterizedPatchCount,
                    m_shadowClodReyesTelemetryPhase1.patchRasterizedMicroTriangleCount);
            });
    }
    if (!canCaptureShadowReyes) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Shadow Reyes Status: %s", m_shadowClodReyesTelemetryStatus.c_str());

    if (!alphaCaptureResourcesReady) {
        ImGui::TextDisabled("Alpha deep-visibility metrics unavailable: required resources not found.");
    }

    if (!canCaptureAlpha) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Capture CLod Alpha Metrics")) {
        m_clodAlphaTelemetryCapturePending = true;
        m_clodAlphaTelemetryCaptureId++;
        m_clodAlphaTelemetryHasPendingNodeCount = false;
        m_clodAlphaTelemetryHasPendingOverflow = false;
        m_clodAlphaTelemetryHasPendingStats = false;
        m_clodAlphaTelemetryPendingNodeCount = 0;
        m_clodAlphaTelemetryPendingOverflow = 0;
        m_clodAlphaTelemetryPendingStats = {};
        m_clodAlphaTelemetryStatus = "Alpha capture requested.";

        const uint64_t captureId = m_clodAlphaTelemetryCaptureId;
        readbackService->RequestReadbackCapture(
            "CLodAlpha::DeepVisibilityResolvePass",
            alphaNodeCounterResource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(uint32_t)) {
                    m_clodAlphaTelemetryStatus = "Alpha capture failed: node counter payload too small.";
                    m_clodAlphaTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodAlphaTelemetryPendingNodeCount, result.data.data(), sizeof(uint32_t));
                m_clodAlphaTelemetryHasPendingNodeCount = true;
                TryFinalizeCLodAlphaTelemetryCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodAlpha::DeepVisibilityResolvePass",
            alphaOverflowCounterResource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(uint32_t)) {
                    m_clodAlphaTelemetryStatus = "Alpha capture failed: overflow payload too small.";
                    m_clodAlphaTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodAlphaTelemetryPendingOverflow, result.data.data(), sizeof(uint32_t));
                m_clodAlphaTelemetryHasPendingOverflow = true;
                TryFinalizeCLodAlphaTelemetryCapture(captureId);
            });

        readbackService->RequestReadbackCapture(
            "CLodAlpha::DeepVisibilityResolvePass",
            alphaStatsResource,
            RangeSpec{},
            [this, captureId](ReadbackCaptureResult&& result) {
                if (!m_clodAlphaTelemetryCapturePending || m_clodAlphaTelemetryCaptureId != captureId) {
                    return;
                }

                if (result.data.size() < sizeof(CLodDeepVisibilityStats)) {
                    m_clodAlphaTelemetryStatus = "Alpha capture failed: stats payload too small.";
                    m_clodAlphaTelemetryCapturePending = false;
                    return;
                }

                std::memcpy(&m_clodAlphaTelemetryPendingStats, result.data.data(), sizeof(CLodDeepVisibilityStats));
                m_clodAlphaTelemetryHasPendingStats = true;
                TryFinalizeCLodAlphaTelemetryCapture(captureId);
            });
    }
    if (!canCaptureAlpha) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Text("Alpha Status: %s", m_clodAlphaTelemetryStatus.c_str());

    {
        CLodStreamingOperationStats latestOps{};
        if (TryReadCLodStreamingOperationStats(m_clodStreamingOpsLastSequence, latestOps)) {
            m_clodStreamingOpsLatest = latestOps;
            m_clodStreamingOpsHistory.push_back({ std::chrono::steady_clock::now(), latestOps });
        }

        CLodDirectionalShadowDebugSnapshot latestShadowDebug{};
        if (TryReadCLodDirectionalShadowDebugSnapshot(m_directionalShadowDebugLastSequence, latestShadowDebug)) {
            m_directionalShadowDebugLatest = latestShadowDebug;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto horizon = std::chrono::seconds(5);
        m_clodStreamingOpsHistory.erase(
            std::remove_if(
                m_clodStreamingOpsHistory.begin(),
                m_clodStreamingOpsHistory.end(),
                [&](const CLodStreamingOpsHistorySample& sample) {
                    return (now - sample.timestamp) > horizon;
                }),
            m_clodStreamingOpsHistory.end());

        CLodStreamingOperationStats max5s{};
        for (const auto& sample : m_clodStreamingOpsHistory) {
            max5s.loadRequested = std::max(max5s.loadRequested, sample.stats.loadRequested);
            max5s.loadUnique = std::max(max5s.loadUnique, sample.stats.loadUnique);
            max5s.loadApplied = std::max(max5s.loadApplied, sample.stats.loadApplied);
            max5s.loadFailed = std::max(max5s.loadFailed, sample.stats.loadFailed);

            max5s.unloadRequested = std::max(max5s.unloadRequested, sample.stats.unloadRequested);
            max5s.unloadUnique = std::max(max5s.unloadUnique, sample.stats.unloadUnique);
            max5s.unloadApplied = std::max(max5s.unloadApplied, sample.stats.unloadApplied);
            max5s.unloadFailed = std::max(max5s.unloadFailed, sample.stats.unloadFailed);

            max5s.residentGroups = std::max(max5s.residentGroups, sample.stats.residentGroups);
            max5s.residentAllocations = std::max(max5s.residentAllocations, sample.stats.residentAllocations);
            max5s.queuedRequests = std::max(max5s.queuedRequests, sample.stats.queuedRequests);
            max5s.completedResults = std::max(max5s.completedResults, sample.stats.completedResults);
            max5s.residentAllocationBytes = std::max(max5s.residentAllocationBytes, sample.stats.residentAllocationBytes);
            max5s.completedResultBytes = std::max(max5s.completedResultBytes, sample.stats.completedResultBytes);
            max5s.streamedBytesThisFrame = std::max(max5s.streamedBytesThisFrame, sample.stats.streamedBytesThisFrame);
        }

        auto formatBytes = [](uint64_t bytes) {
            const double kib = 1024.0;
            const double mib = kib * 1024.0;
            const double gib = mib * 1024.0;

            if (bytes >= static_cast<uint64_t>(gib)) {
                return std::format("{:.2f} GiB", static_cast<double>(bytes) / gib);
            }
            if (bytes >= static_cast<uint64_t>(mib)) {
                return std::format("{:.2f} MiB", static_cast<double>(bytes) / mib);
            }
            if (bytes >= static_cast<uint64_t>(kib)) {
                return std::format("{:.2f} KiB", static_cast<double>(bytes) / kib);
            }

            return std::format("{} B", bytes);
        };

        ImGui::Separator();
        ImGui::TextUnformatted("Streaming operations (per frame)");
        ImGui::Text("Load: requested=%u unique=%u applied=%u failed=%u",
            m_clodStreamingOpsLatest.loadRequested,
            m_clodStreamingOpsLatest.loadUnique,
            m_clodStreamingOpsLatest.loadApplied,
            m_clodStreamingOpsLatest.loadFailed);
        ImGui::Text("Unload: requested=%u unique=%u applied=%u failed=%u",
            m_clodStreamingOpsLatest.unloadRequested,
            m_clodStreamingOpsLatest.unloadUnique,
            m_clodStreamingOpsLatest.unloadApplied,
            m_clodStreamingOpsLatest.unloadFailed);
        ImGui::Text("Resident: groups=%u allocations=%u bytes=%s",
            m_clodStreamingOpsLatest.residentGroups,
            m_clodStreamingOpsLatest.residentAllocations,
            formatBytes(m_clodStreamingOpsLatest.residentAllocationBytes).c_str());
        ImGui::Text("Backlog: queued=%u completed=%u completedBytes=%s",
            m_clodStreamingOpsLatest.queuedRequests,
            m_clodStreamingOpsLatest.completedResults,
            formatBytes(m_clodStreamingOpsLatest.completedResultBytes).c_str());
        {
            const double kbPerFrame = static_cast<double>(m_clodStreamingOpsLatest.streamedBytesThisFrame) / 1024.0;
            const float fps = ImGui::GetIO().Framerate;
            const double gbPerSec = (fps > 0.0f)
                ? (static_cast<double>(m_clodStreamingOpsLatest.streamedBytesThisFrame) * static_cast<double>(fps)) / (1024.0 * 1024.0 * 1024.0)
                : 0.0;
            ImGui::Text("Throughput: %.1f KB/frame  %.3f GB/s", kbPerFrame, gbPerSec);
        }

        ImGui::TextUnformatted("Max in last 5 seconds");
        ImGui::Text("Load max: requested=%u unique=%u applied=%u failed=%u",
            max5s.loadRequested,
            max5s.loadUnique,
            max5s.loadApplied,
            max5s.loadFailed);
        ImGui::Text("Unload max: requested=%u unique=%u applied=%u failed=%u",
            max5s.unloadRequested,
            max5s.unloadUnique,
            max5s.unloadApplied,
            max5s.unloadFailed);
        ImGui::Text("Resident max: groups=%u allocations=%u bytes=%s",
            max5s.residentGroups,
            max5s.residentAllocations,
            formatBytes(max5s.residentAllocationBytes).c_str());
        ImGui::Text("Backlog max: queued=%u completed=%u completedBytes=%s",
            max5s.queuedRequests,
            max5s.completedResults,
            formatBytes(max5s.completedResultBytes).c_str());
        {
            const double kbPerFrame = static_cast<double>(max5s.streamedBytesThisFrame) / 1024.0;
            const float fps = ImGui::GetIO().Framerate;
            const double gbPerSec = (fps > 0.0f)
                ? (static_cast<double>(max5s.streamedBytesThisFrame) * static_cast<double>(fps)) / (1024.0 * 1024.0 * 1024.0)
                : 0.0;
            ImGui::Text("Throughput max: %.1f KB/frame  %.3f GB/s", kbPerFrame, gbPerSec);
        }
    }

    const auto drawWorkGraphCaptureSection = [&](const char* title, const CLodWorkGraphCaptureState& captureState) {
        ImGui::Separator();
        ImGui::TextUnformatted(title);

        if (captureState.capturePending) {
            ImGui::Text("Telemetry capture status: pending...");
        }
        else if (!captureState.hasData) {
            ImGui::TextDisabled("No telemetry capture results yet.");
        }
        else {
            auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                return captureState.counters.counters[static_cast<size_t>(idx)];
            };

            auto drawUtilizationRow = [&](const char* label, uint32_t active, uint32_t total) {
                const float efficiency = (total > 0)
                    ? (100.0f * static_cast<float>(active) / static_cast<float>(total))
                    : 0.0f;
                ImGui::Text("%s: %u / %u (%.1f%%)", label, active, total, efficiency);
            };

            ImGui::Text("Telemetry captures: %llu", static_cast<unsigned long long>(captureState.captureCount));
            const uint32_t sourceGroupMismatchCount = counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch);
            if (sourceGroupMismatchCount != 0u) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.15f, 0.10f, 1.0f),
                    "Source group mismatches: %u",
                    sourceGroupMismatchCount);
            }
            else {
                ImGui::Text("Source group mismatches: %u", sourceGroupMismatchCount);
            }
            const uint32_t zeroPageSlabCount = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedZeroPageSlab);
            if (zeroPageSlabCount != 0u) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.15f, 0.10f, 1.0f),
                    "Zero page slab: %u",
                    zeroPageSlabCount);
            }
            else {
                ImGui::Text("Zero page slab: %u", zeroPageSlabCount);
            }
            drawUtilizationRow(
                "ObjectCull active draw threads",
                counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads),
                counter(CLodWorkGraphCounterIndex::ObjectCullThreads));
            drawUtilizationRow(
                "ObjectCull visible threads",
                counter(CLodWorkGraphCounterIndex::ObjectCullVisibleThreads),
                counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads));

            const uint32_t objectCullRejectedFrustum = counter(CLodWorkGraphCounterIndex::ObjectCullRejectedFrustum);
            const uint32_t objectCullInvalidBounds = counter(CLodWorkGraphCounterIndex::ObjectCullInvalidBounds);
            const uint32_t objectCullRejectedTotal = objectCullRejectedFrustum + objectCullInvalidBounds;
            ImGui::Text("ObjectCull rejected: %u", objectCullRejectedTotal);
            if (objectCullRejectedTotal > 0u) {
                auto rejectionRow = [](const char* label, uint32_t count, uint32_t total) {
                    const float pct = (total > 0)
                        ? (100.0f * static_cast<float>(count) / static_cast<float>(total))
                        : 0.0f;
                    ImGui::Text("  %s: %u (%.1f%%)", label, count, pct);
                };
                rejectionRow("Invalid bounds", objectCullInvalidBounds, objectCullRejectedTotal);
                rejectionRow("Frustum reject", objectCullRejectedFrustum, objectCullRejectedTotal);
                rejectionRow("Left plane", counter(CLodWorkGraphCounterIndex::ObjectCullRejectedPlaneLeft), objectCullRejectedFrustum);
                rejectionRow("Right plane", counter(CLodWorkGraphCounterIndex::ObjectCullRejectedPlaneRight), objectCullRejectedFrustum);
                rejectionRow("Bottom plane", counter(CLodWorkGraphCounterIndex::ObjectCullRejectedPlaneBottom), objectCullRejectedFrustum);
                rejectionRow("Top plane", counter(CLodWorkGraphCounterIndex::ObjectCullRejectedPlaneTop), objectCullRejectedFrustum);
                rejectionRow("Near plane", counter(CLodWorkGraphCounterIndex::ObjectCullRejectedPlaneNear), objectCullRejectedFrustum);
                rejectionRow("Far plane", counter(CLodWorkGraphCounterIndex::ObjectCullRejectedPlaneFar), objectCullRejectedFrustum);
            }
            drawUtilizationRow(
                "TraverseNodes active child threads",
                counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads),
                counter(CLodWorkGraphCounterIndex::TraverseNodesThreads));
            drawUtilizationRow(
                "ClusterCull in-range threads",
                counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads),
                counter(CLodWorkGraphCounterIndex::ClusterCullThreads));

            const uint32_t bucketDispatchRecords = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
            const uint32_t phase2Records = counter(CLodWorkGraphCounterIndex::ClusterCullDenseExpansionBuckets);
            const uint32_t denseClustersDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullDenseClustersDispatched);
            const bool denseDispatchActive = (phase2Records > 0u || denseClustersDispatched > 0u);
            const char* clusterDispatchMode = denseDispatchActive
                ? ((bucketDispatchRecords > 0u) ? "mixed" : "phase2 records")
                : "bucketed";
            ImGui::Text(
                "ClusterCull dispatch mode: %s | bucket records=%u | phase2 records=%u | phase2 clusters=%u",
                clusterDispatchMode,
                bucketDispatchRecords,
                phase2Records,
                denseClustersDispatched);

            const uint32_t clusterActiveLanes = counter(CLodWorkGraphCounterIndex::ClusterCullActiveLanes);
            const uint32_t clusterSurvivingLanes = counter(CLodWorkGraphCounterIndex::ClusterCullSurvivingLanes);
            drawUtilizationRow("ClusterCull surviving lanes", clusterSurvivingLanes, clusterActiveLanes);

            ImGui::Text("Traverse node records: internal=%u leaf=%u culled=%u rejectedByError=%u",
                counter(CLodWorkGraphCounterIndex::TraverseNodesInternalNodeRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCulledNodeRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesRejectedByErrorRecords));

            ImGui::Text("Voxel leaves: reached=%u rejectedByError=%u segmentPageHit=%u segmentPageMiss=%u rasterWork=%u rasterDrop=%u",
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped));

            const uint32_t traverseCoalescedLaunches = counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedLaunches);
            const uint32_t traverseCoalescedInputRecords = counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputRecords);
            const float avgRecordsPerLaunch = (traverseCoalescedLaunches > 0)
                ? (static_cast<float>(traverseCoalescedInputRecords) / static_cast<float>(traverseCoalescedLaunches))
                : 0.0f;
            const float packingPercent = 100.0f * avgRecordsPerLaunch / 8.0f;

            ImGui::Text("Traverse coalesced launches: %u | input records: %u | avg records/launch: %.2f (%.1f%% of 8)",
                traverseCoalescedLaunches,
                traverseCoalescedInputRecords,
                avgRecordsPerLaunch,
                packingPercent);

            std::array<uint32_t, 8> traverseInputHistogram = {
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount1),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount2),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount3),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount4),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount5),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount6),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount7),
                counter(CLodWorkGraphCounterIndex::TraverseNodesCoalescedInputCount8)
            };

            ImGui::TextUnformatted("Traverse coalesced input histogram (records per launch):");
            float histogramValues[8] = {};
            for (size_t i = 0; i < traverseInputHistogram.size(); ++i) {
                histogramValues[i] = static_cast<float>(traverseInputHistogram[i]);
            }

            static const char* kHistogramLabels[8] = { "1", "2", "3", "4", "5", "6", "7", "8" };
            const std::string histogramId = std::string("##TraverseCoalescedInputHistogram") + title;
            if (ImPlot::BeginPlot(histogramId.c_str(), ImVec2(-1.0f, 150.0f), ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Records", "Launches", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                ImPlot::SetupAxisTicks(ImAxis_X1, 0.0, 7.0, 8, kHistogramLabels);
                ImPlot::PlotBars("Launches", histogramValues, 8, 0.6f, 0.0f);
                ImPlot::EndPlot();
            }

            const uint32_t clusterWaves = counter(CLodWorkGraphCounterIndex::ClusterCullWaves);
            const uint32_t zeroSurvivorWaves = counter(CLodWorkGraphCounterIndex::ClusterCullZeroSurvivorWaves);
            const uint32_t survivingWaves = (clusterWaves > zeroSurvivorWaves)
                ? (clusterWaves - zeroSurvivorWaves)
                : 0u;
            drawUtilizationRow("ClusterCull waves with survivors", survivingWaves, clusterWaves);

            ImGui::Text("Visible cluster writes: %u", counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites));

            ImGui::Separator();
            ImGui::TextUnformatted("Raster bucket sort/compaction");
            {
                const uint32_t histInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
                const uint32_t histVoxels = counter(CLodWorkGraphCounterIndex::RasterSortHistogramVoxelSkipped);
                const uint32_t histReyes = counter(CLodWorkGraphCounterIndex::RasterSortHistogramReyesSkipped);
                const uint32_t histTriangles = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors);
                const uint32_t compactInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
                const uint32_t compactVoxels = counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped);
                const uint32_t compactReyes = counter(CLodWorkGraphCounterIndex::RasterSortCompactionReyesSkipped);
                const uint32_t compactTriangles = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
                const uint32_t rasterGroups = counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
                const uint32_t rasterInRange = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
                const uint32_t rasterInitFailed = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
                const uint32_t rasterOutputTriangles = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
                const uint32_t rasterZeroTriangleOutputs = counter(CLodWorkGraphCounterIndex::RasterMeshShaderZeroTriangleOutputs);
                const uint32_t rasterInitZeroPage = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedZeroPageSlab);
                const uint32_t rasterInitMeshletOob = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedMeshletOutOfBounds);
                const uint32_t rasterInitInvalidOutput = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailedInvalidOutputCounts);
                const uint32_t rasterSourceGroupMismatch = counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch);
                const uint32_t pixelInvocations = counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations);
                const uint32_t pixelScissorRejected = counter(CLodWorkGraphCounterIndex::RasterPixelScissorRejected);
                const uint32_t pixelBoundsRejected = counter(CLodWorkGraphCounterIndex::RasterPixelTargetBoundsRejected);
                const uint32_t pixelVisibilityWrites = counter(CLodWorkGraphCounterIndex::RasterPixelVisibilityWrites);
                const uint32_t pixelVsmClipmapRejected = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowClipmapRejected);
                const uint32_t pixelVsmPageRejected = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected);
                const uint32_t pixelVsmWrites = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites);
                ImGui::Text("Histogram: input=%u triangles=%u voxelsSkipped=%u reyesSkipped=%u",
                    histInputs,
                    histTriangles,
                    histVoxels,
                    histReyes);
                ImGui::Text("Compaction: input=%u emittedTriangles=%u voxelsSkipped=%u reyesSkipped=%u",
                    compactInputs,
                    compactTriangles,
                    compactVoxels,
                    compactReyes);
                ImGui::Text("Raster MS: groups=%u inRange=%u initFailed=%u outputTriangles=%u zeroTriOutputs=%u",
                    rasterGroups,
                    rasterInRange,
                    rasterInitFailed,
                    rasterOutputTriangles,
                    rasterZeroTriangleOutputs);
                ImGui::Text("Raster MS init failures: zeroPageSlab=%u meshletOOB=%u invalidOutputCounts=%u",
                    rasterInitZeroPage,
                    rasterInitMeshletOob,
                    rasterInitInvalidOutput);
                ImGui::Text("Raster MS diagnostics: sourceGroupMismatch=%u", rasterSourceGroupMismatch);
                ImGui::Text("Raster PS: invocations=%u scissorRejected=%u boundsRejected=%u visibilityWrites=%u",
                    pixelInvocations,
                    pixelScissorRejected,
                    pixelBoundsRejected,
                    pixelVisibilityWrites);
                ImGui::Text("Raster PS VSM: clipmapRejected=%u pageRejected=%u writes=%u",
                    pixelVsmClipmapRejected,
                    pixelVsmPageRejected,
                    pixelVsmWrites);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("ClusterCull meshlet rejection breakdown");
            {
                const uint32_t meshletIter = counter(CLodWorkGraphCounterIndex::ClusterCullMeshletIterations);
                const uint32_t rejFrustum = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedFrustum);
                const uint32_t rejCond2 = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCondition2);
                const uint32_t rejOccl = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOcclusion);
                const uint32_t rejOOR = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOutOfRange);
                const uint32_t rejPageBounds = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedPageBounds);
                const uint32_t rejCleanPages = counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCleanPages);
                const uint32_t shadowClipmapMisses = counter(CLodWorkGraphCounterIndex::ClusterCullShadowClipmapMisses);
                const uint32_t shadowDirtyRegionHits = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyRegionHits);
                const uint32_t shadowDirtyQueries = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyQueries);
                const uint32_t shadowDirtyClipped = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyQueriesClipped);
                const uint32_t shadowDirtyCoarseMipChecks = counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyRegionCoarseMipChecks);
                const uint32_t survived = counter(CLodWorkGraphCounterIndex::ClusterCullSurvivingLanes);
                const uint32_t totalRejected = rejFrustum + rejCond2 + rejOccl + rejOOR + rejPageBounds + rejCleanPages;

                ImGui::Text("Meshlet iterations evaluated: %u", meshletIter);
                ImGui::Text("Survived: %u", survived);
                ImGui::Text("Rejected total: %u", totalRejected);

                auto rejectionRow = [](const char* label, uint32_t count, uint32_t total) {
                    const float pct = (total > 0)
                        ? (100.0f * static_cast<float>(count) / static_cast<float>(total))
                        : 0.0f;
                    ImGui::Text("  %s: %u (%.1f%%)", label, count, pct);
                };
                rejectionRow("Frustum cull", rejFrustum, totalRejected);
                rejectionRow("Condition 2 (child group refinement)", rejCond2, totalRejected);
                rejectionRow("Occlusion cull", rejOccl, totalRejected);
                rejectionRow(
                    denseDispatchActive ? "Inactive iterations / tail lanes" : "WaveActiveMax padding (inactive iterations)",
                    rejOOR,
                    totalRejected);
                rejectionRow("Page bounds overflow", rejPageBounds, totalRejected);
                rejectionRow("Clean shadow pages", rejCleanPages, totalRejected);
                ImGui::Text("  Shadow clipmap misses: %u", shadowClipmapMisses);
                ImGui::Text("  Shadow dirty queries: %u | clipped: %u | coarse-mip checks: %u",
                    shadowDirtyQueries,
                    shadowDirtyClipped,
                    shadowDirtyCoarseMipChecks);
                ImGui::Text("  Shadow dirty hits: %u | clean-page rejects: %u",
                    shadowDirtyRegionHits,
                    rejCleanPages);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("SW/HW/PageJob classification breakdown");
            {
                const uint32_t contributing = counter(CLodWorkGraphCounterIndex::ClassifyContributing);
                const uint32_t routedHW = counter(CLodWorkGraphCounterIndex::ClassifyRoutedHW);
                const uint32_t routedSW = counter(CLodWorkGraphCounterIndex::ClassifyRoutedSW);
                const uint32_t routedPJ = counter(CLodWorkGraphCounterIndex::ClassifyRoutedPageJob);
                ImGui::Text("Contributing (survived cull): %u", contributing);
                ImGui::Text("Routed -> HW: %u | SW: %u | PageJob: %u", routedHW, routedSW, routedPJ);
                if (contributing > 0) {
                    ImGui::Text("  HW: %.1f%% | SW: %.1f%% | PJ: %.1f%%",
                        100.0f * routedHW / (float)contributing,
                        100.0f * routedSW / (float)contributing,
                        100.0f * routedPJ / (float)contributing);
                }

                const uint32_t pjRejectReyes = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectReyesDisplacement);
                const uint32_t pjRejectAlpha = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectAlphaTested);
                const uint32_t pjRejectNoClipmap = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectNoClipmapIndex);
                const uint32_t pjRejectThreshold = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectBelowThreshold);
                const uint32_t pjRejectDisabled = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectDisabled);
                const uint32_t pjRejectAlreadySW = counter(CLodWorkGraphCounterIndex::ClassifyPJRejectAlreadySW);
                const uint32_t swDisabled = counter(CLodWorkGraphCounterIndex::ClassifySwDisabled);
                const uint32_t totalPJRejects = pjRejectReyes + pjRejectAlpha + pjRejectNoClipmap
                    + pjRejectThreshold + pjRejectDisabled + pjRejectAlreadySW;

                ImGui::Text("PageJob rejections (total: %u):", totalPJRejects);
                ImGui::Text("  Disabled: %u | AlreadySW: %u | ReyesDisplacement: %u",
                    pjRejectDisabled, pjRejectAlreadySW, pjRejectReyes);
                ImGui::Text("  AlphaTested: %u | NoClipmapIdx: %u | BelowThreshold: %u",
                    pjRejectAlpha, pjRejectNoClipmap, pjRejectThreshold);
                ImGui::Text("  SW classification disabled (contributes but !swRasterEnabled): %u", swDisabled);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("PageJob WG pipeline");
            {
                const uint32_t buildProcessed = counter(CLodWorkGraphCounterIndex::PageJobBuildClustersProcessed);
                const uint32_t buildEmitted = counter(CLodWorkGraphCounterIndex::PageJobBuildPagesEmitted);
                ImGui::Text("Build: processed=%u emitted=%u", buildProcessed, buildEmitted);

                const uint32_t rasterJobs = counter(CLodWorkGraphCounterIndex::PageJobRasterJobsLaunched);
                const uint32_t pixWritten = counter(CLodWorkGraphCounterIndex::PageJobRasterPixelsWritten);
                const uint32_t flagWrites = counter(CLodWorkGraphCounterIndex::PageJobRasterFlagWrites);
                ImGui::Text("Raster: jobs=%u pixWritten=%u flagWrites=%u", rasterJobs, pixWritten, flagWrites);
            }

            ImGui::Separator();
            ImGui::TextUnformatted("Occlusion -> Phase 2 enqueue attempts");
            ImGui::Text("Node attempts: %u | Cluster attempts: %u",
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionNodeReplayEnqueueAttempts),
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionClusterReplayEnqueueAttempts));

            ImGui::TextUnformatted("Phase 2 replay launch validation");
            ImGui::Text("ReplayNode launches: %u | input records: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords));
            ImGui::Text("ReplayNode emitted traverse records: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeRecordsEmitted));
            ImGui::Text("ReplayMeshlet launches: %u | input records: %u | emitted bucket records: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletBucketRecordsEmitted));

            ImGui::TextUnformatted("Phase 2 downstream consumption");
            ImGui::Text("Replay Traverse records consumed: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayTraverseRecordsConsumed));
            ImGui::Text("Replay ClusterCull bucket records consumed: %u",
                counter(CLodWorkGraphCounterIndex::Phase2ReplayClusterBucketRecordsConsumed));
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Extended capture statistics");
        if (captureState.captureStatsPending) {
            ImGui::Text("Capture stats status: pending...");
        }
        else if (!captureState.captureStatsAvailable) {
            ImGui::TextDisabled("No extended capture results yet.");
        }
        else {
            ImGui::Text("Visible clusters: %u", captureState.captureStats.visibleClusterCount);
            ImGui::Text("Unique views: %u | Unique instances: %u | Unique meshlets: %u",
                captureState.captureStats.uniqueViews,
                captureState.captureStats.uniqueInstances,
                captureState.captureStats.uniqueMeshlets);
            ImGui::Text("Avg clusters/view: %.2f | Avg clusters/instance: %.2f",
                captureState.captureStats.avgClustersPerView,
                captureState.captureStats.avgClustersPerInstance);
            ImGui::Text("Max clusters/view: %u (%.1f%% of total)",
                captureState.captureStats.maxClustersPerView,
                captureState.captureStats.dominantViewPercent);
            ImGui::Text("Max clusters/instance: %u (%.1f%% of total)",
                captureState.captureStats.maxClustersPerInstance,
                captureState.captureStats.dominantInstancePercent);
        }
    };

    drawWorkGraphCaptureSection("Primary CLod WG", m_clodTelemetry);
    drawWorkGraphCaptureSection("Shadow CLod WG", m_shadowClodTelemetry);

    ImGui::Separator();
    ImGui::TextUnformatted("Directional shadow clipmap snapshot");
    ImGui::Text("Clipmaps published: %u", m_directionalShadowDebugLatest.clipmapCount);
    for (uint32_t clipmapIndex = 0; clipmapIndex < m_directionalShadowDebugLatest.clipmapCount; ++clipmapIndex) {
        const auto& clipmap = m_directionalShadowDebugLatest.clipmaps[clipmapIndex];
        if (clipmap.valid == 0u) {
            continue;
        }

        ImGui::Text(
            "Clip %u: pos=(%.2f, %.2f, %.2f) size=%.2f near=%.3f far=%.3f pageOffset=(%lld, %lld)",
            clipmapIndex,
            clipmap.positionWorldSpace[0],
            clipmap.positionWorldSpace[1],
            clipmap.positionWorldSpace[2],
            clipmap.clipDiameter,
            clipmap.nearPlane,
            clipmap.farPlane,
            static_cast<long long>(clipmap.pageOffsetX),
            static_cast<long long>(clipmap.pageOffsetY));
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Virtual Shadow Map");
    if (m_shadowVirtualShadowTelemetry.capturePending) {
        ImGui::Text("VSM capture status: pending...");
    }
    else if (!m_shadowVirtualShadowTelemetry.hasData) {
        ImGui::TextDisabled("No VSM capture results yet.");
    }
    else {
        const CLodVirtualShadowStats& stats = m_shadowVirtualShadowTelemetry.stats;
        const CLodVirtualShadowRuntimeState& runtimeState = m_shadowVirtualShadowTelemetry.runtimeState;
        const uint32_t displayedClipmapCount = (std::min)(
            (std::max)((std::max)(stats.activeClipmapCount, stats.validClipmapCount), runtimeState.clipmapCount),
            CLodVirtualShadowMaxSupportedClipmapCount);
        ImGui::Text("Captures: %llu", static_cast<unsigned long long>(m_shadowVirtualShadowTelemetry.captureCount));
        ImGui::Text("Clipmaps: active=%u valid=%u supportedMax=%u", stats.activeClipmapCount, stats.validClipmapCount, CLodVirtualShadowMaxSupportedClipmapCount);
        ImGui::Text(
            "Runtime: publishedActive=%u supported=%u virtualRes=%u pageTable=%u physicalAtlas=%ux%u maxPhysicalPages=%u maxRequests=%u lodBias=%.2f",
            runtimeState.clipmapCount,
            runtimeState.supportedClipmapCount,
            runtimeState.virtualResolution,
            runtimeState.pageTableResolution,
            runtimeState.physicalAtlasPagesWide,
            runtimeState.physicalAtlasPagesHigh,
            runtimeState.maxPhysicalPages,
            runtimeState.maxAllocationRequests,
            runtimeState.directionalLodBias);
        const uint32_t allocatablePhysicalPages = std::min(
            stats.freePhysicalPageCount + stats.reusablePhysicalPageCount,
            runtimeState.maxPhysicalPages);
        const uint32_t unbackedAllocationRequests =
            stats.allocationRequestCount > allocatablePhysicalPages
            ? stats.allocationRequestCount - allocatablePhysicalPages
            : 0u;
        ImGui::Text("Allocator: requests=%u dispatchGroups=%u freePages=%u reusablePages=%u allocatable=%u unbacked=%u",
            stats.allocationRequestCount,
            stats.allocationDispatchGroupCount,
            stats.freePhysicalPageCount,
            stats.reusablePhysicalPageCount,
            allocatablePhysicalPages,
            unbackedAllocationRequests);
        ImGui::Text(
            "Controller: requestAllocation=%.1f%% targetBias=%.2f smoothedBias=%.2f recoveryStableFrames=%u",
            stats.currentAllocationPercentage * 100.0f,
            stats.targetPressureLodBias,
            stats.smoothedPressureLodBias,
            stats.framesSinceOverBudget);
        ImGui::Text("Lifecycle diagnostics: setupReset=%u requestOverflow=%u unwrittenClears=%u",
            stats.setupResetApplied,
            stats.markRequestOverflowCount,
            std::accumulate(
                std::begin(stats.clearedUnwrittenDirtyPages),
                std::end(stats.clearedUnwrittenDirtyPages),
                0u));
        ImGui::Text("Setup reset reasons: forced=%u noPrev=%u structureMismatch=%u lightDirChanged=%u",
            stats.setupResetForced,
            stats.setupResetNoPreviousState,
            stats.setupResetStructureMismatch,
            stats.setupResetLightDirectionChanged);
        uint32_t totalVisitedPageTableEntries = 0u;
        uint32_t totalVisitedDirtyPageTableEntries = 0u;
        uint32_t totalResidentCleanHits = 0u;
        uint32_t totalResidentDirtyHits = 0u;
        uint32_t totalRequestedPages = 0u;
        uint32_t totalDirtyPageTableEntries = 0u;
        uint32_t totalPredictiveInvalidatedPageTableEntries = 0u;
        uint32_t totalInvalidatedCurrentBoundsPageTableEntries = 0u;
        uint32_t totalInvalidatedPreviousBoundsPageTableEntries = 0u;
        uint32_t totalInvalidatedSkinnedPageTableEntries = 0u;
        for (uint32_t clipmapIndex = 0u; clipmapIndex < displayedClipmapCount; ++clipmapIndex) {
            totalVisitedPageTableEntries += stats.visitedPageTableEntries[clipmapIndex];
            totalVisitedDirtyPageTableEntries += stats.visitedDirtyPageTableEntries[clipmapIndex];
            totalResidentCleanHits += stats.markResidentCleanHits[clipmapIndex];
            totalResidentDirtyHits += stats.markResidentDirtyHits[clipmapIndex];
            totalRequestedPages += stats.requestedPages[clipmapIndex];
            totalDirtyPageTableEntries += stats.dirtyPageTableEntries[clipmapIndex];
            totalPredictiveInvalidatedPageTableEntries += stats.predictiveInvalidatedPageTableEntries[clipmapIndex];
            totalInvalidatedCurrentBoundsPageTableEntries += stats.invalidatedCurrentBoundsPageTableEntries[clipmapIndex];
            totalInvalidatedPreviousBoundsPageTableEntries += stats.invalidatedPreviousBoundsPageTableEntries[clipmapIndex];
            totalInvalidatedSkinnedPageTableEntries += stats.invalidatedSkinnedPageTableEntries[clipmapIndex];
        }
        ImGui::Text("Cache lifecycle: cleanHits=%u dirtyHits=%u requests=%u visitedPT=%u visitedDirtyPT=%u dirtyPT=%u",
            totalResidentCleanHits,
            totalResidentDirtyHits,
            totalRequestedPages,
            totalVisitedPageTableEntries,
            totalVisitedDirtyPageTableEntries,
            totalDirtyPageTableEntries);
        ImGui::Text("Request creators: predictiveInv=%u invalidateCurr=%u invalidatePrev=%u invalidateSkinned=%u wrapClr=%u staleClr=%u",
            totalPredictiveInvalidatedPageTableEntries,
            totalInvalidatedCurrentBoundsPageTableEntries,
            totalInvalidatedPreviousBoundsPageTableEntries,
            totalInvalidatedSkinnedPageTableEntries,
            std::accumulate(
                std::begin(stats.setupWrappedClearedPageTableEntries),
                std::end(stats.setupWrappedClearedPageTableEntries),
                0u),
            std::accumulate(
                std::begin(stats.setupStaleDirtyClearedPageTableEntries),
                std::end(stats.setupStaleDirtyClearedPageTableEntries),
                0u));
        ImGui::TextDisabled("Dirty PT is sampled at GatherStatsPass before ClearPages re-marks cleared pages dirty for the hierarchy build.");
        if (ImGui::BeginTable("##VirtualShadowStatsTable", 20, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Clip");
            ImGui::TableSetupColumn("Selected");
            ImGui::TableSetupColumn("Proj Reject");
            ImGui::TableSetupColumn("Requests");
            ImGui::TableSetupColumn("Clean Hits");
            ImGui::TableSetupColumn("Dirty Hits");
            ImGui::TableSetupColumn("Wrap Clr");
            ImGui::TableSetupColumn("Stale Clr");
            ImGui::TableSetupColumn("Pre NZ PT");
            ImGui::TableSetupColumn("Pre Dirty PT");
            ImGui::TableSetupColumn("NonZero PT");
            ImGui::TableSetupColumn("Allocated PT");
            ImGui::TableSetupColumn("Visited PT");
            ImGui::TableSetupColumn("Visited Dirty");
            ImGui::TableSetupColumn("Dirty PT");
            ImGui::TableSetupColumn("NoWrite Clr");
            ImGui::TableSetupColumn("Pred Inv");
            ImGui::TableSetupColumn("Inv Curr");
            ImGui::TableSetupColumn("Inv Prev");
            ImGui::TableSetupColumn("Inv Skin");
            ImGui::TableHeadersRow();

            for (uint32_t clipmapIndex = 0u; clipmapIndex < displayedClipmapCount; ++clipmapIndex) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%u", clipmapIndex);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", stats.selectedPixels[clipmapIndex]);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", stats.projectionRejectedPixels[clipmapIndex]);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", stats.requestedPages[clipmapIndex]);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%u", stats.markResidentCleanHits[clipmapIndex]);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%u", stats.markResidentDirtyHits[clipmapIndex]);
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%u", stats.setupWrappedClearedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%u", stats.setupStaleDirtyClearedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(8);
                ImGui::Text("%u", stats.preAllocateNonZeroPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(9);
                ImGui::Text("%u", stats.preAllocateDirtyPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(10);
                ImGui::Text("%u", stats.nonZeroPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(11);
                ImGui::Text("%u", stats.allocatedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(12);
                ImGui::Text("%u", stats.visitedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(13);
                ImGui::Text("%u", stats.visitedDirtyPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(14);
                ImGui::Text("%u", stats.dirtyPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(15);
                ImGui::Text("%u", stats.clearedUnwrittenDirtyPages[clipmapIndex]);
                ImGui::TableSetColumnIndex(16);
                ImGui::Text("%u", stats.predictiveInvalidatedPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(17);
                ImGui::Text("%u", stats.invalidatedCurrentBoundsPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(18);
                ImGui::Text("%u", stats.invalidatedPreviousBoundsPageTableEntries[clipmapIndex]);
                ImGui::TableSetColumnIndex(19);
                ImGui::Text("%u", stats.invalidatedSkinnedPageTableEntries[clipmapIndex]);
            }

            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Reyes Pipeline");
    const auto drawReyesPhase = [](const char* label, const CLodReyesTelemetry& telemetry) {
        if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        const uint32_t totalDiceInputs = telemetry.immediateDiceQueueEntryCount + telemetry.finalDiceQueueEntryCount;
        ImGui::Text("Phase index: %u", telemetry.phaseIndex);
        ImGui::Text("Classify: visible input=%u reyes-owned=%u bypass-full=%u immediate dice=%u",
            telemetry.visibleClusterInputCount,
            telemetry.ownedClusterOutputCount,
            telemetry.fullClusterOutputCount,
            telemetry.immediateDiceQueueEntryCount);
        ImGui::Text("Split: deepest level=%u max configured=%u split-routed dice=%u",
            telemetry.deepestSplitLevelReached,
            telemetry.configuredMaxSplitPassCount,
            telemetry.finalDiceQueueEntryCount);
        ImGui::Text("Split rejects: invalid domains=%u fallback-to-dice=%u frustum=%u shadow-dirty=%u child=%u",
            telemetry.invalidSplitPatchDomainCount,
            telemetry.splitCollapseFallbackDiceCount,
            telemetry.splitFrustumCullCount,
            telemetry.splitShadowDirtyCullCount,
            telemetry.splitChildCullCount);
        ImGui::Text("Coarse dirty-only: eligible=%u rejected=%u leaf outputs=%u",
            telemetry.splitCoarseOnlyDirtyEligibleCount,
            telemetry.splitCoarseOnlyDirtyRejectedCount,
            telemetry.splitCoarseOnlyDirtyLeafOutputCount);
        ImGui::Text("Dice: total queue inputs=%u valid patches=%u invalid domains=%u est triangles=%u est vertices=%u",
            totalDiceInputs,
            telemetry.dicedPatchCount,
            telemetry.invalidDicePatchDomainCount,
            telemetry.dicedTriangleEstimateCount,
            telemetry.dicedVertexEstimateCount);
        ImGui::Text("Raster work: entries=%u emitted patches=%u emitted microtriangles=%u overflow patches=%u overflow batches=%u",
            telemetry.rasterWorkEntryCount,
            telemetry.patchRasterizedPatchCount,
            telemetry.patchRasterizedMicroTriangleCount,
            telemetry.rasterWorkOverflowPatchCount,
            telemetry.rasterWorkOverflowBatchCount);
        ImGui::Text(
            "Hardware Reyes: meshGroups=%u packed entries=%u emitted triangles=%u avg entries/group=%.2f avg emitted/group=%.2f avg requested/group=%.2f",
            telemetry.hardwareRasterMeshGroupCount,
            telemetry.hardwareRasterPackedWorkEntryCount,
            telemetry.hardwareRasterMicroTriangleCount,
            telemetry.hardwareRasterMeshGroupCount > 0u
                ? static_cast<float>(telemetry.hardwareRasterPackedWorkEntryCount) / static_cast<float>(telemetry.hardwareRasterMeshGroupCount)
                : 0.0f,
            telemetry.hardwareRasterMeshGroupCount > 0u
                ? static_cast<float>(telemetry.hardwareRasterMicroTriangleCount) / static_cast<float>(telemetry.hardwareRasterMeshGroupCount)
                : 0.0f,
            telemetry.hardwareRasterMeshGroupCount > 0u
                ? static_cast<float>(telemetry.hardwareRasterRequestedMicroTriangleCount) / static_cast<float>(telemetry.hardwareRasterMeshGroupCount)
                : 0.0f);
        ImGui::Text("Patch raster rejects: zeroCount=%u overflow=%u clip=%u area=%u bounds=%u clippedQuad=%u",
            telemetry.rasterZeroMicroTriangleCount,
            telemetry.rasterMicroTriangleOverflowCount,
            telemetry.rasterClipCullCount,
            telemetry.rasterPreAreaCullCount,
            telemetry.rasterEmptyBoundsCullCount,
            telemetry.rasterNearPlaneClippedQuadCount);
        ImGui::Text("Patch raster projected-triangles: windingSwaps=%u postSwapDegenerate=%u",
            telemetry.rasterWindingSwapCount,
            telemetry.rasterPostSwapNonNegativeAreaCount);
        ImGui::Text("Patch raster tiny-triangle fallback=%u",
            telemetry.rasterTinyTriangleFallbackCount);

        ImGui::TextUnformatted("Per split pass");
        for (uint32_t splitPassIndex = 0; splitPassIndex < CLodReyesMaxSplitPassCount; ++splitPassIndex) {
            ImGui::Text(
                "  Split %u: input=%u children=%u diced=%u splitOverflow=%u diceOverflow=%u",
                splitPassIndex,
                telemetry.splitInputCounts[splitPassIndex],
                telemetry.splitChildOutputCounts[splitPassIndex],
                telemetry.splitDiceOutputCounts[splitPassIndex],
                telemetry.splitQueueOverflowCounts[splitPassIndex],
                telemetry.diceQueueOverflowCounts[splitPassIndex]);
        }
    };

    if (m_clodReyesTelemetryCapturePending) {
        ImGui::Text("Reyes capture status: pending...");
    }
    else if (!m_clodReyesTelemetryHasData) {
        ImGui::TextDisabled("No Reyes telemetry capture results yet.");
    }
    else {
        ImGui::Text("Reyes telemetry captures: %llu", static_cast<unsigned long long>(m_clodReyesTelemetryCaptureCount));
        drawReyesPhase("Reyes Phase 1", m_clodReyesTelemetryPhase1);
        drawReyesPhase("Reyes Phase 2", m_clodReyesTelemetryPhase2);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Shadow Reyes Pipeline");
    if (m_shadowClodReyesTelemetryCapturePending) {
        ImGui::Text("Shadow Reyes capture status: pending...");
    }
    else if (!m_shadowClodReyesTelemetryHasData) {
        ImGui::TextDisabled("No shadow Reyes telemetry capture results yet.");
    }
    else {
        ImGui::Text("Shadow Reyes telemetry captures: %llu", static_cast<unsigned long long>(m_shadowClodReyesTelemetryCaptureCount));
        drawReyesPhase("Shadow Reyes Phase 1", m_shadowClodReyesTelemetryPhase1);
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Alpha Deep Visibility");
    if (m_clodAlphaTelemetryCapturePending) {
        ImGui::Text("Alpha capture status: pending...");
    }
    else if (!m_clodAlphaTelemetryHasData) {
        ImGui::TextDisabled("No alpha deep-visibility capture results yet.");
    }
    else {
        ImGui::Text("Allocated nodes: %u", m_clodAlphaNodeCount);
        ImGui::Text("Overflowed allocations: %u", m_clodAlphaOverflowCount);
        ImGui::Text("Truncated pixels: %u | Truncated nodes: %u",
            m_clodAlphaStats.truncatedPixelCount,
            m_clodAlphaStats.truncatedNodeCount);
        ImGui::Text("Resolved samples: %u", m_clodAlphaStats.totalResolvedSamples);
        ImGui::Text("Max raw node count/pixel: %u", m_clodAlphaStats.maxRawNodeCount);
        ImGui::Text("Max resolved samples/pixel: %u", m_clodAlphaStats.maxResolvedSamples);
    }

    ImGui::End();
}

inline void Menu::DrawFrameTaskGraphWindow() {
    ImGui::Begin("CPU Frame Task Graph", nullptr);

    ImGui::Text(
        "Scene overlap: %s | committed=%llu | completed=%llu | source frame=%llu | last task=%.2f ms",
        m_sceneOverlapStatus.taskInFlight ? "running" : (m_sceneOverlapStatus.enabled ? "idle" : "disabled"),
        static_cast<unsigned long long>(m_sceneOverlapStatus.committedSnapshotSequence),
        static_cast<unsigned long long>(m_sceneOverlapStatus.lastCompletedSnapshotSequence),
        static_cast<unsigned long long>(m_sceneOverlapStatus.lastCommittedSourceFrame),
        m_sceneOverlapStatus.lastTaskDurationMs);
    if (!m_sceneOverlapStatus.hasCommittedSnapshot) {
        ImGui::TextDisabled("No committed scene snapshot is available yet.");
    }
    ImGui::Separator();

    ImGui::Checkbox("Pause", &m_frameTaskGraphPaused);

    if (!m_frameTaskGraphPaused) {
        br::telemetry::FrameTaskGraphSnapshot latestSnapshot{};
        if (br::telemetry::TryReadFrameTaskGraphSnapshot(m_frameTaskGraphLastSequence, latestSnapshot)) {
            m_frameTaskGraphLatest = latestSnapshot;
            m_frameTaskGraphHasData = true;
            if (m_frameTaskGraphHistory.empty() || m_frameTaskGraphHistory.back().frameNumber != latestSnapshot.frameNumber) {
                m_frameTaskGraphHistory.push_back(latestSnapshot);
                constexpr size_t kMaxHistoryFrames = 240;
                if (m_frameTaskGraphHistory.size() > kMaxHistoryFrames) {
                    m_frameTaskGraphHistory.erase(m_frameTaskGraphHistory.begin());
                }
            }
        }
    }

    if (!m_frameTaskGraphHasData || m_frameTaskGraphLatest.nodeCount == 0) {
        ImGui::TextDisabled("No CPU frame task graph snapshots published yet.");
        ImGui::End();
        return;
    }

    const auto domainName = [](br::telemetry::CpuTaskDomain domain) {
        switch (domain) {
        case br::telemetry::CpuTaskDomain::MainThread:
            return "Main";
        case br::telemetry::CpuTaskDomain::Worker:
            return "Worker";
        case br::telemetry::CpuTaskDomain::IOService:
            return "IO";
        case br::telemetry::CpuTaskDomain::BackgroundService:
            return "Background";
        default:
            return "Unknown";
        }
    };

    const auto domainColor = [](br::telemetry::CpuTaskDomain domain) {
        switch (domain) {
        case br::telemetry::CpuTaskDomain::MainThread:
            return IM_COL32(74, 144, 226, 255);
        case br::telemetry::CpuTaskDomain::Worker:
            return IM_COL32(91, 192, 120, 255);
        case br::telemetry::CpuTaskDomain::IOService:
            return IM_COL32(245, 166, 35, 255);
        case br::telemetry::CpuTaskDomain::BackgroundService:
            return IM_COL32(214, 93, 177, 255);
        default:
            return IM_COL32(160, 160, 160, 255);
        }
    };

    const auto isWaitForFrame = [](const char* name) {
        return std::strcmp(name, "WaitForFrame") == 0;
    };

    struct StageAggregate {
        char name[64]{};
        br::telemetry::CpuTaskDomain domain = br::telemetry::CpuTaskDomain::MainThread;
        int32_t dependencyNodeIndex = -1;
        uint64_t avgStartUs = 0;
        uint64_t avgSpanUs = 0;
        uint64_t avgTotalDurationUs = 0;
        uint64_t minTotalDurationUs = 0;
        uint64_t maxTotalDurationUs = 0;
        uint64_t latestStartUs = 0;
        uint64_t latestSpanUs = 0;
        uint64_t latestTotalDurationUs = 0;
        uint32_t avgDispatchCount = 0;
        uint32_t latestDispatchCount = 0;
        uint32_t sampleCount = 0;
    };

    const int clampedAverageWindow = (std::max)(1, (std::min)(m_frameTaskGraphAverageWindow, 120));
    m_frameTaskGraphAverageWindow = clampedAverageWindow;

    const size_t historyCount = m_frameTaskGraphHistory.size();
    const size_t windowCount = (std::min)(historyCount, static_cast<size_t>(m_frameTaskGraphAverageWindow));
    const size_t windowStart = historyCount > windowCount ? (historyCount - windowCount) : 0;

    const auto buildStageGroups = [](const br::telemetry::FrameTaskGraphSnapshot& snapshot) {
        std::vector<StageAggregate> groups;
        groups.reserve(snapshot.nodeCount);

        for (uint32_t nodeIndex = 0; nodeIndex < snapshot.nodeCount; ++nodeIndex) {
            const auto& node = snapshot.nodes[nodeIndex];
            auto existingGroup = std::find_if(groups.begin(), groups.end(), [&](const StageAggregate& group) {
                return group.domain == node.domain && std::strcmp(group.name, node.name) == 0;
            });

            if (existingGroup == groups.end()) {
                StageAggregate group{};
                std::snprintf(group.name, sizeof(group.name), "%s", node.name);
                group.domain = node.domain;
                group.dependencyNodeIndex = node.dependencyNodeIndex;
                group.latestStartUs = node.startTimeUs;
                group.latestSpanUs = node.durationUs;
                group.latestTotalDurationUs = node.durationUs;
                group.latestDispatchCount = 1;
                groups.push_back(group);
                continue;
            }

            const uint64_t currentEndUs = existingGroup->latestStartUs + existingGroup->latestSpanUs;
            existingGroup->latestStartUs = (std::min)(existingGroup->latestStartUs, node.startTimeUs);
            const uint64_t nodeEndUs = node.startTimeUs + node.durationUs;
            const uint64_t updatedEndUs = (std::max)(currentEndUs, nodeEndUs);
            existingGroup->latestSpanUs = updatedEndUs - existingGroup->latestStartUs;
            existingGroup->latestTotalDurationUs += node.durationUs;
            ++existingGroup->latestDispatchCount;
        }

        return groups;
    };

    std::vector<StageAggregate> stageAggregates = buildStageGroups(m_frameTaskGraphLatest);

    uint64_t latestFrameEndUs = 0;
    for (uint32_t nodeIndex = 0; nodeIndex < m_frameTaskGraphLatest.nodeCount; ++nodeIndex) {
        const auto& node = m_frameTaskGraphLatest.nodes[nodeIndex];
        latestFrameEndUs = (std::max)(latestFrameEndUs, node.startTimeUs + node.durationUs);
    }

    for (auto& aggregate : stageAggregates) {
        uint64_t totalStartUs = 0;
        uint64_t totalSpanUs = 0;
        uint64_t totalBusyUs = 0;
        uint64_t totalDispatches = 0;

        for (size_t historyIndex = windowStart; historyIndex < historyCount; ++historyIndex) {
            const auto groupedSnapshot = buildStageGroups(m_frameTaskGraphHistory[historyIndex]);
            const auto match = std::find_if(groupedSnapshot.begin(), groupedSnapshot.end(), [&](const StageAggregate& snapshotGroup) {
                return snapshotGroup.domain == aggregate.domain && std::strcmp(snapshotGroup.name, aggregate.name) == 0;
            });
            if (match == groupedSnapshot.end()) {
                continue;
            }

            totalStartUs += match->latestStartUs;
            totalSpanUs += match->latestSpanUs;
            totalBusyUs += match->latestTotalDurationUs;
            totalDispatches += match->latestDispatchCount;
            if (aggregate.sampleCount == 0) {
                aggregate.minTotalDurationUs = match->latestTotalDurationUs;
                aggregate.maxTotalDurationUs = match->latestTotalDurationUs;
            }
            else {
                aggregate.minTotalDurationUs = (std::min)(aggregate.minTotalDurationUs, match->latestTotalDurationUs);
                aggregate.maxTotalDurationUs = (std::max)(aggregate.maxTotalDurationUs, match->latestTotalDurationUs);
            }
            ++aggregate.sampleCount;
        }

        if (aggregate.sampleCount > 0) {
            aggregate.avgStartUs = totalStartUs / aggregate.sampleCount;
            aggregate.avgSpanUs = totalSpanUs / aggregate.sampleCount;
            aggregate.avgTotalDurationUs = totalBusyUs / aggregate.sampleCount;
            aggregate.avgDispatchCount = static_cast<uint32_t>(totalDispatches / aggregate.sampleCount);
        }
        else {
            aggregate.avgStartUs = aggregate.latestStartUs;
            aggregate.avgSpanUs = aggregate.latestSpanUs;
            aggregate.avgTotalDurationUs = aggregate.latestTotalDurationUs;
            aggregate.minTotalDurationUs = aggregate.latestTotalDurationUs;
            aggregate.maxTotalDurationUs = aggregate.latestTotalDurationUs;
            aggregate.avgDispatchCount = aggregate.latestDispatchCount;
        }
    }

    std::vector<float> frameHistoryMs;
    frameHistoryMs.reserve(windowCount);
    uint64_t avgFrameEndUs = 0;
    uint64_t minFrameEndUs = 0;
    uint64_t maxFrameEndUs = 0;
    uint32_t frameSamples = 0;
    for (size_t historyIndex = windowStart; historyIndex < historyCount; ++historyIndex) {
        const auto& snapshot = m_frameTaskGraphHistory[historyIndex];
        uint64_t frameEndUs = 0;
        for (uint32_t nodeIndex = 0; nodeIndex < snapshot.nodeCount; ++nodeIndex) {
            const auto& node = snapshot.nodes[nodeIndex];
            frameEndUs = (std::max)(frameEndUs, node.startTimeUs + node.durationUs);
        }
        frameHistoryMs.push_back(static_cast<float>(frameEndUs) / 1000.0f);
        avgFrameEndUs += frameEndUs;
        if (frameSamples == 0) {
            minFrameEndUs = frameEndUs;
            maxFrameEndUs = frameEndUs;
        }
        else {
            minFrameEndUs = (std::min)(minFrameEndUs, frameEndUs);
            maxFrameEndUs = (std::max)(maxFrameEndUs, frameEndUs);
        }
        ++frameSamples;
    }
    if (frameSamples > 0) {
        avgFrameEndUs /= frameSamples;
    }

    ImGui::Text(
        "Frame %llu | swap index %u | nodes %u | grouped stages %zu",
        static_cast<unsigned long long>(m_frameTaskGraphLatest.frameNumber),
        static_cast<unsigned int>(m_frameTaskGraphLatest.frameIndex),
        m_frameTaskGraphLatest.nodeCount,
        stageAggregates.size());
    ImGui::Text(
        "Frame total: latest %.3f ms | avg(%u) %.3f ms | min/max %.3f / %.3f ms",
        static_cast<double>(latestFrameEndUs) / 1000.0,
        frameSamples,
        static_cast<double>(avgFrameEndUs) / 1000.0,
        static_cast<double>(minFrameEndUs) / 1000.0,
        static_cast<double>(maxFrameEndUs) / 1000.0);
    if (m_frameTaskGraphLatest.droppedNodeCount > 0) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
            "Dropped %u task nodes in the latest snapshot because the capture buffer filled.",
            m_frameTaskGraphLatest.droppedNodeCount);
    }

    {
        uint64_t waitUs = 0;
        bool hasWait = false;
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            if (isWaitForFrame(m_frameTaskGraphLatest.nodes[i].name)) {
                waitUs += m_frameTaskGraphLatest.nodes[i].durationUs;
                hasWait = true;
            }
        }
        if (hasWait) {
            ImGui::TextDisabled("WaitForFrame: %.3f ms (hidden from graph, GPU throughput proxy)",
                static_cast<double>(waitUs) / 1000.0);
        }
    }

    ImGui::SliderInt("Average window (frames)", &m_frameTaskGraphAverageWindow, 1, 120);

    if (!frameHistoryMs.empty() && ImPlot::BeginPlot("##CpuFrameTaskFrameHistory", ImVec2(-1.0f, 150.0f), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Recent frames", "ms", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
        std::vector<float> xValues(frameHistoryMs.size());
        for (size_t i = 0; i < frameHistoryMs.size(); ++i) {
            xValues[i] = static_cast<float>(i);
        }
        ImPlot::PlotLine("Frame total", xValues.data(), frameHistoryMs.data(), static_cast<int>(frameHistoryMs.size()));
        if (frameSamples > 0) {
            const double avgLine[2] = { static_cast<double>(avgFrameEndUs) / 1000.0, static_cast<double>(avgFrameEndUs) / 1000.0 };
            const double avgX[2] = { 0.0, static_cast<double>((std::max)(size_t{ 1 }, frameHistoryMs.size())) - 1.0 };
            ImPlot::SetNextLineStyle(ImVec4(0.95f, 0.8f, 0.2f, 1.0f), 1.5f);
            ImPlot::PlotLine("Average", avgX, avgLine, 2);
        }
        ImPlot::EndPlot();
    }

    std::vector<size_t> bottleneckOrder(stageAggregates.size());
    for (size_t i = 0; i < bottleneckOrder.size(); ++i) {
        bottleneckOrder[i] = i;
    }
    std::sort(bottleneckOrder.begin(), bottleneckOrder.end(), [&](size_t lhs, size_t rhs) {
        return stageAggregates[lhs].avgTotalDurationUs > stageAggregates[rhs].avgTotalDurationUs;
    });

    ImGui::SeparatorText("Bottlenecks");
    size_t bottleneckRank = 0;
    for (size_t i = 0; i < bottleneckOrder.size() && bottleneckRank < 5; ++i) {
        const auto& aggregate = stageAggregates[bottleneckOrder[i]];
        if (isWaitForFrame(aggregate.name)) continue;
        ++bottleneckRank;
        ImGui::Text(
            "%zu. %s [%s] avg busy %.3f ms | latest busy %.3f ms | avg dispatches %u",
            bottleneckRank,
            aggregate.name,
            domainName(aggregate.domain),
            static_cast<double>(aggregate.avgTotalDurationUs) / 1000.0,
            static_cast<double>(aggregate.latestTotalDurationUs) / 1000.0,
            aggregate.avgDispatchCount);
    }

    ImGui::SeparatorText("Task Graph");

    // Layout: assign Y position per individual node, grouped into domain swim lanes.
    // Concurrent tasks within a domain are stacked into sub-lanes.
    struct NodeLayout {
        float yCenter;
        float startMs;
        float endMs;
    };

    std::vector<NodeLayout> nodeLayouts(m_frameTaskGraphLatest.nodeCount);

    // Compute the earliest start time among visible (non-WaitForFrame) nodes
    // so we can subtract it from all displayed times, eliminating the gap.
    uint64_t displayTimeBaseUs = UINT64_MAX;
    for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
        const auto& n = m_frameTaskGraphLatest.nodes[i];
        if (!isWaitForFrame(n.name))
            displayTimeBaseUs = (std::min)(displayTimeBaseUs, n.startTimeUs);
    }
    if (displayTimeBaseUs == UINT64_MAX) displayTimeBaseUs = 0;

    constexpr float kBarHeight = 0.6f;
    constexpr float kSubLaneHeight = 0.85f;
    constexpr float kDomainGap = 0.6f;
    constexpr int kDomainOrder[] = { 0, 1, 2, 3 }; // Main, Worker, IO, Background

    float currentY = 0.0f;
    float domainLabelY[4] = {};
    bool domainHasNodes[4] = {};

    for (int di = 0; di < 4; ++di) {
        auto domain = static_cast<br::telemetry::CpuTaskDomain>(kDomainOrder[di]);

        std::vector<uint32_t> domainNodes;
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            if (m_frameTaskGraphLatest.nodes[i].domain == domain && !isWaitForFrame(m_frameTaskGraphLatest.nodes[i].name))
                domainNodes.push_back(i);
        }

        if (domainNodes.empty()) {
            domainLabelY[di] = currentY;
            continue;
        }
        domainHasNodes[di] = true;

        std::sort(domainNodes.begin(), domainNodes.end(), [&](uint32_t a, uint32_t b) {
            return m_frameTaskGraphLatest.nodes[a].startTimeUs < m_frameTaskGraphLatest.nodes[b].startTimeUs;
        });

        struct SubLane { float endTimeMs; };
        std::vector<SubLane> subLanes;
        float domainBaseY = currentY;

        for (uint32_t idx : domainNodes) {
            const auto& node = m_frameTaskGraphLatest.nodes[idx];
            float startMs = static_cast<float>(node.startTimeUs - displayTimeBaseUs) / 1000.0f;
            float endMs = static_cast<float>(node.startTimeUs + node.durationUs - displayTimeBaseUs) / 1000.0f;

            int subLane = -1;
            for (int s = 0; s < static_cast<int>(subLanes.size()); ++s) {
                if (subLanes[s].endTimeMs <= startMs) {
                    subLane = s;
                    break;
                }
            }
            if (subLane < 0) {
                subLane = static_cast<int>(subLanes.size());
                subLanes.push_back({});
            }
            subLanes[subLane].endTimeMs = endMs;

            float y = domainBaseY + subLane * kSubLaneHeight + kSubLaneHeight * 0.5f;
            nodeLayouts[idx] = { y, startMs, endMs };
        }

        float domainHeight = (std::max)(1.0f, static_cast<float>(subLanes.size())) * kSubLaneHeight;
        domainLabelY[di] = domainBaseY + domainHeight * 0.5f;
        currentY = domainBaseY + domainHeight + kDomainGap;
    }

    float totalYRange = (std::max)(currentY, 1.0f);
    float ganttPlotHeight = (std::max)(200.0f, (std::min)(totalYRange * 35.0f, 500.0f));

    uint64_t timelineFrameEndUs = 0;
    for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
        const auto& n = m_frameTaskGraphLatest.nodes[i];
        if (!isWaitForFrame(n.name))
            timelineFrameEndUs = (std::max)(timelineFrameEndUs, n.startTimeUs + n.durationUs - displayTimeBaseUs);
    }

    if (ImPlot::BeginPlot("##CpuTaskGantt", ImVec2(-1.0f, ganttPlotHeight), ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Time (ms)", nullptr, 0, ImPlotAxisFlags_Invert | ImPlotAxisFlags_NoGridLines);
        const double maxTimeMs = static_cast<double>(timelineFrameEndUs) / 1000.0 * 1.05;
        ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, (std::max)(maxTimeMs, 0.1), ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, static_cast<double>(totalYRange) + 0.5, ImPlotCond_Always);

        std::vector<double> yTicks;
        std::vector<const char*> yLabels;
        for (int di = 0; di < 4; ++di) {
            if (domainHasNodes[di]) {
                yTicks.push_back(static_cast<double>(domainLabelY[di]));
                yLabels.push_back(domainName(static_cast<br::telemetry::CpuTaskDomain>(kDomainOrder[di])));
            }
        }
        if (!yTicks.empty()) {
            ImPlot::SetupAxisTicks(ImAxis_Y1, yTicks.data(), static_cast<int>(yTicks.size()), yLabels.data());
        }

        ImDrawList* drawList = ImPlot::GetPlotDrawList();

        // Draw domain swim-lane backgrounds
        for (int di = 0; di < 4; ++di) {
            if (!domainHasNodes[di]) continue;
            float dMinY = 1e9f, dMaxY = -1e9f;
            for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
                if (m_frameTaskGraphLatest.nodes[i].domain == static_cast<br::telemetry::CpuTaskDomain>(kDomainOrder[di])) {
                    dMinY = (std::min)(dMinY, nodeLayouts[i].yCenter - kBarHeight * 0.5f);
                    dMaxY = (std::max)(dMaxY, nodeLayouts[i].yCenter + kBarHeight * 0.5f);
                }
            }
            ImVec2 bgMin = ImPlot::PlotToPixels(0.0, static_cast<double>(dMinY - 0.15f));
            ImVec2 bgMax = ImPlot::PlotToPixels(maxTimeMs, static_cast<double>(dMaxY + 0.15f));
            drawList->AddRectFilled(bgMin, bgMax, IM_COL32(40, 40, 40, 80), 4.0f);
        }

        // Draw task bars
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            const auto& node = m_frameTaskGraphLatest.nodes[i];
            if (isWaitForFrame(node.name)) continue;
            const auto& layout = nodeLayouts[i];

            ImVec2 pMin = ImPlot::PlotToPixels(
                static_cast<double>(layout.startMs),
                static_cast<double>(layout.yCenter - kBarHeight * 0.5f));
            ImVec2 pMax = ImPlot::PlotToPixels(
                static_cast<double>(layout.endMs),
                static_cast<double>(layout.yCenter + kBarHeight * 0.5f));

            if (pMax.x - pMin.x < 3.0f) pMax.x = pMin.x + 3.0f;

            ImU32 color = domainColor(node.domain);
            drawList->AddRectFilled(pMin, pMax, color, 3.0f);
            drawList->AddRect(pMin, pMax, IM_COL32(20, 20, 20, 200), 3.0f);

            float barPx = pMax.x - pMin.x;
            if (barPx > 44.0f) {
                drawList->AddText(nullptr, 0.0f,
                    ImVec2(pMin.x + 4.0f, pMin.y + 1.0f),
                    IM_COL32(10, 10, 10, 255), node.name, nullptr, barPx - 6.0f);
            }
        }

        // Draw dependency arrows
        for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
            const auto& node = m_frameTaskGraphLatest.nodes[i];
            if (isWaitForFrame(node.name)) continue;
            if (node.dependencyNodeIndex < 0 ||
                static_cast<uint32_t>(node.dependencyNodeIndex) >= m_frameTaskGraphLatest.nodeCount)
                continue;
            if (isWaitForFrame(m_frameTaskGraphLatest.nodes[static_cast<uint32_t>(node.dependencyNodeIndex)].name))
                continue;

            const auto& depLayout = nodeLayouts[static_cast<uint32_t>(node.dependencyNodeIndex)];
            const auto& thisLayout = nodeLayouts[i];

            ImVec2 from = ImPlot::PlotToPixels(
                static_cast<double>(depLayout.endMs),
                static_cast<double>(depLayout.yCenter));
            ImVec2 to = ImPlot::PlotToPixels(
                static_cast<double>(thisLayout.startMs),
                static_cast<double>(thisLayout.yCenter));

            const ImU32 arrowColor = IM_COL32(255, 220, 80, 200);
            drawList->AddLine(from, to, arrowColor, 1.5f);

            // Arrowhead
            float dx = to.x - from.x;
            float dy = to.y - from.y;
            float lenSq = dx * dx + dy * dy;
            if (lenSq > 4.0f) {
                float invLen = 1.0f / std::sqrt(lenSq);
                dx *= invLen;
                dy *= invLen;
                constexpr float arrowSz = 7.0f;
                ImVec2 p1(to.x - dx * arrowSz - dy * arrowSz * 0.4f,
                          to.y - dy * arrowSz + dx * arrowSz * 0.4f);
                ImVec2 p2(to.x - dx * arrowSz + dy * arrowSz * 0.4f,
                          to.y - dy * arrowSz - dx * arrowSz * 0.4f);
                drawList->AddTriangleFilled(to, p1, p2, arrowColor);
            }
        }

        // Tooltip on hover
        if (ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
            for (uint32_t i = 0; i < m_frameTaskGraphLatest.nodeCount; ++i) {
                if (isWaitForFrame(m_frameTaskGraphLatest.nodes[i].name)) continue;
                const auto& layout = nodeLayouts[i];
                if (mouse.x >= static_cast<double>(layout.startMs) &&
                    mouse.x <= static_cast<double>(layout.endMs) &&
                    mouse.y >= static_cast<double>(layout.yCenter - kBarHeight * 0.5f) &&
                    mouse.y <= static_cast<double>(layout.yCenter + kBarHeight * 0.5f)) {
                    const auto& node = m_frameTaskGraphLatest.nodes[i];
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(node.name);
                    ImGui::Text("Domain: %s", domainName(node.domain));
                    ImGui::Text("Start: %.3f ms", static_cast<double>(node.startTimeUs - displayTimeBaseUs) / 1000.0);
                    ImGui::Text("Duration: %.3f ms", static_cast<double>(node.durationUs) / 1000.0);
                    if (node.dependencyNodeIndex >= 0 &&
                        static_cast<uint32_t>(node.dependencyNodeIndex) < m_frameTaskGraphLatest.nodeCount) {
                        ImGui::Text("Depends on: %s",
                            m_frameTaskGraphLatest.nodes[node.dependencyNodeIndex].name);
                    }
                    ImGui::EndTooltip();
                    break;
                }
            }
        }

        ImPlot::EndPlot();
    }

    ImGui::SeparatorText("Stages");
    if (ImGui::BeginTable("##CpuFrameTaskStages", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Stage");
        ImGui::TableSetupColumn("Domain");
        ImGui::TableSetupColumn("Avg Start (ms)");
        ImGui::TableSetupColumn("Avg Span (ms)");
        ImGui::TableSetupColumn("Avg Busy (ms)");
        ImGui::TableSetupColumn("Latest Busy (ms)");
        ImGui::TableSetupColumn("Dispatches");
        ImGui::TableSetupColumn("Min/Max Busy (ms)");
        ImGui::TableSetupColumn("Depends On");
        ImGui::TableHeadersRow();

        for (size_t nodeIndex = 0; nodeIndex < stageAggregates.size(); ++nodeIndex) {
            const auto& aggregate = stageAggregates[nodeIndex];
            if (isWaitForFrame(aggregate.name)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(aggregate.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(domainName(aggregate.domain));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", static_cast<double>(aggregate.avgStartUs) / 1000.0);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", static_cast<double>(aggregate.avgSpanUs) / 1000.0);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f", static_cast<double>(aggregate.avgTotalDurationUs) / 1000.0);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.3f", static_cast<double>(aggregate.latestTotalDurationUs) / 1000.0);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%u / %u", aggregate.avgDispatchCount, aggregate.latestDispatchCount);
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%.3f / %.3f", static_cast<double>(aggregate.minTotalDurationUs) / 1000.0, static_cast<double>(aggregate.maxTotalDurationUs) / 1000.0);
            ImGui::TableSetColumnIndex(8);
            if (aggregate.dependencyNodeIndex >= 0 && static_cast<uint32_t>(aggregate.dependencyNodeIndex) < m_frameTaskGraphLatest.nodeCount) {
                ImGui::TextUnformatted(m_frameTaskGraphLatest.nodes[aggregate.dependencyNodeIndex].name);
            }
            else {
                ImGui::TextDisabled("None");
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

inline void Menu::DrawPassTimingWindow() {
    if (!m_renderGraph) {
        return;
    }

    auto* statisticsService = m_renderGraph->GetStatisticsService();
    if (!statisticsService) {
        return;
    }

    auto& names = statisticsService->GetPassNames();
    auto& techniquePaths = statisticsService->GetPassTechniquePaths();
    auto& stats = statisticsService->GetPassStats();
    auto& meshStats = statisticsService->GetMeshStats();
    auto& isGeom = statisticsService->GetIsGeometryPassVector();
    static int maxStaleFrames = 240;
    static int viewMode = 0;

    if (names.empty()) {
        return;
    }

    ImGui::Begin("Pass Timings");
    ImGui::SliderInt("Max Stale Frames", &maxStaleFrames, 0, 2000);
    constexpr const char* kPassTimingViewNames[] = { "Flat", "Techniques" };
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("View", &viewMode, kPassTimingViewNames, IM_ARRAYSIZE(kPassTimingViewNames));

    const auto& visible = statisticsService->GetVisiblePassIndices(static_cast<uint64_t>(maxStaleFrames));
    if (visible.empty()) {
        ImGui::TextUnformatted("No pass timings within selected staleness window.");
        ImGui::End();
        return;
    }

    static std::vector<bool> pinned;
    if (pinned.size() != names.size()) {
        pinned.assign(names.size(), false);
    }

    enum class PassTimingViewMode : int {
        Flat = 0,
        Techniques = 1,
    };

    enum class PassTimingColumn : ImGuiID {
        Pass = 1,
        Gpu = 2,
        Cpu = 3,
        CpuUpdate = 4,
        CpuExecute = 5,
    };

    auto compareDoubles = [](double lhs, double rhs) {
        if (lhs < rhs) {
            return -1;
        }
        if (lhs > rhs) {
            return 1;
        }
        return 0;
    };

    auto compareNames = [&](int lhs, int rhs) {
        const int cmp = std::strcmp(names[lhs].c_str(), names[rhs].c_str());
        if (cmp < 0) {
            return -1;
        }
        if (cmp > 0) {
            return 1;
        }
        return 0;
    };

    auto compareRows = [&](int lhs, int rhs, const ImGuiTableColumnSortSpecs* sortSpec) {
        const ImGuiID sortColumn = sortSpec != nullptr
            ? sortSpec->ColumnUserID
            : static_cast<ImGuiID>(PassTimingColumn::Gpu);
        const ImGuiSortDirection direction = sortSpec != nullptr
            ? sortSpec->SortDirection
            : ImGuiSortDirection_Descending;

        int result = 0;
        switch (static_cast<PassTimingColumn>(sortColumn)) {
        case PassTimingColumn::Pass:
            result = compareNames(lhs, rhs);
            break;
        case PassTimingColumn::Gpu:
            result = compareDoubles(stats[lhs].gpuTimeEma, stats[rhs].gpuTimeEma);
            break;
        case PassTimingColumn::Cpu:
            result = compareDoubles(stats[lhs].GetCpuTimeEma(), stats[rhs].GetCpuTimeEma());
            break;
        case PassTimingColumn::CpuUpdate:
            result = compareDoubles(stats[lhs].cpuUpdateTimeEma, stats[rhs].cpuUpdateTimeEma);
            break;
        case PassTimingColumn::CpuExecute:
            result = compareDoubles(stats[lhs].cpuExecuteTimeEma, stats[rhs].cpuExecuteTimeEma);
            break;
        default:
            result = compareDoubles(stats[lhs].gpuTimeEma, stats[rhs].gpuTimeEma);
            break;
        }

        if (result == 0) {
            result = compareNames(lhs, rhs);
        }

        return direction == ImGuiSortDirection_Ascending ? (result < 0) : (result > 0);
    };

    std::vector<int> pinnedRows;
    std::vector<int> unpinnedRows;
    pinnedRows.reserve(visible.size());
    unpinnedRows.reserve(visible.size());
    for (unsigned rawIdx : visible) {
        if (rawIdx >= names.size() || rawIdx >= stats.size()) {
            continue;
        }

        const int idx = static_cast<int>(rawIdx);
        if (pinned[idx]) {
            pinnedRows.push_back(idx);
        }
        else {
            unpinnedRows.push_back(idx);
        }
    }

    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("PassTimingsTable", 6, tableFlags)) {
        const bool techniquesView = viewMode == static_cast<int>(PassTimingViewMode::Techniques);
        ImGui::TableSetupColumn(techniquesView ? "Passes" : "Pin", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 0.0f, static_cast<ImGuiID>(PassTimingColumn::Pass));
        ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_DefaultSort, 0.0f, static_cast<ImGuiID>(PassTimingColumn::Gpu));
        ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, static_cast<ImGuiID>(PassTimingColumn::Cpu));
        ImGui::TableSetupColumn("Update (ms)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, static_cast<ImGuiID>(PassTimingColumn::CpuUpdate));
        ImGui::TableSetupColumn("Execute (ms)", ImGuiTableColumnFlags_PreferSortDescending, 0.0f, static_cast<ImGuiID>(PassTimingColumn::CpuExecute));
        ImGui::TableHeadersRow();

        const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        const ImGuiTableColumnSortSpecs* primarySort =
            (sortSpecs != nullptr && sortSpecs->SpecsCount > 0) ? &sortSpecs->Specs[0] : nullptr;

        auto drawRow = [&](int idx) {
            const bool hasMeshDetails = idx < static_cast<int>(isGeom.size()) && idx < static_cast<int>(meshStats.size()) && isGeom[idx];

            ImGui::PushID(idx);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::SmallButton(pinned[idx] ? "Unpin" : "Pin")) {
                pinned[idx] = !pinned[idx];
            }

            ImGui::TableSetColumnIndex(1);
            ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanFullWidth;
            if (!hasMeshDetails) {
                treeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            }
            const bool open = ImGui::TreeNodeEx("PassRow", treeFlags, "%s", names[idx].c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f", stats[idx].gpuTimeEma);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f", stats[idx].GetCpuTimeEma());

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f", stats[idx].cpuUpdateTimeEma);

            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.3f", stats[idx].cpuExecuteTimeEma);

            if (hasMeshDetails && open) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::Indent();
                ImGui::Text("Mesh Invocations: %.0f", meshStats[idx].invocationsEma);
                ImGui::Text("Mesh Primitives:  %.0f", meshStats[idx].primitivesEma);
                ImGui::Unindent();
                ImGui::TreePop();
            }

            ImGui::PopID();
        };

        if (viewMode == static_cast<int>(PassTimingViewMode::Flat)) {
            auto sortRows = [&](std::vector<int>& rows) {
                std::stable_sort(rows.begin(), rows.end(), [&](int lhs, int rhs) {
                    return compareRows(lhs, rhs, primarySort);
                });
            };

            sortRows(pinnedRows);
            sortRows(unpinnedRows);

            for (int idx : pinnedRows) {
                drawRow(idx);
            }
            for (int idx : unpinnedRows) {
                drawRow(idx);
            }
        }
        else {
            struct TechniqueTreeNode {
                std::string label;
                int parentIndex = -1;
                std::vector<int> childNodeIndices;
                std::vector<int> passIndices;
                double gpuTimeMs = 0.0;
                double cpuTimeMs = 0.0;
                double updateTimeMs = 0.0;
                double executeTimeMs = 0.0;
                uint32_t totalPassCount = 0;
            };

            std::vector<TechniqueTreeNode> techniqueNodes;
            techniqueNodes.reserve(visible.size() + 1);
            techniqueNodes.push_back({ "All Techniques", -1 });

            std::unordered_map<std::string, int> techniquePathToNodeIndex;
            techniquePathToNodeIndex.reserve(visible.size() + 1);
            techniquePathToNodeIndex.emplace("", 0);

            auto ensureTechniqueNode = [&](int parentIndex, const std::string& techniquePath, std::string_view label) {
                auto it = techniquePathToNodeIndex.find(techniquePath);
                if (it != techniquePathToNodeIndex.end()) {
                    return it->second;
                }

                const int nodeIndex = static_cast<int>(techniqueNodes.size());
                techniqueNodes.push_back({ std::string(label), parentIndex });
                techniquePathToNodeIndex.emplace(techniquePath, nodeIndex);
                techniqueNodes[parentIndex].childNodeIndices.push_back(nodeIndex);
                return nodeIndex;
            };

            for (unsigned rawIdx : visible) {
                if (rawIdx >= names.size() || rawIdx >= stats.size()) {
                    continue;
                }

                const int passIndex = static_cast<int>(rawIdx);
                std::string techniquePath =
                    rawIdx < techniquePaths.size() && !techniquePaths[rawIdx].empty()
                    ? techniquePaths[rawIdx]
                    : "Ungrouped";

                int currentNodeIndex = 0;
                std::string currentPath;
                size_t segmentStart = 0;
                while (segmentStart <= techniquePath.size()) {
                    const size_t separator = techniquePath.find("::", segmentStart);
                    const size_t segmentEnd = separator == std::string::npos ? techniquePath.size() : separator;
                    const std::string_view segment(techniquePath.data() + segmentStart, segmentEnd - segmentStart);
                    if (!segment.empty()) {
                        if (!currentPath.empty()) {
                            currentPath += "::";
                        }
                        currentPath.append(segment);
                        currentNodeIndex = ensureTechniqueNode(currentNodeIndex, currentPath, segment);
                    }

                    if (separator == std::string::npos) {
                        break;
                    }
                    segmentStart = separator + 2;
                }

                techniqueNodes[currentNodeIndex].passIndices.push_back(passIndex);
                for (int aggregateNodeIndex = currentNodeIndex; aggregateNodeIndex >= 0; aggregateNodeIndex = techniqueNodes[aggregateNodeIndex].parentIndex) {
                    auto& aggregateNode = techniqueNodes[aggregateNodeIndex];
                    aggregateNode.gpuTimeMs += stats[passIndex].gpuTimeEma;
                    aggregateNode.cpuTimeMs += stats[passIndex].GetCpuTimeEma();
                    aggregateNode.updateTimeMs += stats[passIndex].cpuUpdateTimeEma;
                    aggregateNode.executeTimeMs += stats[passIndex].cpuExecuteTimeEma;
                    aggregateNode.totalPassCount += 1;
                }
            }

            auto compareNodeValues = [&](const TechniqueTreeNode& lhs, const TechniqueTreeNode& rhs) {
                int result = 0;
                switch (static_cast<PassTimingColumn>(primarySort != nullptr ? primarySort->ColumnUserID : static_cast<ImGuiID>(PassTimingColumn::Gpu))) {
                case PassTimingColumn::Pass:
                    result = std::strcmp(lhs.label.c_str(), rhs.label.c_str());
                    break;
                case PassTimingColumn::Gpu:
                    result = compareDoubles(lhs.gpuTimeMs, rhs.gpuTimeMs);
                    break;
                case PassTimingColumn::Cpu:
                    result = compareDoubles(lhs.cpuTimeMs, rhs.cpuTimeMs);
                    break;
                case PassTimingColumn::CpuUpdate:
                    result = compareDoubles(lhs.updateTimeMs, rhs.updateTimeMs);
                    break;
                case PassTimingColumn::CpuExecute:
                    result = compareDoubles(lhs.executeTimeMs, rhs.executeTimeMs);
                    break;
                default:
                    result = compareDoubles(lhs.gpuTimeMs, rhs.gpuTimeMs);
                    break;
                }

                if (result == 0) {
                    result = std::strcmp(lhs.label.c_str(), rhs.label.c_str());
                }

                const ImGuiSortDirection direction = primarySort != nullptr
                    ? primarySort->SortDirection
                    : ImGuiSortDirection_Descending;
                return direction == ImGuiSortDirection_Ascending ? (result < 0) : (result > 0);
            };

            auto compareTechniquePasses = [&](int lhs, int rhs) {
                if (pinned[lhs] != pinned[rhs]) {
                    return pinned[lhs] && !pinned[rhs];
                }
                return compareRows(lhs, rhs, primarySort);
            };

            std::function<void(int)> sortTechniqueTree = [&](int nodeIndex) {
                auto& node = techniqueNodes[nodeIndex];
                std::stable_sort(node.childNodeIndices.begin(), node.childNodeIndices.end(), [&](int lhs, int rhs) {
                    return compareNodeValues(techniqueNodes[lhs], techniqueNodes[rhs]);
                });
                std::stable_sort(node.passIndices.begin(), node.passIndices.end(), compareTechniquePasses);
                for (int childNodeIndex : node.childNodeIndices) {
                    sortTechniqueTree(childNodeIndex);
                }
            };
            sortTechniqueTree(0);

            std::function<void(int)> drawTechniqueNode = [&](int nodeIndex) {
                auto& node = techniqueNodes[nodeIndex];
                if (nodeIndex != 0) {
                    ImGui::PushID(nodeIndex + 100000);
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%u", node.totalPassCount);

                    ImGui::TableSetColumnIndex(1);
                    ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanFullWidth;
                    if (node.childNodeIndices.empty() && node.passIndices.empty()) {
                        treeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    }
                    const bool open = ImGui::TreeNodeEx("TechniqueRow", treeFlags, "%s", node.label.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f", node.gpuTimeMs);

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.3f", node.cpuTimeMs);

                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.3f", node.updateTimeMs);

                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.3f", node.executeTimeMs);

                    if (!open) {
                        ImGui::PopID();
                        return;
                    }

                    for (int childNodeIndex : node.childNodeIndices) {
                        drawTechniqueNode(childNodeIndex);
                    }
                    for (int passIndex : node.passIndices) {
                        drawRow(passIndex);
                    }
                    ImGui::TreePop();
                    ImGui::PopID();
                    return;
                }

                for (int childNodeIndex : node.childNodeIndices) {
                    drawTechniqueNode(childNodeIndex);
                }
                for (int passIndex : node.passIndices) {
                    drawRow(passIndex);
                }
            };

            drawTechniqueNode(0);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

inline void Menu::DrawAutoAliasPlannerWindow() {
    if (!ImGui::Begin("Auto Alias Planner", nullptr)) {
        ImGui::End();
        return;
    }

    constexpr const char* kAutoAliasModeNames[] = {
        "Off",
        "Conservative",
        "Balanced",
        "Aggressive"
    };

    int autoAliasModeIndex = static_cast<int>(m_autoAliasMode);
    if (ImGui::Combo("Mode", &autoAliasModeIndex, kAutoAliasModeNames, IM_ARRAYSIZE(kAutoAliasModeNames))) {
        autoAliasModeIndex = std::clamp(autoAliasModeIndex, 0, static_cast<int>(IM_ARRAYSIZE(kAutoAliasModeNames) - 1));
        m_autoAliasMode = static_cast<AutoAliasMode>(autoAliasModeIndex);
        setAutoAliasMode(m_autoAliasMode);
    }

    constexpr const char* kPackingStrategyNames[] = {
        "Greedy Sweep-Line",
        "Beam Search (Near-Optimal)",
    };

    int packingStrategyIndex = static_cast<int>(m_autoAliasPackingStrategy);
    if (ImGui::Combo("Packing Strategy", &packingStrategyIndex, kPackingStrategyNames, IM_ARRAYSIZE(kPackingStrategyNames))) {
        packingStrategyIndex = std::clamp(packingStrategyIndex, 0, static_cast<int>(IM_ARRAYSIZE(kPackingStrategyNames) - 1));
        m_autoAliasPackingStrategy = static_cast<AutoAliasPackingStrategy>(packingStrategyIndex);
        setAutoAliasPackingStrategy(m_autoAliasPackingStrategy);
    }

    if (ImGui::Checkbox("Log Exclusions", &m_autoAliasLogExclusionReasons)) {
        setAutoAliasLogExclusionReasons(m_autoAliasLogExclusionReasons);
    }

    int retireIdleFrames = static_cast<int>(m_autoAliasPoolRetireIdleFrames);
    if (ImGui::SliderInt("Pool Retire Idle Frames", &retireIdleFrames, 0, 2000)) {
        retireIdleFrames = std::max(retireIdleFrames, 0);
        m_autoAliasPoolRetireIdleFrames = static_cast<uint32_t>(retireIdleFrames);
        setAutoAliasPoolRetireIdleFrames(m_autoAliasPoolRetireIdleFrames);
    }

    if (ImGui::SliderFloat("Pool Growth Headroom", &m_autoAliasPoolGrowthHeadroom, 1.0f, 3.0f, "%.2fx")) {
        m_autoAliasPoolGrowthHeadroom = std::max(1.0f, m_autoAliasPoolGrowthHeadroom);
        setAutoAliasPoolGrowthHeadroom(m_autoAliasPoolGrowthHeadroom);
    }

    if (m_renderGraph) {
        ImGui::Separator();
        auto formatBytes = [](uint64_t bytes) {
            constexpr double kKB = 1024.0;
            constexpr double kMB = 1024.0 * 1024.0;
            constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

            const double value = static_cast<double>(bytes);
            if (value >= kGB) {
                return std::format("{:.2f} GB", value / kGB);
            }
            if (value >= kMB) {
                return std::format("{:.2f} MB", value / kMB);
            }
            if (value >= kKB) {
                return std::format("{:.2f} KB", value / kKB);
            }
            return std::format("{:.2f} B", value);
        };

        const auto snapshot = m_renderGraph->GetAutoAliasDebugSnapshot();
        constexpr const char* kModeNames[] = { "Off", "Conservative", "Balanced", "Aggressive" };
        constexpr const char* kStrategyNames[] = { "Greedy Sweep-Line", "Beam Search (Near-Optimal)" };
        const int modeIdx = std::clamp(static_cast<int>(snapshot.mode), 0, static_cast<int>(IM_ARRAYSIZE(kModeNames) - 1));
        const int strategyIdx = std::clamp(static_cast<int>(snapshot.packingStrategy), 0, static_cast<int>(IM_ARRAYSIZE(kStrategyNames) - 1));
        ImGui::Text("Active mode: %s", kModeNames[modeIdx]);
        ImGui::Text("Active strategy: %s", kStrategyNames[strategyIdx]);

        ImGui::Text("Candidates: %llu | Manual: %llu | Auto: %llu | Excluded: %llu",
            static_cast<unsigned long long>(snapshot.candidatesSeen),
            static_cast<unsigned long long>(snapshot.manuallyAssigned),
            static_cast<unsigned long long>(snapshot.autoAssigned),
            static_cast<unsigned long long>(snapshot.excluded));

        ImGui::Text("Candidate MB: %.2f | Auto MB: %.2f",
            static_cast<double>(snapshot.candidateBytes) / (1024.0 * 1024.0),
            static_cast<double>(snapshot.autoAssignedBytes) / (1024.0 * 1024.0));

        const double independentMB = static_cast<double>(snapshot.pooledIndependentBytes) / (1024.0 * 1024.0);
        const double pooledMB = static_cast<double>(snapshot.pooledActualBytes) / (1024.0 * 1024.0);
        const double savedMB = static_cast<double>(snapshot.pooledSavedBytes) / (1024.0 * 1024.0);
        const double savedPct = (snapshot.pooledIndependentBytes > 0)
            ? (100.0 * static_cast<double>(snapshot.pooledSavedBytes) / static_cast<double>(snapshot.pooledIndependentBytes))
            : 0.0;

        ImGui::Text("Pooling memory (alias candidates)");
        ImGui::BulletText("Independent: %.2f MB", independentMB);
        ImGui::BulletText("Pooled: %.2f MB", pooledMB);
        ImGui::BulletText("Saved: %.2f MB (%.1f%%)", savedMB, savedPct);

        if (snapshot.planCacheHits > 0 || snapshot.planCacheMisses > 0 || !snapshot.primaryPlanCacheMissReason.empty()) {
            ImGui::Text("Planner cache");
            ImGui::BulletText(
                "Hits: %llu | Misses: %llu",
                static_cast<unsigned long long>(snapshot.planCacheHits),
                static_cast<unsigned long long>(snapshot.planCacheMisses));
            if (!snapshot.primaryPlanCacheMissReason.empty()) {
                ImGui::BulletText("Primary miss reason: %s", snapshot.primaryPlanCacheMissReason.c_str());
            }
        }

        if (!snapshot.poolDebug.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Pool byte overlap view");

            for (const auto& pool : snapshot.poolDebug) {
                ImGui::PushID(static_cast<int>(pool.poolID & 0x7fffffff));

                const double requiredMB = static_cast<double>(pool.requiredBytes) / (1024.0 * 1024.0);
                const double reservedMB = static_cast<double>(pool.reservedBytes) / (1024.0 * 1024.0);
                const std::string header = std::format(
                    "Pool {} (resources={}, required={:.2f} MB, reserved={:.2f} MB)",
                    static_cast<unsigned long long>(pool.poolID),
                    pool.ranges.size(),
                    requiredMB,
                    reservedMB);

                if (ImGui::TreeNode(header.c_str())) {
                    if (pool.ranges.empty()) {
                        ImGui::TextDisabled("No ranges");
                        ImGui::TreePop();
                        ImGui::PopID();
                        continue;
                    }

                    std::vector<RenderGraph::AutoAliasPoolRangeDebug> ranges = pool.ranges;
                    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
                        if (a.startByte != b.startByte) {
                            return a.startByte < b.startByte;
                        }
                        return a.resourceID < b.resourceID;
                        });

                    uint64_t maxByte = std::max<uint64_t>(1ull, std::max(pool.requiredBytes, pool.reservedBytes));
                    for (const auto& r : ranges) {
                        maxByte = std::max(maxByte, r.endByte);
                    }

                    const float rowHeight = 18.0f;
                    const float plotHeight = std::max(80.0f, rowHeight * static_cast<float>(ranges.size()) + 28.0f);
                    const float labelWidth = 260.0f;
                    ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, plotHeight);
                    if (canvasSize.x < 320.0f) {
                        canvasSize.x = 320.0f;
                    }

                    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##AliasPoolOverlapPlot", canvasSize);
                    ImDrawList* draw = ImGui::GetWindowDrawList();

                    const float left = canvasPos.x;
                    const float top = canvasPos.y;
                    const float right = canvasPos.x + canvasSize.x;
                    const float bottom = canvasPos.y + canvasSize.y;

                    const float plotLeft = left + labelWidth;
                    const float plotRight = right - 10.0f;
                    const float plotWidth = std::max(1.0f, plotRight - plotLeft);
                    const float plotTop = top + 6.0f;

                    draw->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(20, 20, 20, 100));
                    draw->AddRect(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(255, 255, 255, 40));

                    auto toX = [&](uint64_t byteOffset) {
                        const double t = static_cast<double>(byteOffset) / static_cast<double>(maxByte);
                        return plotLeft + static_cast<float>(t) * plotWidth;
                        };

                    draw->AddLine(ImVec2(plotLeft, bottom - 14.0f), ImVec2(plotRight, bottom - 14.0f), IM_COL32(220, 220, 220, 140), 1.0f);
                    draw->AddText(ImVec2(plotLeft, bottom - 13.0f), IM_COL32(220, 220, 220, 200), "0");
                    const std::string maxLabel = std::format("{} B", static_cast<unsigned long long>(maxByte));
                    draw->AddText(ImVec2(plotRight - ImGui::CalcTextSize(maxLabel.c_str()).x, bottom - 13.0f), IM_COL32(220, 220, 220, 200), maxLabel.c_str());

                    if (pool.reservedBytes > 0 && pool.reservedBytes != maxByte) {
                        const float reservedX = toX(pool.reservedBytes);
                        draw->AddLine(ImVec2(reservedX, plotTop), ImVec2(reservedX, bottom - 16.0f), IM_COL32(255, 230, 120, 120), 1.0f);
                    }

                    for (size_t i = 0; i < ranges.size(); ++i) {
                        const auto& r = ranges[i];
                        const float y0 = plotTop + static_cast<float>(i) * rowHeight;
                        const float y1 = y0 + rowHeight - 4.0f;
                        const float x0 = toX(r.startByte);
                        const float x1 = toX(r.endByte);

                        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x0 + 1.0f, x1), y1), IM_COL32(90, 170, 250, 180));
                        draw->AddRect(ImVec2(x0, y0), ImVec2(std::max(x0 + 1.0f, x1), y1), IM_COL32(15, 30, 45, 220));

                        std::vector<std::pair<uint64_t, uint64_t>> overlapSegments;
                        overlapSegments.reserve(ranges.size());
                        for (size_t j = 0; j < ranges.size(); ++j) {
                            if (i == j) {
                                continue;
                            }
                            const auto& other = ranges[j];
                            const uint64_t overlapStart = std::max(r.startByte, other.startByte);
                            const uint64_t overlapEnd = std::min(r.endByte, other.endByte);
                            if (overlapStart < overlapEnd) {
                                overlapSegments.emplace_back(overlapStart, overlapEnd);
                            }
                        }

                        if (!overlapSegments.empty()) {
                            std::sort(overlapSegments.begin(), overlapSegments.end());
                            std::vector<std::pair<uint64_t, uint64_t>> merged;
                            for (const auto& seg : overlapSegments) {
                                if (merged.empty() || seg.first > merged.back().second) {
                                    merged.push_back(seg);
                                }
                                else {
                                    merged.back().second = std::max(merged.back().second, seg.second);
                                }
                            }

                            for (const auto& seg : merged) {
                                const float ox0 = toX(seg.first);
                                const float ox1 = toX(seg.second);
                                draw->AddRectFilled(ImVec2(ox0, y0), ImVec2(std::max(ox0 + 1.0f, ox1), y1), IM_COL32(255, 80, 80, 210));
                            }
                        }

                        const std::string label = std::format(
                            "{} ({})",
                            r.resourceName,
                            formatBytes(r.sizeBytes));
                        draw->AddText(ImVec2(left + 6.0f, y0), IM_COL32(230, 230, 230, 230), label.c_str());
                    }

                    ImGui::TreePop();
                }

                ImGui::PopID();
            }
        }

        if (!snapshot.exclusionReasons.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Top exclusion reasons:");
            const size_t maxReasons = std::min<size_t>(snapshot.exclusionReasons.size(), 8);
            for (size_t i = 0; i < maxReasons; ++i) {
                ImGui::BulletText("%s (%llu)",
                    snapshot.exclusionReasons[i].reason.c_str(),
                    static_cast<unsigned long long>(snapshot.exclusionReasons[i].count));
            }
        }

        if (!snapshot.excludedResources.empty()) {
            ImGui::Separator();
            uint64_t excludedBytes = 0;
            for (const auto& excludedResource : snapshot.excludedResources) {
                excludedBytes += excludedResource.sizeBytes;
            }

            ImGui::Text(
                "Non-aliasable resources: %llu | Total bytes: %s",
                static_cast<unsigned long long>(snapshot.excludedResources.size()),
                formatBytes(excludedBytes).c_str());
            ImGui::TextDisabled("Sorted by memory size (largest first)");

            constexpr ImGuiTableFlags excludedTableFlags =
                ImGuiTableFlags_Borders |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp |
                ImGuiTableFlags_ScrollY;
            const float listHeight = std::min(320.0f, 22.0f * static_cast<float>(snapshot.excludedResources.size()) + 28.0f);
            if (ImGui::BeginTable("##AutoAliasExcludedResources", 3, excludedTableFlags, ImVec2(0.0f, std::max(140.0f, listHeight)))) {
                ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableHeadersRow();
                ImGui::TableSetupScrollFreeze(0, 1);

                for (const auto& excludedResource : snapshot.excludedResources) {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(excludedResource.resourceName.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(formatBytes(excludedResource.sizeBytes).c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", excludedResource.reason.c_str());
                }

                ImGui::EndTable();
            }
        }
    }
    else {
        ImGui::Separator();
        ImGui::TextDisabled("Render graph not available.");
    }

    ImGui::End();
}
