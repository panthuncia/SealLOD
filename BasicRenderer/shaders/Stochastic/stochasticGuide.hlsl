// Stochastic lighting's guide (docs/stochastic_lighting.md "Denoiser", "Overhaul phase 2", "Overhaul phase 5"): per pixel, at full
// resolution, a deterministic prediction of the unshadowed light the pixel's light grid froxel brings, the sun included, as the
// reference's loop (stochasticReference.hlsl) evaluates it but with no rays. Deterministic: no random numbers; the same surfaces
// and lights give the same value. In froxels of at most STOCHASTIC_GUIDE_SMALL_FROXEL lights (every froxel at 16-64 lights in
// the test scene) it is exact: every light through the estimator's own contribution function (StochasticEvaluateLightUnshadowed)
// over the population selection draws from (the froxel's run, directional lights included; a listed light outside the run is out
// of range and contributes 0 there too). In larger froxels the far-field guide (STOCHASTIC_GUIDE_FAR_FIELD, below) evaluates the
// lights near the pixel's 8x8 group, or carrying a large share of its light, exactly, and the many others as one virtual light per
// group, so its cost stops growing with the light count (2.7 ms at 256 and at 1024 lights, where the exact walk costs 4.6 and 21).
//
// The denoiser accumulates the noisy signal divided by the guide (shading divides at the sampled pixel) and multiplies by the
// guide at the output pixel after the spatial filter, averaging ratios of sums (sum S / sum G over the same weights). For a
// deterministic guide P > 0 that keeps the converged mean whatever P's accuracy where the ratio S / P does not vary with P at the
// averaging scale; the closer P is to S, the less is left to accumulate (and a P whose pixel-scale structure is not the light's,
// such as a guide without Fresnel, biases: "Overhaul phase 5").
//
// Output, R32G32_Float (fp16 can't hold 250 / (1 cm)^2), in the denoiser's demodulated domain
// (STOCHASTIC_DEMODULATION_FLOOR, StochasticDemodulatedLuminance in stochasticLightingRootConstants.h):
//   r = lum(sum of the lights' unshadowed diffuse / max(BaseColorOpacity.rgb, 0.02)), weights (0.25, 0.5, 0.25)
//   g = lum(sum of the lights' unshadowed specular / max(SpecularAo.rgb, 0.02)), the same weights
// Background pixels get 0.
// With STOCHASTIC_FLAG_MIXTURE (docs/stochastic_lighting.md "Guide-normalized accumulation"), also the light-mixture signature
// (STOCHASTIC_GUIDE_SIGNATURE_UAV, R32G32_UInt): each light's share of the pixel's light (GUIDE_SIGNATURE_MEASURE), summed into 8
// buckets by light (StochasticSignatureBucket), a byte each; per pixel, or averaged per 4x4 block (STOCHASTIC_SIGNATURE_LOWRES).
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

// Which normal the guide's lights are evaluated with. 0: the shading normal (normal-mapped), exactly what the estimator's
// samples use. 1: the geometric normal, from the depth buffer (STOCHASTIC_GUIDE_SHADING_FLOOR below keeps it positive where
// the samples are). The denoiser filters the ratio sample / guide across pixels (the reduced-resolution reconstruction, the
// spatial filter) and over jittered frames, then multiplies by the output pixel's guide; that is unbiased only where the
// ratio does not vary with the guide's own fine detail. Normal maps make the two anticorrelated at pixel scale (a bump facing
// the light has a large guide and is the likeliest to be shadowed by the coarser BVH), which brightened shadowed pixels.
#ifndef STOCHASTIC_GUIDE_NORMAL
#define STOCHASTIC_GUIDE_NORMAL 0
#endif
// With the geometric normal: the guide is at least this fraction of the shading normal's, so it is positive wherever a
// sample can be (a lobe whose guide is 0 is written as 0, which would lose light at the geometric terminator). 0: off (and
// the shading normal's guide is not evaluated).
#ifndef STOCHASTIC_GUIDE_SHADING_FLOOR
#define STOCHASTIC_GUIDE_SHADING_FLOOR 0.25f
#endif

// The view-space position at a pixel and linear depth, as StochasticLoadSurface rebuilds it.
float3 StochasticGuideViewPosition(int2 pixel, float linearZ, float2 resolution, Camera camera)
{
    float2 uv = (float2(pixel) + 0.5f) / resolution;
    uv.y = 1.0f - uv.y;
    const float2 ndc = uv * 2.0f - 1.0f;
    return float3(ndc.x * camera.projectionInverse[0][0], ndc.y * camera.projectionInverse[1][1], -1.0f) * linearZ;
}

// The geometric normal in world space from the linear depth, as the denoiser's DenoiseGeometricNormal: along each axis the
// neighbour nearer in depth, facing the camera. The shading normal where an axis has no valid neighbour.
float3 StochasticGuideGeometricNormal(uint2 pixel, float3 shadingNormal, PerFrameBuffer perFrame, Camera camera)
{
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    const int2 screen = int2(perFrame.screenResX, perFrame.screenResY);
    const float2 resolution = float2(screen);
    const int2 center = int2(pixel);
    const float centerDepth = depthTexture[center];
    const float3 centerVS = StochasticGuideViewPosition(center, centerDepth, resolution, camera);
    float3 axes[2];
    [unroll]
    for (uint axis = 0; axis < 2u; ++axis)
    {
        const int2 step = axis == 0u ? int2(1, 0) : int2(0, 1);
        const int2 plus = min(center + step, screen - 1);
        const int2 minus = max(center - step, int2(0, 0));
        const float depthPlus = depthTexture[plus];
        const float depthMinus = depthTexture[minus];
        const bool validPlus = asuint(depthPlus) != 0x7F7FFFFFu && any(plus != center);
        const bool validMinus = asuint(depthMinus) != 0x7F7FFFFFu && any(minus != center);
        const bool usePlus = validPlus && (!validMinus || abs(depthPlus - centerDepth) <= abs(depthMinus - centerDepth));
        axes[axis] = usePlus ? StochasticGuideViewPosition(plus, depthPlus, resolution, camera) - centerVS
            : validMinus ? centerVS - StochasticGuideViewPosition(minus, depthMinus, resolution, camera) : 0.0f;
    }
    float3 normalVS = cross(axes[0], axes[1]);
    const float lengthSquared = dot(normalVS, normalVS);
    if (lengthSquared <= 1e-20f)
    {
        return shadingNormal;
    }
    normalVS *= rsqrt(lengthSquared);
    if (dot(normalVS, centerVS) > 0.0f)
    {
        normalVS = -normalVS;
    }
    return normalize(mul(normalVS, (float3x3)camera.viewInverse));
}

// The light-mixture signature (STOCHASTIC_FLAG_MIXTURE; stochasticLightingRootConstants.h, docs/stochastic_lighting.md
// "Overhaul phase 4"): what each light's share is measured in. 0: its unshadowed radiance (diffuse + specular as shaded,
// luminance with the guide's weights), so a light's share is its share of what the pixel shows, and one signature serves
// both lobes' histories (the light whose visibility the history holds is the same for both). 1: its diffuse alone (no
// view dependence, so steadier under jitter where a normal map moves a highlight from light to light). 2: its incident
// radiance times a wrapped cosine, 0.5 + 0.5 N.L (no BRDF, and a normal map's bumps move it half as much as N.L, with no
// cut-off at the terminator). 2 by default: one signature serves both lobes either way, and with 0 or 1 the shares of lights
// low over the ground swing with the normal map from pixel to pixel and with the jitter from frame to frame, which read as a
// mixture change on 7-13% of the pixels of a still view (2: 0.07-0.5%, stochasticDenoise.hlsl DENOISE_MIXTURE_RADIUS).
// The shares need not match the guide's lobes: they only have to move when the light comes from other lights.
// 3: its incident radiance alone (no normal at all). The far-field guide (STOCHASTIC_GUIDE_FAR_FIELD, below) always uses 3,
// since it knows its far lights only at a point per group of pixels, not at each pixel's normal.
#ifndef STOCHASTIC_GUIDE_SIGNATURE_LOBES
#define STOCHASTIC_GUIDE_SIGNATURE_LOBES 2
#endif

