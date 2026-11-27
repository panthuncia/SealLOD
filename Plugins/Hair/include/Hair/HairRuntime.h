#pragma once

#include "Hair/GroomBuilder.h"
#include "Hair/HairRuntimeGPUTypes.h"
#include "Hair/ProceduralGroom.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Renderer;
namespace org { class Buffer; }
namespace org { class Resource; }
namespace org::services { class BufferImage; }

namespace br::hair {

struct HairGpuBuffers; // src/HairGpuBuffers.h

// Rows of the pooled GPU buffers. They are made once, at these sizes, when the first groom is loaded; a groom or an
// instance that does not fit is refused. At the defaults they take about 673 MB.
struct HairCapacities {
    std::uint32_t clusters = 16384u;   // HairClusterHeader (80 B) + HairClusterAggregate (208 B)
    std::uint32_t strands = 262144u;   // HairStrandRecord (48 B) + HairRootBinding (80 B)
    std::uint32_t guides = 65536u;     // HairGuideBinding (32 B) + guide strand index (4 B)
    std::uint32_t points = 8388608u;   // HairPackedPoint (8 B)
    std::uint32_t instances = 1024u;   // HairInstanceGPU (96 B)
    std::uint32_t rootFrames = 1048576u; // HairRootFramePair (96 B): the strands of every instance
    // Per frame, for the main view. A visible cluster that does not fit either is dropped and counted (HairGpuCounters).
    std::uint32_t visibleClusters = HAIR_MAX_VISIBLE_CLUSTERS; // HairVisibleCluster (32 B); the visibility key's limit
    std::uint32_t poolVertices = 4194304u; // HairPoolVertex (32 B) + segment id (4 B): the drawn vertices of a frame
    std::uint32_t guidePoints = 4194304u;  // HairGuidePoint (80 B): the points of every guide of every instance
    std::uint32_t simGroups = 131072u;     // HairSimGroup (16 B): a frame's simulation thread groups, 64 guides each
};

// The simulation controls (hair.sim). Guides are simulated with a fixed timestep; the other strands follow them.
struct HairSimSettings {
    bool enabled = false; // off: every strand follows its root rigidly
    Float3 gravity{ 0.0f, -9.81f, 0.0f }; // m/s^2
    float damping = 3.0f;                 // 1/s: how fast a point's velocity goes to the air's (the wind, or rest)
    float stretchCompliance = 0.0f;       // XPBD, s^2: 0 keeps segments at their rest length
    // XPBD, s^2: how soft the pull of each point toward its rigid pose is at the tip (it stiffens toward the root). Under
    // gravity a free tip settles up to 9.81 * shapeCompliance metres from its rigid pose. It keeps the style.
    float shapeCompliance = 0.01f;
    // XPBD, s^2: of the constraint that keeps each point where the rest shape puts it after its two parents, which keeps
    // waves and curls. One sweep a substep makes it stiff against kinks and weak against long bends whatever this is.
    // With 0 a tightly curled strand on a weak shape pull may never come to rest.
    float bendCompliance = 1.0e-5f;
    float stepSeconds = 1.0f / 60.0f; // one fixed step
    std::uint32_t substeps = 4u;      // per step
    // The most steps one frame takes. A frame longer than that many steps loses the rest of its time (HairSimStatus).
    std::uint32_t maxStepsPerFrame = 4u;
    Float3 wind{};                // m/s
    float gustAmplitude = 0.5f;   // the wind's speed varies by this share of itself
    float gustFrequency = 0.7f;   // Hz
    float friction = 0.3f;        // [0, 1]: share of the sliding speed along a collider lost per substep in contact
    float collisionMargin = 0.002f; // metres kept between a guide point and a collider's surface
    float teleportDistance = 1.0f;  // a root that moved further than this in one frame puts its guide back on the rest pose
    // > 0: every frame simulates this much time, whatever the frame's own time step. For measuring: the statistical
    // profiler runs frames with a time step of 0, in which the simulation would take no step.
    float frameSeconds = 0.0f;

