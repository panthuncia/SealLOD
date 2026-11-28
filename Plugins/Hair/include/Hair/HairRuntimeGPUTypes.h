#ifndef BR_HAIR_RUNTIME_GPU_TYPES_H
#define BR_HAIR_RUNTIME_GPU_TYPES_H

// Runtime-only structs shared by HairPlugin and the hair shaders: what the plugin uploads per instance, a root's frame,
// the guides' simulated state, and what the select, evaluate and raster passes share. Like HairGPUTypes.h this compiles as C++ and HLSL, with the same layout rules. Nothing
// here is stored in a .groom file, so a change needs no schema bump.
//
// HLSL includes HairGPUTypes.h before this file (hairStructs.hlsli).

#ifdef __cplusplus
#include "Hair/HairGPUTypes.h"

namespace br::hair {

// HLSL's float4 and uint3, for HairVolumeGPU.
struct Float4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};
struct UInt3 {
    std::uint32_t x = 0u;
    std::uint32_t y = 0u;
    std::uint32_t z = 0u;
};
using float4 = Float4;
using uint3 = UInt3;
#endif

// One groom attached to one scene entity. The bases are rows of the plugin's pooled buffers; a groom's aggregates
// share its cluster rows, its root bindings share its strand rows and its guide strand indices share its guide rows.
struct HairInstanceGPU {
    uint clusterBase; // HairClusterHeader, HairClusterAggregate
    uint clusterCount;
    uint strandBase; // HairStrandRecord, HairRootBinding
    uint strandCount;
    uint guideBase; // HairGuideBinding, guide strand index
    uint guideCount;
    uint pointBase; // HairPackedPoint
    uint pointCount;
    // The instance's first cluster in the cluster state buffer (HAIR_CLUSTER_STATE_*): what its clusters showed of
    // themselves in the last frames. HAIR_INVALID_INDEX: it has none, and its clusters always count as in front.
    uint clusterSeenBase;
    uint perObjectIndex;       // the entity's row of Builtin::PerObjectBuffer: model and previous model matrix
    uint skinningInstanceSlot; // row of SkinningInstanceInfo; HAIR_INVALID_INDEX: the roots follow the entity rigidly
    uint attachBone;           // every root follows this joint; HAIR_INVALID_INDEX: each root's own joints and weights
    float3 baseColor;          // the surface path's placeholder material, which the resolve writes: linear base colour
    float roughness;           // and perceptual roughness. The layered path does not read them.
    // HairGuidePoint: the instance's guide state, one row per point of every guide. The rows are lent to an instance
    // while it is simulated or keeps a shape (HairRuntime's guide rows); HAIR_INVALID_INDEX while it has none, and then
    // HAIR_SIM_FLAG_ENABLED is never set.
    uint guidePointBase;
    uint colliderBase;   // HairColliderGPU, colliderCount rows
    uint colliderCount;
    uint simFlags;       // HAIR_SIM_FLAG_*
    // sFrac, the instance's fractional simulation level this frame (HairGuideIsSimulated): above 0 its level 0 guides
    // are simulated, and a guide of level k >= 1 when simLevel >= k + its HairGuideBinding::randomOffset. At the
    // groom's level count every guide is. 0 while HAIR_SIM_FLAG_ENABLED is set: the guides blend back to the rigid pose.
    float simLevel;
    // The fibre's material on the layered path (HairBsdfShared.h): Chiang's parameters, as HairBsdfMaterial has them,
    // and how far a strand's own material may be from them. A strand's absorption is sigmaA times
    // 1 + melaninVariation * r and its longitudinal roughness betaM times 1 + roughnessVariation * r', r and r' in
    // [-1, 1) and fixed for the strand.
    float melaninVariation;
    float roughnessVariation;
    uint pad0;
    float3 sigmaA;
    float betaM;
    float betaN;
    float alpha; // radians
    float eta;
    uint pad1;
};

// HairInstanceGPU::simFlags.
#define HAIR_SIM_FLAG_ENABLED 1u // the guide state is live and the other strands follow it; off: every strand follows its root rigidly
#define HAIR_SIM_FLAG_RESET 2u   // this frame: put every guide back on its rest pose, at rest
// With HAIR_SIM_FLAG_RESET: the instance was simulated before and its guide rows hold the shape it was left in. A guide
// the solver owned then, and that the level still asks for, starts at rest in that shape: every point keeps its offset
// from its rigid pose. The other guides are evaluated from those.
#define HAIR_SIM_FLAG_KEEP_SHAPE 4u
// Not simulated because no view needs it (HairRuntime's OnScreenExtent), though its size on screen would ask for it. Set
// without HAIR_SIM_FLAG_ENABLED. None of its strands should be drawn: the evaluate pass counts those that are
// (HAIR_COUNTER_UNSEEN_STRANDS_DRAWN).
#define HAIR_SIM_FLAG_UNSEEN 8u

// A strand root's frame in world space. Nothing stores these from frame to frame: every pass that needs a root's frame
// works it out (hairRootFrame.hlsli).
struct HairRootFrame {
    float3 position;
    uint writtenFrame; // the self-check's rows only: low 32 bits of the frame number of the dispatch that wrote the row
    float3 normal;     // unit
    float pad0;
    float3 tangent; // unit; perpendicular to the normal up to what skinning and the model matrix do to the rest frame
    float pad1;
};

// A root's frame this frame and the one the previous frame's matrices give.
struct HairRootFramePair {
    HairRootFrame current;
    HairRootFrame previous;
};

// One root the self-check (hair.selfcheck) asks the GPU for: the root-check pass writes row i of the check buffer from
// row i of this list.
struct HairRootCheckSample {
    uint instanceIndex; // HairInstanceGPU
    uint strandRow;     // groom-relative
};

#define HAIR_ROOT_CHECK_CAPACITY 4096u

// One point of a guide, in world space. A guide (by rank) owns 2^k + 1 consecutive rows from
// guidePointBase + its HairGuidePointRange's offset, k being its cluster's vertex level; row 0 is its root.
// While its instance is simulated every guide's rows are live: a simulated guide's hold its simulated state, and any
// other guide's hold what its parents give it (the guide-level pass), kept so that the solver can take it over as it is.
struct HairGuidePoint {
    float3 position;         // where the point is: simulated, evaluated from the guide's parents, or a blend of the two
    float scratch0;          // the solver's own, within one dispatch
    float3 solverPrevious;   // where the last substep started: velocity is (position - solverPrevious) / solverSeconds
    float scratch1;
    float3 previousPosition; // `position` at the end of the previous frame
    float solverSeconds;     // the length of that substep: a guide takes more, shorter substeps in a frame its root jumps in
    float3 rigid;            // the rest point through this frame's root frame: where the point is without simulation
    float guideBlend;        // row 0 only: seconds into the guide's promotion or demotion blend
    float3 previousRigid;    // the same through the previous frame's root frame
    uint guideState;         // row 0: HAIR_GUIDE_STATE_*. Row 1: HAIR_GUIDE_STAMP of the frame the guide's rows were last written in
};