// The far-field guide (docs/performance/stochastic-lighting-experiments.md "Overhaul phase 5"). 0: every pixel walks every light of its froxel through
// the full BRDF (phase 2's guide; its cost grows with the lights a froxel holds: 1.0 ms at 16 lights, 21 ms at 1024). 1: froxels of
// more than STOCHASTIC_GUIDE_SMALL_FROXEL lights (smaller ones keep the exact walk): each 8x8 group splits each of its froxels' lights,
// cooperatively, into exact and far ones relative to the box its pixels' surface points span and their share of its light
// (GuideNearWeight): an exact light (or the sun) is evaluated at every pixel, as before; the far ones are summed once
// per group into a first-order expansion of their irradiance about the box's centre (GuideAddFar) and turned into a light
// per pixel (GuideFarLight). The guide stays deterministic (no random numbers; the split and the sums depend only on this
// frame's surfaces and lights) and continuous (the split weight fades, the expansion's first order matches its neighbours'
// to second order). Only the paired ratio needs the exact guide (it assumes E[X] = G).
#ifndef STOCHASTIC_GUIDE_FAR_FIELD
#define STOCHASTIC_GUIDE_FAR_FIELD 1
#endif
#if STOCHASTIC_GUIDE_FAR_FIELD
#define GUIDE_SIGNATURE_MEASURE 3
#else
#define GUIDE_SIGNATURE_MEASURE STOCHASTIC_GUIDE_SIGNATURE_LOBES
#endif

// The guide's per-light term. 0: the full BRDF (EvaluateDirectLightContribution), exactly the estimator's contribution. 1: the
// cheap stand-in selection's importance uses (StochasticCheapImportance): Lambert diffuse and a GGX distribution term for
// specular, no Fresnel, masking or energy terms, so in the denoiser's demodulated domain radiance * N.L / pi and radiance *
// D / (4 N.V). It keeps what makes the guide useful (the distance falloff, N.L, the lobe's shape and the normal map) and
// leaves out factors that change slowly with angle; any deterministic positive guide keeps the stationary mean (the ratio of
// sums is S / G times G). Returned as radiance times the floored albedos, so the demodulation at the end is unchanged.
// 2 (the default since 2026-11-17): the middle term (below), GGX D, masking and Schlick Fresnel with Lambert diffuse and its
// energy terms, falling back per pixel to the full BRDF where the material needs it (STOCHASTIC_GUIDE_MID_FALLBACK).
#ifndef STOCHASTIC_GUIDE_CHEAP_BRDF
#define STOCHASTIC_GUIDE_CHEAP_BRDF 2
#endif
// With 2: also the coat and fuzz layers, per light as the full BRDF evaluates them (1), or none (0).
#ifndef STOCHASTIC_GUIDE_MID_LAYERS
#define STOCHASTIC_GUIDE_MID_LAYERS 0
#endif
// With 2: the diffuse. 0: Lambert times the pixel's energy compensation. 1: also the light's half of it (the table, as the
// full BRDF). 2: the full base diffuse (EON with the light's table), so only the specular is approximated.
#ifndef STOCHASTIC_GUIDE_MID_DIFFUSE
#define STOCHASTIC_GUIDE_MID_DIFFUSE 1
#endif
// With 2: a pixel whose material has anything the middle term leaves out (coat, fuzz, glint, metal, diffuse roughness) takes
// the full BRDF instead, a choice per pixel, so the guide is the exact one everywhere (to EON's 1e-4 floor at zero diffuse
// roughness, with STOCHASTIC_GUIDE_MID_DIFFUSE 1 and STOCHASTIC_GUIDE_MID_LAYERS 0).
#ifndef STOCHASTIC_GUIDE_MID_FALLBACK
#define STOCHASTIC_GUIDE_MID_FALLBACK 1
#endif
bool GuideMidMatchesFull(StochasticSurface surface)
{
    bool simple = surface.parameters.coatWeight <= 0.0f && surface.parameters.fuzzWeight <= 0.0f && surface.parameters.glintEnabled == 0u;
#if !defined(CLOD_FORWARD_METAL) || CLOD_FORWARD_METAL
    simple = simple && surface.baseState.metalSpecularWeight <= 0.0f;
#endif
#if STOCHASTIC_GUIDE_MID_DIFFUSE < 2 && (!defined(CLOD_FORWARD_DIFFUSE_ROUGHNESS) || CLOD_FORWARD_DIFFUSE_ROUGHNESS)
    simple = simple && surface.baseState.baseDiffuseRoughness <= 0.0f;
#endif
    return simple;
}
// Performance pass (docs/performance/stochastic-lighting-experiments.md "Performance pass (2026-11-17)"), each exact (the same values):
//   STOCHASTIC_GUIDE_PIXEL_BRDF 1: the full BRDF's per-view terms (the coat and fuzz states, five of the base layer's nine
//     table samples and its analytic directional albedo) built once per pixel (MakeOpenPBRSurfaceBrdfFrame), not per light.
//   STOCHASTIC_GUIDE_FRAGMENT_ALBEDO 1: the demodulation albedos from the surface already loaded (FragmentInfo's diffuseColor
//     and F0 are BaseColorOpacity.rgb and SpecularAo.rgb), not two more texture loads after the group's barriers.
//   STOCHASTIC_GUIDE_SKIP_EMPTY_FAR 1: a group none of whose pixels is in a large froxel skips the far field's clearing,
//     walks and barriers (at 16-64 lights, nearly every group).
#ifndef STOCHASTIC_GUIDE_PIXEL_BRDF
#define STOCHASTIC_GUIDE_PIXEL_BRDF 1
#endif
#ifndef STOCHASTIC_GUIDE_FRAGMENT_ALBEDO
#define STOCHASTIC_GUIDE_FRAGMENT_ALBEDO 1
#endif
#ifndef STOCHASTIC_GUIDE_SKIP_EMPTY_FAR
#define STOCHASTIC_GUIDE_SKIP_EMPTY_FAR 1
#endif
DirectLightContribution GuideContribution(LightFragmentData data, StochasticSurface surface, OpenPBRSurfaceBrdfFrame frame, float3 diffuseAlbedo,
    float3 specularAlbedo, bool mid = true)
{
#if STOCHASTIC_GUIDE_CHEAP_BRDF == 1
    // Never taken (no such flag): it keeps the full BRDF's resources (the OpenPBR tables) in the shader's interface, so
    // pso.recompile can switch between the two (a rebuild cannot change the resources a shader names).
    if ((STOCHASTIC_FLAGS & 0x80000000u) != 0u)
    {
        return EvaluateDirectLightContribution(data, surface.parameters, surface.baseState);
    }
    DirectLightContribution result = (DirectLightContribution)0;
    const float3 n = surface.parameters.normal;
    const float3 v = surface.parameters.viewDir;
    const float nl = dot(n, data.lightToFrag);
    const float nv = dot(n, v);
    if (nl <= 0.0f || nv <= 0.0f)
    {
        return result;
    }
    const float nh = saturate(dot(n, normalize(data.lightToFrag + v)));
    const float a2 = max(surface.parameters.specularAlpha * surface.parameters.specularAlpha, 1e-4f);
    const float denominator = nh * nh * (a2 - 1.0f) + 1.0f;
    const float distribution = a2 / (PI * denominator * denominator);
    const float3 radiance = max(LightIncidentRadiance(data), 0.0f);
    result.diffuse = radiance * (nl * (1.0f / PI)) * diffuseAlbedo;
    result.specular = radiance * (distribution / (4.0f * nv)) * specularAlbedo;
    result.total = result.diffuse + result.specular;
    return result;
#elif STOCHASTIC_GUIDE_CHEAP_BRDF == 2
    // The middle term (docs/stochastic_lighting.md "Speed options"): the base layer without the work that only
    // some materials need. Specular: one GGX D and correlated Smith masking shared by the dielectric and metal lobes, each
    // with its own Schlick Fresnel (the full BRDF's specularLobe), the dielectric's directional-albedo compensation and the
    // metal's multiple scattering from the per-pixel frame (the light's table complement taken as the average). Diffuse:
    // Lambert times both halves of the energy compensation (STOCHASTIC_GUIDE_MID_DIFFUSE). No coat, fuzz or glint
    // (STOCHASTIC_GUIDE_MID_LAYERS). With STOCHASTIC_GUIDE_MID_FALLBACK, mid is false for a pixel whose material has anything
    // left out and the full BRDF runs instead, so the guide is the exact one; without it, a never-taken branch keeps the full
    // BRDF's resources in the interface, as for 1.
#if STOCHASTIC_GUIDE_MID_FALLBACK
    if (!mid)
#else
    if ((STOCHASTIC_FLAGS & 0x80000000u) != 0u)
#endif
    {
        return EvaluateDirectLightContributionFrame(data, surface.parameters, surface.baseState, frame);
    }
    DirectLightContribution result = (DirectLightContribution)0;
    const float3 n = surface.parameters.normal;
    const float3 v = surface.parameters.viewDir;
    const float nl = saturate(dot(n, data.lightToFrag));
    const float nv = frame.baseView.NdotV;
    if (nl <= 0.0f || nv <= 0.0f)
    {
        return result;
    }
    const OpenPBRBaseLayerState state = surface.baseState;
    const float3 h = normalize(data.lightToFrag + v);
    const float nh = saturate(dot(n, h));
    const float lh = saturate(dot(data.lightToFrag, h));
    const float dv = D_GGX(state.specularAlpha, nh, h) * V_SmithGGXCorrelated(state.specularAlpha, nv, nl);
    float3 specular = state.dielectricSpecularWeight * fresnel(state.dielectricSpecularF0, lh) * frame.baseView.dielectricEnergyCompensation;
#if !defined(CLOD_FORWARD_METAL) || CLOD_FORWARD_METAL
    specular += state.metalSpecularWeight * fresnel(state.metalSpecularF0, lh);
    specular *= dv;
    specular += state.metalSpecularWeight * state.metalMultipleScatterScale * (min(frame.baseView.metalViewComplement, rcp(max(nl, 1.0e-4f))) / PI);
#else
    specular *= dv;
#endif
#if STOCHASTIC_GUIDE_MID_DIFFUSE == 0
    float3 diffuse = state.diffuseColor * (frame.baseView.diffuseViewCompensation / PI);
#else
    // The light's half of the diffuse energy compensation (one table, two samples), as the full BRDF has it.
    const float lightComplement = OpenPBRLookUpOpaqueDielectricEnergyComplement(state.weightedSpecularIor, state.specularAlpha, nl);
    const float diffuseEnergy = max(0.0f, frame.baseView.diffuseViewCompensation * lightComplement);
#if STOCHASTIC_GUIDE_MID_DIFFUSE == 2 && (!defined(CLOD_FORWARD_DIFFUSE_ROUGHNESS) || CLOD_FORWARD_DIFFUSE_ROUGHNESS)
    float3 diffuse = OpenPBRDiffuseEON(state.diffuseColor, state.baseDiffuseRoughness, nv, nl, dot(v, data.lightToFrag)) * diffuseEnergy;
#else
    float3 diffuse = state.diffuseColor * (diffuseEnergy / PI);
#endif
#endif
#if STOCHASTIC_GUIDE_MID_LAYERS
    // The coat and fuzz layers as the full BRDF has them (EvaluateOpenPBRSurfaceBrdfFrame): both attenuate the base, and
    // each adds its own lobe; each returns at once for a surface without it.
    const LightingParameters p = surface.parameters;
#if !defined(CLOD_FORWARD_COAT) || CLOD_FORWARD_COAT
    const OpenPBRCoatLayerState coatState = MakeOpenPBRCoatLayerState(state, p.coatColor, p.coatWeight, p.coatIor, p.coatRoughness, p.coatDarkening);
    float3 baseScale = OpenPBRCoatBaseLayerScaleComplete(coatState, nv, nl);
    float3 coatFr = 0.0f;
    if (coatState.presence > 0.0f)
    {
        coatFr = specularLobe(p.coatRoughness, p.coatF0, h, nv, nl, nh, lh)
            * (mx_ggx_energy_compensation(nv, p.coatRoughness, p.coatF0) * coatState.presence);
    }
#else
    float3 baseScale = 1.0f;
    const float3 coatFr = 0.0f;
#endif
#if !defined(CLOD_FORWARD_FUZZ) || CLOD_FORWARD_FUZZ
    const OpenPBRFuzzLayerState fuzzState = MakeOpenPBRFuzzLayerState(n, v, p.fuzzColor, p.fuzzWeight, p.fuzzRoughness);
    const float fuzzScale = OpenPBRFuzzBaseLayerScaleComplete(fuzzState, data.lightToFrag);
    const float3 fuzzFr = OpenPBRFuzzSheenBRDF(fuzzState, data.lightToFrag);
#else
    const float fuzzScale = 1.0f;
    const float3 fuzzFr = 0.0f;
#endif
    baseScale *= fuzzScale;
    diffuse *= baseScale;
    specular = specular * baseScale + coatFr * fuzzScale + fuzzFr;
#endif
    const float3 irradiance = LightIncidentRadiance(data) * nl;
    result.diffuse = diffuse * irradiance;
    result.specular = specular * irradiance;
    result.total = result.diffuse + result.specular;
    return result;
#elif STOCHASTIC_GUIDE_PIXEL_BRDF
    return EvaluateDirectLightContributionFrame(data, surface.parameters, surface.baseState, frame);
#else
    return EvaluateDirectLightContribution(data, surface.parameters, surface.baseState);
#endif
}

