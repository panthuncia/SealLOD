#include <BasicRenderer/Renderer.h>
#include "OpenRenderGraph/GraphHost.h"
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include "Render/RenderGraph/Aliasing/RenderGraphAliasingSubsystem.h"
#include <rhi_debug.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Resources/ResourceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Pipeline/PipelineState/PSOManager.h"
#include "Pipeline/PipelineState/PipelineLibrary.h"
#include "PostProcessing/FidelityFX/FFXManager.h"
#include "PostProcessing/Upscaling/UpscalingManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include <BasicRenderer/Pipeline/RendererSettings.h>
#include "BasicRenderer/Diagnostics/TerrainRvtTelemetry.h"
#include "PostProcessing/ToneMapping/TonemapTypes.h"
#include "Utilities/Utilities.h"
#include "Runtime/Settings/RendererSettingsHelpers.h"
#include "Runtime/GraphIntegration/FrameBuild.h"
#include "PostProcessing/AmbientOcclusion/AmbientOcclusionSettings.h"
#include "../shaders/PerPassRootConstants/materialTextureResolveConstants.h"
#include "../shaders/PerPassRootConstants/lightCullingRootConstants.h"
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
    // Upscaler configurations (M5 step 3): the attached graphs' and the latest prepared one.
    const auto configuration = [&](const UpscalingManager::Status::Configuration& c) {
        return nlohmann::json{ { "serial", c.serial }, { "viewport", c.viewport }, { "mode", name(c.mode) },
            { "render_resolution", { c.render.x, c.render.y } }, { "output_resolution", { c.output.x, c.output.y } },
            { "prepared", c.prepared }, { "allocated_ahead", c.allocatedAhead }, { "prepare_ms", c.prepareMs } };
    };
    reply["configuration"] = {
        { "current", configuration(status.currentConfiguration) },
        { "latest", configuration(status.latestConfiguration) },
        { "prepared", status.configurationsPrepared },
        { "retired", status.configurationsRetired },
        { "live", status.configurationsLive },
        { "last_attach_wait_ms", status.lastAttachWaitMs } };
    return reply.dump();
}

void Renderer::SimulateUpscalerFailures(uint32_t outOfMemoryFrames, uint32_t settingsFailures) {
    UpscalingManager::GetInstance().SimulateFailures(outOfMemoryFrames, settingsFailures);
}