// What a guide's row 1 holds in guideState: the instance the rows were written for and the frame they were written in
// (its low 22 bits), by the simulate pass or the guide-level pass. Guide rows are lent to instances and taken back, so
// the passes check this and count a mismatch: the evaluate pass that the rows a drawn strand reads were written this
// frame for its instance (HAIR_COUNTER_STALE_GUIDES_DRAWN), the simulate and guide-level passes that the rows a guide
// goes on from were written for its instance in the frame before (HAIR_SIM_METRIC_STALE_GUIDES). A guide has at least
// two points.
#define HAIR_GUIDE_STAMP(instanceIndex, frameNumber) ((((uint)(instanceIndex)) << 22u) | (((uint)(frameNumber)) & 0x3FFFFFu))

// HairGuidePoint::guideState, in a guide's row 0. A guide the simulation level asks for is promoted: simulated from the
// state its rows hold. One it no longer asks for is demoted: still simulated while its state is pulled onto what its
// parents give it, then not. Both blends pull the simulated state toward the evaluated one after each frame's steps, by
// a share that falls from 1 to 0 over a promotion and rises from 0 to 1 over a demotion (HairGuideBlendPull).
#define HAIR_GUIDE_STATE_SIMULATED 1u // the solver owns the rows
#define HAIR_GUIDE_STATE_PROMOTING 2u // simulated, in its promotion blend
#define HAIR_GUIDE_STATE_DEMOTING 4u  // simulated, in its demotion blend
#define HAIR_GUIDE_STATE_CHANGED 8u   // became simulated this frame, or stopped being simulated in the frame before

// The most guide levels the runtime handles (the builder's defaults make 4).
#define HAIR_MAX_GUIDE_LEVELS 8u

// Where a guide's rows are, by rank: parallel to the guide bindings, relative to the instance's guidePointBase.
struct HairGuidePointRange {
    uint offset;
    uint count; // 2^k + 1
};

// An analytic collider in the rest space of an instance's entity (its mesh space): a capsule from a to b. A sphere is
// a capsule with a == b. It follows the instance's attach bone when it has one, and the entity's transform.
struct HairColliderGPU {
    float3 a;
    float radius;
    float3 b;
    float pad0;
};

#define HAIR_MAX_COLLIDERS_PER_INSTANCE 8u

// One thread group of a simulation dispatch that covers several instances: its 64 threads work on guides
// [firstRank, firstRank + count) of one instance. The CPU writes the frame's groups (HairFramePlan says which rows each
// dispatch covers), so a dispatch's size follows the guides that have work and not the instances in the scene.
struct HairSimGroup {
    uint instanceIndex; // HairInstanceGPU
    uint firstRank;
    uint count; // at most 64
    uint pad0;
};

// A guide takes as many substeps in a frame as keep its root's travel per substep, and its fastest point's, under this
// many metres, and at least the ones asked for (HairSimFrameGPU), up to HAIR_SIM_MAX_SUBSTEPS: a root, a collider or a
// point that covers a segment's length in one substep tears at the strand.
#define HAIR_SIM_SUBSTEP_TRAVEL 0.008f
#define HAIR_SIM_MAX_SUBSTEPS 64u

// The one row of the simulation's frame buffer, uploaded every frame.
struct HairSimFrameGPU {
    uint stepCount;      // fixed steps to take this frame: 0 when the frame's time has not filled a step yet
    uint substeps;       // per step
    float stepSeconds;   // one fixed step
    float time;          // simulated seconds before this frame's steps: the gust's clock
    float3 gravity;      // m/s^2
    float damping;       // 1/s: how fast a point's velocity goes to the air's (the wind, or rest)
    float3 wind;         // m/s
    float gustAmplitude; // the wind's speed varies by this share of itself
    float gustFrequency; // Hz
    float stretchCompliance; // XPBD, s^2 (per unit mass): 0 is inextensible
    float bendCompliance;    // XPBD, s^2: of the constraint that keeps each point where the rest shape puts it after its parents
    float friction;          // [0, 1]: share of a point's sliding speed along a collider lost per substep in contact
    float collisionMargin;   // metres kept between a point and a collider's surface
    float teleportDistance;  // a root that moved further than this in one frame puts its guide back on the rest pose
    float shapeCompliance;   // XPBD, s^2: of the pull toward the rigid pose at the tip; it falls to 0 at the root
    float promoteSeconds;    // simulation LOD: how long a promoted guide's blend from its evaluated state takes
    float demoteSeconds;     // and a demoted guide's blend back onto it
    uint frameNumber;        // HairFrameGPU's: the frame the root frames of this frame's guides were written in
    uint pad1;
    uint pad2;
};

