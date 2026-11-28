#ifndef BR_HAIR_BSDF_SHARED_H
#define BR_HAIR_BSDF_SHARED_H

// The hair fibre's scattering function, written once for C++ and HLSL: the shade pass includes it through
// hairBsdf.hlsli, and HairCoreTests checks this same text on the CPU. Like HairLodShared.h the bodies use only spellings
// both languages have, and intrinsics go through the macros below.
//
// The model is the real-time form of Marschner et al. 2003: three lobes, R (reflected off the cuticle), TT (through the
// fibre) and TRT (through, reflected inside, and out again), each a longitudinal function M of the inclinations to the
// fibre times an azimuthal function N around it, N being a distribution D times an attenuation A (Fresnel and
// absorption). It follows Karis, "Physically Based Hair Shading in Unreal" (SIGGRAPH 2016 course; "Karis" below, slide
// numbers are the PDF's pages) with the changes of Tafuri, "Strand-based Hair Rendering in Frostbite" (SIGGRAPH 2019;
// "Frostbite"), and takes its parameters and their mappings from Chiang et al. 2016 as implemented by pbrt-v4's
// HairBxDF ("pbrt": src/pbrt/bxdfs.h and bxdfs.cpp). Everything marked "ours" is neither's.
//
// The parameter set is Chiang's, so a path tracer's HairBxDF can take the same material:
//   sigmaA  absorption coefficient of the fibre's interior per colour channel, with distance measured in fibre
//           diameters (pbrt's sigma_a)
//   betaM   longitudinal roughness in [0, 1] (pbrt's beta_m)
//   betaN   azimuthal roughness in [0, 1] (pbrt's beta_n)
//   alpha   the cuticle scales' tilt, radians (pbrt's alpha, which it takes in degrees)
//   eta     index of refraction (pbrt's eta)
// Chiang's model is near-field (it takes the offset h across the fibre); this one is far-field, integrated over h.
//
// Conventions. `tangent` runs along the fibre from root to tip; `view` and `light` point away from the shading point;
// all three are unit vectors. The inclination theta of a direction is its angle above the plane normal to the fibre,
// toward the tip: sin(theta) = dot(tangent, direction). phi is the angle between the two directions around the fibre.
// The scales' faces are tilted toward the root, so the cone of directions R reflects into is turned toward the root
// and TT's and TRT's toward the tip, with the signs Karis gives (slide 44). On hair that drapes over a head the white
// highlight then sits toward the tips from the coloured one.
//
// What HairBsdfEvaluate returns is the radiance leaving toward `view` per unit of irradiance arriving from `light` on a
// plane facing the light: sum of M_p A_p N_p. That is pbrt's f times |cos(theta_i)|: the cosine is included, a caller
// multiplies by the light's incident radiance and nothing else. Over all incident directions it integrates (with the
// solid angle measure) to the fibre's albedo for that view.

#ifdef __cplusplus
#include "Hair/HairGPUTypes.h"

#include <cmath>

#define HAIR_SHARED_INLINE inline
#define HAIR_BSDF_SQRT(x) std::sqrt(x)
#define HAIR_BSDF_EXP(x) std::exp(x)
#define HAIR_BSDF_LOG(x) std::log(x)
#define HAIR_BSDF_SIN(x) std::sin(x)
#define HAIR_BSDF_ACOS(x) std::acos(x)
#define HAIR_BSDF_TANH(x) std::tanh(x)
#define HAIR_BSDF_DOT(a, b) Dot(a, b)
#define HAIR_BSDF_CROSS(a, b) Cross(a, b)
#define HAIR_BSDF_FLOAT3(x, y, z) Float3{ x, y, z }
#define HAIR_BSDF_OUT(type) type&

