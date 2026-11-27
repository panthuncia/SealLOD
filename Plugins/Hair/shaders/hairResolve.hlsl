// Resolves the hair visibility target into the renderer's surface data, so the passes after material evaluation treat a
// hair pixel as an ordinary opaque surface: the canonical surface targets and record that deferred shading reads, the
// depth targets, and a motion vector from the strand pool's current and previous positions. The rasterizer already
// rejected hair behind the opaque surface, so every pixel that holds a key is resolved.
//
// HairProbeCS (hair.probe) copies what the frame's targets hold at hair pixels into a buffer for the CPU to check.

#include "HairShaders/hairCommon.hlsli"

// HairResolvePass::Record and HairProbePass::Record. The camera and the canonical surface targets are bound by name.
#define HAIR_VISIBILITY UintRootConstant0        // Texture2D<uint64_t>
#define HAIR_VISIBLE_CLUSTERS UintRootConstant1  // StructuredBuffer<HairVisibleCluster>
#define HAIR_INSTANCES UintRootConstant2         // StructuredBuffer<HairInstanceGPU>
#define HAIR_POOL_VERTICES UintRootConstant3     // StructuredBuffer<HairPoolVertex>
#define HAIR_LINEAR_DEPTH UintRootConstant4      // Texture2D<float>, mip 0: RW in the resolve
#define HAIR_PROJECTED_DEPTH UintRootConstant5   // Texture2D<float>: RW in the resolve
#define HAIR_DEVICE_DEPTH UintRootConstant6      // Texture2D<float>: RW in the resolve
#define HAIR_VISIBILITY_WIDTH UintRootConstant7
#define HAIR_VISIBILITY_HEIGHT UintRootConstant8
#define HAIR_COUNTERS UintRootConstant9          // RWStructuredBuffer<uint> (resolve only)
#define HAIR_PROBE UintRootConstant9             // RWStructuredBuffer<uint> (probe only)

// The placeholder material: a rough dielectric with the groom's base colour. The hair BSDF replaces it.
static const float kHairSpecularF0 = 0.04f;

// The point of a strand a pixel shows.
struct HairHit
{
    HairVisibleCluster cluster;
    uint strandIndex;
    uint segmentIndex;
    float parameter;        // along the segment, 0 at its first vertex; perspective correct
    float3 position;        // world space, on the centreline
    float3 previousPosition;
    float3 tangent;         // unit, toward the tip
    float width;            // diameter, metres
    float across;           // where the pixel is across the strand: -1 and 1 at its edges, 0 on the centreline and for a
                            // strand thinner than a pixel
    float3 side;            // unit, world space: the direction `across` grows in
};

// Finds the hit from a pixel's key, as the rasterizer placed the segment (hairRaster.hlsl).
HairHit HairReconstructHit(uint2 pixel, uint64_t key, Camera camera)
{
    HairHit hit;
    uint visibleIndex;
    HairUnpackSegmentId((uint)key, visibleIndex, hit.strandIndex, hit.segmentIndex);
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    StructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_POOL_VERTICES];
    hit.cluster = visibleClusters[visibleIndex];
    const uint row = hit.cluster.poolVertexBase + hit.strandIndex * ((1u << hit.cluster.vertexLevel) + 1u) + hit.segmentIndex;
    HairPoolVertex start = poolVertices[row];
    HairPoolVertex end = poolVertices[row + 1u];

    float4 clip0 = mul(float4(start.position, 1.0f), camera.viewProjection);
    float4 clip1 = mul(float4(end.position, 1.0f), camera.viewProjection);
    // A segment that crosses the near plane was drawn from where it crosses.
    float clipped0 = 0.0f;
    float clipped1 = 1.0f;
    if (clip0.w < camera.zNear)
    {
        clipped0 = (camera.zNear - clip0.w) / (clip1.w - clip0.w);
        clip0 = lerp(clip0, clip1, clipped0);
    }
    else if (clip1.w < camera.zNear)
    {
        clipped1 = 1.0f - (camera.zNear - clip1.w) / (clip0.w - clip1.w);
        clip1 = lerp(clip0, clip1, clipped1);
    }

    const float2 size = float2((float)HAIR_VISIBILITY_WIDTH, (float)HAIR_VISIBILITY_HEIGHT);
    const float inverseW0 = 1.0f / clip0.w;
    const float inverseW1 = 1.0f / clip1.w;
    const float2 screen0 = float2(clip0.x * inverseW0 * 0.5f + 0.5f, 0.5f - clip0.y * inverseW0 * 0.5f) * size;
    const float2 screen1 = float2(clip1.x * inverseW1 * 0.5f + 0.5f, 0.5f - clip1.y * inverseW1 * 0.5f) * size;
    const float2 delta = screen1 - screen0;
    const float lengthSquared = dot(delta, delta);
    const float2 pixelCenter = float2(pixel) + 0.5f;
    const float screenParameter = lengthSquared > 1.0e-12f ? saturate(dot(pixelCenter - screen0, delta) / lengthSquared) : 0.0f;
    // Linear on screen to linear in space, then back onto the unclipped segment.
    const float clippedParameter = screenParameter * inverseW1 / lerp(inverseW0, inverseW1, screenParameter);
    hit.parameter = lerp(clipped0, clipped1, clippedParameter);

    hit.position = lerp(start.position, end.position, hit.parameter);
    hit.previousPosition = lerp(start.previousPosition, end.previousPosition, hit.parameter);
    hit.width = lerp(start.width, end.width, hit.parameter);
    const float3 along = end.position - start.position;
    hit.tangent = dot(along, along) > 1.0e-20f ? normalize(along) : float3(0.0f, 1.0f, 0.0f);

    // Across the strand: the pixel's offset from the centreline over the strand's half-width on screen. A strand under a
    // pixel wide was drawn a pixel wide, and the offset says nothing about where on it the pixel is.
    const float2 screenSide = lengthSquared > 1.0e-12f ? float2(-delta.y, delta.x) * rsqrt(lengthSquared) : float2(1.0f, 0.0f);
    const float halfWidthPixels =
        0.5f * hit.width * HairPixelsPerMetreAtUnitDepth(camera, size.y) * lerp(inverseW0, inverseW1, screenParameter);
    const float offset = dot(pixelCenter - (screen0 + delta * screenParameter), screenSide);
    hit.across = clamp(offset / max(halfWidthPixels, 0.5f), -1.0f, 1.0f) * saturate(halfWidthPixels * 2.0f - 1.0f);
    // The world direction that screenSide is the image of: across the strand, facing neither toward nor away.
    const float3 toCamera = normalize(camera.positionWorldSpace.xyz - hit.position);
    const float3 sideways = cross(hit.tangent, toCamera);
    hit.side = dot(sideways, sideways) > 1.0e-12f ? normalize(sideways) : float3(1.0f, 0.0f, 0.0f);
    const float4 sideClip = mul(float4(hit.side, 0.0f), camera.viewProjection);
    if (dot(float2(sideClip.x, -sideClip.y), screenSide) < 0.0f)
    {
        hit.side = -hit.side;
    }
    return hit;
}

