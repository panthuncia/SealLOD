// Stochastic lighting's estimator, kernel 2 of 3: the shadow rays (docs/stochastic_lighting.md "Kernel split"). Selection
// (stochasticSelect.hlsl) appended every ray that needs tracing to a compacted list; one thread here traces one of them and,
// when it reaches its light, sets its slot's bit in the sampled pixel's word for shading (stochasticShade.hlsl). The ray's
// origin is rebuilt from the surface history (linear depth and normal), as selection built it from the same surface.
#define LIGHTING_NO_VIRTUAL_SHADOW_MAPS 1

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/utilities.hlsli"
#include "include/lighting.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/materialFlags.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include "PerPassRootConstants/stochasticLightingRootConstants.h"
#include "include/clodRayTracingHits.hlsli"
#include "include/stochasticLighting.hlsli"
#include "include/stochasticSplit.hlsli"

// STOCHASTIC_DEBUG_PROBE 1 and 3 (the ray probes) run here. They are branches on a compile-time constant rather than #if, so
// the shader names the same resources with or without them and pso.recompile can switch them.
#if defined(STOCHASTIC_DEBUG_PROBE) && (STOCHASTIC_DEBUG_PROBE == 1 || STOCHASTIC_DEBUG_PROBE == 3)
#define STOCHASTIC_TRACE_PROBE STOCHASTIC_DEBUG_PROBE
#else
#define STOCHASTIC_TRACE_PROBE 0
#endif

// The trace's indirect arguments from the rays selection appended, and the count the trace reads. The counter itself is
// cleared by an upload before selection runs.
[numthreads(1, 1, 1)]
void StochasticTraceArgsCS()
{
    RWStructuredBuffer<uint> rayCounter = ResourceDescriptorHeap[STOCHASTIC_RAY_COUNTER_UAV];
    RWStructuredBuffer<uint> arguments = ResourceDescriptorHeap[STOCHASTIC_RAY_ARGS_UAV];
    const uint count = min(rayCounter[0], STOCHASTIC_RAY_CAPACITY);
    rayCounter[1] = count;
    arguments[0] = (count + STOCHASTIC_TRACE_GROUP_SIZE - 1u) / STOCHASTIC_TRACE_GROUP_SIZE;
    arguments[1] = 1u;
    arguments[2] = 1u;
}

