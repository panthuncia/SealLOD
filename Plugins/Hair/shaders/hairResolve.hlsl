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
// Probe only: the composite information target (Texture2D<float2>) on the layered path, where the probe runs after the
// composite; HAIR_INVALID_INDEX on the surface path.
#define HAIR_COMPOSITE_INFO UintRootConstant10

#include "HairShaders/hairHit.hlsli"

// The placeholder material: a rough dielectric with the groom's base colour. The hair BSDF replaces it.
static const float kHairSpecularF0 = 0.04f;

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
    if (HAIR_COMPOSITE_INFO != HAIR_INVALID_INDEX)
    {
        // The layered path: the composited hair alpha, and whether the pixel took hair's depth and motion vector.
        Texture2D<float2> compositeInfo = ResourceDescriptorHeap[HAIR_COMPOSITE_INFO];
        const float2 info = compositeInfo[pixel];
        probe[base + 8u] = asuint(info.x);
        probe[base + 9u] = asuint(((uint)(info.y * 255.0f + 0.5f) & HAIR_COMPOSITE_INFO_DEPTH) != 0u ? 1.0f : 0.0f);
        return;
    }
    // The stored normal's length (1 where the resolve wrote it) and the roughness.
    probe[base + 8u] = asuint(length(normalRoughness[pixel].xyz));
    probe[base + 9u] = asuint(normalRoughness[pixel].w);
}