// The one row of the frame-state buffer, uploaded every frame that has an active instance.
struct HairFrameGPU {
    uint frameNumber;
    float lodErrorPixels; // strands of a cluster are all drawn while they are at least this far apart on screen
    float maxWidthScale;  // wMax: the most a drawn strand is widened to make up for dropped ones
    float minDetail;      // lMin: the lowest detail level a cluster takes
    float forceDetail;    // > 0: every cluster takes this detail level, whatever its distance
    float segmentPixels;  // vertex LOD: the length on screen a drawn segment aims for
    uint visibleClusterCapacity; // rows of the visible-cluster list
    uint poolVertexCapacity;     // rows of the strand pool
    uint coverageMode;       // HAIR_COVERAGE_*: how the rasterizer treats a strand thinner than its one-pixel footprint
    uint coverageAccumulate; // not 0: the rasterizer adds every segment's coverage into the coverage target
    float detailExponent;    // the detail level is (strand spacing / lodErrorPixels) to this power: 1 spacing, 2 area
    uint renderPath;         // HAIR_PATH_*: how hair gets into the frame
    // The layered path (HAIR_PATH_LAYERED).
    float depthAlpha;         // composite: a pixel whose hair alpha is at least this takes hair's depth and motion vector
    float coverageSaturation; // raster: the coverage target stops taking adds at this many pixels' worth; 0: never
    float reactiveStrength;   // composite: scale of the upscaler's reactive mask; 0: the mask is all zero
    float reactivePixels;     // ... a hair pixel is fully reactive when hair and background motion differ by this many pixels
    float ambientScale;       // shade: scale of the environment term
    uint lightLookup;         // shade: HAIR_LIGHTS_*
    uint shadeMask;           // shade: HAIR_SHADE_*, what a vertex's colour is the sum of
    // The density volumes (HairVolumeGPU): the layered path's self-shadowing and multiple scattering.
    uint volumeFlags;         // HAIR_VOLUME_FLAG_*; 0: no volume is built or read, and hair is lit as without one
    uint volumeSteps;         // shade: samples of a march toward a light
    uint volumeEnvironmentSteps; // ... and toward one of the environment's directions
    float volumeBias;         // shade: voxels a march skips at its start, where the vertex's own neighbours are
    float volumeDensityFactor; // shade: Zinke et al.'s d_f and d_b, the share of a fibre's surroundings that holds hair
    uint volumeScalpDepth;    // voxelize: voxels of the opaque shell marked under every root; 0: none
    uint volumeInstanceCount; // rows of the volume buffer in use: one per instance row
    // Shadow casting (hairShadow.hlsl): hair in the sun's virtual shadow map.
    uint shadowFlags;           // HAIR_SHADOW_FLAG_*; 0: hair casts no shadow, and hair is lit exactly as without
    float shadowDetailScale;    // a shadow texel counts as this many when the shadow view picks its level of detail
    float shadowForceDetail;    // > 0: every cluster takes this detail level in the shadow view
    uint shadowClusterCapacity; // rows of the shadow cluster list
    uint shadowPoolCapacity;    // rows of the shadow pool
    float shadowReceiverBias;   // shade: metres an opaque occluder inside a groom's box must be nearer the sun than a vertex to shadow it
    // The culled rasterizer (hairRasterCulled.hlsl), the layered path's.
    uint rasterFlags;           // HAIR_RASTER_FLAG_*; 0: the direct rasterizer (hairRaster.hlsl) draws the layered path
    uint rasterItemCapacity;    // rows of the first pass's item list
    uint saturationUnits;       // HAIR_RASTER_FLAG_OCCLUDE_SATURATED: the near coverage (HAIR_COVERAGE_UNIT) at which a pixel hides what is behind its split depth
    // Select: a cluster that drew no pixel in the last frames (behind the head, or behind hair that hides it) is drawn
    // at this fraction of its detail level. Its scale moves toward that by detailRateDown a frame, and back to 1 by
    // detailRateUp a frame once it draws a pixel.
    float hiddenDetail;
    uint clusterSeenCapacity;   // clusters the cluster state buffer holds
    float hintDecay;            // the first pass's guess keeps this share a frame of how far it is past the newest tile depth; 0: the last frame's depths alone
    float detailRateUp;
    float detailRateDown;
};

// HairFrameGPU::rasterFlags.
#define HAIR_RASTER_FLAG_CULLED 1u            // the culled rasterizer draws the layered path
// A footprint pixel a strand covers whole (coverage 1) adds HAIR_COVERAGE_OPAQUE to the pixel's coverage, not 1: nothing
// shows through it, so whatever is behind it adds nothing, wherever among the pixel's strands it is.
#define HAIR_RASTER_FLAG_OPAQUE_COVERAGE 2u
#define HAIR_RASTER_FLAG_OCCLUDE_OPAQUE 4u    // ... and it lowers the pixel's hidden depth to its own: hair behind it is skipped
// A pixel whose coverage in front of its split depth reaches saturationUnits lowers its hidden depth to the split depth.
#define HAIR_RASTER_FLAG_OCCLUDE_SATURATED 8u
// Which chunks the first pass draws; the others wait for the late pass, which tests them against the hidden depths the
// first pass left. Neither: the first pass draws every chunk and there is no late pass.
#define HAIR_RASTER_FLAG_FIRST_PREVIOUS 16u   // chunks of clusters seen in the last frames that the last frame's hidden depths do not hide
#define HAIR_RASTER_FLAG_FIRST_PREFIX 32u     // every chunk of the first eighth of each cluster's drawn strands
#define HAIR_RASTER_FLAG_PROFILE 64u          // count loop iterations by wave, for the lane occupancy
// The culled rasterizer has just taken over from the direct one: the cluster state is not this frame's, and the select
// pass leaves every cluster's detail scale as it is.
#define HAIR_RASTER_FLAG_HOLD_DETAIL 256u
#define HAIR_RASTER_FLAG_SKIP_LATE 128u       // for measurement only: the late pass draws nothing, so hair the first pass left out is missing

// The culled rasterizer cuts a segment into chunks of this many pixels along its major axis, aligned to the screen, and
// keeps the largest hidden depth of every tile of this many pixels each way.
#define HAIR_RASTER_TILE_SHIFT 3u
#define HAIR_RASTER_TILE_SIZE 8u
// An item of the first pass: a pool row in the low bits and one of its chunks above them.
#define HAIR_RASTER_ITEM_ROW_BITS 22u
// A row's late chunks are a mask: bit i is chunk i, and the last bit every chunk from that one on.
#define HAIR_RASTER_LATE_BITS 32u
#define HAIR_COVERAGE_OPAQUE 16.0f
// A depth field no hair depth reaches: "nothing is hidden", "everything is in front of the split".
#define HAIR_DEPTH_BITS_FAR 0x7FFFFFFFu
// Hair.Occlusion's low word is the pixel's split depth, with this bit set once hair has lowered the hidden depth: the
// pixel's hair is opaque from there on, and the composite takes its remainder as saturated without a coverage sum.
#define HAIR_OCCLUSION_SATURATED 0x80000000u
// Hair.SplitGuess: the next frame's split depth in the low 31 bits, and this bit while the guess is not to come nearer.
#define HAIR_SPLIT_GUESS_HOLD 0x80000000u
// HairVisibleCluster::flags.
#define HAIR_VISIBLE_FLAG_SEEN 1u // the cluster drew a pixel within the last two frames (or has no state)