// The lights' summed unshadowed diffuse and specular at a surface; with mixture, each light's share measure (above) summed
// into its signature bucket (buckets 0-3 in bucketsLow, 4-7 in bucketsHigh).
void StochasticGuideSum(StochasticSurface surface, OpenPBRSurfaceBrdfFrame frame, LightGridRange lightRange, bool mixture, float3 diffuseAlbedo,
    float3 specularAlbedo, out float3 diffuse, out float3 specular, out float4 bucketsLow, out float4 bucketsHigh, bool mid = true)
{
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    diffuse = 0.0f;
    specular = 0.0f;
    bucketsLow = 0.0f;
    bucketsHigh = 0.0f;
    for (uint i = 0; i < lightRange.count; i++)
    {
        const uint lightIndex = LightGridLight(lightRange, i);
        const LightInfo light = lights[lightIndex];
        // StochasticEvaluateLightUnshadowed, inlined for the light's incident radiance (signature measure 2).
        const LightFragmentData data = EvaluateLightAtPoint(light, surface.position);
        if (LightInRange(light, data))
        {
            const DirectLightContribution contribution = GuideContribution(data, surface, frame, diffuseAlbedo, specularAlbedo, mid);
            const float3 d = max(contribution.diffuse, 0.0f);
            const float3 s = max(contribution.specular, 0.0f);
            diffuse += d;
            specular += s;
            if (mixture)
            {
#if GUIDE_SIGNATURE_MEASURE == 3
                const float share = StochasticDemodulatedLuminance(max(LightIncidentRadiance(data), 0.0f));
#elif GUIDE_SIGNATURE_MEASURE == 2
                const float share = StochasticDemodulatedLuminance(max(LightIncidentRadiance(data), 0.0f))
                    * saturate(0.5f + 0.5f * dot(surface.parameters.normal, data.lightToFrag));
#elif GUIDE_SIGNATURE_MEASURE == 1
                const float share = StochasticDemodulatedLuminance(d);
#else
                const float share = StochasticDemodulatedLuminance(d + s);
#endif
                const uint bucket = StochasticSignatureBucket(lightIndex, light.generation);
                bucketsLow += select(bucket == uint4(0u, 1u, 2u, 3u), share, 0.0f);
                bucketsHigh += select(bucket == uint4(4u, 5u, 6u, 7u), share, 0.0f);
            }
        }
    }
}

