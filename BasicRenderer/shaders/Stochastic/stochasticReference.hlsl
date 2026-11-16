// Stochastic lighting's reference (docs/stochastic_lighting.md "Reference"): every light in the pixel's light grid froxel, the sun
// included, each shadowed by one fresh ray toward a point on its sphere (a direction in its disc for directional lights),
// averaged over frames while nothing changes. It converges to the shading convention the stochastic estimator is unbiased
// for, so the estimator is checked against it. The deferred pass adds its output in place of the light loop.
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

// Past this many samples the running average stops moving (float precision); the image has converged long before.
#define STOCHASTIC_REFERENCE_MAX_SAMPLES 65536.0f
// 1: the output is this frame's estimate (every light, one ray each), not the running average, which keeps running. The
// average is jitter-averaged, so the upscaler takes it as already antialiased and reconstructs edges and texture differently
// from a frame whose lighting jitters with the rest of the image; this frame's estimate jitters with them, and with 1 cm
// emitters it is noisy only in penumbrae. The truth after DLSS for the post-DLSS noise metric (docs/stochastic_lighting.md
// "Noise after DLSS").
#ifndef STOCHASTIC_REFERENCE_PER_FRAME
#define STOCHASTIC_REFERENCE_PER_FRAME 0
#endif
// Each light's visibility from a G x G stratified grid of rays over its emitter instead of one ray. With PER_FRAME, the sun's
// penumbrae (its disc is far wider than the 1 cm point lights) are otherwise noisy enough that DLSS treats them as it treats
// the estimator's noise, and its output of them is no truth.
#ifndef STOCHASTIC_REFERENCE_RAY_GRID
#define STOCHASTIC_REFERENCE_RAY_GRID 1
#endif

void StochasticReferenceAccumulateLight(
    uint lightIndex,
    StochasticSurface surface,
    Camera camera,
    PerFrameBuffer perFrame,
    inout StochasticRng rng,
    inout float3 diffuse,
    inout float3 specular)
{
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    const LightInfo light = lights[lightIndex];
    DirectLightContribution contribution;
    if (!StochasticEvaluateLightUnshadowed(light, surface, contribution) || all(contribution.total <= 0.0f))
    {
        return;
    }
    float visibility = 1.0f;
    if (light.shadowCaster != 0u)
    {
        visibility = 0.0f;
        for (uint ray = 0; ray < STOCHASTIC_REFERENCE_RAY_GRID * STOCHASTIC_REFERENCE_RAY_GRID; ++ray)
        {
            const float2 cell = float2(ray % STOCHASTIC_REFERENCE_RAY_GRID, ray / STOCHASTIC_REFERENCE_RAY_GRID);
            const float2 u = (cell + StochasticRngNext2(rng)) / float(STOCHASTIC_REFERENCE_RAY_GRID);
            visibility += StochasticLightVisibility(light, surface, u, camera, perFrame);
        }
        visibility /= float(STOCHASTIC_REFERENCE_RAY_GRID * STOCHASTIC_REFERENCE_RAY_GRID);
    }
    diffuse += contribution.diffuse * visibility;
    specular += contribution.specular * visibility;
}

[numthreads(8, 8, 1)]
void StochasticReferenceCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (id.x >= perFrame.screenResX || id.y >= perFrame.screenResY)
    {
        return;
    }
    const uint2 pixel = id.xy;
    RWTexture2D<float4> outputDiffuse = ResourceDescriptorHeap[STOCHASTIC_DIFFUSE_UAV];
    RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[STOCHASTIC_SPECULAR_UAV];
    RWTexture2D<float4> accumulationDiffuse = ResourceDescriptorHeap[STOCHASTIC_ACCUMULATION_DIFFUSE_UAV];
    RWTexture2D<float4> accumulationSpecular = ResourceDescriptorHeap[STOCHASTIC_ACCUMULATION_SPECULAR_UAV];

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    StochasticSurface surface;
    if (!StochasticLoadSurface(pixel, perFrame, camera, surface))
    {
        outputDiffuse[pixel] = 0.0f;
        outputSpecular[pixel] = 0.0f;
        accumulationDiffuse[pixel] = 0.0f;
        accumulationSpecular[pixel] = 0.0f;
        return;
    }

    StochasticRng rng = StochasticRngCreate(pixel, CLOD_RT_SEC_FRAME, 0u);
    float3 diffuse = 0.0f;
    float3 specular = 0.0f;

    const LightGridRange lightRange = StochasticLoadLightRange(surface, perFrame, camera);
    for (uint i = 0; i < lightRange.count; i++)
    {
        StochasticReferenceAccumulateLight(LightGridLight(lightRange, i), surface, camera, perFrame, rng, diffuse, specular);
    }

    // A running average: rgb the mean so far, a the number of samples in it.
    const bool restart = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_RESTART_ACCUMULATION) != 0u;
    const float4 previousDiffuse = restart ? 0.0f : accumulationDiffuse[pixel];
    const float4 previousSpecular = restart ? 0.0f : accumulationSpecular[pixel];
    const float count = min(previousDiffuse.a + 1.0f, STOCHASTIC_REFERENCE_MAX_SAMPLES);
    const float3 meanDiffuse = lerp(previousDiffuse.rgb, diffuse, 1.0f / count);
    const float3 meanSpecular = lerp(previousSpecular.rgb, specular, 1.0f / count);
    accumulationDiffuse[pixel] = float4(meanDiffuse, count);
    accumulationSpecular[pixel] = float4(meanSpecular, count);
#if STOCHASTIC_REFERENCE_PER_FRAME
    outputDiffuse[pixel] = float4(diffuse, 1.0f);
    outputSpecular[pixel] = float4(specular, 1.0f);
#else
    outputDiffuse[pixel] = float4(meanDiffuse, 1.0f);
    outputSpecular[pixel] = float4(meanSpecular, 1.0f);
#endif
}
