// Sun shadows by ray. Writes occlusion (1 = shadowed) for the deferred pass, which combines it with the shadow maps that
// still cover whatever the BVH leaves out.
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/materialFlags.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include "include/clodRayTracingHits.hlsli"

// The sun's angular radius, roughly.
#define CLOD_RT_SUN_CONE_TANGENT 0.005f
#define CLOD_RT_SHADOW_MAX_DISTANCE 100000.0f

uint CLodRtPcgHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float3 CLodRtSunDirection()
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<unsigned int> activeLights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    for (uint i = 0u; i < perFrame.numLights; ++i) {
        const LightInfo light = lights[activeLights[i]];
        if (light.type == 2u && light.shadowCaster) {
            return normalize(-light.dirWorldSpace.xyz);
        }
    }
    return 0.0f.xxx;
}

[numthreads(8, 8, 1)]
void CLodRtShadowTraceCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (id.x >= perFrame.screenResX || id.y >= perFrame.screenResY) {
        return;
    }
    RWTexture2D<unorm float> output = ResourceDescriptorHeap[CLOD_RT_SEC_OUTPUT_UAV];
    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    const uint64_t key = visibility[id.xy];
    const float3 sun = CLodRtSunDirection();
    if (key == 0xFFFFFFFFFFFFFFFFull || dot(sun, sun) == 0.0f) {
        output[id.xy] = 0.0f;
        return;
    }

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const RayDesc primary = CLodRtCameraRay(camera, id.xy, float2(perFrame.screenResX, perFrame.screenResY));
    const float3 forward = -camera.viewInverse[2].xyz;
    const float distance = UnpackVisKeyDepth(key) / max(dot(primary.Direction, forward), 1e-3f);
    const float3 position = primary.Origin + primary.Direction * distance;

    Texture2D<float4> normals = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    const float3 normal = normalize(normals[id.xy].xyz);
    const float3 shadingNormal = all(isfinite(normal)) ? normal : sun;
    if (dot(shadingNormal, sun) <= 0.0f) {
        output[id.xy] = 1.0f; // Facing away: dark regardless, and tracing it would only add acne.
        return;
    }

    // A point on the sun's disk, different for each pixel and frame. Temporal accumulation downstream smooths it further.
    const uint seed = CLodRtPcgHash(id.x + id.y * 65521u + CLOD_RT_SEC_FRAME * 2039u);
    const float angle = float(seed & 0xFFFFu) / 65536.0f * 6.2831853f;
    const float radius = sqrt(float(seed >> 16u) / 65536.0f) * CLOD_RT_SUN_CONE_TANGENT;
    const float3 tangent = normalize(abs(sun.y) < 0.99f ? cross(sun, float3(0.0f, 1.0f, 0.0f)) : cross(sun, float3(1.0f, 0.0f, 0.0f)));
    const float3 bitangent = cross(sun, tangent);

    RayDesc ray;
    // Offset by the size of a pixel at this distance: the BVH can be at a coarser LOD than the surface it starts from.
    const float offset = max(0.05f, distance * 2e-3f);
    ray.Origin = position + shadingNormal * offset;
    ray.Direction = normalize(sun + tangent * (cos(angle) * radius) + bitangent * (sin(angle) * radius));
    ray.TMin = 0.0f;
    ray.TMax = CLOD_RT_SHADOW_MAX_DISTANCE;

    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[CLOD_RT_SEC_TLAS_SRV];
    StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
    const CLodRtHit hit = CLodRtTrace(scene, clasInfo, ray,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFFu,
        CLodRtPixelConeSpread(camera, float(perFrame.screenResY)));
    output[id.xy] = hit.hit ? 1.0f : 0.0f;
}

// One pass of depth-aware blur, enough to remove single-pixel noise in the penumbra without smearing across edges.
[numthreads(8, 8, 1)]
void CLodRtShadowFilterCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (id.x >= perFrame.screenResX || id.y >= perFrame.screenResY) {
        return;
    }
    Texture2D<float> input = ResourceDescriptorHeap[CLOD_RT_SEC_INPUT_SRV];
    RWTexture2D<unorm float> output = ResourceDescriptorHeap[CLOD_RT_SEC_OUTPUT_UAV];
    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    const float centerDepth = UnpackVisKeyDepth(visibility[id.xy]);
    float sum = 0.0f;
    float weights = 0.0f;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 sample = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(perFrame.screenResX - 1u, perFrame.screenResY - 1u));
            const float depth = UnpackVisKeyDepth(visibility[sample]);
            const float weight = abs(depth - centerDepth) <= 0.02f * max(centerDepth, 1.0f) ? 1.0f : 0.0f;
            sum += input[sample] * weight;
            weights += weight;
        }
    }
    output[id.xy] = weights > 0.0f ? sum / weights : input[id.xy];
}
