// Reflections, first stage: trace one mirror ray per pixel and record what it hit. rayTracedReflectionShade.hlsl then
// shades the hits with the renderer's own material and light code. Every pixel gets radiance here, because the composite
// uses this texture in place of the environment: pixels that don't trace or don't hit get the environment itself.
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
#include "include/clodRayTracingReflectionHit.hlsli"

[numthreads(8, 8, 1)]
void CLodRtReflectionTraceCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (id.x >= perFrame.screenResX || id.y >= perFrame.screenResY) {
        return;
    }
    RWTexture2D<float4> output = ResourceDescriptorHeap[CLOD_RT_SEC_OUTPUT_UAV];
    RWStructuredBuffer<CLodRtReflectionHit> hits = ResourceDescriptorHeap[CLOD_RT_SEC_HITS_UAV];
    const uint hitIndex = id.y * perFrame.screenResX + id.x;
    CLodRtReflectionHit record = (CLodRtReflectionHit)0;
    record.drawRecord = CLOD_RT_REFLECTION_NO_HIT;

    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    const uint64_t key = visibility[id.xy];
    if (key == 0xFFFFFFFFFFFFFFFFull) {
        output[id.xy] = 0.0f.xxxx;
        hits[hitIndex] = record;
        return;
    }

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    Texture2D<float4> normals = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    const float4 normalRoughness = normals[id.xy];
    const float roughness = normalRoughness.w;
    const float3 normal = normalize(normalRoughness.xyz);

    const RayDesc primary = CLodRtCameraRay(camera, id.xy, float2(perFrame.screenResX, perFrame.screenResY));
    const float3 forward = -camera.viewInverse[2].xyz;
    const float distance = UnpackVisKeyDepth(key) / max(dot(primary.Direction, forward), 1e-3f);
    const float3 position = primary.Origin + primary.Direction * distance;
    const float3 direction = normalize(reflect(primary.Direction, all(isfinite(normal)) ? normal : -primary.Direction));

    float3 radiance = CLodRtSampleEnvironment(direction, roughness);
    const float roughnessCutoff = asfloat(CLOD_RT_SEC_ROUGHNESS_CUTOFF);
    if (all(isfinite(normal)) && roughness < roughnessCutoff) {
        RaytracingAccelerationStructure scene = ResourceDescriptorHeap[CLOD_RT_SEC_TLAS_SRV];
        StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
        const float pixelSpread = CLodRtPixelConeSpread(camera, float(perFrame.screenResY));
        RayDesc ray;
        ray.Origin = position + normal * max(0.05f, distance * 2e-3f);
        ray.Direction = direction;
        ray.TMin = 0.0f;
        ray.TMax = asfloat(CLOD_RT_SEC_MAX_DISTANCE);
        const CLodRtHit hit = CLodRtTrace(scene, clasInfo, ray, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFFu, pixelSpread);
        if (hit.hit) {
            record.drawRecord = hit.drawRecord;
            record.transformIndex = hit.transformIndex;
            record.clasSlot = hit.clasSlot;
            record.primitive = hit.primitive;
            record.frontFace = hit.frontFace ? 1u : 0u;
            record.barycentrics = hit.barycentrics;
            record.t = hit.t;
            // A mirror on a curved or rough surface spreads rays out, so the hit is seen through a wider footprint.
            record.coneWidth = (distance + hit.t) * pixelSpread + hit.t * roughness * roughness;
            record.origin = ray.Origin;
            record.direction = ray.Direction;
        }
    }
    output[id.xy] = float4(radiance, 1.0f);
    hits[hitIndex] = record;
}
