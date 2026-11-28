#include "Hair/HairCommands.h"

#include "Hair/HairBsdfShared.h"

#include <BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace br::hair {
namespace {

using Json = nlohmann::json;

Float3 ReadFloat3(const Json& object, const char* name, Float3 fallback)
{
    const auto found = object.find(name);
    if (found == object.end()) return fallback;
    if (!found->is_array() || found->size() != 3u) throw std::runtime_error(std::string(name) + " must be [x, y, z]");
    return { (*found)[0].get<float>(), (*found)[1].get<float>(), (*found)[2].get<float>() };
}

ProceduralGroomSettings ReadProcedural(const Json& spec)
{
    if (!spec.is_object()) throw std::runtime_error("procedural must be an object");
    ProceduralGroomSettings settings;
    const std::string style = spec.value("style", "straight");
    if (style == "straight") settings.style = ProceduralHairStyle::Straight;
    else if (style == "wavy") settings.style = ProceduralHairStyle::Wavy;
    else if (style == "curly") settings.style = ProceduralHairStyle::Curly;
    else throw std::runtime_error("unknown procedural style " + style + ": expected straight, wavy or curly");
    settings.strandCount = spec.value("strands", settings.strandCount);
    settings.seed = spec.value("seed", settings.seed);
    settings.pointsPerStrand = spec.value("points", settings.pointsPerStrand);
    settings.strandLength = spec.value("length", settings.strandLength);
    settings.capRadius = spec.value("radius", settings.capRadius);
    settings.capCenter = ReadFloat3(spec, "center", settings.capCenter);
    settings.capAxis = ReadFloat3(spec, "axis", settings.capAxis);
    settings.capAngleRadians = spec.value("cap_angle_degrees", 90.0f) * (3.14159265358979f / 180.0f);
    settings.rootWidth = spec.value("root_width", settings.rootWidth);
    settings.tipWidth = spec.value("tip_width", settings.tipWidth);
    settings.waveAmplitude = spec.value("wave_amplitude", settings.waveAmplitude);
    settings.waveCount = spec.value("wave_count", settings.waveCount);
    settings.droop = spec.value("droop", settings.droop);
    settings.droopDirection = ReadFloat3(spec, "down", settings.droopDirection);
    if (settings.strandCount == 0u || settings.pointsPerStrand < 2u) {
        throw std::runtime_error("a procedural groom needs at least one strand and two points per strand");
    }
    return settings;
}

const char* StateName(HairInstanceState state)
{
    switch (state) {
    case HairInstanceState::Attached: return "attached";
    case HairInstanceState::Failed: return "failed";
    default: return "pending";
    }
}

const char* ModeName(HairAttachMode mode)
{
    switch (mode) {
    case HairAttachMode::Bone: return "bone";
    case HairAttachMode::PerRoot: return "per_root";
    default: return "rigid";
    }
}

Json Rows(std::uint32_t used, std::uint32_t capacity) { return { { "used", used }, { "capacity", capacity } }; }

struct DebugViewName {
    const char* name;
    HairDebugView view;
};
constexpr DebugViewName kDebugViews[] = {
    { "off", HairDebugView::Off },           { "cluster", HairDebugView::Cluster }, { "detail", HairDebugView::Detail },
    { "strand", HairDebugView::Strand },     { "rank", HairDebugView::Rank },       { "vertex_level", HairDebugView::VertexLevel },
    { "coverage", HairDebugView::Coverage }, { "layers", HairDebugView::Layers },   { "alpha", HairDebugView::Alpha },
    { "reactive", HairDebugView::Reactive }, { "lobe_r", HairDebugView::LobeR },    { "lobe_tt", HairDebugView::LobeTT },
    { "lobe_trt", HairDebugView::LobeTRT },  { "environment", HairDebugView::Environment }, { "direct", HairDebugView::Direct },
    { "density", HairDebugView::Density },   { "transmittance", HairDebugView::Transmittance },
    { "fibre_count", HairDebugView::FibreCount }, { "scatter_global", HairDebugView::ScatterGlobal },
    { "scatter_local", HairDebugView::ScatterLocal }, { "vertex_density", HairDebugView::VertexDensity },
};

const char* DebugViewText(HairDebugView view)
{
    for (const auto& entry : kDebugViews) {
        if (entry.view == view) return entry.name;
    }
    return "off";
}

struct CoverageModeName {
    const char* name;
    HairCoverageMode mode;
};
constexpr CoverageModeName kCoverageModes[] = {
    { "off", HairCoverageMode::Off }, { "white", HairCoverageMode::White }, { "interleaved", HairCoverageMode::Interleaved },
};

const char* CoverageModeText(HairCoverageMode mode)
{
    for (const auto& entry : kCoverageModes) {
        if (entry.mode == mode) return entry.name;
    }
    return "off";
}

Json DescribeLod(const HairLodSettings& lod)
{
    return {
        { "error_pixels", lod.errorPixels },   { "max_width_scale", lod.maxWidthScale }, { "min_detail", lod.minDetail },
        { "force_detail", lod.forceDetail },   { "segment_pixels", lod.segmentPixels }, { "freeze", lod.freeze },
        { "coverage", CoverageModeText(lod.coverage) }, { "coverage_accumulate", lod.accumulateCoverage },
        { "detail_exponent", lod.detailExponent },
    };
}

// The layered path's material, as hair.attach and hair.material take it.
Json DescribeMaterial(const HairMaterial& material)
{
    return {
        { "sigma_a", { material.sigmaA.x, material.sigmaA.y, material.sigmaA.z } },
        { "beta_m", material.betaM },
        { "beta_n", material.betaN },
        { "alpha_degrees", material.alphaDegrees },
        { "eta", material.eta },
        { "melanin_variation", material.melaninVariation },
        { "roughness_variation", material.roughnessVariation },
    };
}

Json DescribeRender(const HairRenderSettings& render)
{
    return {
        { "path", render.path == HairRenderPath::Layered ? "layered" : "surface" },
        { "depth_alpha", render.depthAlpha },
        { "coverage_saturation", render.coverageSaturation },
        { "reactive", render.reactiveMask },
        { "reactive_strength", render.reactiveStrength },
        { "reactive_pixels", render.reactivePixels },
        { "ambient", render.ambient },
        { "lights", render.lights == HairLightLookup::Grid ? "grid" : "list" },
        { "raster", render.raster == HairRasterMode::Culled ? "culled" : render.raster == HairRasterMode::Auto ? "auto" : "direct" },
        { "auto_width_pixels", render.autoWidthPixels },
        { "opaque_coverage", render.opaqueCoverage },
        { "occlude_opaque", render.occludeOpaque },
        { "occlude_saturated", render.occludeSaturated },
        { "saturation", render.saturation },
        { "first_pass", render.firstPass == HairFirstPass::All ? "all" : render.firstPass == HairFirstPass::Previous ? "previous"
            : render.firstPass == HairFirstPass::Prefix ? "prefix" : "both" },
        { "raster_profile", render.rasterProfile },
        { "late_pass", render.latePass },
        { "hint_decay", render.hintDecay },
        { "hidden_detail", render.hiddenDetail },
        { "detail_rate_up", render.detailRateUp },
        { "detail_rate_down", render.detailRateDown },
    };
}

const char* ShadowCoverageName(HairShadowCoverage coverage)
{
    return coverage == HairShadowCoverage::Opaque ? "opaque" : coverage == HairShadowCoverage::Fixed ? "fixed" : "temporal";
}

Json DescribeShadows(const HairShadowSettings& shadows, const HairShadowStats& stats)
{
    return {
        { "enabled", shadows.enabled },
        { "detail_scale", shadows.detailScale },
        { "force_detail", shadows.forceDetail },
        { "coverage", ShadowCoverageName(shadows.coverage) },
        { "receiver_bias", shadows.receiverBias },
        { "dynamic_radius", shadows.dynamicRadius },
        // The last frame, on the CPU. The GPU's counts are in gpu_frame (shadow_*).
        { "active", stats.active },
        { "instances", stats.instances },
        { "moved_instances", stats.movedInstances },
        { "invalidations", stats.invalidations },
        { "total_invalidations", stats.totalInvalidations },
        { "dropped_invalidations", stats.droppedInvalidations },
        // The shadow map's own: the sun's clipmaps in use, and how many of the finest it redraws deformable casters into every frame.
        { "sun_clipmaps", g_clodSkinnedShadowActiveClipmapCount.load(std::memory_order_relaxed) },
        { "dynamic_clipmaps", g_clodSkinnedShadowEffectiveDynamicClipmapCount.load(std::memory_order_relaxed) },
    };
}

Json DescribeVolume(const HairVolumeSettings& volume, const HairVolumeStats& stats)
{
    return {
        { "enabled", volume.enabled },
        { "dual_scattering", volume.dualScattering },
        { "voxel_pixels", volume.voxelPixels },
        { "max_resolution", volume.maxResolution },
        { "voxel_budget", volume.voxelBudget },
        { "steps", volume.steps },
        { "environment_steps", volume.environmentSteps },
        { "bias", volume.bias },
        { "density_factor", volume.densityFactor },
        { "margin", volume.margin },
        { "scalp_depth", volume.scalpDepth },
        { "splat", volume.trilinear ? "trilinear" : "nearest" },
        { "jitter", volume.jitterPerFrame ? "frame" : "vertex" },
        // The last frame's volumes.
        { "volumes", stats.volumes },
        { "dropped_volumes", stats.droppedVolumes },
        { "unknown_instances", stats.unknownInstances },
        { "voxels", stats.voxels },
        { "voxel_bytes", static_cast<std::uint64_t>(stats.voxels) * sizeof(std::uint32_t) },
        { "capacity_voxels", stats.capacityVoxels },
        { "budget_voxels", stats.budgetVoxels },
        { "high_water_voxels", stats.highWaterVoxels },
        { "coarsening", stats.coarsening },
        { "largest_resolution", stats.largestResolution },
        { "smallest_voxel", stats.smallestVoxel },
        { "largest_voxel", stats.largestVoxel },
    };
}

// An instance's material from a command's fields; a field not given keeps `material`'s value. `color` and `roughness`
// are the surface path's placeholder. The layered path's absorption is the last of these that is given: `preset`,
// `melanin` with `redness`, `hair_color` (the colour a head of such hair has, by Chiang's fit) or `sigma_a` itself.
void ReadMaterial(const Json& args, HairMaterial& material)
{
    material.baseColor = ReadFloat3(args, "color", material.baseColor);
    material.roughness = args.value("roughness", material.roughness);
    material.betaM = args.value("beta_m", material.betaM);
    material.betaN = args.value("beta_n", material.betaN);
    material.alphaDegrees = args.value("alpha_degrees", material.alphaDegrees);
    material.eta = args.value("eta", material.eta);
    material.melaninVariation = args.value("melanin_variation", material.melaninVariation);
    material.roughnessVariation = args.value("roughness_variation", material.roughnessVariation);
    if (const auto preset = args.find("preset"); preset != args.end()) {
        const std::string name = preset->get<std::string>();
        if (!ApplyHairMaterialPreset(name, material)) {
            throw std::runtime_error("unknown hair material preset " + name + ": expected " + HairMaterialPresetNames());
        }
    }
    if (args.contains("melanin") || args.contains("redness")) {
        material.sigmaA = HairSigmaAFromMelaninRedness((std::max)(args.value("melanin", 2.0f), 0.0f),
            std::clamp(args.value("redness", 0.0f), 0.0f, 1.0f));
    }
    if (args.contains("hair_color")) {
        material.sigmaA = HairSigmaAFromColor(ReadFloat3(args, "hair_color", Float3{ 0.5f, 0.5f, 0.5f }), std::clamp(material.betaN, 0.0f, 1.0f));
    }
    material.sigmaA = ReadFloat3(args, "sigma_a", material.sigmaA);
}

Json DescribeFrameCounters(const HairGpuCounters& counters, const HairCapacities& capacities)
{
    return {
        { "valid", counters.valid },
        { "frame", counters.frame },
        { "selected", counters.selected },
        { "camera_position", { counters.cameraPosition.x, counters.cameraPosition.y, counters.cameraPosition.z } },
        { "stale_guides_drawn", counters.staleGuidesDrawn },
        { "unseen_strands_drawn", counters.unseenStrandsDrawn },
        { "clusters_tested", counters.clustersTested },
        { "clusters_visible", counters.clustersVisible },
        { "instances_visible", counters.instancesVisible },
        { "strands_selected", counters.strandsSelected },
        { "segments_selected", counters.segmentsSelected },
        { "pool_vertices", Rows(counters.poolVertices, capacities.poolVertices) },
        { "visible_clusters", Rows(counters.clustersVisible, capacities.visibleClusters) },
        { "overflow_clusters", counters.overflowClusters },
        { "pixels_covered", counters.pixelsCovered },
        { "coverage_sum", counters.coverageSum },
        { "segments_drawn", counters.segmentsDrawn },
        { "pixels_written", counters.pixelsWritten },
        { "pixels_occluded", counters.pixelsOccluded },
        { "segments_capped", counters.segmentsCapped },
        { "pixels_resolved", counters.pixelsResolved },
        // The layered path.
        { "layer_rejected", counters.layerRejected },
        { "coverage_skipped", counters.coverageSkipped },
        { "layer_pixels", counters.layerPixels },
        { "remainder_pixels", counters.remainderPixels },
        { "remainder_alpha", counters.remainderAlpha },
        { "alpha_sum", counters.alphaSum },
        { "depth_pixels", counters.depthPixels },
        { "shade_list", counters.shadeList },
        { "shaded_vertices", counters.shadedVertices },
        { "shadow_lookups", counters.shadowLookups },
        { "shadow_coarser", counters.shadowCoarser },
        { "shadow_missing", counters.shadowMissing },
        { "shadow_cleared", counters.shadowCleared },
        // The density volumes.
        { "volume_segments", counters.volumeSegments },
        { "volume_deposits", counters.volumeDeposits },
        { "volume_outside", counters.volumeOutside },
        { "raster_items", counters.rasterItems },
        { "raster_items_overflow", counters.rasterItemsOverflow },
        { "chunks", counters.chunks },
        { "chunks_late", counters.chunksLate },
        { "chunks_late_drawn", counters.chunksLateDrawn },
        { "pixels_tested", counters.pixelsTested },
        { "coverage_adds", counters.coverageAdds },
        { "hidden_writes", counters.hiddenWrites },
        { "pixels_saturated", counters.pixelsSaturated },
        { "late_pixels_written", counters.latePixelsWritten },
        { "hair_tiles", counters.hairTiles },
        { "clusters_unseen", counters.clustersUnseen },
        { "clusters_reduced", counters.clustersReduced },
        { "wave_iterations", counters.waveIterations },
        { "volume_vertices", counters.volumeVertices },
        { "volume_marches", counters.volumeMarches },
        { "volume_steps", counters.volumeSteps },
        { "shadow_clusters_tested", counters.shadowClustersTested },
        { "shadow_clusters", counters.shadowClusters },
        { "shadow_strands", counters.shadowStrands },
        { "shadow_segments", counters.shadowSegments },
        { "shadow_pool_vertices", counters.shadowPoolVertices },
        { "shadow_overflow_clusters", counters.shadowOverflow },
        { "shadow_unseen_strands", counters.shadowUnseenStrands },
        { "shadow_stale_guides_drawn", counters.shadowStaleGuides },
        { "shadow_segments_drawn", counters.shadowSegmentsDrawn },
        { "shadow_texels_covered", counters.shadowTexelsCovered },
        { "shadow_texels_written", counters.shadowTexelsWritten },
        { "shadow_segments_capped", counters.shadowSegmentsCapped },
        { "shadow_pages_admitted", counters.shadowPagesAdmitted },
        { "shadow_pages_invalidated", counters.shadowPagesInvalidated },
        { "shadow_clipmap_mask", counters.shadowClipmapMask },
    };
}

Json DescribeCapture(const HairDebugCapture& capture)
{
    const char* state = capture.state == HairDebugCapture::State::Pending ? "pending"
        : capture.state == HairDebugCapture::State::Written ? "written"
        : capture.state == HairDebugCapture::State::Failed ? "failed" : "none";
    return { { "state", state }, { "path", capture.path }, { "width", capture.width }, { "height", capture.height },
        { "error", capture.error } };
}

Json Float3Json(const Float3& value) { return { value.x, value.y, value.z }; }

Json DescribeSim(const HairSimStatus& status)
{
    const HairSimSettings& settings = status.settings;
    Json metrics = Json::array();
    for (const auto& instance : status.metrics) {
        Json row = {
            { "instance", instance.instance },
            { "frame", instance.frame },
            { "steps", instance.steps },
            { "guides", instance.guides },
            { "stretch_max", instance.stretchMax },
            { "stretch_mean", instance.stretchMean },
            { "stretched_guides", instance.stretchedGuides },
            { "speed_max", instance.speedMax },
            { "speed_mean", instance.speedMean },
            { "deviation_mean", instance.deviationMean },
            { "guide_roots", instance.guideRoots },
            { "guide_root_error", instance.guideRootError },
            { "strand_roots", instance.strandRoots },
            { "strand_root_error", instance.strandRootError },
            { "reset_guides", instance.resetGuides },
            { "resumed_guides", instance.resumedGuides },
            { "stale_guides", instance.staleGuides },
            { "holds_rows", instance.holdsRows },
            { "starved", instance.starved },
            { "root_shift", instance.rootShift },
            { "sim_level", instance.simLevel },
            { "target_level", instance.targetLevel },
            { "coverage", instance.coverage },
            { "view_extent", instance.viewExtent },
            { "in_view", instance.inView },
            { "level_guides", std::vector<std::uint32_t>(instance.levelGuides.begin(), instance.levelGuides.begin() + instance.levelCount) },
            { "simulated_points", instance.simulatedPoints },
            { "evaluated_guides", instance.evaluatedGuides },
            { "blending_guides", instance.blendingGuides },
            { "promoted_guides", instance.promotedGuides },
            { "demoted_guides", instance.demotedGuides },
            { "transition_motion", instance.transitionMotion },
            { "steady_motion", instance.steadyMotion },
        };
        // Absent without colliders.
        if (instance.hasColliders) row["collider_distance"] = instance.colliderDistance;
        metrics.push_back(std::move(row));
    }
    return {
        { "enabled", settings.enabled },
        { "gravity", Float3Json(settings.gravity) },
        { "damping", settings.damping },
        { "stretch_compliance", settings.stretchCompliance },
        { "bend_compliance", settings.bendCompliance },
        { "shape_compliance", settings.shapeCompliance },
        { "step_seconds", settings.stepSeconds },
        { "substeps", settings.substeps },
        { "max_steps", settings.maxStepsPerFrame },
        { "wind", Float3Json(settings.wind) },
        { "gust_amplitude", settings.gustAmplitude },
        { "gust_frequency", settings.gustFrequency },
        { "friction", settings.friction },
        { "collision_margin", settings.collisionMargin },
        { "teleport_distance", settings.teleportDistance },
        { "frame_seconds", settings.frameSeconds },
        { "lod_full_coverage", settings.lodFullCoverage },
        { "lod_level0_coverage", settings.lodLevel0Coverage },
        { "lod_off_coverage", settings.lodOffCoverage },
        { "lod_rate", settings.lodRate },
        { "lod_force_level", settings.lodForceLevel },
        { "lod_point_budget", settings.lodPointBudget },
        { "promote_seconds", settings.promoteSeconds },
        { "demote_seconds", settings.demoteSeconds },
        { "lod_view_grace_seconds", settings.lodViewGraceSeconds },
        { "lod_settle_seconds", settings.lodSettleSeconds },
        { "guide_point_budget", settings.guidePointBudget },
        { "guide_keep_seconds", settings.guideKeepSeconds },
        { "steps", status.steps },
        { "simulated_seconds", status.simulatedSeconds },
        { "dropped_seconds", status.droppedSeconds },
        { "capped_frames", status.cappedFrames },
        { "last_frame_steps", status.lastFrameSteps },
        { "guide_points", status.guidePoints },
        { "metrics", std::move(metrics) },
    };
}

// Each pooled buffer's capacity, what is in use and the most that was, and the lending of guide state.
Json DescribeMemory(const HairStats& stats)
{
    Json buffers = Json::array();
    std::uint64_t capacity = 0u;
    std::uint64_t used = 0u;
    std::uint64_t highWater = 0u;
    for (const HairBufferUse& buffer : stats.bufferUse) {
        capacity += buffer.capacityRows * buffer.rowBytes;
        used += buffer.usedRows * buffer.rowBytes;
        highWater += buffer.highWaterRows * buffer.rowBytes;
        buffers.push_back({
            { "name", buffer.name },
            { "row_bytes", buffer.rowBytes },
            { "capacity_rows", buffer.capacityRows },
            { "used_rows", buffer.usedRows },
            { "high_water_rows", buffer.highWaterRows },
            { "capacity_bytes", buffer.capacityRows * buffer.rowBytes },
            { "used_bytes", buffer.usedRows * buffer.rowBytes },
            { "high_water_bytes", buffer.highWaterRows * buffer.rowBytes },
        });
    }
    const HairGuideRowStats& guide = stats.guideRows;
    return {
        { "capacity_bytes", capacity },
        { "used_bytes", used },
        { "high_water_bytes", highWater }, // the sum of each buffer's own: an upper bound on the most in use at once
        { "buffers", std::move(buffers) },
        { "guide_state", {
            { "row_bytes", sizeof(HairGuidePoint) },
            { "capacity_rows", guide.capacityRows },
            { "budget_rows", guide.budgetRows },
            { "rows_in_use", guide.rowsInUse },
            { "rows_active", guide.rowsActive },
            { "rows_kept", guide.rowsInUse - guide.rowsActive },
            { "high_water_rows", guide.highWaterRows },
            { "largest_free_rows", guide.largestFreeRows },
            { "holders", guide.holders },
            { "active_holders", guide.activeHolders },
            { "kept_holders", guide.holders - guide.activeHolders },
            { "starved_instances", guide.starvedInstances },
            { "unsettled_instances", guide.unsettledInstances },
            { "allocations", guide.allocations },
            { "releases", guide.releases },
            { "evictions", guide.evictions },
            { "evictions_for_rows", guide.evictionsForRows },
            { "evictions_by_age", guide.evictionsByAge },
            { "allocation_failures", guide.allocationFailures },
        } },
    };
}

Json DescribeStats(const HairStats& stats)
{
    Json reply = Json::object();
    reply["assets"] = stats.assets;
    reply["instances"] = stats.instances;
    reply["attached_instances"] = stats.attachedInstances;
    reply["active_instances"] = stats.activeInstances;
    reply["failed_instances"] = stats.failedInstances;
    reply["strands"] = stats.strands;
    reply["points"] = stats.points;
    reply["instance_strands"] = stats.instanceStrands;
    reply["rows"] = {
        { "clusters", Rows(stats.clusters, stats.capacities.clusters) },
        { "strands", Rows(stats.strands, stats.capacities.strands) },
        { "guides", Rows(stats.guides, stats.capacities.guides) },
        { "points", Rows(stats.points, stats.capacities.points) },
        { "instances", Rows(stats.instances, stats.capacities.instances) },
        { "guide_points", Rows(stats.guidePoints, stats.capacities.guidePoints) }, // lent out now (memory.guide_state)
    };
    reply["buffers_created"] = stats.buffersCreated;
    reply["buffer_bytes"] = stats.bufferBytes;
    reply["memory"] = DescribeMemory(stats);
    reply["asset_bytes_uploaded"] = stats.assetBytesUploaded;
    reply["instance_bytes_uploaded"] = stats.instanceBytesUploaded;
    reply["bytes_uploaded"] = stats.assetBytesUploaded + stats.instanceBytesUploaded;
    reply["pending_upload_assets"] = stats.pendingUploadAssets;
    reply["frames"] = stats.frames;
    reply["evaluate_dispatches"] = stats.evaluateDispatches;
    const auto& counters = stats.gpuCounters;
    reply["gpu_counter"] = {
        { "valid", counters.valid },
        { "evaluate_dispatches", counters.evaluateDispatches },
        { "cpu_evaluate_dispatches", counters.cpuEvaluateDispatches },
        { "frame", counters.frame },
    };
    // The same readback: what the select and raster passes counted in that frame.
    reply["gpu_frame"] = DescribeFrameCounters(counters, stats.capacities);
    reply["continuous_counters"] = stats.continuousCounters;
    Json history = Json::array();
    for (const auto& frame : stats.gpuCounterHistory) history.push_back(DescribeFrameCounters(frame, stats.capacities));
    reply["gpu_history"] = std::move(history);
    reply["lod"] = DescribeLod(stats.lod);
    reply["render"] = DescribeRender(stats.render);
    reply["volume"] = DescribeVolume(stats.volume, stats.volumeStats);
    reply["shadows"] = DescribeShadows(stats.shadows, stats.shadowStats);
    Json targets = Json::array();
    for (const HairBufferUse& target : stats.targets) targets.push_back({ { "name", target.name }, { "bytes", target.rowBytes } });
    reply["targets"] = std::move(targets);
    reply["debug_view"] = DebugViewText(stats.debugView);
    reply["visibility_target"] = { stats.visibilityWidth, stats.visibilityHeight };
    reply["raster_active"] = stats.rasterCulled ? "culled" : "direct";
    reply["raster_width_pixels"] = stats.rasterWidthPixels;
    reply["capture"] = DescribeCapture(stats.debugCapture);
    reply["probe"] = DescribeCapture(stats.probe); // its width is the rows written
    reply["dumps"] = { { "pending", stats.dumpsPending }, { "written", stats.dumpsWritten }, { "failed", stats.dumpsFailed } };
    const auto& check = stats.selfCheck;
    reply["self_check"] = {
        { "enabled", check.enabled },
        { "roots_per_instance", check.rootsPerInstance },
        { "completed_checks", check.completedChecks },
        { "frame", check.frame },
        { "instances", check.instances },
        { "skinned_instances", check.skinnedInstances },
        { "stale_instances", check.staleInstances },
        { "roots", check.roots },
        { "previous_roots", check.previousRoots },
        { "position_error", check.positionError },
        { "direction_error", check.directionError },
        { "previous_position_error", check.previousPositionError },
        { "previous_direction_error", check.previousDirectionError },
        { "motion", check.motion },
        { "skinned_motion", check.skinnedMotion },
        { "largest_motion", check.largestMotion },
        { "largest_skinned_motion", check.largestSkinnedMotion },
        { "worst_position_error", check.worstPositionError },
        { "worst_direction_error", check.worstDirectionError },
        { "total_stale_instances", check.totalStaleInstances },
    };
    reply["sim"] = DescribeSim(stats.sim);
    Json instances = Json::array();
    for (const auto& instance : stats.instanceInfo) {
        instances.push_back({
            { "instance", instance.id },
            { "asset", instance.asset },
            { "entity", instance.entity },
            { "bone", instance.bone },
            { "state", StateName(instance.state) },
            { "mode", ModeName(instance.mode) },
            { "color", { instance.material.baseColor.x, instance.material.baseColor.y, instance.material.baseColor.z } },
            { "roughness", instance.material.roughness },
            { "material", DescribeMaterial(instance.material) },
            { "active", instance.active },
            { "strands", instance.strands },
            { "guide_rows", instance.guideRows },
            // Its first row of guide state while it holds rows; -1 while it has none.
            { "guide_row_base", instance.guideRowBase == HAIR_INVALID_INDEX ? -1 : static_cast<std::int64_t>(instance.guideRowBase) },
            // Its density volume in the last frame: voxels along each side (zeros: none) and a voxel's size, metres.
            { "volume_resolution", instance.volumeResolution },
            { "volume_voxel_size", instance.volumeVoxelSize },
            { "error", instance.error },
        });
    }
    reply["instance_list"] = std::move(instances);
    return reply;
}

} // namespace