// The exact walk with the middle term (STOCHASTIC_GUIDE_CHEAP_BRDF 2) and its per-pixel fallback: the choice is made once per
// pixel, outside the loop, so each loop is compiled for one term (inside the loop, both terms' registers at once: slower than
// the full BRDF alone). The far field's exact lights choose per light (GUIDE_MID_FAR; 0: always the full BRDF).
#ifndef STOCHASTIC_GUIDE_MID_FAR
#define STOCHASTIC_GUIDE_MID_FAR 0
#endif
#if STOCHASTIC_GUIDE_CHEAP_BRDF == 2 && STOCHASTIC_GUIDE_MID_FALLBACK
#define GUIDE_SUM(surface, frame, range, mixture, da, sa, d, s, bl, bh)     if (GuideMidMatchesFull(surface)) { StochasticGuideSum(surface, frame, range, mixture, da, sa, d, s, bl, bh, true); }     else { StochasticGuideSum(surface, frame, range, mixture, da, sa, d, s, bl, bh, false); }
#define GUIDE_MID_FAR(surface) (STOCHASTIC_GUIDE_MID_FAR != 0 && GuideMidMatchesFull(surface))
#else
#define GUIDE_SUM(surface, frame, range, mixture, da, sa, d, s, bl, bh) StochasticGuideSum(surface, frame, range, mixture, da, sa, d, s, bl, bh)
#define GUIDE_MID_FAR(surface) true
#endif

// ---- The far-field guide (STOCHASTIC_GUIDE_FAR_FIELD).
//
// Per group, for each distinct froxel its pixels read (up to GUIDE_SLOTS; a pixel of a fifth froxel walks its lights alone):
// the box its pixels' surface points span (centre c, half-diagonal R) and their mean shading normal N0. Each light gets a weight
// n in [0, 1] (GuideNearWeight), the larger of two:
//   - near: 1 at d <= STOCHASTIC_GUIDE_NEAR_RATIO * R from c, fading to 0 at STOCHASTIC_GUIDE_FAR_RATIO * R, so a light whose
//     1 / d^2 varies quickly across the box (one skimming the surface) is exact;
//   - share: 1 where its incident radiance at c is at least STOCHASTIC_GUIDE_SHARE of the slot's point and spot lights' total,
//     fading to 0 at half that, so the few lights that carry most of the box's light (all of them at 16 lights in a froxel of 4)
//     are exact wherever they are;
// and directional lights 1. Every pixel evaluates n times each light with n > 0 exactly, as the exact guide does. The group sums
// (1 - n) times the rest, the many lights that each carry little, into one virtual directional light:
//   E(p, N) = sum_i e_i(p) max(0, N . L_i(p))  ~  max(0, N . G(p)),   G = sum_i h_i e_i L_i,
// h_i = smoothstep(-0.1, 0.1, N0 . L_i) (the lights above the box's mean horizon). That is exact while N and N0 see the same
// lights; a normal-mapped pixel differs only through lights near the horizon, which give little light. G is expanded to first
// order in p about c (GuideAddFar: G and its Jacobian), so its error at the box's edge is second order, about 3 (R / d)^2 of a
// light at d (19% at the far ratio, 4; ratios 3 / 6 until 2026-11-17 gave the same accuracy at 256 lights, docs "Speed options
// (2026-11-17)"), and so is the step between two groups' expansions. The virtual light is shaded through
// the full BRDF with the pixel's own normal (GuideFarLight), so the far lights' diffuse follows the normal map as the exact
// guide's does.
//
// Cost: the walk is shared by the group's 64 pixels (a froxel of 200 lights is about 3 lights a thread, twice: the total, then
// the split), and a pixel evaluates its exact lights plus one.
#ifndef STOCHASTIC_GUIDE_NEAR_RATIO
#define STOCHASTIC_GUIDE_NEAR_RATIO 2.0f
#endif
#ifndef STOCHASTIC_GUIDE_FAR_RATIO
#define STOCHASTIC_GUIDE_FAR_RATIO 4.0f
#endif
#ifndef STOCHASTIC_GUIDE_SHARE
#define STOCHASTIC_GUIDE_SHARE 0.125f
#endif
// The share test only runs in slots of at most this many lights: it needs a walk of its own (the total first), 1.2 ms at 1024
// lights, where a froxel holds ~200 lights and hardly any carries 3% of the light.
#ifndef STOCHASTIC_GUIDE_SHARE_MAX_LIGHTS
#define STOCHASTIC_GUIDE_SHARE_MAX_LIGHTS 64u
#endif
// The far light's floor, as a fraction of its radiance (GuideFarLight).
#define GUIDE_FAR_FLOOR 0.05f
// The box's half-diagonal is at least this (metres), so a group of one pixel still has a scale.
#define GUIDE_MIN_RADIUS 1e-3f
// The far lights' specular. 0: smooth, max(0, N0 . G) / pi in the guide's demodulated domain, with no structure of its own at
// pixel scale: the ratio of sums then blurs their highlights as the unnormalized denoiser does, instead of imprinting the
// virtual light's one highlight where the real lights have many. 1: the virtual light's specular through the BRDF.
#ifndef STOCHASTIC_GUIDE_FAR_SPECULAR
#define STOCHASTIC_GUIDE_FAR_SPECULAR 0
#endif

// Pixels whose froxel holds at most this many lights evaluate them all exactly, without the group's walk (at 16 lights, nearly
// every froxel): the far field pays only where there are lights to save.
#ifndef STOCHASTIC_GUIDE_SMALL_FROXEL
#define STOCHASTIC_GUIDE_SMALL_FROXEL 32u
#endif
// A slot is a froxel and a depth bin (STOCHASTIC_GUIDE_DEPTH_BINS per doubling of depth, about 15% each), so a group spanning a
// leaf and the ground behind it (one froxel: a slice is about 50% deep) gets a box per layer, not one box metres deep whose
// near radius would hold every light. A pixel whose slot does not fit joins its froxel's first slot (the box grows), or walks
// its lights alone if its froxel has none.
#define GUIDE_SLOTS 8u
#ifndef STOCHASTIC_GUIDE_DEPTH_BINS
#define STOCHASTIC_GUIDE_DEPTH_BINS 5.0f
#endif
#define GUIDE_MASK_WORDS 32u // A slot's exact lights as a bitmask over its run: froxels of up to 1024 lights.
#define GUIDE_MAX_SLOT_LIGHTS (GUIDE_MASK_WORDS * 32u)
#define GUIDE_EMPTY 0xFFFFFFFFu
#define GUIDE_MAX_WAVES 4u // 64 threads in waves of 16 or more.
// G (3), its Jacobian (symmetric: xx yy zz, xy xz yz), and the signature's 8 buckets (incident radiance).
#define GUIDE_FAR_TERMS 17u
#define GUIDE_NORMAL_SCALE 32767.0f