namespace br::hair {
#else
#ifndef HAIR_SHARED_INLINE
#define HAIR_SHARED_INLINE
#endif
#define HAIR_BSDF_SQRT(x) sqrt(x)
#define HAIR_BSDF_EXP(x) exp(x)
#define HAIR_BSDF_LOG(x) log(x)
#define HAIR_BSDF_SIN(x) sin(x)
#define HAIR_BSDF_ACOS(x) acos(x)
#define HAIR_BSDF_TANH(x) tanh(x)
#define HAIR_BSDF_DOT(a, b) dot(a, b)
#define HAIR_BSDF_CROSS(a, b) cross(a, b)
#define HAIR_BSDF_FLOAT3(x, y, z) float3(x, y, z)
#define HAIR_BSDF_OUT(type) out type
#endif

#define HAIR_BSDF_PI 3.14159265358979f

// The lobes HairBsdfEvaluate adds up (its lobeMask), for the debug views.
#define HAIR_LOBE_R 1u
#define HAIR_LOBE_TT 2u
#define HAIR_LOBE_TRT 4u
#define HAIR_LOBE_ALL 7u

// The smallest longitudinal roughness the evaluation uses. Hair is lit at strand vertices and the colours interpolated,
// so a highlight narrower than the turn of the fibre between two vertices would be missed or flicker. Ours.
#define HAIR_BSDF_MIN_BETA_M 0.05f

// The directions HairBsdfEnvironmentSample integrates the environment over.
#define HAIR_BSDF_ENVIRONMENT_SAMPLES 4u

// A fibre's material: Chiang's parameters (above).
struct HairBsdfMaterial {
    float3 sigmaA;
    float betaM;
    float betaN;
    float alpha;
    float eta;
};

HAIR_SHARED_INLINE float HairBsdfClamp(float x, float low, float high)
{
    return x < low ? low : (x > high ? high : x);
}

HAIR_SHARED_INLINE float3 HairBsdfExp3(float3 v)
{
    return HAIR_BSDF_FLOAT3(HAIR_BSDF_EXP(v.x), HAIR_BSDF_EXP(v.y), HAIR_BSDF_EXP(v.z));
}

// Absorption from pigment concentrations. Chiang et al. 2016, as pbrt's HairBxDF::SigmaAFromConcentration: eumelanin
// (brown to black) absorbs (0.419, 0.697, 1.37) per unit of concentration and pheomelanin (orange to red)
// (0.187, 0.4, 1.05), in RGB.
HAIR_SHARED_INLINE float3 HairSigmaAFromMelanin(float eumelanin, float pheomelanin)
{
    return HAIR_BSDF_FLOAT3(0.419f, 0.697f, 1.37f) * eumelanin + HAIR_BSDF_FLOAT3(0.187f, 0.4f, 1.05f) * pheomelanin;
}

// The same from an amount of melanin and the share of it that is pheomelanin. Ours: Chiang's two concentrations,
// eumelanin = melanin * (1 - redness) and pheomelanin = melanin * redness.
HAIR_SHARED_INLINE float3 HairSigmaAFromMelaninRedness(float melanin, float redness)
{
    return HairSigmaAFromMelanin(melanin * (1.0f - redness), melanin * redness);
}

// Absorption that gives a head of hair about the colour `c` (in (0, 1]) after multiple scattering. Chiang et al. 2016's
// fit, as pbrt's HairBxDF::SigmaAFromReflectance: sigma_a = (ln c / (5.969 - 0.215 b + 2.532 b^2 - 10.73 b^3 + 5.574 b^4
// + 0.245 b^5))^2 with b the azimuthal roughness.
HAIR_SHARED_INLINE float HairSigmaAFromColorChannel(float c, float betaN)
{
    const float b = betaN;
    const float denominator = 5.969f + b * (-0.215f + b * (2.532f + b * (-10.73f + b * (5.574f + b * 0.245f))));
    const float root = HAIR_BSDF_LOG(HairBsdfClamp(c, 1.0e-4f, 1.0f)) / denominator;
    return root * root;
}

HAIR_SHARED_INLINE float3 HairSigmaAFromColor(float3 color, float betaN)
{
    return HAIR_BSDF_FLOAT3(HairSigmaAFromColorChannel(color.x, betaN), HairSigmaAFromColorChannel(color.y, betaN),
        HairSigmaAFromColorChannel(color.z, betaN));
}

// The standard deviation of the R lobe's longitudinal Gaussian from betaM: the square root of Chiang's variance,
// v = (0.726 b + 0.812 b^2 + 3.7 b^20)^2 (pbrt's HairBxDF constructor). TT takes half of it and TRT twice (pbrt:
// v_TT = v / 4, v_TRT = 4 v; Karis slide 44 has the same ratios).
HAIR_SHARED_INLINE float HairBsdfLongitudinalDeviation(float betaM)
{
    const float b = HairBsdfClamp(betaM, HAIR_BSDF_MIN_BETA_M, 1.0f);
    const float b2 = b * b;
    const float b4 = b2 * b2;
    const float b20 = b4 * b4 * b4 * b4 * b4;
    return 0.726f * b + 0.812f * b2 + 3.7f * b20;
}

// The scale of the azimuthal logistic from betaN: Chiang's s = sqrt(pi / 8) (0.265 b + 1.194 b^2 + 5.372 b^22) (pbrt's
// HairBxDF constructor).
HAIR_SHARED_INLINE float HairBsdfAzimuthalScale(float betaN)
{
    const float b = HairBsdfClamp(betaN, 0.0f, 1.0f);
    const float b2 = b * b;
    const float b4 = b2 * b2;
    const float b8 = b4 * b4;
    const float b22 = b8 * b8 * b4 * b2;
    return 0.626657069f * (0.265f * b + 1.194f * b2 + 5.372f * b22);
}

// A material's lobes: what does not depend on the directions, worked out once per shading point.
struct HairBsdfLobes {
    float3 sigmaA;
    float eta;
    float f0;          // Fresnel reflectance at normal incidence
    float deviationR;  // longitudinal standard deviations, in sin(theta_i) + sin(theta_o)
    float deviationTT;
    float deviationTRT;
    float shiftR;      // where each longitudinal lobe peaks, in sin(theta_i) + sin(theta_o)
    float shiftTT;
    float shiftTRT;
    float scaleTT;     // the TT logistic's scale, and one over its integral over [-pi, pi] times the scale
    float normTT;
    float kappaTRT;    // the TRT lobe's concentration, and its normalization
    float normTRT;
    // What each longitudinal function is scaled by for the view (HairBsdfForView); 1 before that.
    float viewScaleR;
    float viewScaleTT;
    float viewScaleTRT;
};

HAIR_SHARED_INLINE HairBsdfLobes HairBsdfMakeLobes(HairBsdfMaterial material)
{
    HairBsdfLobes lobes;
    lobes.sigmaA = material.sigmaA;
    lobes.eta = material.eta > 1.01f ? material.eta : 1.01f;
    // Karis slide 20: F0 = (1 - eta)^2 / (1 + eta)^2, 0.0465 at eta 1.55.
    const float f0Root = (1.0f - lobes.eta) / (1.0f + lobes.eta);
    lobes.f0 = f0Root * f0Root;
    lobes.deviationR = HairBsdfLongitudinalDeviation(material.betaM);
    lobes.deviationTT = 0.5f * lobes.deviationR;
    lobes.deviationTRT = 2.0f * lobes.deviationR;
    // The lobes' shifts for scales tilted by alpha: pbrt turns theta_o by 2 alpha for R, alpha for TT and 4 alpha for
    // TRT, the other two against R. Karis slide 44 has the same multiples (alpha_R = -2 Shift, alpha_TT = Shift,
    // alpha_TRT = 4 Shift) and these signs, as a shift of sin(theta_i) + sin(theta_o); his Shift is our sin(alpha).
    lobes.shiftR = -HAIR_BSDF_SIN(2.0f * material.alpha);
    lobes.shiftTT = HAIR_BSDF_SIN(material.alpha);
    lobes.shiftTRT = HAIR_BSDF_SIN(4.0f * material.alpha);
    // TT's distribution around the fibre. Karis slide 29 uses a logistic of scale 0.35 about phi = pi for a smooth
    // fibre: the spread of the exit directions over the fibre's width. Frostbite widens it with betaN through a table
    // that is not published. Ours: that logistic convolved with Chiang's roughness logistic, as the root of the summed
    // squares of the two scales.
    const float roughnessScale = HairBsdfAzimuthalScale(material.betaN);
    lobes.scaleTT = HAIR_BSDF_SQRT(0.35f * 0.35f + roughnessScale * roughnessScale);
    // pbrt's TrimmedLogistic: the logistic's integral over [-pi, pi] is tanh(pi / (2 s)).
    lobes.normTT = 1.0f / (lobes.scaleTT * HAIR_BSDF_TANH(HAIR_BSDF_PI / (2.0f * lobes.scaleTT)));
    // TRT's distribution. Karis slide 32: exp(17 cos(phi) - 16.78), which is three quarters of a logistic of scale 0.15
    // about phi = 0. Frostbite slide 21 widens it with betaN by s_r = clamp(1.5 (1 - betaN)): s_r exp(s_r (17 cos(phi)
    // - 16.78)), whose integral falls to nothing as betaN nears 1. Ours: the same widening, 17 s_r as the concentration
    // of a lobe exp(kappa (cos(phi) - 1)), but kept at Karis's integral of three quarters at every roughness (the
    // normalization is that of a von Mises distribution, with I0's asymptotic series), and s_r no lower than 0.15.
    const float scale = HairBsdfClamp(1.5f * (1.0f - material.betaN), 0.15f, 1.0f);
    lobes.kappaTRT = 17.0f * scale;
    const float inverseKappa = 1.0f / lobes.kappaTRT;
    lobes.normTRT = 0.75f * HAIR_BSDF_SQRT(lobes.kappaTRT / (2.0f * HAIR_BSDF_PI)) /
        (1.0f + inverseKappa * (0.125f + inverseKappa * 0.0703125f));
    lobes.viewScaleR = 1.0f;
    lobes.viewScaleTT = 1.0f;
    lobes.viewScaleTRT = 1.0f;
    return lobes;
}

// Karis slide 20: Schlick's Fresnel.
HAIR_SHARED_INLINE float HairBsdfFresnel(float f0, float cosine)
{
    const float m = 1.0f - HairBsdfClamp(cosine, 0.0f, 1.0f);
    const float m2 = m * m;
    return f0 + (1.0f - f0) * m2 * m2 * m;
}

// Schlick's Fresnel averaged over the offsets h in [0, 1] across a fibre, for light whose inclination gives the cosine
// `c` at h = 0: the mean of F(c sqrt(1 - h^2)). (1 - c s)^5 expanded, with the means of s^k for s = sqrt(1 - h^2):
// pi/4, 2/3, 3 pi/16, 8/15, 5 pi/32. Ours.
HAIR_SHARED_INLINE float HairBsdfFresnelAcrossFibre(float f0, float cosine)
{
    const float c = HairBsdfClamp(cosine, 0.0f, 1.0f);
    const float mean = 1.0f + c * (-3.92699082f + c * (6.66666667f + c * (-5.89048623f + c * (2.66666667f - c * 0.490873852f))));
    return f0 + (1.0f - f0) * HairBsdfClamp(mean, 0.0f, 1.0f);
}

// Karis slide 18: the longitudinal function, a Gaussian in sin(theta_i) + sin(theta_o) about the lobe's shift, in place
// of d'Eon's energy conserving one. It integrates to 1 over sin(theta_i), less what falls outside [-1, 1]
// (HairBsdfForView makes up for that).
HAIR_SHARED_INLINE float HairBsdfLongitudinal(float sineSum, float shift, float deviation)
{
    const float x = (sineSum - shift) / deviation;
    return HAIR_BSDF_EXP(-0.5f * x * x) / (deviation * 2.50662827f);
}

// The error function, Abramowitz and Stegun 7.1.26 (within 1.5e-7).
HAIR_SHARED_INLINE float HairBsdfErf(float x)
{
    const float a = x < 0.0f ? -x : x;
    const float t = 1.0f / (1.0f + 0.3275911f * a);
    const float y = 1.0f - t * (0.254829592f + t * (-0.284496736f + t * (1.421413741f + t * (-1.453152027f + t * 1.061405429f)))) *
        HAIR_BSDF_EXP(-a * a);
    return x < 0.0f ? -y : y;
}

// One over the share of a longitudinal Gaussian that lies on real incident directions, sin(theta_i) in [-1, 1], for a
// view at sin(theta_o).
HAIR_SHARED_INLINE float HairBsdfLongitudinalViewScale(float sinThetaO, float shift, float deviation)
{
    const float mean = shift - sinThetaO;
    const float scale = 0.707106781f / deviation;
    const float share = 0.5f * (HairBsdfErf((1.0f - mean) * scale) - HairBsdfErf((-1.0f - mean) * scale));
    return 1.0f / (share > 0.1f ? share : 0.1f);
}

// The lobes for one view direction. Ours. Karis's Gaussian has its peak at sin(theta_i) = shift - sin(theta_o), and for
// a view along the fibre much of it lies past sin(theta_i) = -1 or 1, on no direction at all: the lobe loses that share
// of its energy, where d'Eon's function, which the Gaussian stands in for, keeps it (measured against pbrt: a third of
// R's energy left at 70 degrees). Each lobe is scaled back up by the share that is left, at most ten times, so that it
// integrates to its attenuation for every view. The scale depends on the view alone: once per shading point, not per
// light. With it the response is no longer the same with the two directions swapped.
HAIR_SHARED_INLINE HairBsdfLobes HairBsdfForView(HairBsdfLobes lobes, float3 tangent, float3 view)
{
    const float sinThetaO = HairBsdfClamp(HAIR_BSDF_DOT(tangent, view), -1.0f, 1.0f);
    lobes.viewScaleR = HairBsdfLongitudinalViewScale(sinThetaO, lobes.shiftR, lobes.deviationR);
    lobes.viewScaleTT = HairBsdfLongitudinalViewScale(sinThetaO, lobes.shiftTT, lobes.deviationTT);
    lobes.viewScaleTRT = HairBsdfLongitudinalViewScale(sinThetaO, lobes.shiftTRT, lobes.deviationTRT);
    return lobes;
}

// The directions' angles to the fibre and to each other.
struct HairBsdfGeometry {
    float sinThetaI;
    float sinThetaO;
    float cosThetaD;    // of half the difference of the two inclinations
    float cosPhi;       // of the angle between the two directions around the fibre
    float lightDotView;
};

HAIR_SHARED_INLINE HairBsdfGeometry HairBsdfMakeGeometry(float3 tangent, float3 view, float3 light)
{
    HairBsdfGeometry geometry;
    geometry.sinThetaI = HairBsdfClamp(HAIR_BSDF_DOT(tangent, light), -1.0f, 1.0f);
    geometry.sinThetaO = HairBsdfClamp(HAIR_BSDF_DOT(tangent, view), -1.0f, 1.0f);
    geometry.lightDotView = HAIR_BSDF_DOT(light, view);
    const float cosThetaI = HAIR_BSDF_SQRT(1.0f - geometry.sinThetaI * geometry.sinThetaI);
    const float cosThetaO = HAIR_BSDF_SQRT(1.0f - geometry.sinThetaO * geometry.sinThetaO);
    const float cosDifference = cosThetaI * cosThetaO + geometry.sinThetaI * geometry.sinThetaO;
    geometry.cosThetaD = HAIR_BSDF_SQRT(HairBsdfClamp(0.5f + 0.5f * cosDifference, 0.0f, 1.0f));
    // The two directions projected onto the plane normal to the fibre. Along the fibre there is no such projection:
    // phi is then taken as 0.
    const float cosProduct = cosThetaI * cosThetaO;
    geometry.cosPhi = cosProduct > 1.0e-6f
        ? HairBsdfClamp((geometry.lightDotView - geometry.sinThetaI * geometry.sinThetaO) / cosProduct, -1.0f, 1.0f)
        : 1.0f;
    return geometry;
}

// R's attenuation. Karis slide 20: Fresnel at half the angle between the two directions.
HAIR_SHARED_INLINE float HairBsdfAttenuationR(HairBsdfLobes lobes, float lightDotView)
{
    return HairBsdfFresnel(lobes.f0, HAIR_BSDF_SQRT(HairBsdfClamp(0.5f + 0.5f * lightDotView, 0.0f, 1.0f)));
}

// What one pass through the fibre's interior lets through, at an offset h whose refracted path has cos(gamma_t)
// `cosGammaT`. pbrt: T = exp(-sigma_a 2 cos(gamma_t) / cos(theta_t)), the path's length in fibre diameters. theta_t is
// the refracted inclination; here it is taken from theta_d, which is the same for the two directions swapped.
HAIR_SHARED_INLINE float3 HairBsdfInteriorTransmittance(HairBsdfLobes lobes, float cosThetaD, float cosGammaT)
{
    const float sinThetaD2 = 1.0f - cosThetaD * cosThetaD;
    const float cosThetaT = HAIR_BSDF_SQRT(1.0f - sinThetaD2 / (lobes.eta * lobes.eta));
    return HairBsdfExp3(lobes.sigmaA * (-2.0f * cosGammaT / cosThetaT));
}

// TT's attenuation, A(1, h) = (1 - f)^2 T (Karis slide 23). Frostbite slide 17 evaluates it at h = 0, straight through
// the middle of the fibre, in place of Karis's fitted offset and his colour raised to a power, which is what makes it
// right at grazing angles. Ours: at h = 0 the Fresnel term is the smallest it gets across the fibre, and the lobe then
// carries more than a fibre without absorption receives (measured: up to 10% over); the Fresnel term here is the
// average across the fibre instead. The absorption is Frostbite's, the path through the middle.
HAIR_SHARED_INLINE float3 HairBsdfAttenuationTT(HairBsdfLobes lobes, float cosThetaD)
{
    const float f = HairBsdfFresnelAcrossFibre(lobes.f0, cosThetaD);
    return HairBsdfInteriorTransmittance(lobes, cosThetaD, 1.0f) * ((1.0f - f) * (1.0f - f));
}

// TRT's attenuation, A(2, h) = (1 - f)^2 f T^2 at h = sqrt(3) / 2 (Karis slide 32, Frostbite slide 21), with the
// absorption of the path that offset gives: sin(gamma_t) = h / eta', eta' = sqrt(eta^2 - sin^2(theta_d)) / cos(theta_d)
// (Karis slide 26, whose fit 1.19 / cos + 0.36 cos is for eta 1.55 only).
HAIR_SHARED_INLINE float3 HairBsdfAttenuationTRT(HairBsdfLobes lobes, float cosThetaD)
{
    const float f = HairBsdfFresnel(lobes.f0, cosThetaD * 0.5f); // cos(theta_d) sqrt(1 - h^2)
    const float sinThetaD2 = 1.0f - cosThetaD * cosThetaD;
    // (h / eta')^2
    const float sinGammaT2 = 0.75f * cosThetaD * cosThetaD / (lobes.eta * lobes.eta - sinThetaD2);
    const float3 through = HairBsdfInteriorTransmittance(lobes, cosThetaD, HAIR_BSDF_SQRT(1.0f - sinGammaT2));
    return through * through * ((1.0f - f) * (1.0f - f) * f);
}

// R's distribution around the fibre. Karis slide 20: cos(phi / 2) / 4, a smooth cylinder's.
HAIR_SHARED_INLINE float HairBsdfDistributionR(float cosPhi)
{
    return 0.25f * HAIR_BSDF_SQRT(HairBsdfClamp(0.5f + 0.5f * cosPhi, 0.0f, 1.0f));
}

// TT's: a logistic about phi = pi (HairBsdfMakeLobes), normalized over the circle.
HAIR_SHARED_INLINE float HairBsdfDistributionTT(HairBsdfLobes lobes, float cosPhi)
{
    const float fromOpposite = HAIR_BSDF_PI - HAIR_BSDF_ACOS(cosPhi);
    const float e = HAIR_BSDF_EXP(-fromOpposite / lobes.scaleTT);
    return lobes.normTT * e / ((1.0f + e) * (1.0f + e));
}

// TRT's: a lobe about phi = 0 that integrates to three quarters (HairBsdfMakeLobes).
HAIR_SHARED_INLINE float HairBsdfDistributionTRT(HairBsdfLobes lobes, float cosPhi)
{
    return lobes.normTRT * HAIR_BSDF_EXP(lobes.kappaTRT * (cosPhi - 1.0f));
}

// The fibre's response to light from one direction (the header comment says in what units). `lobes` are the material's
// for this view (HairBsdfForView); `lobeMask` is HAIR_LOBE_*.
HAIR_SHARED_INLINE float3 HairBsdfEvaluate(HairBsdfLobes lobes, float3 tangent, float3 view, float3 light, uint lobeMask)
{
    const HairBsdfGeometry geometry = HairBsdfMakeGeometry(tangent, view, light);
    const float sineSum = geometry.sinThetaI + geometry.sinThetaO;
    float3 result = HAIR_BSDF_FLOAT3(0.0f, 0.0f, 0.0f);
    if ((lobeMask & HAIR_LOBE_R) != 0u) {
        const float r = lobes.viewScaleR * HairBsdfLongitudinal(sineSum, lobes.shiftR, lobes.deviationR) *
            HairBsdfDistributionR(geometry.cosPhi) * HairBsdfAttenuationR(lobes, geometry.lightDotView);
        result = result + HAIR_BSDF_FLOAT3(r, r, r);
    }
    if ((lobeMask & HAIR_LOBE_TT) != 0u) {
        result = result + HairBsdfAttenuationTT(lobes, geometry.cosThetaD) *
            (lobes.viewScaleTT * HairBsdfLongitudinal(sineSum, lobes.shiftTT, lobes.deviationTT) *
                HairBsdfDistributionTT(lobes, geometry.cosPhi));
    }
    if ((lobeMask & HAIR_LOBE_TRT) != 0u) {
        result = result + HairBsdfAttenuationTRT(lobes, geometry.cosThetaD) *
            (lobes.viewScaleTRT * HairBsdfLongitudinal(sineSum, lobes.shiftTRT, lobes.deviationTRT) *
                HairBsdfDistributionTRT(lobes, geometry.cosPhi));
    }
    return result;
}

// Direction `index` of the HAIR_BSDF_ENVIRONMENT_SAMPLES the environment is integrated over (HairBsdfEnvironmentSample
// says which they are): two on the viewer's side of the fibre (index 0 and 1, the inner ones) and two on the far side.
HAIR_SHARED_INLINE float3 HairBsdfEnvironmentDirection(float3 tangent, float3 view, uint index)
{
    const float sinThetaO = HairBsdfClamp(HAIR_BSDF_DOT(tangent, view), -1.0f, 1.0f);
    const float cosThetaO = HAIR_BSDF_SQRT(1.0f - sinThetaO * sinThetaO);
    // The side of the fibre that faces the viewer, and the direction across it.
    float3 facing = view - tangent * sinThetaO;
    const float facingLength = HAIR_BSDF_SQRT(HAIR_BSDF_DOT(facing, facing));
    if (facingLength > 1.0e-4f) {
        facing = facing * (1.0f / facingLength);
    } else {
        // Looking along the fibre: any side.
        const float3 helper = (tangent.y < 0.9f && tangent.y > -0.9f) ? HAIR_BSDF_FLOAT3(0.0f, 1.0f, 0.0f) : HAIR_BSDF_FLOAT3(1.0f, 0.0f, 0.0f);
        const float3 side = HAIR_BSDF_CROSS(helper, tangent);
        facing = side * (1.0f / HAIR_BSDF_SQRT(HAIR_BSDF_DOT(side, side)));
    }
    const float3 across = HAIR_BSDF_CROSS(tangent, facing);
    const bool inner = index < 2u;
    // cos and sin of each quarter's phi, on either side: 0.7423 and 2.1055, the quarters' centres of cos(phi / 2).
    const float cosPhi = inner ? 0.736922f : -0.509838f;
    const float sinPhi = ((index & 1u) != 0u ? -1.0f : 1.0f) * (inner ? 0.675978f : 0.860274f);
    return tangent * (-sinThetaO) + (facing * cosPhi + across * sinPhi) * cosThetaO;
}

// The environment, for a fibre nothing occludes (hair lit without a density volume; with one,
// HairBsdfEnvironmentWeightDual in HairScatterShared.h gives the weights): HAIR_BSDF_ENVIRONMENT_SAMPLES directions and the weight to give the
// environment's radiance along each; the sum of weight times radiance is the radiance toward the viewer. It is the
// integral of HairBsdfEvaluate against an environment that varies slowly (the caller's radiance is the renderer's
// second-order spherical harmonics), which lets the longitudinal functions be integrated out: each lobe is then on the
// cone of directions the fibre mirrors the view into, and what is left is an integral around the fibre.
//   R    cos(phi / 2) / 4 times its Fresnel term, over four quarters of the circle: one direction for each, at the
//        quarter's centre of cos(phi / 2) (phi 0.74 and 2.11 on either side of the view), with the quarter's integral of
//        cos(phi / 2) / 4 as its weight.
//   TRT  narrow about phi = 0: its whole weight, three quarters of its attenuation, on the two inner directions.
//   TT   about phi = pi, the environment straight behind the fibre. Left out, as Karis does (slide 48): until hair
//        occludes the environment (the density volume), that light would come through the head.
// Also Karis's, for the same reason: R is scaled by saturate(dot(light, view) + 1), which takes the light from behind
// the fibre away.
HAIR_SHARED_INLINE float3 HairBsdfEnvironmentSample(HairBsdfLobes lobes, float3 tangent, float3 view, uint index,
    HAIR_BSDF_OUT(float3) direction)
{
    const float sinThetaO = HairBsdfClamp(HAIR_BSDF_DOT(tangent, view), -1.0f, 1.0f);
    const float cosThetaO = HAIR_BSDF_SQRT(1.0f - sinThetaO * sinThetaO);
    const bool inner = index < 2u;
    direction = HairBsdfEnvironmentDirection(tangent, view, index);
    // The integrals of cos(phi / 2) / 4 over [0, pi/2] and [pi/2, pi].
    const float lightDotView = HAIR_BSDF_DOT(direction, view);
    const float unoccluded = HairBsdfClamp(lightDotView + 1.0f, 0.0f, 1.0f);
    const float r = (inner ? 0.353553391f : 0.146446609f) * HairBsdfAttenuationR(lobes, lightDotView) * unoccluded;
    float3 weight = HAIR_BSDF_FLOAT3(r, r, r);
    if (inner) {
        // theta_d is theta_o on the mirror cone. Half of the lobe's three quarters to each inner direction.
        weight = weight + HairBsdfAttenuationTRT(lobes, cosThetaO) * 0.375f;
    }
    return weight;
}

#ifdef __cplusplus
} // namespace br::hair
#endif

#endif // BR_HAIR_BSDF_SHARED_H