void RegisterHairCommands(br::diagnostics::ControlCommandRegistry& commands, std::shared_ptr<HairRuntime> runtime)
{
    commands.Register("hair.load", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        HairGroomRequest groom;
        groom.path = args.value("path", std::string());
        const auto procedural = args.find("procedural");
        if (groom.path.empty() == (procedural == args.end())) {
            throw std::runtime_error("hair.load takes either path or procedural");
        }
        if (procedural != args.end()) groom.procedural = ReadProcedural(*procedural);
        groom.unitScale = args.value("unit_scale", 0.0f);
        // How the groom is built (GroomBuildSettings); a .groom file is loaded as it was built.
        groom.build.maxGuideCount = args.value("max_guides", groom.build.maxGuideCount);
        groom.build.maxVertexLevel = args.value("max_vertex_level", groom.build.maxVertexLevel);
        groom.offset = ReadFloat3(args, "offset", groom.offset);
        if (const auto upAxis = args.find("up_axis"); upAxis != args.end()) {
            const std::string axis = upAxis->get<std::string>();
            if (axis != "y" && axis != "z") throw std::runtime_error("up_axis must be y or z");
            groom.zUp = axis == "z";
        }
        std::string error;
        const auto asset = runtime->LoadGroom(groom, error);
        if (!asset) throw std::runtime_error(error);
        return Json{ { "asset", *asset } };
    });
    commands.Register("hair.attach", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        const auto asset = args.find("asset");
        if (asset == args.end() || !asset->is_number_unsigned()) throw std::runtime_error("hair.attach needs asset");
        HairMaterial material;
        ReadMaterial(args, material);
        std::string error;
        const auto instance = runtime->AttachGroom(asset->get<std::uint32_t>(), args.value("entity", std::string()),
            args.value("bone", std::string()), material, error);
        if (!instance) throw std::runtime_error(error);
        return Json{ { "instance", *instance } };
    });
    // Only the fields given change.
    commands.Register("hair.material", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        const auto instance = args.find("instance");
        if (instance == args.end() || !instance->is_number_unsigned()) throw std::runtime_error("hair.material needs instance");
        const std::uint32_t id = instance->get<std::uint32_t>();
        HairMaterial material;
        bool found = false;
        for (const auto& info : runtime->Stats().instanceInfo) {
            if (info.id != id) continue;
            material = info.material;
            found = true;
        }
        if (!found) throw std::runtime_error("no hair instance " + std::to_string(id));
        ReadMaterial(args, material);
        std::string error;
        if (!runtime->SetInstanceMaterial(id, material, error)) throw std::runtime_error(error);
        return Json{ { "instance", id }, { "material", DescribeMaterial(material) } };
    });
    commands.Register("hair.clear", [runtime](const br::diagnostics::ControlRequest&) {
        runtime->Clear();
        return Json::object();
    });
    commands.Register("hair.selfcheck", [runtime](const br::diagnostics::ControlRequest& request) {
        const bool enabled = request.args.value("enabled", true);
        runtime->SetSelfCheck(enabled, request.args.value("roots", 8u));
        return Json{ { "enabled", enabled } };
    });
    commands.Register("hair.stats", [runtime](const br::diagnostics::ControlRequest& request) {
        if (const auto continuous = request.args.find("continuous"); continuous != request.args.end()) {
            runtime->SetContinuousCounters(continuous->get<bool>());
        }
        return DescribeStats(runtime->Stats());
    });
    // Only the fields given change.
    commands.Register("hair.lod", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        HairLodSettings lod = runtime->LodSettings();
        lod.errorPixels = args.value("error_pixels", lod.errorPixels);
        lod.maxWidthScale = args.value("max_width_scale", lod.maxWidthScale);
        lod.minDetail = args.value("min_detail", lod.minDetail);
        lod.forceDetail = args.value("force_detail", lod.forceDetail);
        lod.segmentPixels = args.value("segment_pixels", lod.segmentPixels);
        lod.freeze = args.value("freeze", lod.freeze);
        lod.accumulateCoverage = args.value("coverage_accumulate", lod.accumulateCoverage);
        lod.detailExponent = args.value("detail_exponent", lod.detailExponent);
        if (const auto coverage = args.find("coverage"); coverage != args.end()) {
            // A mode's name, or true and false for the default mode and off.
            const std::string name = coverage->is_boolean() ? (coverage->get<bool>() ? "white" : "off") : coverage->get<std::string>();
            const auto found = std::ranges::find_if(kCoverageModes, [&](const CoverageModeName& entry) { return name == entry.name; });
            if (found == std::end(kCoverageModes)) {
                throw std::runtime_error("unknown hair coverage mode " + name + ": expected off, white or interleaved");
            }
            lod.coverage = found->mode;
        }
        runtime->SetLodSettings(lod);
        return DescribeLod(runtime->LodSettings());
    });
    commands.Register("hair.debug", [runtime](const br::diagnostics::ControlRequest& request) {
        const std::string name = request.args.value("view", std::string("cluster"));
        for (const auto& entry : kDebugViews) {
            if (name == entry.name) {
                runtime->SetDebugView(entry.view);
                return Json{ { "view", entry.name } };
            }
        }
        throw std::runtime_error("unknown hair debug view " + name +
            ": expected off, cluster, detail, strand, rank, vertex_level, coverage, layers, alpha, reactive, lobe_r, lobe_tt, "
            "lobe_trt, environment, direct, density, transmittance, fibre_count, scatter_global, scatter_local or vertex_density");
    });
    // The density volumes. Only the fields given change.
    commands.Register("hair.volume", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        HairVolumeSettings volume = runtime->VolumeSettings();
        volume.enabled = args.value("enabled", volume.enabled);
        volume.dualScattering = args.value("dual_scattering", volume.dualScattering);
        volume.voxelPixels = args.value("voxel_pixels", volume.voxelPixels);
        volume.maxResolution = args.value("max_resolution", volume.maxResolution);
        volume.voxelBudget = args.value("voxel_budget", volume.voxelBudget);
        volume.steps = args.value("steps", volume.steps);
        volume.environmentSteps = args.value("environment_steps", volume.environmentSteps);
        volume.bias = args.value("bias", volume.bias);
        volume.densityFactor = args.value("density_factor", volume.densityFactor);
        volume.margin = args.value("margin", volume.margin);
        volume.scalpDepth = args.value("scalp_depth", volume.scalpDepth);
        if (const auto splat = args.find("splat"); splat != args.end()) {
            const std::string name = splat->get<std::string>();
            if (name != "trilinear" && name != "nearest") throw std::runtime_error("unknown hair volume splat " + name + ": expected trilinear or nearest");
            volume.trilinear = name == "trilinear";
        }
        if (const auto jitter = args.find("jitter"); jitter != args.end()) {
            const std::string name = jitter->get<std::string>();
            if (name != "vertex" && name != "frame") throw std::runtime_error("unknown hair volume jitter " + name + ": expected vertex or frame");
            volume.jitterPerFrame = name == "frame";
        }
        runtime->SetVolumeSettings(volume);
        return DescribeVolume(runtime->VolumeSettings(), runtime->Stats().volumeStats);
    });
    // Shadow casting. Only the fields given change.
    commands.Register("hair.shadows", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        HairShadowSettings shadows = runtime->ShadowSettings();
        shadows.enabled = args.value("enabled", shadows.enabled);
        shadows.detailScale = args.value("detail_scale", shadows.detailScale);
        shadows.forceDetail = args.value("force_detail", shadows.forceDetail);
        shadows.receiverBias = args.value("receiver_bias", shadows.receiverBias);
        shadows.dynamicRadius = args.value("dynamic_radius", shadows.dynamicRadius);
        if (const auto coverage = args.find("coverage"); coverage != args.end()) {
            const std::string name = coverage->get<std::string>();
            if (name == "temporal") shadows.coverage = HairShadowCoverage::Temporal;
            else if (name == "fixed") shadows.coverage = HairShadowCoverage::Fixed;
            else if (name == "opaque") shadows.coverage = HairShadowCoverage::Opaque;
            else throw std::runtime_error("unknown hair shadow coverage " + name + ": expected temporal, fixed or opaque");
        }
        runtime->SetShadowSettings(shadows);
        return DescribeShadows(runtime->ShadowSettings(), runtime->Stats().shadowStats);
    });
    // Only the fields given change.
    commands.Register("hair.render", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        HairRenderSettings render = runtime->RenderSettings();
        if (const auto path = args.find("path"); path != args.end()) {
            const std::string name = path->get<std::string>();
            if (name != "surface" && name != "layered") throw std::runtime_error("unknown hair render path " + name + ": expected surface or layered");
            render.path = name == "layered" ? HairRenderPath::Layered : HairRenderPath::Surface;
        }
        if (const auto lights = args.find("lights"); lights != args.end()) {
            const std::string name = lights->get<std::string>();
            if (name != "list" && name != "grid") throw std::runtime_error("unknown hair light lookup " + name + ": expected list or grid");
            render.lights = name == "grid" ? HairLightLookup::Grid : HairLightLookup::List;
        }
        render.depthAlpha = args.value("depth_alpha", render.depthAlpha);
        render.coverageSaturation = args.value("coverage_saturation", render.coverageSaturation);
        render.reactiveMask = args.value("reactive", render.reactiveMask);
        render.reactiveStrength = args.value("reactive_strength", render.reactiveStrength);
        render.reactivePixels = args.value("reactive_pixels", render.reactivePixels);
        render.ambient = args.value("ambient", render.ambient);
        if (const auto raster = args.find("raster"); raster != args.end()) {
            const std::string name = raster->get<std::string>();
            if (name != "direct" && name != "culled" && name != "auto") throw std::runtime_error("unknown hair rasterizer " + name + ": expected auto, direct or culled");
            render.raster = name == "culled" ? HairRasterMode::Culled : name == "auto" ? HairRasterMode::Auto : HairRasterMode::Direct;
        }
        if (const auto firstPass = args.find("first_pass"); firstPass != args.end()) {
            const std::string name = firstPass->get<std::string>();
            if (name == "all") render.firstPass = HairFirstPass::All;
            else if (name == "previous") render.firstPass = HairFirstPass::Previous;
            else if (name == "prefix") render.firstPass = HairFirstPass::Prefix;
            else if (name == "both") render.firstPass = HairFirstPass::Both;
            else throw std::runtime_error("unknown hair first pass " + name + ": expected all, previous, prefix or both");
        }
        render.opaqueCoverage = args.value("opaque_coverage", render.opaqueCoverage);
        render.occludeOpaque = args.value("occlude_opaque", render.occludeOpaque);
        render.occludeSaturated = args.value("occlude_saturated", render.occludeSaturated);
        render.saturation = args.value("saturation", render.saturation);
        render.autoWidthPixels = args.value("auto_width_pixels", render.autoWidthPixels);
        render.rasterProfile = args.value("raster_profile", render.rasterProfile);
        render.latePass = args.value("late_pass", render.latePass);
        render.hintDecay = args.value("hint_decay", render.hintDecay);
        render.hiddenDetail = args.value("hidden_detail", render.hiddenDetail);
        render.detailRateUp = args.value("detail_rate_up", render.detailRateUp);
        render.detailRateDown = args.value("detail_rate_down", render.detailRateDown);
        runtime->SetRenderSettings(render);
        return DescribeRender(runtime->RenderSettings());
    });
    // Only the fields given change. `reset` puts every guide back on its rest pose.
    commands.Register("hair.sim", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        HairSimSettings sim = runtime->SimSettings();
        sim.enabled = args.value("enabled", sim.enabled);
        sim.gravity = ReadFloat3(args, "gravity", sim.gravity);
        sim.damping = args.value("damping", sim.damping);
        sim.stretchCompliance = args.value("stretch_compliance", sim.stretchCompliance);
        sim.bendCompliance = args.value("bend_compliance", sim.bendCompliance);
        sim.shapeCompliance = args.value("shape_compliance", sim.shapeCompliance);
        sim.stepSeconds = args.value("step_seconds", sim.stepSeconds);
        sim.substeps = args.value("substeps", sim.substeps);
        sim.maxStepsPerFrame = args.value("max_steps", sim.maxStepsPerFrame);
        sim.wind = ReadFloat3(args, "wind", sim.wind);
        sim.gustAmplitude = args.value("gust_amplitude", sim.gustAmplitude);
        sim.gustFrequency = args.value("gust_frequency", sim.gustFrequency);
        sim.friction = args.value("friction", sim.friction);
        sim.collisionMargin = args.value("collision_margin", sim.collisionMargin);
        sim.teleportDistance = args.value("teleport_distance", sim.teleportDistance);
        sim.frameSeconds = args.value("frame_seconds", sim.frameSeconds);
        sim.lodFullCoverage = args.value("lod_full_coverage", sim.lodFullCoverage);
        sim.lodLevel0Coverage = args.value("lod_level0_coverage", sim.lodLevel0Coverage);
        sim.lodOffCoverage = args.value("lod_off_coverage", sim.lodOffCoverage);
        sim.lodRate = args.value("lod_rate", sim.lodRate);
        sim.lodForceLevel = args.value("lod_force_level", sim.lodForceLevel);
        sim.lodPointBudget = args.value("lod_point_budget", sim.lodPointBudget);
        sim.promoteSeconds = args.value("promote_seconds", sim.promoteSeconds);
        sim.demoteSeconds = args.value("demote_seconds", sim.demoteSeconds);
        sim.lodViewGraceSeconds = args.value("lod_view_grace_seconds", sim.lodViewGraceSeconds);
        sim.lodSettleSeconds = args.value("lod_settle_seconds", sim.lodSettleSeconds);
        sim.guidePointBudget = args.value("guide_point_budget", sim.guidePointBudget);
        sim.guideKeepSeconds = args.value("guide_keep_seconds", sim.guideKeepSeconds);
        runtime->SetSimSettings(sim);
        if (args.value("reset", false)) runtime->ResetSimulation();
        return DescribeSim(runtime->SimStatus());
    });
    // Simulation LOD, for testing: forces one instance's simulation level; a negative level gives it back to hair.sim.
    commands.Register("hair.simlevel", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        const auto instance = args.find("instance");
        if (instance == args.end() || !instance->is_number_unsigned()) throw std::runtime_error("hair.simlevel needs instance");
        const float level = args.value("level", -1.0f);
        std::string error;
        if (!runtime->SetInstanceSimLevel(instance->get<std::uint32_t>(), level, error)) throw std::runtime_error(error);
        return Json{ { "instance", instance->get<std::uint32_t>() }, { "level", level } };
    });
    // Replaces an instance's colliders: spheres {center, radius} and capsules {a, b, radius}, in the entity's mesh space.
    commands.Register("hair.colliders", [runtime](const br::diagnostics::ControlRequest& request) {
        const Json& args = request.args;
        const auto instance = args.find("instance");
        if (instance == args.end() || !instance->is_number_unsigned()) throw std::runtime_error("hair.colliders needs instance");
        std::vector<HairCollider> colliders;
        if (const auto list = args.find("colliders"); list != args.end()) {
            if (!list->is_array()) throw std::runtime_error("colliders must be an array");
            for (const Json& spec : *list) {
                HairCollider collider;
                collider.a = ReadFloat3(spec, "a", ReadFloat3(spec, "center", collider.a));
                collider.b = ReadFloat3(spec, "b", collider.a);
                collider.radius = spec.value("radius", 0.0f);
                if (!(collider.radius > 0.0f)) throw std::runtime_error("a collider needs a radius above 0");
                colliders.push_back(collider);
            }
        }
        std::string error;
        if (!runtime->SetColliders(instance->get<std::uint32_t>(), colliders, error)) throw std::runtime_error(error);
        return Json{ { "instance", instance->get<std::uint32_t>() }, { "colliders", colliders.size() } };
    });
    commands.Register("hair.capture", [runtime](const br::diagnostics::ControlRequest& request) {
        const std::string path = request.args.value("path", std::string());
        if (path.empty()) throw std::runtime_error("hair.capture needs path (a .bmp file to write)");
        std::string error;
        if (!runtime->CaptureDebugView(path, error)) throw std::runtime_error(error);
        return Json{ { "path", path }, { "state", "pending" } };
    });
    commands.Register("hair.dump", [runtime](const br::diagnostics::ControlRequest& request) {
        const std::string buffer = request.args.value("buffer", std::string());
        const std::string path = request.args.value("path", std::string());
        if (path.empty()) throw std::runtime_error("hair.dump needs buffer and path (a binary file to write)");
        std::string error;
        if (!runtime->DumpBuffer(buffer, path, request.args.value("rows", 0u), error)) throw std::runtime_error(error);
        return Json{ { "buffer", buffer }, { "path", path }, { "state", "pending" } };
    });
    commands.Register("hair.probe", [runtime](const br::diagnostics::ControlRequest& request) {
        const std::string path = request.args.value("path", std::string());
        if (path.empty()) throw std::runtime_error("hair.probe needs path (a binary file to write)");
        std::string error;
        if (!runtime->CaptureProbe(path, error)) throw std::runtime_error(error);
        return Json{ { "path", path }, { "state", "pending" } };
    });
}

} // namespace br::hair