groupshared uint gs_slotCell[GUIDE_SLOTS];
groupshared uint2 gs_slotRange[GUIDE_SLOTS]; // First index, count.
groupshared uint gs_slotBounds[GUIDE_SLOTS * 6u]; // Order-preserving uints of the min xyz, then the max xyz.
groupshared int gs_slotNormal[GUIDE_SLOTS * 3u]; // The sum of the pixels' normals, fixed point.
groupshared uint gs_nearMask[GUIDE_SLOTS * GUIDE_MASK_WORDS];
groupshared float gs_far[GUIDE_MAX_WAVES * GUIDE_SLOTS * GUIDE_FAR_TERMS];
groupshared float gs_total[GUIDE_MAX_WAVES * GUIDE_SLOTS];
// Each slot's centre and radius, mean normal and share flag (1) or not (0), once the walk is done (the finalize step after the walk).
groupshared float4 gs_slotCenter[GUIDE_SLOTS];
groupshared float4 gs_slotNormal4[GUIDE_SLOTS];
groupshared uint gs_waveCounter;
groupshared uint gs_overflow; // More distinct slot keys than slots: every pixel of the group walks its lights alone.
// The low-resolution signature (STOCHASTIC_SIGNATURE_LOWRES): each 4x4 block's summed shares (two buckets' bytes per word,
// 16 bits each: at most 16 * 255) and its lit pixels.
groupshared uint gs_blockShares[4u * 4u];
groupshared uint gs_blockCount[4u];

uint GuideOrderedUint(float value)
{
    const uint bits = asuint(value);
    return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
}

float GuideOrderedFloat(uint ordered)
{
    return asfloat((ordered & 0x80000000u) != 0u ? (ordered & 0x7FFFFFFFu) : ~ordered);
}

struct GuideSlot
{
    bool share; // The share test applies (STOCHASTIC_GUIDE_SHARE_MAX_LIGHTS).
    float3 center;
    float radius;
    float3 normal; // The mean shading normal, unit length (or 0).
    float total; // The point and spot lights' summed incident radiance at the centre.
};

GuideSlot GuideLoadSlot(uint slot)
{
    GuideSlot result;
    const float3 boundsMin = float3(GuideOrderedFloat(gs_slotBounds[slot * 6u]), GuideOrderedFloat(gs_slotBounds[slot * 6u + 1u]),
        GuideOrderedFloat(gs_slotBounds[slot * 6u + 2u]));
    const float3 boundsMax = float3(GuideOrderedFloat(gs_slotBounds[slot * 6u + 3u]), GuideOrderedFloat(gs_slotBounds[slot * 6u + 4u]),
        GuideOrderedFloat(gs_slotBounds[slot * 6u + 5u]));
    result.share = STOCHASTIC_GUIDE_SHARE > 0.0f && gs_slotRange[slot].y <= STOCHASTIC_GUIDE_SHARE_MAX_LIGHTS;
    result.center = 0.5f * (boundsMin + boundsMax);
    result.radius = max(0.5f * length(boundsMax - boundsMin), GUIDE_MIN_RADIUS);
    const float3 normalSum = float3(gs_slotNormal[slot * 3u], gs_slotNormal[slot * 3u + 1u], gs_slotNormal[slot * 3u + 2u]);
    const float normalLength = length(normalSum);
    result.normal = normalLength > 0.0f ? normalSum / normalLength : 0.0f;
    float total = 0.0f;
    [unroll]
    for (uint wave = 0; wave < GUIDE_MAX_WAVES; ++wave)
    {
        total += gs_total[wave * GUIDE_SLOTS + slot];
    }
    result.total = total;
    return result;
}

// The slot as the pixels read it once the walk is done: the waves' far terms are summed into wave 0's by the finalize step after the walk.
GuideSlot GuideLoadFinalSlot(uint slot)
{
    GuideSlot result;
    const float4 center = gs_slotCenter[slot];
    const float4 normal = gs_slotNormal4[slot];
    result.center = center.xyz;
    result.radius = center.w;
    result.normal = normal.xyz;
    result.share = normal.w > 0.0f;
    result.total = gs_total[slot];
    return result;
}

// A point or spot light's incident radiance at a point (luminance with the guide's weights), as LightIncidentRadiance gives it.
float GuideIncident(LightInfo light, float3 position)
{
    const LightFragmentData data = EvaluateLightAtPoint(light, position);
    return LightInRange(light, data) ? StochasticDemodulatedLuminance(max(LightIncidentRadiance(data), 0.0f)) : 0.0f;
}

// How much of a light the pixels evaluate exactly (1) rather than in the far field (0).
float GuideNearWeight(LightInfo light, GuideSlot slot)
{
    if (light.type == LIGHT_TYPE_DIRECTIONAL)
    {
        return 1.0f;
    }
    const float ratio = length(light.posWorldSpace.xyz - slot.center) / slot.radius;
    const float near = saturate((STOCHASTIC_GUIDE_FAR_RATIO - ratio) / (STOCHASTIC_GUIDE_FAR_RATIO - STOCHASTIC_GUIDE_NEAR_RATIO));
    if (!slot.share)
    {
        return near;
    }
    const float share = slot.total > 0.0f
        ? saturate(2.0f * GuideIncident(light, slot.center) / (STOCHASTIC_GUIDE_SHARE * slot.total) - 1.0f) : 0.0f;
    return max(near, share);
}

// Adds weight times a point or spot light's far-field terms at the slot's centre: G (its incident radiance e there, luminance
// with the guide's weights, along its direction L, times the horizon weight) and G's Jacobian (lightModel.hlsli's falloff
// differentiated; the horizon weight and a spot's cone are taken at the centre only). Also its signature bucket's share,
// measured as e.
void GuideAddFar(LightInfo light, uint lightIndex, GuideSlot slot, float weight, inout float3 g, inout float3 jacobianDiagonal,
    inout float3 jacobianOff, inout float4 bucketsLow, inout float4 bucketsHigh)
{
    const float3 toLight = light.posWorldSpace.xyz - slot.center;
    const float s = max(dot(toLight, toLight), 1e-12f);
    const float d = sqrt(s);
    const float3 l = toLight / d;
    const float minimumSquared = max(light.sourceRadius * light.sourceRadius, LIGHT_MIN_DISTANCE_SQUARED);
    const float rangeSquared = max(light.range * light.range, 1.0e-8f);
    const float q = s / rangeSquared;
    const float window = saturate(1.0f - q * q);
    float intensity = StochasticDemodulatedLuminance(max(light.color.rgb, 0.0f)) * max(light.color.w, 0.0f);
    if (light.type == LIGHT_TYPE_SPOT)
    {
        intensity *= LightSpotFalloff(l, light.dirWorldSpace.xyz, light.outerConeCos, light.innerConeCos);
    }
    const float inverse = 1.0f / max(s, minimumSquared);
    const float e = intensity * window * window * inverse;
    // de/ds; the gradient of e is de/ds * (-2 toLight): along L, magnitude gradient = -2 d de/ds >= 0.
    const float dInverse = s > minimumSquared ? -inverse * inverse : 0.0f;
    const float dWindow = q < 1.0f ? -2.0f * q / rangeSquared : 0.0f;
    const float gradient = -2.0f * d * intensity * (window * window * dInverse + inverse * 2.0f * window * dWindow);
    const float horizon = all(slot.normal == 0.0f) ? 1.0f : smoothstep(-0.1f, 0.1f, dot(slot.normal, l));
    const float w = weight * horizon;
    g += w * e * l;
    // d(e L) / dp = (gradient + e / d) L L^T - (e / d) I.
    const float radial = w * (gradient + e / d);
    const float isotropic = w * e / d;
    jacobianDiagonal += radial * l * l - isotropic;
    jacobianOff += radial * float3(l.x * l.y, l.x * l.z, l.y * l.z);
    const uint bucket = StochasticSignatureBucket(lightIndex, light.generation);
    bucketsLow += select(bucket == uint4(0u, 1u, 2u, 3u), weight * e, 0.0f);
    bucketsHigh += select(bucket == uint4(4u, 5u, 6u, 7u), weight * e, 0.0f);
}

