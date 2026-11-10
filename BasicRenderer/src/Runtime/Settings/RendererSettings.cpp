#include <BasicRenderer/Renderer.h>
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include "Render/RenderGraph/Aliasing/RenderGraphAliasingSubsystem.h"
#include <rhi_debug.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Resources/ResourceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "PostProcessing/FidelityFX/FFXManager.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "BasicRenderer/Diagnostics/TerrainRvtTelemetry.h"
#include "PostProcessing/ToneMapping/TonemapTypes.h"
#include "Utilities/Utilities.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include <nlohmann/json.hpp>

namespace {
constexpr const char* CLodVisibilityTelemetryDebugSettingName = "clodVisibilityTelemetryDebug";
constexpr const char* CLodVirtualShadowTelemetryDebugSettingName = "clodVirtualShadowTelemetryDebug";
constexpr const char* ObjectReyesAtlasTelemetryDebugSettingName = "objectReyesAtlasTelemetryDebug";

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
}

void Renderer::SelectPathTracingUpscaler(bool pathTracing) {
    auto& settings = SettingsManager::GetInstance();
    const UpscalingMode mode = settings.getSettingGetter<UpscalingMode>("upscalingMode")();
    // One sample per pixel needs denoising, unless the mode is averaging samples itself. Without RR support it falls back to SR.
    const bool rayReconstruction = pathTracing && !settings.getSettingGetter<bool>("pathTracingAccumulate")()
        && !ReadTruthyEnvironmentFlag("SARP_RT_PT_NO_RR");
    if (rayReconstruction) {
        if (mode != UpscalingMode::DLSSRayReconstruction) {
            m_upscalingModeBeforePathTracing = mode;
            settings.getSettingSetter<UpscalingMode>("upscalingMode")(UpscalingMode::DLSSRayReconstruction);
        }
    }
    else if (mode == UpscalingMode::DLSSRayReconstruction && m_upscalingModeBeforePathTracing) {
        settings.getSettingSetter<UpscalingMode>("upscalingMode")(*m_upscalingModeBeforePathTracing);
        m_upscalingModeBeforePathTracing.reset();
    }
}

std::string Renderer::DescribeUpscaler() const {
    const auto status = UpscalingManager::GetInstance().GetStatus();
    const auto name = [](UpscalingMode mode) { return std::string(UpscalingModeNames[static_cast<int>(mode)]); };
    auto& settings = SettingsManager::GetInstance();
    const auto renderResolution = settings.getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    nlohmann::json reply = {
        { "setting", name(settings.getSettingGetter<UpscalingMode>("upscalingMode")()) },
        { "requested", name(status.requested) },
        { "current", name(status.current) },
        { "effective", name(status.effective) },
        { "graph", name(status.graph) },
        { "evaluated", name(status.evaluated) },
        { "ray_reconstruction", status.evaluated == UpscalingMode::DLSSRayReconstruction },
        { "dlss_supported", status.dlssSupported },
        { "ray_reconstruction_supported", status.rayReconstructionSupported },
        { "ray_reconstruction_guides_available", status.rayReconstructionGuidesAvailable },
        { "mode_before_path_tracing", m_upscalingModeBeforePathTracing ? name(*m_upscalingModeBeforePathTracing) : std::string() },
        { "path_tracing", m_pathTracing },
        { "accumulate", settings.getSettingGetter<bool>("pathTracingAccumulate")() },
        { "render_resolution", { renderResolution.x, renderResolution.y } },
        { "quality", static_cast<int>(settings.getSettingGetter<UpscaleQualityMode>("upscalingQualityMode")()) },
        { "evaluated_frame", status.evaluatedFrame },
        { "evaluations", status.evaluations },
        { "failed_evaluations", status.failedEvaluations },
        { "out_of_memory_skips", status.outOfMemorySkips },
        { "last_failed_frame", status.lastFailedFrame },
        { "consecutive_failures", status.consecutiveFailures },
        { "fallback", {
            { "input_copies", status.fallbackCopies },
            { "dlss_sr", status.fallbackSuperResolution },
            { "cleared", status.fallbackClears } } },
        { "ray_reconstruction_settings_failed", status.rayReconstructionSettingsFailed },
        { "ray_reconstruction_settings_failures", status.rayReconstructionSettingsFailures },
        { "ray_reconstruction_retry_seconds", status.rayReconstructionRetrySeconds },
    };
    return reply.dump();
}