std::string Renderer::DescribeGraphRevisions() const {
    const auto pending = m_frameRevisions.Peek();
    nlohmann::json reply = {
        { "builds", m_graphBuilds },
        { "last_build_frame", m_lastGraphBuildFrame },
        { "last_build_ms", m_lastGraphBuildMs },
        { "last_build_reasons", m_lastGraphBuildReasons },
        { "pending", pending.Describe() },
        { "pending_topology_epochs", pending.topologyEpochs },
        { "topology_requests", m_frameRevisions.TopologyRequests() },
        { "frames", m_totalFramesRendered },
    };
    // M4 step 5: the frame's host epochs, each a graph of its own, and how many graphs each has had attached. A build answers
    // the requests for the epochs it names (last_build_host_epochs); "builds" counts builds, not graphs.
    nlohmann::json epochBuilds = nlohmann::json::object();
    for (const auto& [host, count] : m_epochGraphBuilds) epochBuilds[std::to_string(host)] = count;
    reply["epoch_builds"] = std::move(epochBuilds);
    reply["host_epochs"] = FrameGraphHostEpochs();
    reply["last_build_host_epochs"] = m_lastGraphBuildHostEpochs;
    // M6 step 3: the kept epochs' runs since the start (GraphHost::SetKeptEpochs): submitted their copy's kept recording, recorded
    // and kept it, or recorded one that cannot be kept, with why each recorded.
    {
        nlohmann::json recordings = nlohmann::json::object();
        for (const auto& [host, kept] : m_keptRecordingTotals) {
            nlohmann::json reasons = nlohmann::json::object();
            for (const auto& [reason, count] : kept.reasons) reasons[reason] = count;
            nlohmann::json mismatches = nlohmann::json::object();
            for (const auto& [name, count] : kept.mismatches) mismatches[name] = count;
            nlohmann::json batchReasons = nlohmann::json::object();
            for (const auto& [reason, count] : kept.batchReasons) batchReasons[reason] = count;
            recordings[std::to_string(host)] = { { "runs", kept.runs }, { "reused", kept.reused }, { "recorded", kept.kept },
                { "unkeepable", kept.unkeepable }, { "reasons", std::move(reasons) },
                // M6 step 6: staged recordings at their first use.
                { "staged", { { "reused", kept.stagedReused }, { "recorded", kept.stagedRecorded } } },
                // M6 step 7: batch reuse (runs partly reused, batches reused and recorded, why batches recorded).
                { "batches", { { "partial", kept.partial }, { "reused", kept.batchesReused }, { "recorded", kept.batchesRecorded },
                    { "reasons", std::move(batchReasons) } } },
                // M6 step 4: the revision audit (BASICRENDERER_KEPT_RECORDING_AUDIT).
                { "audit", { { "audited", kept.audited }, { "compared", kept.auditCompared }, { "uncomparable", kept.auditUncomparable },
                    { "mismatches", kept.auditMismatches }, { "passes", std::move(mismatches) } } } };
        }
        reply["recordings"] = std::move(recordings);
        reply["kept_epochs"] = m_frameHost ? m_frameHost->KeptEpochs() : std::vector<uint32_t>{};
    }
    // M4 step 7: frames checked for a mixed set of builds (CheckFrameBuildSet), and the host's own count of staged sets.
    reply["frame_sets"] = {
        { "frames_checked", m_frameSetCheck.totalFrames },
        { "mixed_frames", m_frameSetCheck.totalMixed },
        { "unknown_frames", m_frameSetCheck.totalUnknown },
        { "host_sets_attached", m_frameHost ? m_frameHost->AttachStatsTotal().setsAttached : 0 },
        { "host_staged", m_frameHost ? m_frameHost->AttachStatsTotal().staged : 0 },
    };
    // M5 step 4: the resolution revision. Requests (window sizes, upscaler settings) recorded, the consolidation points that took
    // them, and the builds they started; at most one of those is in flight.
    {
        const auto& resolution = m_resolutionStats;
        const auto waiting = m_frameRevisions.PeekResolution();
        const auto attached = AttachedResolution();
        reply["resolution"] = {
            { "requests", m_frameRevisions.ResolutionRequests() },
            { "pending", waiting.Describe() },
            { "pending_requests", waiting.requests },
            { "consumed", resolution.consumed },
            { "applied", resolution.applied },
            { "unchanged", resolution.unchanged },
            { "deferred_frames", resolution.deferredFrames },
            { "settle_frames", resolution.settleFrames },
            { "swapchain_resizes", resolution.swapchainResizes },
            { "swapchain_only_resizes", resolution.swapchainOnlyResizes },
            { "stale_mode_changes", resolution.staleModeChanges },
            { "builds", resolution.resolutionBuilds },
            { "builds_in_flight", resolution.resolutionBuildsInFlight },
            { "max_builds_in_flight", resolution.maxResolutionBuildsInFlight },
            { "last_coalesced", resolution.lastCoalesced },
            { "max_coalesced", resolution.maxCoalesced },
            { "last_frame", resolution.lastFrame },
            { "last_reasons", resolution.lastReasons },
            { "last_apply_ms", resolution.lastApplyMs },
            { "max_apply_ms", resolution.maxApplyMs },
            { "last_prepare_ms", resolution.lastPrepareMs },
            { "max_prepare_ms", resolution.maxPrepareMs },
            { "switches", resolution.switches },
            { "last_switch_ms", resolution.lastSwitchMs },
            { "max_switch_ms", resolution.maxSwitchMs },
            { "last_swapchain_wait_ms", resolution.lastSwapchainWaitMs },
            { "max_swapchain_wait_ms", resolution.maxSwapchainWaitMs },
            { "last_resize_buffers_ms", resolution.lastResizeBuffersMs },
            { "max_resize_buffers_ms", resolution.maxResizeBuffersMs },
            { "stall_pipeline_calls", resolution.stallPipelineCalls },
            { "last_stall_reason", resolution.lastStallReason },
            { "attached_render", { attached.render.x, attached.render.y } },
            { "attached_output", { attached.output.x, attached.output.y } },
        };
    }
    // M3 step 9: builds run on the build thread and are attached at the consolidation point.
    const auto& stats = m_graphBuildStats;
    const bool inFlight = FrameBuildInFlight();
    reply["async"] = {
        { "enabled", m_frameBuildWorker != nullptr },
        { "in_flight", inFlight },
        { "in_flight_frames", inFlight ? m_totalFramesRendered - stats.inFlightSinceFrame : 0 },
        { "started", stats.started },
        { "on_render_thread", stats.onRenderThread },
        { "failed", stats.failed },
        { "discarded", stats.discarded },
        { "superseded", stats.superseded },
        { "staged_sets", stats.stagedSets },
        { "refused_sets", stats.refusedSets },
        { "epoch_failures", stats.epochFailures },
        { "last_prepare_ms", stats.lastPrepareMs },
        { "last_attach_ms", stats.lastAttachMs },
        { "last_pipeline_wait_ms", stats.lastPipelineWaitMs },
        { "last_pipelines_declared", stats.lastPipelinesDeclared },
        { "last_latency_ms", stats.lastLatencyMs },
        { "last_frames_during_build", stats.lastFrames },
        { "max_render_thread_ms", stats.maxRenderThreadMs },
        { "last_error", stats.lastError },
        { "last_epoch_failure", stats.lastEpochFailure },
        // Pipelines a running graph declared outside any build (its recordings may wait for them on the render thread).
        { "pipelines_declared_outside_builds", PSOManager::GetInstance().GetPipelineLibrary()
            ? PSOManager::GetInstance().GetPipelineLibrary()->DeclaredOutsideScopes() : 0 },
    };
    return reply.dump();
}

void Renderer::ReconfigureUpscalerIfRequested() {
    if (!UpscalingManager::GetInstance().TakeReconfigureRequest()) {
        return;
    }
    // As a change of upscaling mode does it. A retry that fails again gives the configuration there is: nothing is rebuilt.
    m_frameRevisions.RequestUpscaler("upscaler reconfiguration");
}