// The far field at a surface point, offset from the slot's centre: G expanded there, as a white directional light of radiance
// |G| from G / |G|, shaded. Adds radiance to diffuse (and to specular with STOCHASTIC_GUIDE_FAR_SPECULAR 1); returns the smooth
// specular (demodulated) for 0.
float GuideFarLight(StochasticSurface surface, OpenPBRSurfaceBrdfFrame frame, GuideSlot slot, float3 offset, float3 g, float3 jacobianDiagonal,
    float3 jacobianOff, float3 diffuseAlbedo, float3 specularAlbedo, inout float3 diffuse, inout float3 specular)
{
    const float3 here = g + float3(
        jacobianDiagonal.x * offset.x + jacobianOff.x * offset.y + jacobianOff.y * offset.z,
        jacobianOff.x * offset.x + jacobianDiagonal.y * offset.y + jacobianOff.z * offset.z,
        jacobianOff.y * offset.x + jacobianOff.z * offset.y + jacobianDiagonal.z * offset.z);
    const float radiance = length(here);
    if (!(radiance > 0.0f))
    {
        return 0.0f;
    }
    LightFragmentData data = (LightFragmentData)0;
    data.lightType = LIGHT_TYPE_DIRECTIONAL;
    data.lightColor = 1.0f;
    data.intensity = radiance;
    data.lightToFrag = here / radiance;
    data.attenuation = 1.0f;
    data.spotAttenuation = 1.0f;
    const DirectLightContribution c = GuideContribution(data, surface, frame, diffuseAlbedo, specularAlbedo, GUIDE_MID_FAR(surface));
    // A floor independent of the normal: where the pixel's normal faces away from G but not from every far light, the guide
    // must not be 0 (a lobe whose guide is 0 is written as 0 and loses its light; 0.1% of pixels at 64 lights without it).
    diffuse += max(c.diffuse, 0.0f) + (GUIDE_FAR_FLOOR / PI) * radiance * diffuseAlbedo;
#if STOCHASTIC_GUIDE_FAR_SPECULAR
    specular += max(c.specular, 0.0f) + (GUIDE_FAR_FLOOR / PI) * radiance * specularAlbedo;
    return 0.0f;
#else
    const float3 normal = all(slot.normal == 0.0f) ? surface.parameters.normal : slot.normal;
    return max(dot(normal, here), GUIDE_FAR_FLOOR * radiance) * (1.0f / PI);
#endif
}

[numthreads(8, 8, 1)]
void StochasticGuideCS(uint3 id : SV_DispatchThreadID, uint3 threadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const uint2 pixel = id.xy;
    const uint2 screen = uint2(perFrame.screenResX, perFrame.screenResY);
    const bool inside = all(pixel < screen);
    const bool mixture = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_MIXTURE) != 0u;
    // Every thread reaches every barrier below; `active` marks those with a surface.
    if (groupIndex < GUIDE_SLOTS)
    {
        gs_slotCell[groupIndex] = GUIDE_EMPTY;
        gs_slotRange[groupIndex] = 0u;
        [unroll]
        for (uint axis = 0; axis < 3u; ++axis)
        {
            gs_slotBounds[groupIndex * 6u + axis] = 0xFFFFFFFFu;
            gs_slotBounds[groupIndex * 6u + 3u + axis] = 0u;
            gs_slotNormal[groupIndex * 3u + axis] = 0;
        }
    }
#if !STOCHASTIC_GUIDE_SKIP_EMPTY_FAR
    for (uint word = groupIndex; word < GUIDE_SLOTS * GUIDE_MASK_WORDS; word += 64u)
    {
        gs_nearMask[word] = 0u;
    }
    for (uint term = groupIndex; term < GUIDE_MAX_WAVES * GUIDE_SLOTS * GUIDE_FAR_TERMS; term += 64u)
    {
        gs_far[term] = 0.0f;
    }
    if (groupIndex < GUIDE_MAX_WAVES * GUIDE_SLOTS)
    {
        gs_total[groupIndex] = 0.0f;
    }
#endif
    if (groupIndex < 16u)
    {
        gs_blockShares[groupIndex] = 0u;
    }
    if (groupIndex < 4u)
    {
        gs_blockCount[groupIndex] = 0u;
    }
    if (groupIndex == 0u)
    {
        gs_waveCounter = 0u;
        gs_overflow = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    StochasticSurface surface = (StochasticSurface)0;
    const bool active = inside && StochasticLoadSurface(pixel, perFrame, camera, surface);
    LightGridRange lightRange = (LightGridRange)0;
    uint slot = GUIDE_EMPTY;
    if (active)
    {
        lightRange = StochasticLoadLightRange(surface, perFrame, camera);
#if STOCHASTIC_GUIDE_FAR_FIELD
        // The paired ratio (STOCHASTIC_FLAG_PAIRED) needs the exact guide, G = E[X] (docs/performance/stochastic-lighting-experiments.md "Correctness pass (2026-11-17)"): no pixel takes a slot, so every group takes the exact walk.
        if (lightRange.count > STOCHASTIC_GUIDE_SMALL_FROXEL && lightRange.count <= GUIDE_MAX_SLOT_LIGHTS
            && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_PAIRED) == 0u)
        {
            // This pixel's slot among the group's (a lock-free set of up to GUIDE_SLOTS keys: froxel, depth bin).
            const uint cell = LightGridCellIndex(LightGridCell(surface.fragment.pixelCoords, surface.fragment.fragPosViewSpace.z, perFrame, camera), perFrame);
            const uint bin = uint(int(floor(log2(max(-surface.fragment.fragPosViewSpace.z, 1e-4f)) * STOCHASTIC_GUIDE_DEPTH_BINS))) & 63u;
            const uint key = (cell << 6u) | bin;
            [unroll]
            for (uint k = 0; k < GUIDE_SLOTS; ++k)
            {
                if (slot == GUIDE_EMPTY)
                {
                    uint previous;
                    InterlockedCompareExchange(gs_slotCell[k], GUIDE_EMPTY, key, previous);
                    if (previous == GUIDE_EMPTY || previous == key)
                    {
                        slot = k;
                        if (previous == GUIDE_EMPTY)
                        {
                            gs_slotRange[k] = uint2(lightRange.first, lightRange.count);
                        }
                    }
                }
            }
            if (slot == GUIDE_EMPTY)
            {
                // No slot left. Which keys won one is a race, so rather than join one (a result that would depend on the
                // race), the whole group takes the exact path (rare: nine depth layers or slices in 8x8 pixels).
                InterlockedOr(gs_overflow, 1u);
            }
        }
#endif
    }
#if STOCHASTIC_GUIDE_FAR_FIELD
    // Each slot's box and normal sum, reduced per wave first (one atomic per wave and slot instead of one per pixel).
    {
        bool pending = slot != GUIDE_EMPTY;
        const int3 normal = int3(round(surface.parameters.normal * GUIDE_NORMAL_SCALE));
        [loop]
        while (WaveActiveAnyTrue(pending))
        {
            if (pending)
            {
                const uint first = WaveReadLaneFirst(slot);
                if (slot == first)
                {
                    const float3 low = WaveActiveMin(surface.position);
                    const float3 high = WaveActiveMax(surface.position);
                    const int3 normalSum = WaveActiveSum(normal);
                    if (WaveIsFirstLane())
                    {
                        [unroll]
                        for (uint axis = 0; axis < 3u; ++axis)
                        {
                            InterlockedMin(gs_slotBounds[slot * 6u + axis], GuideOrderedUint(low[axis]));
                            InterlockedMax(gs_slotBounds[slot * 6u + 3u + axis], GuideOrderedUint(high[axis]));
                            InterlockedAdd(gs_slotNormal[slot * 3u + axis], normalSum[axis]);
                        }
                    }
                    pending = false;
                }
            }
        }
    }
#endif
    uint waveSlot = 0u;
    if (WaveIsFirstLane())
    {
        InterlockedAdd(gs_waveCounter, 1u, waveSlot);
    }
    waveSlot = min(WaveReadLaneFirst(waveSlot), GUIDE_MAX_WAVES - 1u);
    GroupMemoryBarrierWithGroupSync();
    const bool overflow = gs_overflow != 0u;
    slot = overflow ? GUIDE_EMPTY : slot;

