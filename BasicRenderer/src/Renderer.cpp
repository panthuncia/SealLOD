//
// Created by matth on 6/25/2024.
//

#include "Renderer.h"

#define _USE_MATH_DEFINES
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
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/PSOManager.h"
#include "Managers/Singletons/ResourceManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Render/RenderContext.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/TextureImageTableArtifacts.h"
#include "Render/ViewStateArtifacts.h"
#include "Render/PoseStateArtifacts.h"
#include "Render/LightStateArtifacts.h"
#include "Telemetry/NvPerfIntegration.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Render/PassBuilders.h"
#include "Resources/Buffers/DynamicBuffer.h"
#include "RenderPasses/Base/RenderPass.h"
#include "RenderPasses/ForwardRenderPass.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Materials/MaterialTextureStreaming.h"
#include "RenderPasses/SkyboxRenderPass.h"
#include "RenderPasses/EnvironmentFilterPass.h"
#include "RenderPasses/DebugSpheresPass.h"
#include "RenderPasses/DebugSkeletonPass.h"
#include "RenderPasses/Base/ComputePass.h"
#include "RenderPasses/Base/CopyPass.h"
#include "RenderPasses/FidelityFX/Downsample.h"
#include "RenderPasses/PostProcessing/Tonemapping.h"
#include "RenderPasses/PostProcessing/Upscaling.h"
#include "RenderPasses/PostProcessing/DilateMotionVectorsPass.h"
#include "RenderPasses/PostProcessing/luminanceHistogram.h"
#include "RenderPasses/PostProcessing/luminanceHistogramAverage.h"
#include "RenderPasses/ClearVisibilityBufferPass.h"
#include "RenderPasses/PostProcessing/DebugResolvePass.h"
#include "RenderPasses/MenuRenderPass.h"
#include "RenderPasses/PresentPass.h"
#include "RenderPasses/PrimaryCameraUploadPass.h"
#include "Resources/TextureDescription.h"
#include "Menu/Menu.h"
#include "Managers/Singletons/DeletionManager.h"
#include "Managers/Singletons/DescriptorHeapManager.h"
#include "Managers/Singletons/CommandSignatureManager.h"
#include "Managers/Singletons/RendererECSManager.h"
#include "Managers/IndirectCommandBufferManager.h"
#include "Managers/ObjectManager.h"
#include "Utilities/MathUtils.h"
#include "Scene/MovementState.h"
#include "ThirdParty/XeGTAO.h"
#include "Managers/EnvironmentManager.h"
#if BASICRENDERER_HAS_INTEROP_VALIDATION
#include "Validation/SARPInteropValidation.h"
#endif
#include "Render/TonemapTypes.h"
#include "../generated/BuiltinResources.h"
#include "Resources/ResourceIdentifier.h"
#include "Render/RenderGraphBuildHelper.h"
#include "Managers/Singletons/UpscalingManager.h"
#include "Managers/Singletons/FFXManager.h"
#include "Managers/Singletons/DirectStorageManager.h"
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include "Render/OutputTypes.h"
#include "Render/GraphExtensions/IOExtension.h"
#include "Render/GraphExtensions/CLodExtension.h"
#include "Render/GraphExtensions/VirtualShadowCasterProvider.h"
#include "Render/GraphExtensions/CLodExtensionComponents.h"
#include "Render/GraphExtensions/ClusterLOD/CLodStreamingSystem.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "RenderPasses/DebugGridPass.h"
#include "Render/GraphExtensions/ReadbackCaptureExtension.h"
#include "Render/Runtime/IReadbackService.h"
#include "Render/Runtime/ScopedActiveGraphServices.h"
#include "Resources/Resource.h"
#include "Resources/components.h"
#include "Resources/ReadbackRequest.h"
#include "Resources/DynamicResource.h"
#include "Resources/ExternalTextureResource.h"
#include "Render/MemoryIntrospectionBackend.h"
#include "Render/Runtime/UploadPolicyServiceAccess.h"
#include "Render/TbbTaskService.h"
#include "Mesh/MeshInstance.h"
#include "Render/DrawWorkload.h"
#include "Render/IndirectStateArtifacts.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/TextureBindingArtifacts.h"
#include "Render/TerrainStateArtifacts.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/StaticStateArtifacts.h"
#include "Render/GeometryResidencyStateArtifacts.h"
#include "Render/GeometryBufferStateArtifacts.h"
#include "Render/ObjectBufferStateArtifacts.h"
#include "Render/RasterBucketFlags.h"
#include "Render/TerrainRvtTelemetry.h"

void D3D12DebugCallback(
    D3D12_MESSAGE_CATEGORY Category,
    D3D12_MESSAGE_SEVERITY Severity,
    D3D12_MESSAGE_ID ID,
    LPCSTR pDescription,
    void* pContext) {
    std::string message(pDescription);

    // Redirect messages to spdlog based on severity
    switch (Severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        spdlog::critical("D3D12 CORRUPTION: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_ERROR:
        spdlog::error("D3D12 ERROR: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        spdlog::warn("D3D12 WARNING: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_INFO:
        spdlog::info("D3D12 INFO: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
        spdlog::debug("D3D12 MESSAGE: {}", message);
        break;
    }
}

namespace {

constexpr const char* CLodVisibilityTelemetryDebugSettingName = "clodVisibilityTelemetryDebug";
constexpr const char* CLodVirtualShadowTelemetryDebugSettingName = "clodVirtualShadowTelemetryDebug";
constexpr const char* ObjectReyesAtlasTelemetryDebugSettingName = "objectReyesAtlasTelemetryDebug";
constexpr size_t TerrainRvtTelemetryMipBins = 16u;

br::render::SceneIngestionConfiguration CaptureSceneIngestionConfiguration()
{
    auto& settings = SettingsManager::GetInstance();
    br::render::SceneIngestionConfiguration result{};
    result.renderResolution = settings.getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    result.outputResolution = settings.getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    result.shadowResolution = settings.getSettingGetter<uint16_t>("shadowResolution")();
    result.directionalCascadeCount = settings.getSettingGetter<uint8_t>("numDirectionalLightCascades")();
    result.maxShadowDistance = settings.getSettingGetter<float>("maxShadowDistance")();
    result.directionalShadowDistanceLowerBound =
        settings.getSettingGetter<float>("directionalShadowDistanceLowerBound")();
    result.directionalShadowSceneExtent =
        settings.getSettingGetter<float>("directionalShadowSceneExtent")();
    result.primaryCameraUsesOutputHeight =
        settings.getSettingGetter<CLodLodHeightMode>(CLodLodHeightModeSettingName)() ==
        CLodLodHeightMode::OutputHeight;
    return result;
}

uint32_t PrimaryCameraLodHeight(const br::render::SceneIngestionConfiguration& configuration)
{
    return configuration.primaryCameraUsesOutputHeight && configuration.outputResolution.y != 0u
        ? configuration.outputResolution.y : configuration.renderResolution.y;
}

bool OutputTypeRequiresRenderGraphRebuild(unsigned int outputType)
{
    return outputType == static_cast<unsigned int>(OutputType::SKELETONS);
}

bool ReadTruthyEnvironmentFlag(const char* name)
{
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return false;
    }

    const bool result =
        std::strcmp(value, "1") == 0 ||
        _stricmp(value, "true") == 0 ||
        _stricmp(value, "yes") == 0 ||
        _stricmp(value, "on") == 0;
    std::free(value);
    return result;
}

uint32_t ReadBoundedEnvironmentUint(
    const char* name,
    uint32_t fallback,
    uint32_t minimum,
    uint32_t maximum)
{
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    const bool valid = end != value && *end == '\0';
    std::free(value);
    if (!valid) {
        return fallback;
    }
    if (parsed < minimum) {
        return minimum;
    }
    if (parsed > maximum) {
        return maximum;
    }
    return static_cast<uint32_t>(parsed);
}

struct TerrainRvtTelemetryStatsReadback {
    uint32_t heightRequests;
    uint32_t materialRequests;
    uint32_t requestOverflows;
    uint32_t generatedPages;
    uint32_t allocationFailures;
    uint32_t heightFallbacks;
    uint32_t materialFallbacks;
    uint32_t residentHits;
    uint32_t heightSampleAttempts;
    uint32_t materialSampleAttempts;
    uint32_t heightSampleHits;
    uint32_t materialSampleHits;
    uint32_t heightPageTableMisses;
    uint32_t materialPageTableMisses;
    uint32_t heightComputePageFailures;
    uint32_t materialComputePageFailures;
    uint32_t heightDisabledFallbacks;
    uint32_t materialDisabledFallbacks;
    uint32_t heightForcedFallbacks;
    uint32_t materialForcedFallbacks;
    uint32_t markComputePageFailures;
    uint32_t markWorldRectCalls;
    uint32_t markWorldRectPages;
    uint32_t resolveResidentPages;
    uint32_t generationHeightPages;
    uint32_t generationMaterialPages;
    uint32_t generationCombinedPages;
    uint32_t generationTexels;
    uint32_t materialSampleRequestedPageXor;
    uint32_t materialSampleResidentPageXor;
    uint32_t materialSamplePhysicalPageXor;
    uint32_t materialSampleRequestedPageMin;
    uint32_t materialSampleRequestedPageMax;
    uint32_t materialSampleResidentPageMin;
    uint32_t materialSampleResidentPageMax;
    uint32_t materialSamplePhysicalPageMin;
    uint32_t materialSamplePhysicalPageMax;
    uint32_t materialSampleCoarserResidentHits;
    uint32_t materialSampleAtlasPoolMask;
    uint32_t heightOwnerMismatches;
    uint32_t materialOwnerMismatches;
    uint32_t requestPageTableXor;
    uint32_t requestPageTableMin;
    uint32_t requestPageTableMax;
    uint32_t generationPageTableMin;
    uint32_t generationPageTableMax;
    uint32_t materialSampleAttemptedPageXor;
    uint32_t materialSampleAttemptedPageMin;
    uint32_t materialSampleAttemptedPageMax;
    uint32_t materialSamplePageMissRequestedPageXor;
    uint32_t materialSamplePageMissRequestedPageMin;
    uint32_t materialSamplePageMissRequestedPageMax;
    uint32_t heightSampleAttemptedPageXor;
    uint32_t heightSampleAttemptedPageMin;
    uint32_t heightSampleAttemptedPageMax;
    uint32_t heightSamplePageMissRequestedPageXor;
    uint32_t heightSamplePageMissRequestedPageMin;
    uint32_t heightSamplePageMissRequestedPageMax;
    uint32_t heightFastSampleAttempts;
    uint32_t heightFastSampleHits;
    uint32_t heightFastPageMissRequests;
    uint32_t heightFullSampleAttempts;
    uint32_t heightFullSampleHits;
    uint32_t generationPageTableXor;
    uint32_t generationPhysicalPageXor;
    uint32_t physicalPageOwnerCollisions;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> heightRequestMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> materialRequestMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> heightSampleMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> materialSampleMipHistogram;
    std::array<uint32_t, TerrainRvtTelemetryMipBins> generationMipHistogram;
};

static_assert(sizeof(TerrainRvtTelemetryStatsReadback) == sizeof(uint32_t) * (66u + TerrainRvtTelemetryMipBins * 5u));

std::string FormatTerrainRvtMipHistogram(const std::array<uint32_t, TerrainRvtTelemetryMipBins>& histogram)
{
    std::ostringstream output;
    bool any = false;
    for (size_t i = 0; i < histogram.size(); ++i) {
        if (histogram[i] == 0u) {
            continue;
        }
        if (any) {
            output << ",";
        }
        output << i << ":" << histogram[i];
        any = true;
    }
    return any ? output.str() : "none";
}

std::string RendererExceptionStacktraceString()
{
#if defined(__cpp_lib_stacktrace) && (__cpp_lib_stacktrace >= 202011L)
    try {
        std::ostringstream output;
        output << std::stacktrace::current();
        return output.str();
    } catch (...) {
        return "(stacktrace capture failed)";
    }
#else
    return "(no <stacktrace> support in this build)";
#endif
}

std::filesystem::path MakeRendererExceptionPath(uint64_t frameNumber, const char* stageName)
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    (void)localtime_s(std::addressof(localTime), std::addressof(time));

    std::ostringstream name;
    name << "RendererException-"
         << std::put_time(std::addressof(localTime), "%Y%m%d-%H%M%S")
         << "-frame" << frameNumber
         << "-" << stageName
         << "-tid" << GetCurrentThreadId()
         << ".txt";

    std::error_code ec;
    std::filesystem::create_directories("crashes", ec);
    return std::filesystem::current_path() / "crashes" / name.str();
}

void WriteRendererExceptionNote(
    const char* stageName,
    uint64_t frameNumber,
    uint8_t frameIndex,
    uint64_t frameFenceValue,
    const std::exception& ex)
{
    const auto path = MakeRendererExceptionPath(frameNumber, stageName);
    std::ofstream report(path, std::ios::trunc);
    if (!report) {
        return;
    }

    std::ostringstream threadId;
    threadId << std::this_thread::get_id();
    report << "Renderer exception note\n";
    report << "stage='" << stageName << "'\n";
    report << "frame=" << frameNumber << "\n";
    report << "frame_index=" << static_cast<unsigned>(frameIndex) << "\n";
    report << "frame_fence_value=" << frameFenceValue << "\n";
    report << "thread_id=" << threadId.str() << "\n";
    report << "win32_thread_id=" << GetCurrentThreadId() << "\n";
    report << "exception_type='" << typeid(ex).name() << "'\n";
    report << "exception_what='" << ex.what() << "'\n\n";
    report << "Catch-site stacktrace:\n" << RendererExceptionStacktraceString() << "\n";
}

void SyncOpenRenderGraphSettings(uint8_t numFramesInFlight) {
    auto& sm = SettingsManager::GetInstance();
    org::runtime::OpenRenderGraphSettings orgSettings{};
    orgSettings.numFramesInFlight = numFramesInFlight;
    orgSettings.collectPassStatistics = sm.getSettingGetter<bool>("collectPassStatistics")();
    orgSettings.collectPipelineStatistics = sm.getSettingGetter<bool>("collectPipelineStatistics")();
    orgSettings.useAsyncCompute = sm.getSettingGetter<bool>("useAsyncCompute")();
    const auto requestedCompileMode = sm.getSettingGetter<int>("experimentalAsyncCompileMode")();
    orgSettings.experimentalAsyncCompileMode = requestedCompileMode == 2
        ? org::runtime::AsyncCompileMode::Async : org::runtime::AsyncCompileMode::Off;
    orgSettings.experimentalCompileConcurrency = static_cast<uint8_t>(std::clamp(
        sm.getSettingGetter<int>("experimentalCompileConcurrency")(), 1, 4));
    orgSettings.renderGraphCompileDumpEnabled = sm.getSettingGetter<bool>("renderGraphCompileDumpEnabled")();
    orgSettings.renderGraphVramDumpEnabled = sm.getSettingGetter<bool>("renderGraphVramDumpEnabled")();
    orgSettings.renderGraphBatchTraceEnabled = sm.getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    orgSettings.renderGraphLightweightCompileSummaryEnabled = sm.getSettingGetter<bool>("renderGraphLightweightCompileSummaryEnabled")();
    orgSettings.readOnlyUniformTransitionElisionEnabled = true;
    orgSettings.autoAliasMode = static_cast<uint8_t>(sm.getSettingGetter<AutoAliasMode>("autoAliasMode")());
    orgSettings.autoAliasPackingStrategy = static_cast<uint8_t>(sm.getSettingGetter<AutoAliasPackingStrategy>("autoAliasPackingStrategy")());
    orgSettings.autoAliasEnableLogging = sm.getSettingGetter<bool>("autoAliasEnableLogging")();
    orgSettings.autoAliasLogExclusionReasons = sm.getSettingGetter<bool>("autoAliasLogExclusionReasons")();
    orgSettings.autoAliasBuildDebugData = sm.getSettingGetter<bool>("autoAliasBuildDebugData")();
    orgSettings.queueSchedulingEnableLogging = sm.getSettingGetter<bool>("queueSchedulingEnableLogging")();
    orgSettings.queueSchedulingSelectionPolicy = static_cast<org::runtime::QueueSchedulingSelectionPolicy>(sm.getSettingGetter<uint8_t>("queueSchedulingSelectionPolicy")());
    orgSettings.queueSchedulingWidthScale = sm.getSettingGetter<float>("queueSchedulingWidthScale")();
    orgSettings.queueSchedulingPenaltyBias = sm.getSettingGetter<float>("queueSchedulingPenaltyBias")();
    orgSettings.queueSchedulingMinPenalty = sm.getSettingGetter<float>("queueSchedulingMinPenalty")();
    orgSettings.queueSchedulingResourcePressureWeight = sm.getSettingGetter<float>("queueSchedulingResourcePressureWeight")();
    orgSettings.queueSchedulingUavPressureWeight = sm.getSettingGetter<float>("queueSchedulingUavPressureWeight")();
    orgSettings.queueSchedulingAutoGraphicsBias = sm.getSettingGetter<float>("queueSchedulingAutoGraphicsBias")();
    orgSettings.queueSchedulingAsyncOverlapBonus = sm.getSettingGetter<float>("queueSchedulingAsyncOverlapBonus")();
    orgSettings.queueSchedulingCrossQueueHandoffPenalty = sm.getSettingGetter<float>("queueSchedulingCrossQueueHandoffPenalty")();
    orgSettings.autoAliasPoolRetireIdleFrames = sm.getSettingGetter<uint32_t>("autoAliasPoolRetireIdleFrames")();
    orgSettings.autoAliasPoolGrowthHeadroom = sm.getSettingGetter<float>("autoAliasPoolGrowthHeadroom")();
    orgSettings.transitionPlacementMode = static_cast<org::runtime::TransitionPlacementMode>(sm.getSettingGetter<uint8_t>("transitionPlacementMode")());
    orgSettings.heavyDebug = sm.getSettingGetter<bool>("heavyDebug")();
    org::runtime::SetOpenRenderGraphSettings(orgSettings);
}

bool IsStreamlineDisabledByEnvironment() {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, "BASICRENDERER_DISABLE_STREAMLINE") != 0 || value == nullptr) {
        return false;
    }
    const bool disabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
    free(value);
    return disabled;
}

bool IsDirectStorageDisabledByEnvironment() {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, "BASICRENDERER_DISABLE_DIRECTSTORAGE") != 0 || value == nullptr) {
        return false;
    }
    const bool disabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
    free(value);
    return disabled;
}

bool IsCLodVisibilityTelemetryEnabledByEnvironment() {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, "SARP_CLOD_VISIBILITY_TELEMETRY") != 0 || value == nullptr) {
        return false;
    }
    const bool enabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
    free(value);
    return enabled;
}

bool IsCLodVisibilityTelemetryDebugEnabled() {
	return IsCLodVisibilityTelemetryEnabledByEnvironment() ||
		SettingsManager::GetInstance().getSettingGetter<bool>(CLodVisibilityTelemetryDebugSettingName)();
}

uint32_t ReadUintEnvironmentValue(const char* name, uint32_t fallback)
{
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    const bool valid = end != value && *end == '\0' && parsed <= UINT32_MAX;
    free(value);
    return valid ? static_cast<uint32_t>(parsed) : fallback;
}

bool IsCLodVirtualShadowTelemetryDebugEnabled()
{
    return ReadTruthyEnvironmentFlag("SARP_CLOD_VSM_TELEMETRY") ||
        SettingsManager::GetInstance().getSettingGetter<bool>(
            CLodVirtualShadowTelemetryDebugSettingName)();
}

bool DefaultEnableReShapeForBuild() {
#if BASICRHI_ENABLE_RESHAPE
    return true;
#else
    return false;
#endif
}

void ProbeGraphicsCommandListCreation(rhi::Device device, std::string_view phase) {
    (void)device;
    (void)phase;
}

flecs::entity FindSceneEntityByStableSceneID(flecs::entity node, uint64_t stableSceneID) {
    if (!node.is_alive()) {
        return {};
    }

    if (const auto* currentStableSceneID = node.try_get<Components::StableSceneID>()) {
        if (currentStableSceneID->value == stableSceneID) {
            return node;
        }
    }

    flecs::entity found;
    node.children([&](flecs::entity child) {
        if (found.is_alive()) {
            return;
        }

        auto candidate = FindSceneEntityByStableSceneID(child, stableSceneID);
        if (candidate.is_alive()) {
            found = candidate;
        }
    });
    return found;
}

} // namespace

Renderer::~Renderer() {
    // Resources unregister from this process-global service during member
    // destruction. Clear the slot before the service member itself is
    // destroyed so exception unwinding cannot call through a dangling pointer.
    org::runtime::SetActiveUploadPolicyService(nullptr);
}

void Renderer::StartAsyncStateGraphTrace(br::render::AsyncStateGraphTraceConfig config) {
    m_pendingAsyncStateGraphTrace = config;
    if (m_asyncStateGraph) m_asyncStateGraph->StartTrace(config);
}

bool Renderer::AsyncStateGraphTraceActive() const {
    return m_asyncStateGraph && m_asyncStateGraph->TraceActive();
}

br::render::AsyncStateGraphTraceReport Renderer::StopAsyncStateGraphTraceAndWriteReport(
    const std::filesystem::path& outputDirectory) {
    m_pendingAsyncStateGraphTrace.reset();
    return m_asyncStateGraph
        ? m_asyncStateGraph->StopTraceAndWriteReport(outputDirectory)
        : br::render::AsyncStateGraphTraceReport{};
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
    BufferBase::ScopedBackingMutation initializationBackingMutation;
    m_hwnd = hwnd;

    auto& settingsManager = SettingsManager::GetInstance();
    const bool enableStreamline = !IsStreamlineDisabledByEnvironment();
    const bool enableDirectStorage = !IsDirectStorageDisabledByEnvironment();
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
    settingsManager.registerSetting<bool>("enableReShape", DefaultEnableReShapeForBuild());
    settingsManager.registerSetting<bool>("reshapeSynchronousRecording", false);
    settingsManager.registerSetting<bool>("reshapeTexelAddressing", true);
    settingsManager.registerSetting<uint64_t>("reshapeGlobalFeatureMask", 0ull);
    settingsManager.registerSetting<bool>(
        "renderGraphBatchTraceEnabled",
        ReadTruthyEnvironmentFlag("BASICRENDERER_RENDER_GRAPH_BATCH_TRACE"));
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
    TrackedEntityToken::Hooks trackedEntityHooks{};
    trackedEntityHooks.createEntity = [](flecs::entity existing) {
            auto& ecsManager = RendererECSManager::GetInstance();
            if (!ecsManager.IsAlive()) {
                return TrackedEntityToken{};
            }

            TrackedEntityToken token = TrackedEntityToken::CreateDeferred();
            const flecs::entity_t existingId = existing.id();
            auto deferredState = token.deferredState;
            ecsManager.EnqueueDeferredWorldOperation([deferredState, existingId](flecs::world& world) mutable {
                std::vector<std::function<void(flecs::entity)>> pendingOps;
                bool destroyRequested = false;
                flecs::entity entity = existingId != 0 ? flecs::entity{ world, existingId } : flecs::entity{};
                if (!entity.is_alive()) {
                    entity = world.entity();
                }

                if (!TrackedEntityToken::ResolveDeferredState(
                    deferredState,
                    world,
                    entity.id(),
                    pendingOps,
                    destroyRequested)) {
                    if (entity.is_alive()) {
                        entity.destruct();
                    }
                    TrackedEntityToken::MarkDeferredStateDestroyed(deferredState);
                    return;
                }

                for (auto& op : pendingOps) {
                    op(entity);
                }

                if (destroyRequested && entity.is_alive()) {
                    entity.destruct();
                    TrackedEntityToken::MarkDeferredStateDestroyed(deferredState);
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
    trackedEntityHooks.enqueueAttachBundle = [](flecs::entity_t id, EntityComponentBundle bundle) {
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
    TrackedEntityToken::SetHooks(std::move(trackedEntityHooks));

    Resource::ECSEntityHooks resourceEntityHooks{};
    resourceEntityHooks.createEntity = []() -> Resource::ECSEntityHandle {
		auto& ecsManager = RendererECSManager::GetInstance();
		if (!ecsManager.IsAlive()) {
			return {};
		}

		if (!ecsManager.IsMainThread()) {
			auto handle = Resource::ECSEntityHandle::CreateDeferred();
                auto deferredState = handle.deferredState;
                ecsManager.EnqueueDeferredWorldOperation([deferredState](flecs::world& world) mutable {
					flecs::entity entity = world.entity();
                    Resource::ECSEntityHandle deferredHandle;
                    deferredHandle.deferredState = deferredState;
                    if (!deferredHandle.Resolve(world, entity.id())) {
                        deferredHandle.MarkDestroyed();
					}
				});
                return std::move(handle);
        }

        auto& world = RendererECSManager::GetInstance().GetWorld();
        auto entity = world.entity();
        Resource::ECSEntityHandle handle{};
        handle.world = &world;
        handle.id = entity.id();
        return handle;
    };
    resourceEntityHooks.destroyEntity = [](const Resource::ECSEntityHandle& handle) {
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
                Resource::ECSEntityHandle deferredHandle;
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
    Resource::SetEntityHooks(std::move(resourceEntityHooks));

    if (!currentRenderGraph) {
		currentRenderGraph = std::make_unique<RenderGraph>(DeviceManager::GetInstance().GetDevice(), DeviceManager::GetInstance().GetBackend());
		if (DeviceManager::GetInstance().IsMultiRHIEnabled()) {
			currentRenderGraph->RegisterBackendDevice(DeviceManager::GetInstance().GetPeerBackend(), DeviceManager::GetInstance().GetPeerDevice());
		}
    }

    if (auto* uploadService = currentRenderGraph->GetUploadService()) {
            uploadService->Initialize();
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
    if (m_pendingAsyncStateGraphTrace) {
        m_asyncStateGraph->StartTrace(*m_pendingAsyncStateGraphTrace);
    }
    m_rendererStatePublisher = std::make_unique<br::render::RendererStatePublisher>(m_numFramesInFlight);
    br::render::PublishedStateSource::SetProcessSource(m_rendererStatePublisher->ResourceSource());
    m_rendererStateRequests = std::make_unique<br::render::RendererStateRequestService>(
        *m_asyncStateGraph, *m_rendererStatePublisher);
    br::render::RegisterIndirectStateProducer(*m_asyncStateGraph);
    br::render::RegisterMaterialStateProducer(*m_asyncStateGraph);
	br::render::RegisterTextureBindingProducer(*m_asyncStateGraph);
	br::render::RegisterTerrainStateProducer(*m_asyncStateGraph);
    br::render::RegisterVersionedGpuBufferProducer(*m_asyncStateGraph);
	br::render::RegisterTextureImageTableProducer(*m_asyncStateGraph);
    br::render::RegisterObjectBufferStateProducer(*m_asyncStateGraph);
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
    SetAsyncBufferBackingResizeScheduler([](std::string taskName, std::function<void()>&& task) {
        return TaskSchedulerManager::GetInstance().Submit(
            TaskLane::Background, TaskDomain::Cleanup, taskName, std::move(task));
    });
    currentRenderGraph->SetTaskService(std::make_shared<br::TbbTaskService>());
    spdlog::info("Renderer initialization: initializing PSO manager");
    PSOManager::GetInstance().initialize();
    spdlog::info("Renderer initialization: initializing deletion manager");
    DeletionManager::GetInstance().Initialize();
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
	m_pIndirectCommandBufferManager = IndirectCommandBufferManager::CreateUnique();
	m_pViewManager = ViewManager::CreateUnique();
	m_pEnvironmentManager = EnvironmentManager::CreateUnique(currentRenderGraph->RetainUploadService());
	m_pEnvironmentManager->SetWorkServices(m_environmentWorkServices);
    CreateDefaultEnvironmentResources();
    m_pEnvironmentManager->SetRequestReadbackFn([this](std::shared_ptr<PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback, bool cubemap) {
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
        [this](std::shared_ptr<PixelBuffer> texture, std::wstring outputFile, std::function<void()> callback) {
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

void Renderer::RunGameUpdateStage(float elapsedSeconds) {
    BT_ZONE_SCOPE("Renderer::Update::GameUpdate");
    currentScene->Update(elapsedSeconds);
}

void Renderer::RunAnimationUpdateStage(float elapsedSeconds) {
    BT_ZONE_SCOPE("Renderer::Update::AnimationUpdate");
    m_pSkeletonManager->TickAnimations(elapsedSeconds);
    m_pSkeletonManager->UpdateAllDirtyInstances();
}

void Renderer::RunTransformPropagationStage() {
    BT_ZONE_SCOPE("Renderer::Update::TransformPropagation");
    currentScene->PropagateTransforms();
}

void Renderer::RunSceneBridgeSyncStage() {
    BT_ZONE_SCOPE("Renderer::Update::SceneBridgeSync");
    if (!currentScene) {
        return;
    }
    m_sceneRenderBridge.Sync(*currentScene, GetSceneIngestionServices(),
        CaptureSceneIngestionConfiguration());
}

void Renderer::SetExternalSceneMode(bool enabled) {
    if (m_externalSceneMode == enabled) {
        return;
    }

    m_externalSceneMode = enabled;
    if (enabled) {
        SetSceneRenderOverlapEnabled(false);
        InvalidateSceneOverlapState();
        m_warnedNullScene = false;
        m_warnedMissingPrimaryCamera = false;
    }
}

void Renderer::IngestExternalSnapshot(const br::render::SceneFrameSnapshot& snapshot) {
    SetExternalSceneMode(true);
    RegisterExternalSnapshotMeshes(snapshot);
    m_sceneRenderBridge.IngestSnapshot(snapshot, GetSceneIngestionServices(),
        CaptureSceneIngestionConfiguration());
    m_hasCommittedSceneSnapshot = true;
    m_lastCommittedSceneSnapshotSequence = snapshot.snapshotSequence;
    m_lastCommittedSceneSourceFrame = snapshot.sourceFrameNumber;
}

ObjectManager::Stats Renderer::GetObjectManagerStats() const {
    return m_pObjectManager ? m_pObjectManager->GetStats() : ObjectManager::Stats{};
}

Renderer::SamplingReadinessSnapshot Renderer::GetSamplingReadinessSnapshot(bool includeExpensiveDiagnostics) const {
    SamplingReadinessSnapshot snapshot;
    const auto sceneStatus = GetSceneOverlapStatus();
    snapshot.sceneTaskInFlight = sceneStatus.taskInFlight;
    snapshot.hasCommittedSceneSnapshot = sceneStatus.hasCommittedSnapshot;
    snapshot.committedSceneSnapshotSequence = sceneStatus.committedSnapshotSequence;
    snapshot.pendingSceneSnapshotSequence = sceneStatus.pendingSnapshotSequence;

    if (m_pMaterialManager && !includeExpensiveDiagnostics) {
        const auto textureStats = m_pMaterialManager->GetMaterialTextureStreamingReadinessStats();
        snapshot.pendingTextureReloads = textureStats.pendingReloadTextureCount;
        snapshot.fullResolutionTextures = textureStats.fullResolutionResidentTextureCount;
    }
    else if (m_pMaterialManager) {
        const auto textureStats = m_pMaterialManager->GetMaterialTextureStreamingStats();
        snapshot.pendingTextureReloads = textureStats.pendingReloadTextureCount;
        snapshot.fullResolutionTextures = textureStats.fullResolutionResidentTextureCount;
        snapshot.materialTextures = textureStats.uniqueMaterialTextureCount;
        snapshot.streamableMaterialTextures = textureStats.uniqueStreamableTextureCount;
        snapshot.streamingEnabledMaterialTextures = textureStats.uniqueStreamingEnabledTextureCount;
        snapshot.streamableFullResolutionTextures = textureStats.streamableFullResolutionResidentTextureCount;
        snapshot.materialTextureResidentBytes = textureStats.totalResidentBytes;
        snapshot.streamableMaterialTextureResidentBytes = textureStats.streamableResidentBytes;
        snapshot.materialTextureResidentTopMipHistogram = textureStats.residentTopMipHistogram;
        snapshot.materialTextureRequestedTopMipHistogram = textureStats.requestedTopMipHistogram;
        snapshot.materialTextureFeedbackTopMipHistogram = textureStats.feedbackTopMipHistogram;
        snapshot.materialTexturesWithoutFeedback = textureStats.texturesWithoutFeedback;
        snapshot.materialTextureResidentBytesByTopMip = textureStats.residentBytesByTopMip;
        snapshot.materialTextureResidentShapeMismatchCount = textureStats.residentShapeMismatchTextureCount;
        snapshot.materialTextureResidentShapeMismatchBytes = textureStats.residentShapeMismatchBytes;
        snapshot.materialTextureDistinctPreparedCount = textureStats.distinctPreparedTextureCount;
        snapshot.materialTextureDistinctPreparedBytes = textureStats.distinctPreparedTextureBytes;
        snapshot.activeMaterialTextureResourceCount = textureStats.activeMaterialResourceCount;
        snapshot.activeMaterialTextureResourceBytes = textureStats.activeMaterialResourceBytes;
        snapshot.externallyManagedActiveTextureResourceCount = textureStats.externallyManagedActiveResourceCount;
        snapshot.externallyManagedActiveTextureResourceBytes = textureStats.externallyManagedActiveResourceBytes;
		snapshot.graphManagedParticipatingActiveTextureResourceCount = textureStats.graphManagedParticipatingActiveResourceCount;
		snapshot.graphManagedParticipatingActiveTextureResourceBytes = textureStats.graphManagedParticipatingActiveResourceBytes;
        snapshot.alphaTestedMaterialTextureCount = textureStats.alphaTestedTextureCount;
        snapshot.alphaTestedMaterialTextureMipCapViolationCount = textureStats.alphaTestedMipCapViolationCount;
        snapshot.materialTexturePublishedResourceIDs = textureStats.publishedResourceIDs;
        snapshot.largestMaterialTextureRecords.reserve(textureStats.largestResidentTextures.size());
        for (const auto& record : textureStats.largestResidentTextures) {
            std::ostringstream stream;
            stream
                << "streaming_id=" << record.streamingTextureID
                << " descriptor=" << record.imageDescriptorIndex
                << " resource=" << record.imageResourceID
                << " bytes=" << record.residentBytes
                << " resident_dimensions=" << record.residentWidth << "x" << record.residentHeight
                << " expected_resident_dimensions=" << record.expectedResidentWidth << "x" << record.expectedResidentHeight
                << " total_mips=" << record.totalMipCount
                << " resident_top_mip=" << record.residentTopMip
                << " resident_mip_count=" << record.residentMipCount
                << " requested_top_mip=" << record.requestedTopMip
                << " feedback_top_mip=";
            if (record.feedbackTopMip == UINT32_MAX) {
                stream << "none";
            }
            else {
                stream << record.feedbackTopMip;
            }
            stream
                << " eligible=" << (record.eligible ? 1 : 0)
                << " enabled=" << (record.enabled ? 1 : 0)
                << " alpha_tested=" << (record.alphaTested ? 1 : 0)
                << " identifier=\"" << record.identifier << "\"";
            snapshot.largestMaterialTextureRecords.push_back(stream.str());
        }
    }
    if (m_pMeshManager) {
        const auto clodStats = m_pMeshManager->GetCLodStreamingDebugStats();
        snapshot.residentClodGroups = clodStats.residentGroups;
        snapshot.residentClodAllocations = clodStats.residentAllocations;
        snapshot.residentClodAllocationBytes = clodStats.residentAllocationBytes;
        snapshot.totalClodStreamedBytes = clodStats.totalStreamedBytes;
        snapshot.queuedClodRequests = clodStats.queuedRequests;
        snapshot.inFlightClodGroups = clodStats.queuedOrInFlightGroups + clodStats.dispatchedOrInFlightGroups;
        snapshot.completedClodResults = clodStats.completedResults;
        snapshot.pendingDirectStorageLaunches = clodStats.pendingDirectStorageLaunches;
        snapshot.pendingDirectStorageUploads = clodStats.pendingDirectStorageUploads;
    }
    const auto taskStats = TaskSchedulerManager::GetInstance().GetQueueStats();
    snapshot.schedulerWorkerCount = TaskSchedulerManager::GetInstance().WorkerCount();
    for (std::size_t index = 0; index < snapshot.schedulerDomains.size(); ++index) {
        const auto& source = taskStats.domains[index];
        auto& target = snapshot.schedulerDomains[index];
        target.queued = source.queued;
        target.active = source.active;
        target.completed = source.completed;
        target.queueWaitMicros = source.queueWaitMicros;
        target.maxQueueWaitMicros = source.maxQueueWaitMicros;
        target.executionMicros = source.executionMicros;
        target.maxExecutionMicros = source.maxExecutionMicros;
        target.highWatermark = source.highWatermark;
        target.concurrency = TaskSchedulerManager::GetInstance().DomainConcurrency(
            static_cast<TaskDomain>(index));
        target.maxQueueWaitTask = source.maxQueueWaitTask;
        target.maxExecutionTask = source.maxExecutionTask;
    }
    if (m_rendererStatePublisher) {
        snapshot.rendererStatePublisher = m_rendererStatePublisher->Stats();
    }
    snapshot.ioTasks = taskStats.ioQueued + taskStats.ioActive;
    snapshot.backgroundTasks = taskStats.backgroundQueued + taskStats.backgroundActive;
    snapshot.shaderCompileTasks = taskStats.shaderCompileQueued + taskStats.shaderCompileActive;
    if (includeExpensiveDiagnostics) {
        const auto deferredReleaseStats = DescriptorHeapManager::GetInstance().GetDeferredReleaseStats();
        snapshot.deferredGpuReleaseCount = deferredReleaseStats.releaseCount;
        snapshot.deferredGpuReleaseResourceCount = deferredReleaseStats.resourceCount;
        snapshot.blockedGpuReleaseCount = deferredReleaseStats.blockedReleaseCount;
        snapshot.invalidGpuReleaseTimelineCount = deferredReleaseStats.invalidTimelineCount;
        snapshot.deviceErrorGpuReleaseTimelineCount = deferredReleaseStats.deviceErrorTimelineCount;
        snapshot.incompleteGpuReleaseTimelineCount = deferredReleaseStats.incompleteTimelineCount;
        snapshot.deferredGpuReleaseResourceIDs = std::move(deferredReleaseStats.resourceIDs);
        const auto deletionStats = DeletionManager::GetInstance().GetStats();
        snapshot.deletionQueueObjectCount = deletionStats.objectCount;
        snapshot.deletionQueueAllocationCount = deletionStats.allocationCount;
        snapshot.deletionQueueTrackedAllocationCount = deletionStats.trackedAllocationCount;
    }
    if (m_pObjectManager) {
        const auto objectStats = m_pObjectManager->GetStats();
        snapshot.deferredRetireQueueDepth = objectStats.deferredRetireQueueDepth;
        snapshot.drawRecordsAllocated = objectStats.instanceDrawRecordsAllocated;
    }
    return snapshot;
}

void Renderer::SetDeterministicSamplingMode(bool enabled) {
    m_deterministicSamplingMode = enabled;
    if (m_pMaterialManager) {
        m_pMaterialManager->SetTextureStreamingFeedbackSuppressed(enabled);
    }
    if (enabled) {
        m_jitter = false;
        movementState = {};
    }
}

void Renderer::RegisterExternalSnapshotMeshes(const br::render::SceneFrameSnapshot& snapshot) {
    if (!m_pMeshManager || !m_pMaterialManager || !m_pIndirectCommandBufferManager) {
        return;
    }

    const bool useMeshletReorderedVertices = getMeshShadersEnabled ? getMeshShadersEnabled() : m_useMeshShaders;

    for (const auto& renderable : snapshot.changedRenderables) {
        for (const auto& meshInstance : renderable.meshInstances.meshInstances) {
            if (!meshInstance) {
                continue;
            }

            auto mesh = meshInstance->GetMesh();
            auto material = meshInstance->GetEffectiveMaterial();
            if (!mesh || !material) {
                continue;
            }

            const auto instanceKey = reinterpret_cast<uint64_t>(meshInstance.get());
            const bool externalInstanceKnown = m_externalRegisteredMeshInstances.contains(instanceKey);
            const bool needsExternalInstanceRegistration =
                !externalInstanceKnown || meshInstance->GetPerMeshInstanceBufferView() == nullptr;
            if (needsExternalInstanceRegistration && meshInstance->HasSkin() && m_pSkeletonManager) {
                meshInstance->SetPoseRegistrationService(std::addressof(m_poseInstanceRegistrationService));
                auto skinInst = meshInstance->GetSkin();
                m_poseInstanceRegistrationService.Acquire(skinInst);
                meshInstance->SetSkinningInstanceSlot(skinInst->GetSkinningInstanceSlot());
                meshInstance->SyncSkinningStateFromSkeleton();
            }

            const bool externalMeshKnown = m_externalRegisteredMeshes.contains(mesh->GetGlobalID());
            const bool needsExternalMeshRegistration =
                !externalMeshKnown || mesh->GetPerMeshBufferView() == nullptr;
            if (needsExternalMeshRegistration) {
                if (!externalMeshKnown) {
                    m_pMaterialManager->IncrementMaterialUsageCount(*material);
                }
                const auto materialDataIndex = m_pMaterialManager->GetMaterialSlot(material->GetMaterialID());
                mesh->SetMaterialDataIndex(materialDataIndex);
                const auto materialEvalVariants = ComposeMaterialEvalVariantSet(*mesh, *material);
                const auto materialEvalCompileFlagsID =
                    m_pMaterialManager->AcquireCompileFlagsSlot(materialEvalVariants.regular);
                mesh->SetMaterialEvalCompileFlagsID(materialEvalCompileFlagsID);
                const auto materialReyesEvalCompileFlagsID =
                    materialEvalVariants.hasDistinctReyes
                        ? m_pMaterialManager->AcquireCompileFlagsSlot(materialEvalVariants.reyes)
                        : materialEvalCompileFlagsID;
                mesh->SetMaterialReyesEvalCompileFlagsID(materialReyesEvalCompileFlagsID);

                auto rasterFlags = material->Technique().rasterFlags;
                if ((mesh->GetPerMeshCBData().vertexFlags & VERTEX_SKINNED) != 0u) {
                    rasterFlags |= MaterialRasterFlagsSkinned;
                }
                const auto rasterBucketIndex = m_pMaterialManager->AcquireRasterBucket(rasterFlags);
                mesh->SetRasterBucketIndex(rasterBucketIndex);

                if (!m_pMeshManager->AddMesh(mesh, useMeshletReorderedVertices)) {
                    m_pMaterialManager->ReleaseRasterBucket(rasterFlags);
                    m_pMaterialManager->ReleaseCompileFlagsSlot(materialEvalVariants.regular);
                    if (materialEvalVariants.hasDistinctReyes) {
                        m_pMaterialManager->ReleaseCompileFlagsSlot(materialEvalVariants.reyes);
                    }
                    if (!externalMeshKnown) {
                        m_pMaterialManager->DecrementMaterialUsageCount(*material);
                    }
                    m_externalRegisteredMeshes.erase(mesh->GetGlobalID());
                    continue;
                }
                m_externalRegisteredMeshes.insert(mesh->GetGlobalID());
                m_externalMeshRegistrations[mesh->GetGlobalID()] = ExternalMeshRegistration{
                    .material = material,
                    .regularEvalFlags = materialEvalVariants.regular,
                    .reyesEvalFlags = materialEvalVariants.reyes,
                    .rasterFlags = rasterFlags,
                    .hasDistinctReyes = materialEvalVariants.hasDistinctReyes,
                };

                ForEachMeshDrawWorkload(*mesh, *material, [&](const DrawWorkloadKey& workload) {
                    m_pIndirectCommandBufferManager->RegisterWorkload(workload);
                });
            }

            if (needsExternalInstanceRegistration) {
                if (m_pMeshManager->AddMeshInstance(meshInstance.get(), useMeshletReorderedVertices)) {
                    m_externalRegisteredMeshInstances.insert(instanceKey);
                } else {
                    m_externalRegisteredMeshInstances.erase(instanceKey);
                }
            }
        }
    }
}

void Renderer::ClearExternalSnapshotMeshRegistrations() {
    if (m_pMaterialManager) {
        for (const auto& [_, registration] : m_externalMeshRegistrations) {
            m_pMaterialManager->ReleaseCompileFlagsSlot(registration.regularEvalFlags);
            if (registration.hasDistinctReyes) {
                m_pMaterialManager->ReleaseCompileFlagsSlot(registration.reyesEvalFlags);
            }
            m_pMaterialManager->ReleaseRasterBucket(registration.rasterFlags);
            if (registration.material) {
                m_pMaterialManager->DecrementMaterialUsageCount(*registration.material);
            }
        }
    }
    m_externalMeshRegistrations.clear();
    m_externalRegisteredMeshes.clear();
    m_externalRegisteredMeshInstances.clear();
}

void Renderer::ApplyPrimaryCameraInput(float elapsedSeconds) {
    if (m_deterministicSamplingMode) {
        return;
    }
    if (!currentScene || !currentScene->HasUsablePrimaryCamera()) {
        return;
    }

    Components::Position& cameraPosition = currentScene->GetPrimaryCameraPosition();
    Components::Rotation& cameraRotation = currentScene->GetPrimaryCameraRotation();
    ApplyMovement(cameraPosition, cameraRotation, movementState, elapsedSeconds);
    RotatePitchYaw(cameraRotation, verticalAngle, horizontalAngle);
    currentScene->GetPrimaryCamera().modified<Components::Position>();
    currentScene->GetPrimaryCamera().modified<Components::Rotation>();
    verticalAngle = 0.0f;
    horizontalAngle = 0.0f;
}

void Renderer::InvalidateSceneOverlapState() {
    m_sceneOverlapEpoch.fetch_add(1, std::memory_order_relaxed);
    m_sceneTaskCompleted.store(false);
    {
        std::scoped_lock lock(m_sceneSnapshotMutex);
        m_hasCommittedSceneSnapshot = false;
        m_completedSceneSnapshot.reset();
    }
}

void Renderer::SetSceneRenderOverlapEnabled(bool enabled) {
    if (m_sceneRenderOverlapEnabled == enabled) {
        return;
    }

    if (m_sceneTaskInFlight.load()) {
        spdlog::info("Renderer: scene overlap mode changed while async update was running. Dropping stale async snapshot work.");
    }

    m_sceneRenderOverlapEnabled = enabled;
    InvalidateSceneOverlapState();

    if (!currentScene) {
        return;
    }

    if (enabled) {
        BootstrapCommittedSceneSnapshot();
        return;
    }

    RunSceneBridgeSyncStage();
}

void Renderer::QueueSceneNodePositionEdit(uint64_t stableSceneID, DirectX::XMFLOAT3 position) {
    std::scoped_lock lock(m_pendingSceneExplorerEditsMutex);
    auto& edit = m_pendingSceneExplorerEdits[stableSceneID];
    edit.hasPosition = true;
    edit.position = position;
}

void Renderer::QueueSceneNodeUniformScaleEdit(uint64_t stableSceneID, float uniformScale) {
    std::scoped_lock lock(m_pendingSceneExplorerEditsMutex);
    auto& edit = m_pendingSceneExplorerEdits[stableSceneID];
    edit.hasUniformScale = true;
    edit.uniformScale = uniformScale;
}

void Renderer::FlushPendingSceneExplorerEdits() {
    if (!currentScene || m_sceneTaskInFlight.load()) {
        return;
    }

    std::unordered_map<uint64_t, PendingSceneExplorerEdit> pendingEdits;
    {
        std::scoped_lock lock(m_pendingSceneExplorerEditsMutex);
        if (m_pendingSceneExplorerEdits.empty()) {
            return;
        }
        pendingEdits.swap(m_pendingSceneExplorerEdits);
    }

    bool anyApplied = false;
    auto root = currentScene->GetRoot();
    for (const auto& [stableSceneID, edit] : pendingEdits) {
        auto entity = FindSceneEntityByStableSceneID(root, stableSceneID);
        if (!entity.is_alive()) {
            continue;
        }

        if (edit.hasPosition && entity.has<Components::Position>()) {
            entity.set<Components::Position>(edit.position);
            anyApplied = true;
        }

        if (edit.hasUniformScale && entity.has<Components::Scale>()) {
            entity.set<Components::Scale>({ edit.uniformScale, edit.uniformScale, edit.uniformScale });
            anyApplied = true;
        }
    }

    if (anyApplied) {
        currentScene->PropagateTransforms();

        InvalidateSceneOverlapState();
        if (m_sceneRenderOverlapEnabled) {
            BootstrapCommittedSceneSnapshot();
        } else {
            RunSceneBridgeSyncStage();
        }
    }
}

void Renderer::BootstrapCommittedSceneSnapshot() {
    if (!currentScene) {
        std::scoped_lock lock(m_sceneSnapshotMutex);
        m_hasCommittedSceneSnapshot = false;
        return;
    }

    auto snapshot = std::make_shared<br::render::SceneFrameSnapshot>(
        m_sceneRenderBridge.ExportSnapshot(*currentScene, m_nextSceneSnapshotSequence++, m_totalFramesRendered));
    m_sceneRenderBridge.IngestSnapshot(*snapshot, GetSceneIngestionServices(),
        CaptureSceneIngestionConfiguration());

    std::scoped_lock lock(m_sceneSnapshotMutex);
    m_hasCommittedSceneSnapshot = true;
    m_lastCommittedSceneSnapshotSequence = snapshot->snapshotSequence;
    m_lastCommittedSceneSourceFrame = snapshot->sourceFrameNumber;
}

void Renderer::CommitCompletedSceneSnapshot() {
    BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot");

    if (!m_sceneTaskCompleted.exchange(false)) {
        return;
    }

    std::shared_ptr<br::render::SceneFrameSnapshot> completedSnapshot;
    {
        BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot::TakeCompletedSnapshot");
        std::scoped_lock lock(m_sceneSnapshotMutex);
        completedSnapshot = std::exchange(m_completedSceneSnapshot, nullptr);
    }

    if (!completedSnapshot) {
        return;
    }

    if (!currentScene || completedSnapshot->sceneID != currentScene->GetSceneID()) {
        return;
    }

    {
        BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot::IngestSnapshot");
        m_sceneRenderBridge.IngestSnapshot(*completedSnapshot, GetSceneIngestionServices(),
            CaptureSceneIngestionConfiguration());
    }
    {
        BT_ZONE_SCOPE("Renderer::CommitCompletedSceneSnapshot::PublishCommittedSnapshot");
        std::scoped_lock lock(m_sceneSnapshotMutex);
        m_hasCommittedSceneSnapshot = true;
        m_lastCommittedSceneSnapshotSequence = completedSnapshot->snapshotSequence;
        m_lastCommittedSceneSourceFrame = completedSnapshot->sourceFrameNumber;
    }
}

void Renderer::ScheduleSceneUpdateTask(float elapsedSeconds) {
    ZoneScopedN("Renderer::ScheduleSceneUpdateTask");
    if (!m_sceneRenderOverlapEnabled || !currentScene || m_sceneTaskInFlight.exchange(true)) {
        return;
    }

    auto scene = currentScene;
    const auto movementSnapshot = movementState;
    const float verticalAngleSnapshot = verticalAngle;
    const float horizontalAngleSnapshot = horizontalAngle;
    const uint64_t overlapEpoch = m_sceneOverlapEpoch.load(std::memory_order_relaxed);
    const uint64_t snapshotSequence = m_nextSceneSnapshotSequence++;
    const uint64_t sourceFrameNumber = m_totalFramesRendered + 1;

    verticalAngle = 0.0f;
    horizontalAngle = 0.0f;

    TaskSchedulerManager::GetInstance().Submit(TaskLane::Streaming, TaskDomain::General, "SceneUpdateOverlap", [this, scene, elapsedSeconds, movementSnapshot, verticalAngleSnapshot, horizontalAngleSnapshot, overlapEpoch, snapshotSequence, sourceFrameNumber]() mutable {
        ZoneScopedN("Renderer::SceneUpdateOverlap");
        const auto taskStart = std::chrono::steady_clock::now();

        if (!scene) {
            m_sceneTaskInFlight.store(false);
            return;
        }

        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::ApplyCameraInput");
            if (scene->HasUsablePrimaryCamera()) {
                Components::Position& cameraPosition = scene->GetPrimaryCameraPosition();
                Components::Rotation& cameraRotation = scene->GetPrimaryCameraRotation();
                ApplyMovement(cameraPosition, cameraRotation, movementSnapshot, elapsedSeconds);
                RotatePitchYaw(cameraRotation, verticalAngleSnapshot, horizontalAngleSnapshot);
                scene->GetPrimaryCamera().modified<Components::Position>();
                scene->GetPrimaryCamera().modified<Components::Rotation>();
            }
        }

        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::SceneUpdate");
            scene->Update(elapsedSeconds);
        }
        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::PropagateTransforms");
            scene->PropagateTransforms();
        }

        std::shared_ptr<br::render::SceneFrameSnapshot> snapshot;
        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::ExportSnapshot");
            snapshot = std::make_shared<br::render::SceneFrameSnapshot>(
                m_sceneRenderBridge.ExportSnapshot(*scene, snapshotSequence, sourceFrameNumber));
        }

        if (overlapEpoch != m_sceneOverlapEpoch.load(std::memory_order_relaxed)) {
            m_sceneTaskInFlight.store(false);
            return;
        }

        const auto taskEnd = std::chrono::steady_clock::now();
        const auto durationMs = std::chrono::duration<double, std::milli>(taskEnd - taskStart).count();

        {
            ZoneScopedN("Renderer::SceneUpdateOverlap::PublishSnapshot");
            std::scoped_lock lock(m_sceneSnapshotMutex);
            m_completedSceneSnapshot = std::move(snapshot);
            m_lastCompletedSceneSnapshotSequence = snapshotSequence;
            m_lastSceneTaskDurationMs = durationMs;
        }

        m_sceneTaskCompleted.store(true);
        m_sceneTaskInFlight.store(false);
    });
}

bool Renderer::HasCommittedSceneSnapshot() const {
    std::scoped_lock lock(m_sceneSnapshotMutex);
    return m_hasCommittedSceneSnapshot;
}

bool Renderer::NeedsSceneSnapshotBootstrap() const {
    if (!m_sceneRenderOverlapEnabled || !currentScene || m_sceneTaskInFlight.load()) {
        return false;
    }

    if (!currentScene->HasUsablePrimaryCamera()) {
        return false;
    }

    return !HasCommittedSceneSnapshot() || !m_sceneRenderBridge.HasPrimaryCamera();
}

br::render::SceneOverlapStatus Renderer::GetSceneOverlapStatus() const {
    br::render::SceneOverlapStatus status;
    status.enabled = m_sceneRenderOverlapEnabled;
    status.taskInFlight = m_sceneTaskInFlight.load();

    std::scoped_lock lock(m_sceneSnapshotMutex);
    status.hasCommittedSnapshot = m_hasCommittedSceneSnapshot;
    status.committedSnapshotSequence = m_lastCommittedSceneSnapshotSequence;
    status.lastCompletedSnapshotSequence = m_lastCompletedSceneSnapshotSequence;
    status.lastCommittedSourceFrame = m_lastCommittedSceneSourceFrame;
    status.lastTaskDurationMs = m_lastSceneTaskDurationMs;
    if (m_completedSceneSnapshot) {
        status.pendingSnapshotSequence = m_completedSceneSnapshot->snapshotSequence;
    }

    return status;
}

void Renderer::RunRenderResourceSyncStage() {
    BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync");

    auto& world = RendererECSManager::GetInstance().GetWorld();

    if (!m_renderSyncQueriesBuilt) {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::BuildQueries");
        m_renderSyncObjectQuery = world.query_builder<Components::Matrix, Components::RenderableObject, Components::ObjectDrawInfo, Components::MeshInstances>()
            .with<Components::Active>()
            .with<Components::RenderTransformUpdated>()
            .build();
        m_renderSyncCameraQuery = world.query_builder<Components::Matrix, Components::Camera, Components::RenderViewRef>()
            .with<Components::Active>()
            .build();
        m_renderSyncLightQuery = world.query_builder<Components::Matrix, Components::Light>()
            .with<Components::Active>()
            .build();
        m_renderTransformUpdatedCleanupQuery = world.query_builder<>()
            .with<Components::RenderTransformUpdated>()
            .build();
        m_renderSyncQueriesBuilt = true;
    }

    // Collect object entity data for parallel processing
    struct ObjectSyncItem {
        Components::Matrix* worldMatrix;
        Components::RenderableObject* object;
        Components::ObjectDrawInfo* drawInfo;
        Components::MeshInstances* meshInstances;
        const Components::InstanceTransforms* instanceTransforms;
    };
    std::vector<ObjectSyncItem> objectItems;
    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CollectObjectsAndMaterials");
        m_renderSyncObjectQuery.run([&](flecs::iter& it) {
            while (it.next()) {
                auto matrices = it.field<Components::Matrix>(0);
                auto objects = it.field<Components::RenderableObject>(1);
                auto drawInfos = it.field<Components::ObjectDrawInfo>(2);
                auto meshInstances = it.field<Components::MeshInstances>(3);
                for (auto i : it) {
                    objectItems.push_back({
                        &matrices[i],
                        &objects[i],
                        &drawInfos[i],
                        &meshInstances[i],
                        it.entity(i).try_get<Components::InstanceTransforms>()
                    });
                }
            }
        });
    }

    if (auto* terrainService = m_sceneIngestionServices.sceneAssetRequests) {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::ProcessPendingTerrainUpdates");
        terrainService->ProcessPendingTerrainUpdates();
    }

    auto* objectManager = m_pObjectManager.get();
    std::vector<std::pair<size_t, size_t>> perObjectDirtyRanges;
    std::vector<std::pair<size_t, size_t>> perInstanceTransformDirtyRanges;
    std::vector<std::pair<size_t, size_t>> normalMatrixDirtyRanges;
    perObjectDirtyRanges.reserve(objectItems.size());
    perInstanceTransformDirtyRanges.reserve(objectItems.size());
    normalMatrixDirtyRanges.reserve(objectItems.size());

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::ScanObjectDirtyRanges");
        const auto appendRanges = [](std::vector<std::pair<size_t, size_t>>& ranges, const std::vector<std::shared_ptr<BufferView>>& views, size_t stride) {
            for (const auto& view : views) {
                if (!view) {
                    continue;
                }
                const size_t begin = view->GetOffset();
                ranges.emplace_back(begin, begin + stride);
            }
        };
        for (const auto& item : objectItems) {
            if (!item.drawInfo->perObjectCBViews.empty()) {
                appendRanges(perObjectDirtyRanges, item.drawInfo->perObjectCBViews, sizeof(PerObjectCB));
            } else if (item.drawInfo->perObjectCBView) {
                const size_t perObjectBegin = item.drawInfo->perObjectCBView->GetOffset();
                perObjectDirtyRanges.emplace_back(perObjectBegin, perObjectBegin + sizeof(PerObjectCB));
            }
            appendRanges(perInstanceTransformDirtyRanges, item.drawInfo->perInstanceTransformViews, sizeof(PerInstanceTransformCB));
            if (!item.drawInfo->normalMatrixViews.empty()) {
                appendRanges(normalMatrixDirtyRanges, item.drawInfo->normalMatrixViews, sizeof(DirectX::XMFLOAT4X4));
            } else if (item.drawInfo->normalMatrixView) {
                const size_t normalMatrixBegin = item.drawInfo->normalMatrixView->GetOffset();
                normalMatrixDirtyRanges.emplace_back(normalMatrixBegin, normalMatrixBegin + sizeof(DirectX::XMFLOAT4X4));
            }
        }
    }

    // Pre-size scratch buffers single-threaded so the parallel loop can
    // memcpy into non-overlapping regions without any synchronization.
    auto perObjectHandle = objectManager->BeginPerObjectBulkWrite();
    auto perInstanceTransformHandle = objectManager->BeginPerInstanceTransformBulkWrite();
    auto normalMatrixHandle = objectManager->BeginNormalMatrixBulkWrite();

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::ObjectSync");
        TaskSchedulerManager::GetInstance().ParallelFor("ObjectSync", objectItems.size(),
            [&objectItems, &perObjectHandle, &perInstanceTransformHandle, &normalMatrixHandle](size_t idx) {
                auto& item = objectItems[idx];
                auto* worldMatrix = item.worldMatrix;
                auto* object = item.object;
                auto* drawInfo = item.drawInfo;
                const auto computeNormalMatrix = [](const XMMATRIX& modelMatrix) {
                    const XMMATRIX upperLeft3x3 = XMMatrixSet(
                        XMVectorGetX(modelMatrix.r[0]), XMVectorGetY(modelMatrix.r[0]), XMVectorGetZ(modelMatrix.r[0]), 0.0f,
                        XMVectorGetX(modelMatrix.r[1]), XMVectorGetY(modelMatrix.r[1]), XMVectorGetZ(modelMatrix.r[1]), 0.0f,
                        XMVectorGetX(modelMatrix.r[2]), XMVectorGetY(modelMatrix.r[2]), XMVectorGetZ(modelMatrix.r[2]), 0.0f,
                        0.0f, 0.0f, 0.0f, 1.0f);
                    DirectX::XMFLOAT4X4 stored{};
                    XMStoreFloat4x4(&stored, XMMatrixTranspose(XMMatrixInverse(nullptr, upperLeft3x3)));
                    return stored;
                };
                const auto writeRow = [&](size_t rowIndex, PerObjectCB perObject) {
                    const auto perObjectView = rowIndex < drawInfo->perObjectCBViews.size()
                        ? drawInfo->perObjectCBViews[rowIndex]
                        : drawInfo->perObjectCBView;
                    const auto instanceTransformView = rowIndex < drawInfo->perInstanceTransformViews.size()
                        ? drawInfo->perInstanceTransformViews[rowIndex]
                        : nullptr;
                    const auto normalMatrixView = rowIndex < drawInfo->normalMatrixViews.size()
                        ? drawInfo->normalMatrixViews[rowIndex]
                        : drawInfo->normalMatrixView;

                    if (normalMatrixView) {
                        perObject.normalMatrixBufferIndex = static_cast<uint32_t>(normalMatrixView->GetOffset() / sizeof(DirectX::XMFLOAT4X4));
                    }

                    if (perObjectView) {
                        const size_t offset = perObjectView->GetOffset();
                        std::memcpy(perObjectHandle.data + offset, &perObject, sizeof(PerObjectCB));
                    }
                    if (instanceTransformView) {
                        const size_t offset = instanceTransformView->GetOffset();
                        std::memcpy(perInstanceTransformHandle.data + offset, &perObject, sizeof(PerInstanceTransformCB));
                    }
                    if (normalMatrixView) {
                        const size_t offset = normalMatrixView->GetOffset();
                        const auto storedNormal = computeNormalMatrix(perObject.modelMatrix);
                        std::memcpy(normalMatrixHandle.data + offset, &storedNormal, sizeof(DirectX::XMFLOAT4X4));
                    }
                };

                const bool hasInstanceTransforms = item.instanceTransforms && !item.instanceTransforms->transforms.empty();
                if (hasInstanceTransforms) {
                    const size_t rowCount = std::min({
                        item.instanceTransforms->transforms.size(),
                        drawInfo->perObjectCBViews.size(),
                        drawInfo->perInstanceTransformViews.size(),
                        drawInfo->normalMatrixViews.size()
                    });
                    XMMATRIX previousFirstTransform = item.instanceTransforms->transforms.front().matrix;
                    for (size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
                        const auto& instanceTransform = item.instanceTransforms->transforms[rowIndex];
                        auto perObject = object->perObjectCB;
                        perObject.modelMatrix = instanceTransform.matrix;
                        perObject.prevModelMatrix = instanceTransform.matrix;
                        const auto& instanceTransformView = drawInfo->perInstanceTransformViews[rowIndex];
                        if (instanceTransformView && perInstanceTransformHandle.data) {
                            const size_t offset = instanceTransformView->GetOffset();
                            if (offset <= perInstanceTransformHandle.capacity &&
                                sizeof(PerInstanceTransformCB) <= perInstanceTransformHandle.capacity - offset) {
                                // The CPU shadow still contains the last uploaded row
                                // until writeRow replaces it below.
                                PerInstanceTransformCB previousRow{};
                                std::memcpy(
                                    &previousRow,
                                    perInstanceTransformHandle.data + offset,
                                    sizeof(previousRow));
                                perObject.prevModelMatrix = previousRow.modelMatrix;
                            }
                        }
                        if (rowIndex == 0) {
                            previousFirstTransform = perObject.prevModelMatrix;
                        }
                        perObject.modelInverseMatrix = XMMatrixInverse(nullptr, instanceTransform.matrix);
                        const XMVECTOR det = XMMatrixDeterminant(instanceTransform.matrix);
                        perObject.objectFlags = (XMVectorGetX(det) < 0.0f) ? OBJECT_FLAG_REVERSE_WINDING : 0u;
                        writeRow(rowIndex, perObject);
                    }
                    if (!item.instanceTransforms->transforms.empty()) {
                        object->perObjectCB.modelMatrix = item.instanceTransforms->transforms.front().matrix;
                        object->perObjectCB.prevModelMatrix = previousFirstTransform;
                        object->perObjectCB.modelInverseMatrix = XMMatrixInverse(nullptr, object->perObjectCB.modelMatrix);
                    }
                } else {
                    object->perObjectCB.prevModelMatrix = object->perObjectCB.modelMatrix;
                    object->perObjectCB.modelMatrix = worldMatrix->matrix;
                    object->perObjectCB.modelInverseMatrix = XMMatrixInverse(nullptr, worldMatrix->matrix);
                    const XMVECTOR det = XMMatrixDeterminant(worldMatrix->matrix);
                    object->perObjectCB.objectFlags = (XMVectorGetX(det) < 0.0f) ? OBJECT_FLAG_REVERSE_WINDING : 0u;
                    writeRow(0, object->perObjectCB);
                }
            });
    }

    // Register dirty ranges single-threaded.
    // Upload only the range actually written this frame. Uploading the entire
    // grown backing every frame scales badly
    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitObjectBulkWrites");
        const auto commitRanges = [](auto& ranges, auto&& commit) {
            if (ranges.empty()) {
                return;
            }
            std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.first < rhs.first;
            });

            size_t begin = ranges.front().first;
            size_t end = ranges.front().second;
            for (size_t i = 1; i < ranges.size(); ++i) {
                const auto [nextBegin, nextEnd] = ranges[i];
                if (nextBegin <= end) {
                    end = std::max(end, nextEnd);
                    continue;
                }
                commit(begin, end - begin);
                begin = nextBegin;
                end = nextEnd;
            }
            commit(begin, end - begin);
        };

        {
            BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitPerObjectRanges");
            commitRanges(perObjectDirtyRanges, [objectManager](size_t offset, size_t size) {
                objectManager->EndPerObjectBulkWrite(offset, size);
            });
        }
        {
            BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitPerInstanceTransformRanges");
            commitRanges(perInstanceTransformDirtyRanges, [objectManager](size_t offset, size_t size) {
                objectManager->EndPerInstanceTransformBulkWrite(offset, size);
            });
        }
        {
            BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CommitNormalMatrixRanges");
            commitRanges(normalMatrixDirtyRanges, [objectManager](size_t offset, size_t size) {
                objectManager->EndNormalMatrixBulkWrite(offset, size);
            });
        }
    }

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::CameraSync");
        m_renderSyncCameraQuery.each([&](flecs::entity entity, Components::Matrix& worldMatrix, Components::Camera& camera, Components::RenderViewRef& renderView) {
            const auto* externalCamera = entity.try_get<Components::ExternalCameraMatrices>();
            const XMMATRIX cameraModel = externalCamera ? externalCamera->info.viewInverse : RemoveScalingFromMatrix(worldMatrix.matrix);
            const XMMATRIX view = externalCamera ? externalCamera->info.view : XMMatrixInverse(nullptr, cameraModel);
            DirectX::XMMATRIX projection = camera.info.unjitteredProjection;
            camera.info.prevJitteredProjection = camera.info.jitteredProjection;
            camera.info.prevUnjitteredProjection = camera.info.unjitteredProjection;
            if (externalCamera) {
                projection = externalCamera->info.unjitteredProjection;
                camera.info.clippingPlanes[0] = externalCamera->info.clippingPlanes[0];
                camera.info.clippingPlanes[1] = externalCamera->info.clippingPlanes[1];
                camera.info.clippingPlanes[2] = externalCamera->info.clippingPlanes[2];
                camera.info.clippingPlanes[3] = externalCamera->info.clippingPlanes[3];
                camera.info.clippingPlanes[4] = externalCamera->info.clippingPlanes[4];
                camera.info.clippingPlanes[5] = externalCamera->info.clippingPlanes[5];
                camera.info.fov = externalCamera->info.fov;
                camera.info.aspectRatio = externalCamera->info.aspectRatio;
                camera.info.zNear = externalCamera->info.zNear;
                camera.info.zFar = externalCamera->info.zFar;
            } else if (m_jitter && entity.has<Components::PrimaryCamera>()) {
                const auto jitterPixelSpace = UpscalingManager::GetInstance().GetJitter(m_totalFramesRendered);
                camera.jitterPixelSpace = jitterPixelSpace;
                const auto renderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
                const DirectX::XMFLOAT2 jitterNDC = {
                    (2.0f * jitterPixelSpace.x / renderRes.x),
                    (-2.0f * jitterPixelSpace.y / renderRes.y)
                };
                camera.jitterNDC = jitterNDC;
                const auto jitterMatrix = DirectX::XMMatrixTranslation(jitterNDC.x, jitterNDC.y, 0.0f);
                projection = XMMatrixMultiply(projection, jitterMatrix);
            }

            camera.info.jitteredProjection = projection;
            camera.info.prevView = camera.info.view;
            camera.info.view = view;
            camera.info.viewInverse = cameraModel;
            camera.info.viewProjection = XMMatrixMultiply(camera.info.view, projection);
            camera.info.projectionInverse = XMMatrixInverse(nullptr, projection);

            if (externalCamera) {
                camera.info.positionWorldSpace = externalCamera->info.positionWorldSpace;
            } else {
                const auto pos = GetGlobalPositionFromMatrix(worldMatrix.matrix);
                camera.info.positionWorldSpace = { pos.x, pos.y, pos.z, 1.0f };
            }

            m_pViewManager->UpdateCamera(renderView.viewID, camera.info);
        });
    }

    {
        BT_ZONE_SCOPE("Renderer::Update::RenderResourceSync::LightSync");
        m_renderSyncLightQuery.each([&](flecs::entity entity, Components::Matrix& worldMatrix, Components::Light& light) {
            const XMVECTOR worldForward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            light.lightInfo.dirWorldSpace = XMVector3Normalize(XMVector3TransformNormal(worldForward, worldMatrix.matrix));
            light.lightInfo.posWorldSpace = XMVectorSet(
                XMVectorGetX(worldMatrix.matrix.r[3]),
                XMVectorGetY(worldMatrix.matrix.r[3]),
                XMVectorGetZ(worldMatrix.matrix.r[3]),
                1.0f);
            switch (light.lightInfo.type) {
            case Components::LightType::Spot:
                light.lightInfo.boundingSphere = ComputeConeBoundingSphere(light.lightInfo.posWorldSpace, light.lightInfo.dirWorldSpace, light.lightInfo.maxRange, acos(light.lightInfo.outerConeAngle));
                break;
            case Components::LightType::Point:
                light.lightInfo.boundingSphere = {{
                    XMVectorGetX(worldMatrix.matrix.r[3]),
                    XMVectorGetY(worldMatrix.matrix.r[3]),
                    XMVectorGetZ(worldMatrix.matrix.r[3]),
                    light.lightInfo.maxRange }};
                break;
            default:
                break;
            }

            if (light.lightInfo.shadowCaster && entity.has<Components::LightViewInfo>()) {
                const Components::LightViewInfo& viewInfo = entity.get<Components::LightViewInfo>();
                m_pLightManager->UpdateLightBufferView(viewInfo.lightBufferView.get(), light.lightInfo);
                m_pLightManager->UpdateLightViewInfo(entity);
            }
        });
    }

}

void Renderer::BeginFrameTaskGraphCapture() {
    br::telemetry::BeginFrameTaskGraphCapture(m_totalFramesRendered, m_frameIndex);
    m_lastFrameTaskNodeIndex = -1;
}

void Renderer::RecordFrameTaskStage(
    const char* stageName,
    br::telemetry::CpuTaskDomain domain,
    const std::chrono::steady_clock::time_point& stageStart,
    const std::chrono::steady_clock::time_point& stageEnd) {
    m_lastFrameTaskNodeIndex = br::telemetry::RecordFrameTaskNode(stageName, domain, m_lastFrameTaskNodeIndex, stageStart, stageEnd);
}

void Renderer::PublishFrameTaskGraphCapture() {
    br::telemetry::PublishFrameTaskGraphSnapshot();
}

void Renderer::CreateGlobalResources() {
    auto blueNoiseAsset = LoadTextureFromFile(L"BuiltinTextures/BlueNoise470.png", nullptr, false);
    if (blueNoiseAsset) {
        blueNoiseAsset->EnsureUploaded(*m_pTextureFactory);
        m_blueNoiseTexture = blueNoiseAsset->ImagePtr();
        if (m_blueNoiseTexture) {
            m_blueNoiseTexture->SetName("Blue Noise 2D");
            org::memory::SetResourceUsageHint(*m_blueNoiseTexture, "Noise lookup resources");
        }
    }

    m_openPBRLookupResources = CreateOpenPBRLookupResources(*m_pTextureFactory);
}

void Renderer::CreateDefaultEnvironmentResources() {
    auto makeFallbackCubemap = [](uint32_t resolution, bool generateMipMaps, const char* name) {
        TextureDescription desc;
        desc.channels = 4;
        desc.isCubemap = true;
        desc.format = rhi::Format::R8G8B8A8_UNorm;
        desc.hasSRV = true;
        desc.generateMipMaps = generateMipMaps;

        ImageDimensions dims;
        dims.width = resolution;
        dims.height = resolution;
        dims.rowPitch = resolution * 4;
        dims.slicePitch = resolution * resolution * 4;
        for (int i = 0; i < 6; ++i) {
            desc.imageDimensions.push_back(dims);
        }

        auto cubemap = PixelBuffer::CreateShared(desc);
        cubemap->SetName(name);
        return cubemap;
    };

    auto reflectionResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("reflectionCubemapResolution")();
    auto skyboxResolution = SettingsManager::GetInstance().getSettingGetter<uint16_t>("skyboxResolution")();

    if (!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap) && !m_defaultEnvironmentCubemap) {
        m_defaultEnvironmentCubemap = makeFallbackCubemap(skyboxResolution, false, "Fallback Environment Cubemap");
        org::memory::SetResourceUsageHint(*m_defaultEnvironmentCubemap, "Fallback environment resources");
    }
    if (!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentPrefilteredCubemap) && !m_defaultEnvironmentPrefilteredCubemap) {
        m_defaultEnvironmentPrefilteredCubemap = makeFallbackCubemap(
            reflectionResolution,
            true,
            "Fallback Prefiltered Environment Cubemap");
        org::memory::SetResourceUsageHint(*m_defaultEnvironmentPrefilteredCubemap, "Fallback environment resources");
    }
}

bool Renderer::IsSceneReadyForFrame(bool logWarnings) {
    if (m_externalSceneMode) {
        if (!m_sceneRenderBridge.HasPrimaryCamera()) {
            if (logWarnings && !m_warnedMissingPrimaryCamera) {
                spdlog::warn("Renderer: external scene snapshot has no primary camera. Skipping frame update work.");
            }
            m_warnedMissingPrimaryCamera = true;
            return false;
        }

        m_warnedNullScene = false;
        m_warnedMissingPrimaryCamera = false;
        return true;
    }

    if (!currentScene) {
        if (logWarnings && !m_warnedNullScene) {
            spdlog::warn("Renderer: current scene is null. Skipping scene update/render work until a valid scene is set.");
        }
        m_warnedNullScene = true;
        m_warnedMissingPrimaryCamera = false;
        return false;
    }

    m_warnedNullScene = false;

    const bool hasPrimaryCamera = m_sceneRenderOverlapEnabled
        ? (NeedsSceneSnapshotBootstrap() || (HasCommittedSceneSnapshot() && m_sceneRenderBridge.HasPrimaryCamera()))
        : currentScene->HasUsablePrimaryCamera();

    if (!hasPrimaryCamera) {
        if (logWarnings && !m_warnedMissingPrimaryCamera) {
            spdlog::warn("Renderer: primary camera is missing or invalid. Skipping scene update/render work until a valid camera is available.");
        }
        m_warnedMissingPrimaryCamera = true;
        return false;
    }

    m_warnedMissingPrimaryCamera = false;
    return true;
}

flecs::entity Renderer::GetValidatedPrimaryRenderCamera(bool attemptResync) {
    if (!RendererECSManager::GetInstance().IsAlive()) {
        return {};
    }

    if (!m_externalSceneMode && !currentScene) {
        return {};
    }

    if (!m_externalSceneMode && m_sceneRenderOverlapEnabled && !HasCommittedSceneSnapshot()) {
        return {};
    }

    if (!m_externalSceneMode && !m_sceneRenderOverlapEnabled && !currentScene->HasUsablePrimaryCamera()) {
        return {};
    }

    if (!m_externalSceneMode && m_sceneRenderOverlapEnabled && NeedsSceneSnapshotBootstrap()) {
        if (attemptResync) {
            BootstrapCommittedSceneSnapshot();
        }

        if (!HasCommittedSceneSnapshot()) {
            return {};
        }
    }

    auto validateCamera = [](flecs::entity camera) {
        return camera
            && camera.is_alive()
            && camera.has<Components::Camera>()
            && camera.has<Components::RenderViewRef>()
            && camera.has<Components::DepthMap>();
    };

    auto primaryCamera = m_sceneRenderBridge.GetPrimaryCameraEntity();
    if (!m_externalSceneMode && !validateCamera(primaryCamera) && attemptResync && !m_sceneRenderOverlapEnabled) {
        m_sceneRenderBridge.Sync(*currentScene, GetSceneIngestionServices(),
            CaptureSceneIngestionConfiguration());
        primaryCamera = m_sceneRenderBridge.GetPrimaryCameraEntity();
    }

    if (!validateCamera(primaryCamera)) {
        return {};
    }

    return primaryCamera;
}

void Renderer::SetSettings() {
	auto& settingsManager = SettingsManager::GetInstance();

    uint8_t numDirectionalCascades = static_cast<uint8_t>(CLodVirtualShadowDefaultClipmapCount);
	float maxShadowDistance = 100.0f;
    float directionalShadowDistanceLowerBound = maxShadowDistance;
	settingsManager.registerSetting<uint8_t>("numDirectionalLightCascades", numDirectionalCascades);
    settingsManager.registerSetting<float>("maxShadowDistance", maxShadowDistance);
    settingsManager.registerSetting<float>("directionalShadowDistanceLowerBound", directionalShadowDistanceLowerBound);
    // Populated by scene-domain providers such as TerrainManager. Zero retains
    // the camera-derived clip ladder for scenes without explicit extents.
    settingsManager.registerSetting<float>("directionalShadowSceneExtent", 0.0f);
        settingsManager.registerSetting<std::vector<float>>("directionalLightCascadeSplits", calculateCascadeSplits(numDirectionalCascades, 0.1f, maxShadowDistance, maxShadowDistance));
    settingsManager.registerSetting<uint16_t>("shadowResolution", 2048);
    settingsManager.registerSetting<float>("cameraSpeed", 10);
    settingsManager.registerSetting<bool>("rememberCameraPose", false);
	settingsManager.registerSetting<float>(ProceduralWindDisplacementScaleSettingName, 0.0f);
	settingsManager.registerSetting<float>(ProceduralWindGrassDisplacementScaleSettingName, 1.0f);
	settingsManager.registerSetting<float>(ProceduralWindGrassOscillationScaleSettingName, 1.0f);
	settingsManager.registerSetting<float>(ProceduralWindGrassFlutterFrequencySettingName, 1.0f);
	settingsManager.registerSetting<std::vector<float>>(ProceduralWindSkeletonLodQualityCurveSettingName,
		{ 0.50f, 1.00f, 0.25f, 0.70f, 0.10f, 0.48f, 0.04f, 0.28f, 0.015f, 0.10f, 0.005f, 0.00f });
	settingsManager.registerSetting<float>(ProceduralWindSkeletonLodStaticCutoffSettingName, 0.0f);
	settingsManager.registerSetting<float>(ProceduralWindInnerRadiusSettingName, 8000.0f);
	settingsManager.registerSetting<float>(ProceduralWindOuterRadiusSettingName, 10000.0f);
	settingsManager.registerSetting<int32_t>(CLodSkinnedShadowDynamicClipmapCountOverrideSettingName, -1);
	settingsManager.registerSetting<float>(ProceduralWindSkeletonLodCapacityTargetSettingName, 0.95f);
	settingsManager.registerSetting<float>(ProceduralWindSkeletonLodLateReserveSettingName, 0.10f);
	settingsManager.registerSetting<float>(ProceduralWindSkeletonLodHysteresisSettingName, 0.15f);
	settingsManager.registerSetting<uint32_t>(
		ProceduralWindTransientBoneCapacitySettingName,
		ReadUintEnvironmentValue("SARP_PROCEDURAL_WIND_TRANSIENT_BONE_CAPACITY", 262144u));
	settingsManager.registerSetting<uint32_t>(
		MaterialTextureStreamingIdleFramesSettingName,
		ReadUintEnvironmentValue("SARP_TEXTURE_STREAMING_IDLE_FRAMES", 1800u));
	settingsManager.registerSetting<uint32_t>(
		AlphaTestedMaterialTextureMaxResidentTopMipSettingName,
		ReadUintEnvironmentValue(
			"SARP_ALPHA_TESTED_TEXTURE_MAX_RESIDENT_TOP_MIP",
			AlphaTestedMaterialTextureMaxResidentTopMipDefault));
	int32_t forcedSkeletonLod = -1;
	char* forcedSkeletonLodValue = nullptr;
	size_t forcedSkeletonLodValueSize = 0;
	if (_dupenv_s(&forcedSkeletonLodValue, &forcedSkeletonLodValueSize, "SARP_PROCEDURAL_WIND_FORCE_LOD") == 0 &&
		forcedSkeletonLodValue != nullptr) {
		char* end = nullptr;
		const long parsed = std::strtol(forcedSkeletonLodValue, &end, 10);
		if (end != forcedSkeletonLodValue && *end == '\0') {
			forcedSkeletonLod = std::clamp(static_cast<int32_t>(parsed), -1, 15);
		}
	}
	std::free(forcedSkeletonLodValue);
	settingsManager.registerSetting<int32_t>(ProceduralWindForcedSkeletonLodSettingName, forcedSkeletonLod);
	settingsManager.registerSetting<bool>("enableWireframe", false);
    settingsManager.registerSetting<bool>(
        "enableShadows",
        m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>());
	settingsManager.registerSetting<uint16_t>("skyboxResolution", 2048);
    settingsManager.registerSetting<uint16_t>("reflectionCubemapResolution", 512);
	settingsManager.registerSetting<bool>("enableImageBasedLighting", true);
	settingsManager.registerSetting<bool>("enablePunctualLighting", true);
	settingsManager.registerSetting<std::string>("environmentName", "");
	settingsManager.registerSetting<unsigned int>("outputType", OutputType::COLOR);
	settingsManager.registerSetting<unsigned int>("tonemapType", TonemapType::AMD_LPM);
    settingsManager.registerSetting<bool>("allowTearing", false);
    settingsManager.registerSetting<bool>("drawBoundingSpheres", false);
    settingsManager.registerSetting<bool>("enableClusteredLighting", m_clusteredLighting);
    settingsManager.registerSetting<bool>("enableTerrainStochasticSampling", true);
    settingsManager.registerSetting<bool>("enableTerrainStochasticDiffuseSampling", true);
    settingsManager.registerSetting<bool>("enableTerrainStochasticNormalSampling", true);
    settingsManager.registerSetting<bool>("enableTerrainStochasticDerivativeNormalSampling", true);
    settingsManager.registerSetting<float>("terrainStochasticBlendCurve", 0.65f);
    settingsManager.registerSetting<bool>("enableTerrainGaussianStochasticSampling", false);
    settingsManager.registerSetting<bool>("enableParallaxOcclusionMapping", true);
    settingsManager.registerSetting<bool>("enableTerrainParallaxOcclusionMapping", true);
    settingsManager.registerSetting<bool>(
        "enableTerrainRegionMaterialEvaluation",
        m_pipelineRecipe.Contains<br::pipeline::TerrainRegionMaterialEvaluationTechnique>());
    settingsManager.registerSetting<bool>(
        "enableTerrainRvt",
        m_pipelineRecipe.Contains<br::pipeline::TerrainRvtTechnique>());
    settingsManager.registerSetting<bool>("forceDirectTerrainRvtFallback", false);
    settingsManager.registerSetting<bool>(TerrainRvtTelemetryDebugSettingName, false);
    settingsManager.registerSetting<uint32_t>("terrainRvtDebugView", 0u);
    settingsManager.registerSetting<uint32_t>("terrainRvtPageSize", 128u);
    settingsManager.registerSetting<uint32_t>("terrainRvtBorderTexels", 4u);
    settingsManager.registerSetting<uint32_t>("terrainRvtPhysicalAtlasPagesWide", 48u);
    settingsManager.registerSetting<uint32_t>("terrainRvtPhysicalAtlasPagesHigh", 48u);
    settingsManager.registerSetting<uint32_t>("terrainRvtPhysicalAtlasPoolCount", 1u);
    settingsManager.registerSetting<uint32_t>("terrainRvtClipPageTableResolution", 128u);
    settingsManager.registerSetting<uint32_t>("terrainRvtMaxTerrainSets", 2u);
    settingsManager.registerSetting<uint32_t>("terrainRvtMaxClipLevels", 16u);
    settingsManager.registerSetting<uint32_t>("terrainRvtMaxGeneratedPagesPerFrame", 1024u);
    settingsManager.registerSetting<uint32_t>("terrainRvtMipCount", 14u);
    settingsManager.registerSetting<float>("terrainRvtMipOffset", -0.5f);
    settingsManager.registerSetting<float>("terrainRvtSourceTexelsPerWorld", 24.0f);
    settingsManager.registerSetting<float>("terrainRvtBasePageWorldSize", 128.0f / 24.0f);
    settingsManager.registerSetting<bool>("enableTerrainReyesDisplacement", false);
    settingsManager.registerSetting<float>("terrainReyesDisplacementScale", 8.0f);
    settingsManager.registerSetting<float>("terrainReyesDisplacementGlobalScale", 1.0f);
    settingsManager.registerSetting<float>("objectReyesDisplacementScale", 500.0f);
    settingsManager.registerSetting<float>("objectParallaxHeightScale", 1.0f);
    settingsManager.registerSetting<float>("terrainRvtMipOffset", -0.06f);
    settingsManager.registerSetting<float>("terrainParallaxHeightScale", 0.10f);
    settingsManager.registerSetting<uint32_t>("terrainParallaxMaxSteps", 25u);
    settingsManager.registerSetting<float>("terrainParallaxFadeStartDistance", 2000.0f);
    settingsManager.registerSetting<float>("terrainParallaxFadeEndDistance", 3000.0f);
    settingsManager.registerSetting<DirectX::XMUINT3>("lightClusterSize", m_lightClusterSize);
    settingsManager.registerSetting<bool>("collectPassStatistics", true);
    settingsManager.registerSetting<bool>("collectPipelineStatistics", false);
	// This feels like abuse of the settings manager, but it's the easiest way to get the renderable objects to the menu
    settingsManager.registerSetting<std::function<flecs::entity()>>("getSceneRoot", [this]() -> flecs::entity {
        if (m_externalSceneMode) {
            return m_sceneRenderBridge.GetSceneRoot();
        }
        if (!currentScene || m_sceneTaskInFlight.load()) {
            return {};
        }
        return currentScene->GetRoot();
        });
    settingsManager.registerSetting<std::function<void(uint64_t, DirectX::XMFLOAT3)>>("queueSceneNodePositionEdit", [this](uint64_t stableSceneID, DirectX::XMFLOAT3 position) {
        QueueSceneNodePositionEdit(stableSceneID, position);
        });
    settingsManager.registerSetting<std::function<void(uint64_t, float)>>("queueSceneNodeUniformScaleEdit", [this](uint64_t stableSceneID, float uniformScale) {
        QueueSceneNodeUniformScaleEdit(stableSceneID, uniformScale);
        });
    bool meshShaderSupported = DeviceManager::GetInstance().GetMeshShadersSupported();
	settingsManager.registerSetting<bool>("enableMeshShader", meshShaderSupported && m_useMeshShaders);
	settingsManager.registerSetting<bool>("enableIndirectDraws", meshShaderSupported);
	settingsManager.registerSetting<bool>("enableGTAO", m_gtaoEnabled);
	settingsManager.registerSetting<bool>("enableOcclusionCulling", m_occlusionCulling);
    settingsManager.registerSetting<CLodCullingBackend>(CLodCullingBackendSettingName, CLodCullingBackend::PureCompute);
    settingsManager.registerSetting<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName, CLodSoftwareRasterMode::Compute);
    settingsManager.registerSetting<CLodVSMRasterMode>(CLodVSMRasterModeSettingName, CLodVSMRasterMode::Standard);
    settingsManager.registerSetting<CLodTransparencyMode>(CLodTransparencyModeSettingName, CLodTransparencyMode::Disabled);
    settingsManager.registerSetting<CLodLodHeightMode>(CLodLodHeightModeSettingName, CLodLodHeightMode::RenderHeight);
    settingsManager.registerSetting<bool>(CLodEnablePageJobVSMSettingName, true);
    settingsManager.registerSetting<bool>(
        CLodDisableNonVoxelVisibilitySettingName,
        ReadTruthyEnvironmentFlag("SARP_CLOD_DISABLE_NON_VOXEL_VISIBILITY"));
    settingsManager.registerSetting<bool>(CLodReyesUseNormalMapsSettingName, false);
    settingsManager.registerSetting<bool>(CLodReyesGeometricNormalSettingName, true);
    settingsManager.registerSetting<float>(CLodReyesObjectNormalMapBlendSettingName, CLodReyesObjectNormalMapBlendDefault);
    settingsManager.registerSetting<float>(CLodReyesTerrainNormalBlendSettingName, CLodReyesTerrainNormalBlendDefault);
    settingsManager.registerSetting<uint32_t>(CLodReyesTerrainNormalMipBiasSettingName, CLodReyesTerrainNormalMipBiasDefault);
    settingsManager.registerSetting<float>(CLodReyesDiceRatePixelsSettingName, CLodReyesDiceRatePixelsDefault);
    settingsManager.registerSetting<bool>(CLodReyesUseAabbOcclusionSettingName, false);
    settingsManager.registerSetting<bool>(CLodWorkGraphReyesVisibilitySettingName, false);
    settingsManager.registerSetting<bool>(CLodWorkGraphRigidOnlySettingName, false);
    settingsManager.registerSetting<float>(
        CLodReyesShadowCoarseTargetPagesPerTriangleSettingName,
        CLodReyesShadowCoarseTargetPagesPerTriangleDefault);
    settingsManager.registerSetting<uint32_t>(CLodPageJobDiameterThresholdSettingName, 64u);
    settingsManager.registerSetting<uint32_t>(CLodSoftwareRasterDiameterThresholdSettingName, 16u);
    settingsManager.registerSetting<uint32_t>(CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName, 32u);
    settingsManager.registerSetting<float>(CLodPageJobSparseRatioSettingName, 0.5f);
    settingsManager.registerSetting<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName, 32u);
    settingsManager.registerSetting<uint32_t>(CLodPageJobRecordCapacitySettingName, CLodPageJobDefaultRecordCapacity);
    settingsManager.registerSetting<bool>(CLodPageJobForceAllSettingName, false);
    settingsManager.registerSetting<uint32_t>(CLodForceTraversalDepthRootSettingName, CLodForceTraversalDepthRootDisabled);
    settingsManager.registerSetting<uint32_t>(CLodVisibleClusterCapacitySettingName, CLodDefaultVisibleClusterCapacity);
    settingsManager.registerSetting<bool>(CLodFrustumCullingSettingName, true);
    settingsManager.registerSetting<uint32_t>(
        CLodPureComputePhase2ExpansionFactorSettingName,
        CLodPureComputePhase2ExpansionFactorDefault);
    settingsManager.registerSetting<uint32_t>(
        CLodPureComputeReplayExpansionFactorSettingName,
        CLodPureComputeReplayExpansionFactorDefault);
    settingsManager.registerSetting<bool>("enableBloom", m_bloom);
    settingsManager.registerSetting<bool>("enableJitter", m_jitter);
    settingsManager.registerSetting<std::function<std::shared_ptr<Scene>(std::shared_ptr<Scene>)>>("appendScene", [this](std::shared_ptr<Scene> scene) -> std::shared_ptr<Scene> {
        return AppendScene(scene);
        });
	settingsManager.registerSetting<bool>("enableScreenSpaceReflections", m_screenSpaceReflections);
    settingsManager.registerSetting<bool>("enableRayTracedReflections", m_rayTracedReflections);
    settingsManager.registerSetting<float>("rayTracedReflectionMaxDistance", 100.0f);
    settingsManager.registerSetting<float>("rayTracedReflectionRoughnessCutoff", 1.0f);
    settingsManager.registerSetting<float>("rayTracedReflectionLodBias", 0.0f);
    settingsManager.registerSetting<bool>("useAsyncCompute", false);
    settingsManager.registerSetting<bool>("enableSceneRenderOverlap", m_sceneRenderOverlapEnabled);
	settingsManager.registerSetting<bool>(MaterialTextureStreamingSettingName, true);
	settingsManager.registerSetting<bool>("renderGraphCompileDumpEnabled", false);
    settingsManager.registerSetting<bool>(
        "renderGraphVramDumpEnabled",
        ReadTruthyEnvironmentFlag("BASICRENDERER_RENDER_GRAPH_VRAM_DUMP"));
    settingsManager.registerSetting<bool>("renderGraphQueueSyncTraceEnabled", false);
	settingsManager.registerSetting<AutoAliasMode>("autoAliasMode", AutoAliasMode::Balanced);
    settingsManager.registerSetting<AutoAliasPackingStrategy>("autoAliasPackingStrategy", AutoAliasPackingStrategy::GreedySweepLine);
    settingsManager.registerSetting<bool>("autoAliasEnableLogging", false);
    settingsManager.registerSetting<bool>("autoAliasLogExclusionReasons", false);
    settingsManager.registerSetting<bool>("autoAliasBuildDebugData", false);
    settingsManager.registerSetting<bool>("queueSchedulingEnableLogging", false);
    settingsManager.registerSetting<uint8_t>("queueSchedulingSelectionPolicy", static_cast<uint8_t>(org::runtime::QueueSchedulingSelectionPolicy::FirstFit));
    settingsManager.registerSetting<float>("queueSchedulingWidthScale", 0.0f); // Disable multi-queue scheduling
    settingsManager.registerSetting<float>("queueSchedulingPenaltyBias", 0.0f);
    settingsManager.registerSetting<float>("queueSchedulingMinPenalty", 1.0f);
    settingsManager.registerSetting<float>("queueSchedulingResourcePressureWeight", 1.0f);
    settingsManager.registerSetting<float>("queueSchedulingUavPressureWeight", 0.5f);
    settingsManager.registerSetting<float>("queueSchedulingAutoGraphicsBias", 2.5f);
    settingsManager.registerSetting<float>("queueSchedulingAsyncOverlapBonus", 3.0f);
    settingsManager.registerSetting<float>("queueSchedulingCrossQueueHandoffPenalty", 2.0f);
	settingsManager.registerSetting<uint32_t>("autoAliasPoolRetireIdleFrames", 120u);
	settingsManager.registerSetting<float>("autoAliasPoolGrowthHeadroom", 1.5f);
    settingsManager.registerSetting<uint8_t>("transitionPlacementMode", static_cast<uint8_t>(org::runtime::TransitionPlacementMode::CanonicalThenOptimize));
    settingsManager.registerSetting<bool>(
        "heavyDebug",
        ReadTruthyEnvironmentFlag("BASICRENDERER_RENDER_GRAPH_HEAVY_DEBUG"));
    settingsManager.registerSetting<bool>(CLodVisibilityTelemetryDebugSettingName, false);
    settingsManager.registerSetting<bool>(CLodVirtualShadowTelemetryDebugSettingName, false);
    settingsManager.registerSetting<bool>(ObjectReyesAtlasTelemetryDebugSettingName, false);
    settingsManager.registerSetting<uint32_t>(CLodStreamingCpuUploadBudgetSettingName, 500u);
    settingsManager.registerSetting<bool>(CLodStreamingEnableDirectStorageSettingName, true);
    settingsManager.registerSetting<bool>(
        CLodDisableReyesRasterizationSettingName,
        m_pipelineRecipe.Options<br::pipeline::ClusterLodTechnique>().reyes == br::pipeline::ReyesMode::Disabled);
	settingsManager.registerSetting<bool>(CLodDisableVirtualShadowPageCachingSettingName, false);
    settingsManager.registerSetting<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName, CLodVirtualShadowDefaultBackingResolution);
    settingsManager.registerSetting<uint32_t>(
        CLodDirectionalVirtualShadowMaxPhysicalPagesSettingName,
        ReadUintEnvironmentValue(
            "SARP_CLOD_VSM_MAX_PHYSICAL_PAGES",
            4096u));
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowLodBiasSettingName, CLodVirtualShadowDefaultDirectionalLodBias);
    settingsManager.registerSetting<bool>(
        CLodDirectionalVirtualShadowAutoLodBiasSettingName,
        !ReadTruthyEnvironmentFlag(
            "SARP_CLOD_VSM_DISABLE_AUTO_LOD_BIAS"));
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowAutoLodBiasScaleSettingName, 1.0f);
    settingsManager.registerSetting<bool>(CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName, true);
    settingsManager.registerSetting<uint32_t>(
        CLodDirectionalVirtualShadowPageRenderBudgetSettingName,
        ReadUintEnvironmentValue("SARP_CLOD_VSM_PAGE_BUDGET", 500u));
    settingsManager.registerSetting<uint32_t>(
        CLodDirectionalVirtualShadowUpgradePageRenderBudgetSettingName,
        ReadUintEnvironmentValue("SARP_CLOD_VSM_UPGRADE_PAGE_BUDGET", 500u));
    settingsManager.registerSetting<bool>(
        CLodDirectionalVirtualShadowReceiverSubpageMaskSettingName,
        false);
    settingsManager.registerSetting<uint32_t>(
        CLodDirectionalVirtualShadowReceiverSubpageModeSettingName,
        CLodVirtualShadowReceiverSubpageModeOff);
    settingsManager.registerSetting<bool>(CLodDynamicWindBoundsCacheEnabledSettingName, false);
    settingsManager.registerSetting<uint32_t>(CLodDynamicWindBoundsCacheMiBSettingName, 16u);
    settingsManager.registerSetting<bool>(CLodDynamicWindVertexCacheEnabledSettingName, false);
    settingsManager.registerSetting<uint32_t>(CLodDynamicWindVertexCacheMiBSettingName, 64u);
    settingsManager.registerSetting<bool>(
        CLodDirectionalVirtualShadowDynamicContentFilterSettingName,
        false);
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowSourceAngleDegreesSettingName, CLodVirtualShadowDefaultDirectionalSourceAngleDegrees);
    settingsManager.registerSetting<uint32_t>(CLodDirectionalVirtualShadowSmrtRayCountDirectionalSettingName, CLodVirtualShadowDefaultSmrtRayCountDirectional);
    settingsManager.registerSetting<uint32_t>(CLodDirectionalVirtualShadowSmrtSamplesPerRayDirectionalSettingName, CLodVirtualShadowDefaultSmrtSamplesPerRayDirectional);
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowSmrtMaxRayAngleFromLightDegreesSettingName, CLodVirtualShadowDefaultSmrtMaxRayAngleFromLightDegrees);
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowSmrtRayLengthScaleDirectionalSettingName, CLodVirtualShadowDefaultSmrtRayLengthScaleDirectional);
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowSmrtMaxTraceDistanceWorldSettingName, CLodVirtualShadowDefaultSmrtMaxTraceDistanceWorld);
    settingsManager.registerSetting<bool>(CLodDirectionalVirtualShadowReceiverTraceEnabledSettingName, CLodVirtualShadowDefaultReceiverTraceEnabled);
    settingsManager.registerSetting<uint32_t>(CLodDirectionalVirtualShadowReceiverTraceSampleCountSettingName, CLodVirtualShadowDefaultReceiverTraceSampleCount);
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowReceiverTraceMaxDistanceWorldSettingName, CLodVirtualShadowDefaultReceiverTraceMaxDistanceWorld);
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowReceiverTraceUncertaintyScaleSettingName, CLodVirtualShadowDefaultReceiverTraceUncertaintyScale);
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowReceiverTraceDepthSafetyScaleSettingName, CLodVirtualShadowDefaultReceiverTraceDepthSafetyScale);
	settingsManager.registerSetting<uint32_t>(CLodReyesResourceBudgetBytesSettingName, 512u*1024u*1024u*1u); // 1GB for reyes
	settingsManager.registerSetting<uint32_t>("usdPointInstancerMaxInstances", 10000u);
    getShadowResolution = settingsManager.getSettingGetter<uint16_t>("shadowResolution");
    setCameraSpeed = settingsManager.getSettingSetter<float>("cameraSpeed");
	getCameraSpeed = settingsManager.getSettingGetter<float>("cameraSpeed");
	setWireframeEnabled = settingsManager.getSettingSetter<bool>("enableWireframe");
	getWireframeEnabled = settingsManager.getSettingGetter<bool>("enableWireframe");
	setShadowsEnabled = settingsManager.getSettingSetter<bool>("enableShadows");
	getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
	getSkyboxResolution = settingsManager.getSettingGetter<uint16_t>("skyboxResolution");
	setImageBasedLightingEnabled = settingsManager.getSettingSetter<bool>("enableImageBasedLighting");
	setEnvironment = settingsManager.getSettingSetter<std::string>("environmentName");
	getMeshShadersEnabled = settingsManager.getSettingGetter<bool>("enableMeshShader");
	getIndirectDrawsEnabled = settingsManager.getSettingGetter<bool>("enableIndirectDraws");
	getDrawBoundingSpheres = settingsManager.getSettingGetter<bool>("drawBoundingSpheres");
	getImageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting");
    

    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableShadows", [this](const bool& newValue) {
        if (m_syncingPipelineTopologySettings) {
            return;
        }
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) {
            recipe.Add<br::pipeline::ClusterLodShadowTechnique>(
                recipe.Options<br::pipeline::ClusterLodTechnique>());
        }
        else {
            recipe.Remove<br::pipeline::ClusterLodShadowTechnique>();
        }
        RequestPipelineReplacement(std::move(recipe));
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<std::string>("environmentName", [this](const std::string& newValue) {
		SetEnvironmentInternal(s2ws(newValue));
		rebuildRenderGraph = true;
		}));
    bool outputTypeRequiresRenderGraphRebuild =
        OutputTypeRequiresRenderGraphRebuild(settingsManager.getSettingGetter<unsigned int>("outputType")());
    m_settingsSubscriptions.push_back(settingsManager.addObserver<unsigned int>("outputType", [this, outputTypeRequiresRenderGraphRebuild](const unsigned int& newValue) mutable {
        ::ResourceManager::GetInstance().SetOutputType(newValue);
        const bool newOutputTypeRequiresRenderGraphRebuild = OutputTypeRequiresRenderGraphRebuild(newValue);
        if (newOutputTypeRequiresRenderGraphRebuild != outputTypeRequiresRenderGraphRebuild) {
            rebuildRenderGraph = true;
        }
        outputTypeRequiresRenderGraphRebuild = newOutputTypeRequiresRenderGraphRebuild;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableMeshShader", [this](const bool& newValue) {
		ToggleMeshShaders(newValue);
		rebuildRenderGraph = true;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableWireframe", [this](const bool& newValue) {
		rebuildRenderGraph = true;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableIndirectDraws", [this](const bool& newValue) {
		rebuildRenderGraph = true;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("allowTearing", [this](const bool& newValue) {
		m_allowTearing = newValue;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("drawBoundingSpheres", [this](const bool& newValue) {
		rebuildRenderGraph = true;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableClusteredLighting", [this](const bool& newValue) {
		m_clusteredLighting = newValue;
		if (m_syncingPipelineTopologySettings) return;
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) recipe.Add<br::pipeline::ClusteredLightingTechnique>();
        else recipe.Remove<br::pipeline::ClusteredLightingTechnique>();
        RequestPipelineReplacement(std::move(recipe));
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableImageBasedLighting", [this](const bool& newValue) {
		m_imageBasedLighting = newValue;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableGTAO", [this](const bool& newValue) {
		m_gtaoEnabled = newValue;
		if (m_syncingPipelineTopologySettings) return;
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) recipe.Add<br::pipeline::GtaoTechnique>();
        else recipe.Remove<br::pipeline::GtaoTechnique>();
        RequestPipelineReplacement(std::move(recipe));
		}));
	m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableVisibilityRendering", [this](const bool& newValue) {
		m_visibilityRendering = newValue;
		rebuildRenderGraph = true;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableTerrainRegionMaterialEvaluation", [this](const bool& newValue) {
        if (m_syncingPipelineTopologySettings) return;
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) recipe.Add<br::pipeline::TerrainRegionMaterialEvaluationTechnique>();
        else recipe.Remove<br::pipeline::TerrainRegionMaterialEvaluationTechnique>();
        RequestPipelineReplacement(std::move(recipe));
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableTerrainRvt", [this](const bool& newValue) {
        if (m_syncingPipelineTopologySettings) {
            return;
        }
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) {
            recipe.Add<br::pipeline::TerrainRvtTechnique>();
        }
        else {
            recipe.Remove<br::pipeline::TerrainRvtTechnique>();
        }
        RequestPipelineReplacement(std::move(recipe));
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPageSize", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtBorderTexels", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPhysicalAtlasPagesWide", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPhysicalAtlasPagesHigh", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPhysicalAtlasPoolCount", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtClipPageTableResolution", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtMaxTerrainSets", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtMaxClipLevels", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableOcclusionCulling", [this](const bool& newValue) {
		m_occlusionCulling = newValue;
		rebuildRenderGraph = true;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodCullingBackend>(CLodCullingBackendSettingName, [this](const CLodCullingBackend& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
        m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableSceneRenderOverlap", [this](const bool& newValue) {
                SetSceneRenderOverlapEnabled(newValue);
                }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName, [this](const CLodSoftwareRasterMode& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodVSMRasterMode>(CLodVSMRasterModeSettingName, [this](const CLodVSMRasterMode& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodTransparencyMode>(CLodTransparencyModeSettingName, [this](const CLodTransparencyMode& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodEnablePageJobVSMSettingName, [this](const bool& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodPageJobDiameterThresholdSettingName, [this](const uint32_t& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<float>(CLodPageJobSparseRatioSettingName, [this](const float& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodPageJobMaxPagesPerClusterSettingName, [this](const uint32_t& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodPageJobRecordCapacitySettingName, [this](const uint32_t& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodPageJobForceAllSettingName, [this](const bool& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodDynamicWindBoundsCacheEnabledSettingName, [this](const bool&) {
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodDynamicWindBoundsCacheMiBSettingName, [this](const uint32_t&) {
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodDynamicWindVertexCacheEnabledSettingName, [this](const bool&) {
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodDynamicWindVertexCacheMiBSettingName, [this](const uint32_t&) {
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodDirectionalVirtualShadowReceiverSubpageModeSettingName, [this](const uint32_t&) {
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodVisibleClusterCapacitySettingName, [this](const uint32_t& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodWorkGraphReyesVisibilitySettingName, [this](const bool& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodWorkGraphRigidOnlySettingName, [this](const bool& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
        m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodDisableReyesRasterizationSettingName, [this](const bool& newValue) {
            if (m_syncingPipelineTopologySettings) {
                return;
            }
            auto recipe = GetPipelineRecipeForMutation();
            auto options = recipe.Options<br::pipeline::ClusterLodTechnique>();
            options.reyes = newValue ? br::pipeline::ReyesMode::Disabled : br::pipeline::ReyesMode::Enabled;
            recipe.Configure<br::pipeline::ClusterLodTechnique>(options);
            if (recipe.Contains<br::pipeline::ClusterLodAlphaTechnique>()) {
                recipe.Configure<br::pipeline::ClusterLodAlphaTechnique>(options);
            }
            if (recipe.Contains<br::pipeline::ClusterLodShadowTechnique>()) {
                recipe.Configure<br::pipeline::ClusterLodShadowTechnique>(options);
            }
            RequestPipelineReplacement(std::move(recipe));
            }));
        m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName, [this](const uint32_t& newValue) {
            (void)newValue;
            rebuildRenderGraph = true;
            }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodReyesResourceBudgetBytesSettingName, [this](const uint32_t& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableBloom", [this](const bool& newValue) {
        m_bloom = newValue;
        if (m_syncingPipelineTopologySettings) return;
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) recipe.Add<br::pipeline::BloomTechnique>();
        else recipe.Remove<br::pipeline::BloomTechnique>();
        RequestPipelineReplacement(std::move(recipe));
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableJitter", [this](const bool& newValue) {
        m_jitter = newValue;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableReShape", [this](const bool& newValue) {
        (void)newValue;
        if (m_isInitialized) {
            spdlog::warn("Changing enableReShape requires device recreation to take effect.");
        }
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("reshapeTexelAddressing", [this](const bool& newValue) {
        (void)newValue;
        if (m_isInitialized) {
            spdlog::warn("Changing reshapeTexelAddressing requires device recreation to take effect.");
        }
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("reshapeSynchronousRecording", [this](const bool& newValue) {
        auto result = rhi::debug::SetSynchronousRecording(m_device, newValue);
        if (rhi::IsOk(result)) {
            spdlog::info("GPU-Reshape: runtime synchronous recording set to {}", newValue);
        }
        if (!rhi::IsOk(result) && result != rhi::Result::Unsupported) {
            spdlog::warn("Failed to update runtime instrumentation synchronous recording state: {}", static_cast<uint32_t>(result));
        } else if (result == rhi::Result::Unsupported) {
            spdlog::warn("GPU-Reshape: runtime synchronous recording update is unsupported by the active device/backend");
        }
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint64_t>("reshapeGlobalFeatureMask", [this](const uint64_t& newValue) {
        auto result = rhi::debug::SetGlobalInstrumentationMask(m_device, newValue);
        if (rhi::IsOk(result)) {
            spdlog::info("GPU-Reshape: runtime global feature mask set to 0x{:016X}", newValue);
        }
        if (!rhi::IsOk(result) && result != rhi::Result::Unsupported) {
            spdlog::warn("Failed to update runtime instrumentation feature mask: {}", static_cast<uint32_t>(result));
        } else if (result == rhi::Result::Unsupported) {
            spdlog::warn("GPU-Reshape: runtime global feature mask update is unsupported by the active device/backend");
        }
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint8_t>("numDirectionalLightCascades", [](const uint8_t& newValue) {
		auto& settingsManager = SettingsManager::GetInstance();
        const float zNear = 0.1f;
        const float zFar = settingsManager.getSettingGetter<float>("maxShadowDistance")();
        settingsManager.getSettingSetter<std::vector<float>>("directionalLightCascadeSplits")(calculateCascadeSplits(newValue, zNear, zFar, zFar));
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<std::vector<float>>("directionalLightCascadeSplits", [this](const std::vector<float>& newValue) {
        ::ResourceManager::GetInstance().SetDirectionalCascadeSplits(newValue);
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<UpscalingMode>("upscalingMode", [this](const UpscalingMode& newValue) {

        m_preFrameDeferredFunctions.defer([newValue, this]() { // Don't do this during a frame
            StallPipeline(); // Wait for all GPU work before destroying contexts
            UpscalingManager::GetInstance().Shutdown();
            UpscalingManager::GetInstance().InitFFX(); // Needs device
            UpscalingManager::GetInstance().SetUpscalingMode(newValue);
            UpscalingManager::GetInstance().Setup();

            FFXManager::GetInstance().Shutdown();
            FFXManager::GetInstance().InitFFX();

            CreateTextures();
            const auto ingestionConfiguration = CaptureSceneIngestionConfiguration();
            m_sceneRenderBridge.ResyncPrimaryCameraDepth(m_sceneIngestionServices,
                ingestionConfiguration.renderResolution.x, ingestionConfiguration.renderResolution.y,
                PrimaryCameraLodHeight(ingestionConfiguration));
            rebuildRenderGraph = true;
            });
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<UpscaleQualityMode>("upscalingQualityMode", [this](const UpscaleQualityMode& newValue) {

        m_preFrameDeferredFunctions.defer([newValue, this]() { // Don't do this during a frame
            StallPipeline(); // Wait for all GPU work before destroying contexts
            UpscalingManager::GetInstance().SetUpscalingQualityMode(newValue);
            UpscalingManager::GetInstance().Shutdown();
            UpscalingManager::GetInstance().InitFFX(); // Recreate FSR context before Setup queries it
            UpscalingManager::GetInstance().Setup();
            FFXManager::GetInstance().Shutdown();
            FFXManager::GetInstance().InitFFX();
            CreateTextures();
            const auto ingestionConfiguration = CaptureSceneIngestionConfiguration();
            m_sceneRenderBridge.ResyncPrimaryCameraDepth(m_sceneIngestionServices,
                ingestionConfiguration.renderResolution.x, ingestionConfiguration.renderResolution.y,
                PrimaryCameraLodHeight(ingestionConfiguration));
            rebuildRenderGraph = true;
            });
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableDilatedMotionVectors", [this](const bool&) {
        m_preFrameDeferredFunctions.defer([this]() {
            UpscalingManager::GetInstance().RequestHistoryReset();
            rebuildRenderGraph = true;
        });
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<WindowResolutionPreset>(
        WindowResolutionPresetSettingName,
        [this](const WindowResolutionPreset& newValue) {
            m_preFrameDeferredFunctions.defer([newValue, this]() {
                ApplyWindowResolutionPreset(newValue);
            });
        }));
	m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableScreenSpaceReflections", [this](const bool& newValue) {
		m_screenSpaceReflections = newValue;
		rebuildRenderGraph = true;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableRayTracedReflections", [this](const bool& newValue) {
        m_rayTracedReflections = newValue;
        if (newValue && !DeviceManager::GetInstance().GetCLodRayTracingSupported() && !m_warnedRayTracedReflectionsUnsupported) {
            m_warnedRayTracedReflectionsUnsupported = true;
            spdlog::warn("Ray traced reflections requested, but clustered ray tracing is not supported by the active RHI backend/device.");
        }
        rebuildRenderGraph = true;
        }));


	// Indirect draws require mesh shaders (due to not having implemented indirect draws with traditional pipelines)
	settingsManager.addImplicationConstraint("enableIndirectDraws", "enableMeshShader");

	// Visibility rendering requires mesh shaders (due to not having implemented visibility VS)
    settingsManager.addImplicationConstraint("enableVisibilityRendering", "enableMeshShader");

    //Visibility rendering requires indirect draws (because of a bug) TODO: fix
	settingsManager.addImplicationConstraint("enableVisibilityRendering", "enableIndirectDraws");
}

void Renderer::ToggleMeshShaders(bool useMeshShaders) {
    // We need to:
    // 1. Remove all meshes in the global mesh library from the mesh manager
	// 2. Re-add them to the mesh manager
    // 3. Get all objects with mesh instances by querying the ECS
	// 4. Remove and re-add all instances to the mesh manager
	// 5. Remove and re-add all objects from the object manager to rebuild indirect draw info

    auto& world = RendererECSManager::GetInstance().GetWorld();
	auto& meshLibrary = world.get_mut<Components::GlobalMeshLibrary>().meshes;

	// Remove all meshes from the mesh manager
	for (auto& meshPair : meshLibrary) {
		auto& mesh = meshPair.second;
		m_pMeshManager->RemoveMesh(mesh.lock().get());
	}
	// Re-add them to the mesh manager
	for (auto& meshPair : meshLibrary) {
		auto& mesh = meshPair.second;
        auto ptr = mesh.lock();
        m_pMeshManager->AddMesh(ptr, useMeshShaders);
	}

	// Get all active objects with mesh instances by querying the ECS
    auto query = world.query_builder<Components::RenderableObject, Components::ObjectDrawInfo>().with<Components::Active>()
        .build();

    world.defer_begin();
    query.each([&](flecs::entity entity, Components::RenderableObject& object, const Components::ObjectDrawInfo& drawInfo) {
        auto meshInstances = entity.try_get<Components::MeshInstances>();

        if (meshInstances) {
            for (auto& meshInstance : meshInstances->meshInstances) {
                m_pMeshManager->RemoveMeshInstance(meshInstance.get());
                m_pMeshManager->AddMeshInstance(meshInstance.get(), useMeshShaders);
            }
        }

		// Remove and re-add all objects from the object manager to rebuild indirect draw info
		m_pObjectManager->RemoveObject(&drawInfo);
		auto newDrawInfo = m_pObjectManager->AddObject(object.perObjectCB, meshInstances);
		object.perObjectCB.normalMatrixBufferIndex = newDrawInfo.normalMatrixIndex;
		entity.set<Components::ObjectDrawInfo>(newDrawInfo);
		entity.add<Components::RenderTransformUpdated>();
            });
    world.defer_end();
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
        m_backbufferResources[n] = std::make_shared<ExternalTextureResource>(
            renderTargets[n], x_res, y_res, rhi::Format::R8G8B8A8_UNorm);
        m_backbufferResources[n]->SetName("Backbuffer " + std::to_string(n));
    }
    m_dynamicBackbuffer = std::make_shared<DynamicResource>(m_backbufferResources[0]);
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

void Renderer::CreateTextures() {
    auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    // Create HDR color target
    TextureDescription hdrDesc;
    hdrDesc.arraySize = 1;
    hdrDesc.channels = 4; // RGBA
    hdrDesc.isCubemap = false;
    hdrDesc.hasRTV = true;
    hdrDesc.hasUAV = true;
    hdrDesc.hasNonShaderVisibleUAV = true;
    hdrDesc.format = rhi::Format::R16G16B16A16_Float; // HDR format
    hdrDesc.generateMipMaps = false; // For bloom downsampling
    ImageDimensions dims;
    dims.height = resolution.y;
    dims.width = resolution.x;
    hdrDesc.imageDimensions.push_back(dims);
    hdrDesc.allowAlias = true;
    auto hdrColorTarget = PixelBuffer::CreateSharedUnmaterialized(hdrDesc);
    hdrColorTarget->SetName("Primary Camera HDR Color Target");
    org::memory::SetResourceUsageHint(*hdrColorTarget, "Primary color buffers");
	m_coreResourceProvider.m_HDRColorTarget = hdrColorTarget;

    auto outputResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    hdrDesc.imageDimensions[0].width = outputResolution.x;
    hdrDesc.imageDimensions[0].height = outputResolution.y;
    // Streamline's D3D12 interop operates on the native resource as a whole.
    // Keep its output single-mip; the optional bloom technique owns a separate
    // mip chain so removing bloom also removes that allocation.
    hdrDesc.generateMipMaps = false;
    hdrDesc.allowAlias = true;
	auto upscaledHDRColorTarget = PixelBuffer::CreateSharedUnmaterialized(hdrDesc);
	upscaledHDRColorTarget->SetName("Upscaled HDR Color Target");
    org::memory::SetResourceUsageHint(*upscaledHDRColorTarget, "Upscaled color buffers");
	m_coreResourceProvider.m_upscaledHDRColorTarget = upscaledHDRColorTarget;

    // Scene recording targets a logical-frame-owned LDR surface.  Only the
    // presentation copy depends on the acquired swapchain image, so compiled
    // scene work can eventually be planned and recorded before acquisition.
    TextureDescription presentationDesc{};
    presentationDesc.arraySize = 1;
    presentationDesc.channels = 4;
    presentationDesc.hasRTV = true;
    presentationDesc.format = rhi::Format::R8G8B8A8_UNorm;
    presentationDesc.rtvFormat = rhi::Format::R8G8B8A8_UNorm;
    presentationDesc.imageDimensions.push_back({outputResolution.x, outputResolution.y, 0, 0});
    presentationDesc.allowAlias = false;
    m_presentationColorResources.clear();
    m_presentationColorResources.reserve(m_numFramesInFlight);
    for (uint32_t slot = 0; slot < m_numFramesInFlight; ++slot) {
        auto target = PixelBuffer::CreateSharedUnmaterialized(presentationDesc);
        target->SetName("Frame Presentation Color " + std::to_string(slot));
        org::memory::SetResourceUsageHint(*target, "Frame presentation surfaces");
        m_presentationColorResources.push_back(std::move(target));
    }
    if (!m_presentationColorResources.empty()) {
        m_dynamicPresentationColor = std::make_shared<DynamicResource>(m_presentationColorResources.front());
        m_dynamicPresentationColor->SetName("Frame Presentation Color");
    }

    TextureDescription motionVectors;
    motionVectors.arraySize = 1;
    motionVectors.channels = 2;
    motionVectors.isCubemap = false;
    motionVectors.hasRTV = true;
    motionVectors.format = rhi::Format::R16G16_Float;
    motionVectors.generateMipMaps = false;
    motionVectors.hasSRV = true;
    motionVectors.hasUAV = true;
    motionVectors.hasNonShaderVisibleUAV = true;
    motionVectors.srvFormat = rhi::Format::R16G16_Float;
    ImageDimensions motionVectorsDims = { resolution.x, resolution.y, 0, 0 };
    motionVectors.imageDimensions.push_back(motionVectorsDims);
	motionVectors.allowAlias = true;
    auto dilatedMotionVectorsBuffer = PixelBuffer::CreateSharedUnmaterialized(motionVectors);
    dilatedMotionVectorsBuffer->SetName("Dilated Motion Vectors");
    org::memory::SetResourceUsageHint(*dilatedMotionVectorsBuffer, "Upscaling resources");
	m_coreResourceProvider.m_gbufferDilatedMotionVectors = dilatedMotionVectorsBuffer;
}

void Renderer::CreateRTVs() {
    auto device = DeviceManager::GetInstance().GetDevice();
    const bool renderGraphBatchTraceEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();
    const auto outputResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    // Recreate the render target views
    for (UINT n = 0; n < m_numFramesInFlight; n++) {
        renderTargets[n] = m_swapChain->Image(n);
        rhi::RtvDesc rtvDesc = {};
        rtvDesc.dimension = rhi::RtvDim::Texture2D;
        rtvDesc.formatOverride = rhi::Format::R8G8B8A8_UNorm;
        rtvDesc.range = { 0, 1, 0, 1 };
        device.CreateRenderTargetView({ rtvHeap->GetHandle(), n }, renderTargets[n], rtvDesc);

        // Keep external texture wrappers in sync after resize
        if (n < m_backbufferResources.size() && m_backbufferResources[n]) {
            m_backbufferResources[n]->SetHandle(renderTargets[n]);
            m_backbufferResources[n]->SetDimensions(outputResolution.x, outputResolution.y);
            m_backbufferResources[n]->SetRTVSlot({ rtvHeap->GetHandle(), n });
            m_backbufferResources[n]->ResetToUndefined();
            if (renderGraphBatchTraceEnabled) {
                spdlog::info(
                    "Renderer: CreateRTVs slot={} resourceID={} handle=({}, {}) rtv=({}, {})",
                    n,
                    m_backbufferResources[n]->GetGlobalResourceID(),
                    renderTargets[n].index,
                    renderTargets[n].generation,
                    rtvHeap->GetHandle().index,
                    n);
            }
        }
    }
}

void Renderer::OnResize(UINT newWidth, UINT newHeight) {
    spdlog::info(
        "Renderer: OnResize {}x{} frameIndex={} totalFramesRendered={}",
        newWidth,
        newHeight,
        static_cast<unsigned>(m_frameIndex),
        m_totalFramesRendered);
    // Wait for all in-flight GPU work before destroying resources
	StallPipeline();

    // Release the resources tied to the swap chain
    auto numFramesInFlight = getNumFramesInFlight();

    // Resize the swap chain
    m_swapChainReady = false;
    m_loggedSwapChainNotReady = false;
    auto resizeResult = m_swapChain->ResizeBuffers(m_numFramesInFlight, newWidth, newHeight, rhi::Format::R8G8B8A8_UNorm, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH); // TODO: Port flags to RHI
    if (resizeResult != rhi::Result::Ok) {
        spdlog::critical(
            "Renderer: ResizeBuffers failed during OnResize {}x{} result={} frameIndex={} totalFramesRendered={}",
            newWidth,
            newHeight,
            static_cast<unsigned>(resizeResult),
            static_cast<unsigned>(m_frameIndex),
            m_totalFramesRendered);
        return;
    }

	SettingsManager::GetInstance().getSettingSetter<DirectX::XMUINT2>("outputResolution")({ newWidth, newHeight });

    m_frameIndex = static_cast<uint8_t>(m_swapChain->CurrentImageIndex());

    CreateRTVs();
    m_swapChainReady = true;
    m_loggedSwapChainNotReady = false;

    UpscalingManager::GetInstance().Shutdown();
    UpscalingManager::GetInstance().Setup();
    FFXManager::GetInstance().Shutdown();
    FFXManager::GetInstance().InitFFX();

    CreateTextures();

    const auto ingestionConfiguration = CaptureSceneIngestionConfiguration();
    m_sceneRenderBridge.ResyncPrimaryCameraDepth(m_sceneIngestionServices,
        ingestionConfiguration.renderResolution.x, ingestionConfiguration.renderResolution.y,
        PrimaryCameraLodHeight(ingestionConfiguration));

	//Rebuild the render graph
	rebuildRenderGraph = true;
}

void Renderer::ApplyWindowResolutionPreset(WindowResolutionPreset preset)
{
    const auto resolution = ResolveWindowResolutionPreset(preset);
    const auto currentResolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    if (currentResolution.x == resolution.x && currentResolution.y == resolution.y) {
        return;
    }

    if (!m_hwnd) {
        OnResize(resolution.x, resolution.y);
        return;
    }

    RECT windowRect{
        0,
        0,
        static_cast<LONG>(resolution.x),
        static_cast<LONG>(resolution.y),
    };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(m_hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(m_hwnd, GWL_EXSTYLE));
    if (!AdjustWindowRectEx(&windowRect, style, FALSE, exStyle)) {
        spdlog::warn("Renderer: AdjustWindowRectEx failed while applying {}x{} window preset", resolution.x, resolution.y);
        return;
    }

    const int windowWidth = static_cast<int>(windowRect.right - windowRect.left);
    const int windowHeight = static_cast<int>(windowRect.bottom - windowRect.top);
    if (!SetWindowPos(
            m_hwnd,
            nullptr,
            0,
            0,
            windowWidth,
            windowHeight,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        spdlog::warn("Renderer: SetWindowPos failed while applying {}x{} window preset", resolution.x, resolution.y);
    }
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

void Renderer::Update(float elapsedSeconds) {
    if (m_deterministicSamplingMode) {
        elapsedSeconds = 0.0f;
    }
    BT_ZONE_SCOPE("Renderer::Update");
    // Capture input before camera application clears the per-frame mouse delta.
    // Used only by the opt-in camera telemetry stream below.
    const auto cameraTelemetryMovement = movementState;
    const float cameraTelemetryPitch = verticalAngle;
    const float cameraTelemetryYaw = horizontalAngle;
    // The previous accepted owner remains retained by its queued graph work.
    // Clear the renderer-side alias so any early return applies backpressure
    // instead of rendering the preceding logical frame twice.
    m_frameInputs.reset();
    BufferBase::ScopedBackingMutation frameBoundaryBackingMutation;

	std::vector<PSOManager::PipelineRetirementPoint> pipelineRetirementPoints;
	for (const auto& point : DescriptorHeapManager::GetInstance().GetQueueFenceSnapshot()) {
		pipelineRetirementPoints.push_back({ point.timeline, point.value });
	}
	PSOManager::GetInstance().PublishPendingLivePipelines(std::move(pipelineRetirementPoints));
	PSOManager::GetInstance().CollectRetiredLivePipelines();

    BeginFrameTaskGraphCapture();

    const auto runCapturedStage = [this](const char* stageName, auto&& stageFn) {
        const auto stageStart = std::chrono::steady_clock::now();
        try {
            stageFn();
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Renderer::Update stage '") + stageName + "' failed: " + error.what());
        }
        const auto stageEnd = std::chrono::steady_clock::now();
        RecordFrameTaskStage(stageName, br::telemetry::CpuTaskDomain::MainThread, stageStart, stageEnd);
    };

    runCapturedStage("PublishDeferredBackingResizesEarly", []() {
        BT_ZONE_SCOPE("Renderer::Update::PublishDeferredBackingResizesEarly");
        // Publication is opportunistic at the frame boundary. Waiting here
        // defeats the asynchronous resize path and can park the render thread
        // behind backing creation for tens of milliseconds.
        (void)PublishReadyDeferredBackingResizes(false);
    });

    if (!IsSceneReadyForFrame()) {
        return;
    }

    if (m_shaderReloadRequested) {
        runCapturedStage("ShaderReload", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::ShaderReload");
            spdlog::info("Renderer: draining GPU work before shader reload.");
            StallPipeline();
            std::string rebuildError;
            if (PSOManager::GetInstance().RebuildAllPipelines(rebuildError)) {
                rebuildRenderGraph = true;
            } else {
                spdlog::error("Renderer: global PSO rebuild failed: {}", rebuildError);
            }
            m_shaderReloadRequested = false;
        });
    }

    runCapturedStage("SceneExplorerEdits", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::SceneExplorerEdits");
        if (!m_externalSceneMode) {
            FlushPendingSceneExplorerEdits();
        }
    });
    if (m_externalSceneMode) {
        runCapturedStage("ExternalScene", []() {
            BT_ZONE_SCOPE("Renderer::Update::ExternalScene");
        });
    } else if (m_sceneRenderOverlapEnabled) {
        if (NeedsSceneSnapshotBootstrap()) {
            runCapturedStage("BootstrapSceneSnapshot", [&]() {
                BT_ZONE_SCOPE("Renderer::Update::BootstrapSceneSnapshot");
                ApplyPrimaryCameraInput(elapsedSeconds);
                RunGameUpdateStage(elapsedSeconds);
                RunTransformPropagationStage();
                BootstrapCommittedSceneSnapshot();
            });
        } else {
            runCapturedStage("CommitSceneSnapshot", [&]() {
                BT_ZONE_SCOPE("Renderer::Update::CommitSceneSnapshot");
                CommitCompletedSceneSnapshot();
            });
        }
    } else {
        runCapturedStage("SynchronousSceneUpdate", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::SynchronousSceneUpdate");
            ApplyPrimaryCameraInput(elapsedSeconds);
            RunGameUpdateStage(elapsedSeconds);
            RunTransformPropagationStage();
            RunSceneBridgeSyncStage();
        });
    }

    runCapturedStage("AnimationUpdate", [&]() {
		m_pSkeletonManager->BeginFrame(m_totalFramesRendered);
        if (m_externalSceneMode) {
            m_pSkeletonManager->UpdateAllDirtyInstances();
        } else {
            RunAnimationUpdateStage(elapsedSeconds);
        }
    });
    // Flush deferred functions before rebuilding the render graph so that
    // deferred state changes (e.g. environment creation from SetEnvironmentInternal)
    // are visible when the graph is constructed.
    if (!m_preFrameDeferredFunctions.empty()) {
        runCapturedStage("DeferredWorkEarly", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::DeferredWorkEarly");
            m_preFrameDeferredFunctions.flush();
        });
    }
    SyncOpenRenderGraphSettings(m_numFramesInFlight);
    const bool asyncFrameQueue = SettingsManager::GetInstance()
        .getSettingGetter<int>("experimentalAsyncCompileMode")() == 2;
    if (!asyncFrameQueue) m_preparationFrameIndex = m_frameIndex;
    ApplyPendingPipelineReplacement();

    if (rebuildRenderGraph) {
        runCapturedStage("RenderGraphBuild", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::RenderGraphBuild");
            try {
		        CreateRenderGraph();
                m_pipelineRollbackRecipe.reset();
            }
            catch (const std::exception& error) {
                if (!m_pipelineRollbackRecipe) {
                    throw;
                }
                HandlePipelineReplacementFailure(error);
                CreateRenderGraph();
            }
        });
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), "after RenderGraphBuild");
    }
    runCapturedStage("RenderResourceSync", [&]() {
        RunRenderResourceSyncStage();
    });

    auto& world = RendererECSManager::GetInstance().GetWorld();

    auto camera = GetValidatedPrimaryRenderCamera(false);
    if (!camera) {
        camera = GetValidatedPrimaryRenderCamera(true);
    }
    if (!camera) {
        spdlog::warn("Renderer: bridged primary camera is unavailable after scene sync. Skipping frame update work.");
        return;
    }
    unsigned int cameraIndex = m_pViewManager->Get(camera.get<Components::RenderViewRef>().viewID)->gpu.cameraBufferIndex;

    runCapturedStage("WaitForFrame", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::WaitForFrame");
        WaitForFrame(m_preparationFrameIndex);
        if (m_pObjectManager) {
            const std::uint64_t retireDelayFrames = static_cast<std::uint64_t>(m_numFramesInFlight) + 1u;
            const std::uint64_t safeFrameNumber = m_totalFramesRendered > retireDelayFrames
                ? m_totalFramesRendered - retireDelayFrames
                : 0u;
            m_pObjectManager->PublishDeferredRetireCompletedFrame(safeFrameNumber, retireDelayFrames);
        }
        DescriptorHeapManager::GetInstance().ProcessDeferredReleases(m_preparationFrameIndex);
        RendererECSManager::GetInstance().FlushDeferredWorldOperations();

		// Retire upload pages only after the previous use of this frame slot has
		// completed, and before CompileFrame can assign newly recorded uploads to
		// the slot.  Doing this in FrameMaintenance (after RenderGraph::Update)
		// recycled current-frame staging pages before RenderGraph::Execute had
		// submitted their copies.
		if (currentRenderGraph) {
			if (auto* uploadService = currentRenderGraph->GetUploadService()) {
				uploadService->ProcessDeferredReleases(m_preparationFrameIndex);
			}
		}
        });

    runCapturedStage("CommitPublishedRendererState", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState");
        try {
            if (m_rendererStatePublisher) {
                auto commit = [&] {
                    BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::PublisherCommit");
                    return m_rendererStatePublisher->Commit(m_preparationFrameIndex);
                }();
				if (m_asyncStateGraph && m_asyncStateGraph->TraceActive()) {
					m_asyncStateGraph->TraceEvent(
						commit.committed ? br::render::AsyncStateGraphTraceEventID::ManifestCommitAccepted :
							br::render::AsyncStateGraphTraceEventID::ManifestCommitUnchanged,
						{ br::render::ArtifactKind::FrameManifest, 0, 0 },
						commit.state ? commit.state->epoch : 0, 0,
						{ { m_preparationFrameIndex } });
					if (commit.committed && commit.state) {
						for (std::size_t index = 0; index < br::render::kPublishedFragmentCount; ++index) {
							const auto& fragment = commit.state->Fragment(
								static_cast<br::render::PublishedFragmentKind>(index));
							if (!fragment.publicationRoot) continue;
							m_asyncStateGraph->TraceEvent(br::render::AsyncStateGraphTraceEventID::ManifestFragmentCommitted,
								fragment.publicationRoot.address, fragment.publicationRoot.revision,
								fragment.publicationRoot.generation,
								{ { commit.state->epoch, m_preparationFrameIndex, index } });
						}
					}
				}
                {
                    BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::InstallFrameLease");
                m_context.publishedRendererState = commit.state;
                m_context.publishedManifestLease = commit.lease;
                }
                if (commit.committed && commit.state) {
                    // Versioned table families need the same publication
                    // acknowledgement as object/material tables. Without it,
                    // their bounded backing rings retain the original active
                    // generation forever; after a few camera updates the next
                    // view build suspends permanently on ring exhaustion.
                    const auto acknowledgeTables = [](const auto& families, const auto& state) {
                        if (!state) return;
                        const auto count = (std::min)(families.size(), state->tableVersions.size());
                        for (std::size_t i = 0; i < count; ++i) {
                            if (families[i]) families[i]->Acknowledge(state->tableVersions[i]);
                        }
                    };
                    acknowledgeTables(m_lightTableFamilies,
                        commit.state->lights.payload.Get<br::render::PublishedLightTableState>());
                    acknowledgeTables(m_poseTableFamilies,
                        commit.state->poses.payload.Get<br::render::PublishedPoseState>());
                }
                if (m_rendererStateRequests) {
                    m_rendererStateRequests->RefreshPublication();
                }
                if (commit.committed && commit.state && m_asyncStateGraph) {
                    std::array<br::render::ArtifactVersionID,
                        br::render::kPublishedFragmentCount> publishedRoots{};
                    std::size_t publishedRootCount = 0;
                    for (std::size_t index = 0; index < br::render::kPublishedFragmentCount; ++index) {
                        const auto root = commit.state->Fragment(
                            static_cast<br::render::PublishedFragmentKind>(index)).publicationRoot;
                        if (root) publishedRoots[publishedRootCount++] = root;
                    }
                    const bool acknowledgementSubmitted = TaskSchedulerManager::GetInstance().Submit(
                        m_rendererStateCommitScope, TaskLane::FrameCritical,
                        TaskDomain::GraphPublication, "RendererStatePublisher::MarkPublished",
                        [stateGraph = m_asyncStateGraph.get(), publishedRoots,
                            publishedRootCount](const br::TaskContext& context) {
                            if (!context.StopRequested() && stateGraph) {
                                stateGraph->MarkPublished(
                                    std::span(publishedRoots).first(publishedRootCount));
                            }
                        });
                    if (!acknowledgementSubmitted) {
                        spdlog::warn("Renderer-state graph publication acknowledgement was rejected");
                    }
                }
                if (commit.HasDeferredWork()) {
                    BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::ScheduleDeferredCommit");
                    auto* objectManager = commit.committed ? m_pObjectManager.get() : nullptr;
					auto* meshManager = commit.committed ? m_pMeshManager.get() : nullptr;
					auto* materialManager = commit.committed ? m_pMaterialManager.get() : nullptr;
                    auto committedState = commit.committed ? commit.state : nullptr;
                    // Publication acknowledgement advances object-buffer mutation
                    // coverage and wakes the next admissible graph cut.  Keeping it
                    // in Background/Cleanup allowed an import-time cleanup flood to
                    // strand otherwise committed renderer state indefinitely.  Run
                    // the deferred handoff off-thread in the serialized renderer-
                    // state domain, where streaming work has bounded precedence.
                    const bool submitted = TaskSchedulerManager::GetInstance().Submit(
                        m_rendererStateCommitScope, TaskLane::Streaming, TaskDomain::RendererState,
                        "RendererStatePublisher::DeferredCommit",
						[commit = std::move(commit), objectManager, meshManager, materialManager,
                            committedState = std::move(committedState)](
                            const br::TaskContext& context) mutable {
                            if (context.StopRequested()) return;
                            if (objectManager && committedState) {
                                objectManager->AcknowledgePublishedBufferState(committedState);
                            }
							if (meshManager && committedState) {
								meshManager->AcknowledgePublishedBufferState(committedState);
							}
							if (materialManager && committedState) {
								materialManager->AcknowledgePublishedTextureImageTable(committedState);
							}
                            commit.RunDeferred();
                        });
                    if (!submitted) {
                        spdlog::warn("Renderer-state deferred commit cleanup was rejected by the scheduler");
                    }
                }
            } else {
                m_context.publishedRendererState.reset();
                m_context.publishedManifestLease.reset();
            }
        } catch (const std::exception& exception) {
            spdlog::critical("CommitPublishedRendererState: publisher Commit failed: {}", exception.what());
            throw;
        }
		if (m_pMaterialManager) {
			BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::ActivateMaterials");
			(void)m_pMaterialManager->TryActivatePublishedMaterialState(
				m_context.publishedRendererState);
		}
		if (m_sceneIngestionServices.sceneAssetRequests) {
			BT_ZONE_SCOPE("Renderer::Update::CommitPublishedRendererState::ActivateTerrain");
			(void)m_sceneIngestionServices.sceneAssetRequests->ActivatePublishedTerrainState(
				m_context.publishedRendererState);
		}
    });

	// Final material bindings are published only after the reusable frame slot is
	// known idle. The transfer service pumps its own graphics work here, before
	// material-buffer uploads are captured by CompileFrame.
	if (m_pMaterialManager) {
		runCapturedStage("MaterialTexturePublication", [&]() {
			BT_ZONE_SCOPE("Renderer::Update::MaterialTexturePublication");
			m_pMaterialManager->ProcessPendingMaterialUpdates(m_totalFramesRendered + 1u);
		});
	}

    if (m_dynamicBackbuffer && m_swapChainReady && m_frameIndex < m_backbufferResources.size()) {
        m_dynamicBackbuffer->SetResource(m_backbufferResources[m_frameIndex]);
    }

    auto& resourceManager = ::ResourceManager::GetInstance();
    auto res = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    runCapturedStage("PerFrameBuffer", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::PerFrameBuffer");
        resourceManager.UpdatePerFrameBuffer(cameraIndex, m_pLightManager->GetNumLights(), { res.x, res.y }, m_lightClusterSize, static_cast<uint32_t>(m_totalFramesRendered));
    });

    const Components::DrawStats& drawStats = world.get<Components::DrawStats>();
    auto renderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    auto outputRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    UpdateContext updateData{};
    updateData.publishedRendererState = m_context.publishedRendererState;
    updateData.publishedManifestLease = m_context.publishedManifestLease;
    updateData.drawStats = drawStats;
    updateData.materialTextureStreamingStats = m_pMaterialManager
        ? m_pMaterialManager->GetMaterialTextureStreamingStats()
        : MaterialTextureStreamingStats{};
    updateData.environmentWork = m_environmentWorkServices;
    m_environmentWorkServices.PublishTelemetry();
    updateData.clodRayTracingSystem = m_clodRayTracingSystem;
    updateData.terrainRegionMaterialEvaluationEnabled =
        SettingsManager::GetInstance().getSettingGetter<bool>(
            "enableTerrainRegionMaterialEvaluation")();
    updateData.outputType = SettingsManager::GetInstance()
        .getSettingGetter<unsigned int>("outputType")();
    updateData.tonemapType = SettingsManager::GetInstance()
        .getSettingGetter<unsigned int>("tonemapType")();
    updateData.lightClusterSize = SettingsManager::GetInstance()
        .getSettingGetter<DirectX::XMUINT3>("lightClusterSize")();
    updateData.lighting = {
        .imageBasedLightingEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableImageBasedLighting")(),
        .punctualLightingEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enablePunctualLighting")(),
        .shadowsEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableShadows")(),
        .gtaoEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableGTAO")(),
        .clusteredLightingEnabled = SettingsManager::GetInstance()
            .getSettingGetter<bool>("enableClusteredLighting")() };
    {
    BT_ZONE_SCOPE("Renderer::Update::CaptureFrameInputs::Settings");
    updateData.proceduralWind = {
        .displacementScale = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindDisplacementScaleSettingName)(),
        .innerRadius = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindInnerRadiusSettingName)(),
        .outerRadius = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindOuterRadiusSettingName)(),
        .skeletonLodQualityCurve = SettingsManager::GetInstance().getSettingGetter<std::vector<float>>(
            ProceduralWindSkeletonLodQualityCurveSettingName)(),
        .skeletonLodStaticCutoff = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodStaticCutoffSettingName)(),
        .skeletonLodHysteresis = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodHysteresisSettingName)(),
        .forcedSkeletonLod = SettingsManager::GetInstance().getSettingGetter<int32_t>(
            ProceduralWindForcedSkeletonLodSettingName)(),
        .skeletonLodCapacityTarget = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodCapacityTargetSettingName)(),
        .skeletonLodLateReserve = SettingsManager::GetInstance().getSettingGetter<float>(
            ProceduralWindSkeletonLodLateReserveSettingName)(),
        .occlusionCullingEnabled = SettingsManager::GetInstance().getSettingGetter<bool>(
            "enableOcclusionCulling")() };
    }
    const auto publishedMaterialState = updateData.publishedRendererState
        ? updateData.publishedRendererState->materials.payload
            .Get<br::render::PublishedMaterialState>()
        : nullptr;
    if (publishedMaterialState) {
        updateData.preparedRasterBucketFlags = publishedMaterialState->rasterBucketFlags;
        updateData.preparedRasterBucketCount =
            static_cast<uint32_t>(updateData.preparedRasterBucketFlags.size());
        basic_telemetry::AddCounter("SARP.FrameInputs.PublishedRasterBuckets");
    }
    updateData.windPaletteService = m_pSkeletonManager;
    updateData.textureDescriptorHeap = m_context.textureDescriptorHeap;
    updateData.samplerDescriptorHeap = m_context.samplerDescriptorHeap;
    updateData.rtvHeap = rtvHeap->GetHandle();
    updateData.primaryCamera = camera.get<Components::Camera>();
    const auto desiredPrimaryCamera = updateData.primaryCamera;
    updateData.primaryViewID = m_context.primaryViewID;
    updateData.hasPrimaryCamera = true;
    updateData.frameSlot = m_preparationFrameIndex;
    updateData.frameIndex = updateData.frameSlot;
    updateData.frameFenceValue = m_currentFrameFenceValue;
    updateData.frameNumber = m_totalFramesRendered;
    updateData.renderResolution = renderRes;
    updateData.outputResolution = outputRes;
    if (m_imageBasedLighting) updateData.globalPSOFlags |= PSOFlags::PSO_IMAGE_BASED_LIGHTING;
    if (m_clusteredLighting) updateData.globalPSOFlags |= PSOFlags::PSO_CLUSTERED_LIGHTING;
    if (m_screenSpaceReflections || (m_rayTracedReflections && DeviceManager::GetInstance().GetCLodRayTracingSupported()))
        updateData.globalPSOFlags |= PSOFlags::PSO_SCREENSPACE_REFLECTIONS;
    updateData.deltaTime = elapsedSeconds;

    // The executable-frame request owns the logical render snapshot used by
    // transitional packets. It must never reinterpret UpdateContext as the
    // differently-laid-out RenderContext during delayed recording.
    auto renderSnapshot = m_context;
    renderSnapshot.publishedRendererState = updateData.publishedRendererState;
    renderSnapshot.publishedManifestLease = updateData.publishedManifestLease;
    renderSnapshot.primaryCamera = updateData.primaryCamera;
    renderSnapshot.primaryViewID = updateData.primaryViewID;
    renderSnapshot.hasPrimaryCamera = updateData.hasPrimaryCamera;
    renderSnapshot.frameSlot = updateData.frameSlot;
    renderSnapshot.frameIndex = renderSnapshot.frameSlot;
    renderSnapshot.swapchainImageIndex = UINT_MAX;
    renderSnapshot.frameFenceValue = updateData.frameFenceValue;
    renderSnapshot.frameNumber = updateData.frameNumber;
    renderSnapshot.renderResolution = updateData.renderResolution;
    renderSnapshot.outputResolution = updateData.outputResolution;
    renderSnapshot.globalPSOFlags = updateData.globalPSOFlags;
    renderSnapshot.terrainRegionMaterialEvaluationEnabled =
        updateData.terrainRegionMaterialEvaluationEnabled;
    renderSnapshot.outputType = updateData.outputType;
    renderSnapshot.tonemapType = updateData.tonemapType;
    renderSnapshot.lightClusterSize = updateData.lightClusterSize;
    renderSnapshot.lighting = updateData.lighting;
    renderSnapshot.proceduralWind = updateData.proceduralWind;
    renderSnapshot.deltaTime = updateData.deltaTime;
    auto currentViews = std::make_shared<br::render::PreparedViewFamilyState>();
    br::render::PrimaryCameraFrameUpload primaryCameraUpload{};
    if (m_pViewManager) {
        currentViews->revision = m_pViewManager->GetPublicationRevision();
        currentViews->cameraBufferSize = m_pViewManager->GetCameraBufferSize();
        primaryCameraUpload = m_pViewManager->CapturePrimaryCameraUpload(updateData.frameNumber);
        if (auto cameraBuffer = m_pViewManager->ProvideResource(Builtin::CameraBuffer))
            currentViews->retainedResources.push_back(std::move(cameraBuffer));
        if (auto cullingBuffer = m_pViewManager->ProvideResource(Builtin::CullingCameraBuffer))
            currentViews->retainedResources.push_back(std::move(cullingBuffer));
        const auto appendView = [&](uint64_t viewID) {
            auto* view = m_pViewManager->Get(viewID);
            if (!view) return;
            if (view->gpu.visibilityBuffer && !view->gpu.clodDeepVisibilityHeadPointers) {
                (void)m_pViewManager->EnsureCLodDeepVisibilityHeadPointers(viewID);
                view = m_pViewManager->Get(viewID);
                if (!view) return;
            }
            currentViews->views.push_back({
                .id = view->id,
                .cameraBufferIndex = view->gpu.cameraBufferIndex,
                .primary = view->flags.primaryCamera,
                .shadow = view->flags.shadow,
                .cascade = view->flags.cascaded,
                .lightType = view->lightType,
                .cameraInfo = view->cameraInfo,
                .jitterPixelSpace = view->flags.primaryCamera
                    ? desiredPrimaryCamera.jitterPixelSpace : DirectX::XMFLOAT2{},
                .jitterNDC = view->flags.primaryCamera
                    ? desiredPrimaryCamera.jitterNDC : DirectX::XMFLOAT2{},
                .visibilityBuffer = view->gpu.visibilityBuffer,
                .deepVisibilityHeadPointers = view->gpu.clodDeepVisibilityHeadPointers,
                .linearDepthMap = view->gpu.linearDepthMap,
                .depthHistory = m_depthHistory.Select(view->id, view->gpu.linearDepthMap),
                .linearDepthSRVIndices = view->gpu.linearDepthSRVIndices,
                .depthBufferArrayIndex = view->cameraInfo.depthBufferArrayIndex,
                .visibilitySRVIndex = view->gpu.visibilitySRVIndex,
                .visibilityUAVIndex = view->gpu.visibilityUAVIndex,
                .deepVisibilityHeadPointersUAVIndex = view->gpu.clodDeepVisibilityHeadPointersUAVIndex,
            });
            if (view->gpu.visibilityBuffer)
                currentViews->retainedResources.push_back(view->gpu.visibilityBuffer);
            if (view->gpu.clodDeepVisibilityHeadPointers)
                currentViews->retainedResources.push_back(view->gpu.clodDeepVisibilityHeadPointers);
            if (view->gpu.linearDepthMap)
                currentViews->retainedResources.push_back(view->gpu.linearDepthMap);
        };
        // Primary is a structural invariant: every frame snapshot starts with it.
        appendView(updateData.primaryViewID);
        m_pViewManager->ForEachView([&](uint64_t viewID) {
            if (viewID != updateData.primaryViewID) appendView(viewID);
        });
        currentViews->resourceLayoutRevision = m_pViewManager->GetResourceLayoutRevision();
        if (currentViews->views.empty() || !currentViews->views.front().primary ||
            currentViews->views.front().cameraBufferIndex != 0) {
            throw std::logic_error("Accepted frame is missing primary view slot zero");
        }
    }
    updateData.viewFamily = currentViews;
    renderSnapshot.viewFamily = currentViews;
    const auto& desiredCameraPosition = desiredPrimaryCamera.info.positionWorldSpace;
    BT_PLOT("SARP.Camera.Desired.X", static_cast<int64_t>(desiredCameraPosition.x));
    BT_PLOT("SARP.Camera.Desired.Y", static_cast<int64_t>(desiredCameraPosition.y));
    BT_PLOT("SARP.Camera.Desired.Z", static_cast<int64_t>(desiredCameraPosition.z));
    BT_PLOT("SARP.Camera.CapturedRevision", static_cast<int64_t>(primaryCameraUpload.revision));
    static const std::filesystem::path cameraTelemetryPath = [] {
        wchar_t* value = nullptr;
        size_t length = 0;
        _wdupenv_s(&value, &length, L"SARP_CAMERA_TELEMETRY_PATH");
        std::filesystem::path result = value && value[0] ? value : L"";
        std::free(value);
        return result;
    }();
    if (!cameraTelemetryPath.empty()) {
        std::error_code fileError;
        if (cameraTelemetryPath.has_parent_path())
            std::filesystem::create_directories(cameraTelemetryPath.parent_path(), fileError);
        const bool writeHeader = !std::filesystem::exists(cameraTelemetryPath, fileError) ||
            std::filesystem::file_size(cameraTelemetryPath, fileError) == 0u;
        std::ofstream output(cameraTelemetryPath, std::ios::app);
        if (writeHeader) {
            output << "frame,dt,forward,backward,left,right,up,down,pitch,yaw,camera_revision,x,y,z\n";
        }
        output << m_totalFramesRendered << ',' << elapsedSeconds << ','
            << cameraTelemetryMovement.forwardMagnitude << ',' << cameraTelemetryMovement.backwardMagnitude << ','
            << cameraTelemetryMovement.leftMagnitude << ',' << cameraTelemetryMovement.rightMagnitude << ','
            << cameraTelemetryMovement.upMagnitude << ',' << cameraTelemetryMovement.downMagnitude << ','
            << cameraTelemetryPitch << ',' << cameraTelemetryYaw << ',' << primaryCameraUpload.revision << ','
            << desiredCameraPosition.x << ',' << desiredCameraPosition.y << ',' << desiredCameraPosition.z << '\n';
    }
    basic_telemetry::SetGauge("SARP.FrameInputs.ViewFamily.ViewCount",
        static_cast<std::int64_t>(renderSnapshot.Views().size()));
	const auto publishedObjects = updateData.publishedRendererState
		? updateData.publishedRendererState->drawRecords.payload
			.Get<br::render::PublishedObjectBufferState>()
		: nullptr;
	if (publishedObjects) {
		basic_telemetry::AddCounter("SARP.FrameInputs.PublishedObjectSelection");
		basic_telemetry::SetGauge("SARP.FrameInputs.ObjectSnapshot.PlacementCount",
			publishedObjects->placementRecords
				? static_cast<std::int64_t>(publishedObjects->placementRecords->size()) : 0);
		basic_telemetry::SetGauge("SARP.FrameInputs.ObjectSnapshot.ActivePlacementCount",
			publishedObjects->activePlacementEntries
				? static_cast<std::int64_t>(publishedObjects->activePlacementEntries->size()) : 0);
	}
	const auto publishedStaticScene = updateData.publishedRendererState
		? updateData.publishedRendererState->geometry.payload
			.Get<br::render::PublishedStaticSceneState>()
		: nullptr;
	if (publishedStaticScene) {
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.DesiredPlacements",
			static_cast<std::int64_t>(publishedStaticScene->desiredPlacementCount));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.PublishedPlacements",
			static_cast<std::int64_t>(publishedStaticScene->publishedPlacementCount));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.MaterializedPlacements",
			static_cast<std::int64_t>(publishedStaticScene->materializedPlacementCount));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.GeometryRevision",
			static_cast<std::int64_t>(updateData.publishedRendererState->geometry.revision));
		basic_telemetry::SetGauge("SARP.FrameInputs.StaticScene.ManifestEpoch",
			static_cast<std::int64_t>(updateData.publishedRendererState->epoch));
	}
    renderSnapshot.preparedRasterBucketCount = updateData.preparedRasterBucketCount;
    renderSnapshot.preparedRasterBucketFlags = updateData.preparedRasterBucketFlags;

    std::shared_ptr<br::render::LightTableBuildInput> desiredLights;
    if (m_pLightManager && m_rendererStateRequests) {
        const auto lightSourceRevision = (std::max<std::uint64_t>)(
            m_pLightManager->GetPublicationRevision(), 1u);
        const auto lightViewRevision = std::uint64_t{0};
        if (lightSourceRevision != m_lastLightSourceRevision ||
            lightViewRevision != m_lastLightViewFamilyRevision) {
            ++m_lightArtifactRevision;
            m_lastLightSourceRevision = lightSourceRevision;
            m_lastLightViewFamilyRevision = lightViewRevision;
            desiredLights = std::make_shared<br::render::LightTableBuildInput>();
        desiredLights->revision = m_lightArtifactRevision;
        desiredLights->lightCount = m_pLightManager->GetNumLights();
        desiredLights->lightPagePoolSize = m_pLightManager->GetLightPagePoolSize();
        desiredLights->tableImages = m_pLightManager->CaptureTableImages();
        auto& lightWorld = RendererECSManager::GetInstance().GetWorld();
        auto directionalLights = lightWorld.query_builder<const Components::Light,
            const Components::LightViewInfo>().build();
        directionalLights.each([&](flecs::entity, const Components::Light& light,
            const Components::LightViewInfo& viewInfo) {
            if (!light.lightInfo.shadowCaster || light.type != Components::LightType::Directional) return;
            br::render::PublishedDirectionalShadowLight shadow{};
            DirectX::XMStoreFloat3(&shadow.direction,
                DirectX::XMVector3Normalize(light.lightInfo.dirWorldSpace));
            shadow.viewIDs = viewInfo.viewIDs;
            shadow.unwrappedPageOffsetX.assign(viewInfo.virtualShadowUnwrappedPageOffsetX.begin(),
                viewInfo.virtualShadowUnwrappedPageOffsetX.end());
            shadow.unwrappedPageOffsetY.assign(viewInfo.virtualShadowUnwrappedPageOffsetY.begin(),
                viewInfo.virtualShadowUnwrappedPageOffsetY.end());
            desiredLights->directionalShadows.push_back(std::move(shadow));
        });
        for (const auto& key : m_pLightManager->GetSupportedKeys()) {
            if (auto resource = m_pLightManager->ProvideResource(key))
                desiredLights->retainedResources.push_back(std::move(resource));
        }
        std::vector<br::render::ArtifactRequirement> lightRequirements;
        if (auto uploads = currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr) {
            const std::array<std::uint32_t, 5> strides{
                sizeof(LightInfo), sizeof(std::uint32_t), sizeof(std::uint32_t), sizeof(std::uint32_t),
                sizeof(std::uint32_t) };
            for (std::size_t i = 0; i < desiredLights->tableImages.size() && i < strides.size(); ++i) {
                const auto& image = desiredLights->tableImages[i];
                const auto count = image ? image->size() / strides[i] : 0;
                const auto request = m_lightTableFamilies[i]->RequestContentSnapshot(
                    *m_rendererStateRequests, uploads,
                    image ? std::span<const std::byte>(*image) : std::span<const std::byte>{},
                    count, (std::max<std::uint64_t>)(count, 1));
                if (request) lightRequirements.push_back(br::render::Exact(request.Handle()));
            }
        }
        (void)m_rendererStateRequests->SubmitLatest({
            { br::render::ArtifactKind::LightTable, 0, 0 }, desiredLights->revision,
            std::move(lightRequirements),
            br::render::ArtifactPayload::Make<br::render::LightTableBuildInput>(desiredLights),
            desiredLights->revision });
        }
    }
    auto selectedLights = updateData.publishedRendererState
        ? updateData.publishedRendererState->lights.payload.Get<br::render::PublishedLightTableState>()
        : nullptr;
    if (selectedLights) {
        basic_telemetry::AddCounter("SARP.FrameInputs.PublishedLightTableSelection");
    }
    updateData.lightTables = selectedLights;
    renderSnapshot.lightTables = selectedLights;

    // Snapshot active skeleton membership and immutable base-skeleton data into
    // the state graph. Accepted frames can now outlive later manager mutations.
    std::shared_ptr<br::render::PoseStateBuildInput> desiredPoses;
    if (m_pSkeletonManager && m_rendererStateRequests) {
        const auto poseSourceRevision = (std::max<std::uint64_t>)(
            m_pSkeletonManager->GetActiveInstanceRevision(), 1u);
        if (poseSourceRevision != m_lastPoseSourceRevision) {
        m_lastPoseSourceRevision = poseSourceRevision;
        desiredPoses = std::make_shared<br::render::PoseStateBuildInput>();
        desiredPoses->activeInstanceRevision = poseSourceRevision;
        desiredPoses->tableImages = m_pSkeletonManager->CapturePoseTableImages();
        std::vector<br::render::ArtifactRequirement> poseRequirements;
        if (auto uploads = currentRenderGraph ? currentRenderGraph->RetainUploadService() : nullptr) {
            const std::array<std::uint32_t, 4> strides{
                sizeof(DirectX::XMMATRIX), sizeof(DirectX::XMMATRIX), sizeof(DirectX::XMMATRIX),
                sizeof(SkinningInstanceGPUInfo) };
            for (std::size_t i = 0; i < 1 && i < desiredPoses->tableImages.size(); ++i) {
                const auto& image = desiredPoses->tableImages[i];
                const auto count = image ? image->size() / strides[i] : 0;
                const auto request = m_poseTableFamilies[i]->RequestContentSnapshot(
                    *m_rendererStateRequests, uploads,
                    image ? std::span<const std::byte>(*image) : std::span<const std::byte>{},
                    count, (std::max<std::uint64_t>)(count, 1));
                if (request) poseRequirements.push_back(br::render::Exact(request.Handle()));
            }
        }
        for (const auto& instance : m_pSkeletonManager->GetActiveInstanceViews()) {
            if (!instance.skeleton) continue;
            desiredPoses->activeInstances.push_back({
                .baseSkeleton = instance.skeleton->GetBaseSkeletonShared(),
                .instanceSlot = instance.instanceSlot,
                .transformOffsetMatrices = instance.transformOffsetMatrices,
                .inverseSkinOffsetMatrices = instance.inverseSkinOffsetMatrices,
                .boneCount = instance.boneCount,
            });
        }
        for (const auto& key : m_pSkeletonManager->GetSupportedKeys()) {
            if (auto resource = m_pSkeletonManager->ProvideResource(key))
                desiredPoses->retainedResources.push_back(std::move(resource));
        }
        (void)m_rendererStateRequests->SubmitLatest({
            { br::render::ArtifactKind::PoseState, 0, 0 }, desiredPoses->activeInstanceRevision,
            std::move(poseRequirements),
            br::render::ArtifactPayload::Make<br::render::PoseStateBuildInput>(desiredPoses),
            desiredPoses->activeInstanceRevision });
        }
    }
    auto selectedPoses = updateData.publishedRendererState
        ? updateData.publishedRendererState->poses.payload.Get<br::render::PublishedPoseState>()
        : nullptr;
    if (selectedPoses) {
        basic_telemetry::AddCounter("SARP.FrameInputs.PublishedPoseSelection");
    }
    updateData.poses = selectedPoses;
    renderSnapshot.poses = std::move(selectedPoses);
    runCapturedStage("PublishDeferredBackingResizesLate", []() {
        BT_ZONE_SCOPE("Renderer::Update::PublishDeferredBackingResizesLate");
        (void)PublishReadyDeferredBackingResizes(false);
    });

    runCapturedStage("FlushUploadPolicies", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::FlushUploadPolicies");
        org::runtime::FlushUploadPolicies();
    });

    runCapturedStage("CommitGpuVisibleSnapshots", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::CommitGpuVisibleSnapshots");
        if (m_pMaterialManager) {
			m_pMaterialManager->ScheduleGpuVisibleSnapshotCommit();
        }
		if (m_pObjectManager) {
			m_pObjectManager->PublishDesiredBufferState();
		}
        if (m_pIndirectCommandBufferManager && m_pObjectManager) {
			m_pIndirectCommandBufferManager->PublishDesiredState(
				m_pObjectManager->DesiredBufferStateRequirement(),
				m_pObjectManager->GetResidentInstanceDrawRecordCount(),
				m_context.publishedRendererState);
        }
    });

    const bool publicationsReady = publishedMaterialState
        && updateData.lightTables && updateData.poses;
    if (!publicationsReady) {
        m_frameInputs.reset();
        basic_telemetry::AddCounter("SARP.FrameInputs.PublicationAdmissionBackpressure");
        basic_telemetry::SetGauge("SARP.FrameInputs.PublicationAdmissionReady", 0);
        return;
    }
    basic_telemetry::SetGauge("SARP.FrameInputs.PublicationAdmissionReady", 1);
    renderSnapshot.uiDrawData = Menu::GetInstance().PrepareDrawData(renderSnapshot);
    auto immutableUpdate = std::make_shared<const UpdateContext>(updateData);
    m_frameInputs = std::make_shared<const br::render::RendererFrameInputs>(
        std::move(immutableUpdate),
        std::make_shared<const RenderContext>(std::move(renderSnapshot)),
        primaryCameraUpload);

    UpdateExecutionContext context{};
    context.resolverCaptureContext = std::make_shared<const org::ResolverCaptureContext>(m_context.publishedManifestLease);
    context.frameIndex = m_preparationFrameIndex;
    context.preparationSlot = m_preparationFrameIndex;
    context.frameFenceValue = m_currentFrameFenceValue;
    context.deltaTime = elapsedSeconds;
    context.hostData = m_frameInputs.get();
    context.ownedHostData = m_frameInputs;
    context.beforeCompileFrame = [this]() {
        BT_ZONE_SCOPE("Renderer::Update::TerrainRvtTelemetry");
        MaybeRequestTerrainRvtTelemetry();
        MaybeRequestObjectReyesAtlasTelemetry();
        // Opt-in, bounded camera-buffer stream. At most one capture may be in
        // flight, which preserves request/completion ordering and avoids
        // retaining an unbounded readback tail during shutdown.
        struct CameraReadbackStreamState {
            std::atomic_bool inFlight{ false };
            std::atomic_uint64_t completionSequence{ 0 };
            std::mutex outputMutex;
        };
        static auto cameraReadbackState = std::make_shared<CameraReadbackStreamState>();
        static const std::filesystem::path cameraReadbackPath = [] {
            wchar_t* value = nullptr;
            size_t length = 0;
            _wdupenv_s(&value, &length, L"SARP_CAMERA_READBACK_PATH");
            std::filesystem::path result = value && value[0] ? value : L"";
            std::free(value);
            return result;
        }();
        if (!cameraReadbackPath.empty() && currentRenderGraph && m_frameInputs &&
            !cameraReadbackState->inFlight.exchange(true, std::memory_order_acq_rel)) {
            const auto renderInputs = m_frameInputs->Render();
            const auto views = renderInputs ? renderInputs->viewFamily : nullptr;
            const auto primary = views ? std::find_if(views->views.begin(), views->views.end(),
                [](const auto& view) { return view.primary; }) : decltype(views->views.begin()){};
            const auto cameraResource = m_pViewManager ? m_pViewManager->GetCameraBuffer() : nullptr;
            if (cameraResource && views && primary != views->views.end()) {
                if (auto* service = currentRenderGraph->GetReadbackService()) {
                    const auto requestFrame = m_totalFramesRendered;
                    const auto cameraRevision = m_frameInputs->PrimaryCameraUpload().revision;
                    const auto expected = primary->cameraInfo;
                    try {
                        service->RequestReadbackCapture("PrimaryCameraUploadReadback",
                            cameraResource.get(), RangeSpec{},
                            [state = cameraReadbackState, path = cameraReadbackPath, requestFrame,
                                cameraRevision, expected](ReadbackCaptureResult&& result) {
                            CameraInfo camera{};
                            if (result.data.size() >= sizeof(camera))
                                std::memcpy(&camera, result.data.data(), sizeof(camera));
                            const auto expectedPreviousInverse = DirectX::XMMatrixInverse(nullptr, expected.prevView);
                            const auto gpuPreviousInverse = DirectX::XMMatrixInverse(nullptr, camera.prevView);
                            const auto completion = state->completionSequence.fetch_add(1,
                                std::memory_order_relaxed) + 1u;
                            {
                                std::scoped_lock lock(state->outputMutex);
                                std::error_code fileError;
                                if (path.has_parent_path())
                                    std::filesystem::create_directories(path.parent_path(), fileError);
                                const bool writeHeader = !std::filesystem::exists(path, fileError) ||
                                    std::filesystem::file_size(path, fileError) == 0u;
                                std::ofstream output(path, std::ios::app);
                                if (writeHeader) {
                                    output << "completion,request_frame,camera_revision,bytes,cpu_x,cpu_y,cpu_z,cpu_prev_x,cpu_prev_y,cpu_prev_z,gpu_x,gpu_y,gpu_z,gpu_prev_x,gpu_prev_y,gpu_prev_z\n";
                                }
                                output << completion << ',' << requestFrame << ',' << cameraRevision << ','
                                    << result.data.size() << ','
                                    << expected.positionWorldSpace.x << ',' << expected.positionWorldSpace.y << ','
                                    << expected.positionWorldSpace.z << ','
                                    << DirectX::XMVectorGetX(expectedPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetY(expectedPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetZ(expectedPreviousInverse.r[3]) << ','
                                    << camera.positionWorldSpace.x << ',' << camera.positionWorldSpace.y << ','
                                    << camera.positionWorldSpace.z << ','
                                    << DirectX::XMVectorGetX(gpuPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetY(gpuPreviousInverse.r[3]) << ','
                                    << DirectX::XMVectorGetZ(gpuPreviousInverse.r[3]) << '\n';
                            }
                                if (result.data.size() < sizeof(camera) ||
                                    std::memcmp(&camera, &expected, sizeof(camera)) != 0)
                                    basic_telemetry::AddCounter("SARP.Camera.Upload.ReadbackMismatch");
                                state->inFlight.store(false, std::memory_order_release);
                            });
                    } catch (...) {
                        cameraReadbackState->inFlight.store(false, std::memory_order_release);
                        throw;
                    }
                } else {
                    cameraReadbackState->inFlight.store(false, std::memory_order_release);
                }
            } else {
                cameraReadbackState->inFlight.store(false, std::memory_order_release);
            }
        }
        // Opt-in, one-frame GPU work snapshot. Capture producers as well as their
        // indirect consumers so visual failures can be localized without GPU printf.
        static const uint64_t diagnosticCaptureFrame = [] {
            char* value = nullptr;
            size_t length = 0;
            _dupenv_s(&value, &length, "SARP_GPU_WORK_READBACK_FRAME");
            const uint64_t frame = value ? std::strtoull(value, nullptr, 10) : 120u;
            std::free(value);
            return frame;
        }();
        static bool gpuWorkReadbackRequested = false;
        if (!gpuWorkReadbackRequested && m_totalFramesRendered >= diagnosticCaptureFrame && currentRenderGraph) {
            wchar_t* value = nullptr;
            size_t length = 0;
            _wdupenv_s(&value, &length, L"SARP_GPU_WORK_READBACK_DIR");
            if (value && value[0]) {
                const std::filesystem::path directory(value);
                std::filesystem::create_directories(directory);
                if (auto* service = currentRenderGraph->GetReadbackService()) {
                    gpuWorkReadbackRequested = true;
                    const auto captureResource = [&](const char* label, const char* anchor,
                        std::shared_ptr<Resource> resource,
                        std::shared_ptr<const br::render::PublishedGpuBufferVersion> version = {}) {
                        if (!resource) return;
                        // DynamicResource is a mutable indirection used by the
                        // frame graph for slot rotation. Capture the backing
                        // resource selected at acceptance time so an async
                        // frame cannot observe a later slot's contents.
                        if (auto* dynamic = dynamic_cast<DynamicResource*>(resource.get())) {
                            if (auto backing = dynamic->GetResource()) resource = std::move(backing);
                        }
                        const auto frame = m_totalFramesRendered;
                        const auto published = m_context.publishedRendererState;
                        const auto drawRecordsRevision = published ? published->drawRecords.revision : 0u;
                        const auto indirectRevision = published ? published->indirectWorkloads.revision : 0u;
                        const auto desiredDrawRecordsRevision = m_pObjectManager
                            ? m_pObjectManager->DesiredBufferStateRequirement()
                                .transform([](const auto& requirement) { return requirement.minimumRevision; })
                                .value_or(0u)
                            : 0u;
                        const auto indirectState = published
                            ? published->indirectWorkloads.payload.Get<br::render::PublishedIndirectState>()
                            : nullptr;
                        const auto indirectDrawRecordsRevision = indirectState
                            ? indirectState->drawRecordsRoot.revision : 0u;
                        const auto path = directory / (std::string(label) + ".bin");
                        service->RequestReadbackCapture(anchor, resource.get(), RangeSpec{},
                            [path, frame, drawRecordsRevision, indirectRevision,
                                desiredDrawRecordsRevision, indirectDrawRecordsRevision,
                                version = std::move(version)](ReadbackCaptureResult&& result) {
                                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                                output.write(reinterpret_cast<const char*>(result.data.data()),
                                    static_cast<std::streamsize>(result.data.size()));
                                auto metadataPath = path;
                                metadataPath += L".meta.txt";
                                std::ofstream metadata(metadataPath);
                                metadata << "frame=" << frame << '\n'
                                    << "draw_records_revision=" << drawRecordsRevision << '\n'
                                    << "desired_draw_records_revision=" << desiredDrawRecordsRevision << '\n'
                                    << "indirect_revision=" << indirectRevision << '\n'
                                    << "indirect_draw_records_revision=" << indirectDrawRecordsRevision << '\n'
                                    << "resource=" << result.desc.resourceId << '\n'
                                    << "format=" << static_cast<uint32_t>(result.format) << '\n'
                                    << "width=" << result.width << '\n'
                                    << "height=" << result.height << '\n'
                                    << "bytes=" << result.data.size() << '\n';
                                if (version) metadata
                                    << "buffer_revision=" << version->revision << '\n'
                                    << "write_sequence=" << version->writeSequence << '\n'
                                    << "element_count=" << version->elementCount << '\n'
                                    << "capacity=" << version->capacity << '\n'
                                    << "element_stride=" << version->elementStride << '\n'
                                    << "content_version=" << version->contentVersion << '\n'
                                    << "content_epoch=" << version->contentEpoch << '\n'
                                    << "backing_epoch=" << version->backingEpoch << '\n'
                                    << "backing_generation=" << (version->backing
                                        ? version->backing->backingGeneration : 0u) << '\n';
                                if (!result.layouts.empty()) metadata
                                    << "offset=" << result.layouts.front().offset << '\n'
                                    << "row_pitch=" << result.layouts.front().rowPitch << '\n';
                            });
                    };
                    const auto capture = [&](const char* label, const char* anchor, ResourceIdentifier id) {
                        captureResource(label, anchor, currentRenderGraph->RequestResourcePtr(id, true));
                    };
                    // Preserve the visibility target at the two phase-1 producer
                    // boundaries as well as at its first material consumer.  The
                    // fixed benchmark camera expects phase 1 to cover the scene;
                    // shader invocation counts alone did not catch descriptor-table
                    // mismatches that discarded almost every raster write.
                    capture("visibility-after-phase1-hw", "CLodOpaque::RasterizeClustersPass1",
                        Builtin::PrimaryCamera::VisibilityTexture);
                    capture("visibility-after-phase1-sw", "CLodOpaque::SoftwareRasterizeClustersPass1",
                        Builtin::PrimaryCamera::VisibilityTexture);
                    capture("visibility", "MaterialHistogramPass", Builtin::PrimaryCamera::VisibilityTexture);
                    capture("material-counts", "BuildMaterialIndirectCommandBufferPass", "Builtin::VisUtil::MaterialPixelCountBuffer");
                    capture("material-offsets", "BuildMaterialIndirectCommandBufferPass", "Builtin::VisUtil::MaterialOffsetBuffer");
                    capture("material-args", "BuildMaterialIndirectCommandBufferPass", "Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer");
                    capture("pixel-list", "BuildPixelListPass", "Builtin::VisUtil::PixelListBuffer");
                    capture("surface-identity", "EvaluateMaterialGroupsPass", Builtin::Surface::Identity);
                    // Publication audit: retain the exact object buffer backing
                    // selected for this graph generation alongside visibility.
                    // Its byte count and resource id are written by the
                    // readback service, allowing CPU publication cuts to be
                    // compared with the GPU-consumed object data.
                    if (const auto published = m_context.publishedRendererState) {
                        if (const auto objects = published->drawRecords.payload
                                .Get<br::render::PublishedObjectBufferState>()) {
                            const auto capturePublishedObjectVersion = [&](const char* label,
                                std::uint64_t variant) {
                                const auto version = objects->FindVersion(variant);
                                captureResource(label, "CLodOpaque::RasterizeClustersPass1",
                                    version ? version->resource : nullptr, version);
                            };
                            // Capture the immutable resources selected by the
                            // accepted manifest.  The manager-facing logical
                            // resources can point at a newer backing by the
                            // time this async frame reaches the readback pass.
                            capturePublishedObjectVersion("published-per-object",
                                br::render::kObjectPerObjectVariant);
                            capturePublishedObjectVersion("published-instance-transforms",
                                br::render::kObjectInstanceTransformVariant);
                            capturePublishedObjectVersion("published-draw-records",
                                br::render::kObjectDrawRecordVariant);
                        }
                        if (const auto indirect = published->indirectWorkloads.payload
                                .Get<br::render::PublishedIndirectState>()) {
                            captureResource("published-visibility-generations",
                                "CLodOpaque::RasterizeClustersPass1", indirect->visibilityGenerations);
                            std::unordered_set<std::uint64_t> capturedActiveResources;
                            std::unordered_set<std::uint64_t> capturedArgumentResources;
                            for (std::size_t workloadIndex = 0;
                                workloadIndex < indirect->workloads.size(); ++workloadIndex) {
                                const auto& workload = indirect->workloads[workloadIndex];
                                // Multiple views commonly retain the same
                                // immutable buffer version.  Read each backing
                                // once; its label still records the retained
                                // revision and advertised element count.
                                const auto label = std::format("published-active-draw-{}-rev-{}-count-{}",
                                    workloadIndex, workload.activeListRevision, workload.count);
                                if (workload.activeDrawList && capturedActiveResources.insert(
                                        workload.activeDrawList->GetGlobalResourceID()).second) {
                                    captureResource(label.c_str(), "CLodOpaque::RasterizeClustersPass1",
                                        workload.activeDrawList);
                                }
                                const auto argsLabel = std::format(
                                    "published-indirect-args-{}-active-rev-{}-count-{}",
                                    workloadIndex, workload.activeListRevision, workload.count);
                                if (workload.indirectArguments && capturedArgumentResources.insert(
                                        workload.indirectArguments->GetGlobalResourceID()).second) {
                                    captureResource(argsLabel.c_str(), "CLodOpaque::RasterizeClustersPass1",
                                        workload.indirectArguments);
                                }
                            }
                        }
                    }
                }
            }
            std::free(value);
        }
        static bool colorOutputReadbackRequested = false;
        if (!colorOutputReadbackRequested && m_totalFramesRendered >= diagnosticCaptureFrame &&
            currentRenderGraph && m_dynamicPresentationColor) {
            wchar_t* outputPath = nullptr;
            size_t outputPathLength = 0;
            if (_wdupenv_s(
                    &outputPath,
                    &outputPathLength,
                    L"SARP_COLOR_OUTPUT_READBACK_PATH") == 0 &&
                outputPath != nullptr && outputPath[0] != L'\0') {
                const std::filesystem::path path(outputPath);
                std::free(outputPath);
                outputPath = nullptr;
                if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
                    colorOutputReadbackRequested = true;
                    readbackService->RequestReadbackCapture(
                        "PresentationReadyPass",
                        m_dynamicPresentationColor.get(),
                        RangeSpec{},
                        [path](ReadbackCaptureResult&& result) {
                            BT_ZONE_SCOPE("Renderer::ColorOutputReadback::Analyze");
                            std::ofstream output(path, std::ios::binary | std::ios::trunc);
                            if (output && !result.data.empty()) {
                                output.write(
                                    reinterpret_cast<const char*>(result.data.data()),
                                    static_cast<std::streamsize>(result.data.size()));
                            }

                            std::uint64_t whitePixels = 0;
                            std::uint64_t opaquePixels = 0;
                            std::uint64_t allWhiteTiles = 0;
                            constexpr std::uint32_t tileSize = 16;
                            if (!result.layouts.empty() && result.width && result.height) {
                                const auto& layout = result.layouts.front();
                                const auto rowPitch = static_cast<std::size_t>(layout.rowPitch);
                                const auto baseOffset = static_cast<std::size_t>(layout.offset);
                                if (rowPitch >= static_cast<std::size_t>(result.width) * 4u &&
                                    baseOffset + rowPitch * result.height <= result.data.size()) {
                                    for (std::uint32_t y = 0; y < result.height; ++y) {
                                        const auto* row = result.data.data() + baseOffset + rowPitch * y;
                                        for (std::uint32_t x = 0; x < result.width; ++x) {
                                            const auto* pixel = row + static_cast<std::size_t>(x) * 4u;
                                            const bool white = std::to_integer<std::uint8_t>(pixel[0]) >= 250u &&
                                                std::to_integer<std::uint8_t>(pixel[1]) >= 250u &&
                                                std::to_integer<std::uint8_t>(pixel[2]) >= 250u;
                                            whitePixels += white ? 1u : 0u;
                                            opaquePixels += std::to_integer<std::uint8_t>(pixel[3]) >= 250u ? 1u : 0u;
                                        }
                                    }
                                    for (std::uint32_t y = 0; y + tileSize <= result.height; y += tileSize) {
                                        for (std::uint32_t x = 0; x + tileSize <= result.width; x += tileSize) {
                                            bool allWhite = true;
                                            for (std::uint32_t tileY = 0; tileY < tileSize && allWhite; ++tileY) {
                                                const auto* row = result.data.data() + baseOffset +
                                                    rowPitch * (y + tileY) + static_cast<std::size_t>(x) * 4u;
                                                for (std::uint32_t tileX = 0; tileX < tileSize; ++tileX) {
                                                    const auto* pixel = row + static_cast<std::size_t>(tileX) * 4u;
                                                    if (std::to_integer<std::uint8_t>(pixel[0]) < 250u ||
                                                        std::to_integer<std::uint8_t>(pixel[1]) < 250u ||
                                                        std::to_integer<std::uint8_t>(pixel[2]) < 250u) {
                                                        allWhite = false;
                                                        break;
                                                    }
                                                }
                                            }
                                            allWhiteTiles += allWhite ? 1u : 0u;
                                        }
                                    }
                                }
                            }
                            BT_PLOT("Renderer.ColorOutputReadback.WhitePixels", static_cast<int64_t>(whitePixels));
                            BT_PLOT("Renderer.ColorOutputReadback.AllWhite16x16Tiles", static_cast<int64_t>(allWhiteTiles));
                            BT_PLOT("Renderer.ColorOutputReadback.Bytes", static_cast<int64_t>(result.data.size()));

                            auto metadataPath = path;
                            metadataPath += L".meta.txt";
                            std::ofstream metadata(metadataPath, std::ios::trunc);
                            if (metadata) {
                                metadata << "format=" << static_cast<std::uint32_t>(result.format) << '\n';
                                metadata << "width=" << result.width << '\n';
                                metadata << "height=" << result.height << '\n';
                                metadata << "bytes=" << result.data.size() << '\n';
                                metadata << "white_pixels=" << whitePixels << '\n';
                                metadata << "opaque_pixels=" << opaquePixels << '\n';
                                metadata << "all_white_16x16_tiles=" << allWhiteTiles << '\n';
                                if (!result.layouts.empty()) {
                                    metadata << "offset=" << result.layouts.front().offset << '\n';
                                    metadata << "row_pitch=" << result.layouts.front().rowPitch << '\n';
                                }
                            }
                            spdlog::info(
                                "Color output readback: bytes={} dimensions={}x{} white_pixels={} all_white_16x16_tiles={} output='{}'.",
                                result.data.size(), result.width, result.height, whitePixels, allWhiteTiles,
                                path.string());
                        });
                }
            }
            if (outputPath) {
                std::free(outputPath);
            }
        }
        static bool materialBufferReadbackRequested = false;
        if (!materialBufferReadbackRequested && m_totalFramesRendered >= 120u &&
            currentRenderGraph && m_pMaterialManager) {
            wchar_t* outputPath = nullptr;
            size_t outputPathLength = 0;
            if (_wdupenv_s(
                    &outputPath,
                    &outputPathLength,
                    L"SARP_MATERIAL_BUFFER_READBACK_PATH") == 0 &&
                outputPath != nullptr && outputPath[0] != L'\0') {
                const std::filesystem::path path(outputPath);
                std::free(outputPath);
                outputPath = nullptr;
                if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
                    const auto lease = m_context.publishedManifestLease;
                    const auto published = lease ? lease->state : nullptr;
                    const auto materialState = published
                        ? published->materials.payload.Get<br::render::PublishedMaterialState>() : nullptr;
                    if (materialState && materialState->baseTable && materialState->evalTable &&
                        materialState->openPbrTable) {
                        auto metadataPath = path;
                        metadataPath += L".meta.txt";
                        std::ofstream metadata(metadataPath, std::ios::trunc);
                        if (metadata) {
                            metadata << "epoch=" << published->epoch << '\n';
                            metadata << "lease_sequence=" << lease->sequence << '\n';
                            metadata << "lease_frame_slot=" << lease->frameSlot << '\n';
                            metadata << "revision=" << published->materials.revision << '\n';
                            metadata << "compile_flag_slots=" << materialState->compileFlagSlotsUsed << '\n';
                            metadata << "active_compile_flags=" << materialState->activeCompileFlags.size() << '\n';
                            for (std::size_t index = 0;
                                index < materialState->activeCompileFlags.size() &&
                                index < materialState->activeCompileFlagSlots.size(); ++index) {
                                metadata << "active[" << index << "].flags="
                                    << static_cast<std::uint64_t>(materialState->activeCompileFlags[index])
                                    << " slot=" << materialState->activeCompileFlagSlots[index] << '\n';
                            }
                        }
                        const auto requestTable = [readbackService, &path, published](
                            const char* label,
                            const std::shared_ptr<const br::render::PublishedGpuBufferVersion>& table,
                            Resource* captureResource = nullptr) {
                            if (!table || !table->resource || !table->image) return;
                            auto tablePath = path;
                            tablePath += std::filesystem::path(fmt::format(".{}.bin", label));
                            const auto expected = table->MaterializeCpuImage();
							auto expectedPath = tablePath;
							expectedPath += L".expected";
							std::ofstream expectedOutput(expectedPath, std::ios::binary | std::ios::trunc);
							if (expectedOutput && !expected->empty()) {
								expectedOutput.write(reinterpret_cast<const char*>(expected->data()),
									static_cast<std::streamsize>(expected->size()));
							}
                            const auto resourceID = table->resource->GetGlobalResourceID();
                            const br::render::ArtifactSnapshot* tableArtifact = nullptr;
                            for (const auto& dependency : published->materials.dependencyClosure) {
                                const auto root = dependency.payload
                                    .Get<br::render::RendererStateFragmentArtifact>();
                                const auto dependencyTable = root
                                    ? root->fragment.payload
                                        .Get<br::render::PublishedGpuBufferVersion>() : nullptr;
                                if (dependencyTable == table) {
                                    tableArtifact = &dependency;
                                    break;
                                }
                            }
                            auto tableMetadataPath = tablePath;
                            tableMetadataPath += L".meta.txt";
                            std::ofstream tableMetadata(tableMetadataPath, std::ios::trunc);
                            if (tableMetadata) {
                                tableMetadata << "manifest_epoch=" << published->epoch << '\n';
                                tableMetadata << "manifest_material_revision="
                                    << published->materials.revision << '\n';
                                tableMetadata << "manifest_material_root_kind="
                                    << static_cast<std::uint32_t>(
                                        published->materials.publicationRoot.address.kind) << '\n';
                                tableMetadata << "manifest_material_root_primary="
                                    << published->materials.publicationRoot.address.primaryID << '\n';
                                tableMetadata << "manifest_material_root_variant="
                                    << published->materials.publicationRoot.address.variantID << '\n';
                                tableMetadata << "buffer_revision=" << table->revision << '\n';
                                tableMetadata << "buffer_write_sequence="
                                    << table->writeSequence << '\n';
                                tableMetadata << "buffer_content_version="
                                    << table->contentVersion << '\n';
                                tableMetadata << "buffer_backing_generation="
                                    << (table->backing ? table->backing->backingGeneration : 0u) << '\n';
                                tableMetadata << "resource_id=" << resourceID << '\n';
                                tableMetadata << "capture_resource_id="
                                    << (captureResource
                                        ? captureResource->GetGlobalResourceID() : resourceID) << '\n';
                                tableMetadata << "capture_matches_published="
                                    << (!captureResource || captureResource == table->resource.get()) << '\n';
                                if (tableArtifact) {
                                    tableMetadata << "artifact_kind="
                                        << static_cast<std::uint32_t>(tableArtifact->key.kind) << '\n';
                                    tableMetadata << "artifact_primary="
                                        << tableArtifact->key.primaryID << '\n';
                                    tableMetadata << "artifact_variant="
                                        << tableArtifact->key.variantID << '\n';
                                    tableMetadata << "artifact_revision="
                                        << tableArtifact->revision << '\n';
                                    tableMetadata << "artifact_generation="
                                        << tableArtifact->generation << '\n';
                                    tableMetadata << "artifact_readiness="
                                        << static_cast<std::uint32_t>(
                                            tableArtifact->readiness) << '\n';
                                }
                            }
                            readbackService->RequestReadbackCapture(
                                "EvaluateMaterialGroupsPass",
                                captureResource ? captureResource : table->resource.get(), RangeSpec{},
                                [tablePath, expected, resourceID, label](ReadbackCaptureResult&& result) {
                                    std::ofstream output(tablePath, std::ios::binary | std::ios::trunc);
                                    if (output && !result.data.empty()) {
                                        output.write(reinterpret_cast<const char*>(result.data.data()),
                                            static_cast<std::streamsize>(result.data.size()));
                                    }
                                    const auto comparedBytes = (std::min)(result.data.size(), expected->size());
                                    std::size_t firstMismatch = comparedBytes;
                                    for (std::size_t offset = 0; offset < comparedBytes; ++offset) {
                                        if (result.data[offset] != (*expected)[offset]) {
                                            firstMismatch = offset;
                                            break;
                                        }
                                    }
                                    const bool exactPrefix = result.data.size() >= expected->size() &&
                                        firstMismatch == comparedBytes;
                                    spdlog::info(
                                        "Published material GPU readback: table={} resource={} gpuBytes={} expectedBytes={} exactPrefix={} firstMismatch={} output='{}'.",
                                        label, resourceID, result.data.size(), expected->size(), exactPrefix,
                                        firstMismatch == comparedBytes ? UINT64_MAX : firstMismatch,
                                        tablePath.string());
                                });
                        };
                        materialBufferReadbackRequested = true;
                        requestTable("base", materialState->baseTable);
                        requestTable("eval", materialState->evalTable);
                        requestTable("openpbr", materialState->openPbrTable);
                        const auto textureImages = published->textureImages.payload
                            .Get<br::render::PublishedTextureImageTable>();
                        if (textureImages && textureImages->table) {
                            const auto* textureStreaming = m_pMaterialManager
                                ? m_pMaterialManager->GetTextureStreamingManager() : nullptr;
                            const auto boundTextureTable = textureStreaming
                                ? textureStreaming->ResolvePublishedImageTableResourceForDiagnostics()
                                : std::shared_ptr<Resource>{};
							const auto readbackAnchor = textureStreaming
								? textureStreaming->PublishedImageTableReadbackAnchorForDiagnostics()
								: std::shared_ptr<Resource>{};
                            // Request the resolver-selected published backing directly.  The
                            // logical bootstrap resource is only the resolver fallback; passing
                            // it to the readback service captures that stale backing rather than
                            // the resource whose descriptor is selected for this pass.
                            requestTable("texture-images", textureImages->table,
								boundTextureTable ? boundTextureTable.get() :
									(readbackAnchor ? readbackAnchor.get() : nullptr));
                        }

                        const auto requestTexture = [readbackService, &path](
                            const char* label, const std::shared_ptr<Resource>& texture) {
                            if (!texture) return;
                            auto texturePath = path;
                            texturePath += std::filesystem::path(fmt::format(".{}.bin", label));
                            const auto resourceID = texture->GetGlobalResourceID();
                            readbackService->RequestReadbackCapture(
                                "EvaluateMaterialGroupsPass", texture.get(), RangeSpec{},
                                [texturePath, resourceID, label](ReadbackCaptureResult&& result) {
                                    std::ofstream output(texturePath, std::ios::binary | std::ios::trunc);
                                    if (output && !result.data.empty()) {
                                        output.write(reinterpret_cast<const char*>(result.data.data()),
                                            static_cast<std::streamsize>(result.data.size()));
                                    }
                                    auto textureMetadataPath = texturePath;
                                    textureMetadataPath += L".meta.txt";
                                    std::ofstream textureMetadata(textureMetadataPath, std::ios::trunc);
                                    if (textureMetadata) {
                                        textureMetadata << "resource_id=" << resourceID << '\n';
                                        textureMetadata << "format="
                                            << static_cast<std::uint32_t>(result.format) << '\n';
                                        textureMetadata << "width=" << result.width << '\n';
                                        textureMetadata << "height=" << result.height << '\n';
                                        textureMetadata << "depth=" << result.depth << '\n';
                                        textureMetadata << "bytes=" << result.data.size() << '\n';
                                        textureMetadata << "layouts=" << result.layouts.size() << '\n';
                                        for (std::size_t index = 0; index < result.layouts.size(); ++index) {
                                            const auto& layout = result.layouts[index];
                                            textureMetadata << "layout[" << index << "].offset="
                                                << layout.offset << '\n';
                                            textureMetadata << "layout[" << index << "].row_pitch="
                                                << layout.rowPitch << '\n';
                                        }
                                    }
                                    spdlog::info(
                                        "Published material pixel readback: texture={} resource={} bytes={} dimensions={}x{} output='{}'.",
                                        label, resourceID, result.data.size(), result.width, result.height,
                                        texturePath.string());
                                });
                            auto requestMetadataPath = texturePath;
                            requestMetadataPath += L".request.txt";
                            std::ofstream requestMetadata(requestMetadataPath, std::ios::trunc);
                            if (requestMetadata) {
                                requestMetadata << "resource_id=" << resourceID << '\n';
                                requestMetadata << "requested=1\n";
                            }
                        };
                        requestTexture("hdr", m_coreResourceProvider.m_HDRColorTarget);
                        if (const auto* primaryView = m_pViewManager
                                ? m_pViewManager->Get(m_context.primaryViewID) : nullptr) {
                            requestTexture("visibility", primaryView->gpu.visibilityBuffer);
                        }
                        requestTexture("surface-base-color", currentRenderGraph->RequestResourcePtr(
                            Builtin::Surface::BaseColorOpacity, true));
                        requestTexture("surface-records", currentRenderGraph->RequestResourcePtr(
                            Builtin::Surface::Records, true));
                    }
                }
            }
            std::free(outputPath);
        }

    };

    auto& deviceManager = DeviceManager::GetInstance();

    runCapturedStage("RenderGraphUpdate", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::RenderGraphUpdate");
        // Dynamic wrapper selection is part of serialized preparation state.
        // Do not mutate it while the preceding preparation owner may still be
        // freezing that frame's concrete resource table.
        currentRenderGraph->WaitForPreparation();
        if (m_dynamicPresentationColor &&
            m_preparationFrameIndex < m_presentationColorResources.size()) {
            m_dynamicPresentationColor->SetResource(
                m_presentationColorResources[m_preparationFrameIndex]);
        }
        currentRenderGraph->Update(context, deviceManager.GetDevice());
    });
    if (asyncFrameQueue) {
        m_preparationFrameIndex = static_cast<uint8_t>(
            (m_preparationFrameIndex + 1) % m_numFramesInFlight);
        BT_PLOT("ORG.AsyncExecution.NextPreparationSlot",
            static_cast<int64_t>(m_preparationFrameIndex));
    }
    ProbeGraphicsCommandListCreation(deviceManager.GetDevice(), "after RenderGraphUpdate");

    // Clear transform-update tags only after render-graph update so passes such as
    // virtual shadow invalidation can still consume same-frame movement signals.
    // Renderables remain dirty for one additional frame. That second upload copies
    // the current model into prevModel, preventing a one-frame transform change
    // from producing motion vectors indefinitely while the object is static.
    world.defer_begin();
    m_renderTransformUpdatedCleanupQuery.each([](flecs::entity e) {
        if (!e.has<Components::RenderableObject>()) {
            e.remove<Components::RenderTransformUpdated>();
        } else if (e.has<Components::RenderTransformNeedsConvergence>()) {
            e.remove<Components::RenderTransformNeedsConvergence>();
            e.remove<Components::RenderTransformUpdated>();
        } else {
            e.add<Components::RenderTransformNeedsConvergence>();
        }
    });
    world.defer_end();

    if (!m_externalSceneMode) {
        runCapturedStage("ScheduleSceneUpdate", [&]() {
            BT_ZONE_SCOPE("Renderer::Update::ScheduleSceneUpdate");
            ScheduleSceneUpdateTask(elapsedSeconds);
        });
    }

    runCapturedStage("BeginUploadPolicyFrame", [&]() {
        BT_ZONE_SCOPE("Renderer::Update::BeginUploadPolicyFrame");
        org::runtime::BeginUploadPolicyFrame();
    });

    auto graphicsQueue = deviceManager.GetGraphicsQueue();
    auto computeQueue = deviceManager.GetComputeQueue();
    runCapturedStage("FrameMaintenance", [&]() {
        if (currentRenderGraph) {
            BT_ZONE_SCOPE("Renderer::Update::FrameStatistics");
            if (auto* statisticsService = currentRenderGraph->GetStatisticsService()) {
                statisticsService->OnFrameComplete(m_frameIndex, computeQueue); // Gather statistics for the last iteration of the frame
                statisticsService->OnFrameComplete(m_frameIndex, graphicsQueue); // Gather statistics for the last iteration of the frame
            }
        }
        });
    ProbeGraphicsCommandListCreation(deviceManager.GetDevice(), "after FrameMaintenance");
}

void Renderer::PostUpdate() {
    BT_ZONE_SCOPE("Renderer::PostUpdate");
	if (!currentScene) {
        return;
    }
	currentScene->PostUpdate();
}

bool Renderer::RequestPipelineReplacement(br::pipeline::PipelineRecipe recipe) {
    const auto validation = recipe.Validate();
    if (!validation.valid) {
        for (const auto& error : validation.errors) {
            spdlog::error("Renderer rejected pipeline replacement: {}", error);
        }
        return false;
    }

    std::scoped_lock lock(m_pipelineRecipeMutex);
    m_pendingPipelineRecipe = std::move(recipe);
    return true;
}

br::pipeline::PipelineRecipe Renderer::GetPipelineRecipeForMutation() const {
    std::scoped_lock lock(m_pipelineRecipeMutex);
    return m_pendingPipelineRecipe ? *m_pendingPipelineRecipe : m_pipelineRecipe;
}

void Renderer::ApplyPendingPipelineReplacement() {
    std::optional<br::pipeline::PipelineRecipe> pending;
    {
        std::scoped_lock lock(m_pipelineRecipeMutex);
        pending.swap(m_pendingPipelineRecipe);
    }
    if (!pending) {
        return;
    }

    m_pipelineRollbackRecipe = m_pipelineRecipe;
    m_pipelineRecipe = std::move(*pending);
    m_pipelineExtensionsDirty = true;
    rebuildRenderGraph = true;

    m_syncingPipelineTopologySettings = true;
    auto& settings = SettingsManager::GetInstance();
    settings.getSettingSetter<bool>("enableTerrainRvt")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRvtTechnique>());
    settings.getSettingSetter<bool>(CLodDisableReyesRasterizationSettingName)(
        m_pipelineRecipe.Options<br::pipeline::ClusterLodTechnique>().reyes == br::pipeline::ReyesMode::Disabled);
    settings.getSettingSetter<bool>("enableGTAO")(
        m_pipelineRecipe.Contains<br::pipeline::GtaoTechnique>());
    settings.getSettingSetter<bool>("enableClusteredLighting")(
        m_pipelineRecipe.Contains<br::pipeline::ClusteredLightingTechnique>());
    settings.getSettingSetter<bool>("enableBloom")(
        m_pipelineRecipe.Contains<br::pipeline::BloomTechnique>());
    settings.getSettingSetter<bool>("enableTerrainRegionMaterialEvaluation")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRegionMaterialEvaluationTechnique>());
    settings.getSettingSetter<bool>("enableShadows")(
        m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>());
    m_syncingPipelineTopologySettings = false;
}

void Renderer::HandlePipelineReplacementFailure(const std::exception& error) {
    spdlog::error("Renderer pipeline replacement failed: {}", error.what());
    if (m_pipelineReplacementDebugBreakHandler) {
        m_pipelineReplacementDebugBreakHandler();
    }
    if (!m_pipelineRollbackRecipe) {
        throw;
    }

    m_pipelineRecipe = std::move(*m_pipelineRollbackRecipe);
    m_pipelineRollbackRecipe.reset();
    m_pipelineExtensionsDirty = true;
    rebuildRenderGraph = true;
    m_syncingPipelineTopologySettings = true;
    auto& settings = SettingsManager::GetInstance();
    settings.getSettingSetter<bool>("enableTerrainRvt")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRvtTechnique>());
    settings.getSettingSetter<bool>(CLodDisableReyesRasterizationSettingName)(
        m_pipelineRecipe.Options<br::pipeline::ClusterLodTechnique>().reyes == br::pipeline::ReyesMode::Disabled);
    settings.getSettingSetter<bool>("enableGTAO")(
        m_pipelineRecipe.Contains<br::pipeline::GtaoTechnique>());
    settings.getSettingSetter<bool>("enableClusteredLighting")(
        m_pipelineRecipe.Contains<br::pipeline::ClusteredLightingTechnique>());
    settings.getSettingSetter<bool>("enableBloom")(
        m_pipelineRecipe.Contains<br::pipeline::BloomTechnique>());
    settings.getSettingSetter<bool>("enableTerrainRegionMaterialEvaluation")(
        m_pipelineRecipe.Contains<br::pipeline::TerrainRegionMaterialEvaluationTechnique>());
    settings.getSettingSetter<bool>("enableShadows")(
        m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>());
    m_syncingPipelineTopologySettings = false;
}

void Renderer::MaybeRequestCLodVisibilityTelemetry() {
    if (!currentRenderGraph) {
        return;
    }

    if (!IsCLodVisibilityTelemetryDebugEnabled()) {
        if (m_clodVisibilityTelemetryDebugEnabledByRenderer) {
            SetCLodWorkGraphTelemetryEnabled(false);
            m_clodVisibilityTelemetryDebugEnabledByRenderer = false;
            m_loggedCLodVisibilityTelemetryEnabled = false;
        }
        return;
    }

    SetCLodWorkGraphTelemetryEnabled(true);
    m_clodVisibilityTelemetryDebugEnabledByRenderer = true;

    if (!m_loggedCLodVisibilityTelemetryEnabled) {
        spdlog::info(
            "SARP CLOD visibility telemetry debug enabled (setting '{}' or SARP_CLOD_VISIBILITY_TELEMETRY).",
            CLodVisibilityTelemetryDebugSettingName);
        m_loggedCLodVisibilityTelemetryEnabled = true;
    }

    constexpr uint64_t kCaptureIntervalFrames = 30;
    if (m_lastCLodVisibilityTelemetryRequestFrame != UINT64_MAX &&
        m_totalFramesRendered - m_lastCLodVisibilityTelemetryRequestFrame < kCaptureIntervalFrames) {
        return;
    }
    if (m_clodTelemetryReadbackPending ||
        m_clodRasterArgsReadbackPending ||
        m_clodVisibleCounterReadbackPending ||
        m_clodVisibleRecordsReadbackPending ||
        m_clodReplayStateReadbackPending) {
        return;
    }

    auto* readbackService = currentRenderGraph->GetReadbackService();
    if (!readbackService) {
        return;
    }

    auto& world = RendererECSManager::GetInstance().GetWorld();
    const auto visibilityTag = world.component<CLodExtensionVisibilityBufferTag>();

    std::shared_ptr<Resource> telemetryResource;
    world.query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!telemetryResource) {
                telemetryResource = component.resource.lock();
            }
        });

    std::shared_ptr<Resource> visibleCounterResource;
    world.query_builder<const Components::Resource>()
        .with<VisibleClustersCounterTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!visibleCounterResource) {
                visibleCounterResource = component.resource.lock();
            }
        });

    std::shared_ptr<Resource> visibleRecordsResource;
    world.query_builder<const Components::Resource>()
        .with<VisibleClustersBufferTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!visibleRecordsResource) visibleRecordsResource = component.resource.lock();
        });

    std::shared_ptr<Resource> rasterArgsResource;
    world.query_builder<const Components::Resource>()
        .with<CLodPrimaryPhase1RasterIndirectArgsTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!rasterArgsResource) {
                rasterArgsResource = component.resource.lock();
            }
        });

    std::shared_ptr<Resource> replayStateResource;
    world.query_builder<const Components::Resource>()
        .with<CLodOcclusionReplayStateBufferTag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!replayStateResource) {
                replayStateResource = component.resource.lock();
            }
        });

    if (!telemetryResource || !rasterArgsResource || !visibleCounterResource ||
        !visibleRecordsResource || !replayStateResource) {
        return;
    }

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastCLodVisibilityTelemetryRequestFrame = requestedFrame;
    m_clodTelemetryReadbackPending = true;
    m_clodRasterArgsReadbackPending = true;
    m_clodVisibleCounterReadbackPending = true;
    m_clodVisibleRecordsReadbackPending = true;
    m_clodReplayStateReadbackPending = true;

    uint32_t primaryActiveSetWorkloads = 0u;
    uint32_t primaryActiveSetMembers = 0u;
    struct VisibleRecordAudit {
        std::mutex mutex;
        std::optional<std::uint32_t> count;
        std::optional<std::vector<std::byte>> indices;
        std::unordered_set<std::uint32_t> allowed;
        std::uint32_t residentDrawRecordCount = 0;
        std::uint64_t drawRecordsRevision = 0;
        std::uint64_t indirectRevision = 0;
        bool reported = false;

        void TryReport(std::uint64_t frame) {
            std::lock_guard lock(mutex);
            if (reported || !count || !indices) return;
            constexpr std::size_t recordBytes = sizeof(std::uint32_t) * 4u;
            const auto available = static_cast<std::uint32_t>(indices->size() / recordBytes);
            const auto used = (std::min)(*count, available);
            std::uint32_t matched = 0, unpublished = 0, outOfRange = 0;
            for (std::uint32_t i = 0; i < used; ++i) {
                std::uint32_t packedIdentity = 0;
                std::memcpy(&packedIdentity, indices->data() + i * recordBytes, sizeof(packedIdentity));
                const auto drawRecordIndex = (packedIdentity >> 8u) & 0xFFFFFFu;
                if (drawRecordIndex >= residentDrawRecordCount) ++outOfRange;
                if (allowed.contains(drawRecordIndex)) ++matched; else ++unpublished;
            }
            reported = true;
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.VisibleRecordsMatched", matched);
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.VisibleRecordsUnpublished", unpublished);
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.VisibleRecordsOutOfRange", outOfRange);
            spdlog::info(
                "SARP CLOD visible-record audit: frame={} draw_revision={} indirect_revision={} "
                "visible={} available={} allowed={} matched={} unpublished={} out_of_range={}",
                frame, drawRecordsRevision, indirectRevision, *count, available, allowed.size(),
                matched, unpublished, outOfRange);
        }
    };
    auto visibleTransformAudit = std::make_shared<VisibleRecordAudit>();
    if (const auto published = m_context.publishedRendererState) {
        visibleTransformAudit->drawRecordsRevision = published->drawRecords.revision;
        visibleTransformAudit->indirectRevision = published->indirectWorkloads.revision;
        const auto objects = published->drawRecords.payload.Get<br::render::PublishedObjectBufferState>();
        const auto indirect = published->indirectWorkloads.payload.Get<br::render::PublishedIndirectState>();
        const auto drawVersion = objects ? objects->FindVersion(br::render::kObjectDrawRecordVariant) : nullptr;
        const auto generationVersion = objects ? objects->FindVersion(br::render::kObjectVisibilityGenerationVariant) : nullptr;
        visibleTransformAudit->residentDrawRecordCount = drawVersion
            ? static_cast<std::uint32_t>((std::min<std::uint64_t>)(drawVersion->elementCount, UINT32_MAX)) : 0u;
        const auto drawBytes = drawVersion ? drawVersion->MaterializeCpuImage() : nullptr;
        const auto generationBytes = generationVersion ? generationVersion->MaterializeCpuImage() : nullptr;
        if (drawBytes && generationBytes && indirect) {
            for (const auto& active : indirect->activeListVersions) {
                const auto activeBytes = active.version ? active.version->MaterializeCpuImage() : nullptr;
                if (!activeBytes) continue;
                const auto activeByteCount = (std::min<std::size_t>)(activeBytes->size(),
                    static_cast<std::size_t>(active.version->elementCount) * sizeof(br::render::ActiveDrawEntryDTO));
                for (std::size_t offset = 0; offset + sizeof(br::render::ActiveDrawEntryDTO) <= activeByteCount;
                    offset += sizeof(br::render::ActiveDrawEntryDTO)) {
                    br::render::ActiveDrawEntryDTO entry{};
                    std::memcpy(&entry, activeBytes->data() + offset, sizeof(entry));
                    const auto generationOffset = static_cast<std::size_t>(entry.drawRecordIndex) * sizeof(std::uint32_t);
                    const auto drawOffset = static_cast<std::size_t>(entry.drawRecordIndex) * sizeof(InstanceDrawRecordCB);
                    if (generationOffset + sizeof(std::uint32_t) > generationBytes->size() ||
                        drawOffset + sizeof(InstanceDrawRecordCB) > drawBytes->size()) continue;
                    std::uint32_t generation = 0;
                    std::memcpy(&generation, generationBytes->data() + generationOffset, sizeof(generation));
                    if (generation != entry.generation) continue;
                    visibleTransformAudit->allowed.insert(entry.drawRecordIndex);
                }
            }
        }
    }
    if (auto* objectManager = m_pObjectManager.get()) {
        auto activeStats = objectManager->SnapshotActiveDrawSetDebugStats();
        std::uint64_t totalSpan = 0;
        std::uint64_t totalLive = 0;
        std::uint64_t totalTombstoneEstimate = 0;
        std::uint64_t totalCpuMatches = 0;
        std::uint64_t totalCpuStale = 0;
        std::uint64_t totalCpuOutOfRange = 0;
        for (const auto& row : activeStats) {
            totalSpan += row.span;
            totalLive += row.liveSize;
            totalTombstoneEstimate += row.tombstoneEstimate;
            totalCpuMatches += row.cpuGenerationMatches;
            totalCpuStale += row.cpuGenerationStale;
            totalCpuOutOfRange += row.cpuGenerationOutOfRange;
        }
        primaryActiveSetWorkloads = static_cast<uint32_t>(activeStats.size());
        primaryActiveSetMembers = static_cast<uint32_t>((std::min<std::uint64_t>)(totalLive, UINT32_MAX));
        spdlog::info(
            "SARP CLOD active-set CPU telemetry: frame={} workloads={} span={} live={} tombstone_est={} cpu_match={} cpu_stale={} cpu_oob={}",
            requestedFrame,
            activeStats.size(),
            totalSpan,
            totalLive,
            totalTombstoneEstimate,
            totalCpuMatches,
            totalCpuStale,
            totalCpuOutOfRange);

        const std::size_t rowsToLog = (std::min<std::size_t>)(activeStats.size(), 6u);
        for (std::size_t i = 0; i < rowsToLog; ++i) {
            const auto& row = activeStats[i];
            const auto bad = row.cpuGenerationStale + row.cpuGenerationOutOfRange;
            if (i >= 3u && bad == 0u) {
                break;
            }
            spdlog::info(
                "SARP CLOD active-set CPU workload[{}]: frame={} flags={} phase={} clodOnly={} span={} live={} tombstone_est={} cpu_match={} cpu_stale={} cpu_oob={}",
                i,
                requestedFrame,
                static_cast<std::uint64_t>(row.workloadKey.compileFlags),
                row.workloadKey.renderPhase.hash,
                row.workloadKey.clodOnly ? 1 : 0,
                row.span,
                row.liveSize,
                row.tombstoneEstimate,
                row.cpuGenerationMatches,
                row.cpuGenerationStale,
                row.cpuGenerationOutOfRange);
        }
    }

    readbackService->RequestReadbackCapture(
        "CLodOpaque::RasterizeClustersPass2",
        telemetryResource.get(),
        RangeSpec{},
        [this, requestedFrame, primaryActiveSetWorkloads, primaryActiveSetMembers](ReadbackCaptureResult&& result) {
            m_clodTelemetryReadbackPending = false;

            constexpr size_t telemetryBytes = sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
            if (result.data.size() < telemetryBytes) {
                spdlog::warn(
                    "SARP CLOD visibility telemetry: frame={} work-graph payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            CLodWorkGraphTelemetryCounters decoded{};
            std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);
            auto counter = [&](CLodWorkGraphCounterIndex idx) -> uint32_t {
                return decoded.counters[static_cast<size_t>(idx)];
            };
            const auto publishCounter = [&](std::string_view name,
                CLodWorkGraphCounterIndex index) {
                basic_telemetry::SetGauge(name,
                    static_cast<std::int64_t>(counter(index)));
            };
            auto distributionCounter = [&](uint32_t depthBin, uint32_t footprintBin) -> uint32_t {
                constexpr uint32_t footprintBinCount = 6u;
                const auto index = static_cast<size_t>(CLodWorkGraphCounterIndex::VoxelRasterDistributionBinBase) +
                    depthBin * footprintBinCount + footprintBin;
                return decoded.counters[index];
            };

            const uint32_t traversalLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords);
            const uint32_t nonresidentLeaves = counter(CLodWorkGraphCounterIndex::SegmentEvaluateNonResidentRefinedChildThreads);
            PublishCLodTelemetrySnapshot(g_clodPrimaryVisibility, CLodPrimaryVisibilitySnapshot{
                .frame = requestedFrame,
                .traversalLeaves = traversalLeaves,
                .errorRejectedLeaves = counter(CLodWorkGraphCounterIndex::TraverseNodesRejectedByErrorRecords),
                .residentLeaves = traversalLeaves > nonresidentLeaves ? traversalLeaves - nonresidentLeaves : 0u,
                .nonresidentLeaves = nonresidentLeaves,
                .visibleClusterWrites = counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites),
                .bucketRecordsDispatched = counter(CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched),
                .histogramInputs = counter(CLodWorkGraphCounterIndex::RasterSortHistogramInputs),
                .histogramTriangleContributors = counter(CLodWorkGraphCounterIndex::RasterSortHistogramTriangleContributors),
                .compactionInputs = counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs),
                .compactionTriangleEmitted = counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted),
                .rasterInitializationFailures = counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed),
                .sourceGroupMismatches = counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch),
                .outputTriangles = counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles),
                .activeSetWorkloads = primaryActiveSetWorkloads,
                .activeSetMembers = primaryActiveSetMembers,
                .depthTileOccupancyAvailable = false,
            });

            // Keep the GPU readback boundaries in structured telemetry.  These
            // counters are specifically intended to distinguish missing scene
            // admission from traversal, bucket, mesh-shader, and pixel-stage
            // collapses in automated runs; the log-only diagnostic was too easy
            // to lose when the host exits immediately after benchmarking.
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Frame",
                static_cast<std::int64_t>(requestedFrame));
            publishCounter("BasicRenderer.CLod.Readback.ObjectCullInRange",
                CLodWorkGraphCounterIndex::ObjectCullInRangeThreads);
            publishCounter("BasicRenderer.CLod.Readback.ObjectCullVisible",
                CLodWorkGraphCounterIndex::ObjectCullVisibleThreads);
            publishCounter("BasicRenderer.CLod.Readback.TraversalLeaves",
                CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords);
            publishCounter("BasicRenderer.CLod.Readback.TraversalEmitted",
                CLodWorkGraphCounterIndex::TraverseNodesTraverseRecordsEmitted);
            publishCounter("BasicRenderer.CLod.Readback.ClusterVisibleWrites",
                CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites);
            publishCounter("BasicRenderer.CLod.Readback.BucketRecords",
                CLodWorkGraphCounterIndex::ClusterCullBucketRecordsDispatched);
            publishCounter("BasicRenderer.CLod.Readback.HistogramInputs",
                CLodWorkGraphCounterIndex::RasterSortHistogramInputs);
            publishCounter("BasicRenderer.CLod.Readback.CompactionInputs",
                CLodWorkGraphCounterIndex::RasterSortCompactionInputs);
            publishCounter("BasicRenderer.CLod.Readback.CompactionTriangles",
                CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
            publishCounter("BasicRenderer.CLod.Readback.RasterArgsNonZeroBuckets",
                CLodWorkGraphCounterIndex::RasterArgsNonZeroBuckets);
            publishCounter("BasicRenderer.CLod.Readback.RasterArgsDispatchGroups",
                CLodWorkGraphCounterIndex::RasterArgsDispatchGroups);
            publishCounter("BasicRenderer.CLod.Readback.RasterGroups",
                CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
            publishCounter("BasicRenderer.CLod.Readback.RasterInRange",
                CLodWorkGraphCounterIndex::RasterMeshShaderInRange);
            publishCounter("BasicRenderer.CLod.Readback.RasterInitFailures",
                CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed);
            publishCounter("BasicRenderer.CLod.Readback.RasterSourceGroupMismatches",
                CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch);
            publishCounter("BasicRenderer.CLod.Readback.RasterOutputTriangles",
                CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
            publishCounter("BasicRenderer.CLod.Readback.PixelInvocations",
                CLodWorkGraphCounterIndex::RasterPixelShaderInvocations);
            publishCounter("BasicRenderer.CLod.Readback.PixelVisibilityWrites",
                CLodWorkGraphCounterIndex::RasterPixelVisibilityWrites);
            publishCounter("BasicRenderer.CLod.Readback.PixelScissorRejected",
                CLodWorkGraphCounterIndex::RasterPixelScissorRejected);
            publishCounter("BasicRenderer.CLod.Readback.PixelTargetBoundsRejected",
                CLodWorkGraphCounterIndex::RasterPixelTargetBoundsRejected);

            const auto compactedTriangles = counter(
                CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted);
            const auto argumentDispatchGroups = counter(
                CLodWorkGraphCounterIndex::RasterArgsDispatchGroups);
            const auto rasterGroups = counter(
                CLodWorkGraphCounterIndex::RasterMeshShaderGroups);
            const auto outputTriangles = counter(
                CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles);
            const auto pixelInvocations = counter(
                CLodWorkGraphCounterIndex::RasterPixelShaderInvocations);
            const auto visibilityWrites = counter(
                CLodWorkGraphCounterIndex::RasterPixelVisibilityWrites);
            const bool collapsed = CLodRasterPipelineCollapsed(compactedTriangles,
                argumentDispatchGroups, rasterGroups, outputTriangles,
                pixelInvocations, visibilityWrites);
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.RasterPipelineCollapsed",
                collapsed ? 1 : 0);
            if (collapsed) {
                basic_telemetry::AddCounter(
                    "BasicRenderer.CLod.Readback.RasterPipelineCollapseSamples");
            }

			spdlog::info(
				"SARP CLOD visibility telemetry: frame={} object(in_range={} visible={} total={} rejected_stale_generation={} rejected_frustum={} rejected_occlusion={} replay_rejected_occlusion={} invalid_bounds={}) traverse(internal={} leaf={} culled={} rejected_error={} active_children={} emitted={} child_frustum={} child_lod={}) stream(request_attempts={} range_rejects={} resident_hits={} request_appends={}) cluster(in_range={} visible_writes={} total={} rejected_frustum={} rejected_condition2={} rejected_occlusion={} rejected_out_of_range={} zero_survivor_waves={} nonresident_leaf={} emit_bucket={}) voxel_object(candidates={} frustum_reject={} visible={} traverse={} root_internal={} root_leaf={}) voxel(leaves={} rejected_error={} desc_hits={} desc_misses={} raster_work={} raster_dropped={}) voxel_raster(groups={} rigid={} skinned={} cube_candidates={} skin_bone_groups={} invalid_cluster={} desc_miss={} invalid_payload={} bad_width={} proj_reject={} scissor_reject={} depth_reject={} dda_miss={} vis_writes={} vis_wins={} vis_losses={} projected_px={} queued_px={} queue_overflow={} nonpos_depth={}) raster(groups={} in_range={} init_failed={} source_group_mismatch={} zero_tri_outputs={} out_tris={}) sort(compact_inputs={} voxel_skipped={} reyes_skipped={} compact_tris={})",
				requestedFrame,
				counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads),
				counter(CLodWorkGraphCounterIndex::ObjectCullVisibleThreads),
				counter(CLodWorkGraphCounterIndex::ObjectCullThreads),
				counter(CLodWorkGraphCounterIndex::ObjectCullRejectedStaleGeneration),
				counter(CLodWorkGraphCounterIndex::ObjectCullRejectedFrustum),
				counter(CLodWorkGraphCounterIndex::ObjectCullRejectedOcclusion),
				counter(CLodWorkGraphCounterIndex::ObjectReplayRejectedOcclusion),
				counter(CLodWorkGraphCounterIndex::ObjectCullInvalidBounds),
				counter(CLodWorkGraphCounterIndex::TraverseNodesInternalNodeRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesCulledNodeRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesRejectedByErrorRecords),
				counter(CLodWorkGraphCounterIndex::TraverseNodesActiveChildThreads),
				counter(CLodWorkGraphCounterIndex::TraverseNodesTraverseRecordsEmitted),
				counter(CLodWorkGraphCounterIndex::ChildPrefilterFrustumCulled),
				counter(CLodWorkGraphCounterIndex::ChildPrefilterLodRejected),
				counter(CLodWorkGraphCounterIndex::StreamRequestAttempts),
				counter(CLodWorkGraphCounterIndex::StreamRequestRangeRejects),
				counter(CLodWorkGraphCounterIndex::StreamResidentHits),
				counter(CLodWorkGraphCounterIndex::StreamRequestAppends),
				counter(CLodWorkGraphCounterIndex::ClusterCullInRangeThreads),
                counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites),
                counter(CLodWorkGraphCounterIndex::ClusterCullThreads),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedFrustum),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCondition2),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOcclusion),
                counter(CLodWorkGraphCounterIndex::ClusterCullRejectedOutOfRange),
                counter(CLodWorkGraphCounterIndex::ClusterCullZeroSurvivorWaves),
                counter(CLodWorkGraphCounterIndex::SegmentEvaluateNonResidentRefinedChildThreads),
                counter(CLodWorkGraphCounterIndex::SegmentEvaluateEmitBucketThreads),
                counter(CLodWorkGraphCounterIndex::VoxelObjectCandidates),
                counter(CLodWorkGraphCounterIndex::VoxelObjectFrustumRejected),
                counter(CLodWorkGraphCounterIndex::VoxelObjectVisible),
                counter(CLodWorkGraphCounterIndex::VoxelObjectTraverseRecords),
                counter(CLodWorkGraphCounterIndex::VoxelRootInternalRecords),
                counter(CLodWorkGraphCounterIndex::VoxelRootLeafRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRejectedByErrorRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageHits),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelSegmentPageMisses),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkRecords),
                counter(CLodWorkGraphCounterIndex::TraverseNodesVoxelRasterWorkDropped),
                counter(CLodWorkGraphCounterIndex::VoxelRasterWorkGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterRigidWorkGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterSkinnedWorkGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterPreparedCubeCandidates),
                counter(CLodWorkGraphCounterIndex::VoxelRasterSkinBoneGroups),
                counter(CLodWorkGraphCounterIndex::VoxelRasterInvalidCluster),
                counter(CLodWorkGraphCounterIndex::VoxelRasterSegmentPageMisses),
                counter(CLodWorkGraphCounterIndex::VoxelRasterInvalidPackedCluster),
                counter(CLodWorkGraphCounterIndex::VoxelRasterInvalidVoxelWidth),
                counter(CLodWorkGraphCounterIndex::VoxelRasterProjectionRejected),
                counter(CLodWorkGraphCounterIndex::VoxelRasterScissorRejected),
                counter(CLodWorkGraphCounterIndex::VoxelRasterDepthRejected),
                counter(CLodWorkGraphCounterIndex::VoxelRasterDdaMisses),
                counter(CLodWorkGraphCounterIndex::VoxelRasterVisibilityWrites),
                counter(CLodWorkGraphCounterIndex::VoxelRasterVisibilityWins),
                counter(CLodWorkGraphCounterIndex::VoxelRasterVisibilityLosses),
                counter(CLodWorkGraphCounterIndex::VoxelRasterProjectedPixels),
                counter(CLodWorkGraphCounterIndex::VoxelRasterQueuedPixels),
                counter(CLodWorkGraphCounterIndex::VoxelRasterQueueOverflow),
                counter(CLodWorkGraphCounterIndex::VoxelRasterNonPositiveDepth),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderInRange),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderInitFailed),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderSourceGroupMismatch),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderZeroTriangleOutputs),
                counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionVoxelSkipped),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionReyesSkipped),
                counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted));

            for (uint32_t depthBin = 0u; depthBin < 8u; ++depthBin) {
                constexpr std::array<uint32_t, 9> depthEdges = {
                    0u, 4096u, 8192u, 16384u, 32768u, 65536u, 131072u, 262144u, UINT32_MAX
                };
                spdlog::info(
                    "SARP CLOD voxel distribution: frame={} depth_bin={} depth_range=[{},{}) occupied_voxels_by_projected_px(<0.5,0.5-1,1-2,2-4,4-8,>=8)=[{},{},{},{},{},{}]",
                    requestedFrame,
                    depthBin,
                    depthEdges[depthBin],
                    depthEdges[depthBin + 1u],
                    distributionCounter(depthBin, 0u),
                    distributionCounter(depthBin, 1u),
                    distributionCounter(depthBin, 2u),
                    distributionCounter(depthBin, 3u),
                    distributionCounter(depthBin, 4u),
                    distributionCounter(depthBin, 5u));
            }
            spdlog::info(
                "SARP CLOD assembly traversal telemetry: frame={} instance_roots={} part_instance_roots={} assembly_part(traverse={} voxel_leaves={} voxel_raster={} triangle_buckets={}) assembly_voxel(leaves={} rejected_error={} suppressed_by_child={} nonresident={} raster_work={})",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::AssemblyInstanceRootRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartInstanceRootRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartTraversalRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartVoxelRasterWorkRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyPartTriangleBucketRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelLeafRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelRejectedByErrorRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelSuppressedByChildRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelNonResidentRecords),
                counter(CLodWorkGraphCounterIndex::AssemblyVoxelRasterWorkRecords));
			spdlog::info(
				"SARP CLOD animated node bounds: frame={} explicit_evaluations={} explicit_frustum_rejections={} overflow_fallbacks={} assembly_fallbacks={} invalid_data_fallbacks={}",
				requestedFrame,
				counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitEvaluations),
				counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitFrustumRejected),
				counter(CLodWorkGraphCounterIndex::NodeBoundsOverflowFallbacks),
				counter(CLodWorkGraphCounterIndex::NodeBoundsAssemblyFallbacks),
				counter(CLodWorkGraphCounterIndex::NodeBoundsInvalidFallbacks));
            spdlog::info(
                "SARP CLOD traversal divergence: frame={} waves={} active_lanes={} avg_active={:.2f} node_type_waves(internal_only={} leaf_only={} mixed={}) skinning(lanes_rigid={} lanes_skinned={} waves_rigid_only={} waves_skinned_only={} waves_mixed={}) child_loops(nodes={} slots={} emitted={} avg_slots={:.2f} survival={:.3f}) explicit_bounds(total_bones={} avg_bones={:.2f} hist_1={} hist_2={} hist_3_4={} hist_5_8={} hist_9_plus={})",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::TraverseWaves),
                counter(CLodWorkGraphCounterIndex::TraverseActiveLanes),
                counter(CLodWorkGraphCounterIndex::TraverseWaves) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseActiveLanes)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseWaves))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::TraverseInternalOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseLeafOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseMixedNodeTypeWaves),
                counter(CLodWorkGraphCounterIndex::TraverseRigidLanes),
                counter(CLodWorkGraphCounterIndex::TraverseSkinnedLanes),
                counter(CLodWorkGraphCounterIndex::TraverseRigidOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseSkinnedOnlyWaves),
                counter(CLodWorkGraphCounterIndex::TraverseMixedSkinningWaves),
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopNodes),
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots),
                counter(CLodWorkGraphCounterIndex::TraverseChildRecordsEmitted),
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopNodes) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildLoopNodes))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildRecordsEmitted)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::TraverseChildLoopSlots))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitEvaluations) != 0u
                    ? static_cast<double>(counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount)) /
                        static_cast<double>(counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitEvaluations))
                    : 0.0,
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount1),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount2),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount3To4),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount5To8),
                counter(CLodWorkGraphCounterIndex::NodeBoundsExplicitBoneCount9Plus));
			spdlog::info(
				"SARP CLOD animated meshlet bounds: frame={} live_evaluations={} invalid_slot_fallbacks={} no_valid_bone_fallbacks={} fallback_frustum_rejections={}",
				requestedFrame,
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedLiveEvaluations),
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedInvalidSlotFallbacks),
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedNoValidBoneFallbacks),
				counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedFallbackFrustumRejected));
            spdlog::info(
                "SARP CLOD occlusion replay telemetry: frame={} node_enqueue_attempts={} cluster_enqueue_attempts={} "
                "phase2_node_launches={} phase2_node_inputs={} phase2_node_emitted={} "
                "phase2_meshlet_launches={} phase2_meshlet_inputs={} phase2_meshlet_emitted={}",
                requestedFrame,
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionNodeReplayEnqueueAttempts),
                counter(CLodWorkGraphCounterIndex::Phase1OcclusionClusterReplayEnqueueAttempts),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeInputRecords),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayNodeRecordsEmitted),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletLaunches),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletInputRecords),
                counter(CLodWorkGraphCounterIndex::Phase2ReplayMeshletBucketRecordsEmitted));
        });

    readbackService->RequestReadbackCapture(
        "CLodOpaque::RasterizeClustersPass1",
        rasterArgsResource.get(),
        RangeSpec{},
        [this, requestedFrame](ReadbackCaptureResult&& result) {
            m_clodRasterArgsReadbackPending = false;

            const size_t commandCount = result.data.size() / sizeof(RasterizeClustersCommand);
            uint64_t dispatchGroups = 0u;
            uint32_t nonZeroCommands = 0u;
            RasterizeClustersCommand firstNonZero{};
            bool foundFirst = false;
            for (size_t i = 0; i < commandCount; ++i) {
                RasterizeClustersCommand command{};
                std::memcpy(&command,
                    result.data.data() + i * sizeof(RasterizeClustersCommand),
                    sizeof(command));
                const uint64_t groups = static_cast<uint64_t>(command.dispatchX) *
                    command.dispatchY * command.dispatchZ;
                if (groups == 0u) {
                    continue;
                }
                ++nonZeroCommands;
                dispatchGroups += groups;
                if (!foundFirst) {
                    firstNonZero = command;
                    foundFirst = true;
                }
            }

            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferBytes",
                static_cast<std::int64_t>(result.data.size()));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferCommands",
                static_cast<std::int64_t>(commandCount));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferNonZeroCommands",
                static_cast<std::int64_t>(nonZeroCommands));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferDispatchGroups",
                static_cast<std::int64_t>(dispatchGroups));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferFirstBase",
                static_cast<std::int64_t>(firstNonZero.baseClusterOffset));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferFirstXDim",
                static_cast<std::int64_t>(firstNonZero.xDim));
            basic_telemetry::SetGauge("BasicRenderer.CLod.Readback.Phase1RasterArgsBufferFirstBucket",
                static_cast<std::int64_t>(firstNonZero.rasterBucketID));

            spdlog::info(
                "SARP CLOD phase-1 raster-args readback: frame={} bytes={} commands={} nonzero={} groups={} "
                "first(base={} xdim={} bucket={} dispatch={},{},{})",
                requestedFrame,
                result.data.size(),
                commandCount,
                nonZeroCommands,
                dispatchGroups,
                firstNonZero.baseClusterOffset,
                firstNonZero.xDim,
                firstNonZero.rasterBucketID,
                firstNonZero.dispatchX,
                firstNonZero.dispatchY,
                firstNonZero.dispatchZ);
        });

    readbackService->RequestReadbackCapture(
        "CLodOpaque::HierarchicalCullingPass2",
        visibleCounterResource.get(),
        RangeSpec{},
        [this, requestedFrame, visibleTransformAudit](ReadbackCaptureResult&& result) {
            m_clodVisibleCounterReadbackPending = false;

            if (result.data.size() < sizeof(uint32_t)) {
                spdlog::warn(
                    "SARP CLOD visibility telemetry: frame={} visible-counter payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            uint32_t visibleClusters = 0;
            std::memcpy(&visibleClusters, result.data.data(), sizeof(uint32_t));
            {
                std::lock_guard lock(visibleTransformAudit->mutex);
                visibleTransformAudit->count = visibleClusters;
            }
            visibleTransformAudit->TryReport(requestedFrame);
            spdlog::info(
                "SARP CLOD visibility counter: frame={} visible_clusters={}",
                requestedFrame,
                visibleClusters);
        });

    readbackService->RequestReadbackCapture(
        "CLodOpaque::HierarchicalCullingPass2",
        visibleRecordsResource.get(),
        RangeSpec{},
        [this, requestedFrame, visibleTransformAudit](ReadbackCaptureResult&& result) {
            m_clodVisibleRecordsReadbackPending = false;
            {
                std::lock_guard lock(visibleTransformAudit->mutex);
                visibleTransformAudit->indices = std::move(result.data);
            }
            visibleTransformAudit->TryReport(requestedFrame);
        });

    readbackService->RequestReadbackCapture(
        "CLodOpaque::HierarchicalCullingPass2",
        replayStateResource.get(),
        RangeSpec{},
        [this, requestedFrame](ReadbackCaptureResult&& result) {
            m_clodReplayStateReadbackPending = false;

            if (result.data.size() < sizeof(CLodReplayBufferState)) {
                spdlog::warn(
                    "SARP CLOD replay-state telemetry: frame={} payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            CLodReplayBufferState state{};
            std::memcpy(&state, result.data.data(), sizeof(state));
            spdlog::info(
                "SARP CLOD replay-state telemetry: frame={} node_writes={} node_dropped={} "
                "meshlet_writes={} meshlet_dropped={} reyes_split_writes={} reyes_split_dropped={} "
                "reyes_dice_writes={} reyes_dice_dropped={}",
                requestedFrame,
                state.nodeWriteCount,
                state.nodeDropped,
                state.meshletWriteCount,
                state.meshletDropped,
                state.reyesSplitWriteCount,
                state.reyesSplitDropped,
                state.reyesDiceWriteCount,
                state.reyesDiceDropped);
        });

}

void Renderer::MaybeRequestCLodVirtualShadowTelemetry()
{
    if (!currentRenderGraph || !IsCLodVirtualShadowTelemetryDebugEnabled()) {
        m_loggedCLodVirtualShadowTelemetryEnabled = false;
        return;
    }

    if (!m_loggedCLodVirtualShadowTelemetryEnabled) {
        spdlog::info(
            "CLOD VSM telemetry enabled (setting '{}' or SARP_CLOD_VSM_TELEMETRY).",
            CLodVirtualShadowTelemetryDebugSettingName);
        m_loggedCLodVirtualShadowTelemetryEnabled = true;
    }
    SetCLodWorkGraphTelemetryEnabled(true);

    constexpr uint64_t kCaptureIntervalFrames = 30u;
    // Startup and resize can replace the aliased shadow resources after the
    // ECS query has found them but before the readback pass is compiled.
    // Wait for one full capture interval so the graph/resource generation is
    // stable before arming the first capture.
    if (m_totalFramesRendered < kCaptureIntervalFrames) {
        return;
    }
    if (m_lastCLodVirtualShadowTelemetryRequestFrame != UINT64_MAX &&
        m_totalFramesRendered - m_lastCLodVirtualShadowTelemetryRequestFrame < kCaptureIntervalFrames) {
        return;
    }
    if (m_clodVirtualShadowTelemetryReadbackPending ||
        m_clodVirtualShadowWorkTelemetryReadbackPending ||
        m_virtualShadowCasterTelemetryReadbacksPending != 0u) {
        return;
    }

    auto* readbackService = currentRenderGraph->GetReadbackService();
    if (!readbackService) {
        return;
    }
    auto& world = RendererECSManager::GetInstance().GetWorld();
    const auto shadowTag = world.component<CLodExtensionShadowTag>();
    std::shared_ptr<Resource> statsResource;
    std::shared_ptr<Resource> workTelemetryResource;
    world.query_builder<const Components::Resource>()
        .with<CLodVirtualShadowStatsTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!statsResource) {
                statsResource = component.resource.lock();
            }
        });
    if (!statsResource) {
        return;
    }
    world.query_builder<const Components::Resource>()
        .with<CLodWorkGraphTelemetryBufferTag>()
        .with<CLodExtensionTypeTag>(shadowTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!workTelemetryResource) {
                workTelemetryResource = component.resource.lock();
            }
        });

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastCLodVirtualShadowTelemetryRequestFrame = requestedFrame;
    m_clodVirtualShadowTelemetryReadbackPending = true;
    readbackService->RequestReadbackCapture(
        "DeferredShadingPass",
        statsResource.get(),
        RangeSpec{},
        [this, requestedFrame](ReadbackCaptureResult&& result) {
            m_clodVirtualShadowTelemetryReadbackPending = false;
            if (result.data.size() < sizeof(CLodVirtualShadowStats)) {
                spdlog::error(
                    "CLOD VSM telemetry frame={}: payload too small ({} < {}).",
                    requestedFrame,
                    result.data.size(),
                    sizeof(CLodVirtualShadowStats));
                return;
            }

            CLodVirtualShadowStats stats{};
            std::memcpy(&stats, result.data.data(), sizeof(stats));
            const bool normalBudgetValid =
                stats.configuredPageRenderBudget == 0u ||
                stats.normalAdmittedPageCount <= stats.configuredPageRenderBudget;
            const bool upgradeBudgetValid =
                stats.configuredUpgradePageRenderBudget == 0u ||
                stats.upgradeAdmittedPageCount <= stats.configuredUpgradePageRenderBudget;
            const bool renderedWithinAdmission =
                stats.normalRenderedPageCount + stats.upgradeRenderedPageCount <= stats.admittedPageCount;
            const uint32_t admittedNotRendered =
                stats.admittedPageCount > stats.normalRenderedPageCount + stats.upgradeRenderedPageCount
                ? stats.admittedPageCount - stats.normalRenderedPageCount - stats.upgradeRenderedPageCount
                : 0u;
            const bool admittedPagesCleared =
                stats.physicalPageClearCount == stats.admittedPageCount;
            const auto sumClipmapCounters = [](const auto& counters) {
                uint32_t total = 0u;
                for (uint32_t value : counters) {
                    total += value;
                }
                return total;
            };

            spdlog::info(
                "CLOD VSM budget frame={}: totalBudget={} upgradeBudget={} eligible(normal={},upgrade={}) admitted(total={},normal={},upgrade={}) deferred(normal={},upgrade={}) rendered(normal={},upgrade={}) admittedNotRendered={} valid(total={},upgrade={},rendered={})",
                requestedFrame,
                stats.configuredPageRenderBudget,
                stats.configuredUpgradePageRenderBudget,
                stats.normalEligiblePageCount,
                stats.upgradeEligiblePageCount,
                stats.admittedPageCount,
                stats.normalAdmittedPageCount,
                stats.upgradeAdmittedPageCount,
                stats.normalDeferredPageCount,
                stats.upgradeDeferredPageCount,
                stats.normalRenderedPageCount,
                stats.upgradeRenderedPageCount,
                admittedNotRendered,
                normalBudgetValid,
                upgradeBudgetValid,
                renderedWithinAdmission);
            spdlog::info(
                "CLOD VSM exact recovery frame={}: fallbackCandidates={} candidateOverflow={} finalizedRaw={} rawOverflow={} finalizedDependencies={} dedupOverflow={} captureRetries={} upgradeInputs(accepted={},rejected={},pageTouches={}) upgradePages(eligible={},admitted={},deferred={},rendered={}) cumulative(inputs={},touches={},admitted={},rendered={})",
                requestedFrame,
                stats.upgradeCandidateInputCount,
                stats.upgradeCandidateAppendOverflowCount,
                stats.upgradeRawPageCount,
                stats.upgradeRawPageOverflowCount,
                stats.readyUpgradePageCount,
                stats.readyUpgradePageOverflowCount,
                stats.invalidUpgradeDependencyCount,
                stats.upgradeInvalidationInputCount,
                stats.upgradeInvalidationRejectedInputCount,
                stats.upgradeInvalidationAllocatedPageTouchCount,
                stats.upgradeEligiblePageCount,
                stats.upgradeAdmittedPageCount,
                stats.upgradeDeferredPageCount,
                stats.upgradeRenderedPageCount,
                stats.cumulativeUpgradeInvalidationInputCount,
                stats.cumulativeUpgradeInvalidationAllocatedPageTouchCount,
                stats.cumulativeUpgradePageAdmittedCount,
                stats.cumulativeUpgradePageRenderedCount);
            spdlog::info(
                "CLOD VSM ownership frame={}: pool(free={},reusable={},allocationRequests={}) pageTableMismatches={} contentValidMismatches={} residentTagMismatches={} renderedWithoutMatchingClear={} syntheticEmptyValid={} newlyAllocated={} staticClears={} dynamicClears={} composed={} admitted={} clearAdmissionInvariant={}",
                requestedFrame,
                stats.freePhysicalPageCount,
                stats.reusablePhysicalPageCount,
                stats.allocationRequestCount,
                stats.pageTableOwnerMismatchCount,
                stats.contentValidOwnerMismatchCount,
                stats.markResidentTagMismatchCount,
                stats.renderedWithoutMatchingClearCount,
                stats.syntheticEmptyValidPageCount,
                stats.newlyAllocatedPageCount,
                stats.physicalPageClearCount,
                stats.dynamicPageClearCount,
                stats.composedPageCount,
                stats.admittedPageCount,
                admittedPagesCleared);
            constexpr uint64_t physicalPageTexelCount =
                static_cast<uint64_t>(CLodVirtualShadowPhysicalPageSize) *
                static_cast<uint64_t>(CLodVirtualShadowPhysicalPageSize);
            const uint64_t staticQueuedTexels =
                static_cast<uint64_t>(stats.physicalPageClearCount) * physicalPageTexelCount;
            const uint64_t dynamicQueuedTexels =
                static_cast<uint64_t>(stats.dynamicPageClearCount) * physicalPageTexelCount;
            CLodVirtualShadowPageAttributionSnapshot pageAttribution{
                .frame = requestedFrame,
                .pageSize = CLodVirtualShadowPhysicalPageSize,
                .staticQueuedPages = stats.physicalPageClearCount,
                .dynamicQueuedPages = stats.dynamicPageClearCount,
                .composedPages = stats.composedPageCount,
                .admittedPages = stats.admittedPageCount,
            };
            std::copy_n(stats.selectedPixels, pageAttribution.selectedPixels.size(), pageAttribution.selectedPixels.begin());
            std::copy_n(stats.requestedPages, pageAttribution.requestedPages.size(), pageAttribution.requestedPages.begin());
            std::copy_n(stats.allocatedPageTableEntries, pageAttribution.allocatedPages.size(), pageAttribution.allocatedPages.begin());
            std::copy_n(stats.visitedPageTableEntries, pageAttribution.visitedPages.size(), pageAttribution.visitedPages.begin());
            PublishCLodTelemetrySnapshot(g_clodVsmPageAttribution, pageAttribution);
            spdlog::info(
                "CLOD VSM page area frame={}: pageSize={} staticQueued(pages={},texels={}) dynamicQueued(pages={},texels={}) totalQueued(pages={},texels={}) composed(pages={},texels={})",
                requestedFrame,
                CLodVirtualShadowPhysicalPageSize,
                stats.physicalPageClearCount,
                staticQueuedTexels,
                stats.dynamicPageClearCount,
                dynamicQueuedTexels,
                stats.physicalPageClearCount + stats.dynamicPageClearCount,
                staticQueuedTexels + dynamicQueuedTexels,
                stats.composedPageCount,
                static_cast<uint64_t>(stats.composedPageCount) * physicalPageTexelCount);
            spdlog::info(
                "CLOD VSM raster expansion frame={}: swBlocks(requested={},committed={},dropped={}) pageJobs(requested={},committed={},dropped={},doubleSided={})",
                requestedFrame,
                stats.blockExpandedRequestedRecordCount,
                stats.blockExpandedCommittedRecordCount,
                stats.blockExpandedDroppedRecordCount,
                stats.pageJobRequestedRecordCount,
                stats.pageJobCommittedRecordCount,
                stats.pageJobDroppedRecordCount,
                stats.pageJobDoubleSidedRecordCount);
            spdlog::info(
                "CLOD VSM dirty sources frame={}: residentDirtyHits={} dirtyPages={} visitedDirty={} predictiveInvalidated={} currentBoundsInvalidated={} previousBoundsInvalidated={}",
                requestedFrame,
                sumClipmapCounters(stats.markResidentDirtyHits),
                sumClipmapCounters(stats.dirtyPageTableEntries),
                sumClipmapCounters(stats.visitedDirtyPageTableEntries),
                sumClipmapCounters(stats.predictiveInvalidatedPageTableEntries),
                sumClipmapCounters(stats.invalidatedCurrentBoundsPageTableEntries),
                sumClipmapCounters(stats.invalidatedPreviousBoundsPageTableEntries));
            spdlog::info(
                "CLOD VSM page-job raster frame={}: jobs={} clusterBoundsOverlap={} triangles(total={},depthRejected={},backfaceRejected={},bboxRejected={}) coveredPixels={} pageWrites={} emptyJobs={}",
                requestedFrame,
                stats.pageJobRasterJobCount,
                stats.pageJobRasterClusterBoundsOverlapCount,
                stats.pageJobRasterTriangleCount,
                stats.pageJobRasterDepthRejectedTriangleCount,
                stats.pageJobRasterBackfaceRejectedTriangleCount,
                stats.pageJobRasterBboxRejectedTriangleCount,
                stats.pageJobRasterCoveredPixelCount,
                stats.pageJobRasterPageWriteCount,
                stats.pageJobRasterJobCount > stats.pageJobRasterPageWriteCount
                    ? stats.pageJobRasterJobCount - stats.pageJobRasterPageWriteCount
                    : 0u);

            if (!normalBudgetValid || !upgradeBudgetValid || !renderedWithinAdmission ||
                !admittedPagesCleared) {
                spdlog::error(
                    "CLOD VSM budget invariant violation frame={}: totalValid={} upgradeValid={} renderedWithinAdmission={} admittedPagesCleared={}.",
                    requestedFrame,
                    normalBudgetValid,
                    upgradeBudgetValid,
                    renderedWithinAdmission,
                    admittedPagesCleared);
            }
        });

    if (workTelemetryResource) {
        m_clodVirtualShadowWorkTelemetryReadbackPending = true;
        readbackService->RequestReadbackCapture(
            "CLodShadow::RasterizeClustersPass1",
            workTelemetryResource.get(),
            RangeSpec{},
            [requestedFrame](ReadbackCaptureResult&& result) {
                constexpr size_t telemetryBytes =
                    sizeof(uint32_t) * static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    return;
                }
                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(decoded.counters.data(), result.data.data(), telemetryBytes);
                const auto counter = [&](CLodWorkGraphCounterIndex index) {
                    return decoded.counters[static_cast<size_t>(index)];
                };
                PublishCLodTelemetrySnapshot(g_clodVsmHardwareAttribution, CLodVirtualShadowHardwareAttributionSnapshot{
                    .frame = requestedFrame,
                    .invocations = counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations),
                    .pageRejected = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected),
                    .writes = counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites),
                });
            });
        readbackService->RequestReadbackCapture(
            // Compose consumes the completed static and dynamic shadow layers,
            // so it is the first stable cross-queue attribution point. Reading
            // at either raster pass can observe only graphics or only compute.
            "CLodShadow::VirtualShadowComposePagesPass",
            workTelemetryResource.get(),
            RangeSpec{},
            [this, requestedFrame](ReadbackCaptureResult&& result) {
                m_clodVirtualShadowWorkTelemetryReadbackPending = false;
                constexpr size_t telemetryBytes =
                    sizeof(uint32_t) *
                    static_cast<size_t>(CLodWorkGraphCounterCount);
                if (result.data.size() < telemetryBytes) {
                    spdlog::warn(
                        "CLOD VSM work telemetry frame={}: payload too small.",
                        requestedFrame);
                    return;
                }
                CLodWorkGraphTelemetryCounters decoded{};
                std::memcpy(
                    decoded.counters.data(),
                    result.data.data(),
                    telemetryBytes);
                PublishCLodTelemetrySnapshot(g_clodVsmWorkAttribution, CLodVirtualShadowWorkAttributionSnapshot{
                    .frame = requestedFrame,
                    .counters = decoded,
                });
                const auto counter = [&](CLodWorkGraphCounterIndex index) {
                    return decoded.counters[static_cast<size_t>(index)];
                };
                spdlog::info(
                "CLOD VSM work frame={}: object(total={},inRange={},visible={},staleRejected={},frustumRejected={},occlusionRejected={},emitted={}) traverse(internal={},leaf={},culled={},emitted={}) cluster(visibleWrites={},skinnedBounds={},dirtyQueries={},dirtyHits={},cleanRejected={}) sort(inputs={},emitted={}) raster(groups={},triangles={},pixels={},pageRejected={},writes={})",
                    requestedFrame,
                    counter(CLodWorkGraphCounterIndex::ObjectCullThreads),
                    counter(CLodWorkGraphCounterIndex::ObjectCullInRangeThreads),
                    counter(CLodWorkGraphCounterIndex::ObjectCullVisibleThreads),
                    counter(CLodWorkGraphCounterIndex::ObjectCullRejectedStaleGeneration),
                    counter(CLodWorkGraphCounterIndex::ObjectCullRejectedFrustum),
                    counter(CLodWorkGraphCounterIndex::ObjectCullRejectedOcclusion),
                    counter(CLodWorkGraphCounterIndex::ObjectCullTraverseRecordsEmitted),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesInternalNodeRecords),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesLeafNodeRecords),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesCulledNodeRecords),
                    counter(CLodWorkGraphCounterIndex::TraverseNodesTraverseRecordsEmitted),
                    counter(CLodWorkGraphCounterIndex::ClusterCullVisibleClusterWrites),
                    counter(CLodWorkGraphCounterIndex::MeshletBoundsSkinnedLiveEvaluations),
                    counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyQueries),
                    counter(CLodWorkGraphCounterIndex::ClusterCullShadowDirtyRegionHits),
                    counter(CLodWorkGraphCounterIndex::ClusterCullRejectedCleanPages),
                    counter(CLodWorkGraphCounterIndex::RasterSortCompactionInputs),
                    counter(CLodWorkGraphCounterIndex::RasterSortCompactionTriangleEmitted),
                    counter(CLodWorkGraphCounterIndex::RasterMeshShaderGroups),
                    counter(CLodWorkGraphCounterIndex::RasterMeshShaderOutputTriangles),
                    counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites));
                spdlog::info(
                    "CLOD VSM routing frame={}: contributing={} hardware={} software={} pageJob={} pageJobReject(alpha={},reyes={},threshold={},disabled={})",
                    requestedFrame,
                    counter(CLodWorkGraphCounterIndex::ClassifyContributing),
                    counter(CLodWorkGraphCounterIndex::ClassifyRoutedHW),
                    counter(CLodWorkGraphCounterIndex::ClassifyRoutedSW),
                    counter(CLodWorkGraphCounterIndex::ClassifyRoutedPageJob),
                    counter(CLodWorkGraphCounterIndex::ClassifyPJRejectAlphaTested),
                    counter(CLodWorkGraphCounterIndex::ClassifyPJRejectReyesDisplacement),
                    counter(CLodWorkGraphCounterIndex::ClassifyPJRejectBelowThreshold),
                    counter(CLodWorkGraphCounterIndex::ClassifyPJRejectDisabled));
                spdlog::info(
                    "CLOD VSM pixel attribution frame={}: hw(invocations={},pageRejected={},writes={}) sw(available=false)",
                    requestedFrame,
                    counter(CLodWorkGraphCounterIndex::RasterPixelShaderInvocations),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowPageRejected),
                    counter(CLodWorkGraphCounterIndex::RasterPixelVirtualShadowWrites));
				spdlog::info(
					"CLOD VSM DynamicWind cache frame={}: bounds(hit={},miss={},insert={},race={},probeFail={},ineligible={}) clusters(eligible={},unique={},duplicate={}) vertices(requested={},skinned={},fallback={},cachedRaster={},inlineRaster={})",
					requestedFrame,
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheHits),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheMisses),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheInsertions),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheRaces),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheProbeFailures),
					counter(CLodWorkGraphCounterIndex::DynamicWindBoundsCacheIneligible),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheEligibleClusters),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheUniqueClusters),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheDuplicateClusters),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheRequestedVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheSkinnedVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheFallbackVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheCachedRasterVertices),
					counter(CLodWorkGraphCounterIndex::DynamicWindSkinCacheInlineRasterVertices));
            });
    }

    struct CasterTelemetryResource {
        std::shared_ptr<Resource> resource;
        std::string providerId;
        std::string completionPassName;
    };
    std::vector<CasterTelemetryResource> casterTelemetryResources;
    world.query_builder<const Components::Resource, const VirtualShadowCasterTelemetryTag>()
        .build()
        .each([&](const Components::Resource& component, const VirtualShadowCasterTelemetryTag& tag) {
            if (auto resource = component.resource.lock()) {
                casterTelemetryResources.push_back({
                    std::move(resource), tag.providerId, tag.completionPassName });
            }
        });
    m_virtualShadowCasterTelemetryReadbacksPending =
        static_cast<uint32_t>(casterTelemetryResources.size());
    for (auto& telemetry : casterTelemetryResources) {
        readbackService->RequestReadbackCapture(
            telemetry.completionPassName,
            telemetry.resource.get(),
            RangeSpec{},
            [this, requestedFrame, providerId = telemetry.providerId](ReadbackCaptureResult&& result) {
                if (m_virtualShadowCasterTelemetryReadbacksPending != 0u) {
                    --m_virtualShadowCasterTelemetryReadbacksPending;
                }
                constexpr size_t counterCount = 8u;
                if (result.data.size() < sizeof(uint32_t) * counterCount) {
                    spdlog::warn(
                        "VSM caster telemetry provider={} frame={}: payload too small ({} bytes).",
                        providerId, requestedFrame, result.data.size());
                    return;
                }
                std::array<uint32_t, counterCount> counters{};
                std::memcpy(counters.data(), result.data.data(), sizeof(counters));
                spdlog::info(
                    "VSM caster telemetry provider={} frame={}: records={} candidates={} activeBlockOverlaps={} staticRecords={} dynamicRecords={} depthWrites={} capacityOverflows={}",
                    providerId, requestedFrame, counters[0], counters[1], counters[2],
                    counters[3], counters[4], counters[5], counters[6]);
            });
    }
}

void Renderer::MaybeRequestObjectReyesAtlasTelemetry() {
    if (!currentRenderGraph) {
        return;
    }

    if (!SettingsManager::GetInstance().getSettingGetter<bool>(ObjectReyesAtlasTelemetryDebugSettingName)()) {
        m_loggedObjectReyesAtlasTelemetryEnabled = false;
        return;
    }

    if (!m_loggedObjectReyesAtlasTelemetryEnabled) {
        spdlog::info(
            "SARP Object Reyes atlas shader telemetry debug enabled (setting '{}').",
            ObjectReyesAtlasTelemetryDebugSettingName);
        m_loggedObjectReyesAtlasTelemetryEnabled = true;
    }

    constexpr uint64_t kCaptureIntervalFrames = 300;
    if (m_lastObjectReyesAtlasTelemetryRequestFrame != UINT64_MAX &&
        m_totalFramesRendered - m_lastObjectReyesAtlasTelemetryRequestFrame < kCaptureIntervalFrames) {
        return;
    }
    if (m_objectReyesAtlasTelemetryPhase1ReadbackPending ||
        m_objectReyesAtlasTelemetryPhase2ReadbackPending) {
        return;
    }

    auto* readbackService = currentRenderGraph->GetReadbackService();
    if (!readbackService) {
        return;
    }

    auto& world = RendererECSManager::GetInstance().GetWorld();
    const auto visibilityTag = world.component<CLodExtensionVisibilityBufferTag>();

    std::shared_ptr<Resource> phase1Resource;
    world.query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase1Tag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!phase1Resource) {
                phase1Resource = component.resource.lock();
            }
        });

    std::shared_ptr<Resource> phase2Resource;
    world.query_builder<const Components::Resource>()
        .with<CLodReyesTelemetryBufferPhase2Tag>()
        .with<CLodExtensionTypeTag>(visibilityTag)
        .build()
        .each([&](const Components::Resource& component) {
            if (!phase2Resource) {
                phase2Resource = component.resource.lock();
            }
        });

    if (!phase1Resource && !phase2Resource) {
        return;
    }

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastObjectReyesAtlasTelemetryRequestFrame = requestedFrame;

    auto logTelemetry = [requestedFrame](const char* phaseLabel, ReadbackCaptureResult&& result) {
        if (result.data.size() < sizeof(CLodReyesTelemetry)) {
            spdlog::warn(
                "SARP Object Reyes atlas shader telemetry: frame={} phase={} payload too small ({} bytes).",
                requestedFrame,
                phaseLabel,
                result.data.size());
            return;
        }

        CLodReyesTelemetry telemetry{};
        std::memcpy(&telemetry, result.data.data(), sizeof(CLodReyesTelemetry));
        const auto normalizeSentinel = [](uint32_t value) {
            return value == 0xFFFFFFFFu ? 0u : value;
        };
        spdlog::info(
            "SARP Object Reyes atlas shader telemetry: frame={} phase={} phaseIndex={} atlasMaterials={} displacementEnabled={} zeroDescriptor={} sourceSamples={} materialSlot=[{},{}] heightDescriptor=[{},{}] sampler=[{},{}] sourceHeightU16=[{},{}] patchSamples={} patchHeightU16=[{},{}] patchUvU16=([{},{}],[{},{}]) pageUvSets=[{},{}] heightUvSet=[{},{}] invalidHeightUvSet={} rasterWork={} patches={} microTris={}",
            requestedFrame,
            phaseLabel,
            telemetry.phaseIndex,
            telemetry.objectReyesAtlasDebugMaterialCount,
            telemetry.objectReyesAtlasDebugDisplacementEnabledCount,
            telemetry.objectReyesAtlasDebugZeroHeightDescriptorCount,
            telemetry.objectReyesAtlasDebugSampleCount,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinMaterialSlot),
            telemetry.objectReyesAtlasDebugMaxMaterialSlot,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinHeightDescriptor),
            telemetry.objectReyesAtlasDebugMaxHeightDescriptor,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinSamplerDescriptor),
            telemetry.objectReyesAtlasDebugMaxSamplerDescriptor,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinHeightValueU16),
            telemetry.objectReyesAtlasDebugMaxHeightValueU16,
            telemetry.objectReyesAtlasDebugPatchSampleCount,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPatchHeightValueU16),
            telemetry.objectReyesAtlasDebugMaxPatchHeightValueU16,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPatchUvXU16),
            telemetry.objectReyesAtlasDebugMaxPatchUvXU16,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPatchUvYU16),
            telemetry.objectReyesAtlasDebugMaxPatchUvYU16,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinPageUvSetCount),
            telemetry.objectReyesAtlasDebugMaxPageUvSetCount,
            normalizeSentinel(telemetry.objectReyesAtlasDebugMinHeightUvSetIndex),
            telemetry.objectReyesAtlasDebugMaxHeightUvSetIndex,
            telemetry.objectReyesAtlasDebugInvalidHeightUvSetCount,
            telemetry.rasterWorkEntryCount,
            telemetry.patchRasterizedPatchCount,
            telemetry.patchRasterizedMicroTriangleCount);
    };

    if (phase1Resource) {
        m_objectReyesAtlasTelemetryPhase1ReadbackPending = true;
        readbackService->RequestReadbackCapture(
            "CLodOpaque::ReyesPatchRasterPass1",
            phase1Resource.get(),
            RangeSpec{},
            [this, logTelemetry](ReadbackCaptureResult&& result) mutable {
                m_objectReyesAtlasTelemetryPhase1ReadbackPending = false;
                logTelemetry("phase1", std::move(result));
            });
    }

    if (phase2Resource) {
        m_objectReyesAtlasTelemetryPhase2ReadbackPending = true;
        readbackService->RequestReadbackCapture(
            "CLodOpaque::ReyesPatchRasterPass2",
            phase2Resource.get(),
            RangeSpec{},
            [this, logTelemetry](ReadbackCaptureResult&& result) mutable {
                m_objectReyesAtlasTelemetryPhase2ReadbackPending = false;
                logTelemetry("phase2", std::move(result));
            });
    }
}

void Renderer::MaybeRequestTerrainRvtTelemetry() {
    if (!currentRenderGraph) {
        return;
    }

    if (!IsTerrainRvtTelemetryDebugEnabled()) {
        m_loggedTerrainRvtTelemetryEnabled = false;
        return;
    }

    if (!SettingsManager::GetInstance().getSettingGetter<bool>("enableTerrainRvt")()) {
        return;
    }

    if (!m_loggedTerrainRvtTelemetryEnabled) {
        spdlog::info(
            "SARP terrain RVT telemetry debug enabled (setting '{}' or SARP_TERRAIN_RVT_TELEMETRY).",
            TerrainRvtTelemetryDebugSettingName);
        m_loggedTerrainRvtTelemetryEnabled = true;
    }

    constexpr uint64_t kCaptureIntervalFrames = 30;
    if (m_lastTerrainRvtTelemetryRequestFrame != UINT64_MAX &&
        m_totalFramesRendered - m_lastTerrainRvtTelemetryRequestFrame < kCaptureIntervalFrames) {
        return;
    }
    if (m_terrainRvtStatsReadbackPending || m_terrainRvtCountersReadbackPending) {
        return;
    }

    auto* readbackService = currentRenderGraph->GetReadbackService();
    if (!readbackService) {
        return;
    }

    auto statsResource = currentRenderGraph->RequestResourcePtr(Builtin::Terrain::RvtStats, /*allowFailure=*/true);
    auto countersResource = currentRenderGraph->RequestResourcePtr(Builtin::Terrain::RvtCounters, /*allowFailure=*/true);
    if (!statsResource || !countersResource) {
        return;
    }

    const uint64_t requestedFrame = m_totalFramesRendered;
    m_lastTerrainRvtTelemetryRequestFrame = requestedFrame;
    m_terrainRvtStatsReadbackPending = true;
    m_terrainRvtCountersReadbackPending = true;
    spdlog::info("SARP terrain RVT telemetry: frame={} queued stats/counters readback.", requestedFrame);

    const auto pageSize = SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainRvtPageSize")();
    const auto borderTexels = SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainRvtBorderTexels")();
    const auto atlasPagesWide = TerrainRvt::AtlasPagesWide();
    const auto atlasPagesHigh = TerrainRvt::AtlasPagesHigh();
    const auto atlasPoolCount = TerrainRvt::AtlasPoolCount();
    const auto clipPageTableResolution = TerrainRvt::ClipPageTableResolution();
    const auto maxTerrainSets = TerrainRvt::MaxTerrainSets();
    const auto maxClipLevels = TerrainRvt::MaxClipLevels();
    const auto maxGeneratedPagesPerFrame = TerrainRvt::MaxGeneratedPagesPerFrame();
    const auto mipCount = SettingsManager::GetInstance().getSettingGetter<uint32_t>("terrainRvtMipCount")();
    const auto sourceTexelsPerWorld = TerrainRvt::SourceTexelsPerWorld();
    const auto basePageWorldSize = TerrainRvt::BasePageWorldSize();
    const float mip0TexelWorldSize = basePageWorldSize / static_cast<float>((std::max)(pageSize, 1u));
    const auto forcedFallback = SettingsManager::GetInstance().getSettingGetter<bool>("forceDirectTerrainRvtFallback")();

    readbackService->RequestReadbackCapture(
        "EvaluateMaterialGroupsPass",
        statsResource.get(),
        RangeSpec{},
        [this, requestedFrame](ReadbackCaptureResult&& result) {
            m_terrainRvtStatsReadbackPending = false;

            if (result.data.size() < sizeof(TerrainRvtTelemetryStatsReadback)) {
                spdlog::warn(
                    "SARP terrain RVT telemetry: frame={} stats payload too small ({} bytes, expected {}).",
                    requestedFrame,
                    result.data.size(),
                    sizeof(TerrainRvtTelemetryStatsReadback));
                return;
            }

            TerrainRvtTelemetryStatsReadback stats{};
            std::memcpy(&stats, result.data.data(), sizeof(stats));
            const uint32_t heightMisses = stats.heightSampleAttempts - (std::min)(stats.heightSampleAttempts, stats.heightSampleHits);
            const uint32_t materialMisses = stats.materialSampleAttempts - (std::min)(stats.materialSampleAttempts, stats.materialSampleHits);
            const auto rangeMinOrZero = [](uint32_t value) {
                return value == 0xffffffffu ? 0u : value;
            };

            spdlog::info(
                "SARP terrain RVT telemetry: frame={} requests(height={} material={} resident_hits={} overflows={} mark_fail={} rect_calls={} rect_pages={}) generation(pages={} height={} material={} combined={} texels={} resident_skips={} alloc_fail={}) samples(height_attempts={} height_hits={} height_misses={} material_attempts={} material_hits={} material_misses={})",
                requestedFrame,
                stats.heightRequests,
                stats.materialRequests,
                stats.residentHits,
                stats.requestOverflows,
                stats.markComputePageFailures,
                stats.markWorldRectCalls,
                stats.markWorldRectPages,
                stats.generatedPages,
                stats.generationHeightPages,
                stats.generationMaterialPages,
                stats.generationCombinedPages,
                stats.generationTexels,
                stats.resolveResidentPages,
                stats.allocationFailures,
                stats.heightSampleAttempts,
                stats.heightSampleHits,
                heightMisses,
                stats.materialSampleAttempts,
                stats.materialSampleHits,
                materialMisses);

            spdlog::info(
                "SARP terrain RVT telemetry fallback: frame={} height(total={} disabled={} forced={} compute_fail={} page_miss={} owner_mismatch={}) material(total={} disabled={} forced={} compute_fail={} page_miss={} owner_mismatch={})",
                requestedFrame,
                stats.heightFallbacks,
                stats.heightDisabledFallbacks,
                stats.heightForcedFallbacks,
                stats.heightComputePageFailures,
                stats.heightPageTableMisses,
                stats.heightOwnerMismatches,
                stats.materialFallbacks,
                stats.materialDisabledFallbacks,
                stats.materialForcedFallbacks,
                stats.materialComputePageFailures,
                stats.materialPageTableMisses,
                stats.materialOwnerMismatches);

            spdlog::info(
                "SARP terrain RVT telemetry mips: frame={} request_height=[{}] request_material=[{}] sample_height=[{}] sample_material=[{}] generated=[{}]",
                requestedFrame,
                FormatTerrainRvtMipHistogram(stats.heightRequestMipHistogram),
                FormatTerrainRvtMipHistogram(stats.materialRequestMipHistogram),
                FormatTerrainRvtMipHistogram(stats.heightSampleMipHistogram),
                FormatTerrainRvtMipHistogram(stats.materialSampleMipHistogram),
                FormatTerrainRvtMipHistogram(stats.generationMipHistogram));

            spdlog::info(
                "SARP terrain RVT telemetry pages: frame={} requested_page_range=[{},{}] resident_page_range=[{},{}] physical_page_range=[{},{}] xor(requested=0x{:08X} resident=0x{:08X} physical=0x{:08X}) coarser_resident_hits={} atlas_pool_mask=0x{:08X}",
                requestedFrame,
                rangeMinOrZero(stats.materialSampleRequestedPageMin),
                stats.materialSampleRequestedPageMax,
                rangeMinOrZero(stats.materialSampleResidentPageMin),
                stats.materialSampleResidentPageMax,
                rangeMinOrZero(stats.materialSamplePhysicalPageMin),
                stats.materialSamplePhysicalPageMax,
                stats.materialSampleRequestedPageXor,
                stats.materialSampleResidentPageXor,
                stats.materialSamplePhysicalPageXor,
                stats.materialSampleCoarserResidentHits,
                stats.materialSampleAtlasPoolMask);

            spdlog::info(
                "SARP terrain RVT telemetry page-domains: frame={} requests=[{},{}] generation=[{},{}] sample_attempts=[{},{}] sample_misses=[{},{}] xor(request=0x{:08X} attempt=0x{:08X} miss=0x{:08X})",
                requestedFrame,
                rangeMinOrZero(stats.requestPageTableMin),
                stats.requestPageTableMax,
                rangeMinOrZero(stats.generationPageTableMin),
                stats.generationPageTableMax,
                rangeMinOrZero(stats.materialSampleAttemptedPageMin),
                stats.materialSampleAttemptedPageMax,
                rangeMinOrZero(stats.materialSamplePageMissRequestedPageMin),
                stats.materialSamplePageMissRequestedPageMax,
                stats.requestPageTableXor,
                stats.materialSampleAttemptedPageXor,
                stats.materialSamplePageMissRequestedPageXor);

            spdlog::info(
                "SARP terrain RVT telemetry height-domain: frame={} attempts=[{},{}] misses=[{},{}] xor(attempt=0x{:08X} miss=0x{:08X}) fast(attempts={} hits={} miss_requests={}) full(attempts={} hits={})",
                requestedFrame,
                rangeMinOrZero(stats.heightSampleAttemptedPageMin),
                stats.heightSampleAttemptedPageMax,
                rangeMinOrZero(stats.heightSamplePageMissRequestedPageMin),
                stats.heightSamplePageMissRequestedPageMax,
                stats.heightSampleAttemptedPageXor,
                stats.heightSamplePageMissRequestedPageXor,
                stats.heightFastSampleAttempts,
                stats.heightFastSampleHits,
                stats.heightFastPageMissRequests,
                stats.heightFullSampleAttempts,
                stats.heightFullSampleHits);

            spdlog::info(
                "SARP terrain RVT telemetry atlas-owner: frame={} owner_collisions={} generation_xor(page=0x{:08X} physical=0x{:08X})",
                requestedFrame,
                stats.physicalPageOwnerCollisions,
                stats.generationPageTableXor,
                stats.generationPhysicalPageXor);
        },
        QueueKind::Copy);

    readbackService->RequestReadbackCapture(
        "EvaluateMaterialGroupsPass",
        countersResource.get(),
        RangeSpec{},
        [this,
         requestedFrame,
         pageSize,
         borderTexels,
         sourceTexelsPerWorld,
         basePageWorldSize,
         mip0TexelWorldSize,
         atlasPagesWide,
         atlasPagesHigh,
         atlasPoolCount,
         clipPageTableResolution,
         maxTerrainSets,
         maxClipLevels,
         maxGeneratedPagesPerFrame,
         mipCount,
         forcedFallback](ReadbackCaptureResult&& result) {
            m_terrainRvtCountersReadbackPending = false;

            constexpr size_t counterBytes = sizeof(uint32_t) * 4u;
            if (result.data.size() < counterBytes) {
                spdlog::warn(
                    "SARP terrain RVT telemetry: frame={} counters payload too small ({} bytes).",
                    requestedFrame,
                    result.data.size());
                return;
            }

            uint32_t counters[4] = {};
            std::memcpy(counters, result.data.data(), counterBytes);
            spdlog::info(
                "SARP terrain RVT telemetry counters: frame={} request_count={} generation_count={} allocated_physical_pages={} counter_overflows={} config(page={} border={} source_texels_per_world={:.3f} clip0_page_world={:.3f} mip0_texel_world={:.5f} atlas={}x{}x{} clip_table={} max_sets={} max_clips={} max_gen_pages={} configured_mips={} forced_fallback={})",
                requestedFrame,
                counters[0],
                counters[1],
                counters[2],
                counters[3],
                pageSize,
                borderTexels,
                sourceTexelsPerWorld,
                basePageWorldSize,
                mip0TexelWorldSize,
                atlasPagesWide,
                atlasPagesHigh,
                atlasPoolCount,
                clipPageTableResolution,
                maxTerrainSets,
                maxClipLevels,
                maxGeneratedPagesPerFrame,
                mipCount,
                forcedFallback ? 1 : 0);
        },
        QueueKind::Copy);
}

void Renderer::Render() {
    BT_ZONE_SCOPE("Renderer::Render");

    const auto runCapturedStage = [this](const char* stageName, auto&& stageFn) {
        const auto stageStart = std::chrono::steady_clock::now();
        stageFn();
        const auto stageEnd = std::chrono::steady_clock::now();
        RecordFrameTaskStage(stageName, br::telemetry::CpuTaskDomain::MainThread, stageStart, stageEnd);
    };

    auto deltaTime = m_frameTimer.tick();
    if (m_deterministicSamplingMode) {
        deltaTime = 0.0f;
    }
    if (!IsSceneReadyForFrame()) {
        return;
    }

    if (!currentRenderGraph) {
        return;
    }

    // Update may intentionally apply publication backpressure while the first
    // complete immutable manifest is being produced.
    if (!m_frameInputs) {
        return;
    }

    // Async mode runs graph update/declaration/preparation on its serialized
    // owner. Transitional render-context assembly still reads a few renderer
    // services, so join before those reads until they are publication-only.
    currentRenderGraph->WaitForPreparation();

    if (!m_swapChainReady) {
        if (!m_loggedSwapChainNotReady) {
            spdlog::critical(
                "Renderer: skipping render because swapchain/backbuffers are not ready frame={} frameIndex={}",
                m_totalFramesRendered,
                static_cast<unsigned>(m_frameIndex));
            m_loggedSwapChainNotReady = true;
        }
        return;
    }

    const bool renderGraphBatchTraceEnabled = SettingsManager::GetInstance().getSettingGetter<bool>("renderGraphBatchTraceEnabled")();

    // Vulkan does not guarantee round-robin swapchain acquisition.  Re-read the
    // acquired image at the last responsible point and bind the graph's dynamic
    // backbuffer to that exact image.  Using the CPU frame slot here can record
    // transitions for a presentable image that was not acquired.
    const uint8_t renderedFrameIndex = m_swapChain
        ? static_cast<uint8_t>(m_swapChain->CurrentImageIndex())
        : m_frameIndex;
    if (renderedFrameIndex != m_frameIndex) {
        spdlog::warn(
            "Renderer: acquired swapchain image changed before render (frame slot={} acquired={}); resynchronizing",
            static_cast<unsigned>(m_frameIndex),
            static_cast<unsigned>(renderedFrameIndex));
        m_frameIndex = renderedFrameIndex;
    }
    if (m_dynamicBackbuffer && renderedFrameIndex < m_backbufferResources.size()) {
        m_dynamicBackbuffer->SetResource(m_backbufferResources[renderedFrameIndex]);
    }

    auto& world = RendererECSManager::GetInstance().GetWorld();
	const Components::DrawStats& drawStats = world.get<Components::DrawStats>();
    auto renderRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    auto outputRes = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("outputResolution")();

    auto& deviceManager = DeviceManager::GetInstance();

    {
        BT_ZONE_SCOPE("Renderer::Render::PrepareRenderContext");
        runCapturedStage("PrepareRenderContext", [&]() {
			if (!currentRenderGraph || !currentRenderGraph->GetDescriptorService()) {
				throw std::runtime_error("Renderer: descriptor service unavailable while preparing render context");
			}
			auto* descriptorService = currentRenderGraph->GetDescriptorService();
            m_context.hasPrimaryCamera = false;
            m_context.primaryViewID = 0;
			m_context.textureDescriptorHeap = descriptorService->GetSRVDescriptorHeap();
			m_context.samplerDescriptorHeap = descriptorService->GetSamplerDescriptorHeap();
            m_context.rtvHeap = rtvHeap.Get();
            m_context.rtvDescriptorSize = rtvDescriptorSize;
            m_context.dsvDescriptorSize = dsvDescriptorSize;
            m_context.swapchainImageIndex = renderedFrameIndex;
            m_context.frameIndex = renderedFrameIndex;
            m_context.frameNumber = m_totalFramesRendered;
            m_context.frameFenceValue = m_currentFrameFenceValue;
            m_context.renderResolution = { renderRes.x, renderRes.y };
            m_context.outputResolution = { outputRes.x, outputRes.y };
            m_context.clodRayTracingSupported = deviceManager.GetCLodRayTracingSupported();
            m_context.rayTracedReflectionsEnabled = m_rayTracedReflections && m_context.clodRayTracingSupported;
            m_context.materialTextureStreamingStats = m_pMaterialManager
                ? m_pMaterialManager->GetMaterialTextureStreamingStats()
                : MaterialTextureStreamingStats{};
            m_context.environmentWork = m_environmentWorkServices;
            m_context.clodRayTracingSystem = m_clodRayTracingSystem;

            if (m_context.rayTracedReflectionsEnabled && m_clodRayTracingSystem && m_pMeshManager) {
                std::scoped_lock rayTracingLock(m_clodRayTracingSystem->FrameOperationMutex());
                m_clodRayTracingSystem->Refresh(*m_pMeshManager);
                m_clodRayTracingSystem->UpdateGpuResources(deviceManager.GetDevice(), deviceManager.GetRayTracingFeatures());
            }
            else if (m_clodRayTracingSystem) {
                std::scoped_lock rayTracingLock(m_clodRayTracingSystem->FrameOperationMutex());
                m_clodRayTracingSystem->Reset();
            }
            m_context.drawStats = drawStats;
            m_context.deltaTime = deltaTime;
            m_context.sceneOverlapStatus = GetSceneOverlapStatus();

            auto primaryCamera = GetValidatedPrimaryRenderCamera(false);
            if (primaryCamera) {
                m_context.hasPrimaryCamera = true;
                m_context.primaryViewID = primaryCamera.get<Components::RenderViewRef>().viewID;
                m_context.primaryCamera = primaryCamera.get<Components::Camera>();
                if (auto depthMap = primaryCamera.try_get<Components::DepthMap>()) {
                    m_context.primaryDepthMap = *depthMap;
                }
            }

            unsigned int globalPSOFlags = 0;
            if (m_imageBasedLighting) {
                globalPSOFlags |= PSOFlags::PSO_IMAGE_BASED_LIGHTING;
            }
            if (m_clusteredLighting) {
                globalPSOFlags |= PSOFlags::PSO_CLUSTERED_LIGHTING;
            }
            if (m_screenSpaceReflections || m_context.rayTracedReflectionsEnabled) {
                globalPSOFlags |= PSOFlags::PSO_SCREENSPACE_REFLECTIONS;
            }
            m_context.globalPSOFlags = globalPSOFlags;
        });

    }

    PassExecutionContext passExecutionContext{};
    passExecutionContext.device = deviceManager.GetDevice();
    passExecutionContext.frameIndex = m_context.frameIndex;
    passExecutionContext.executionSlot = renderedFrameIndex;
    passExecutionContext.frameFenceValue = m_context.frameFenceValue;
    passExecutionContext.deltaTime = m_context.deltaTime;
    passExecutionContext.ownedHostData = m_frameInputs;
    passExecutionContext.hostData = passExecutionContext.ownedHostData.get();

    auto graphicsQueue = deviceManager.GetGraphicsQueue();
    auto computeQueue = deviceManager.GetComputeQueue();

    SyncOpenRenderGraphSettings(m_numFramesInFlight);

    {
        BT_ZONE_SCOPE("Renderer::Render::CLodVisibilityTelemetry");
        MaybeRequestCLodVisibilityTelemetry();
        MaybeRequestCLodVirtualShadowTelemetry();
    }
    runCapturedStage("RenderGraphExecute", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::RenderGraphExecute");
        if (renderGraphBatchTraceEnabled) {
            ProbeGraphicsCommandListCreation(deviceManager.GetDevice(), "before RenderGraph::Execute");
            spdlog::info("Renderer: frame {} entering RenderGraph::Execute", m_totalFramesRendered);
        }
        const rhi::Backend activeBackend = deviceManager.GetBackend();
        try {
            br::telemetry::nvperf::BeginFrameCapture(
                activeBackend,
                deviceManager.GetDevice(),
                graphicsQueue,
                computeQueue,
                m_totalFramesRendered);
            passExecutionContext.beginGpuPassRange = [activeBackend](rhi::CommandList commandList, rhi::Queue queue, const char* queueName, const char* passName) {
                br::telemetry::nvperf::BeginPassRange(activeBackend, commandList, queue, queueName, passName);
            };
            passExecutionContext.endGpuPassRange = [activeBackend](rhi::CommandList commandList, rhi::Queue queue) {
                br::telemetry::nvperf::EndPassRange(activeBackend, commandList, queue);
            };
            currentRenderGraph->Execute(passExecutionContext); // Main render graph execution
            passExecutionContext.beginGpuPassRange = {};
            passExecutionContext.endGpuPassRange = {};
            static const std::filesystem::path submittedCameraTracePath = [] {
                wchar_t* value = nullptr;
                size_t length = 0;
                _wdupenv_s(&value, &length, L"SARP_SUBMITTED_CAMERA_TELEMETRY_PATH");
                std::filesystem::path result = value && value[0] ? value : L"";
                std::free(value);
                return result;
            }();
            if (!submittedCameraTracePath.empty()) {
                const auto submittedData = currentRenderGraph->GetLastSubmittedFrameData();
                const auto* submitted = submittedData ? submittedData->Get<RenderContext>() : nullptr;
                const auto views = submitted ? submitted->viewFamily : nullptr;
                const auto primary = views ? std::find_if(views->views.begin(), views->views.end(),
                    [](const auto& view) { return view.primary; }) : decltype(views->views.begin()){};
                if (submitted && views && primary != views->views.end()) {
                    const auto previousInverse = DirectX::XMMatrixInverse(nullptr, primary->cameraInfo.prevView);
                    std::error_code fileError;
                    if (submittedCameraTracePath.has_parent_path())
                        std::filesystem::create_directories(submittedCameraTracePath.parent_path(), fileError);
                    const bool writeHeader = !std::filesystem::exists(submittedCameraTracePath, fileError) ||
                        std::filesystem::file_size(submittedCameraTracePath, fileError) == 0u;
                    std::ofstream output(submittedCameraTracePath, std::ios::app);
                    if (writeHeader)
                        output << "present_call,owned_frame,preparation_slot,view_snapshot_revision,x,y,z,prev_x,prev_y,prev_z,context_x,context_y,context_z,jitter_x,jitter_y\n";
                    output << m_totalFramesRendered << ',' << submitted->frameNumber << ','
                        << submitted->frameSlot << ',' << views->revision << ','
                        << primary->cameraInfo.positionWorldSpace.x << ','
                        << primary->cameraInfo.positionWorldSpace.y << ','
                        << primary->cameraInfo.positionWorldSpace.z << ','
                        << DirectX::XMVectorGetX(previousInverse.r[3]) << ','
                        << DirectX::XMVectorGetY(previousInverse.r[3]) << ','
                        << DirectX::XMVectorGetZ(previousInverse.r[3]) << ','
                        << submitted->primaryCamera.info.positionWorldSpace.x << ','
                        << submitted->primaryCamera.info.positionWorldSpace.y << ','
                        << submitted->primaryCamera.info.positionWorldSpace.z << ','
                        << submitted->primaryCamera.jitterPixelSpace.x << ','
                        << submitted->primaryCamera.jitterPixelSpace.y << '\n';
                }
            }
            if (renderGraphBatchTraceEnabled) {
                spdlog::info("Renderer: frame {} completed RenderGraph::Execute", m_totalFramesRendered);
            }
        }
        catch (const std::exception& ex) {
            passExecutionContext.beginGpuPassRange = {};
            passExecutionContext.endGpuPassRange = {};
            spdlog::critical("Renderer: frame {} RenderGraph::Execute threw: {}", m_totalFramesRendered, ex.what());
            WriteRendererExceptionNote(
                "RenderGraphExecute",
                m_totalFramesRendered,
                renderedFrameIndex,
                m_currentFrameFenceValue,
                ex);
            spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
                logger->flush();
            });
            throw;
        }
    });

    if (!currentRenderGraph->GetLastPresentDependency()) {
        basic_telemetry::AddCounter("ORG.PresentationTail.DeferredForRecording");
        br::telemetry::nvperf::EndFrameCapture(
            deviceManager.GetBackend(), graphicsQueue, m_totalFramesRendered);
        PublishFrameTaskGraphCapture();
        FrameMark;
        return;
    }

    // Acquire/bind the swapchain image only after scene recording and FIFO
    // submission selected the logical frame. PresentationColor is slot-owned
    // and was left in CopySource by PresentationReadyPass.
    const auto presentationSlot = currentRenderGraph->GetLastExecutedPreparationSlot();
    const auto sourceSlot = static_cast<size_t>(presentationSlot.value_or(renderedFrameIndex));
    if (sourceSlot >= m_presentationColorResources.size()
        || renderedFrameIndex >= m_backbufferResources.size()
        || renderedFrameIndex >= m_commandAllocators.size()
        || renderedFrameIndex >= m_commandLists.size()) {
        throw std::runtime_error("Presentation tail has incomplete slot ownership");
    }
    auto presentationSource = m_presentationColorResources[sourceSlot];
    auto currentBackbufferResource = m_backbufferResources[renderedFrameIndex];
    if (!presentationSource || !currentBackbufferResource
        || !presentationSource->HasValidBackingResource()
        || !currentBackbufferResource->HasHandle()) {
        throw std::runtime_error("Presentation tail has invalid resource bindings");
    }
    auto& presentationAllocator = m_commandAllocators[renderedFrameIndex];
    auto& presentationList = m_commandLists[renderedFrameIndex];
    auto recordPresentationTail = std::make_shared<std::packaged_task<void()>>(
        [presentationSource, currentBackbufferResource,
         &presentationAllocator, &presentationList]() {
    presentationAllocator->Recycle();
    presentationList->Recycle(presentationAllocator.Get());
    rhi::TextureSubresourceRange presentationRange{};
    presentationRange.mipCount = 1;
    presentationRange.layerCount = 1;
    rhi::TextureBarrier backbufferToCopy{};
    backbufferToCopy.texture = currentBackbufferResource->GetHandle();
    backbufferToCopy.range = presentationRange;
    backbufferToCopy.beforeSync = rhi::ResourceSyncState::All;
    backbufferToCopy.afterSync = rhi::ResourceSyncState::Copy;
    backbufferToCopy.beforeAccess = rhi::ResourceAccessType::Present;
    backbufferToCopy.afterAccess = rhi::ResourceAccessType::CopyDest;
    backbufferToCopy.beforeLayout = rhi::ResourceLayout::Present;
    backbufferToCopy.afterLayout = rhi::ResourceLayout::CopyDest;
    rhi::BarrierBatch beforePresentationCopy{};
    beforePresentationCopy.textures = {&backbufferToCopy, 1};
    presentationList->Barriers(beforePresentationCopy);

    rhi::TextureCopyRegion sourceRegion{};
    sourceRegion.texture = presentationSource->GetAPIResource().GetHandle();
    rhi::TextureCopyRegion destinationRegion{};
    destinationRegion.texture = currentBackbufferResource->GetHandle();
    presentationList->CopyTextureRegion(destinationRegion, sourceRegion);

    auto backbufferToPresent = backbufferToCopy;
    backbufferToPresent.beforeSync = rhi::ResourceSyncState::Copy;
    backbufferToPresent.afterSync = rhi::ResourceSyncState::All;
    backbufferToPresent.beforeAccess = rhi::ResourceAccessType::CopyDest;
    backbufferToPresent.afterAccess = rhi::ResourceAccessType::Present;
    backbufferToPresent.beforeLayout = rhi::ResourceLayout::CopyDest;
    backbufferToPresent.afterLayout = rhi::ResourceLayout::Present;
    rhi::BarrierBatch afterPresentationCopy{};
    afterPresentationCopy.textures = {&backbufferToPresent, 1};
    presentationList->Barriers(afterPresentationCopy);
    presentationList->End();
        });
    auto presentationTailReady = recordPresentationTail->get_future();
    const bool presentationTailSubmitted = m_presentationTailScope.Valid()
        && TaskSchedulerManager::GetInstance().SubmitCpu(
            m_presentationTailScope, TaskLane::FrameCritical, TaskDomain::General,
            "Renderer::RecordPresentationTail",
            [recordPresentationTail](const br::TaskContext&) { (*recordPresentationTail)(); });
    if (!presentationTailSubmitted)
        throw std::runtime_error("Presentation tail recording task was rejected");
    recordPresentationTail.reset();
    presentationTailReady.get();
    basic_telemetry::AddCounter("ORG.PresentationTail.WorkerRecorded");

    if (const auto dependency = currentRenderGraph->GetLastPresentDependency();
        dependency && dependency->valid
        && graphicsQueue.Wait(dependency->wait) != rhi::Result::Ok) {
        throw std::runtime_error("Presentation tail failed to wait for scene output");
    }
    auto presentationCommandList = presentationList.Get();
    if (graphicsQueue.Submit({&presentationCommandList, 1}) != rhi::Result::Ok) {
        throw std::runtime_error("Presentation tail submission failed");
    }
    currentRenderGraph->ConfirmPresentationTailSubmission();
    basic_telemetry::AddCounter("ORG.PresentationTail.Submitted");

    // Present the frame
    rhi::Result presentResult = rhi::Result::Ok;
    runCapturedStage("Present", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::Present");
        if (renderGraphBatchTraceEnabled) {
            spdlog::info("Renderer: frame {} calling Present for slot {}", m_totalFramesRendered, renderedFrameIndex);
        }
        // The tail was submitted on the presentation queue immediately before
        // this call, so queue order covers the copy and Present transition.
        presentResult = m_swapChain->Present(!m_allowTearing);
    });
	if (presentResult == rhi::Result::ModeChanged) {
		RECT clientRect{};
		if (m_hwnd && GetClientRect(m_hwnd, &clientRect)) {
			const UINT clientWidth = static_cast<UINT>((std::max)(clientRect.right - clientRect.left, 0L));
			const UINT clientHeight = static_cast<UINT>((std::max)(clientRect.bottom - clientRect.top, 0L));
			if (clientWidth != 0 && clientHeight != 0) {
				spdlog::info(
					"Renderer: presentation reported mode change; rebuilding swapchain for client extent {}x{}",
					clientWidth,
					clientHeight);
				OnResize(clientWidth, clientHeight);
				return;
			}
		}
		spdlog::warn("Renderer: presentation reported mode change but no non-zero client extent is available");
		return;
	}
	if (presentResult != rhi::Result::Ok) {
		spdlog::error("Renderer: swapchain presentation failed with {}", rhi::ResultName(presentResult));
		return;
	}

    runCapturedStage("SignalFence", [&]() {
        BT_ZONE_SCOPE("Renderer::Render::SignalFence");
        const auto preparationSlot = currentRenderGraph->GetLastExecutedPreparationSlot();
        SignalFence(graphicsQueue,
            static_cast<uint8_t>(preparationSlot.value_or(renderedFrameIndex)));
        br::telemetry::nvperf::EndFrameCapture(deviceManager.GetBackend(), graphicsQueue, m_totalFramesRendered);
    });

    AdvanceFrameIndex();

    runCapturedStage("ReadbackRequests", [&]() {
        if (currentRenderGraph) {
            BT_ZONE_SCOPE("Renderer::Render::ReadbackRequests");
            if (auto* readbackService = currentRenderGraph->GetReadbackService()) {
                readbackService->ProcessReadbackRequests(); // Process readback captures
            }
        }
        if (m_pReadbackManager) {
            m_pReadbackManager->ProcessReadbackRequests(); // Save images to disk if requested
        }
    });
    PublishFrameTaskGraphCapture();
    FrameMark;
}

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
    SetAsyncBufferBackingResizeScheduler({});
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
    TrackedEntityToken::ResetHooks();
    Resource::ResetEntityHooks();
    // The ingestion source store borrows the renderer ECS world. Release that
    // association before the singleton destroys the world so late availability
    // checks cannot observe a dangling source boundary.
    m_sceneSourceStateStore.Reset();
    RendererECSManager::GetInstance().Cleanup();
	// ECS components and manager destructors can retire resources. Keep the
	// runtime descriptor/backing services alive through their final release.
    if (retiringDescriptors) retiringDescriptors->Cleanup();
    retiringDescriptors.reset();
	RenderGraph::ShutdownRuntime();
	spdlog::info("Cleaning up swap chain");
    m_swapChain.Reset();
	spdlog::info("Cleaning up device manager");
    DirectStorageManager::GetInstance().Cleanup();
    DeviceManager::GetInstance().Cleanup();
	spdlog::info("Cleanup complete");
}

void Renderer::CheckDebugMessages() {
    auto device = DeviceManager::GetInstance().GetDevice();
    if (device) {
        device.CheckDebugMessages();
    }
}

void Renderer::SetEnvironment(std::string environmentName) {
	setEnvironment(environmentName);
}

std::shared_ptr<Scene>& Renderer::GetCurrentScene() {
    return currentScene;
}

void Renderer::SetCurrentScene(std::shared_ptr<Scene> newScene) {
	if (!newScene) {
    InvalidateSceneOverlapState();
        ClearExternalSnapshotMeshRegistrations();
        m_sceneRenderBridge.Clear(GetSceneIngestionServices());
        if (currentScene) {
            currentScene->Deactivate();
        }
        currentScene.reset();
        rebuildRenderGraph = true;
        IsSceneReadyForFrame(true);
        return;
    }

	if (currentScene != newScene) {
		ClearExternalSnapshotMeshRegistrations();
		m_sceneRenderBridge.Clear(GetSceneIngestionServices());
        if (currentScene) {
            currentScene->Deactivate();
        }
	}

    InvalidateSceneOverlapState();

	newScene->GetRoot().add<Components::ActiveScene>();
    currentScene = newScene;
    //currentScene->SetDepthMap(m_depthMap);
    currentScene->Activate(m_sceneIngestionServices);
    currentScene->PropagateTransforms();
    if (m_sceneRenderOverlapEnabled) {
        BootstrapCommittedSceneSnapshot();
    } else {
        RunSceneBridgeSyncStage();
    }
	m_warnedNullScene = false;
	m_warnedMissingPrimaryCamera = false;
	rebuildRenderGraph = true;
}

std::shared_ptr<Scene> Renderer::AppendScene(std::shared_ptr<Scene> scene) {
	if (!scene) {
        spdlog::warn("Renderer: attempted to append a null scene. Ignoring append request.");
        return nullptr;
    }

	if (m_sceneTaskInFlight.load()) {
        spdlog::warn("Renderer: attempted to append a scene while async scene overlap work is running. Ignoring append request for v1 overlap safety.");
        return nullptr;
    }

	if (!currentScene) {
        spdlog::warn("Renderer: attempted to append a scene while no current scene exists. Ignoring append request.");
        return nullptr;
    }

	auto appendedScene = GetCurrentScene()->AppendScene(scene);
	if (!appendedScene) {
		return nullptr;
	}

	currentScene->PropagateTransforms();
	InvalidateSceneOverlapState();
	if (m_sceneRenderOverlapEnabled) {
		BootstrapCommittedSceneSnapshot();
	} else {
		RunSceneBridgeSyncStage();
	}

	{
		BufferBase::ScopedBackingMutation appendBackingMutation;
		(void)PublishReadyDeferredBackingResizes(true);
	}
	org::runtime::FlushUploadPolicies();
	if (m_pMaterialManager) {
		m_pMaterialManager->CommitGpuVisibleSnapshot();
	}
	if (m_pIndirectCommandBufferManager && m_pObjectManager) {
		m_pIndirectCommandBufferManager->PublishDesiredState(
			m_pObjectManager->DesiredBufferStateRequirement(),
			m_pObjectManager->GetResidentInstanceDrawRecordCount(),
			m_context.publishedRendererState);
	}

	m_warnedNullScene = false;
	m_warnedMissingPrimaryCamera = false;
	rebuildRenderGraph = true;

	return appendedScene;
}

InputManager& Renderer::GetInputManager() {
    return inputManager;
}

void Renderer::SetInputMode(InputMode mode) {
    static WASDContext wasdContext;
    static OrbitalCameraContext orbitalContext;
    switch (mode) {
    case InputMode::wasd:
        inputManager.SetInputContext(&wasdContext);
        break;
    case InputMode::orbital:
        inputManager.SetInputContext(&orbitalContext);
        break;
    }
    SetupInputHandlers();
}

void Renderer::SetCameraSpeed(float speed) {
    if (setCameraSpeed) {
        setCameraSpeed(speed);
    }
}

void Renderer::WaitForAsyncPreparation() {
    if (currentRenderGraph) currentRenderGraph->WaitForPreparation();
}

void Renderer::MoveForward() {
    spdlog::info("Moving forward!");
}

void Renderer::SetupInputHandlers() {
	auto& context = *inputManager.GetCurrentContext();
    context.SetActionHandler(InputAction::MoveForward, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving forward!");
        movementState.forwardMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveBackward, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving forward!");
        movementState.backwardMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveRight, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving right!");
        movementState.rightMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveLeft, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving right!");
        movementState.leftMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveUp, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving up!");
        movementState.upMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::MoveDown, [this](float magnitude, const InputData& inputData) {
        //spdlog::info("Moving up!");
        movementState.downMagnitude = magnitude * getCameraSpeed();
        });

    context.SetActionHandler(InputAction::RotateCamera, [this](float magnitude, const InputData& inputData) {
        horizontalAngle -= static_cast<float>(inputData.mouseDeltaX) * 0.005f;
        verticalAngle -= static_cast<float>(inputData.mouseDeltaY) * 0.005f;
        });

    context.SetActionHandler(InputAction::ZoomIn, [](float magnitude, const InputData& inputData) {
        // TODO
        });

    context.SetActionHandler(InputAction::ZoomOut, [](float magnitude, const InputData& inputData) {
        // TODO
        });

	context.SetActionHandler(InputAction::Reset, [this](float magnitude, const InputData& inputData) {
        m_shaderReloadRequested = true;
		});

    context.SetActionHandler(InputAction::X, [](float magnitude, const InputData& inputData) {
        });

    context.SetActionHandler(InputAction::Z, [](float magnitude, const InputData& inputData) {
        });
}

void Renderer::RegisterPipelineExtensions() {
    currentRenderGraph->RegisterExtension(std::make_unique<RenderGraphIOExtension>(
        std::make_shared<br::render::RenderGraphIOService>(
            *m_pTextureFactory,
            currentRenderGraph->RetainUploadService(),
            *m_pReadbackManager,
            *m_pMaterialManager)),
        "BuiltinIO");
    currentRenderGraph->RegisterExtension(std::make_unique<ReadbackCaptureExtension>(
        currentRenderGraph->GetReadbackServiceOwner()),
        "BuiltinReadbackCapture");

    if (!m_producerPersistentState->clodStreaming) m_producerPersistentState->clodStreaming = std::make_shared<CLodStreamingSystem>();
    if (!m_producerPersistentState->virtualShadowCasters) m_producerPersistentState->virtualShadowCasters = std::make_shared<VirtualShadowCasterRegistry>();
    auto clodStreamingSystem = m_producerPersistentState->clodStreaming;
    clodStreamingSystem->SetGeometryStorage(&m_pMeshManager->GetCLodGeometryStorage());
    auto virtualShadowCasters = m_producerPersistentState->virtualShadowCasters;
    br::pipeline::PipelineBuildContext extensionContext(
        *currentRenderGraph,
        m_pipelineRecipe.Bindings(),
        {},
        [this, clodStreamingSystem, virtualShadowCasters](br::pipeline::TechniqueId id, const br::pipeline::TechniqueOptions& optionsVariant) {
            CLodExtensionType extensionType{};
            const char* extensionId = nullptr;
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
                extensionType = CLodExtensionType::Shadow;
                extensionId = "CLodShadow";
                break;
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
            currentRenderGraph->RegisterExtension(
                std::make_unique<CLodExtension>(
                    extensionType,
                    maxClusters,
                    CLodExtensionOptions{
                        .enableReyes = options.reyes == br::pipeline::ReyesMode::Enabled,
                        .enableVoxelRasterization = voxelRasterizationEnabled,
                        .voxelRasterWorkCapacity = voxelRasterWorkCapacity,
                        .streamingSystem = clodStreamingSystem,
                        .virtualShadowCasters = virtualShadowCasters,
                        .persistentState = m_producerPersistentState,
                        .slabResourceGroup = m_pMeshManager
                            ? m_pMeshManager->GetCLodSlabResourceGroup() : nullptr }),
                extensionId);
        });
    // Recipe extensions may register resources consumed by technique extensions
    // (ProceduralWind visibility is consumed by CLod shadow traversal). Register
    // producers first; structural insertion constraints are resolved only after
    // every extension has been gathered, so this does not require their target
    // passes to have been materialized yet.
    for (const auto& [id, factory] : m_pipelineRecipe.Extensions()) {
		if (id == "SARPGrass" && ReadTruthyEnvironmentFlag("BASICRENDERER_DISABLE_SARP_GRASS_EXTENSION")) {
			spdlog::info("Render-graph isolation: SARPGrass extension disabled");
			continue;
		}
        auto extension = factory();
        if (!extension) {
            throw std::runtime_error("Pipeline extension factory returned null: " + id);
        }
        if (auto* casterProvider = dynamic_cast<IVirtualShadowCasterProvider*>(extension.get())) {
            virtualShadowCasters->Register(*casterProvider);
        }
        currentRenderGraph->RegisterExtension(std::move(extension), id);
    }
    for (const auto& entry : m_pipelineRecipe.Techniques()) {
        entry.technique->RegisterExtensions(extensionContext);
    }
}

void Renderer::CreateRenderGraph() {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph");
    if (!IsSceneReadyForFrame()) {
        rebuildRenderGraph = true;
        return;
    }

    auto primaryCameraEntity = GetValidatedPrimaryRenderCamera(true);
    if (!primaryCameraEntity) {
        spdlog::warn("Renderer: primary camera bridge is not ready during render graph creation. Deferring rebuild.");
        rebuildRenderGraph = true;
        return;
    }

    {
        BT_ZONE_SCOPE("Renderer::CreateRenderGraph::StallPipeline");
        StallPipeline();
    }

    // Render-graph queue timelines are recreated below. Descriptor retirement
    // snapshots contain non-owning timeline handles, so consume all pending
    // releases and clear the snapshot while those timelines are still alive.
    DescriptorHeapManager::GetInstance().DrainDeferredReleasesAfterDeviceIdle();
	PSOManager::GetInstance().DrainRetiredLivePipelinesAfterDeviceIdle();

    // TODO: Find a better way to handle resources like this
    // TODO: this access pattern is stupid
    auto primaryViewID = primaryCameraEntity.get<Components::RenderViewRef>().viewID;
    auto primaryCamera = m_pViewManager->Get(primaryViewID);

    // TODO: Primary camera and current environment will change, and I'd rather not recompile the graph every time that happens.
    // How should we manage swapping out their resources? DynamicResource could work, but the ResourceGroup/independantly managed resource
    // part of the compiler would need to become aware of DynamicResource.

    // TODO: Some of these resources don't really need to be recreated (GTAO, etc.)
    // Instead, just create them externally and register them

        if (!currentRenderGraph)
        {
		currentRenderGraph = std::make_unique<RenderGraph>(DeviceManager::GetInstance().GetDevice(), DeviceManager::GetInstance().GetBackend());
		if (DeviceManager::GetInstance().IsMultiRHIEnabled()) {
			currentRenderGraph->RegisterBackendDevice(DeviceManager::GetInstance().GetPeerBackend(), DeviceManager::GetInstance().GetPeerDevice());
		}
        if (auto* uploadService = currentRenderGraph->GetUploadService()) {
            uploadService->Initialize();
        }
        if (m_uploadPolicyService) {
            m_uploadPolicyService->SetUploadService(currentRenderGraph->RetainUploadService());
            org::runtime::SetActiveUploadPolicyService(m_uploadPolicyService.get());
        }
        if (auto* descriptorService = currentRenderGraph->GetDescriptorService()) {
            descriptorService->Initialize();
        }
        }
        if (m_clodRayTracingSystem && currentRenderGraph)
            m_clodRayTracingSystem->SetUploadService(currentRenderGraph->RetainUploadService());
        if (m_pSkeletonManager && currentRenderGraph)
            m_pSkeletonManager->SetUploadService(currentRenderGraph->RetainUploadService());
        if (currentRenderGraph)
            ::ResourceManager::GetInstance().SetUploadService(currentRenderGraph->RetainUploadService());

        if (!m_renderGraphRuntimeInitialized) {
        currentRenderGraph->GetMemorySnapshotProvider().SetProvider(
            org::memory::CreateECSMemorySnapshotProvider(RendererECSManager::GetInstance().GetWorld()));
        Menu::GetInstance().SetRenderGraph(currentRenderGraph.get());

        if (auto* textureFactory = m_pTextureFactory.get()) {
            textureFactory->SetUploadService(currentRenderGraph->RetainUploadService());
            textureFactory->SetReadbackService(currentRenderGraph->GetReadbackServiceOwner());
        }
        if (m_pEnvironmentManager)
            m_pEnvironmentManager->SetUploadService(currentRenderGraph->RetainUploadService());
        if (m_pMaterialManager) {
			m_pMaterialManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService());
			m_pMaterialManager->SetDescriptorService(currentRenderGraph->RetainDescriptorService());
		}
		if (m_pObjectManager)
			m_pObjectManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService(),
				m_numFramesInFlight);
		if (m_pMeshManager)
			m_pMeshManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService(),
				m_numFramesInFlight);
		if (m_pIndirectCommandBufferManager)
			m_pIndirectCommandBufferManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService());
		if (m_sceneIngestionServices.execution.generation) {
			m_sceneIngestionServices.execution.generation->uploads.store(
				currentRenderGraph->RetainUploadService(), std::memory_order_release);
			m_sceneIngestionServices.execution.generation->descriptors.store(
				currentRenderGraph->RetainDescriptorService(), std::memory_order_release);
		}
		if (m_pTerrainManager)
			m_pTerrainManager->SetRendererStateServices(
				m_rendererStateRequests.get(), currentRenderGraph->RetainUploadService(),
				currentRenderGraph->RetainDescriptorService());


        RendererECSManager::GetInstance().CreateRenderPhaseEntity(Engine::Primary::CLodTransparentPass);
		m_renderGraphRuntimeInitialized = true;
    }

    if (m_pipelineExtensionsDirty) {
        currentRenderGraph->ClearExtensions();
        RegisterPipelineExtensions();
        m_pipelineExtensionsDirty = false;
    }

    auto& newGraph = currentRenderGraph;
	org::runtime::ScopedActiveGraphServices activeGraphServices(
		newGraph->GetUploadService(), newGraph->GetDescriptorService());
    const auto probeGraphBuildPhase = [&](const char* phase) {
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    };

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::ResetForRebuild");
    newGraph->ResetForRebuild();
    // CreateRenderGraph stalls every queue before teardown. ResetForRebuild
    // can retire another wave of resources/descriptors after the pre-reset
    // descriptor drain above. Consume that wave before draining API objects:
    // destroying those resources can enqueue their native resources in
    // DeletionManager. If it is left until normal frame maintenance, those
    // native objects survive for numFramesInFlight + 1 frames and can retain
    // stale graph backing through the rebuilt graph's first frames.
    DescriptorHeapManager::GetInstance().DrainDeferredReleasesAfterDeviceIdle();
	PSOManager::GetInstance().DrainRetiredLivePipelinesAfterDeviceIdle();

    // Everything released above is GPU-idle, so it is safe (and important) to
    // release its native objects before materializing the candidate graph.
    // Otherwise two complete alias-pool generations overlap for the normal
    // frames-in-flight retirement delay and can exhaust VRAM.
    DeletionManager::GetInstance().DrainAll();
    }
    probeGraphBuildPhase("CreateRenderGraph after ResetForRebuild");

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::RegisterProvidersAndExtensions");
    newGraph->RegisterProvider(m_pMeshManager.get());
    newGraph->RegisterProvider(m_pObjectManager.get());
    newGraph->RegisterProvider(m_pViewManager.get());
    newGraph->RegisterProvider(m_pLightManager.get());
    newGraph->RegisterProvider(m_pEnvironmentManager.get());
	newGraph->RegisterProvider(m_pMaterialManager.get());
    newGraph->RegisterProvider(m_pTerrainManager.get());
	newGraph->RegisterProvider(m_pSkeletonManager.get());
    newGraph->RegisterProvider(&m_coreResourceProvider);
    newGraph->PrepareExtensionsForBuild();
    }
    probeGraphBuildPhase("CreateRenderGraph after PrepareExtensionsForBuild");

    // The primary camera is high-frequency frame data, not renderer-state
    // publication. Insert its graphics-queue upload before all pipeline passes;
    // their camera SRV declarations establish the required RAW dependencies.
    newGraph->BuildPass<br::render::PrimaryCameraUploadPass>(
        "PrimaryCameraUploadPass", m_pViewManager->GetCameraBuffer(),
        m_pViewManager->GetCullingCameraBuffer(), m_numFramesInFlight);

    auto& depth = primaryCameraEntity.get<Components::DepthMap>();
    std::shared_ptr<PixelBuffer> depthTexture = depth.depthMap;

    const bool terrainRvtEnabled = m_pipelineRecipe.Contains<br::pipeline::TerrainRvtTechnique>();
    m_materialEvaluationInputs = {};
    m_materialEvaluationInputs.pipelines = &PSOManager::GetInstance();
    m_materialEvaluationInputs.commandSignatures = &CommandSignatureManager::GetInstance();
    m_materialEvaluationInputs.clodSlabResources = m_pMeshManager
        ? m_pMeshManager->GetCLodSlabResourceGroup() : nullptr;
    {
        auto& world = RendererECSManager::GetInstance().GetWorld();
        const auto visibilityTag = world.component<CLodExtensionVisibilityBufferTag>();
        const auto capture = [&](auto componentTag, auto&& accept) {
            world.query_builder<>()
                .with<CLodExtensionTypeTag>(visibilityTag)
                .with(componentTag)
                .build()
                .each([&](flecs::entity entity) {
                    if (const auto resource = entity.try_get<Components::Resource>(); resource) {
                        if (auto retained = std::static_pointer_cast<GloballyIndexedResource>(
                                resource->resource.lock())) {
                            accept(entity, std::move(retained));
                        }
                    }
                });
        };
        capture(world.component<VisibleClustersBufferTag>(), [&](flecs::entity entity, auto resource) {
            m_materialEvaluationInputs.visibleClusters = std::move(resource);
            if (const auto capacity = entity.try_get<CLodVisibleClusterCapacity>(); capacity)
                m_materialEvaluationInputs.visibleClusterCapacity = capacity->maxVisibleClusters;
        });
        capture(world.component<VisibleClustersCounterTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.visibleClusterCounter = std::move(resource);
        });
        capture(world.component<VisibleClusterTransformIndicesBufferTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.visibleClusterTransformIndices = std::move(resource);
        });
        capture(world.component<CLodReyesDiceQueueTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesDiceQueue = std::move(resource);
        });
        capture(world.component<CLodReyesTessTableConfigsTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesTessTableConfigs = std::move(resource);
        });
        capture(world.component<CLodReyesTessTableVerticesTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesTessTableVertices = std::move(resource);
        });
        capture(world.component<CLodReyesTessTableTrianglesTag>(), [&](flecs::entity, auto resource) {
            m_materialEvaluationInputs.reyesTessTableTriangles = std::move(resource);
        });
    }
    br::pipeline::PipelineBuildContext buildContext(
        *newGraph,
        m_pipelineRecipe.Bindings(),
        [&](br::pipeline::TechniqueId id, const br::pipeline::TechniqueOptions&) {
            using enum br::pipeline::TechniqueId;
            switch (id) {
            case FrameResources:
                newGraph->RegisterResource(Builtin::PrimaryCamera::DepthTexture, depthTexture);
                newGraph->RegisterResource(Builtin::PrimaryCamera::LinearDepthMap, depth.linearDepthMap);
                newGraph->RegisterResource(
                    Builtin::PrimaryCamera::ProjectedDepthTexture,
                    m_visibilityRendering ? depth.projectedDepthMap : depthTexture);
                newGraph->RegisterResource(Builtin::Backbuffer, m_dynamicBackbuffer);
                newGraph->RegisterResource(Builtin::PresentationColor, m_dynamicPresentationColor);
                newGraph->RegisterResource(Builtin::PerFrameBuffer, ::ResourceManager::GetInstance().GetPerFrameBuffer());
                break;
            case BrdfIntegration:
                BuildBRDFIntegrationPass(newGraph.get());
                break;
            case Environment: {
                if (m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap)) m_defaultEnvironmentCubemap.reset();
                if (m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentPrefilteredCubemap)) m_defaultEnvironmentPrefilteredCubemap.reset();
                if ((!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap) && !m_defaultEnvironmentCubemap) ||
                    (!m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentPrefilteredCubemap) && !m_defaultEnvironmentPrefilteredCubemap)) {
                    CreateDefaultEnvironmentResources();
                }
                BuildEnvironmentPipeline(newGraph.get());
                auto currentCubemap = m_defaultEnvironmentCubemap;
                auto currentPrefiltered = m_defaultEnvironmentPrefilteredCubemap;
                if (m_currentEnvironment && m_currentEnvironment->GetEnvironmentCubemap() &&
                    m_currentEnvironment->GetEnvironmentCubemap()->ImagePtr() &&
                    m_currentEnvironment->GetEnvironmentPrefilteredCubemap()) {
                    currentCubemap = m_currentEnvironment->GetEnvironmentCubemap()->ImagePtr();
                    currentPrefiltered = m_currentEnvironment->GetEnvironmentPrefilteredCubemap();
                    m_warnedUsingFallbackEnvironment = false;
                }
                else if (!m_warnedUsingFallbackEnvironment) {
                    spdlog::warn("Renderer: no valid environment is active. Using fallback blank cubemaps.");
                    m_warnedUsingFallbackEnvironment = true;
                }
                const auto registerEnvironment = [&](ResourceIdentifier resourceId, std::shared_ptr<Resource> fallback) {
                    if (const auto* binding = m_pipelineRecipe.Bindings().Find(resourceId)) {
                        const auto& contract = binding->contract;
                        if (contract.initialAccess != rhi::ResourceAccessType::None ||
                            contract.initialLayout != rhi::ResourceLayout::Undefined ||
                            contract.initialSync != rhi::ResourceSyncState::None) {
                            if (auto* tracker = binding->resource->GetStateTracker()) {
                                tracker->Reset(RangeSpec{}, ResourceState{ contract.initialAccess, contract.initialLayout, contract.initialSync });
                            }
                        }
                        newGraph->RegisterResource(resourceId, binding->resource);
                    }
                    else {
                        newGraph->RegisterResource(resourceId, std::move(fallback));
                    }
                };
                registerEnvironment(Builtin::Environment::CurrentCubemap, currentCubemap);
                registerEnvironment(Builtin::Environment::CurrentPrefilteredCubemap, currentPrefiltered);
                if (m_blueNoiseTexture) newGraph->RegisterResource(Builtin::Noise::BlueNoise2D, m_blueNoiseTexture);
                if (m_openPBRLookupResources.idealDielectricEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealDielectricEnergyComplement, m_openPBRLookupResources.idealDielectricEnergyComplement);
                if (m_openPBRLookupResources.idealDielectricAverageEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealDielectricAverageEnergyComplement, m_openPBRLookupResources.idealDielectricAverageEnergyComplement);
                if (m_openPBRLookupResources.idealDielectricReflectionRatio) newGraph->RegisterResource(Builtin::OpenPBR::IdealDielectricReflectionRatio, m_openPBRLookupResources.idealDielectricReflectionRatio);
                if (m_openPBRLookupResources.opaqueDielectricEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::OpaqueDielectricEnergyComplement, m_openPBRLookupResources.opaqueDielectricEnergyComplement);
                if (m_openPBRLookupResources.opaqueDielectricAverageEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::OpaqueDielectricAverageEnergyComplement, m_openPBRLookupResources.opaqueDielectricAverageEnergyComplement);
                if (m_openPBRLookupResources.idealMetalEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealMetalEnergyComplement, m_openPBRLookupResources.idealMetalEnergyComplement);
                if (m_openPBRLookupResources.idealMetalAverageEnergyComplement) newGraph->RegisterResource(Builtin::OpenPBR::IdealMetalAverageEnergyComplement, m_openPBRLookupResources.idealMetalAverageEnergyComplement);
                if (m_openPBRLookupResources.fuzzLTC) newGraph->RegisterResource(Builtin::OpenPBR::FuzzLTC, m_openPBRLookupResources.fuzzLTC);
                break;
            }
            case ClusterLod:
            case ClusterLodAlpha:
            case ClusterLodShadow:
            case ClusterLodVoxel:
                break; // These techniques own ordered graph extensions.
            case CanonicalSurfaceResources: {
                const auto resolution = SettingsManager::GetInstance().getSettingGetter<DirectX::XMUINT2>("renderResolution")();
                TextureDescription desc;
                desc.channels = 2;
                desc.format = rhi::Format::R32G32_UInt;
                desc.hasRTV = desc.hasSRV = desc.hasUAV = desc.hasNonShaderVisibleUAV = true;
                desc.allowAlias = true;
                desc.imageDimensions.emplace_back(resolution.x, resolution.y, 0, 0);
                auto visibilityBuffer = PixelBuffer::CreateSharedUnmaterialized(desc);
                visibilityBuffer->SetName("Visibility Buffer");
                org::memory::SetResourceUsageHint(*visibilityBuffer, "Canonical surface visibility");
                newGraph->RegisterResource(Builtin::PrimaryCamera::VisibilityTexture, visibilityBuffer);
                m_pViewManager->AttachVisibilityBuffer(primaryViewID, visibilityBuffer);
                CreateCanonicalSurfaceResources(newGraph.get());
                CreateDebugVisualizationResources(newGraph.get());
                if (m_visibilityRendering) {
                    newGraph->BuildPass<ClearVisibilityBufferPass>("ClearVisibilityBufferPass");
                    newGraph->SetPassTechnique("ClearVisibilityBufferPass", "Primary Visibility::Canonical Surface Construction");
                }
                break;
            }
            case VisibilityMaterialBinning:
                RegisterVisUtilResources(newGraph.get(), false);
                BuildVisibilityMaterialBinningPipeline(newGraph.get(), m_materialEvaluationInputs);
                break;
            case TerrainRvt:
                RegisterVisUtilResources(
                    newGraph.get(), true, false,
                    &m_producerPersistentState->terrainRvtResources);
                BuildTerrainRvtPipeline(newGraph.get());
                break;
            case TerrainRegionMaterialEvaluation:
                BuildTerrainRegionMaterialEvaluationPipeline(newGraph.get(), m_materialEvaluationInputs);
                break;
            case MaterialEvaluation:
                BuildMaterialEvaluationPipeline(newGraph.get(), m_materialEvaluationInputs, terrainRvtEnabled);
                break;
            case Gtao:
                RegisterGTAOResources(newGraph.get());
                BuildGTAOPipeline(newGraph.get(), primaryCameraEntity.try_get<Components::Camera>());
                break;
            case ClusteredLighting:
                BuildLightClusteringPipeline(newGraph.get());
                break;
            case PrimaryLighting:
                BuildPrimaryPass(newGraph.get(), m_currentEnvironment.get(),
                    m_pipelineRecipe.Bindings().Contains(Builtin::Environment::CurrentCubemap));
                break;
            case Reflections: {
                const bool rayTraced = m_rayTracedReflections && DeviceManager::GetInstance().GetCLodRayTracingSupported();
                if (rayTraced) BuildRayTracedReflectionPasses(newGraph.get());
                else if (m_screenSpaceReflections) BuildSSRPasses(newGraph.get());
                break;
            }
            case Exposure: {
                auto adapted = CreateIndexedStructuredBuffer(1, sizeof(float), true, false);
                adapted->SetName("Adapted Luminance");
                org::memory::SetResourceUsageHint(*adapted, "Post-Processing resources");
                newGraph->RegisterResource(Builtin::PostProcessing::AdaptedLuminance, adapted);
                auto histogram = CreateIndexedStructuredBuffer(256, sizeof(uint32_t), true, false);
                histogram->SetName("Luminance Histogram Buffer");
                org::memory::SetResourceUsageHint(*histogram, "Post-Processing resources");
                newGraph->RegisterResource(Builtin::PostProcessing::LuminanceHistogram, histogram);
				auto& histogramBuilder = newGraph->BuildPass<LuminanceHistogramPass>("luminanceHistogramPass");
#if BASICRENDERER_HAS_INTEROP_VALIDATION
				br::validation::SARPInteropValidation::ApplyPassPolicy(
					"luminanceHistogramPass", histogramBuilder, DeviceManager::GetInstance().GetPeerBackend());
#endif
                newGraph->SetPassTechnique("luminanceHistogramPass", "Post Process::Exposure");
                newGraph->BuildPass<LuminanceHistogramAveragePass>("LuminanceAveragePass");
                newGraph->SetPassTechnique("LuminanceAveragePass", "Post Process::Exposure");
                break;
            }
            case Upscaling:
                if (UpscalingManager::GetInstance().GetCurrentUpscalingMode() == UpscalingMode::DLSS &&
                    SettingsManager::GetInstance().getSettingGetter<bool>("enableDilatedMotionVectors")()) {
                    newGraph->BuildPass<DilateMotionVectorsPass>("DilateMotionVectorsPass");
                    newGraph->SetPassTechnique("DilateMotionVectorsPass", "Post Process::Upscaling");
                }
                newGraph->BuildPass<UpscalingPass>("UpscalingPass",
                    br::render::UpscalingGenerationService::CaptureCurrent());
                newGraph->SetPassTechnique("UpscalingPass", "Post Process::Upscaling");
                break;
            case Bloom:
				BuildBloomPipeline(newGraph.get());
                break;
            case Tonemapping:
                newGraph->BuildPass<TonemappingPass>(
                    "TonemappingPass",
                    m_pipelineRecipe.Contains<br::pipeline::BloomTechnique>());
                newGraph->SetPassTechnique("TonemappingPass", "Post Process::Tonemapping");
                break;
            case DebugOutput: {
                auto debugSnapshots = br::render::DebugSceneSnapshotService::Create(
                    RendererECSManager::GetInstance().GetWorld());
                const bool skeletons = SettingsManager::GetInstance().getSettingGetter<unsigned int>("outputType")() ==
                    static_cast<unsigned int>(OutputType::SKELETONS) && DeviceManager::GetInstance().GetMeshShadersSupported();
                if (skeletons) {
                    newGraph->BuildPass<DebugSkeletonPass>("DebugSkeletonPass", debugSnapshots);
                    newGraph->SetPassTechnique("DebugSkeletonPass", "Debug::Visualization");
                }
                else {
                    newGraph->BuildPass<DebugResolvePass>("DebugResolvePass");
                    newGraph->SetPassTechnique("DebugResolvePass", "Debug::Visualization");
                }
                if (getDrawBoundingSpheres()) {
                    newGraph->BuildPass<DebugSpherePass>("DebugSpherePass", debugSnapshots);
                    newGraph->SetPassTechnique("DebugSpherePass", "Debug::Visualization");
                }
                break;
            }
            case DebugUi:
                newGraph->BuildPass<MenuRenderPass>("MenuRenderPass");
                newGraph->SetPassTechnique("MenuRenderPass", "Debug::UI");
                break;
            case DepthHistory:
                BuildLinearDepthHistoryCopyPass(newGraph.get(), &m_depthHistory);
                break;
            case Present:
                newGraph->BuildPass<PresentationReadyPass>("PresentationReadyPass");
                newGraph->SetPassTechnique("PresentationReadyPass", "Frame::Present");
                break;
            }
        });

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::BuildTechniques");
	for (const auto& entry : m_pipelineRecipe.Techniques()) {
		entry.technique->Build(buildContext);
		probeGraphBuildPhase(("CreateRenderGraph after technique " + std::to_string(static_cast<uint32_t>(entry.id))).c_str());
	}
    }

    probeGraphBuildPhase("CreateRenderGraph before CompileStructural");

    newGraph->SetStructuralMaterializeCheckpointCallback([](std::string_view passName) {
        if (!passName.starts_with("CLodShadow::")) {
            return;
        }

        std::string phase = "CreateRenderGraph after structural pass ";
        phase += passName;
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    });

    newGraph->SetStructuralMaterializeResourceCheckpointCallback([](std::string_view passName, std::string_view resourceName) {
        if (passName != "CLodShadow::VirtualShadowSetupPass") {
            return;
        }

        std::string phase = "CreateRenderGraph after structural resource ";
        phase += passName;
        phase += " :: ";
        phase += resourceName;
        ProbeGraphicsCommandListCreation(DeviceManager::GetInstance().GetDevice(), phase);
    });

    //newGraph->SetMinimumAutomaticSchedulingQueues(QueueKind::Compute, 3);

    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::CompileStructural");
	spdlog::info("Renderer::CreateRenderGraph entering CompileStructural");
    newGraph->CompileStructural();
	spdlog::info("Renderer::CreateRenderGraph leaving CompileStructural");
    }
    probeGraphBuildPhase("CreateRenderGraph after CompileStructural");
    {
    BT_ZONE_SCOPE("Renderer::CreateRenderGraph::Setup");
	spdlog::info("Renderer::CreateRenderGraph entering Setup");
    newGraph->Setup();
	spdlog::info("Renderer::CreateRenderGraph leaving Setup");
    }
    probeGraphBuildPhase("CreateRenderGraph after Setup");

	rebuildRenderGraph = false;
}

void Renderer::SetEnvironmentInternal(std::wstring name) {

    std::filesystem::path envpath = std::filesystem::path(GetExePath()) / L"textures" / L"environment" / (name+L".hdr");

    if (std::filesystem::exists(envpath)) {
		m_warnedUsingFallbackEnvironment = false;
		m_preFrameDeferredFunctions.defer([envpath, name, this]() { // Don't change this during rendering
            m_currentEnvironment = m_pEnvironmentManager->CreateEnvironment(name);
            m_pEnvironmentManager->SetFromHDRI(m_currentEnvironment.get(), envpath.string());
			::ResourceManager::GetInstance().SetActiveEnvironmentIndex(m_currentEnvironment->GetEnvironmentIndex());
			});
    }
    else {
        m_currentEnvironment.reset();
        ::ResourceManager::GetInstance().SetActiveEnvironmentIndex(0);
        rebuildRenderGraph = true;
        if (!m_warnedUsingFallbackEnvironment) {
            spdlog::warn("Environment file not found: {}. Falling back to blank environment resources.", envpath.string());
            m_warnedUsingFallbackEnvironment = true;
        }
    }
}
