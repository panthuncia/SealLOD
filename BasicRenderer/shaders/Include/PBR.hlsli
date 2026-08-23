#ifndef __PBR_HLSLI__
#define __PBR_HLSLI__

#include "include/constants.hlsli"

// https://github.com/AcademySoftwareFoundation/MaterialX/blob/a578d8a9758f0a6eefc5a1a5c7ab10727ee11b2d/libraries/pbrlib/genglsl/lib/mx_microfacet_specular.glsl#L85
// Rational quadratic fit to Monte Carlo data for GGX directional albedo.
float3 mx_ggx_dir_albedo_analytic(float NdotV, float alpha, float3 F0, float3 F90)
{
    float x = NdotV;
    float y = alpha;
    float x2 = x * x;
    float y2 = y * y;
    float4 r = float4(0.1003, 0.9345, 1.0, 1.0) +
             float4(-0.6303, -2.323, -1.765, 0.2281) * x +
             float4(9.748, 2.229, 8.263, 15.94) * y +
             float4(-2.038, -3.748, 11.53, -55.83) * x * y +
             float4(29.34, 1.424, 28.96, 13.08) * x2 +
             float4(-8.245, -0.7684, -7.507, 41.26) * y2 +
             float4(-26.44, 1.436, -36.11, 54.9) * x2 * y +
             float4(19.99, 0.2913, 15.86, 300.2) * x * y2 +
             float4(-5.448, 0.6286, 33.37, -285.1) * x2 * y2;
    float2 AB = clamp(r.xy / r.zw, 0.0, 1.0);
    return F0 * AB.x + F90 * AB.y;
}

float3 mx_ggx_dir_albedo(float NdotV, float alpha, float3 F0, float3 F90)
{
    return mx_ggx_dir_albedo_analytic(NdotV, alpha, F0, F90);
}

float mx_ggx_dir_albedo(float NdotV, float alpha, float F0, float F90)
{
    return mx_ggx_dir_albedo(NdotV, alpha, float3(F0, F0, F0), float3(F90, F90, F90)).x;
}

// https://blog.selfshadow.com/publications/turquin/ms_comp_final.pdf
// Equations 14 and 16
float3 mx_ggx_energy_compensation(float NdotV, float alpha, float3 Fss) // Fss == Fms == F0 at high roughness, which is where this will be impactful anyway
{
    float Ess = mx_ggx_dir_albedo(NdotV, alpha, 1.0, 1.0);
    return 1.0 + Fss * (1.0 - Ess) / Ess;
}

/***
*** Implementation taken from https://github.com/google/filament/blob/main/shaders/src/surface_brdf.fs
***/

float3 F_Schlick(const float3 f0, float f90, float VoH)
{
    // Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"
    return f0 + (f90 - f0) * pow((1.0 - VoH), 5);
}

float3 F_Schlick(const float3 f0, float VoH)
{
    float f = pow(1.0 - VoH, 5.0);
    return f + f0 * (1.0 - f);
}

float F_Schlick(float f0, float f90, float VoH)
{
    return f0 + (f90 - f0) * pow((1.0 - VoH), 5);
}

float V_SmithGGXCorrelated(float roughness, float NoV, float NoL)
{
    // Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"
    float a2 = roughness * roughness;
    // TODO: lambdaV can be pre-computed for all the lights, it should be moved out of this function
    float lambdaV = NoL * sqrt((NoV - a2 * NoV) * NoV + a2);
    float lambdaL = NoV * sqrt((NoL - a2 * NoL) * NoL + a2);
    // At the horizon both lambda terms can be zero. The direct-light caller
    // ultimately multiplies the BRDF by NoL, but allowing infinity here turns
    // that physically-zero contribution into Inf * 0 = NaN and contaminates
    // later HDR/bloom passes.
    float v = 0.5 / max(lambdaV + lambdaL, 1.0e-6f);
    // a2=0 => v = 1 / 4*NoL*NoV   => min=1/4, max=+inf
    // a2=1 => v = 1 / 2*(NoL+NoV) => min=1/4, max=+inf
    // clamp to the maximum value representable in mediump
    return saturateMediump(v);
}