// The cluster state buffer: HAIR_CLUSTER_STATE_WORDS uints per cluster of every instance, from
// (HairInstanceGPU::clusterSeenBase + cluster) * HAIR_CLUSTER_STATE_WORDS. The rasterizer stamps a cluster that draws a
// pixel the hidden depths do not hide, and the select pass keeps the scale it draws the cluster's detail level with.
#define HAIR_CLUSTER_STATE_WORDS 4u
#define HAIR_CLUSTER_STATE_SEEN 0u   // frame number of the last pixel drawn
#define HAIR_CLUSTER_STATE_SCALE 2u  // asuint of the detail scale, (0, 1]

// HairFrameGPU::shadowFlags.
#define HAIR_SHADOW_FLAG_ENABLED 1u  // hair is drawn into the sun's shadow map this frame
#define HAIR_SHADOW_FLAG_TEMPORAL 2u // the coverage dither changes every frame; off: it is the texel's and the strand's own
#define HAIR_SHADOW_FLAG_OPAQUE 4u   // every footprint texel is written: a strand shadows as if a texel wide

// A shadow segment tests at most this many texels in one clipmap.
#define HAIR_SHADOW_MAX_TEXELS_PER_SEGMENT 1024u

// HairFrameGPU::shadeMask: the lobes of the lights' response (the low bits are HAIR_LOBE_*, HairBsdfShared.h) and the
// environment term. All of them but in the debug views that show one alone.
#define HAIR_SHADE_ENVIRONMENT 8u
#define HAIR_SHADE_ALL 15u
// Debug views of the density volume's part in a vertex's colour, in the same field: the colour written is then not a lit
// colour.
#define HAIR_SHADE_DEBUG_SHIFT 8u
#define HAIR_SHADE_DEBUG_MASK 0xFu
#define HAIR_SHADE_DEBUG_NONE 0u
#define HAIR_SHADE_DEBUG_TRANSMITTANCE 1u  // what reaches the vertex of the first light through the hair, forward scattered light included
#define HAIR_SHADE_DEBUG_FIBRE_COUNT 2u    // the fibres between the vertex and the first light: black none, through red and yellow to white at 16
#define HAIR_SHADE_DEBUG_SCATTER_GLOBAL 3u // lit by the lights' forward scattered light alone (global multiple scattering)
#define HAIR_SHADE_DEBUG_SCATTER_LOCAL 4u  // lit by the lights through the backscattering lobe alone (local multiple scattering)
#define HAIR_SHADE_DEBUG_DENSITY 5u        // the volume's density at the vertex

// HairFrameGPU::volumeFlags.
#define HAIR_VOLUME_FLAG_ENABLED 1u         // volumes are built this frame and the shade pass marches them
#define HAIR_VOLUME_FLAG_DUAL_SCATTERING 2u // shade: forward scattered light and the backscattering lobe; off: the fibres in front only shadow
#define HAIR_VOLUME_FLAG_TRILINEAR 4u       // voxelize: a deposit is shared by the 8 voxels around it; off: the nearest takes it
#define HAIR_VOLUME_FLAG_JITTER_FRAME 8u    // shade: a march's offset changes every frame; off: it is the vertex's own

// The density volume of one instance: a box around its groom, in the entity's space, cut into cubic voxels. Row i of the
// volume buffer belongs to instance row i. It is planned on the CPU every frame from the groom's size on screen
// (HairRuntime.cpp's PlanVolumes) and filled from the frame's strand pool (hairVolume.hlsl). resolution.x is 0 for an
// instance without one (out of view, over the budget, the feature off): its hair is lit as without a volume.
//
// A voxel is one uint of the voxel buffer, at voxelBase + x + resolution.x * (y + resolution.y * z):
//   bits 30..0  fibre density: the summed length times diameter of the drawn strands' segments in the voxel over the
//               voxel's volume, in 1 / HAIR_VOLUME_DENSITY_UNIT per metre. The diameters are the pool's, which carry the
//               LOD's width scale, so a groom drawn with fewer, wider strands leaves the same density.
//   bit  31     scalp: the voxel is under a strand's root, inside whatever the hair grows on. A march that reads one
//               ends there: nothing comes through.
struct HairVolumeGPU {
    // World to voxel coordinates: voxel = (dot(worldToGrid0.xyz, p) + worldToGrid0.w, ...). Voxel (i, j, k) covers
    // [i, i + 1) on each axis.
    float4 worldToGrid0;
    float4 worldToGrid1;
    float4 worldToGrid2;
    uint3 resolution;
    uint voxelBase;
    float voxelSize; // metres along each axis, in world space
    uint pad0;
    uint pad1;
    uint pad2;
};

#define HAIR_VOLUME_DENSITY_UNIT 4096.0f
#define HAIR_VOLUME_DENSITY_MASK 0x7FFFFFFFu
#define HAIR_VOLUME_SCALP_BIT 0x80000000u
// A segment's deposit is made at this many points along it at most: one per voxel it is long.
#define HAIR_VOLUME_MAX_DEPOSITS_PER_SEGMENT 4u
// The fibres a ray crosses per metre are the density times the sine of the angle between the ray and the fibres. The
// volume keeps no directions: this is the mean of that sine over directions, pi / 4.
#define HAIR_VOLUME_ISOTROPIC_SINE 0.785398163f
// The scalp shell starts this many voxels under a root, so that no voxel a root's own vertex can be in is marked.
#define HAIR_VOLUME_SCALP_OFFSET 2.0f
// A march's first samples, where the hair nearest the vertex is and the density changes most from voxel to voxel, are
// interpolated between the eight voxels around them; the others read the one voxel they are in.
#define HAIR_VOLUME_INTERPOLATED_SAMPLES 2u

// HairFrameGPU::renderPath. Surface: the nearest (accepted) segment of a pixel is resolved into the canonical surface
// and lit by the renderer's deferred pass as an opaque surface (hairResolve.hlsl). Layered: the nearest three segments of
// a pixel are kept with the summed coverage, every drawn strand vertex is lit by the hair light loop (hairShade.hlsl),
// and the layers are blended over the lit frame (hairComposite.hlsl).
#define HAIR_PATH_SURFACE 0u
#define HAIR_PATH_LAYERED 1u

// HairFrameGPU::lightLookup: where the shade pass takes a vertex's lights from. List: every active light, tested by its
// range. Grid: the forward run of the vertex's froxel in the light grid, which only a recipe with ClusterLodAlphaTechnique
// builds; without it a vertex in front of the nearest opaque surface gets no light at all.
#define HAIR_LIGHTS_LIST 0u
#define HAIR_LIGHTS_GRID 1u

