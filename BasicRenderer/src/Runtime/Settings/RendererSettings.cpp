#include <BasicRenderer/Renderer.h>
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
	settingsManager.registerSetting<org::AutoAliasMode>("autoAliasMode", org::AutoAliasMode::Balanced);
    settingsManager.registerSetting<org::AutoAliasPackingStrategy>("autoAliasPackingStrategy", org::AutoAliasPackingStrategy::GreedySweepLine);
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
