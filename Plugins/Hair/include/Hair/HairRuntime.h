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
#include <string_view>
#include <vector>

class Renderer;
class VirtualShadowInvalidationQueue; // BasicRenderer/Extensions/VirtualShadowCasterProvider.h
namespace org { class Buffer; }
namespace org { class Resource; }
namespace org::services { class BufferImage; }

namespace br::hair {

struct HairGpuBuffers; // src/HairGpuBuffers.h

// Rows of the pooled GPU buffers. They are made once, at these sizes, when the first groom is loaded, and never
// resized. A groom that does not fit the asset buffers, or an instance past `instances`, is refused. At the defaults
// they take about 475 MB.
struct HairCapacities {
    std::uint32_t clusters = 16384u;   // HairClusterHeader (80 B) + HairClusterAggregate (208 B)
    std::uint32_t strands = 262144u;   // HairStrandRecord (48 B) + HairRootBinding (80 B)
    std::uint32_t guides = 65536u;     // HairGuideBinding (32 B) + guide strand index (4 B)
    std::uint32_t points = 8388608u;   // HairPackedPoint (8 B)
    std::uint32_t instances = 1024u;   // HairInstanceGPU (96 B)
    // Per frame, for the main view. A visible cluster that does not fit either is dropped and counted (HairGpuCounters).
    std::uint32_t visibleClusters = HAIR_MAX_VISIBLE_CLUSTERS; // HairVisibleCluster (32 B); the visibility key's limit
    // HairPoolVertex (32 B) + segment id (4 B) + lit colour (8 B, the layered path's): the drawn vertices of a frame.
    std::uint32_t poolVertices = 4194304u;
    // HairGuidePoint (80 B): guide state, lent to the instances that are simulated or keep a shape (HairGuideRowStats).
    // An instance that cannot get rows is not simulated: its strands follow their roots rigidly, and it is counted.
    std::uint32_t guidePoints = 2097152u;
    std::uint32_t simGroups = 131072u;     // HairSimGroup (16 B): a frame's simulation thread groups, 64 guides each
    // uint (4 B): the voxels of the frame's density volumes, shared out every frame among the instances a view shows
    // (HairVolumeSettings). When they do not go round, every volume is made coarser.
    std::uint32_t volumeVoxels = 2097152u;
    // Per frame, for the shadow view (HairShadowSettings): the clusters it keeps (HairVisibleCluster, 32 B, and a
    // clipmap mask, 4 B) and their evaluated strands (HairPoolVertex, 32 B, and a segment id, 4 B). What does not fit
    // is dropped for the frame and counted, as in the main view.
    std::uint32_t shadowClusters = HAIR_MAX_VISIBLE_CLUSTERS;
    std::uint32_t shadowPoolVertices = 1048576u;
    // The culled rasterizer (HairRasterMode::Culled). uint (4 B) each: the first pass's items (a pool row and one of its
    // chunks; what does not fit is drawn by the late pass), and the state of every cluster of every instance (16 B; an
    // instance past them has none, and its clusters always count as in front). Each pool row also has a late mask (uint).
    std::uint32_t rasterItems = 8388608u;
    std::uint32_t clusterStamps = 262144u;
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
    // Simulation follows the view. An instance no view shows any part of stops being simulated once it has been out of
    // view this long (a groom at the edge of the view does not start and stop), and has had lodSettleSeconds of
    // simulation since it last started from its rigid pose. Its guide state is kept, and it goes on from that shape when
    // a view shows it again.
    float lodViewGraceSeconds = 0.25f;
    // An instance that starts from its rigid pose is simulated this long whether or not a view shows it, at level 0
    // alone while none does: the hair settles once, unseen, so that a view that turns to it later finds it settled. 0:
    // an instance out of view is never started.
    float lodSettleSeconds = 1.5f;
    // Guide rows (HairGuideRowStats). The most rows of guide state lent out at once; 0: all of HairCapacities::guidePoints.
    std::uint32_t guidePointBudget = 0u;
    // > 0: an instance that stopped with its shape kept gives its rows back once it has not been simulated for this long
    // (simulated seconds), and then starts from its rigid pose when a view shows it. 0: it keeps them until an instance
    // in view needs rows, there are no free ones and it is the smallest on screen of those out of view.
    float guideKeepSeconds = 0.0f;
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
    std::uint32_t resumedGuides = 0u;  // guides that started in that frame from the shape the instance was left in
    // Guides that went on from rows not written for this instance in the frame before: 0 unless the lending of guide
    // rows and the passes disagree.
    std::uint32_t staleGuides = 0u;
    // Furthest a guide's root was, when the frame began, from the root frame the renderer calls the previous one: above
    // rounding when the frame did not follow the one the guide was last simulated in (a scene version bound twice, or
    // skipped).
    float rootShift = 0.0f;
    // Simulation LOD.
    float simLevel = 0.0f;    // sFrac in that frame; 0 with no guides: the instance was not simulated
    float targetLevel = 0.0f; // what its coverage (or the forced level, or the budget) asked for
    float coverage = 0.0f;    // the diameter of the groom's bounds on screen over the view's height; 0 when not known
    // How much of that is inside a view (HairRuntime.cpp's OnScreenExtent): 0 when no view shows any part of the groom,
    // and very large when the camera is inside its bounds or the bounds are not known on the CPU.
    float viewExtent = 0.0f;
    bool inView = false;      // viewExtent is above 0
    bool holdsRows = false;   // it had guide rows in that frame: it was simulated, or keeps a shape
    bool starved = false;     // a view shows it and its size asks for simulation, but it could not get guide rows: it follows rigidly
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
    std::uint32_t guidePoints = 0u;    // rows of guide state lent out
    std::vector<HairSimMetrics> metrics; // the newest readback of each instance that has one
};

// Guide state is lent to instances out of one buffer (HairCapacities::guidePoints rows), a contiguous range each, as
// many rows as the groom has guide points. An instance takes rows when it starts being simulated and holds them while it
// is simulated or keeps the shape it stopped with. It gives them back:
//   - at once, when it stops with nothing to keep (its level went to 0 and it blended back onto the rigid pose, the
//     simulation was turned off, Clear), and when it has a kept shape but is now too small on screen to be simulated;
//   - after HairSimSettings::guideKeepSeconds without being simulated, when that is set;
//   - when an instance in view needs rows and none are free (or HairSimSettings::guidePointBudget is reached): from
//     the instances no view shows, kept shapes and instances simulated out of view alike (settling unseen, or in the
//     grace after leaving), the smallest on screen first.
// An instance that loses rows that way (an eviction) starts from its rigid pose when a view next shows it. An instance
// in view is never evicted for another, so when instances in view need more rows than there are, the ones that asked
// last follow rigidly until rows come free (starved). An instance that would settle out of view takes free rows only.
// Ranges are first-fit and merged when freed, never moved: a request can fail on fragmentation with enough rows free
// in total, and then evicts as if there were not.
struct HairGuideRowStats {
    std::uint32_t capacityRows = 0u;
    std::uint32_t budgetRows = 0u;        // the smaller of the capacity and HairSimSettings::guidePointBudget
    std::uint32_t rowsInUse = 0u;         // lent out now
    std::uint32_t rowsActive = 0u;        // of those, to instances simulated in the last frame
    std::uint32_t highWaterRows = 0u;     // the most lent out at once, since the buffers were made
    std::uint32_t largestFreeRows = 0u;   // the largest range a request can take now
    std::uint32_t holders = 0u;           // instances holding rows
    std::uint32_t activeHolders = 0u;     // simulated in the last frame
    std::uint32_t starvedInstances = 0u;  // in view, to be simulated, without rows: following rigidly
    std::uint32_t unsettledInstances = 0u; // out of view, would settle unseen, no free rows for it
    std::uint64_t allocations = 0u;       // ranges lent
    std::uint64_t releases = 0u;          // given back with nothing lost
    std::uint64_t evictions = 0u;         // taken back with a kept shape or a running simulation lost
    std::uint64_t evictionsForRows = 0u;  // ... because an instance in view needed rows, or the budget fell
    std::uint64_t evictionsByAge = 0u;    // ... by guideKeepSeconds
    std::uint64_t allocationFailures = 0u; // times an instance in view could not get rows (once per time it starts to starve)
};

// One pooled buffer's use: rows of rowBytes each.
struct HairBufferUse {
    std::string name;
    std::uint32_t rowBytes = 0u;
    std::uint64_t capacityRows = 0u;
    std::uint64_t usedRows = 0u;      // now; for the buffers a frame fills on the GPU, in the newest frame read back
    std::uint64_t highWaterRows = 0u; // the most since the buffers were made; for those buffers, over the frames read back
};

// How the rasterizer treats a strand thinner than its one-pixel footprint: HAIR_COVERAGE_* (HairRuntimeGPUTypes.h).
enum class HairCoverageMode : std::uint32_t {
    Off = HAIR_COVERAGE_OFF,                 // every footprint pixel is written: a strand is at least a pixel wide and opaque
    White = HAIR_COVERAGE_WHITE,             // written with the probability of the strand's coverage: a hash per pixel, strand and frame
    Interleaved = HAIR_COVERAGE_INTERLEAVED, // the same from interleaved gradient noise, shifted per frame and per strand
};

// The LOD controls. They are commands (hair.lod), not settings: a plugin cannot register a setting.
struct HairLodSettings {
    // A cluster draws all its strands while neighbouring strands are at least this many pixels apart, and a share in
    // proportion below that (HairLodDetailLevel).
    float errorPixels = 1.0f;
    // wMax: the most a drawn strand is widened to make up for dropped ones. At HAIR_MAX_STRANDS_PER_CLUSTER a cluster
    // down to its last strand still carries all its coverage.
    float maxWidthScale = 64.0f;
    float minDetail = 0.02f;    // lMin
    float forceDetail = 0.0f;   // > 0: every cluster takes this detail level; for testing
    float segmentPixels = 4.0f; // vertex LOD: the length on screen a drawn segment aims for
    // Keep the last selection (visible clusters, strand counts, vertex levels) while the camera moves. The kept strands
    // are still evaluated and rasterized every frame. A change to the active instances selects once more.
    bool freeze = false;
    HairCoverageMode coverage = HairCoverageMode::White;
    // Add every segment's coverage into the coverage target. Off by default: it costs one more atomic per footprint
    // pixel, which doubles the rasterizer's time with a dense groom filling the view. The `coverage` debug view turns it
    // on while it is shown, and the counters pixelsCovered and coverageSum do not need it.
    bool accumulateCoverage = false;
    // The detail level is (strand spacing on screen / errorPixels) to this power, in [1, 3]. 1: drawn strands fall as one
    // over the distance. 2: as its square, like the area a groom covers; the width scale then has to make up the coverage.
    float detailExponent = 1.0f;
    bool operator==(const HairLodSettings&) const = default;
};

// How hair gets into the frame: HAIR_PATH_* (HairRuntimeGPUTypes.h).
enum class HairRenderPath : std::uint32_t {
    Surface = HAIR_PATH_SURFACE, // the nearest accepted segment of a pixel becomes an opaque surface the deferred pass lights
    Layered = HAIR_PATH_LAYERED, // three layers and the summed coverage, lit per strand vertex and blended over the frame
};

// Where the shade pass takes a vertex's lights from: HAIR_LIGHTS_*.
enum class HairLightLookup : std::uint32_t {
    List = HAIR_LIGHTS_LIST, // every active light, by its range: right in any recipe
    Grid = HAIR_LIGHTS_GRID, // the light grid's forward runs: right only where the recipe builds them (ClusterLodAlphaTechnique)
};

// How the layered path's rasterizer works (hair.render `raster`).
enum class HairRasterMode : std::uint32_t {
    Direct = 0u, // hairRaster.hlsl: one thread per pool row draws its whole segment; every strand of the pool is drawn
    Culled = 1u, // hairRasterCulled.hlsl: chunks of segments, hair behind hair skipped, in two passes
    // The culled one in a frame where a groom in view has strands at least autoWidthPixels wide on screen (there hair
    // hides hair, and it is several times faster), the direct one otherwise (there it is the cheaper of the two).
    Auto = 2u,
};

// Which chunks the culled rasterizer's first pass draws (hair.render `first_pass`); the late pass tests the others
// against the hidden depths the first pass left, and draws those that are not hidden.
enum class HairFirstPass : std::uint32_t {
    All = 0u,      // every chunk; there is no late pass
    Previous = 1u, // the chunks the frame before did not hide, of clusters that drew a pixel lately
    Prefix = 2u,   // the first eighth of each cluster's drawn strands
    Both = 3u,     // either
};

// The rendering controls (hair.render). The path, and the layered path's own controls.
struct HairRenderSettings {
    HairRenderPath path = HairRenderPath::Surface;
    // A pixel whose composited hair alpha is at least this takes hair's depth and motion vector (its nearest layer's);
    // below it the pixel keeps those of what is behind the hair.
    float depthAlpha = 0.5f;
    // The rasterizer stops adding to a pixel's coverage once it holds this many pixels' worth: the alpha that sum gives
    // is then 1 to within exp(-(this - 3)). 0: every covered pixel adds, always.
    float coverageSaturation = 0.0f;
    // Give the upscaler a reactive mask (Extensions/UpscalerReactiveMask.h): where a pixel's hair and what is behind it
    // move differently, by reactiveStrength times the share of the pixel that does not move with the pixel's motion
    // vector, reached in full when the two motions are reactivePixels apart.
    bool reactiveMask = true;
    float reactiveStrength = 1.0f;
    float reactivePixels = 1.0f;
    float ambient = 1.0f; // scale of the environment term
    HairLightLookup lights = HairLightLookup::List;
    // The rasterizer (hairRasterCulled.hlsl has the scheme).
    HairRasterMode raster = HairRasterMode::Auto;
    float autoWidthPixels = 0.75f; // Auto: the strand width on screen, at a groom's middle, from which the culled rasterizer runs
    bool opaqueCoverage = true;    // a strand that covers its pixel whole saturates the pixel's coverage
    bool occludeOpaque = true;     // ... and hides the hair behind it
    bool occludeSaturated = true;  // a pixel whose coverage in front of its split depth saturates hides the hair behind that
    float saturation = 10.0f;      // that saturation, in pixels' worth of coverage
    HairFirstPass firstPass = HairFirstPass::Previous;
    bool rasterProfile = false;    // count the rasterizer's loop iterations by wave (lane occupancy); costs time
    float hintDecay = 0.8f;        // how long the first pass's guess remembers a tile's larger hidden depths: the share kept a frame; 0: the last frame's alone
    bool latePass = true;          // false, for measurement only: the late pass draws nothing (hair can be missing)
    // The share of its strands a cluster keeps in the pool while no pixel shows it (behind the head, under hair that
    // hides it); 1: all. The share moves by detailRateUp or detailRateDown a frame.
    float hiddenDetail = 0.25f;
    float detailRateUp = 0.25f;
    float detailRateDown = 0.02f;
    bool operator==(const HairRenderSettings&) const = default;
};

// The density volumes (hair.volume): the layered path's self-shadowing and multiple scattering. Every frame each
// instance a view shows gets a box of cubic voxels around its groom, sized from the groom's size on screen, which the
// voxelize pass fills from the frame's strand pool and the shade pass marches toward every light (HairVolumeGPU).
struct HairVolumeSettings {
    // Off: no volume is built and hair is lit as without one, by every light in full.
    bool enabled = true;
    // The light the hair in the way scatters forward, and the light the hair around a fibre scatters back (dual
    // scattering, HairScatterShared.h). Off: the fibres in the way only shadow.
    bool dualScattering = true;
    // The size of a voxel on screen, in pixels, at the groom's distance: a volume's resolution follows its groom's size
    // on screen.
    float voxelPixels = 8.0f;
    // The most voxels along a volume's longest side: near the camera a voxel is larger on screen than voxelPixels.
    std::uint32_t maxResolution = 96u;
    // The most voxels over all the frame's volumes; 0: HairCapacities::volumeVoxels. Over it every volume's voxels are
    // made larger by one factor until they fit; if the coarsest volumes (2 voxels along the longest side) still do not fit, the
    // instances smallest on screen get none and are lit as without one.
    std::uint32_t voxelBudget = 0u;
    std::uint32_t steps = 8u;            // samples of a march toward a light
    std::uint32_t environmentSteps = 4u; // ... and toward one of the environment's four directions
    float bias = 1.0f;                   // voxels a march skips at its start
    float densityFactor = 0.7f;          // Zinke et al.'s d_f and d_b
    // The box is the groom's rest bounds grown on every side by this share of their longest side, for hair the
    // simulation moves and for strand width. Hair outside it is not in the volume.
    float margin = 0.1f;
    // Voxels of the opaque shell marked under every root, which stands for the head the hair grows on; 0: none.
    std::uint32_t scalpDepth = 3u;
    bool trilinear = true;       // a deposit is shared by the 8 voxels around it; off: the nearest takes it
    bool jitterPerFrame = false; // a march's offset changes every frame; off: it is the vertex's own, always
    bool operator==(const HairVolumeSettings&) const = default;
};

// Shadow casting (hair.shadows): hair drawn into the sun's virtual shadow map, on the layered path. The shadow view
// keeps the clusters that lie over a shadow page drawn this frame, evaluates them at a level of detail set by the shadow
// texel, and rasterizes them with their coverage dithered (hairShadow.hlsl).
enum class HairShadowCoverage : std::uint32_t {
    Temporal = 0u, // a texel is written with the probability of the strand's coverage, by a pattern new every frame
    Fixed = 1u,    // ... by a pattern that is the texel's and the strand's own, the same every frame
    Opaque = 2u,   // every footprint texel is written: a strand shadows as if a texel wide
};

struct HairShadowSettings {
    // Off: hair is in no shadow map, and is lit exactly as without this.
    bool enabled = true;
    // A shadow texel counts as this many when the shadow view picks a cluster's level of detail: at 2 it draws the
    // strands and vertices the main view would draw at twice the distance.
    float detailScale = 2.0f;
    float forceDetail = 0.0f; // above 0: every cluster takes this detail level in the shadow view. For testing.
    HairShadowCoverage coverage = HairShadowCoverage::Temporal;
    // Metres an opaque occluder inside a groom's box must be nearer the sun than a hair vertex to shadow it, on top of
    // two shadow texels.
    float receiverBias = 0.005f;
    // Within this distance of the camera the shadow map redraws deformable casters every frame (its dynamic layer),
    // hair among them; beyond it hair's shadow is cached and redrawn where a groom moved.
    float dynamicRadius = 16.0f;
    bool operator==(const HairShadowSettings&) const = default;
};

// Shadow casting in the last frame, on the CPU. The GPU's counts are HairGpuCounters'.
struct HairShadowStats {
    bool active = false;                  // hair was drawn into the shadow map (enabled, the layered path, a shadow map)
    std::uint32_t instances = 0u;         // instances the shadow view was given
    std::uint32_t movedInstances = 0u;    // ... of which moved, or were simulated: their pages are invalidated
    std::uint32_t invalidations = 0u;     // bounds enqueued for the shadow map this frame
    std::uint64_t totalInvalidations = 0u;
    std::uint64_t droppedInvalidations = 0u; // the queue was full, or there was none
};

// The density volumes of the last frame.
struct HairVolumeStats {
    std::uint32_t volumes = 0u;         // instances that got one
    std::uint32_t droppedVolumes = 0u;  // instances in view that got none: the budget did not hold the coarsest
    std::uint32_t unknownInstances = 0u; // active instances whose place the CPU did not know (no matrices): none either
    std::uint32_t voxels = 0u;          // voxels in use
    std::uint32_t capacityVoxels = 0u;
    std::uint32_t budgetVoxels = 0u;
    std::uint32_t highWaterVoxels = 0u; // the most in use in a frame
    float coarsening = 1.0f;            // the factor every voxel was enlarged by to fit the budget; 1: none
    std::uint32_t largestResolution = 0u; // the most voxels along a side of any volume
    float smallestVoxel = 0.0f;         // metres
    float largestVoxel = 0.0f;
};

// What the debug overlay colours hair pixels by: HAIR_DEBUG_VIEW_* (HairRuntimeGPUTypes.h). Off draws nothing.
enum class HairDebugView : std::uint32_t {
    Off = HAIR_DEBUG_VIEW_OFF,
    Cluster = HAIR_DEBUG_VIEW_CLUSTER,
    Detail = HAIR_DEBUG_VIEW_DETAIL,
    Strand = HAIR_DEBUG_VIEW_STRAND,
    Rank = HAIR_DEBUG_VIEW_RANK,
    VertexLevel = HAIR_DEBUG_VIEW_VERTEX_LEVEL,
    Coverage = HAIR_DEBUG_VIEW_COVERAGE,
    Layers = HAIR_DEBUG_VIEW_LAYERS, // the layered path's
    Alpha = HAIR_DEBUG_VIEW_ALPHA,   // the layered path's
    Reactive = HAIR_DEBUG_VIEW_REACTIVE, // the layered path's
    // The layered path's, drawn by the shade pass: hair lit by one part of its response alone. The overlay draws nothing.
    LobeR = HAIR_DEBUG_VIEW_LOBE_R,
    LobeTT = HAIR_DEBUG_VIEW_LOBE_TT,
    LobeTRT = HAIR_DEBUG_VIEW_LOBE_TRT,
    Environment = HAIR_DEBUG_VIEW_ENVIRONMENT,
    Direct = HAIR_DEBUG_VIEW_DIRECT,
    // The density volumes'. Density is drawn by the overlay over the whole frame; the others by the shade pass.
    Density = HAIR_DEBUG_VIEW_DENSITY,
    Transmittance = HAIR_DEBUG_VIEW_TRANSMITTANCE,
    FibreCount = HAIR_DEBUG_VIEW_FIBRE_COUNT,
    ScatterGlobal = HAIR_DEBUG_VIEW_SCATTER_GLOBAL,
    ScatterLocal = HAIR_DEBUG_VIEW_SCATTER_LOCAL,
    VertexDensity = HAIR_DEBUG_VIEW_VERTEX_DENSITY,
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
    std::uint64_t frame = 0u;                 // the frame they were read after
    std::uint32_t evaluateDispatches = 0u;    // running; it starts at an arbitrary value: compare differences
    std::uint64_t cpuEvaluateDispatches = 0u; // HairStats::evaluateDispatches as of that frame
    // Drawn strands of a simulated instance that read guide rows not written in that frame for that instance, or whose
    // instance had no rows: 0 unless the lending of guide rows and the passes disagree.
    std::uint32_t staleGuidesDrawn = 0u;
    std::uint32_t unseenStrandsDrawn = 0u; // drawn strands of instances not simulated because no view showed them: 0 likewise
    // Selection. While the LOD is frozen these are the counts of the frozen selection.
    std::uint32_t clustersTested = 0u;
    std::uint32_t clustersVisible = 0u;  // kept clusters: at most the capacity of the list
    std::uint32_t instancesVisible = 0u; // instances with a cluster inside the frustum
    std::uint32_t strandsSelected = 0u;
    std::uint32_t segmentsSelected = 0u;
    std::uint32_t poolVertices = 0u;     // pool rows in use: at most its capacity
    std::uint32_t overflowClusters = 0u; // visible clusters dropped because the pool or the list was full
    // Raster.
    std::uint32_t pixelsCovered = 0u;  // footprint pixels in front of the opaque surface, counted for each segment
    float coverageSum = 0.0f;          // their coverage, summed: the hair's true projected area in pixels, overlaps counted
    std::uint32_t segmentsDrawn = 0u;  // segments that wrote at least one pixel
    std::uint32_t pixelsWritten = 0u;  // visibility writes: a pixel under several segments counts for each
    std::uint32_t pixelsOccluded = 0u; // covered pixels behind the opaque depth
    std::uint32_t segmentsCapped = 0u; // segments cut short at the per-segment pixel limit
    std::uint32_t pixelsResolved = 0u; // hair pixels the resolve wrote into the surface targets
    // The layered path.
    std::uint32_t layerRejected = 0u;   // raster: covered pixels behind their pixel's third layer (no layer write)
    std::uint32_t coverageSkipped = 0u; // raster: covered pixels whose coverage add was skipped at the saturation
    std::array<std::uint32_t, HAIR_LAYER_COUNT> layerPixels{}; // composite: pixels holding exactly 1, 2 and 3 layers
    std::uint32_t remainderPixels = 0u; // composite: three-layer pixels with coverage beyond the third layer
    float remainderAlpha = 0.0f;        // composite: what the remainder term added to the hair alpha, summed over pixels
    float alphaSum = 0.0f;              // composite: the hair alpha, summed over pixels
    std::uint32_t depthPixels = 0u;     // composite: pixels that took hair's depth and motion vector
    std::uint32_t shadeList = 0u;       // shade list: pool vertices the rasterizer marked, gathered for the shade pass
    std::uint32_t shadedVertices = 0u;  // shade: pool vertices lit
    // Shade: the sun's shadow map lookups, and those that read a coarser clipmap than the vertex asks for, found no
    // page (lit), or read a texel nothing was drawn into (lit).
    std::uint32_t shadowLookups = 0u;
    std::uint32_t shadowCoarser = 0u;
    std::uint32_t shadowMissing = 0u;
    std::uint32_t shadowCleared = 0u;
    // The density volumes.
    std::uint32_t volumeSegments = 0u; // voxelize: pool segments deposited into a volume
    std::uint32_t volumeDeposits = 0u; // voxelize: the voxel adds they made
    std::uint32_t volumeOutside = 0u;  // voxelize: segments of an instance with a volume whose middle is outside its box
    // The culled rasterizer.
    std::uint32_t rasterItems = 0u;         // chunks of the first pass (it can pass the item list's capacity)
    std::uint32_t rasterItemsOverflow = 0u; // chunks sent to the late pass because the list was full
    std::uint32_t chunks = 0u;              // chunks of the frame's on-screen segments
    std::uint32_t chunksLate = 0u;          // ... left for the late pass
    std::uint32_t chunksLateDrawn = 0u;     // ... of those, not hidden and drawn
    std::uint32_t pixelsTested = 0u;        // pixels the two passes looked at
    std::uint32_t coverageAdds = 0u;        // coverage adds
    std::uint32_t hiddenWrites = 0u;        // hidden depths lowered by a strand that covers its pixel whole
    std::uint32_t pixelsSaturated = 0u;     // pixels whose near coverage reached the saturation
    std::uint32_t latePixelsWritten = 0u;   // layer writes of the late pass
    std::uint32_t hairTiles = 0u;           // tiles a chunk touched
    std::uint32_t clustersUnseen = 0u;      // select: kept clusters that drew no pixel in the last frames
    std::uint32_t clustersReduced = 0u;     // select: kept clusters drawn at a detail scale below 1
    std::uint32_t waveIterations = 0u;      // with rasterProfile: lane-iterations the passes' waves ran, idle lanes included
    std::uint32_t volumeVertices = 0u; // shade: lit vertices whose instance has a volume
    std::uint32_t volumeMarches = 0u;  // shade: marches from those, toward lights and environment directions
    std::uint32_t volumeSteps = 0u;    // shade: the voxel reads of those marches
    // Shadow casting (hairShadow.hlsl).
    std::uint32_t shadowClustersTested = 0u; // shadow select: clusters looked at
    std::uint32_t shadowClusters = 0u;       // ... kept: over a shadow page drawn this frame
    std::uint32_t shadowStrands = 0u;        // their drawn strands
    std::uint32_t shadowSegments = 0u;       // and segments
    std::uint32_t shadowPoolVertices = 0u;   // rows of the shadow pool used
    std::uint32_t shadowOverflow = 0u;       // kept clusters dropped because the shadow pool or list was full
    std::uint32_t shadowUnseenStrands = 0u;  // strands of instances no view shows, drawn on their rigid pose
    std::uint32_t shadowStaleGuides = 0u;    // as staleGuidesDrawn, for the shadow view: 0 likewise
    std::uint32_t shadowSegmentsDrawn = 0u;  // (segment, clipmap) pairs that wrote a texel
    std::uint32_t shadowTexelsCovered = 0u;  // footprint texels on pages that take depth this frame
    std::uint32_t shadowTexelsWritten = 0u;  // ... accepted and written
    std::uint32_t shadowSegmentsCapped = 0u;
    std::uint32_t shadowPagesAdmitted = 0u;  // the shadow map's own: pages it draws this frame
    std::uint32_t shadowPagesInvalidated = 0u; // ... and page table entries invalidated by bounds this frame
    std::uint32_t shadowClipmapMask = 0u;    // the clipmaps hair was kept for, a bit each
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

// The material of an instance. Each path has its own.
struct HairMaterial {
    // The surface path's placeholder: the resolve writes it as an ordinary rough dielectric surface.
    Float3 baseColor{ 0.09f, 0.05f, 0.03f }; // linear; dark brown
    float roughness = 0.6f;                  // perceptual
    // The layered path's: the fibre's scattering parameters, Chiang et al. 2016's as pbrt-v4's HairBxDF takes them
    // (HairBsdfShared.h), so a path tracer can use the same material.
    Float3 sigmaA{ 0.838f, 1.394f, 2.74f }; // absorption per fibre diameter, RGB: brown (HairMaterialFromMelanin(2, 0))
    float betaM = 0.3f;                     // longitudinal roughness, [0, 1]
    float betaN = 0.3f;                     // azimuthal roughness, [0, 1]
    float alphaDegrees = 2.0f;              // the cuticle scales' tilt
    float eta = 1.55f;                      // index of refraction
    // Per strand: its absorption is sigmaA times 1 + melaninVariation * r and its betaM is betaM times
    // 1 + roughnessVariation * r', with r and r' in [-1, 1), the strand's own.
    float melaninVariation = 0.15f;
    float roughnessVariation = 0.1f;
};

// A named material for testing (hair.attach's `preset`): "black", "brown", "blond", "red", "grey" or "white". False for
// any other name. It sets sigmaA from an amount of melanin and its redness and leaves the rest of `material` alone.
bool ApplyHairMaterialPreset(std::string_view name, HairMaterial& material);
// The preset names, for an error message.
const char* HairMaterialPresetNames();

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
    bool active = false; // the last frame's passes covered it
    std::uint32_t strands = 0u;
    std::uint32_t guideRows = 0u; // the guide rows it needs to be simulated: its groom's guide points
    // Its first row of guide state while it holds rows (simulated, or keeping a shape); HAIR_INVALID_INDEX otherwise.
    std::uint32_t guideRowBase = HAIR_INVALID_INDEX;
    // Its density volume in the last frame: voxels along each side (0: none) and a voxel's size in metres.
    std::array<std::uint32_t, 3> volumeResolution{};
    float volumeVoxelSize = 0.0f;
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
    // Not compared: the frame's per-object rows are not on the CPU, or the check list was full.
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
    std::uint32_t instanceStrands = 0u; // strands summed over instances
    std::uint32_t guidePoints = 0u;     // guide state rows lent out (guideRows.rowsInUse)
    HairCapacities capacities;
    bool buffersCreated = false;
    std::uint64_t bufferBytes = 0u;           // GPU memory of the pooled buffers
    std::vector<HairBufferUse> bufferUse;     // each pooled buffer: its capacity, what is in use and the most that was
    HairGuideRowStats guideRows;
    std::uint64_t assetBytesUploaded = 0u;    // groom arrays, once per asset
    std::uint64_t instanceBytesUploaded = 0u; // instance rows, whenever one changes
    std::uint64_t pendingUploadAssets = 0u;
    // Counted on the CPU: frames the plugin was updated in, and the evaluate dispatches those frames asked for.
    std::uint64_t frames = 0u;
    std::uint64_t evaluateDispatches = 0u;
    // The newest readback of the GPU counters. A Stats call asks for one more readback. With continuous counters on,
    // every frame is read back, and gpuCounterHistory holds the last kHairCounterHistoryFrames of them, oldest first: a
    // readback arrives a few frames after its frame, so a caller polling once a frame finds every frame in it.
    HairGpuCounters gpuCounters;
    bool continuousCounters = false;
    std::vector<HairGpuCounters> gpuCounterHistory;
    HairLodSettings lod;
    HairRenderSettings render;
    HairVolumeSettings volume;
    HairVolumeStats volumeStats;
    HairShadowSettings shadows;
    HairShadowStats shadowStats;
    HairDebugView debugView = HairDebugView::Off;
    bool rasterCulled = false;          // the last frame's layered path was drawn by the culled rasterizer
    float rasterWidthPixels = 0.0f;     // ... and the strand width on screen the Auto rule saw
    std::uint32_t visibilityWidth = 0u; // the hair visibility target
    std::uint32_t visibilityHeight = 0u;
    std::vector<HairBufferUse> targets; // the textures: one row each, rowBytes its size
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

// One instance a frame works on: the select pass dispatches over its clusters.
struct HairInstanceDispatch {
    std::uint32_t instanceIndex = 0u;
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
    HairRenderPath renderPath = HairRenderPath::Surface;
    bool reactiveMask = false; // the layered path's composite writes the upscaler's reactive mask
    bool rasterCulled = false; // the layered path is drawn by the culled rasterizer (its targets exist)
    float rasterWidthPixels = 0.0f; // the widest strand of a groom in view, on screen at the groom's middle (the Auto rule's)
    // The density volumes: the voxels the frame's volumes use (0: none are built, and the voxelize pass records
    // nothing) and the rows of the volume buffer the frame uploaded.
    std::uint32_t volumeVoxels = 0u;
    std::uint32_t volumeRows = 0u;
    // Shadow casting: whether the shadow passes record this frame, and the most clusters and pool rows they can use.
    bool shadows = false;
    std::uint32_t shadowEvaluateGroups = 0u;
    std::uint32_t shadowRasterRows = 0u;
    bool probe = false; // run the probe pass this frame (CaptureProbe)
    std::uint32_t rootCheckCount = 0u; // rows of the self-check's list this frame: the root-check pass writes their frames
    bool operator==(const HairFramePlan&) const = default;
};

// A perspective view that shows hair, as the simulation's planning needs it (HairRuntime.cpp's OnScreenExtent).
struct HairView {
    std::array<float, 16> view{};                 // world to view, row-major, for row vectors: the renderer's Camera::view
    std::array<std::array<float, 4>, 6> planes{}; // its frustum in view space (Camera::clippingPlanes): inside is dot(xyz, p) + w >= 0
    float projectionScaleX = 0.0f;                // projection[0][0] and [1][1]: a length over its depth, times these, is
    float projectionScaleY = 0.0f;                // its share of half the view's width and height
};

// What a frame's update reads from the renderer.
struct HairFrameInputs {
    std::uint64_t frameNumber = 0u;
    HairFrameObjects objects;
    std::uint32_t renderWidth = 0u; // the size of the opaque visibility buffer: the hair visibility target takes it
    std::uint32_t renderHeight = 0u;
    Float3 cameraPosition{}; // of the main view, for HairGpuCounters
    float deltaSeconds = 0.0f; // the frame's time step: what the simulation's accumulator takes
    // The views that show hair, for deciding which instances are simulated. Hair is in the main view only.
    HairView mainView;
};

// How the runtime writes a buffer: a hair pass's own upload, on the render thread.
using HairUpload = std::function<void(const std::shared_ptr<org::Buffer>& buffer, std::size_t byteOffset,
    const void* data, std::size_t byteCount)>;

// Groom assets and their instances. An asset is the built arrays of one groom, uploaded once into the pooled buffers.
// An instance attaches an asset to a scene entity (found by its Components::Name); it owns nothing in GPU memory but its
// row, and borrows guide state while it is simulated (HairGuideRowStats). Assets and instances are never freed one at a
// time: Clear drops everything.
//
// Threads: Initialize and Shutdown on the thread that runs Renderer::Update (the render thread in the demo).
// UpdateFrame on the render thread, from the root-check pass's Update. Everything else from any thread.
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
    // Replaces an instance's material, from the next frame. False with `error` set for no such instance.
    bool SetInstanceMaterial(std::uint32_t instance, const HairMaterial& material, std::string& error);
    // Drops every instance and asset. The GPU buffers stay.
    void Clear();