// The normal the existing surface model shades a fibre with: the direction to the camera with its part along the strand
// removed, so the strand is lit as the side of a cylinder facing the viewer, turned across a strand wide enough to show
// its roundness.
float3 HairShadingNormal(HairHit hit, Camera camera)
{
    const float3 toCamera = normalize(camera.positionWorldSpace.xyz - hit.position);
    float3 facing = toCamera - hit.tangent * dot(toCamera, hit.tangent);
    facing = dot(facing, facing) > 1.0e-8f ? normalize(facing) : toCamera; // seen end on
    return normalize(facing * sqrt(saturate(1.0f - hit.across * hit.across)) + hit.side * hit.across);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void HairResolveCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= HAIR_VISIBILITY_WIDTH || pixel.y >= HAIR_VISIBILITY_HEIGHT)
    {
        return;
    }
    Texture2D<uint64_t> hairVisibility = ResourceDescriptorHeap[HAIR_VISIBILITY];
    const uint64_t key = hairVisibility[pixel];
    if (key == 0xFFFFFFFFFFFFFFFFull)
    {
        return;
    }

    const Camera camera = HairLoadMainCamera();
    const HairHit hit = HairReconstructHit(pixel, key, camera);
    StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
    const HairInstanceGPU instance = instances[hit.cluster.instanceIndex];
    const float3 normal = HairShadingNormal(hit, camera);
    // The tangent is here for a hair BSDF. The canonical surface has nowhere to put it: its targets hold one normal, a
    // roughness and a specular colour, and its two payloads are the coat and fuzz lobes'.
    const float3 tangent = hit.tangent;

    // The canonical surface (WriteCanonicalSurfaceSample in the renderer's canonicalSurface.hlsl): payload profile 0, so
    // the payload targets are not read for this pixel and are left alone.
    RWTexture2D<float4> baseColorOpacity = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::BaseColorOpacity)];
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    RWTexture2D<float4> specularAo = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::SpecularAo)];
    RWTexture2D<float4> emissive = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Emissive)];
    RWTexture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
    RWTexture2D<uint2> identity = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Identity)];
    RWStructuredBuffer<SARPSurfaceRecordV1> records = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Records)];
    baseColorOpacity[pixel] = float4(saturate(instance.baseColor), 1.0f);
    normalRoughness[pixel] = float4(normal, saturate(instance.roughness));
    specularAo[pixel] = float4(kHairSpecularF0.xxx, 1.0f);
    emissive[pixel] = 0.0f.xxxx;

    // Current minus previous position in NDC, both without jitter, as the renderer's own surfaces write it
    // (ComputeClodMotionVectorFromPrevWorld).
    const float4 clip = mul(mul(float4(hit.position, 1.0f), camera.view), camera.unjitteredProjection);
    const float4 previousClip = mul(mul(float4(hit.previousPosition, 1.0f), camera.prevView), camera.prevUnjitteredProjection);
    motion[pixel] = clip.xy / clip.w - previousClip.xy / previousClip.w;

    // One record a pixel, at the pixel's own index. Semantic family and payload profile 0, no flags; OpenPBR table row 0
    // supplies what the targets do not carry, as it does for SARP's grass.
    const uint recordIndex = pixel.y * HAIR_VISIBILITY_WIDTH + pixel.x;
    identity[pixel] = uint2(recordIndex, 0u);
    SARPSurfaceRecordV1 record = (SARPSurfaceRecordV1)0;
    record.sourceObjectId = uint2(0x52494148u, hit.cluster.instanceIndex); // "HAIR"
    record.sourceMaterialId = uint2(hit.cluster.clusterIndex, hit.strandIndex);
    records[recordIndex] = record;

    // Depth, in the three forms the frame holds it (PerViewPrimaryDepthCopyCS): linear view depth, and device depth for
    // the unjittered projection twice. The key's depth is the rasterizer's, at this pixel.
    const float depth = asfloat(HairVisKeyDepthBits(key) << 1u);
    const float deviceDepth = -camera.unjitteredProjection[2][2] + camera.unjitteredProjection[3][2] / depth;
    RWTexture2D<float> linearDepthTexture = ResourceDescriptorHeap[HAIR_LINEAR_DEPTH];
    RWTexture2D<float> projectedDepthTexture = ResourceDescriptorHeap[HAIR_PROJECTED_DEPTH];
    RWTexture2D<float> deviceDepthTexture = ResourceDescriptorHeap[HAIR_DEVICE_DEPTH];
    linearDepthTexture[pixel] = depth;
    projectedDepthTexture[pixel] = deviceDepth;
    deviceDepthTexture[pixel] = deviceDepth;

    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    InterlockedAdd(counters[HAIR_COUNTER_PIXELS_RESOLVED], 1u);
}