#if STOCHASTIC_GUIDE_FAR_FIELD
    // Slots are taken in order, so slot 0 is empty exactly when no pixel of the group is in a large froxel. Uniform over the
    // group (groupshared, after the barrier), so the barriers inside are reached by every thread or none.
#if STOCHASTIC_GUIDE_SKIP_EMPTY_FAR
    const bool groupFar = !overflow && gs_slotCell[0] != GUIDE_EMPTY;
#else
    const bool groupFar = true;
#endif
    [branch]
    if (groupFar)
    {
#if STOCHASTIC_GUIDE_SKIP_EMPTY_FAR
    for (uint word = groupIndex; word < GUIDE_SLOTS * GUIDE_MASK_WORDS; word += 64u)
    {
        gs_nearMask[word] = 0u;
    }
    for (uint term = groupIndex; term < GUIDE_MAX_WAVES * GUIDE_SLOTS * GUIDE_FAR_TERMS; term += 64u)
    {
        gs_far[term] = 0.0f;
    }
    if (groupIndex < GUIDE_MAX_WAVES * GUIDE_SLOTS)
    {
        gs_total[groupIndex] = 0.0f;
    }
    GroupMemoryBarrierWithGroupSync();
#endif
    // ---- The group's walk, 64 lights at a time: each slot's point and spot lights' total incident radiance at its centre,
    // then the split into exact lights (the bitmask) and far ones (summed).
    [loop]
    for (uint k = 0; k < GUIDE_SLOTS && STOCHASTIC_GUIDE_SHARE > 0.0f && !overflow; ++k)
    {
        const uint2 slotRange = gs_slotRange[k];
        if (gs_slotCell[k] == GUIDE_EMPTY)
        {
            break;
        }
        if (slotRange.y > STOCHASTIC_GUIDE_SHARE_MAX_LIGHTS)
        {
            continue; // No share test (and no total) there.
        }
        const GuideSlot slotData = GuideLoadSlot(k);
        LightGridRange range;
        range.first = slotRange.x;
        range.count = slotRange.y;
        range.slice = 0u;
        float total = 0.0f;
        for (uint j = groupIndex; j < range.count; j += 64u)
        {
            const LightInfo light = lights[LightGridLight(range, j)];
            total += light.type == LIGHT_TYPE_DIRECTIONAL ? 0.0f : GuideIncident(light, slotData.center);
        }
        total = WaveActiveSum(total);
        if (WaveIsFirstLane())
        {
            gs_total[waveSlot * GUIDE_SLOTS + k] = total;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    [loop]
    for (uint k = 0; k < GUIDE_SLOTS && !overflow; ++k)
    {
        const uint2 slotRange = gs_slotRange[k];
        if (gs_slotCell[k] == GUIDE_EMPTY)
        {
            break;
        }
        if (slotRange.y > GUIDE_MAX_SLOT_LIGHTS)
        {
            continue;
        }
        const GuideSlot slotData = GuideLoadSlot(k);
        LightGridRange range;
        range.first = slotRange.x;
        range.count = slotRange.y;
        range.slice = 0u;
        float3 g = 0.0f, jacobianDiagonal = 0.0f, jacobianOff = 0.0f;
        float4 farLow = 0.0f, farHigh = 0.0f;
        for (uint j = groupIndex; j < range.count; j += 64u)
        {
            const uint lightIndex = LightGridLight(range, j);
            const LightInfo light = lights[lightIndex];
            const float near = GuideNearWeight(light, slotData);
            if (near > 0.0f)
            {
                InterlockedOr(gs_nearMask[k * GUIDE_MASK_WORDS + (j >> 5u)], 1u << (j & 31u));
            }
            if (near < 1.0f)
            {
                GuideAddFar(light, lightIndex, slotData, 1.0f - near, g, jacobianDiagonal, jacobianOff, farLow, farHigh);
            }
        }
        const float terms[GUIDE_FAR_TERMS] = { g.x, g.y, g.z, jacobianDiagonal.x, jacobianDiagonal.y, jacobianDiagonal.z,
            jacobianOff.x, jacobianOff.y, jacobianOff.z, farLow.x, farLow.y, farLow.z, farLow.w, farHigh.x, farHigh.y, farHigh.z, farHigh.w };
        [unroll]
        for (uint t = 0; t < GUIDE_FAR_TERMS; ++t)
        {
            if (t >= 9u && !mixture)
            {
                break; // The signature's buckets.
            }
            const float sum = WaveActiveSum(terms[t]);
            if (WaveIsFirstLane())
            {
                gs_far[(waveSlot * GUIDE_SLOTS + k) * GUIDE_FAR_TERMS + t] = sum;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // ---- Finalize: the waves' far terms summed into wave 0's, and each slot's data computed once, so a pixel reads a few values
    // instead of every wave's partial sums.
    {
        const uint waves = min(gs_waveCounter, GUIDE_MAX_WAVES);
        for (uint index = groupIndex; index < GUIDE_SLOTS * GUIDE_FAR_TERMS; index += 64u)
        {
            float sum = gs_far[index];
            for (uint wave = 1; wave < waves; ++wave)
            {
                sum += gs_far[wave * GUIDE_SLOTS * GUIDE_FAR_TERMS + index];
            }
            gs_far[index] = sum;
        }
        if (groupIndex < GUIDE_SLOTS)
        {
            const GuideSlot slotData = GuideLoadSlot(groupIndex);
            gs_slotCenter[groupIndex] = float4(slotData.center, slotData.radius);
            gs_slotNormal4[groupIndex] = float4(slotData.normal, slotData.share ? 1.0f : 0.0f);
        }
        GroupMemoryBarrierWithGroupSync();
        if (groupIndex < GUIDE_SLOTS)
        {
            float total = 0.0f;
            for (uint wave = 0; wave < waves; ++wave)
            {
                total += gs_total[wave * GUIDE_SLOTS + groupIndex];
            }
            gs_total[groupIndex] = total; // Read by GuideLoadFinalSlot only (wave 0's entries, after this barrier).
        }
        GroupMemoryBarrierWithGroupSync();
    }
    } // groupFar
#endif

    // ---- Per pixel.
    float3 diffuse = 0.0f, specular = 0.0f;
    float smoothSpecular = 0.0f; // Demodulated (STOCHASTIC_GUIDE_FAR_SPECULAR 0).
    float4 bucketsLow = 0.0f, bucketsHigh = 0.0f;
    // Demodulated as the denoiser demodulates (DenoiseLoadSurface), by the same textures at the same pixel.
    float3 diffuseAlbedo = 1.0f, specularAlbedo = 1.0f;
    if (active)
    {
#if STOCHASTIC_GUIDE_FRAGMENT_ALBEDO
        // GetFragmentInfoCanonicalSurface's diffuseColor and F0 are exactly these textures' rgb at this pixel.
        diffuseAlbedo = max(surface.fragment.diffuseColor, STOCHASTIC_DEMODULATION_FLOOR);
        specularAlbedo = max(surface.fragment.F0, STOCHASTIC_DEMODULATION_FLOOR);
#else
        Texture2D<float4> baseColor = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::BaseColorOpacity)];
        Texture2D<float4> specularAo = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::SpecularAo)];
        diffuseAlbedo = max(baseColor[pixel].rgb, STOCHASTIC_DEMODULATION_FLOOR);
        specularAlbedo = max(specularAo[pixel].rgb, STOCHASTIC_DEMODULATION_FLOOR);
#endif
    }
    if (active)
    {
        // Built here, after the group's barriers, so its terms are not held through them.
        const OpenPBRSurfaceBrdfFrame frame = MakeOpenPBRSurfaceBrdfFrame(surface.parameters, surface.baseState);
#if STOCHASTIC_GUIDE_FAR_FIELD
        if (slot != GUIDE_EMPTY)
        {
            const GuideSlot slotData = GuideLoadFinalSlot(slot);
            // Exact lights, in the run's order (deterministic sums), each times its weight.
            const uint words = (lightRange.count + 31u) >> 5u;
            for (uint w = 0; w < words; ++w)
            {
                uint bits = gs_nearMask[slot * GUIDE_MASK_WORDS + w];
                while (bits != 0u)
                {
                    const uint bit = firstbitlow(bits);
                    bits &= bits - 1u;
                    const uint lightIndex = LightGridLight(lightRange, w * 32u + bit);
                    const LightInfo light = lights[lightIndex];
                    const float near = GuideNearWeight(light, slotData);
                    const LightFragmentData data = EvaluateLightAtPoint(light, surface.position);
                    if (LightInRange(light, data))
                    {
                        const DirectLightContribution contribution = GuideContribution(data, surface, frame, diffuseAlbedo, specularAlbedo, GUIDE_MID_FAR(surface));
                        diffuse += near * max(contribution.diffuse, 0.0f);
                        specular += near * max(contribution.specular, 0.0f);
                        if (mixture)
                        {
                            const float share = near * StochasticDemodulatedLuminance(max(LightIncidentRadiance(data), 0.0f));
                            const uint bucket = StochasticSignatureBucket(lightIndex, light.generation);
                            bucketsLow += select(bucket == uint4(0u, 1u, 2u, 3u), share, 0.0f);
                            bucketsHigh += select(bucket == uint4(4u, 5u, 6u, 7u), share, 0.0f);
                        }
                    }
                }
            }
            // The far field: the waves' sums, expanded about the centre.
            const uint farBase = slot * GUIDE_FAR_TERMS;
            smoothSpecular = GuideFarLight(surface, frame, slotData, surface.position - slotData.center,
                float3(gs_far[farBase], gs_far[farBase + 1u], gs_far[farBase + 2u]),
                float3(gs_far[farBase + 3u], gs_far[farBase + 4u], gs_far[farBase + 5u]),
                float3(gs_far[farBase + 6u], gs_far[farBase + 7u], gs_far[farBase + 8u]), diffuseAlbedo, specularAlbedo, diffuse, specular);
            if (mixture)
            {
                bucketsLow += float4(gs_far[farBase + 9u], gs_far[farBase + 10u], gs_far[farBase + 11u], gs_far[farBase + 12u]);
                bucketsHigh += float4(gs_far[farBase + 13u], gs_far[farBase + 14u], gs_far[farBase + 15u], gs_far[farBase + 16u]);
            }
        }
        else
        {
            GUIDE_SUM(surface, frame, lightRange, mixture, diffuseAlbedo, specularAlbedo, diffuse, specular, bucketsLow, bucketsHigh);
        }
#elif STOCHASTIC_GUIDE_NORMAL
        float3 shadingDiffuse = 0.0f, shadingSpecular = 0.0f;
        if (STOCHASTIC_GUIDE_SHADING_FLOOR > 0.0f)
        {
            float4 unusedLow, unusedHigh;
            StochasticGuideSum(surface, frame, lightRange, false, diffuseAlbedo, specularAlbedo, shadingDiffuse, shadingSpecular, unusedLow, unusedHigh);
        }
        {
            StochasticSurface geometric = surface;
            const float3 n = StochasticGuideGeometricNormal(pixel, surface.fragment.normalWS, perFrame, camera);
            geometric.fragment.normalWS = n;
            geometric.fragment.NdotV = max(MIN_N_DOT_V, dot(n, geometric.fragment.viewWS));
            geometric.fragment.reflectedWS = reflect(-geometric.fragment.viewWS, n);
            geometric.parameters = MakeLightingParameters(geometric.fragment);
            geometric.baseState = MakeOpenPBRBaseLayerStateForLighting(geometric.parameters);
            StochasticGuideSum(geometric, MakeOpenPBRSurfaceBrdfFrame(geometric.parameters, geometric.baseState), lightRange, mixture, diffuseAlbedo, specularAlbedo, diffuse, specular, bucketsLow, bucketsHigh);
        }
#else
        GUIDE_SUM(surface, frame, lightRange, mixture, diffuseAlbedo, specularAlbedo, diffuse, specular, bucketsLow, bucketsHigh);
#endif
    }

    // ---- The signature: shares of the total, rounded to the nearest 1/255 (the same lights on the same surface give the same
    // signature); per pixel, or averaged over the pixel's 4x4 block (STOCHASTIC_SIGNATURE_LOWRES).
    float4 sharesLow = 0.0f, sharesHigh = 0.0f;
    bool lit = false;
    if (mixture && active)
    {
        const float total = dot(bucketsLow + bucketsHigh, 1.0f);
        lit = total > 0.0f && total < 3.0e38f;
        const float scale = lit ? 1.0f / total : 0.0f;
        sharesLow = bucketsLow * scale;
        sharesHigh = bucketsHigh * scale;
    }
#if STOCHASTIC_SIGNATURE_LOWRES
    if (mixture)
    {
        const uint block = (threadId.y >> 2u) * 2u + (threadId.x >> 2u);
        if (lit)
        {
            const uint4 qLow = uint4(min(floor(saturate(sharesLow) * 255.0f + 0.5f), 255.0f));
            const uint4 qHigh = uint4(min(floor(saturate(sharesHigh) * 255.0f + 0.5f), 255.0f));
            InterlockedAdd(gs_blockShares[block * 4u + 0u], qLow.x | (qLow.y << 16u));
            InterlockedAdd(gs_blockShares[block * 4u + 1u], qLow.z | (qLow.w << 16u));
            InterlockedAdd(gs_blockShares[block * 4u + 2u], qHigh.x | (qHigh.y << 16u));
            InterlockedAdd(gs_blockShares[block * 4u + 3u], qHigh.z | (qHigh.w << 16u));
            InterlockedAdd(gs_blockCount[block], 1u);
        }
        GroupMemoryBarrierWithGroupSync();
        if (all((threadId.xy & 3u) == 0u) && inside)
        {
            const uint count = gs_blockCount[block];
            const uint4 packed = uint4(gs_blockShares[block * 4u], gs_blockShares[block * 4u + 1u], gs_blockShares[block * 4u + 2u],
                gs_blockShares[block * 4u + 3u]);
            const float scale = count > 0u ? 1.0f / (255.0f * float(count)) : 0.0f;
            const float4 low = float4(packed.x & 0xFFFFu, packed.x >> 16u, packed.y & 0xFFFFu, packed.y >> 16u) * scale;
            const float4 high = float4(packed.z & 0xFFFFu, packed.z >> 16u, packed.w & 0xFFFFu, packed.w >> 16u) * scale;
            RWTexture2D<uint2> signature = ResourceDescriptorHeap[STOCHASTIC_GUIDE_SIGNATURE_UAV];
            signature[pixel / STOCHASTIC_SIGNATURE_BLOCK] = count > 0u ? StochasticPackSignature(low, high, 0.5f, 0.5f) : 0u;
        }
    }
#else
    if (mixture && inside)
    {
        RWTexture2D<uint2> signature = ResourceDescriptorHeap[STOCHASTIC_GUIDE_SIGNATURE_UAV];
        signature[pixel] = lit ? StochasticPackSignature(sharesLow, sharesHigh, 0.5f, 0.5f) : 0u;
    }
#endif

    if (!inside)
    {
        return;
    }
    RWTexture2D<float2> guide = ResourceDescriptorHeap[STOCHASTIC_GUIDE_UAV];
    if (!active)
    {
        guide[pixel] = 0.0f;
        return;
    }
    const float2 value = float2(StochasticDemodulatedLuminance(diffuse / diffuseAlbedo),
        StochasticDemodulatedLuminance(specular / specularAlbedo) + smoothSpecular);
    // Finite and non-negative whatever a light record holds.
    guide[pixel] = select(and(value > 0.0f, value < 3.0e38f), value, 0.0f);
}