float D_GGX(float roughness, float NoH, const float3 h)
{
    // Walter et al. 2007, "Microfacet Models for Refraction through Rough Surfaces"

    // In mediump, there are two problems computing 1.0 - NoH^2
    // 1) 1.0 - NoH^2 suffers floating point cancellation when NoH^2 is close to 1 (highlights)
    // 2) NoH doesn't have enough precision around 1.0
    // Both problem can be fixed by computing 1-NoH^2 in highp and providing NoH in highp as well

    // However, we can do better using Lagrange's identity:
    //      ||a x b||^2 = ||a||^2 ||b||^2 - (a . b)^2
    // since N and H are unit vectors: ||N x H||^2 = 1.0 - NoH^2
    // This computes 1.0 - NoH^2 directly (which is close to zero in the highlights and has
    // enough precision).
    // Overall this yields better performance, keeping all computations in mediump

    float oneMinusNoHSquared = 1.0 - NoH * NoH;

    float a = NoH * roughness;
    float k = roughness / (oneMinusNoHSquared + a * a);
    float d = k * k * (1.0 / PI);
    return saturateMediump(d);
}

//------------------------------------------------------------------------------
// Specular BRDF dispatch
//------------------------------------------------------------------------------

float distribution(float roughness, float NoH, const float3 h)
{
#if BRDF_SPECULAR_D == SPECULAR_D_GGX
    return D_GGX(roughness, NoH, h);
#endif
}

float visibility(float roughness, float NoV, float NoL)
{
    return V_SmithGGXCorrelated(roughness, NoV, NoL);
}

float3 fresnel(const float3 f0, float LoH)
{
    float tmp = 50.0 * 0.33;
    float f90 = saturate(dot(f0, float3(tmp, tmp, tmp)));
    return F_Schlick(f0, f90, LoH);
}

float3 fresnel(const float3 f0, const float f90, float LoH)
{
    return F_Schlick(f0, f90, LoH);
}

float3 isotropicLobe(const float roughness, const float3 f0, const float3 h,
        float NoV, float NoL, float NoH, float LoH)
{

    float D = distribution(roughness, NoH, h);
    float V = visibility(roughness, NoV, NoL);
#if defined(MATERIAL_HAS_SPECULAR_COLOR_FACTOR) || defined(MATERIAL_HAS_SPECULAR_FACTOR) // TODO: Per-material specular factor
    vec3  F = fresnel(pixel.f0, pixel.f90, LoH);
#else
    float3 F = fresnel(f0, LoH);
#endif
    return (D * V) * F;
}

float Fd_Lambert()
{
    return 1.0 / PI;
}

float Fd_Burley(float roughness, float NoV, float NoL, float LoH)
{
    // Burley 2012, "Physically-Based Shading at Disney"
    float f90 = 0.5 + 2.0 * roughness * LoH * LoH;
    float lightScatter = F_Schlick(1.0, f90, NoL);
    float viewScatter = F_Schlick(1.0, f90, NoV);
    return lightScatter * viewScatter * (1.0 / PI);
}

// Energy conserving wrap diffuse term, does *not* include the divide by pi
float Fd_Wrap(float NoL, float w)
{
    float tmp = (1.0 + w);
    return saturate((NoL + w) / tmp*tmp);
}

//------------------------------------------------------------------------------
// Diffuse BRDF dispatch
//------------------------------------------------------------------------------

float diffuse(float roughness, float NoV, float NoL, float LoH)
{
//#if BRDF_DIFFUSE == DIFFUSE_LAMBERT
    return Fd_Lambert();
//#elif BRDF_DIFFUSE == DIFFUSE_BURLEY
//    return Fd_Burley(roughness, NoV, NoL, LoH);
//#endif
}

float3 specularLobe(const float roughness, const float3 f0, const float3 h,
        float NoV, float NoL, float NoH, float LoH)
{
    return isotropicLobe(roughness, f0, h, NoV, NoL, NoH, LoH);
}

float3 diffuseLobe(const float roughness, const float3 diffuseColor, float NoV, float NoL, float LoH)
{
    return diffuseColor * diffuse(roughness, NoV, NoL, LoH);
}

#endif // __PBR_HLSLI__