// The ray probes (pso.recompile on Stochastic.Select.PSO and Stochastic.Trace.PSO with the same STOCHASTIC_DEBUG_PROBE,
// denoiser off): selection gives every sampled pixel with a surface one entry and no shadow rays, and the probe's value is
// written here as the pixel's output.
void StochasticTraceRayProbe(uint2 pixel, float3 positionVS, float3 positionWS, float3 normal, Camera camera, PerFrameBuffer perFrame)
{
    const uint downsample = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? 2u : 1u;
    const uint2 screen = uint2(perFrame.screenResX, perFrame.screenResY);
    float3 debugDiffuse = float3(-1.0f, -1.0f, 0.0f);
    float3 debugSpecular = 0.0f;
    RaytracingAccelerationStructure probeScene = ResourceDescriptorHeap[CLOD_RT_SEC_TLAS_SRV];
    StructuredBuffer<CLodRtClasInfo> probeClas = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
    if (STOCHASTIC_TRACE_PROBE == 3)
    {
        // Probe 3: one ray from the surface toward the sun's centre, no culling. Diffuse: hit, distance, the hit material's
        // flags. Specular: N.L.
        if (perFrame.sunLightIndex != LIGHT_INDEX_NONE)
        {
            StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
            const float3 toSun = normalize(-lights[perFrame.sunLightIndex].dirWorldSpace.xyz);
            RayDesc sunProbe;
            sunProbe.Origin = positionWS + normal * max(0.05f, length(positionVS) * 2e-3f);
            sunProbe.Direction = toSun;
            sunProbe.TMin = 0.0f;
            sunProbe.TMax = STOCHASTIC_DIRECTIONAL_RAY_LENGTH;
            const CLodRtHit sunHit = CLodRtTrace(probeScene, probeClas, sunProbe, RAY_FLAG_NONE, 0xFFu, 0.0f);
            uint hitFlags = 0u;
            if (sunHit.hit)
            {
                StructuredBuffer<PerMeshBuffer> probeMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
                StructuredBuffer<MaterialInfo> probeMaterials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
                hitFlags = probeMaterials[probeMeshes[LoadMeshTemplateForDraw(sunHit.drawRecord).perMeshBufferIndex].materialDataIndex].materialFlags;
            }
            debugDiffuse = float3(sunHit.hit ? 1.0f : 0.0f, sunHit.hit ? sunHit.t : -1.0f, float(hitFlags));
            debugSpecular = float3(dot(normal, toSun), 0.0f, 0.0f);
        }
    }
    else
    {
        // Probe 1: a raw camera ray through the pixel. Diffuse: committed hit with every candidate accepted, candidates seen,
        // hit distance. Specular: the same ray through CLodRtTrace.
        const RayDesc probeRay = CLodRtCameraRay(camera, pixel, float2(perFrame.screenResX, perFrame.screenResY));
        RayQuery<RAY_FLAG_NONE, CLOD_RT_RAYQUERY_FLAGS> probe;
        probe.TraceRayInline(probeScene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFFu, probeRay);
        uint candidates = 0u;
        while (probe.Proceed())
        {
            ++candidates;
            probe.CommitNonOpaqueTriangleHit();
        }
        debugDiffuse = float3(probe.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1.0f : 0.0f, float(candidates),
            probe.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? probe.CommittedRayT() : -1.0f);
        const CLodRtHit probeHit = CLodRtTrace(probeScene, probeClas, probeRay, RAY_FLAG_NONE, 0xFFu, 0.0f);
        debugSpecular = float3(probeHit.hit ? 1.0f : 0.0f, probeHit.hit ? probeHit.t : -1.0f, float(CLOD_RT_SEC_TLAS_SRV));
    }
    StochasticWriteDebugOutput(pixel / downsample, pixel, downsample, screen, debugDiffuse, debugSpecular);
}

// Visibility gradients (STOCHASTIC_FLAG_GRADIENT; stochasticLightingRootConstants.h, docs/stochastic_lighting.md "Visibility gradients"): one thread per cell re-traces the sample selection recorded there last frame, from the
// same origin and the same point of the emitter's [0,1)^2 domain, toward the light where it is now (the light alive: its
// generation unchanged). A change of visibility, weighted by the light's share of that pixel's unshadowed light, goes to the
// cell the origin projects to this frame (the largest wins). Dispatched after the estimator's rays, only with
// STOCHASTIC_FLAG_GRADIENT_VALID; it writes nothing the estimate reads.
[numthreads(STOCHASTIC_TRACE_GROUP_SIZE, 1, 1)]
void StochasticTraceGradientCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const uint downsample = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? 2u : 1u;
    const uint2 screen = uint2(perFrame.screenResX, perFrame.screenResY);
    const uint2 blocks = (screen + downsample - 1u) / downsample;
    const uint2 cells = StochasticGradientCells(blocks);
    if (id.x >= cells.x * cells.y)
    {
        return;
    }
    RWStructuredBuffer<uint> gradientBuffer = ResourceDescriptorHeap[STOCHASTIC_GRADIENT_UAV];
    const uint capacity = StochasticGradientCapacity(blocks);
    const uint base = StochasticGradientRecordBase(capacity, (CLOD_RT_SEC_FRAME & 1u) ^ 1u, id.x);
    const float share = asfloat(gradientBuffer[base + 6u]);
    const uint meta = gradientBuffer[base + 0u];
    if (!(share > 0.0f) || (meta & STOCHASTIC_GRADIENT_COMPLETE) == 0u)
    {
        return;
    }
    const uint lightIndex = meta & STOCHASTIC_ENTRY_INDEX_MASK;
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    const LightInfo light = lights[lightIndex];
    if (((meta >> 20u) & 0xFFu) != (light.generation & 0xFFu) || light.shadowCaster == 0u)
    {
        return; // Gone, or no longer shadowed: nothing to compare.
    }
    const float3 origin = float3(asfloat(gradientBuffer[base + 3u]), asfloat(gradientBuffer[base + 4u]), asfloat(gradientBuffer[base + 5u]));
    const float2 u = float2(asfloat(gradientBuffer[base + 1u]), asfloat(gradientBuffer[base + 2u]));
    RayDesc ray;
    if (!StochasticLightShadowRayFrom(light, origin, u, ray))
    {
        return; // Inside or against the emitter now.
    }
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const float visibleNow = StochasticTraceShadowRay(ray, camera, perFrame);
    const float visibleThen = (meta & STOCHASTIC_GRADIENT_VISIBLE) != 0u ? 1.0f : 0.0f;
    const float change = (visibleNow - visibleThen) * share;
    // Where the origin is this frame (the camera may have moved): its cell takes the gradient, and counts the record
    // whether or not it changed (the signed mean's denominator).
    const float4 clip = mul(float4(origin, 1.0f), camera.viewProjection);
    if (!(clip.w > 0.0f))
    {
        return;
    }
    const float2 ndc = clip.xy / clip.w;
    const float2 position = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * float2(screen);
    if (any(position < 0.0f) || any(position >= float2(screen)))
    {
        return;
    }
    const uint2 cell = min(uint2(position) / downsample / STOCHASTIC_GRADIENT_CELL, cells - 1u);
    const uint raw = StochasticGradientRawIndex(capacity, cell.y * cells.x + cell.x);
    if (change != 0.0f)
    {
        // Non-negative floats order as their bits.
        InterlockedMax(gradientBuffer[raw], asuint(abs(change)));
        InterlockedAdd(gradientBuffer[raw + 1u], uint(int(round(change * STOCHASTIC_GRADIENT_FIXED))));
    }
    InterlockedAdd(gradientBuffer[raw + 2u], 1u);
}

