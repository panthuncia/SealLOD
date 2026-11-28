#ifndef HAIR_BSDF_HLSLI
#define HAIR_BSDF_HLSLI

// The fibre's response to light on the layered path, for the shade pass (hairShade.hlsl).
//
// The scattering function itself is Hair/HairBsdfShared.h, one text for this shader and for the CPU's tests: the R, TT
// and TRT lobes of the real-time Marschner model with Chiang's parameters. Its header says where every formula and
// constant comes from. It is laid out as
//   HairBsdfMakeLobes      what depends on the material alone, once per shading point
//   HairBsdfMakeGeometry   the angles of the tangent, the view and the light
//   HairBsdfLongitudinal   M, along the fibre
//   HairBsdfDistribution*  D, around the fibre, per lobe
//   HairBsdfAttenuation*   A, Fresnel and absorption, per lobe
//   HairBsdfEvaluate       the sum over the lobes a mask selects
//   HairBsdfEnvironmentSample  the environment as four directions and weights
// Hair/HairScatterShared.h, shared the same way, is what the hair between a light and the fibre and around the fibre
// does to that response: the dual scattering approximation, fed by the density volume's fibre counts
// (hairVolume.hlsli). HairBsdfEvaluateDual and HairBsdfEnvironmentWeightDual are its entry points.
// This file adds what is the shader's own: a strand's material from its instance's, and the entry points.

#include "HairShaders/hairStructs.hlsli"
#include "Hair/HairBsdfShared.h"
#include "Hair/HairScatterShared.h"

// A fibre's material at a shading point, for the direction it is seen from: its lobes, ready to evaluate.
struct HairFibreMaterial
{
    HairBsdfLobes lobes;
};

// PCG's output permutation (Jarzynski and Olano 2020).
uint HairBsdfHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The material of one strand of an instance, seen along `view` (a unit vector away from the point). `strandKey` is the same for every vertex of a strand in every frame (its
// cluster and its place in the cluster, which is its rank's): a strand's absorption and roughness differ from the
// instance's by a random share that is the strand's own.
HairFibreMaterial HairMakeFibreMaterial(HairInstanceGPU instance, uint strandKey, float3 tangent, float3 view)
{
    const uint hashed = HairBsdfHash(strandKey);
    // Two numbers in [-1, 1).
    const float melaninOffset = (float)(hashed & 0xFFFFu) * (2.0f / 65536.0f) - 1.0f;
    const float roughnessOffset = (float)(hashed >> 16u) * (2.0f / 65536.0f) - 1.0f;
    HairBsdfMaterial material;
    material.sigmaA = instance.sigmaA * max(1.0f + instance.melaninVariation * melaninOffset, 0.0f);
    material.betaM = saturate(instance.betaM * (1.0f + instance.roughnessVariation * roughnessOffset));
    material.betaN = instance.betaN;
    material.alpha = instance.alpha;
    material.eta = instance.eta;
    HairFibreMaterial fibre;
    fibre.lobes = HairBsdfForView(HairBsdfMakeLobes(material), tangent, view);
    return fibre;
}

// The hair between a light and a shading point, and around the point, for HairEvaluateBsdf: what the density volume's
// march found and how to use it.
struct HairLightPath
{
    bool hasVolume;         // false: the instance has no density volume this frame, and the rest is unused
    bool dualScattering;    // false: the fibres in the way only shadow (HAIR_VOLUME_FLAG_DUAL_SCATTERING off)
    float fibreCount;       // fibres a ray to the light is expected to cross (HairMarchVolume)
    float densityFactor;    // Zinke's d_f and d_b
    uint parts;             // HAIR_SCATTER_PART_*
    HairScatterRow scatter; // the instance's scatter table at the light's inclination to the fibre (HairLoadScatterRow)
};

// An instance's scatter table (HairScatterShared.h) at the sine of an inclination. `tables` holds
// HAIR_SCATTER_TABLE_ROWS rows per instance row.
HairScatterRow HairLoadScatterRow(StructuredBuffer<HairScatterRow> tables, uint instanceIndex, float sinTheta)
{
    const float coordinate = HairScatterTableCoordinate(sinTheta);
    const uint first = (uint)coordinate;
    const uint second = min(first + 1u, HAIR_SCATTER_TABLE_ROWS - 1u);
    const uint base = instanceIndex * HAIR_SCATTER_TABLE_ROWS;
    return HairScatterRowLerp(tables[base + first], tables[base + second], coordinate - (float)first);
}

// The fibre's response to light from one direction: the radiance leaving toward `view` per unit of irradiance arriving
// along `light` on a plane facing the light, so a caller multiplies it by the light's incident radiance and its
// visibility and by no cosine. All three directions are unit vectors in world space; `tangent` runs along the fibre from
// root to tip, `view` and `light` point away from the shading point; `view` is the one the material was made for.
// `lobeMask` is HAIR_LOBE_*.
//
// `path` is what the hair does to the light on its way and around the fibre (HairLightPath). Without a density volume
// the result is the single fibre's response, HairBsdfEvaluate, exactly.
float3 HairEvaluateBsdf(float3 tangent, float3 view, float3 light, HairFibreMaterial material, HairLightPath path, uint lobeMask)
{
    if (!path.hasVolume)
    {
        return HairBsdfEvaluate(material.lobes, tangent, view, light, lobeMask);
    }
    if (!path.dualScattering)
    {
        // The fibres in the way only shadow: the share of the light that met none.
        return HairBsdfEvaluate(material.lobes, tangent, view, light, lobeMask) * exp(-max(path.fibreCount, 0.0f));
    }
    return HairBsdfEvaluateDual(material.lobes, path.scatter, tangent, view, light, lobeMask, path.fibreCount, path.densityFactor, path.parts);
}

// For the path tracer, not implemented. Sample draws an incident direction for `view` with a probability density close
// to HairEvaluateBsdf's value (a lobe by its attenuation, then theta from the lobe's longitudinal function and phi from
// its azimuthal one, as pbrt's HairBxDF::Sample_f does), returns the density through `pdf` and the value over the density
// as the result. Pdf returns that density for a given direction, per unit solid angle.
float3 HairSampleBsdf(float3 tangent, float3 view, HairFibreMaterial material, float3 random, out float3 light, out float pdf);
float HairPdfBsdf(float3 tangent, float3 view, float3 light, HairFibreMaterial material);

#endif // HAIR_BSDF_HLSLI
