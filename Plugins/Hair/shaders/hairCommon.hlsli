#ifndef HAIR_COMMON_HLSLI
#define HAIR_COMMON_HLSLI

// What the select, evaluate, raster and debug shaders share: the main view's camera, how a strand follows its root,
// point decoding and the hair visibility key (HairRuntimeGPUTypes.h documents its bits).

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "HairShaders/hairStructs.hlsli"

// The main view: the camera the opaque visibility buffer was rasterized with.
Camera HairLoadMainCamera()
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    return cameras[perFrame.mainCameraIndex];
}

// Pixels a metre covers at linear view depth 1, down the screen: the projection CLod's error threshold uses
// (ViewManager's ComputeErrorOverDistanceThreshold). Divide by the depth.
float HairPixelsPerMetreAtUnitDepth(Camera camera, float screenHeight)
{
    return camera.projection[1][1] * 0.5f * screenHeight;
}

// Linear view depth, positive in front of the camera: the depth the visibility buffers store.
float HairViewDepth(Camera camera, float3 worldPosition)
{
    return -mul(float4(worldPosition, 1.0f), camera.view).z;
}

// The uniform scale of a row-vector model matrix, taken from its x axis.
float HairModelScale(float4x4 model)
{
    return length(model[0].xyz);
}

// A strand follows its root rigidly: a rest-space point keeps its offset from the root in the root's frame. The rows of
// a basis are the tangent, the bitangent and the normal.
struct HairRootFollow
{
    float3 restPosition;
    float3x3 restBasis;
    float3 position;
    float3x3 basis;
    float scale; // the entity's uniform scale: root frames are unit vectors and do not carry it
};

float3x3 HairFrameBasis(float3 normal, float3 tangent)
{
    // Skinning and a skewed model matrix can leave the tangent off the normal's plane.
    float3 t = tangent - normal * dot(normal, tangent);
    const float lengthSquared = dot(t, t);
    if (lengthSquared < 1.0e-12f)
    {
        t = abs(normal.x) < 0.9f ? cross(normal, float3(1.0f, 0.0f, 0.0f)) : cross(normal, float3(0.0f, 1.0f, 0.0f));
    }
    t = normalize(t);
    return float3x3(t, cross(normal, t), normal);
}

HairRootFollow HairMakeRootFollow(HairRootBinding rest, HairRootFrame frame, float scale)
{
    HairRootFollow follow;
    follow.restPosition = rest.restPosition;
    follow.restBasis = HairFrameBasis(rest.restNormal, rest.restTangent);
    follow.position = frame.position;
    follow.basis = HairFrameBasis(frame.normal, frame.tangent);
    follow.scale = scale;
    return follow;
}

float3 HairFollowRoot(HairRootFollow follow, float3 restPoint)
{
    const float3 local = mul(follow.restBasis, (restPoint - follow.restPosition) * follow.scale);
    return follow.position + mul(local, follow.basis);
}

// Whether the simulation level asks for a guide to be simulated this frame. The only place that decides it: the level 0
// guides of an instance that simulates at all (simLevel above 0), and a guide of level k when simLevel >= k + its random
// offset, so that guides come and go one at a time as the level moves. `binding` is the guide's own. The simulate pass
// follows this through each guide's blends (HAIR_GUIDE_STATE_*): a guide is still simulated while it is demoted.
bool HairGuideIsSimulated(HairInstanceGPU instance, HairGuideBinding binding)
{
    return (instance.simFlags & HAIR_SIM_FLAG_ENABLED) != 0u && instance.simLevel > 0.0f &&
        (binding.level == 0u || instance.simLevel >= (float)binding.level + binding.randomOffset);
}