[numthreads(STOCHASTIC_TRACE_GROUP_SIZE, 1, 1)]
void StochasticTraceCS(uint3 id : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> rayCounter = ResourceDescriptorHeap[STOCHASTIC_RAY_COUNTER_UAV];
    if (id.x >= rayCounter[1])
    {
        return;
    }
    RWStructuredBuffer<uint3> rays = ResourceDescriptorHeap[STOCHASTIC_RAYS_UAV];
    const uint3 entry = rays[id.x];
    const uint2 pixel = StochasticRayPixel(entry.x);
    const uint slot = StochasticRaySlot(entry.x);

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const float2 resolution = float2(perFrame.screenResX, perFrame.screenResY);
    RWTexture2D<uint2> history = ResourceDescriptorHeap[STOCHASTIC_SURFACE_HISTORY_UAV];
    const uint2 surface = history[pixel];
    // The surface's position as StochasticLoadSurface rebuilds it, and the origin StochasticLightShadowRay offsets from it.
    const float linearZ = asfloat(surface.x);
    float2 uv = (float2(pixel) + 0.5f) / resolution;
    uv.y = 1.0f - uv.y;
    const float2 ndc = uv * 2.0f - 1.0f;
    const float3 positionVS = float3(ndc.x * camera.projectionInverse[0][0], ndc.y * camera.projectionInverse[1][1], -1.0f) * linearZ;
    const float3 positionWS = mul(float4(positionVS, 1.0f), camera.viewInverse).xyz;
    const float3 normal = StochasticUnpackNormal(surface.y);

    if (STOCHASTIC_TRACE_PROBE != 0)
    {
        StochasticTraceRayProbe(pixel, positionVS, positionWS, normal, camera, perFrame);
        return;
    }

    RayDesc ray;
    ray.Origin = positionWS + normal * max(0.05f, length(positionVS) * 2e-3f);
    ray.Direction = StochasticUnpackNormal(entry.y);
    ray.TMin = 0.0f;
    ray.TMax = asfloat(entry.z);
    if (StochasticTraceShadowRay(ray, camera, perFrame) > 0.0f)
    {
        const uint downsample = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? 2u : 1u;
        const uint blocksX = (perFrame.screenResX + downsample - 1u) / downsample;
        const uint2 block = pixel / downsample;
        RWStructuredBuffer<uint> pixelWords = ResourceDescriptorHeap[STOCHASTIC_PIXEL_WORDS_UAV];
        InterlockedOr(pixelWords[block.y * blocksX + block.x], 1u << slot);
    }
}