    // Off by default. While on, every few frames `rootsPerInstance` roots of each active instance are read back and
    // compared (HairSelfCheckStatus); each comparison is logged.
    void SetSelfCheck(bool enabled, std::uint32_t rootsPerInstance = 8u);
    HairStats Stats();

    // The LOD controls take effect in the next frame. Values out of range are clamped.
    void SetLodSettings(const HairLodSettings& settings);
    HairLodSettings LodSettings() const;
    // The rendering controls take effect in the next frame. Values out of range are clamped.
    void SetRenderSettings(const HairRenderSettings& settings);
    HairRenderSettings RenderSettings() const;
    // The density volume controls take effect in the next frame. Values out of range are clamped.
    void SetVolumeSettings(const HairVolumeSettings& settings);
    HairVolumeSettings VolumeSettings() const;
    // The shadow casting controls take effect in the next frame. Values out of range are clamped.
    void SetShadowSettings(const HairShadowSettings& settings);
    HairShadowSettings ShadowSettings() const;
    // The shadow map's side, for HairExtension as its caster provider: the queue invalidated bounds go to (null: this
    // graph has no shadow map to draw into), and the distance within which the shadow map should redraw deformable
    // casters every frame (0: no request).
    void SetShadowInvalidationQueue(std::shared_ptr<VirtualShadowInvalidationQueue> queue);
    float RequestedDynamicShadowRadius() const;
    // Called every frame by a shadow pass of a graph that has a shadow map: hair casts shadows only while one does.
    void NoteShadowPasses(std::uint64_t frameNumber);
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
    // (HairVisibleCluster), "guide_points" (HairGuidePoint), "instances" (HairInstanceGPU), "clusters"
    // (HairClusterHeader), "strands" (HairStrandRecord) or "guide_bindings" (HairGuideBinding). Several requests made before a frame are read back after
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
    // instance's rows for this frame, lends and takes back guide rows, and decides the frame's plan.
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