void Renderer::SimulateUpscalerFailures(uint32_t outOfMemoryFrames, uint32_t settingsFailures) {
    UpscalingManager::GetInstance().SimulateFailures(outOfMemoryFrames, settingsFailures);
}

void Renderer::ReconfigureUpscalerIfRequested() {
    if (!UpscalingManager::GetInstance().TakeReconfigureRequest()) {
        return;
    }
    // As a change of upscaling mode does it.
    m_preFrameDeferredFunctions.defer([this]() {
        StallPipeline();
        UpscalingManager::GetInstance().Shutdown();
        UpscalingManager::GetInstance().InitFFX();
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
	settingsManager.registerSetting<uint32_t>(
		AlphaTestedMaterialTextureMinResidentDimensionSettingName,
		ReadUintEnvironmentValue(
			"SARP_ALPHA_TESTED_TEXTURE_MIN_RESIDENT_DIMENSION",
			AlphaTestedMaterialTextureMinResidentDimensionDefault));
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
    // TerrainRvt::RefreshStalePagesSettingName: re-render RVT pages once a source texture they were short of improves.
    // SARP_TERRAIN_RVT_REFRESH=0 starts with it off (the menu switches it at runtime).
    settingsManager.registerSetting<bool>("terrainRvtRefreshStalePages", [] {
        const char* value = std::getenv("SARP_TERRAIN_RVT_REFRESH");
        return !(value && value[0] == '0');
    }());
    // TerrainRvt::CompressionSettingName: block-compressed RVT atlases. SARP_TERRAIN_RVT_COMPRESSION=0 starts with it off.
    settingsManager.registerSetting<bool>("terrainRvtCompression", [] {
        const char* value = std::getenv("SARP_TERRAIN_RVT_COMPRESSION");
        return !(value && value[0] == '0');
    }());
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
    settingsManager.registerSetting<uint32_t>(CLodSoftwareRasterDiameterThresholdSettingName, 16u);
    settingsManager.registerSetting<uint32_t>(CLodVirtualShadowSoftwareRasterDiameterThresholdSettingName, 32u);
    settingsManager.registerSetting<uint32_t>(CLodForceTraversalDepthRootSettingName, CLodForceTraversalDepthRootDisabled);
    settingsManager.registerSetting<uint32_t>(CLodVisibleClusterCapacitySettingName, CLodDefaultVisibleClusterCapacity);
    settingsManager.registerSetting<bool>(CLodFrustumCullingSettingName, true);
    settingsManager.registerSetting<float>(CLodLodErrorPixelsSettingName, 1.0f);
    settingsManager.registerSetting<uint32_t>(
        CLodPureComputePhase2ExpansionFactorSettingName,
        CLodPureComputePhase2ExpansionFactorDefault);
    settingsManager.registerSetting<uint32_t>(
        CLodPureComputeReplayExpansionFactorSettingName,
        CLodPureComputeReplayExpansionFactorDefault);
    settingsManager.registerSetting<bool>("enableBloom", m_bloom);
    settingsManager.registerSetting<bool>("enableJitter", m_jitter);
    settingsManager.registerSetting<bool>("rayReconstructionGuidesAvailable", false);
    // The host's model loader; the standalone demo installs one that streams the model's statics.
    settingsManager.registerSetting<std::function<void(const std::string&)>>("loadModel", [](const std::string& path) {
        spdlog::warn("Load Model: no model loader is installed; '{}' was not loaded", path);
        });
	settingsManager.registerSetting<bool>("enableScreenSpaceReflections", m_screenSpaceReflections);
    // For headless runs: these choose what the first render graph is built with.
    if (ReadTruthyEnvironmentFlag("SARP_RT_REFLECTIONS")) m_rayTracedReflections = true;
    if (ReadTruthyEnvironmentFlag("SARP_RT_SHADOWS")) m_rayTracedShadows = true;
    if (ReadTruthyEnvironmentFlag("SARP_RT_PRIMARY")) m_rayTracedPrimaryVisibilityMode = 1u;
    if (ReadTruthyEnvironmentFlag("SARP_RT_PRIMARY_COMPARE")) m_rayTracedPrimaryVisibilityMode = 2u;
    // Path tracing is the default wherever the device can run it. SARP_RT_PATH_TRACING=0 starts on raster, and =1 asks for path
    // tracing (it still falls back to raster, with a warning, where it can't run).
    const auto& deviceManager = DeviceManager::GetInstance();
    if (std::getenv("SARP_RT_PATH_TRACING") != nullptr) {
        m_pathTracing = ReadTruthyEnvironmentFlag("SARP_RT_PATH_TRACING");
        if (m_pathTracing && !deviceManager.GetPathTracingSupported()) {
            spdlog::warn("Path tracing {}; rendering with raster.", deviceManager.GetPathTracingUnsupportedReason());
        }
    }
    else {
        m_pathTracing = true;
    }
    m_pathTracing = m_pathTracing && deviceManager.GetPathTracingSupported();
    if (m_pathTracing) {
        m_rasterRecipeBeforePathTracing = m_pipelineRecipe;
        m_pipelineRecipe = br::pipeline::MakePathTracedRecipe(m_pipelineRecipe);
    }
    settingsManager.registerSetting<bool>("enablePathTracing", m_pathTracing);
    const auto pathTracingEnvironmentNumber = [](const char* name, uint32_t fallback) {
        const char* value = std::getenv(name);
        return value ? static_cast<uint32_t>(std::atoi(value)) : fallback;
    };
    // Primary hit plus one bounce, as RTXMG defaults to.
    settingsManager.registerSetting<uint32_t>("pathTracingMaxBounces", pathTracingEnvironmentNumber("SARP_RT_PT_BOUNCES", 2u));
    // 0 never, 1 secondary rays, 2 every ray.
    settingsManager.registerSetting<uint32_t>("pathTracingReorderMode", pathTracingEnvironmentNumber("SARP_RT_PT_REORDER", 2u));
    // Averages samples while the camera is still: the reference image, with nothing biased left on.
    settingsManager.registerSetting<bool>("pathTracingAccumulate", ReadTruthyEnvironmentFlag("SARP_RT_PT_ACCUMULATE"));
    // A hash of whatever else shapes primary rays (a physical lens's file, focus and wavelength mode). Accumulation restarts
    // when it changes, as it does for the camera's view and projection. 0 is the pinhole camera.
    settingsManager.registerSetting<uint64_t>("pathTracingCameraModelHash", 0ull);
    // The physical camera lens (LensManager, SARP's docs/lens_simulation.md). Mode 0 pinhole, 1 explicit (traced surface by surface),
    // 2 polynomial (the .brlens beside the prescription, of lensPolynomialDegree 5 or 7). The file is a name in lenses/ without
    // .lens.json. The f-number stops the lens down from wide open (0, or anything below the lens's own, is wide open); it rebuilds
    // the lens. Focus is per frame and never rebuilds: lensFocusMode 0 manual focuses at lensFocusDistance (metres from the front
    // vertex, which sits at the camera), 1 autofocus measures the primary hits' depth over lensAutofocusRegion (a fraction of the
    // frame height) around lensAutofocusPoint (NDC, +y up; the centre by default) and follows it at lensAutofocusSpeed (the
    // critically damped spring's rate, 1/s). Manual by default, so reference captures keep a fixed focus.
    const auto lensEnvironmentFloat = [](const char* name, float fallback) {
        const char* value = std::getenv(name);
        return value ? static_cast<float>(std::atof(value)) : fallback;
    };
    const char* lensFile = std::getenv("SARP_LENS_FILE");
    settingsManager.registerSetting<uint32_t>("lensMode", pathTracingEnvironmentNumber("SARP_LENS_MODE", 0u));
    settingsManager.registerSetting<std::string>("lensFile", lensFile ? std::string(lensFile) : std::string("double_gauss_50mm"));
    settingsManager.registerSetting<float>("lensFocusDistance", lensEnvironmentFloat("SARP_LENS_FOCUS_M", 3.0f));
    settingsManager.registerSetting<uint32_t>("lensFocusMode", pathTracingEnvironmentNumber("SARP_LENS_FOCUS_MODE", 0u));
    settingsManager.registerSetting<DirectX::XMFLOAT2>("lensAutofocusPoint", DirectX::XMFLOAT2(0.0f, 0.0f));
    settingsManager.registerSetting<float>("lensAutofocusRegion", lensEnvironmentFloat("SARP_LENS_AF_REGION", 0.02f));
    settingsManager.registerSetting<float>("lensAutofocusSpeed", lensEnvironmentFloat("SARP_LENS_AF_SPEED", 8.0f));
    settingsManager.registerSetting<bool>("lensMonochromatic", !ReadTruthyEnvironmentFlag("SARP_LENS_SPECTRAL"));
    settingsManager.registerSetting<float>("lensFNumber", lensEnvironmentFloat("SARP_LENS_F_NUMBER", 0.0f));
    settingsManager.registerSetting<uint32_t>("lensPolynomialDegree", pathTracingEnvironmentNumber("SARP_LENS_POLYNOMIAL_DEGREE", 5u));
    // 0 remap: a camera field wider than the lens covers is fitted to its covered field (the frame's corners at the covered
    // image radius, exit rays widened to the camera's field), so the whole frame is lit. 1 physical: the sensor is sized by
    // the camera's vertical field through the lens, and the frame vignettes to black past what the lens covers.
    settingsManager.registerSetting<uint32_t>("lensFieldMode", pathTracingEnvironmentNumber("SARP_LENS_FIELD_MODE", 0u));
    // The lens's ghost flares (GhostFlareManager, SARP's docs/lens_simulation.md "Ghost flares"), whenever a lens is on: the
    // brightest two-reflection paths (max ghosts), a lattice of grid x grid cells per source, ghost and wavelength
    // (wavelengths: spectral lenses only), the sun and up to max lights local lights. Intensity 1 is physical. All but the
    // intensity rebuild the graph (they size its buffers); off, the flare passes aren't in it.
    settingsManager.registerSetting<bool>("lensFlareEnabled", pathTracingEnvironmentNumber("SARP_LENS_FLARE", 1u) != 0u);
    settingsManager.registerSetting<uint32_t>("lensFlareMaxGhosts", pathTracingEnvironmentNumber("SARP_LENS_FLARE_GHOSTS", 8u));
    settingsManager.registerSetting<uint32_t>("lensFlareGridResolution", pathTracingEnvironmentNumber("SARP_LENS_FLARE_GRID", 64u));
    settingsManager.registerSetting<uint32_t>("lensFlareWavelengths", pathTracingEnvironmentNumber("SARP_LENS_FLARE_WAVELENGTHS", 3u));
    settingsManager.registerSetting<uint32_t>("lensFlareMaxLights", pathTracingEnvironmentNumber("SARP_LENS_FLARE_LIGHTS", 3u));
    settingsManager.registerSetting<float>("lensFlareIntensity", lensEnvironmentFloat("SARP_LENS_FLARE_INTENSITY", 1.0f));
    settingsManager.registerSetting<bool>("pathTracingSunOnly", ReadTruthyEnvironmentFlag("SARP_RT_PT_SUN_ONLY"));
    // Opacity micromaps for alpha-tested foliage, as the preprocessor baked them (its --opacity-micromap-level sets their
    // subdivision), read when the BVH is first created. SARP_RT_OMM=0 turns them off.
    settingsManager.registerSetting<bool>("pathTracingOpacityMicromaps", pathTracingEnvironmentNumber("SARP_RT_OMM", 1u) != 0u);
    // 0 exact, 1 bounce and shadow rays skip any-hit on unknown micro-triangles and take the bake's guess, 2 every ray does.
    // 1 by default: 14.5 ms against 22.7 exact on SaveLab, with no visible difference in an accumulated image.
    settingsManager.registerSetting<uint32_t>("pathTracingOpacityMicromapTwoState", pathTracingEnvironmentNumber("SARP_RT_OMM_2STATE", 1u));
    if (m_pathTracing) {
        // Directly as well: the setting's observer isn't registered until later in this function.
        settingsManager.getSettingSetter<bool>("rayReconstructionGuidesAvailable")(true);
        UpscalingManager::GetInstance().SetRayReconstructionGuidesAvailable(true);
        SelectPathTracingUpscaler(true);
    }
    settingsManager.registerSetting<bool>("enableRayTracedReflections", m_rayTracedReflections);
    // 0 raster, 1 raytraced where the BVH can represent the draw, 2 raster with per-pixel agreement counted against rays.
    settingsManager.registerSetting<uint32_t>("rayTracedPrimaryVisibilityMode", m_rayTracedPrimaryVisibilityMode);
    settingsManager.registerSetting<bool>("enableRayTracedShadows", m_rayTracedShadows);
    settingsManager.registerSetting<float>("rayTracedReflectionMaxDistance", 16384.0f); // World units: four exterior cells.
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
	settingsManager.registerSetting<org::AutoAliasMode>("autoAliasMode", org::AutoAliasMode::Balanced);
    settingsManager.registerSetting<org::AutoAliasPackingStrategy>("autoAliasPackingStrategy", org::AutoAliasPackingStrategy::GreedySweepLine);
    settingsManager.registerSetting<bool>("autoAliasEnableLogging", ReadTruthyEnvironmentFlag("BASICRENDERER_AUTO_ALIAS_LOG"));
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
    settingsManager.registerSetting<float>(CLodDirectionalVirtualShadowSmrtMaxLateralTexelsSettingName, CLodVirtualShadowDefaultSmrtMaxLateralTexels);
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
    // The atlases change format: the pages they hold go with them.
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("terrainRvtCompression", [this](const bool& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        rebuildRenderGraph = true;
        }));
    // Sizes the compression scratch.
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtMaxGeneratedPagesPerFrame", [this](const uint32_t& newValue) {
        (void)newValue;
        rebuildRenderGraph = true;
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
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("rayReconstructionGuidesAvailable", [this](const bool& newValue) {
        // Applied now rather than deferred: the graph rebuild of the pipeline
        // replacement announcing this runs before the next deferred flush, and
        // must not declare guides the outgoing pipeline wrote.
        UpscalingManager::GetInstance().SetRayReconstructionGuidesAvailable(newValue);
        if (SettingsManager::GetInstance().getSettingGetter<UpscalingMode>("upscalingMode")() != UpscalingMode::DLSSRayReconstruction) {
            return;
        }
        m_preFrameDeferredFunctions.defer([this]() {
            StallPipeline();
            UpscalingManager::GetInstance().Shutdown();
            UpscalingManager::GetInstance().InitFFX();
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
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enablePathTracing", [this](const bool& newValue) {
        if (newValue == m_pathTracing) {
            return;
        }
        if (newValue && !DeviceManager::GetInstance().GetPathTracingSupported()) {
            spdlog::warn("Path tracing {}; staying on raster.", DeviceManager::GetInstance().GetPathTracingUnsupportedReason());
            // Put the setting back, so whatever shows it (the menu) shows raster.
            m_preFrameDeferredFunctions.defer([]() { SettingsManager::GetInstance().getSettingSetter<bool>("enablePathTracing")(false); });
            return;
        }
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) {
            m_rasterRecipeBeforePathTracing = recipe;
            recipe = br::pipeline::MakePathTracedRecipe(recipe);
        }
        else if (m_rasterRecipeBeforePathTracing) {
            // Kept until raster is in place (ApplyPendingPipelineReplacement), in case the switch fails and rolls back.
            recipe = *m_rasterRecipeBeforePathTracing;
        }
        if (RequestPipelineReplacement(std::move(recipe))) {
            m_pathTracing = newValue;
            SelectPathTracingUpscaler(newValue);
            UpscalingManager::GetInstance().RequestHistoryReset();
        }
        }));
    // Accumulating averages samples itself, so it doesn't want Ray Reconstruction's denoising. The path trace pass makes its
    // accumulation buffer when the graph is built.
    // The ghost flares' passes and buffer sizes (LensFlarePasses.h) are fixed when the graph is built.
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("lensFlareEnabled", [this](const bool&) { rebuildRenderGraph = true; }));
    for (const char* name : { "lensFlareMaxGhosts", "lensFlareGridResolution", "lensFlareWavelengths", "lensFlareMaxLights" }) {
        m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(name, [this](const uint32_t&) { rebuildRenderGraph = true; }));
    }
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("pathTracingAccumulate", [this](const bool&) {
        if (m_pathTracing) {
            SelectPathTracingUpscaler(true);
            rebuildRenderGraph = true;
        }
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("rayTracedPrimaryVisibilityMode", [this](const uint32_t& newValue) {
        m_rayTracedPrimaryVisibilityMode = newValue;
        rebuildRenderGraph = true;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableRayTracedShadows", [this](const bool& newValue) {
        m_rayTracedShadows = newValue;
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