// The hair layers a pixel keeps on the layered path: the visibility target is the nearest, then two more.
#define HAIR_LAYER_COUNT 3u

// HairFrameGPU::coverageMode. Off: a covered pixel is written, whatever share of it the strand covers. Otherwise a pixel
// is written with the probability of the strand's coverage there, from a random number per pixel, strand and frame.
#define HAIR_COVERAGE_OFF 0u
#define HAIR_COVERAGE_WHITE 1u       // a hash of the pixel, the strand and the frame
#define HAIR_COVERAGE_INTERLEAVED 2u // interleaved gradient noise over the pixels, shifted each frame and by a hash of the strand

// The coverage target: one uint per pixel, the sum of the coverage of every segment over the pixel in front of the opaque
// surface, in these units (65536 is one whole pixel's worth, so a far strand's thousandth of a pixel still counts).
#define HAIR_COVERAGE_UNIT 65536.0f
// The frame's coverage counter is in these units of a pixel, coarse so that a view full of hair fits 32 bits. Each segment
// adds its own sum, rounded up or down at random in proportion (a far segment's sum is far below one unit).
#define HAIR_COVERAGE_COUNTER_UNIT 32.0f

// One cluster the select pass kept for this frame: what the evaluate pass, the rasterizer and anything reading the hair
// visibility target need to find its strands. Row i of the visible-cluster list is visible cluster i.
struct HairVisibleCluster {
    uint instanceIndex;   // HairInstanceGPU
    uint clusterIndex;    // groom-relative: add the instance's clusterBase
    uint strandCount;     // drawn strands: the cluster's first strandCount (HairLodStrandCountShared)
    uint poolVertexBase;  // strand s, vertex j is pool row poolVertexBase + s * (2^vertexLevel + 1) + j
    float detailLevel;    // l in (0, 1]
    float boundaryWeight; // width factor of the last drawn strand (HairLodBoundaryWeight)
    uint vertexLevel;     // drawn level: every drawn strand has 2^vertexLevel segments; at most the cluster's stored level
    float geomorph;       // [0, 1): how far the odd vertices have moved to the midpoint of their neighbours
    uint seenSlot;        // the cluster's row of the cluster state; HAIR_INVALID_INDEX: none
    uint flags;           // HAIR_VISIBLE_FLAG_*
    // A hidden cluster is drawn at a fraction of its detail level (HairFrameGPU::hiddenDetail). Its drawn strands keep the
    // width they have at the level it would have in view, widthDetail: they are a subset of the strands it would draw,
    // not fewer and wider ones. What the density volume takes from each is scaled by densityScale to make up for the
    // others. Otherwise widthDetail is detailLevel and densityScale 1.
    float widthDetail;
    float densityScale;
};

// One evaluated strand vertex in world space, for this frame and for the previous frame's root frames. The pool holds
// the drawn vertices of the visible clusters back to back; a parallel uint buffer gives each row's segment id.
struct HairPoolVertex {
    float3 position;
    float width; // diameter, LOD width scaling and the boundary strand's fade applied
    float3 previousPosition;
    float previousWidth;
};

// The hair visibility target: one 64-bit key per pixel (an R32G32_UInt texture read as uint64_t), written with
// InterlockedMin so the nearest segment wins. 0xFFFFFFFFFFFFFFFF is "no hair".
//
//   bits 63..33  depth: asuint(linear view depth) >> 1, exactly the depth field of the opaque visibility buffer
//                (visibilityPacking.hlsli), so the two compare directly
//   bit  32      unused, 0
//   bits 31..0   the segment id:
//     bits 31..16  visible-cluster index (row of the visible-cluster list)
//     bits 15..10  strand index within the cluster (0 is its lowest rank)
//     bits  9..0   segment index along the strand, from the root, at the cluster's drawn vertex level
//
// The segment id is also what the pool's parallel buffer holds for the first vertex of each segment; a strand's last
// vertex starts no segment and holds HAIR_INVALID_INDEX.
#define HAIR_VIS_SEGMENT_BITS 10u
#define HAIR_VIS_STRAND_BITS 6u
#define HAIR_VIS_CLUSTER_BITS 16u
#define HAIR_VIS_CLUSTER_SHIFT (HAIR_VIS_SEGMENT_BITS + HAIR_VIS_STRAND_BITS)
#define HAIR_VIS_DEPTH_SHIFT 33u
// So no real segment id is HAIR_INVALID_INDEX, the list holds one cluster fewer than its index field could name.
#define HAIR_MAX_VISIBLE_CLUSTERS 65535u

// Cluster bounds cover strand centrelines at rest. The frustum test grows the sphere by this share of its radius, for
// strands that will move under simulation, and by this many metres, for strand width.
#define HAIR_CULL_RADIUS_MARGIN 0.25f
#define HAIR_CULL_WIDTH_MARGIN 0.01f

// The rasterizer tests at most this many pixels for one segment; a segment that needs more is cut short and counted.
#define HAIR_RASTER_MAX_PIXELS_PER_SEGMENT 4096u

// What the debug overlay colours hair pixels by (HairDebugView).
#define HAIR_DEBUG_VIEW_OFF 0u
#define HAIR_DEBUG_VIEW_CLUSTER 1u // one colour per cluster
#define HAIR_DEBUG_VIEW_DETAIL 2u  // the cluster's detail level l: red at 0, green at 1
#define HAIR_DEBUG_VIEW_STRAND 3u  // one colour per strand
#define HAIR_DEBUG_VIEW_RANK 4u    // the strand's rank in its groom: white is kept longest
#define HAIR_DEBUG_VIEW_VERTEX_LEVEL 5u // the cluster's drawn vertex level and geomorph
#define HAIR_DEBUG_VIEW_COVERAGE 6u     // the coverage target, over black: grey up to one pixel's worth, then towards red
// The layered path's, over black, from what the composite wrote (the composite information target).
#define HAIR_DEBUG_VIEW_LAYERS 7u       // layers a pixel holds: 1 blue, 2 green, 3 yellow, 3 with coverage beyond them red
#define HAIR_DEBUG_VIEW_ALPHA 8u        // the composited hair alpha as a grey level
#define HAIR_DEBUG_VIEW_REACTIVE 9u     // the reactive mask the composite wrote for the upscaler, as a grey level
// The layered path's, drawn by the shade pass and not by the overlay: hair lit by one part of its response alone.
#define HAIR_DEBUG_VIEW_LOBE_R 10u      // the lights' R lobe
#define HAIR_DEBUG_VIEW_LOBE_TT 11u     // the lights' TT lobe
#define HAIR_DEBUG_VIEW_LOBE_TRT 12u    // the lights' TRT lobe
#define HAIR_DEBUG_VIEW_ENVIRONMENT 13u // the environment term
#define HAIR_DEBUG_VIEW_DIRECT 14u      // the lights, all three lobes, without the environment
// The density volume's. The first is drawn by the overlay over the whole frame, the others by the shade pass.
#define HAIR_DEBUG_VIEW_DENSITY 15u        // the largest density along each pixel's ray through the volumes; scalp voxels blue
#define HAIR_DEBUG_VIEW_TRANSMITTANCE 16u  // HAIR_SHADE_DEBUG_TRANSMITTANCE
#define HAIR_DEBUG_VIEW_FIBRE_COUNT 17u    // HAIR_SHADE_DEBUG_FIBRE_COUNT
#define HAIR_DEBUG_VIEW_SCATTER_GLOBAL 18u // HAIR_SHADE_DEBUG_SCATTER_GLOBAL
#define HAIR_DEBUG_VIEW_SCATTER_LOCAL 19u  // HAIR_SHADE_DEBUG_SCATTER_LOCAL
#define HAIR_DEBUG_VIEW_VERTEX_DENSITY 20u // HAIR_SHADE_DEBUG_DENSITY