// The share by which a blending guide's simulated state is pulled onto its evaluated state after this frame's steps,
// `blend` seconds into a blend of `duration`: from 0 to 1 over a demotion, and from 1 to 0 over a promotion. A blend
// that turns round keeps its pull: HairGuideBlendMirror gives the seconds at which the other blend has the same one.
//
// The share is of the distance there is each frame, not a place on a path between two fixed states: the solver takes
// the guide back toward its own shape in every frame's steps, so the pull acts as a hold that tightens over a demotion
// and slackens over a promotion. Two changes to that were tried, together, and under a fast sweep of the level moved
// guides up to 196 mm in a frame against 32 mm: a demotion that closes the distance along the ease curve (each frame
// the share of what is left that the curve covers), which probably leaves the solver winning until the last frames
// take the whole distance; and ending a blend that turns round instead of mirroring it, which lets go of the hold at
// once.
float HairGuideBlendPull(uint guideState, float blend, float duration)
{
    const float progress = duration > 0.0f ? saturate(blend / duration) : 1.0f;
    const float eased = progress * progress * (3.0f - 2.0f * progress);
    return (guideState & HAIR_GUIDE_STATE_DEMOTING) != 0u ? eased : 1.0f - eased;
}

float HairGuideBlendMirror(float blend, float duration, float otherDuration)
{
    return otherDuration * (1.0f - (duration > 0.0f ? saturate(blend / duration) : 1.0f));
}

// A guide's motion this frame, for the metrics: how far point `position` moved relative to the root's own move.
float HairGuidePointMotion(HairGuidePoint state, float3 rootMotion)
{
    return length(state.position - state.previousPosition - rootMotion);
}

float3 HairSafeNormalize(float3 v, float3 fallback)
{
    const float lengthSquared = dot(v, v);
    return lengthSquared > 1.0e-20f ? v * rsqrt(lengthSquared) : fallback;
}

// `v` turned by the smallest rotation that takes the unit vector `from` to the unit vector `to`. Equal vectors leave
// `v` exactly as it is.
float3 HairRotateBetween(float3 from, float3 to, float3 v)
{
    float3 axis = cross(from, to);
    float w = 1.0f + dot(from, to);
    if (w < 1.0e-4f)
    {
        // Opposite: half a turn about any axis across them.
        const float3 across = normalize(abs(from.x) < 0.9f ? cross(from, float3(1.0f, 0.0f, 0.0f)) : cross(from, float3(0.0f, 1.0f, 0.0f)));
        return 2.0f * across * dot(across, v) - v;
    }
    const float scale = rsqrt(dot(axis, axis) + w * w);
    axis *= scale;
    w *= scale;
    return v + 2.0f * cross(axis, cross(axis, v) + w * v);
}

// GroomBuilder.h's DecodeHairPointPosition and the width fraction of DecodeHairPointWidth.
float3 HairDecodePointPosition(HairClusterHeader cluster, HairPackedPoint packed)
{
    const float3 quantized = float3(packed.xy & 0xFFFFu, packed.xy >> 16u, packed.zw & 0xFFFFu);
    return cluster.quantizationOrigin + cluster.quantizationScale * quantized;
}

float HairDecodePointWidthFraction(HairPackedPoint packed)
{
    return (float)(packed.zw >> 16u) * (1.0f / 65535.0f);
}

uint HairPackSegmentId(uint visibleCluster, uint strand, uint segment)
{
    return (visibleCluster << HAIR_VIS_CLUSTER_SHIFT) | (strand << HAIR_VIS_SEGMENT_BITS) | segment;
}

void HairUnpackSegmentId(uint id, out uint visibleCluster, out uint strand, out uint segment)
{
    visibleCluster = id >> HAIR_VIS_CLUSTER_SHIFT;
    strand = (id >> HAIR_VIS_SEGMENT_BITS) & ((1u << HAIR_VIS_STRAND_BITS) - 1u);
    segment = id & ((1u << HAIR_VIS_SEGMENT_BITS) - 1u);
}

// The depth field of a visibility key, hair or opaque: PackVisKey's asuint(depth) >> 1 (visibilityPacking.hlsli).
uint HairDepthBits(float linearDepth)
{
    return asuint(linearDepth) >> 1u;
}

uint HairVisKeyDepthBits(uint64_t key)
{
    return (uint)(key >> HAIR_VIS_DEPTH_SHIFT);
}

uint64_t HairPackVisKey(uint depthBits, uint segmentId)
{
    return ((uint64_t)depthBits << HAIR_VIS_DEPTH_SHIFT) | (uint64_t)segmentId;
}

#endif // HAIR_COMMON_HLSLI
