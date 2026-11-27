#include "Hair/HairCommands.h"

#include <nlohmann/json.hpp>

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
};

const char* DebugViewText(HairDebugView view)
{
    for (const auto& entry : kDebugViews) {
        if (entry.view == view) return entry.name;
    }
    return "off";
}

Json DescribeLod(const HairLodSettings& lod)
{
    return {
        { "error_pixels", lod.errorPixels },   { "max_width_scale", lod.maxWidthScale }, { "min_detail", lod.minDetail },
        { "force_detail", lod.forceDetail },   { "segment_pixels", lod.segmentPixels }, { "freeze", lod.freeze },
    };
}

Json DescribeFrameCounters(const HairGpuCounters& counters, const HairCapacities& capacities)
{
    return {
        { "valid", counters.valid },
        { "frame", counters.frame },
        { "selected", counters.selected },
        { "camera_position", { counters.cameraPosition.x, counters.cameraPosition.y, counters.cameraPosition.z } },
        { "clusters_tested", counters.clustersTested },
        { "clusters_visible", counters.clustersVisible },
        { "instances_visible", counters.instancesVisible },
        { "strands_selected", counters.strandsSelected },
        { "segments_selected", counters.segmentsSelected },
        { "pool_vertices", Rows(counters.poolVertices, capacities.poolVertices) },
        { "visible_clusters", Rows(counters.clustersVisible, capacities.visibleClusters) },
        { "overflow_clusters", counters.overflowClusters },
        { "segments_drawn", counters.segmentsDrawn },
        { "pixels_written", counters.pixelsWritten },
        { "pixels_occluded", counters.pixelsOccluded },
        { "segments_capped", counters.segmentsCapped },
        { "pixels_resolved", counters.pixelsResolved },
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
            { "root_shift", instance.rootShift },
            { "sim_level", instance.simLevel },
            { "target_level", instance.targetLevel },
            { "coverage", instance.coverage },
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
        { "steps", status.steps },
        { "simulated_seconds", status.simulatedSeconds },
        { "dropped_seconds", status.droppedSeconds },
        { "capped_frames", status.cappedFrames },
        { "last_frame_steps", status.lastFrameSteps },
        { "guide_points", status.guidePoints },
        { "metrics", std::move(metrics) },
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
    reply["rows"] = {
        { "clusters", Rows(stats.clusters, stats.capacities.clusters) },
        { "strands", Rows(stats.strands, stats.capacities.strands) },
        { "guides", Rows(stats.guides, stats.capacities.guides) },
        { "points", Rows(stats.points, stats.capacities.points) },
        { "instances", Rows(stats.instances, stats.capacities.instances) },
        { "root_frames", Rows(stats.rootFrames, stats.capacities.rootFrames) },
        { "guide_points", Rows(stats.guidePoints, stats.capacities.guidePoints) },
    };
    reply["buffers_created"] = stats.buffersCreated;
    reply["buffer_bytes"] = stats.bufferBytes;
    reply["asset_bytes_uploaded"] = stats.assetBytesUploaded;
    reply["instance_bytes_uploaded"] = stats.instanceBytesUploaded;
    reply["bytes_uploaded"] = stats.assetBytesUploaded + stats.instanceBytesUploaded;
    reply["pending_upload_assets"] = stats.pendingUploadAssets;
    reply["frames"] = stats.frames;
    reply["root_binding_dispatches"] = stats.rootBindingDispatches;
    const auto& counters = stats.gpuCounters;
    reply["gpu_counter"] = {
        { "valid", counters.valid },
        { "root_binding_dispatches", counters.rootBindingDispatches },
        { "cpu_root_binding_dispatches", counters.cpuRootBindingDispatches },
        { "frame", counters.frame },
    };
    // The same readback: what the select and raster passes counted in that frame.
    reply["gpu_frame"] = DescribeFrameCounters(counters, stats.capacities);
    reply["continuous_counters"] = stats.continuousCounters;
    Json history = Json::array();
    for (const auto& frame : stats.gpuCounterHistory) history.push_back(DescribeFrameCounters(frame, stats.capacities));
    reply["gpu_history"] = std::move(history);
    reply["lod"] = DescribeLod(stats.lod);
    reply["debug_view"] = DebugViewText(stats.debugView);
    reply["visibility_target"] = { stats.visibilityWidth, stats.visibilityHeight };
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
            { "active", instance.active },
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
        material.baseColor = ReadFloat3(args, "color", material.baseColor);
        material.roughness = args.value("roughness", material.roughness);
        std::string error;
        const auto instance = runtime->AttachGroom(asset->get<std::uint32_t>(), args.value("entity", std::string()),
            args.value("bone", std::string()), material, error);
        if (!instance) throw std::runtime_error(error);
        return Json{ { "instance", *instance } };
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
        throw std::runtime_error("unknown hair debug view " + name + ": expected off, cluster, detail, strand, rank or vertex_level");
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