// The composite information target (R8G8_UNorm): x the composited hair alpha, y these bits over 255.
#define HAIR_COMPOSITE_INFO_LAYER_MASK 3u // occupied layers, 0 to 3
#define HAIR_COMPOSITE_INFO_REMAINDER 4u  // coverage beyond the third layer
#define HAIR_COMPOSITE_INFO_DEPTH 8u      // the pixel took hair's depth and motion vector

// Rows of the counter buffer (uint).
#define HAIR_COUNTER_EVALUATE_DISPATCHES 0u // one per evaluate dispatch that ran on the GPU; never reset
// Reset every frame.
// Evaluate: drawn strands of a simulated instance that read guide rows not written this frame for that instance
// (HAIR_GUIDE_STAMP), or whose instance has no guide rows. 0 unless the CPU's lending of guide rows and the passes disagree.
#define HAIR_COUNTER_STALE_GUIDES_DRAWN 1u
#define HAIR_COUNTER_UNSEEN_STRANDS_DRAWN 2u // evaluate: drawn strands of instances the CPU found in no view (HAIR_SIM_FLAG_UNSEEN); 0 likewise
// Raster and shade-list passes, layered path: the rows of the shade list, the pool vertices the shade pass lights.
#define HAIR_COUNTER_SHADE_LIST 3u
// Selection: reset every frame the select pass runs (not while the LOD is frozen).
#define HAIR_COUNTER_CLUSTERS_TESTED 4u
#define HAIR_COUNTER_CLUSTERS_VISIBLE 5u  // also the visible-cluster list's allocator: it can pass the capacity
#define HAIR_COUNTER_STRANDS_SELECTED 6u
#define HAIR_COUNTER_SEGMENTS_SELECTED 7u
#define HAIR_COUNTER_POOL_VERTICES 8u     // also the strand pool's allocator: it can pass the capacity
#define HAIR_COUNTER_OVERFLOW_CLUSTERS 9u // visible clusters dropped because the pool or the list was full
// Raster: reset every frame.
#define HAIR_COUNTER_PIXELS_COVERED 10u   // pixels of segments' footprints in front of the opaque surface, counted for each segment
#define HAIR_COUNTER_COVERAGE_SUM 11u     // the coverage of those, summed, in 1 / HAIR_COVERAGE_COUNTER_UNIT of a pixel
#define HAIR_COUNTER_SEGMENTS_DRAWN 12u   // segments that wrote at least one pixel
#define HAIR_COUNTER_PIXELS_WRITTEN 13u   // InterlockedMin writes: a pixel under several segments counts for each
#define HAIR_COUNTER_PIXELS_OCCLUDED 14u  // covered pixels rejected by the opaque depth
#define HAIR_COUNTER_SEGMENTS_CAPPED 15u  // segments cut short at HAIR_RASTER_MAX_PIXELS_PER_SEGMENT
#define HAIR_COUNTER_PIXELS_RESOLVED 16u  // resolve: hair pixels written into the surface targets; reset every frame
// Selection: one bit per instance (HairInstanceGPU row), set when one of its clusters passes the frustum test. Reset with
// the selection counters.
// The layered path: reset every frame.
#define HAIR_COUNTER_LAYER_REJECTED 17u   // raster: covered pixels behind the pixel's third layer, which skipped the layer writes
#define HAIR_COUNTER_COVERAGE_SKIPPED 18u // raster: covered pixels whose coverage add was skipped at HairFrameGPU::coverageSaturation
#define HAIR_COUNTER_LAYER_PIXELS 19u     // composite, 3 rows: pixels holding exactly 1, 2 and 3 layers
#define HAIR_COUNTER_REMAINDER_PIXELS 22u // composite: three-layer pixels with coverage beyond the third layer
// Composite: what the remainder term added to the hair alpha, summed over pixels, and the hair alpha itself, in
// 1 / HAIR_ALPHA_COUNTER_UNIT of a pixel.
#define HAIR_COUNTER_REMAINDER_ALPHA 23u
#define HAIR_COUNTER_ALPHA_SUM 24u
#define HAIR_COUNTER_DEPTH_PIXELS 25u     // composite: pixels that took hair's depth and motion vector
#define HAIR_COUNTER_SHADED_VERTICES 26u  // shade: pool vertices lit: the shade list's rows that are this frame's
// Shade: the sun's virtual shadow map lookups of those vertices, and the ones that did not find the page the vertex
// asks for: read from a coarser clipmap, found no page at all (lit), or read a texel nothing was drawn into (lit).
#define HAIR_COUNTER_SHADOW_LOOKUPS 27u
#define HAIR_COUNTER_SHADOW_COARSER 28u
#define HAIR_COUNTER_SHADOW_MISSING 29u
#define HAIR_COUNTER_SHADOW_CLEARED 30u
// The density volumes: reset every frame.
#define HAIR_COUNTER_VOLUME_SEGMENTS 31u  // voxelize: pool segments deposited into a volume
#define HAIR_COUNTER_VOLUME_DEPOSITS 32u  // voxelize: the voxel adds they made
#define HAIR_COUNTER_VOLUME_OUTSIDE 33u   // voxelize: segments of an instance with a volume whose middle is outside its box
#define HAIR_COUNTER_VOLUME_VERTICES 34u  // shade: lit vertices whose instance has a volume
#define HAIR_COUNTER_VOLUME_MARCHES 35u   // shade: marches from those, toward lights and environment directions
#define HAIR_COUNTER_VOLUME_STEPS 36u     // shade: the voxel reads of those marches
// Shadow casting (hairShadow.hlsl): reset every frame.
#define HAIR_COUNTER_SHADOW_CLUSTERS_TESTED 37u  // shadow select: clusters looked at
#define HAIR_COUNTER_SHADOW_CLUSTERS 38u         // ... and kept; also the shadow cluster list's allocator: it can pass the capacity
#define HAIR_COUNTER_SHADOW_STRANDS 39u          // drawn strands of the kept clusters
#define HAIR_COUNTER_SHADOW_SEGMENTS 40u         // ... and their segments
#define HAIR_COUNTER_SHADOW_POOL_VERTICES 41u    // also the shadow pool's allocator: it can pass the capacity
#define HAIR_COUNTER_SHADOW_OVERFLOW 42u         // kept clusters dropped because the shadow pool or list was full
#define HAIR_COUNTER_SHADOW_UNSEEN_STRANDS 43u   // shadow evaluate: strands of instances no view shows, drawn on their rigid pose
#define HAIR_COUNTER_SHADOW_STALE_GUIDES 44u     // shadow evaluate: as HAIR_COUNTER_STALE_GUIDES_DRAWN; 0 likewise
#define HAIR_COUNTER_SHADOW_SEGMENTS_DRAWN 45u   // shadow raster: (segment, clipmap) pairs that wrote at least one texel
#define HAIR_COUNTER_SHADOW_TEXELS_COVERED 46u   // footprint texels on pages that take depth this frame, counted for each segment
#define HAIR_COUNTER_SHADOW_TEXELS_WRITTEN 47u   // ... and those accepted and written
#define HAIR_COUNTER_SHADOW_SEGMENTS_CAPPED 48u  // segments cut short at HAIR_SHADOW_MAX_TEXELS_PER_SEGMENT
#define HAIR_COUNTER_SHADOW_PAGES_ADMITTED 49u   // the shadow map's own: pages it draws this frame (cached pages redrawn)
#define HAIR_COUNTER_SHADOW_PAGES_INVALIDATED 50u // ... and page table entries invalidated by bounds this frame, hair's among them
#define HAIR_COUNTER_SHADOW_CLIPMAP_MASK 51u     // the clipmaps any kept cluster is drawn into, a bit each
#define HAIR_COUNTER_SHADOW_FIRST 37u
// The culled rasterizer (hairRasterCulled.hlsl): reset every frame.
#define HAIR_COUNTER_RASTER_ITEMS 52u         // chunks of the first pass; also the item list's allocator: it can pass the capacity
#define HAIR_COUNTER_RASTER_ITEMS_OVERFLOW 53u // chunks sent to the late pass because the item list was full
#define HAIR_COUNTER_CHUNKS 54u               // chunks of the frame's on-screen segments
#define HAIR_COUNTER_CHUNKS_LATE 55u          // ... of them left for the late pass
#define HAIR_COUNTER_CHUNKS_LATE_DRAWN 56u    // ... of those the late pass did not find hidden, and drew
#define HAIR_COUNTER_PIXELS_TESTED 57u        // pixels the two passes' loops looked at
#define HAIR_COUNTER_COVERAGE_ADDS 58u        // coverage adds
#define HAIR_COUNTER_HIDDEN_WRITES 59u        // hidden depths lowered by a strand that covers its pixel whole
#define HAIR_COUNTER_PIXELS_SATURATED 60u     // pixels whose near coverage reached the saturation
#define HAIR_COUNTER_LATE_PIXELS_WRITTEN 61u  // layer writes of the late pass: hair the first pass's choice missed
#define HAIR_COUNTER_HAIR_TILES 62u           // tiles a segment's chunk touched
#define HAIR_COUNTER_CLUSTERS_UNSEEN 63u      // select: kept clusters that drew no pixel in the last frames
#define HAIR_COUNTER_WAVE_ITERATIONS 64u      // HAIR_RASTER_FLAG_PROFILE: loop iterations of the two passes' waves, times the wave's lanes
#define HAIR_COUNTER_CLUSTERS_REDUCED 65u     // select: kept clusters drawn at a detail scale below 1
#define HAIR_COUNTER_INSTANCE_MASK_FIRST 68u
#define HAIR_COUNTER_INSTANCE_MASK_WORDS 32u // 1,024 instances
#define HAIR_COUNTER_COUNT 100u
#define HAIR_ALPHA_COUNTER_UNIT 64.0f
#define HAIR_COUNTER_FRAME_FIRST 1u
#define HAIR_COUNTER_SELECTION_FIRST 4u
#define HAIR_COUNTER_RASTER_FIRST 10u

