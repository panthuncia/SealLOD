#ifndef __LIGHT_MODEL_HLSLI__
#define __LIGHT_MODEL_HLSLI__

// What an analytic light (LightInfo) delivers to a point, independent of how it is shaded or shadowed. Raster's light
// loop (lighting.hlsli), the path tracer (pathTracingLights.hlsli), ray traced shadows and reflections, and the lens
// flare sources all evaluate lights through these functions, so there is one definition of falloff, cone and sun size.

#include "include/structs.hlsli"
#include "include/constants.hlsli"

#define LIGHT_TYPE_POINT 0u
#define LIGHT_TYPE_SPOT 1u
#define LIGHT_TYPE_DIRECTIONAL 2u

#define LIGHT_INDEX_NONE 0xFFFFFFFFu

// Angular radius used where a directional light needs a disc but has no sourceHalfAngleDegrees: roughly the real sun.
#define LIGHT_DEFAULT_SUN_HALF_ANGLE_DEGREES 0.27f

// The closest a point light is treated as being, squared, when it has no source radius: keeps 1/d^2 finite.
#define LIGHT_MIN_DISTANCE_SQUARED 1.0e-4f

struct LightFragmentData {
    uint lightType;
    float3 lightPos;
    float3 lightColor;
    float intensity;
    float3 lightToFrag; // Unit vector from the surface toward the light.
    float attenuation; // Distance falloff; 1 for directional lights.
    float distance; // 0 for directional lights.
    float spotAttenuation; // 1 outside spot lights.
};

// Inverse square, bounded below by the emitter's radius and windowed to reach zero at range smoothly
// (the (1 - (d/r)^4)^2 window): a light has finite energy at any distance and no step at its range.
float LightDistanceFalloff(float distanceSquared, float range, float sourceRadius)
{
    const float minimumSquared = max(sourceRadius * sourceRadius, LIGHT_MIN_DISTANCE_SQUARED);
    const float inverseSquare = 1.0f / max(distanceSquared, minimumSquared);
    const float rangeRatioSquared = distanceSquared / max(range * range, 1.0e-8f);
    const float window = saturate(1.0f - rangeRatioSquared * rangeRatioSquared);
    return inverseSquare * window * window;
}

// Smooth falloff from the inner cone to the outer one. surfaceToLight and lightDirection are unit vectors.
float LightSpotFalloff(float3 surfaceToLight, float3 lightDirection, float outerConeCos, float innerConeCos)
{
    const float cosine = dot(lightDirection, -surfaceToLight);
    if (innerConeCos <= outerConeCos)
    {
        return cosine > outerConeCos ? 1.0f : 0.0f; // No soft edge; smoothstep would divide by zero.
    }
    return smoothstep(outerConeCos, innerConeCos, cosine);
}

// Whether a point/spot light reaches the point at all. Directional lights always do.
bool LightInRange(LightInfo light, LightFragmentData data)
{
    return light.type == LIGHT_TYPE_DIRECTIONAL || data.distance < light.range;
}

LightFragmentData EvaluateLightAtPoint(LightInfo light, float3 position)
{
    LightFragmentData result;
    result.lightType = light.type;
    result.lightPos = light.posWorldSpace.xyz;
    result.lightColor = light.color.xyz;
    result.intensity = light.color.w;
    result.spotAttenuation = 1.0f;

    if (light.type == LIGHT_TYPE_DIRECTIONAL)
    {
        result.lightToFrag = normalize(-light.dirWorldSpace.xyz);
        result.attenuation = 1.0f;
        result.distance = 0.0f;
        return result;
    }

    const float3 toLight = light.posWorldSpace.xyz - position;
    const float distanceSquared = dot(toLight, toLight);
    result.distance = sqrt(distanceSquared);
    result.lightToFrag = toLight / max(result.distance, 1.0e-20f);
    result.attenuation = LightDistanceFalloff(distanceSquared, light.range, light.sourceRadius);
    if (light.type == LIGHT_TYPE_SPOT)
    {
        result.spotAttenuation = LightSpotFalloff(result.lightToFrag, light.dirWorldSpace.xyz, light.outerConeCos, light.innerConeCos);
    }
    return result;
}

// Radiance arriving from the light, before the BRDF and N.L: color * intensity * falloff * cone.
float3 LightIncidentRadiance(LightFragmentData data)
{
    return data.lightColor * data.intensity * data.attenuation * data.spotAttenuation;
}

// Angular radius of a directional light's disc, in radians.
float LightSourceHalfAngleRadians(LightInfo light)
{
    const float degrees = light.sourceHalfAngleDegrees > 0.0f ? light.sourceHalfAngleDegrees : LIGHT_DEFAULT_SUN_HALF_ANGLE_DEGREES;
    return degrees * (PI / 180.0f);
}

void LightBuildOrthonormalBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    // Duff et al., "Building an Orthonormal Basis, Revisited".
    const float signZ = direction.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (signZ + direction.z);
    const float b = direction.x * direction.y * a;
    tangent = float3(1.0f + signZ * direction.x * direction.x * a, signZ * b, -signZ * direction.x);
    bitangent = float3(b, signZ + direction.y * direction.y * a, -direction.y);
}

// A direction drawn uniformly from the cone of halfAngleRadians around axis, from two uniform numbers in [0, 1).
float3 LightSampleConeDirection(float3 axis, float halfAngleRadians, float2 u)
{
    // 1 - cos(theta) through the half-angle identity. Directly, a sun-sized cone would lose most of
    // its precision to the distance between 1 and cos(theta).
    const float sinHalf = sin(0.5f * halfAngleRadians);
    const float oneMinusCos = u.x * 2.0f * sinHalf * sinHalf;
    const float sinTheta = sqrt(oneMinusCos * (2.0f - oneMinusCos));
    const float phi = 2.0f * PI * u.y;
    float3 tangent;
    float3 bitangent;
    LightBuildOrthonormalBasis(axis, tangent, bitangent);
    return normalize(axis * (1.0f - oneMinusCos) + (tangent * cos(phi) + bitangent * sin(phi)) * sinTheta);
}

// The sun (PerFrameCB::sunLightIndex): the first shadow-casting directional light, else the first directional one.
bool HasSunLight(PerFrameBuffer perFrame)
{
    return perFrame.sunLightIndex != LIGHT_INDEX_NONE;
}

#endif // __LIGHT_MODEL_HLSLI__