bool Renderer::RayReconstructionGuidesForNextBuild() const {
    // The recipe the next build is made from (a replacement waiting for it included), as SyncPipelineTopologySettings announces it.
    return GetPipelineRecipeForMutation().Contains<br::pipeline::PathTracingTechnique>();
}

namespace {
// Which host epochs a setting's rebuild builds (M4 steps 5 and 6; docs/frame_graph_settings_classification.md and the M4 design's
// section 3). Read when a setting changes, after the host was made (the split is fixed then). Visibility ends after material
// evaluation: CLod, terrain RVT and material settings are Visibility's; AO, lights, stochastic lighting, reflections and CLod
// transparency Shading's. Settings that change what both epochs declare (ray tracing, the canonical surface) are both.
uint32_t VisibilityEpochs() { return br::render::HostEpochMaskOf({ br::render::kVisibilityEpoch }); }
uint32_t ShadingEpochs() { return br::render::HostEpochMaskOf({ br::render::kShadingEpoch }); }
uint32_t VisibilityShadingEpochs() { return br::render::HostEpochMaskOf({ br::render::kVisibilityEpoch, br::render::kShadingEpoch }); }
uint32_t ShadingPostEpochs() { return br::render::HostEpochMaskOf({ br::render::kShadingEpoch, br::render::kPostEpoch }); }
uint32_t PostEpochs() { return br::render::HostEpochMaskOf({ br::render::kPostEpoch }); }
uint32_t OverlayEpochs() { return br::render::HostEpochMaskOf({ br::render::kOverlayEpoch }); }
uint32_t PostOverlayEpochs() { return br::render::HostEpochMaskOf({ br::render::kPostEpoch, br::render::kOverlayEpoch }); }
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
		MaterialTextureStreamingBudgetMBSettingName,
		ReadUintEnvironmentValue("SARP_TEXTURE_STREAMING_BUDGET_MB", 0u));
	settingsManager.registerSetting<uint32_t>(
		MaterialTextureStreamingVisibleOffsetSettingName,
		ReadUintEnvironmentValue("SARP_TEXTURE_STREAMING_VISIBLE_OFFSET", 16u));
	settingsManager.registerSetting<uint32_t>(
		MaterialTextureStreamingRateMBSettingName,
		ReadUintEnvironmentValue("SARP_TEXTURE_STREAMING_RATE_MB", 1024u));
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
    settingsManager.registerSetting<bool>(
        "enableShadows",
        m_pipelineRecipe.Contains<br::pipeline::ClusterLodShadowTechnique>());
    // Written by the renderer at graph build (Renderer::VirtualShadowMapsActive): shadows on and not stochastic lighting.
    settingsManager.registerSetting<bool>(
        "virtualShadowMapsActive",
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
    // The light grid's tile size in pixels and depth slices (docs/lighting.md "The light grid"); either rebuilds the graph.
    {
        const char* tile = std::getenv("SARP_LIGHT_GRID_TILE");
        const char* slices = std::getenv("SARP_LIGHT_GRID_SLICES");
        settingsManager.registerSetting<uint32_t>(LightGridTileSizeSettingName,
            tile ? static_cast<uint32_t>(std::atoi(tile)) : LightGridDefaultTileSize);
        settingsManager.registerSetting<uint32_t>(LightGridSlicesSettingName,
            slices ? static_cast<uint32_t>(std::atoi(slices)) : LightGridDefaultSlices);
    }
    settingsManager.registerSetting<uint32_t>(LightGridIndexCapacitySettingName, LightGridMinIndexCapacity * 4u);
    settingsManager.registerSetting<uint32_t>(LightGridRecordCapacitySettingName, LightGridMinRecordCapacity * 4u);
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
	settingsManager.registerSetting<bool>("enableGTAO", m_gtaoEnabled);
	{
		namespace ao = br::ambient_occlusion;
		settingsManager.registerSetting<uint32_t>(ao::TechniqueSettingName, ao::TechniqueDefault);
		settingsManager.registerSetting<uint32_t>(ao::CortaoSampleCountSettingName, ao::CortaoSampleCountDefault);
		settingsManager.registerSetting<float>(ao::CortaoRadiusSettingName, ao::CortaoRadiusDefault);
		settingsManager.registerSetting<float>(ao::CortaoScreenRadiusRatioSettingName, ao::CortaoScreenRadiusRatioDefault);
		settingsManager.registerSetting<float>(ao::CortaoMipBiasSettingName, ao::CortaoMipBiasDefault);
		settingsManager.registerSetting<float>(ao::CortaoSlopeCompensationSettingName, ao::CortaoSlopeCompensationDefault);
		settingsManager.registerSetting<float>(ao::CortaoBackfaceThresholdSettingName, ao::CortaoBackfaceThresholdDefault);
		settingsManager.registerSetting<float>(ao::CortaoBackfaceBoostSettingName, ao::CortaoBackfaceBoostDefault);
		settingsManager.registerSetting<float>(ao::CortaoFilterDepthSensitivitySettingName, ao::CortaoFilterDepthSensitivityDefault);
		settingsManager.registerSetting<float>(ao::CortaoFilterNormalSensitivitySettingName, ao::CortaoFilterNormalSensitivityDefault);
	}
	settingsManager.registerSetting<bool>("enableOcclusionCulling", m_occlusionCulling);
    settingsManager.registerSetting<CLodCullingBackend>(CLodCullingBackendSettingName, CLodCullingBackend::PureCompute);
    settingsManager.registerSetting<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName, CLodSoftwareRasterMode::Compute);
    settingsManager.registerSetting<CLodVSMRasterMode>(CLodVSMRasterModeSettingName, CLodVSMRasterMode::Standard);
    settingsManager.registerSetting<CLodTransparencyMode>(CLodTransparencyModeSettingName, CLodTransparencyMode::Disabled);
    settingsManager.registerSetting<CLodLodHeightMode>(CLodLodHeightModeSettingName, CLodLodHeightMode::RenderHeight);
    settingsManager.registerSetting<bool>(
        CLodDisableNonVoxelVisibilitySettingName,
        ReadTruthyEnvironmentFlag("SARP_CLOD_DISABLE_NON_VOXEL_VISIBILITY"));
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
    // Error-channel LOD selection (CLodCommon.h). SARP_CLOD_LOD_APPEARANCE_WEIGHT and SARP_CLOD_LOD_GEOMETRIC_CHANNEL
    // (0 quadric, 1 measured, 2 max) set the starting values for headless runs.
    settingsManager.registerSetting<float>(CLodLodAppearanceWeightSettingName, [] {
        const char* value = std::getenv("SARP_CLOD_LOD_APPEARANCE_WEIGHT");
        const float weight = value ? static_cast<float>(std::atof(value)) : 0.0f;
        return std::isfinite(weight) && weight > 0.0f ? weight : 0.0f;
    }());
    settingsManager.registerSetting<uint32_t>(CLodLodGeometricChannelSettingName, [] {
        const char* value = std::getenv("SARP_CLOD_LOD_GEOMETRIC_CHANNEL");
        const int channel = value ? std::atoi(value) : 0;
        return channel >= 0 && channel <= static_cast<int>(CLodLodGeometricChannelMax) ? static_cast<uint32_t>(channel) : 0u;
    }());
    settingsManager.registerSetting<float>(CLodNormalSpreadRoughnessScaleSettingName, [] {
        const char* value = std::getenv("SARP_CLOD_NORMAL_SPREAD_ROUGHNESS");
        const float scale = value ? static_cast<float>(std::atof(value)) : 1.0f;
        return std::isfinite(scale) && scale > 0.0f ? scale : 0.0f;
    }());
    settingsManager.registerSetting<float>(CLodNormalSpreadRoughnessCapSettingName, [] {
        const char* value = std::getenv("SARP_CLOD_NORMAL_SPREAD_ROUGHNESS_CAP");
        const float cap = value ? static_cast<float>(std::atof(value)) : 1.0f;
        return std::isfinite(cap) && cap > 0.0f ? cap : 0.0f;
    }());
    for (const auto& [name, variable] : { std::pair{ CLodDebugMaterialRoughnessSettingName, "SARP_CLOD_DEBUG_MATERIAL_ROUGHNESS" },
             std::pair{ CLodDebugMaterialMetallicSettingName, "SARP_CLOD_DEBUG_MATERIAL_METALLIC" } }) {
        const char* value = std::getenv(variable);
        const float parsed = value ? static_cast<float>(std::atof(value)) : -1.0f;
        settingsManager.registerSetting<float>(name, std::isfinite(parsed) && parsed >= 0.0f ? std::min(parsed, 1.0f) : -1.0f);
    }
    settingsManager.registerSetting<uint32_t>(MaterialResolvedTextureCapacitySettingName, MATERIAL_RESOLVED_TEXTURE_MIN_CAPACITY);
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
    settingsManager.registerSetting<std::function<void(const std::string&)>>("loadScript", {});
	settingsManager.registerSetting<bool>("enableScreenSpaceReflections", m_screenSpaceReflections);
    // For headless runs: these choose what the first render graph is built with.
    if (ReadTruthyEnvironmentFlag("SARP_RT_REFLECTIONS")) m_rayTracedReflections = true;
    if (ReadTruthyEnvironmentFlag("SARP_RT_SHADOWS")) m_rayTracedShadows = true;
    if (ReadTruthyEnvironmentFlag("SARP_RT_PRIMARY")) m_rayTracedPrimaryVisibilityMode = 1u;
    if (ReadTruthyEnvironmentFlag("SARP_RT_PRIMARY_COMPARE")) m_rayTracedPrimaryVisibilityMode = 2u;
    // Stochastic direct lighting (docs/stochastic_lighting.md): spot and point lights sampled and shadowed by ray instead of the
    // virtual shadow map's local faces. Needs the shadow map (for the sun), clustered lighting and cluster ray tracing; the
    // recipe keeps it while path tracing replaces raster, and it takes effect again when raster comes back.
    if (ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_LIGHTING")) {
        m_pipelineRecipe.Add<br::pipeline::StochasticDirectLightingTechnique>();
    }
    settingsManager.registerSetting<bool>("enableStochasticLighting",
        m_pipelineRecipe.Contains<br::pipeline::StochasticDirectLightingTechnique>());
    // The brute-force reference in place of the estimator: every light in the pixel's cluster, one ray each, averaged over frames
    // while nothing changes. Rebuilds the graph.
    settingsManager.registerSetting<bool>("stochasticLightingReference", ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_REFERENCE"));
    // The estimator's running average while nothing changes, for comparing it with the reference. Rebuilds the graph.
    settingsManager.registerSetting<bool>("stochasticLightingAccumulate", ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_ACCUMULATE"));
    // The temporal and spatial denoiser (on unless SARP_STOCHASTIC_DENOISE=0). Rebuilds the graph.
    {
        const char* denoise = std::getenv("SARP_STOCHASTIC_DENOISE");
        settingsManager.registerSetting<bool>("stochasticLightingDenoise", !denoise || ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_DENOISE"));
    }
    // Guiding by last frame's per-tile visible lights (on unless SARP_STOCHASTIC_GUIDING=0). Per frame.
    {
        const char* guiding = std::getenv("SARP_STOCHASTIC_GUIDING");
        settingsManager.registerSetting<bool>("stochasticLightingGuiding", !guiding || ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_GUIDING"));
    }
    // Guide-normalized accumulation in the denoiser (docs/stochastic_lighting.md "Denoiser", "Overhaul phase 2"; on unless
    // SARP_STOCHASTIC_NORMALIZE=0, since overhaul phase 5). Per frame; the denoiser's history restarts when it changes.
    {
        const char* normalize = std::getenv("SARP_STOCHASTIC_NORMALIZE");
        settingsManager.registerSetting<bool>("stochasticLightingNormalize", !normalize || ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_NORMALIZE"));
    }
    // The paired shadowed / unshadowed ratio on top of it (docs/stochastic_lighting.md "The paired ratio"; SARP_STOCHASTIC_PAIRED=1).
    // Only with stochasticLightingNormalize. Per frame; the denoiser's history restarts when it changes. It forces the exact guide
    // (no far-field guide, "Correctness pass (2026-11-17)"), whose cost grows with the lights per froxel.
    settingsManager.registerSetting<bool>("stochasticLightingPaired", ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_PAIRED"));
    // Light-mixture confidence on top of it (docs/stochastic_lighting.md "Guide-normalized accumulation"; on unless SARP_STOCHASTIC_MIXTURE=0,
    // since overhaul phase 5): the history shortens where the lights its unshadowed light comes from have changed. Only with
    // stochasticLightingNormalize. Per frame; the denoiser's history restarts when it changes.
    {
        const char* mixture = std::getenv("SARP_STOCHASTIC_MIXTURE");
        settingsManager.registerSetting<bool>("stochasticLightingMixtureConfidence", !mixture || ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_MIXTURE"));
    }
    // Visibility gradients (docs/stochastic_lighting.md "Visibility gradients"; on unless
    // SARP_STOCHASTIC_VISIBILITY_GRADIENT=0): one matched shadow ray per cell of sampled pixels re-traces a sample of last frame's,
    // and the history shortens where the same light's visibility changed. Only with stochasticLightingNormalize. Per frame; the
    // denoiser's history restarts when it changes.
    {
        const char* gradient = std::getenv("SARP_STOCHASTIC_VISIBILITY_GRADIENT");
        settingsManager.registerSetting<bool>("stochasticLightingVisibilityGradient", !gradient || ReadTruthyEnvironmentFlag("SARP_STOCHASTIC_VISIBILITY_GRADIENT"));
    }
    // Lights drawn per pixel (1 to 4), each with one shadow ray. Per frame.
    {
        const char* samples = std::getenv("SARP_STOCHASTIC_SAMPLES");
        settingsManager.registerSetting<uint32_t>("stochasticLightingSamplesPerPixel",
            std::clamp(samples ? static_cast<uint32_t>(std::atoi(samples)) : 2u, 1u, 4u));
    }
    // Reduced-resolution sampling: 2 samples one pixel of each 2x2 a frame (twice the samples there, so one per pixel at the
    // default 2) and the denoiser reconstructs full resolution; 1 samples every pixel. Rebuilds the graph.
    {
        const char* downsample = std::getenv("SARP_STOCHASTIC_DOWNSAMPLE");
        settingsManager.registerSetting<uint32_t>("stochasticLightingDownsample",
            std::clamp(downsample ? static_cast<uint32_t>(std::atoi(downsample)) : 2u, 1u, 2u));
    }
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
        UpscalingManager::GetInstance().SetRayReconstructionGuidesAvailable(true);
        SelectPathTracingUpscaler(true);
    }
    settingsManager.registerSetting<bool>("enableRayTracedReflections", m_rayTracedReflections);
    // 0 raster, 1 raytraced where the BVH can represent the draw, 2 raster with per-pixel agreement counted against rays.
    settingsManager.registerSetting<uint32_t>("rayTracedPrimaryVisibilityMode", m_rayTracedPrimaryVisibilityMode);
    settingsManager.registerSetting<bool>("enableRayTracedShadows", m_rayTracedShadows);
    settingsManager.registerSetting<float>("rayTracedReflectionMaxDistance", 16384.0f); // World units: four exterior cells.
    settingsManager.registerSetting<float>("rayTracedReflectionRoughnessCutoff", 1.0f);
    settingsManager.registerSetting<bool>("enableSceneRenderOverlap", m_sceneRenderOverlapEnabled);
	settingsManager.registerSetting<bool>(MaterialTextureStreamingSettingName, true);
	settingsManager.registerSetting<bool>("renderGraphCompileDumpEnabled", false);
    settingsManager.registerSetting<bool>(
        "renderGraphVramDumpEnabled",
        ReadTruthyEnvironmentFlag("BASICRENDERER_RENDER_GRAPH_VRAM_DUMP"));
	settingsManager.registerSetting<org::AutoAliasMode>("autoAliasMode", org::AutoAliasMode::Balanced);
    settingsManager.registerSetting<org::AutoAliasPackingStrategy>("autoAliasPackingStrategy", org::AutoAliasPackingStrategy::GreedySweepLine);
    settingsManager.registerSetting<bool>("autoAliasEnableLogging", ReadTruthyEnvironmentFlag("BASICRENDERER_AUTO_ALIAS_LOG"));
	settingsManager.registerSetting<uint32_t>("autoAliasPoolRetireIdleFrames", 120u);
	settingsManager.registerSetting<float>("autoAliasPoolGrowthHeadroom", 1.5f);
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
    settingsManager.registerSetting<uint32_t>(
        CLodVirtualShadowUpgradeCooldownFramesSettingName,
        ReadUintEnvironmentValue("SARP_CLOD_VSM_UPGRADE_COOLDOWN_FRAMES", 8u));
    settingsManager.registerSetting<uint32_t>(
        CLodVirtualShadowDebugLightSettingName, ReadUintEnvironmentValue("SARP_CLOD_VSM_DEBUG_LIGHT", 0xFFFFFFFFu));
    settingsManager.registerSetting<std::vector<uint32_t>>(CLodVirtualShadowDebugLightIndicesSettingName, {});
    settingsManager.registerSetting<std::vector<std::string>>(CLodVirtualShadowDebugLightLabelsSettingName, {});
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
    settingsManager.registerSetting<uint32_t>(CLodLocalVirtualShadowMaxFacesSettingName, CLodVirtualShadowDefaultLocalMaxFaces);
    settingsManager.registerSetting<float>(CLodLocalVirtualShadowLodBiasSettingName, CLodVirtualShadowDefaultLocalLodBias);
    settingsManager.registerSetting<float>(CLodLocalVirtualShadowMovingLodBiasSettingName, CLodVirtualShadowDefaultLocalMovingLodBias);
    settingsManager.registerSetting<bool>(CLodLocalVirtualShadowForceInvalidateSettingName, false);
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
    getShadowResolution = settingsManager.getSettingGetter<uint16_t>("shadowResolution");
    setCameraSpeed = settingsManager.getSettingSetter<float>("cameraSpeed");
	getCameraSpeed = settingsManager.getSettingGetter<float>("cameraSpeed");
	setShadowsEnabled = settingsManager.getSettingSetter<bool>("enableShadows");
	getShadowsEnabled = settingsManager.getSettingGetter<bool>("enableShadows");
	getSkyboxResolution = settingsManager.getSettingGetter<uint16_t>("skyboxResolution");
	setImageBasedLightingEnabled = settingsManager.getSettingSetter<bool>("enableImageBasedLighting");
	setEnvironment = settingsManager.getSettingSetter<std::string>("environmentName");
	getDrawBoundingSpheres = settingsManager.getSettingGetter<bool>("drawBoundingSpheres");
	getImageBasedLightingEnabled = settingsManager.getSettingGetter<bool>("enableImageBasedLighting");

    // The recipe-backed settings were registered from the recipe Initialize was given, which path tracing may have
    // replaced since. Before their observers exist, so this edits no recipe.
    SyncPipelineTopologySettings();

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
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableStochasticLighting", [this](const bool& newValue) {
        if (m_syncingPipelineTopologySettings) {
            return;
        }
        if (m_pathTracing) {
            // The raster recipe waiting for raster to come back is the one that has it.
            if (m_rasterRecipeBeforePathTracing) {
                if (newValue) m_rasterRecipeBeforePathTracing->Add<br::pipeline::StochasticDirectLightingTechnique>();
                else m_rasterRecipeBeforePathTracing->Remove<br::pipeline::StochasticDirectLightingTechnique>();
            }
            return;
        }
        auto recipe = GetPipelineRecipeForMutation();
        if (newValue) recipe.Add<br::pipeline::StochasticDirectLightingTechnique>();
        else recipe.Remove<br::pipeline::StochasticDirectLightingTechnique>();
        RequestPipelineReplacement(std::move(recipe));
        }));
    // These choose stochastic lighting's passes, which its graph extension makes from the build's settings.
    for (const char* name : { "stochasticLightingReference", "stochasticLightingAccumulate", "stochasticLightingDenoise" }) {
        m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(name, [this, name](const bool&) {
            RequestGraphRebuild(std::string("setting ") + std::string(name), ShadingEpochs());
        }));
    }
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("stochasticLightingDownsample", [this](const uint32_t&) {
        RequestGraphRebuild(std::string("setting ") + "stochasticLightingDownsample", ShadingEpochs());
    }));
    // At full resolution the estimator's buffers hold the setting's samples per pixel (docs/stochastic_lighting.md "Kernel split"), so a change rebuilds them.
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("stochasticLightingSamplesPerPixel", [this](const uint32_t&) {
        if (SettingsManager::GetInstance().getSettingGetter<uint32_t>("stochasticLightingDownsample")() == 1u) {
            RequestGraphRebuild(std::string("setting ") + "stochasticLightingSamplesPerPixel", ShadingEpochs());
        }
    }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<std::string>("environmentName", [this](const std::string& newValue) {
		SetEnvironmentInternal(s2ws(newValue));
		RequestGraphRebuild(std::string("setting ") + "environmentName");
		}));
    bool outputTypeRequiresRenderGraphRebuild =
        OutputTypeRequiresRenderGraphRebuild(settingsManager.getSettingGetter<unsigned int>("outputType")());
    m_settingsSubscriptions.push_back(settingsManager.addObserver<unsigned int>("outputType", [this, outputTypeRequiresRenderGraphRebuild](const unsigned int& newValue) mutable {
        ::ResourceManager::GetInstance().SetOutputType(newValue);
        const bool newOutputTypeRequiresRenderGraphRebuild = OutputTypeRequiresRenderGraphRebuild(newValue);
        if (newOutputTypeRequiresRenderGraphRebuild != outputTypeRequiresRenderGraphRebuild) {
            // The debug resolve and the skeleton view that replaces it are both Overlay's (M6 step 3).
            RequestGraphRebuild(std::string("setting ") + "outputType", OverlayEpochs());
        }
        outputTypeRequiresRenderGraphRebuild = newOutputTypeRequiresRenderGraphRebuild;
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("allowTearing", [this](const bool& newValue) {
		m_allowTearing = newValue;
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("drawBoundingSpheres", [this](const bool& newValue) {
		RequestGraphRebuild(std::string("setting ") + "drawBoundingSpheres", OverlayEpochs());
		}));
    for (const char* name : { LightGridTileSizeSettingName, LightGridSlicesSettingName }) {
        m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(name, [this, name](const uint32_t&) {
            RequestGraphRebuild(std::string("setting ") + std::string(name), ShadingEpochs());
            }));
    }
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
    // Which technique fills the screen-space AO slot, and CORTAO's sample count (a pipeline define): both rebuild the graph.
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(br::ambient_occlusion::TechniqueSettingName, [this](const uint32_t&) {
		RequestGraphRebuild(std::string("setting ") + std::string(br::ambient_occlusion::TechniqueSettingName), ShadingEpochs());
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(br::ambient_occlusion::CortaoSampleCountSettingName, [this](const uint32_t&) {
		RequestGraphRebuild(std::string("setting ") + std::string(br::ambient_occlusion::CortaoSampleCountSettingName), ShadingEpochs());
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
        RequestGraphRebuild(std::string("setting ") + "terrainRvtCompression", VisibilityShadingEpochs());
        }));
    // Sizes the compression scratch.
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtMaxGeneratedPagesPerFrame", [this](const uint32_t& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + "terrainRvtMaxGeneratedPagesPerFrame", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPageSize", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtPageSize", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtBorderTexels", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtBorderTexels", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPhysicalAtlasPagesWide", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtPhysicalAtlasPagesWide", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPhysicalAtlasPagesHigh", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtPhysicalAtlasPagesHigh", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtPhysicalAtlasPoolCount", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtPhysicalAtlasPoolCount", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtClipPageTableResolution", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtClipPageTableResolution", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtMaxTerrainSets", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtMaxTerrainSets", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("terrainRvtMaxClipLevels", [this](const uint32_t& newValue) {
        (void)newValue;
        m_producerPersistentState->InvalidateTerrainRvt();
        RequestGraphRebuild(std::string("setting ") + "terrainRvtMaxClipLevels", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableOcclusionCulling", [this](const bool& newValue) {
		m_occlusionCulling = newValue;
		RequestGraphRebuild(std::string("setting ") + "enableOcclusionCulling", VisibilityShadingEpochs());
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodCullingBackend>(CLodCullingBackendSettingName, [this](const CLodCullingBackend& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodCullingBackendSettingName), VisibilityShadingEpochs());
        }));
        m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableSceneRenderOverlap", [this](const bool& newValue) {
                SetSceneRenderOverlapEnabled(newValue);
                }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodSoftwareRasterMode>(CLodSoftwareRasterModeSettingName, [this](const CLodSoftwareRasterMode& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodSoftwareRasterModeSettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodVSMRasterMode>(CLodVSMRasterModeSettingName, [this](const CLodVSMRasterMode& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodVSMRasterModeSettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<CLodTransparencyMode>(CLodTransparencyModeSettingName, [this](const CLodTransparencyMode& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodTransparencyModeSettingName), ShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodDynamicWindBoundsCacheEnabledSettingName, [this](const bool&) {
        RequestGraphRebuild(std::string("setting ") + std::string(CLodDynamicWindBoundsCacheEnabledSettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodDynamicWindBoundsCacheMiBSettingName, [this](const uint32_t&) {
        RequestGraphRebuild(std::string("setting ") + std::string(CLodDynamicWindBoundsCacheMiBSettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodDynamicWindVertexCacheEnabledSettingName, [this](const bool&) {
        RequestGraphRebuild(std::string("setting ") + std::string(CLodDynamicWindVertexCacheEnabledSettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodDynamicWindVertexCacheMiBSettingName, [this](const uint32_t&) {
        RequestGraphRebuild(std::string("setting ") + std::string(CLodDynamicWindVertexCacheMiBSettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodDirectionalVirtualShadowReceiverSubpageModeSettingName, [this](const uint32_t&) {
        RequestGraphRebuild(std::string("setting ") + std::string(CLodDirectionalVirtualShadowReceiverSubpageModeSettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodVisibleClusterCapacitySettingName, [this](const uint32_t& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodVisibleClusterCapacitySettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodWorkGraphReyesVisibilitySettingName, [this](const bool& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodWorkGraphReyesVisibilitySettingName), VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>(CLodWorkGraphRigidOnlySettingName, [this](const bool& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodWorkGraphRigidOnlySettingName), VisibilityShadingEpochs());
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
            RequestGraphRebuild(std::string("setting ") + std::string(CLodDirectionalVirtualShadowMaxBackingResolutionSettingName), VisibilityShadingEpochs());
            }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(CLodReyesResourceBudgetBytesSettingName, [this](const uint32_t& newValue) {
        (void)newValue;
        RequestGraphRebuild(std::string("setting ") + std::string(CLodReyesResourceBudgetBytesSettingName), VisibilityShadingEpochs());
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

        // A resolution request, consumed at the consolidation point (ApplyResolutionRequest): the guides come from the recipe being
        // built, and a path tracing toggle replaces it in the same frame (SelectPathTracingUpscaler), so one set has the right upscaler.
        m_frameRevisions.RequestUpscaler("setting upscalingMode");
		}));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("rayReconstructionGuidesAvailable", [this](const bool& newValue) {
        // Applied now rather than deferred: the graph rebuild of the pipeline
        // replacement announcing this runs before the next deferred flush, and
        // must not declare guides the outgoing pipeline wrote.
        UpscalingManager::GetInstance().SetRayReconstructionGuidesAvailable(newValue);
        if (SettingsManager::GetInstance().getSettingGetter<UpscalingMode>("upscalingMode")() != UpscalingMode::DLSSRayReconstruction) {
            return;
        }
        // A configuration prepared for the replacement's recipe already has them (ApplyResolutionRequest): then nothing changes.
        m_frameRevisions.RequestUpscaler("setting rayReconstructionGuidesAvailable");
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<UpscaleQualityMode>("upscalingQualityMode", [this](const UpscaleQualityMode& newValue) {

        m_frameRevisions.RequestUpscaler("setting upscalingQualityMode");
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableDilatedMotionVectors", [this](const bool&) {
        m_preFrameDeferredFunctions.defer([this]() {
            UpscalingManager::GetInstance().RequestHistoryReset();
            RequestGraphRebuild(std::string("setting ") + "enableDilatedMotionVectors", PostEpochs());
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
		RequestGraphRebuild(std::string("setting ") + "enableScreenSpaceReflections", ShadingEpochs());
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
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("lensFlareEnabled", [this](const bool&) { RequestGraphRebuild(std::string("setting ") + "lensFlareEnabled", PostEpochs()); }));
    for (const char* name : { "lensFlareMaxGhosts", "lensFlareGridResolution", "lensFlareWavelengths", "lensFlareMaxLights" }) {
        m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>(name, [this, name](const uint32_t&) { RequestGraphRebuild(std::string("setting ") + std::string(name), PostEpochs()); }));
    }
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("pathTracingAccumulate", [this](const bool&) {
        if (m_pathTracing) {
            SelectPathTracingUpscaler(true);
            RequestGraphRebuild(std::string("setting ") + "pathTracingAccumulate", ShadingPostEpochs());
        }
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<uint32_t>("rayTracedPrimaryVisibilityMode", [this](const uint32_t& newValue) {
        m_rayTracedPrimaryVisibilityMode = newValue;
        RequestGraphRebuild(std::string("setting ") + "rayTracedPrimaryVisibilityMode", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableRayTracedShadows", [this](const bool& newValue) {
        m_rayTracedShadows = newValue;
        RequestGraphRebuild(std::string("setting ") + "enableRayTracedShadows", VisibilityShadingEpochs());
        }));
    m_settingsSubscriptions.push_back(settingsManager.addObserver<bool>("enableRayTracedReflections", [this](const bool& newValue) {
        m_rayTracedReflections = newValue;
        if (newValue && !DeviceManager::GetInstance().GetCLodRayTracingSupported() && !m_warnedRayTracedReflectionsUnsupported) {
            m_warnedRayTracedReflectionsUnsupported = true;
            spdlog::warn("Ray traced reflections requested, but clustered ray tracing is not supported by the active RHI backend/device.");
        }
        RequestGraphRebuild(std::string("setting ") + "enableRayTracedReflections", VisibilityShadingEpochs());
        }));


}