// The simulation's metrics (uint): HAIR_SIM_METRIC_COUNT rows per instance, from row instance * HAIR_SIM_METRIC_COUNT.
// Set to their start values every frame by an upload, written by the simulate and evaluate passes, read back after it.
// "float bits" rows hold asuint of a non-negative float, so InterlockedMax orders them.
#define HAIR_SIM_METRIC_GUIDES 0u           // guides the simulate pass measured
#define HAIR_SIM_METRIC_STRETCH_MAX 1u      // float bits: largest |length / rest length - 1| of a guide segment
#define HAIR_SIM_METRIC_STRETCH_SUM 2u      // sum over guides of the guide's mean of that, in HAIR_SIM_METRIC_STRETCH_UNIT
#define HAIR_SIM_METRIC_SPEED_MAX 3u        // float bits: fastest guide point, m/s
#define HAIR_SIM_METRIC_SPEED_SUM 4u        // sum over guides of the guide's mean point speed, in HAIR_SIM_METRIC_SPEED_UNIT
#define HAIR_SIM_METRIC_COLLIDER_DISTANCE 5u // smallest signed distance of a guide point to a collider's surface, as
                                             // (int)(metres / HAIR_SIM_METRIC_DISTANCE_UNIT) + HAIR_SIM_METRIC_DISTANCE_BIAS; starts at all ones