    // Simulation LOD. An instance's simulation level sFrac follows its coverage: the diameter of its groom's bounds on
    // screen over the height of the view. Above 0 its level 0 guides are simulated, a guide of level k >= 1 when
    // sFrac >= k + its random offset, and every guide at the groom's level count. Between the three coverages below
    // sFrac is linear in the logarithm of the coverage.
    float lodFullCoverage = 1.0f;     // here and above every guide is simulated: sFrac is the level count
    float lodLevel0Coverage = 0.125f; // here only level 0 is: sFrac 1
    float lodOffCoverage = 0.03f;     // here and below nothing is: sFrac 0, and the strands follow their roots rigidly
    float lodRate = 2.0f;             // levels per simulated second: the most sFrac moves toward what its coverage asks for
    float lodForceLevel = -1.0f;      // >= 0: every instance takes this sFrac at once, whatever its coverage; for testing
    // > 0: the most guide points simulated over all instances. When the levels asked for need more, all of them are
    // scaled down by one factor, though never below level 0 alone.
    std::uint32_t lodPointBudget = 0u;
    float promoteSeconds = 0.25f; // how long a newly simulated guide's state is blended from what its parents give it
    float demoteSeconds = 0.25f;  // how long a guide is still simulated while its state is blended back onto that
    bool operator==(const HairSimSettings&) const = default;
};

// A sphere or a capsule (a != b) in the rest space of an instance's entity: its mesh space, the groom's. It follows
// the instance's attach bone when it has one, and the entity's transform.
struct HairCollider {
    Float3 a{};
    Float3 b{};
    float radius = 0.0f;
};

// One instance's simulation metrics for one frame, read back after it (HAIR_SIM_METRIC_* in HairRuntimeGPUTypes.h).
// Stretch, speed and clearance are over the simulated points of its guides: every point but a guide's first two, which
// are held on the root.
struct HairSimMetrics {
    bool valid = false;
    std::uint32_t instance = 0u;
    std::uint64_t frame = 0u;
    std::uint64_t steps = 0u;          // HairSimStatus::steps as of that frame
    std::uint32_t guides = 0u;
    float stretchMax = 0.0f;           // largest |length / rest length - 1| of a segment
    float stretchMean = 0.0f;          // mean over guides of each guide's mean
    std::uint32_t stretchedGuides = 0u; // guides with a segment more than HAIR_SIM_METRIC_STRETCH_LIMIT (2%) off
    float speedMax = 0.0f;             // m/s
    float speedMean = 0.0f;            // mean over guides of each guide's mean point speed, m/s
    float deviationMean = 0.0f;        // mean over guides of each guide's mean distance from its rigid pose, metres
    bool hasColliders = false;
    float colliderDistance = 0.0f;     // smallest signed distance of a point to a collider's surface, metres
    // The evaluate pass: how far the first vertex of a drawn strand was from its root frame before it was put on it.
    std::uint32_t guideRoots = 0u;     // drawn strands that are simulated guides
    float guideRootError = 0.0f;       // metres
    std::uint32_t strandRoots = 0u;    // drawn strands interpolated from guides
    float strandRootError = 0.0f;
    std::uint32_t resetGuides = 0u;    // guides put back on the rest pose in that frame
    // Furthest a guide's root was, when the frame began, from the root frame the renderer calls the previous one: above
    // rounding when the frame did not follow the one the guide was last simulated in (a scene version bound twice, or
    // skipped).
    float rootShift = 0.0f;
    // Simulation LOD.
    float simLevel = 0.0f;    // sFrac in that frame; 0 with no guides: the instance was not simulated
    float targetLevel = 0.0f; // what its coverage (or the forced level, or the budget) asked for
    float coverage = 0.0f;    // the diameter of the groom's bounds on screen over the view's height; 0 when not known
    std::uint32_t levelCount = 0u; // the groom's guide levels
    std::array<std::uint32_t, HAIR_MAX_GUIDE_LEVELS> levelGuides{}; // guides simulated, by level; `guides` is their sum
    std::uint32_t simulatedPoints = 0u; // points of the simulated guides
    std::uint32_t evaluatedGuides = 0u; // guides evaluated from their parents
    std::uint32_t blendingGuides = 0u;  // simulated guides in a promotion or demotion blend
    std::uint32_t promotedGuides = 0u;  // guides that became simulated in that frame
    std::uint32_t demotedGuides = 0u;   // guides whose demotion ended in that frame
    // A guide's motion is the furthest any of its points moved in the frame relative to its root's own move. The
    // largest over the guides that changed state in that frame or the one before or are in a blend, and over the others.
    float transitionMotion = 0.0f;
    float steadyMotion = 0.0f;
};

struct HairSimStatus {
    HairSimSettings settings;
    std::uint64_t steps = 0u;          // fixed steps taken since the simulation was last enabled or reset
    double simulatedSeconds = 0.0;
    double droppedSeconds = 0.0;       // frame time not simulated because a frame hit maxStepsPerFrame
    std::uint64_t cappedFrames = 0u;   // frames that did
    std::uint32_t lastFrameSteps = 0u;
    std::uint32_t guidePoints = 0u;    // rows of guide state in use
    std::vector<HairSimMetrics> metrics; // the newest readback of each instance that has one
};

// The LOD controls. They are commands (hair.lod), not settings: a plugin cannot register a setting.
struct HairLodSettings {
    // A cluster draws all its strands while neighbouring strands are at least this many pixels apart, and a share in
    // proportion below that (HairLodDetailLevel).
    float errorPixels = 1.0f;
    float maxWidthScale = 8.0f; // wMax: the most a drawn strand is widened to make up for dropped ones
    float minDetail = 0.02f;    // lMin
    float forceDetail = 0.0f;   // > 0: every cluster takes this detail level; for testing
    float segmentPixels = 4.0f; // vertex LOD: the length on screen a drawn segment aims for
    // Keep the last selection (visible clusters, strand counts, vertex levels) while the camera moves. The kept strands
    // are still evaluated and rasterized every frame. A change to the active instances selects once more.
    bool freeze = false;
    bool operator==(const HairLodSettings&) const = default;
};

// What the debug overlay colours hair pixels by: HAIR_DEBUG_VIEW_* (HairRuntimeGPUTypes.h). Off draws nothing.
enum class HairDebugView : std::uint32_t {
    Off = HAIR_DEBUG_VIEW_OFF,
    Cluster = HAIR_DEBUG_VIEW_CLUSTER,
    Detail = HAIR_DEBUG_VIEW_DETAIL,
    Strand = HAIR_DEBUG_VIEW_STRAND,
    Rank = HAIR_DEBUG_VIEW_RANK,
    VertexLevel = HAIR_DEBUG_VIEW_VERTEX_LEVEL,
};

inline constexpr std::size_t kHairCounterHistoryFrames = 32u;

// The last CaptureDebugView request, or the last CaptureProbe request (its width is then the rows written).
struct HairDebugCapture {
    enum class State { None, Pending, Written, Failed };
    State state = State::None;
    std::string path;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::string error;
};

// The GPU counters of one frame, read back after it (HAIR_COUNTER_* in HairRuntimeGPUTypes.h).
struct HairGpuCounters {
    bool valid = false;
    std::uint64_t frame = 0u;                    // the frame they were read after
    std::uint32_t rootBindingDispatches = 0u;    // running; it starts at an arbitrary value: compare differences
    std::uint64_t cpuRootBindingDispatches = 0u; // HairStats::rootBindingDispatches as of that frame
    // Selection. While the LOD is frozen these are the counts of the frozen selection.
    std::uint32_t clustersTested = 0u;
    std::uint32_t clustersVisible = 0u;  // kept clusters: at most the capacity of the list
    std::uint32_t instancesVisible = 0u; // instances with a cluster inside the frustum
    std::uint32_t strandsSelected = 0u;
    std::uint32_t segmentsSelected = 0u;
    std::uint32_t poolVertices = 0u;     // pool rows in use: at most its capacity
    std::uint32_t overflowClusters = 0u; // visible clusters dropped because the pool or the list was full
    // Raster.
    std::uint32_t segmentsDrawn = 0u;  // segments that wrote at least one pixel
    std::uint32_t pixelsWritten = 0u;  // visibility writes: a pixel under several segments counts for each
    std::uint32_t pixelsOccluded = 0u; // covered pixels behind the opaque depth
    std::uint32_t segmentsCapped = 0u; // segments cut short at the per-segment pixel limit
    std::uint32_t pixelsResolved = 0u; // hair pixels the resolve wrote into the surface targets
    // The main camera position of that frame, and whether the frame ran the select pass (false: frozen).
    Float3 cameraPosition{};
    bool selected = false;
};

// Where a groom comes from: a file (.groom, .hair or .abc, by extension) or, with an empty path, the procedural
// sphere-cap generator. Everything but a .groom is built in memory with BuildGroom and default settings.
struct HairGroomRequest {
    std::string path;
    ProceduralGroomSettings procedural;
    // File units to metres for .hair and .abc. 0 picks 0.01 (centimetres, the UE groom schema's unit) for .abc and
    // 0.003 for .hair: the files name no unit, and at that scale the head under Cem Yuksel's models is 18 cm across.
    // Ignored for the other sources.
    float unitScale = 0.0f;
    // Whether a .hair or .abc file is Z-up and is rotated onto +Y up. Unset: .hair yes, .abc no.
    std::optional<bool> zUp;
    // Added to every position of a .hair or .abc file after the scale and the rotation, in metres: where the file's
    // origin goes in the rest space of the entity the groom will be attached to.
    Float3 offset{};
    // How a source that is not a .groom is built.
    GroomBuildSettings build;
};

// The placeholder material of an instance: the resolve writes it as an ordinary rough dielectric surface.
struct HairMaterial {
    Float3 baseColor{ 0.09f, 0.05f, 0.03f }; // linear; dark brown
    float roughness = 0.6f;                  // perceptual
};

enum class HairInstanceState {
    Pending,  // waiting for the next scene update to find its entity
    Attached, // bound to an entity; drawn while the renderer has that entity
    Failed,   // see HairInstanceInfo::error
};

enum class HairAttachMode {
    Rigid,   // the entity's model matrix only
    Bone,    // one named joint of the entity's skeleton
    PerRoot, // each root's own joints and weights (a groom built with a scalp mesh)
};

struct HairInstanceInfo {
    std::uint32_t id = 0u;
    std::uint32_t asset = 0u;
    std::string entity;
    std::string bone;
    HairInstanceState state = HairInstanceState::Pending;
    HairAttachMode mode = HairAttachMode::Rigid;
    HairMaterial material;
    bool active = false; // the root-binding pass ran for it in the last frame
    std::string error;
};

// The readback self-check: GPU root frames of a few roots per instance against the same arithmetic on the CPU.
struct HairSelfCheckStatus {
    bool enabled = false;
    std::uint32_t rootsPerInstance = 0u;
    std::uint32_t completedChecks = 0u; // readbacks compared since it was enabled
    // The last completed check.
    std::uint64_t frame = 0u;
    std::uint32_t instances = 0u;        // compared
    std::uint32_t skinnedInstances = 0u; // of those, following a skeleton
    // Not compared: their rows were not written in the checked frame, or the frame's per-object rows are not on the CPU.
    std::uint32_t staleInstances = 0u;
    std::uint32_t roots = 0u;
    std::uint32_t previousRoots = 0u; // roots whose previous frame was compared too
    float positionError = 0.0f;         // largest distance, metres
    float directionError = 0.0f;        // largest normal or tangent difference (length of the difference vector)
    float previousPositionError = 0.0f; // the same for the previous-frame rows
    float previousDirectionError = 0.0f;
    // How far the GPU's current and previous positions are apart, at most: that the instances moved between the frames.
    float motion = 0.0f;        // instances following only their entity
    float skinnedMotion = 0.0f; // instances following a skeleton
    // Over every completed check since it was enabled, current and previous rows together.
    float worstPositionError = 0.0f;
    float worstDirectionError = 0.0f;
    float largestMotion = 0.0f;
    float largestSkinnedMotion = 0.0f;
    std::uint32_t totalStaleInstances = 0u;
};

struct HairStats {
    std::uint32_t assets = 0u;
    std::uint32_t instances = 0u;
    std::uint32_t attachedInstances = 0u;
    std::uint32_t activeInstances = 0u;
    std::uint32_t failedInstances = 0u;
    // Rows used in the pooled buffers.
    std::uint32_t clusters = 0u;
    std::uint32_t strands = 0u;
    std::uint32_t guides = 0u;
    std::uint32_t points = 0u;
    std::uint32_t rootFrames = 0u; // strands summed over instances
    std::uint32_t guidePoints = 0u; // guide state rows summed over instances
    HairCapacities capacities;
    bool buffersCreated = false;
    std::uint64_t bufferBytes = 0u;           // GPU memory of the pooled buffers
    std::uint64_t assetBytesUploaded = 0u;    // groom arrays, once per asset
    std::uint64_t instanceBytesUploaded = 0u; // instance rows, whenever one changes
    std::uint64_t pendingUploadAssets = 0u;
    // The root-binding pass, counted on the CPU: frames it was updated in, and the dispatches those frames asked for.
    std::uint64_t frames = 0u;
    std::uint64_t rootBindingDispatches = 0u;
    // The newest readback of the GPU counters. A Stats call asks for one more readback. With continuous counters on,
    // every frame is read back, and gpuCounterHistory holds the last kHairCounterHistoryFrames of them, oldest first: a
    // readback arrives a few frames after its frame, so a caller polling once a frame finds every frame in it.
    HairGpuCounters gpuCounters;
    bool continuousCounters = false;
    std::vector<HairGpuCounters> gpuCounterHistory;
    HairLodSettings lod;
    HairDebugView debugView = HairDebugView::Off;
    std::uint32_t visibilityWidth = 0u; // the hair visibility target
    std::uint32_t visibilityHeight = 0u;
    HairDebugCapture debugCapture;
    HairDebugCapture probe;
    std::uint32_t dumpsPending = 0u; // DumpBuffer requests not yet written
    std::uint32_t dumpsWritten = 0u;
    std::uint32_t dumpsFailed = 0u;
    HairSelfCheckStatus selfCheck;
    HairSimStatus sim;
    std::vector<HairInstanceInfo> instanceInfo;
};

// The per-object buffer a frame binds as Builtin::PerObjectBuffer (the frame's published version): how many rows it
// has, and their bytes on the CPU when the renderer keeps them.
struct HairFrameObjects {
    std::uint64_t rowCount = 0u;
    std::uint32_t rowStride = 0u;
    std::shared_ptr<const org::services::BufferImage> image;
};

// One instance a frame works on: the root-binding pass dispatches over its roots, the select pass over its clusters.
struct HairInstanceDispatch {
    std::uint32_t instanceIndex = 0u;
    std::uint32_t rootCount = 0u;
    std::uint32_t clusterCount = 0u;
    std::uint32_t guideCount = 0u;
    // Simulation LOD: whether the instance is simulated this frame, and its guide levels (level j owns ranks
    // [guideLevelOffsets[j], guideLevelOffsets[j + 1])).
    std::uint32_t simulated = 0u;
    // The guides the simulate pass looks at this frame: ranks [0, simulateGuides), the levels that can have a guide the
    // solver owns. Every guide in the frame the instance starts or is reset.
    std::uint32_t simulateGuides = 0u;
    std::uint32_t guideLevelCount = 0u;
    std::array<std::uint32_t, HAIR_MAX_GUIDE_LEVELS + 1u> guideLevelOffsets{};
    bool operator==(const HairInstanceDispatch&) const = default;
};

// What the hair passes record for a frame, decided by UpdateFrame.
struct HairFramePlan {
    std::vector<HairInstanceDispatch> instances; // one per instance whose entity the frame draws
    bool select = false;               // run the select pass (false while the LOD is frozen on a valid selection)
    bool simulate = false;             // run the simulate and guide-level passes over the simulated instances' guides
    // Rows of the frame's HairSimGroup list. The simulate pass is one dispatch over the first simulateGroups rows; the
    // guide-level pass one dispatch per guide level over rows [levelGroupFirst[j], levelGroupFirst[j + 1]), which hold
    // level j's guides of every simulated instance.
    std::uint32_t simulateGroups = 0u;
    std::array<std::uint32_t, HAIR_MAX_GUIDE_LEVELS + 1u> levelGroupFirst{};
    std::uint32_t evaluateGroups = 0u; // the most clusters the visible list can hold this frame
    std::uint32_t rasterRows = 0u;     // the most pool rows this frame can use
    HairDebugView debugView = HairDebugView::Off;
    bool probe = false; // run the probe pass this frame (CaptureProbe)
    bool operator==(const HairFramePlan&) const = default;
};

// What a frame's update reads from the renderer.
struct HairFrameInputs {
    std::uint64_t frameNumber = 0u;
    HairFrameObjects objects;
    std::uint32_t renderWidth = 0u; // the size of the opaque visibility buffer: the hair visibility target takes it
    std::uint32_t renderHeight = 0u;
    Float3 cameraPosition{}; // of the main view, for HairGpuCounters
    float deltaSeconds = 0.0f; // the frame's time step: what the simulation's accumulator takes
    float projectionScaleY = 0.0f; // the main view's projection[1][1]: a length over its distance, times this, is its share of half the view's height
};

// How the runtime writes a buffer: the root-binding pass's upload, on the render thread.
using HairUpload = std::function<void(const std::shared_ptr<org::Buffer>& buffer, std::size_t byteOffset,
    const void* data, std::size_t byteCount)>;

// Groom assets and their instances. An asset is the built arrays of one groom, uploaded once into the pooled buffers.
// An instance attaches an asset to a scene entity (found by its Components::Name) and owns a range of root frames.
// Nothing is ever freed one at a time: Clear drops everything.
//
// Threads: Initialize and Shutdown on the thread that runs Renderer::Update (the render thread in the demo).
// UpdateFrame on the render thread, from the root-binding pass's Update. Everything else from any thread.
class HairRuntime {
public:
    explicit HairRuntime(HairCapacities capacities = {});
    ~HairRuntime();
    HairRuntime(const HairRuntime&) = delete;
    HairRuntime& operator=(const HairRuntime&) = delete;

