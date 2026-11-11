#ifndef __PATH_TRACING_LIGHTS_HLSLI__
#define __PATH_TRACING_LIGHTS_HLSLI__

#include "include/lighting.hlsli"
#include "include/pathTracingRandom.hlsli"

// The narrow light interface of the reference path tracer. A stochastic light system replaces what
// is behind it; callers only rely on the two functions below. Lights are evaluated by lightModel.hlsli,
// the same model raster's light loop uses.

// Shadow ray TMax for directional lights.
#define PATH_LIGHT_SUN_DISTANCE 1.0e30f

// Estimates direct light with one shadow ray:
//     PathBsdfEval(..., sample.direction) * sample.radiance * visibility / sample.pdf
//
// radiance has the units of raster's light loop: color * intensity * attenuation * spot. pdf is the
// discrete selection probability only. A directional light's direction is drawn uniformly from its
// cone, and that uniform density cancels against spreading the light's irradiance over the same
// cone, so the two never need to appear: the estimate converges to raster's value averaged over the cone.
struct PathLightSample
{
    float3 direction; // From the surface toward the light.
    float distance; // Shadow ray TMax.
    float3 radiance;
    float pdf;
    bool valid; // False means no contribution and no shadow ray is needed.
};

// Picks the sun (PerFrameCB::sunLightSlot) with probability 1/2 and otherwise one of the remaining active
// lights uniformly, or the only available choice when one of those two groups is empty. A half split
// is deliberately simple: this is one sample per hit, and the sun is what lights most of an exterior
// while a uniform draw over many local lights gives each a small chance, so neither should starve.
//
// Further directional lights stay in the uniform group, so every active light has a nonzero
// probability and the estimate is unbiased. Selection ignores position and normal: a choice that
// lands behind the surface or out of range is an invalid, zero-contribution sample.
PathLightSample PathSampleOneLight(float3 position, float3 normal, inout PathRng rng)
{
    PathLightSample result = (PathLightSample)0;

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<unsigned int> activeLightIndices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];

    const uint lightCount = perFrame.numLights;
    if (lightCount == 0u)
    {
        return result;
    }

    const uint sunSlot = perFrame.sunLightSlot < lightCount ? perFrame.sunLightSlot : lightCount;
    const bool hasSun = sunSlot < lightCount;
    const uint otherCount = hasSun ? lightCount - 1u : lightCount;
    const float sunProbability = !hasSun ? 0.0f : (otherCount == 0u ? 1.0f : 0.5f);

    uint selectedSlot = sunSlot;
    if (PathRngNext1(rng) < sunProbability)
    {
        result.pdf = sunProbability;
    }
    else
    {
        selectedSlot = min(uint(PathRngNext1(rng) * float(otherCount)), otherCount - 1u);
        if (selectedSlot >= sunSlot)
        {
            ++selectedSlot; // Skips the sun. When there is none, sunSlot == lightCount and this never runs.
        }
        result.pdf = (1.0f - sunProbability) / float(otherCount);
    }

    const LightInfo light = lights[activeLightIndices[selectedSlot]];
    const LightFragmentData fragmentLight = EvaluateLightAtPoint(light, position);
    result.radiance = LightIncidentRadiance(fragmentLight);
    if (light.type == LIGHT_TYPE_DIRECTIONAL)
    {
        result.direction = LightSampleConeDirection(fragmentLight.lightToFrag, LightSourceHalfAngleRadians(light), PathRngNext2(rng));
        result.distance = PATH_LIGHT_SUN_DISTANCE;
    }
    else
    {
        if (!LightInRange(light, fragmentLight) || fragmentLight.distance <= 0.0f)
        {
            return result;
        }
        result.direction = fragmentLight.lightToFrag;
        result.distance = fragmentLight.distance;
    }

    result.valid = dot(normal, result.direction) > 0.0f && any(result.radiance > 0.0f);
    return result;
}

// Radiance of the environment along a ray that left the scene, from the raw HDR cube. The
// prefiltered map is 8-bit and would clip the sun and sky.
float3 PathSampleSky(float3 direction)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<EnvironmentInfo> environments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Environment::InfoBuffer)];
    TextureCube<float4> sky = ResourceDescriptorHeap[environments[perFrame.activeEnvironmentIndex].cubeMapDescriptorIndex];
    return sky.SampleLevel(g_linearClamp, direction, 0.0f).rgb;
}

#endif // __PATH_TRACING_LIGHTS_HLSLI__