#define HAIR_SIM_METRIC_GUIDE_ROOTS 6u      // drawn strands that are simulated guides (evaluate pass)
#define HAIR_SIM_METRIC_GUIDE_ROOT_ERROR 7u // float bits: furthest any of them has its first vertex from its root frame, metres
#define HAIR_SIM_METRIC_STRAND_ROOTS 8u     // drawn strands interpolated from guides
#define HAIR_SIM_METRIC_STRAND_ROOT_ERROR 9u // float bits: the same for them
#define HAIR_SIM_METRIC_RESET_GUIDES 10u    // guides put back on the rest pose this frame (a reset or a teleport)
#define HAIR_SIM_METRIC_DEVIATION_SUM 11u   // sum over guides of the guide's mean distance from its rigid pose, in HAIR_SIM_METRIC_DEVIATION_UNIT
#define HAIR_SIM_METRIC_STRETCHED_GUIDES 12u // guides with a segment more than HAIR_SIM_METRIC_STRETCH_LIMIT off its rest length
#define HAIR_SIM_METRIC_ROOT_SHIFT 13u      // float bits: furthest a root was from the frame's previous root frame when the frame began, metres
// Simulation LOD. A guide's motion is the furthest any of its points moved this frame relative to its root's own move:
// what the frame did to its shape. It is taken once the guide's rows are final for the frame.
#define HAIR_SIM_METRIC_SIMULATED_POINTS 14u  // points of the guides simulated this frame
#define HAIR_SIM_METRIC_BLENDING_GUIDES 15u   // simulated guides in a promotion or demotion blend
#define HAIR_SIM_METRIC_PROMOTED_GUIDES 16u   // guides that became simulated this frame
#define HAIR_SIM_METRIC_DEMOTED_GUIDES 17u    // guides whose demotion blend ended this frame: not simulated from the next
#define HAIR_SIM_METRIC_TRANSITION_MOTION 18u // float bits: largest motion of a guide that changed state this frame or the
                                              // frame before, or is in a blend
#define HAIR_SIM_METRIC_STEADY_MOTION 19u     // float bits: largest motion of any other guide, simulated or evaluated
#define HAIR_SIM_METRIC_EVALUATED_GUIDES 20u  // guides not simulated: evaluated from their parents (level 0: on the rigid pose)
#define HAIR_SIM_METRIC_RESUMED_GUIDES 21u    // guides that started this frame from the shape their rows held (HAIR_SIM_FLAG_KEEP_SHAPE)
#define HAIR_SIM_METRIC_STALE_GUIDES 22u      // guides that went on from rows not written for this instance in the frame before (HAIR_GUIDE_STAMP); 0 unless the lending and the passes disagree
#define HAIR_SIM_METRIC_LEVEL_GUIDES 24u      // HAIR_MAX_GUIDE_LEVELS rows: guides simulated this frame, by level
#define HAIR_SIM_METRIC_COUNT 32u
#define HAIR_SIM_METRIC_STRETCH_LIMIT 0.02f
#define HAIR_SIM_METRIC_STRETCH_UNIT 1.0e-7f // a guide's mean stretch error is counted in these, capped at HAIR_SIM_METRIC_SUM_CAP
#define HAIR_SIM_METRIC_SPEED_UNIT 1.0e-5f   // metres per second
#define HAIR_SIM_METRIC_DEVIATION_UNIT 1.0e-5f // metres
#define HAIR_SIM_METRIC_SUM_CAP 400000.0f    // so 8,192 guides cannot overflow a row
#define HAIR_SIM_METRIC_DISTANCE_UNIT 1.0e-6f
#define HAIR_SIM_METRIC_DISTANCE_BIAS 0x80000000u

// The probe buffer (hair.probe), in uints: what the frame's targets hold at hair pixels, for the CPU to check.
//   [0] rows the probe pass found (it can pass the capacity), [1] width and [2] height of the hair visibility target,
//   [3] HAIR_PROBE_STRIDE, then from HAIR_PROBE_MATRIX_FIRST four row-major 4x4 float matrices of the main camera:
//   view, unjittered projection, previous view, previous unjittered projection.
//   From HAIR_PROBE_HEADER_WORDS, HAIR_PROBE_ROW_WORDS a row: pixel (x | y << 16), segment id, then floats: the key's
//   depth, the linear depth target, the projected depth target, the surface device depth target, the motion vector's x
//   and y, the length of the surface normal and the surface roughness.
#define HAIR_PROBE_STRIDE 4u // one pixel in this many, each way
#define HAIR_PROBE_MATRIX_FIRST 8u
#define HAIR_PROBE_HEADER_WORDS 72u
#define HAIR_PROBE_ROW_WORDS 10u
#define HAIR_PROBE_ROW_CAPACITY 131072u

#ifdef __cplusplus
static_assert(sizeof(HairInstanceGPU) == 128u);
static_assert(sizeof(HairGuidePoint) == 80u);
static_assert(sizeof(HairGuidePointRange) == 8u); // a plain two-uint stream, like HairPackedPoint
static_assert(sizeof(HairColliderGPU) == 32u);
static_assert(sizeof(HairSimGroup) == 16u);
static_assert(sizeof(HairSimFrameGPU) == 96u);
static_assert(HAIR_SIM_METRIC_LEVEL_GUIDES + HAIR_MAX_GUIDE_LEVELS == HAIR_SIM_METRIC_COUNT);
static_assert(sizeof(HairRootFrame) == 48u);
static_assert(sizeof(HairRootFramePair) == 96u);
static_assert(sizeof(HairRootCheckSample) == 8u);
static_assert(sizeof(HairFrameGPU) == 160u);
static_assert(sizeof(HairVolumeGPU) == 80u);
static_assert(HAIR_COUNTER_INSTANCE_MASK_FIRST + HAIR_COUNTER_INSTANCE_MASK_WORDS == HAIR_COUNTER_COUNT);
static_assert(sizeof(HairVisibleCluster) == 48u);
static_assert(sizeof(HairPoolVertex) == 32u);
static_assert((1u << HAIR_VIS_STRAND_BITS) == HAIR_MAX_STRANDS_PER_CLUSTER);
static_assert(HAIR_MAX_VERTEX_LEVEL <= HAIR_VIS_SEGMENT_BITS); // a strand has at most 2^HAIR_MAX_VERTEX_LEVEL segments
static_assert(HAIR_VIS_CLUSTER_SHIFT + HAIR_VIS_CLUSTER_BITS == 32u);

} // namespace br::hair
#endif

#endif // BR_HAIR_RUNTIME_GPU_TYPES_H