    // Registers the scene hook that finds the entities of pending instances. Call once, after Renderer::Initialize.
    void Initialize(Renderer& renderer);
    // Drops the hook and the GPU buffers. Call before Renderer::Cleanup.
    void Shutdown();

    // Loads or generates a groom, builds it when the source is not a .groom, and reserves its rows. The upload happens
    // in the next frame. Returns the asset id, or nothing with `error` set.
    std::optional<std::uint32_t> LoadGroom(const HairGroomRequest& request, std::string& error);
    // Attaches an asset to the entity named `entityName`: the entity itself when it is renderable, otherwise its
    // first renderable child (a model's root names its first part). With `boneName` the roots follow that joint of
    // the entity's skeleton. Without it they follow each root's own skin binding when the groom has one and the
    // entity is skinned, and the entity's transform otherwise. Returns the instance id, or nothing with `error` set;
    // a missing entity or bone is reported later, as the instance's Failed state.
    std::optional<std::uint32_t> AttachGroom(std::uint32_t asset, std::string entityName, std::string boneName,
        const HairMaterial& material, std::string& error);
    // Drops every instance and asset. The GPU buffers stay.
    void Clear();

    // Off by default. While on, every few frames `rootsPerInstance` roots of each active instance are read back and
    // compared (HairSelfCheckStatus); each comparison is logged.
    void SetSelfCheck(bool enabled, std::uint32_t rootsPerInstance = 8u);
    HairStats Stats();