// hair.probe: one row for every HAIR_PROBE_STRIDE-th hair pixel each way, holding what the frame's targets have there
// after the resolve, behind a header with the frame's camera. HairRuntimeGPUTypes.h documents the layout.
[shader("compute")]
[numthreads(8, 8, 1)]
void HairProbeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> probe = ResourceDescriptorHeap[HAIR_PROBE];
    const Camera camera = HairLoadMainCamera();
    if (all(dispatchThreadId.xy == 0u))
    {
        probe[1] = HAIR_VISIBILITY_WIDTH;
        probe[2] = HAIR_VISIBILITY_HEIGHT;
        probe[3] = HAIR_PROBE_STRIDE;
        const float4x4 matrices[4] = { camera.view, camera.unjitteredProjection, camera.prevView, camera.prevUnjitteredProjection };
        for (uint m = 0u; m < 4u; ++m)
        {
            for (uint element = 0u; element < 16u; ++element)
            {
                probe[HAIR_PROBE_MATRIX_FIRST + m * 16u + element] = asuint(matrices[m][element / 4u][element % 4u]);
            }
        }
    }

    const uint2 pixel = dispatchThreadId.xy * HAIR_PROBE_STRIDE;
    if (pixel.x >= HAIR_VISIBILITY_WIDTH || pixel.y >= HAIR_VISIBILITY_HEIGHT)
    {
        return;
    }
    Texture2D<uint64_t> hairVisibility = ResourceDescriptorHeap[HAIR_VISIBILITY];
    const uint64_t key = hairVisibility[pixel];
    if (key == 0xFFFFFFFFFFFFFFFFull)
    {
        return;
    }
    uint row;
    InterlockedAdd(probe[0], 1u, row);
    if (row >= HAIR_PROBE_ROW_CAPACITY)
    {
        return;
    }
    Texture2D<float> linearDepthTexture = ResourceDescriptorHeap[HAIR_LINEAR_DEPTH];
    Texture2D<float> projectedDepthTexture = ResourceDescriptorHeap[HAIR_PROJECTED_DEPTH];
    Texture2D<float> deviceDepthTexture = ResourceDescriptorHeap[HAIR_DEVICE_DEPTH];
    Texture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
    Texture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    const float2 motionVector = motion[pixel];
    const uint base = HAIR_PROBE_HEADER_WORDS + row * HAIR_PROBE_ROW_WORDS;
    probe[base + 0u] = pixel.x | (pixel.y << 16u);
    probe[base + 1u] = (uint)key; // the segment id
    probe[base + 2u] = HairVisKeyDepthBits(key) << 1u; // the key's depth, as a float's bits
    probe[base + 3u] = asuint(linearDepthTexture[pixel]);
    probe[base + 4u] = asuint(projectedDepthTexture[pixel]);
    probe[base + 5u] = asuint(deviceDepthTexture[pixel]);
    probe[base + 6u] = asuint(motionVector.x);
    probe[base + 7u] = asuint(motionVector.y);
    // The stored normal's length (1 where the resolve wrote it) and the roughness.
    probe[base + 8u] = asuint(length(normalRoughness[pixel].xyz));
    probe[base + 9u] = asuint(normalRoughness[pixel].w);
}