    // The LOD controls take effect in the next frame. Values out of range are clamped.
    void SetLodSettings(const HairLodSettings& settings);
    HairLodSettings LodSettings() const;
    void SetDebugView(HairDebugView view);
    // The simulation controls take effect in the next frame; values out of range are clamped. Turning it on, and
    // ResetSimulation, put every guide back on its rest pose at rest.
    void SetSimSettings(const HairSimSettings& settings);
    HairSimSettings SimSettings() const;
    void ResetSimulation();
    // Simulation LOD, for testing: one instance takes this sFrac at once, whatever its coverage and
    // HairSimSettings::lodForceLevel. A negative level gives it back to them. False with `error` set for no such instance.
    bool SetInstanceSimLevel(std::uint32_t instance, float level, std::string& error);
    // Replaces an instance's colliders (at most HAIR_MAX_COLLIDERS_PER_INSTANCE). False with `error` set otherwise.
    bool SetColliders(std::uint32_t instance, const std::vector<HairCollider>& colliders, std::string& error);
    // The simulation's counters and the newest metrics read back. A call asks for one more readback, like Stats.
    HairSimStatus SimStatus();
    // Writes a presented frame (the presentation colour: the frame with the debug view and the menu over it) to a
    // 32-bit .bmp file: a screenshot that needs no visible window. It is read back a frame or two after the request,
    // so hold the view still; HairStats::debugCapture says when the file is written. False with `error` set when a
    // capture is already pending.
    bool CaptureDebugView(const std::string& path, std::string& error);
    // Writes what the next frame's targets hold at hair pixels (depths, motion vector, surface normal and roughness, for
    // one pixel in HAIR_PROBE_STRIDE each way) to a binary file: the probe buffer's header and rows as
    // HairRuntimeGPUTypes.h lays them out. HairStats::probe says when it is written. False with `error` set when one is
    // already pending.
    bool CaptureProbe(const std::string& path, std::string& error);
    // Writes one of the plugin's buffers as the next frame leaves it to a binary file: the frame number (8 bytes), then
    // the first `rows` rows (0: all of them). `buffer` is "pool_vertices" (HairPoolVertex), "visible_clusters"
    // (HairVisibleCluster) or "guide_points" (HairGuidePoint). Several requests made before a frame are read back after
    // that same frame, with a CaptureProbe made then too. For checking the passes from outside; HairStats::dumps counts
    // the files written and failed. False with `error` set for an unknown buffer.
    bool DumpBuffer(const std::string& buffer, const std::string& path, std::uint32_t rows, std::string& error);
    // Read the GPU counters back after every frame, not only after a Stats call, and keep a short history of them.
    void SetContinuousCounters(bool enabled);

    // For HairExtension's passes.
    // Null until the first groom is loaded and a frame has run. Replaced when the hair visibility target is made again.
    std::shared_ptr<const HairGpuBuffers> GpuBuffers() const;
    // The frame's CPU work: makes the buffers at the first groom and the visibility target at the frame's resolution,
    // uploads new grooms, changed instance rows and the frame state, resets the frame's GPU counters, finds each
    // instance's rows for this frame and decides the frame's plan.
    void UpdateFrame(const HairFrameInputs& inputs, const HairUpload& upload);
    // What the last UpdateFrame decided.
    HairFramePlan FramePlan() const;
    // For the debug overlay's pass, which can name the frame's presentation colour. True once per CaptureDebugView,
    // with its path; the pass answers with ReadDebugCapture and the texture, or null when it has none.
    bool TakeDebugCaptureRequest(std::string& path);
    void ReadDebugCapture(const std::string& path, org::Resource* presentationColor);

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace br::hair
