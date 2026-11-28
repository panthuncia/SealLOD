#ifndef BR_HAIR_SCATTER_SHARED_H
#define BR_HAIR_SCATTER_SHARED_H

// Multiple scattering between hair fibres, written once for C++ and HLSL like HairBsdfShared.h, which it extends: the
// shade pass includes it through hairBsdf.hlsli and HairCoreTests checks the same text on the CPU.
//
// It is the dual scattering approximation of Zinke, Yuksel, Weber and Keyser, "Dual Scattering Approximation for Fast
// Multiple Scattering in Hair" (SIGGRAPH 2008; "Zinke" below, equation numbers are the paper's). Light that reaches a
// fibre through other hair is split in two:
//   global  what the hair between the light and the fibre lets through: the share that met no fibre at all (direct),
//           and the share that was scattered forward by every fibre it met, attenuated and coloured by each and spread
//           along the fibres a little more by each (Zinke's T_f and sigma_f^2);
//   local   what the fibres around the shading point scatter back toward the viewer, as one more lobe of the fibre's
//           response (Zinke's f_back), a property of the material.
// Both need, per material and per inclination of the light to the fibre, the fibre's average forward and backward
// attenuation, shift and spread (a_f, a_b, alpha_f, alpha_b, beta_f, beta_b): HairScatterRow, which the CPU works out
// from the scattering function itself (HairScatterMakeRow) into a small table per instance.
//
// Where this departs from the paper, and why ("ours"):
//   - The number of fibres between the light and the shading point comes from a density volume and is a real number,
//     the expected count n. The paper has an integer count and switches between "lit directly" (n = 0) and "lit by
//     forward scattered light". Here the fibres met are taken as Poisson distributed about n: the direct share is
//     exp(-n), the transmittance T_f = exp(-n (1 - a_f)) (the mean of a_f^k), and the forward scattered light has been
//     scattered n a_f / (1 - exp(-n a_f)) times on average. Both of the paper's cases are the limits.
//   - Units. Our lobes are M A D with M a Gaussian in sin(theta_i) + sin(theta_o) (HairBsdfShared.h), not in the half
//     angle; shifts and spreads here are in that variable, measured from the lobes themselves. Every lobe keeps its
//     integral: the backscattering lobe integrates to A_b (the paper's 2 / pi and cos^2 belong to Marschner's
//     normalization), and the pi the paper's Figure 5 multiplies f_back by under scattered light is not carried over.
//   - N^G, the fibre's azimuthal functions under light that arrives from half the circle (the paper's equation 25
//     as printed does not say which half): each D averaged over the half circle centred on the light, in closed form
//     for TT and TRT and by four samples for R, whose Fresnel term changes around the fibre.
//   - The backscattering lobe's azimuthal shape, which the paper takes as constant over the half circle facing the
//     light: (1 + cos(phi)) / (2 pi) here, the same thing without the edge.
// The table's rows are for the instance's material; a strand's own offsets (HairInstanceGPU's variations) are not in it.

#ifdef __cplusplus
#include "Hair/HairBsdfShared.h"

#include <array>

namespace br::hair {
#endif

// The table: HAIR_SCATTER_TABLE_ROWS rows per instance, for inclinations of the light to the fibre of 0 to
// HAIR_SCATTER_TABLE_MAX_THETA, evenly spaced. Beyond that, where the light runs along the fibre, the last row is used.
#define HAIR_SCATTER_TABLE_ROWS 16u
#define HAIR_SCATTER_TABLE_MAX_THETA 1.48352986f // 85 degrees

// Zinke's density factors d_f and d_b: "between 0.6 and 0.8 give realistic results", 0.7 in every example of the paper.
#define HAIR_SCATTER_DENSITY_FACTOR 0.7f

// What HairBsdfEvaluateDual adds up.
#define HAIR_SCATTER_PART_DIRECT 1u // the light no fibre was in the way of, through the fibre's own lobes
#define HAIR_SCATTER_PART_GLOBAL 2u // the forward scattered light, through the same lobes widened
#define HAIR_SCATTER_PART_LOCAL 4u  // both, through the backscattering lobe
#define HAIR_SCATTER_PART_ALL 7u

// A fibre under light at one inclination, per colour channel: the share of the light it scatters into the half of
// directions that goes on away from the light (forward) and into the half that comes back (backward), and for each
// half where along the fibre that light goes: its mean and variance in sin(theta_i) + sin(theta_o).
struct HairScatterRow {
    float3 forward; // a_f
    float pad0;
    float3 backward; // a_b
    float pad1;
    float3 forwardShift; // alpha_f
    float pad2;
    float3 backwardShift; // alpha_b
    float pad3;
    float3 forwardVariance; // beta_f^2
    float pad4;
    float3 backwardVariance; // beta_b^2
    float pad5;
};

// Where an inclination is in the table: a row and the share of the next one, from the sine of the inclination.
HAIR_SHARED_INLINE float HairScatterTableCoordinate(float sinTheta)
{
    const float s = HairBsdfClamp(sinTheta < 0.0f ? -sinTheta : sinTheta, 0.0f, 1.0f);
    const float theta = 0.5f * HAIR_BSDF_PI - HAIR_BSDF_ACOS(s);
    return HairBsdfClamp(theta / HAIR_SCATTER_TABLE_MAX_THETA, 0.0f, 1.0f) * (float)(HAIR_SCATTER_TABLE_ROWS - 1u);
}

HAIR_SHARED_INLINE HairScatterRow HairScatterRowLerp(HairScatterRow a, HairScatterRow b, float t)
{
    HairScatterRow row;
    row.forward = a.forward + (b.forward - a.forward) * t;
    row.backward = a.backward + (b.backward - a.backward) * t;
    row.forwardShift = a.forwardShift + (b.forwardShift - a.forwardShift) * t;
    row.backwardShift = a.backwardShift + (b.backwardShift - a.backwardShift) * t;
    row.forwardVariance = a.forwardVariance + (b.forwardVariance - a.forwardVariance) * t;
    row.backwardVariance = a.backwardVariance + (b.backwardVariance - a.backwardVariance) * t;
    row.pad0 = 0.0f;
    row.pad1 = 0.0f;
    row.pad2 = 0.0f;
    row.pad3 = 0.0f;
    row.pad4 = 0.0f;
    row.pad5 = 0.0f;
    return row;
}

// What the hair between a light and a fibre does to the light, for one colour channel, from the expected number of
// fibres in the way and the fibre's average forward attenuation and spread (the header comment says how).
struct HairScatterTransport {
    float direct;    // the share that met no fibre
    float scattered; // the share that arrives forward scattered: T_f less the direct share
    float variance;  // sigma_f^2 of the scattered share, in sin(theta_i) + sin(theta_o)
};

HAIR_SHARED_INLINE HairScatterTransport HairScatterMakeTransport(float fibreCount, float forward, float forwardVariance)
{
    const float n = fibreCount > 0.0f ? fibreCount : 0.0f;
    const float a = HairBsdfClamp(forward, 0.0f, 0.99f);
    HairScatterTransport transport;
    transport.direct = HAIR_BSDF_EXP(-n);
    const float through = HAIR_BSDF_EXP(-n * (1.0f - a));
    transport.scattered = through > transport.direct ? through - transport.direct : 0.0f;
    // The mean number of scattering events of the light that was scattered at least once: x / (1 - exp(-x)), x = n a.
    const float x = n * a;
    const float events = x > 1.0e-3f ? x / (1.0f - HAIR_BSDF_EXP(-x)) : 1.0f + 0.5f * x;
    transport.variance = forwardVariance * events;
    return transport;
}

// The backscattering lobe of one colour channel (Zinke section 3.2): its attenuation, shift and variance.
struct HairScatterBack {
    float attenuation; // A_b
    float shift;       // Delta_b
    float variance;    // sigma_b^2
};

HAIR_SHARED_INLINE HairScatterBack HairScatterMakeBack(float forward, float backward, float forwardShift, float backwardShift,
    float forwardVariance, float backwardVariance)
{
    const float af = HairBsdfClamp(forward, 0.0f, 0.99f);
    const float ab = HairBsdfClamp(backward, 0.0f, 0.99f);
    const float af2 = af * af;
    const float ab2 = ab * ab;
    const float through = 1.0f - af2;
    HairScatterBack back;
    // Equations 11, 13 and 14: light scattered back once, and three times, between forward scatterings.
    back.attenuation = ab * af2 / through + ab * ab2 * af2 / (through * through * through);
    // Equation 16.
    back.shift = backwardShift * (1.0f - 2.0f * ab2 / (through * through)) +
        forwardShift * (2.0f * through * through + 4.0f * af2 * ab2) / (through * through * through);
    // Equation 17, with standard deviations beta_f and beta_b.
    const float bf = HAIR_BSDF_SQRT(forwardVariance > 0.0f ? forwardVariance : 0.0f);
    const float bb = HAIR_BSDF_SQRT(backwardVariance > 0.0f ? backwardVariance : 0.0f);
    const float numerator = ab * HAIR_BSDF_SQRT(2.0f * bf * bf + bb * bb) + ab * ab2 * HAIR_BSDF_SQRT(2.0f * bf * bf + 3.0f * bb * bb);
    const float denominator = ab + ab * ab2 * (2.0f * bf + 3.0f * bb);
    const float deviation = denominator > 1.0e-8f ? (1.0f + 0.7f * af2) * numerator / denominator : bb;
    back.variance = deviation * deviation;
    return back;
}

// The backscattering lobe around the fibre: toward the light's side, nothing straight on from the light. It integrates
// to 1 over the circle. Ours (the header comment).
HAIR_SHARED_INLINE float HairScatterBackDistribution(float cosPhi)
{
    return (1.0f + cosPhi) * (0.5f / HAIR_BSDF_PI);
}

// The same under light that arrives from the half circle centred on the light: its mean over phi +- pi / 2.
HAIR_SHARED_INLINE float HairScatterBackDistributionScattered(float cosPhi)
{
    return (HAIR_BSDF_PI + 2.0f * cosPhi) * (0.5f / (HAIR_BSDF_PI * HAIR_BSDF_PI));
}

// TT's distribution under that light: the share of its logistic (about phi = pi, HairBsdfDistributionTT) that lies
// within pi / 2 of phi, over pi.
HAIR_SHARED_INLINE float HairScatterDistributionTTScattered(HairBsdfLobes lobes, float phi)
{
    // The window reaches from pi to `inside` on one side of the lobe's centre and pi - `inside` on the other.
    const float inside = phi - 0.5f * HAIR_BSDF_PI;
    const float upper = 1.0f / (1.0f + HAIR_BSDF_EXP(-inside / lobes.scaleTT));
    const float lower = 1.0f / (1.0f + HAIR_BSDF_EXP((HAIR_BSDF_PI - inside) / lobes.scaleTT));
    const float whole = HAIR_BSDF_TANH(HAIR_BSDF_PI / (2.0f * lobes.scaleTT));
    const float share = (upper - lower) / whole;
    return HairBsdfClamp(share, 0.0f, 1.0f) / HAIR_BSDF_PI;
}

// TRT's: its lobe about phi = 0 (HairBsdfDistributionTRT) is narrow, a Gaussian of variance 1 / kappa: the share of it
// within pi / 2 of phi, times its integral of three quarters, over pi.
HAIR_SHARED_INLINE float HairScatterDistributionTRTScattered(HairBsdfLobes lobes, float phi)
{
    const float edge = (0.5f * HAIR_BSDF_PI - phi) * HAIR_BSDF_SQRT(0.5f * lobes.kappaTRT);
    return 0.75f * 0.5f * (1.0f + HairBsdfErf(edge)) / HAIR_BSDF_PI;
}

// The fibre's response to a light with `fibreCount` fibres expected between the light and the shading point: what
// HairBsdfEvaluate returns, in the same units, with the hair in between and around accounted for. `row` is the
// instance's scatter table at the light's inclination; `densityFactor` is Zinke's d_f and d_b; `parts` is
// HAIR_SCATTER_PART_*. With no fibres in the way and HAIR_SCATTER_PART_DIRECT alone it is HairBsdfEvaluate.
HAIR_SHARED_INLINE float3 HairBsdfEvaluateDual(HairBsdfLobes lobes, HairScatterRow row, float3 tangent, float3 view, float3 light,
    uint lobeMask, float fibreCount, float densityFactor, uint parts)
{
    const HairBsdfGeometry geometry = HairBsdfMakeGeometry(tangent, view, light);
    const float sineSum = geometry.sinThetaI + geometry.sinThetaO;
    const HairScatterTransport transportR = HairScatterMakeTransport(fibreCount, row.forward.x, row.forwardVariance.x);
    const HairScatterTransport transportG = HairScatterMakeTransport(fibreCount, row.forward.y, row.forwardVariance.y);
    const HairScatterTransport transportB = HairScatterMakeTransport(fibreCount, row.forward.z, row.forwardVariance.z);
    const float direct = transportR.direct; // the same for every channel
    const float3 scattered = HAIR_BSDF_FLOAT3(transportR.scattered, transportG.scattered, transportB.scattered) * densityFactor;
    const float3 variance = HAIR_BSDF_FLOAT3(transportR.variance, transportG.variance, transportB.variance);

    float3 result = HAIR_BSDF_FLOAT3(0.0f, 0.0f, 0.0f);
    if ((parts & HAIR_SCATTER_PART_DIRECT) != 0u && direct > 1.0e-4f) {
        result = result + HairBsdfEvaluate(lobes, tangent, view, light, lobeMask) * direct;
    }
    const bool anyScattered = scattered.x > 1.0e-4f || scattered.y > 1.0e-4f || scattered.z > 1.0e-4f;
    const float phi = HAIR_BSDF_ACOS(geometry.cosPhi);
    if ((parts & HAIR_SCATTER_PART_GLOBAL) != 0u && anyScattered) {
        // The fibre's own lobes under the forward scattered light: each longitudinal function widened by the light's
        // spread (Zinke equation 23), each distribution averaged over the half circle the light comes from.
        float3 lobesScattered = HAIR_BSDF_FLOAT3(0.0f, 0.0f, 0.0f);
        if ((lobeMask & HAIR_LOBE_R) != 0u) {
            const float cosThetaI = HAIR_BSDF_SQRT(1.0f - geometry.sinThetaI * geometry.sinThetaI);
            const float cosThetaO = HAIR_BSDF_SQRT(1.0f - geometry.sinThetaO * geometry.sinThetaO);
            const float sineProduct = geometry.sinThetaI * geometry.sinThetaO;
            // cos of phi - 3 pi / 8, - pi / 8, + pi / 8, + 3 pi / 8, from cos and sin of phi.
            const float sinPhi = HAIR_BSDF_SQRT(1.0f - geometry.cosPhi * geometry.cosPhi);
            const float c0 = geometry.cosPhi * 0.382683432f + sinPhi * 0.923879533f;
            const float c1 = geometry.cosPhi * 0.923879533f + sinPhi * 0.382683432f;
            const float c2 = geometry.cosPhi * 0.923879533f - sinPhi * 0.382683432f;
            const float c3 = geometry.cosPhi * 0.382683432f - sinPhi * 0.923879533f;
            const float around = 0.25f * (
                HairBsdfDistributionR(c0) * HairBsdfAttenuationR(lobes, sineProduct + cosThetaI * cosThetaO * c0) +
                HairBsdfDistributionR(c1) * HairBsdfAttenuationR(lobes, sineProduct + cosThetaI * cosThetaO * c1) +
                HairBsdfDistributionR(c2) * HairBsdfAttenuationR(lobes, sineProduct + cosThetaI * cosThetaO * c2) +
                HairBsdfDistributionR(c3) * HairBsdfAttenuationR(lobes, sineProduct + cosThetaI * cosThetaO * c3));
            const float base = lobes.deviationR * lobes.deviationR;
            lobesScattered = lobesScattered + HAIR_BSDF_FLOAT3(
                HairBsdfLongitudinal(sineSum, lobes.shiftR, HAIR_BSDF_SQRT(base + variance.x)),
                HairBsdfLongitudinal(sineSum, lobes.shiftR, HAIR_BSDF_SQRT(base + variance.y)),
                HairBsdfLongitudinal(sineSum, lobes.shiftR, HAIR_BSDF_SQRT(base + variance.z))) * (lobes.viewScaleR * around);
        }
        if ((lobeMask & HAIR_LOBE_TT) != 0u) {
            const float base = lobes.deviationTT * lobes.deviationTT;
            lobesScattered = lobesScattered + HairBsdfAttenuationTT(lobes, geometry.cosThetaD) * HAIR_BSDF_FLOAT3(
                HairBsdfLongitudinal(sineSum, lobes.shiftTT, HAIR_BSDF_SQRT(base + variance.x)),
                HairBsdfLongitudinal(sineSum, lobes.shiftTT, HAIR_BSDF_SQRT(base + variance.y)),
                HairBsdfLongitudinal(sineSum, lobes.shiftTT, HAIR_BSDF_SQRT(base + variance.z))) *
                (lobes.viewScaleTT * HairScatterDistributionTTScattered(lobes, phi));
        }
        if ((lobeMask & HAIR_LOBE_TRT) != 0u) {
            const float base = lobes.deviationTRT * lobes.deviationTRT;
            lobesScattered = lobesScattered + HairBsdfAttenuationTRT(lobes, geometry.cosThetaD) * HAIR_BSDF_FLOAT3(
                HairBsdfLongitudinal(sineSum, lobes.shiftTRT, HAIR_BSDF_SQRT(base + variance.x)),
                HairBsdfLongitudinal(sineSum, lobes.shiftTRT, HAIR_BSDF_SQRT(base + variance.y)),
                HairBsdfLongitudinal(sineSum, lobes.shiftTRT, HAIR_BSDF_SQRT(base + variance.z))) *
                (lobes.viewScaleTRT * HairScatterDistributionTRTScattered(lobes, phi));
        }
        result = result + lobesScattered * scattered;
    }
    if ((parts & HAIR_SCATTER_PART_LOCAL) != 0u && (direct > 1.0e-4f || anyScattered)) {
        // The backscattering lobe, under the direct light and under the forward scattered light (Zinke equation 24).
        const HairScatterBack backR = HairScatterMakeBack(row.forward.x, row.backward.x, row.forwardShift.x, row.backwardShift.x,
            row.forwardVariance.x, row.backwardVariance.x);
        const HairScatterBack backG = HairScatterMakeBack(row.forward.y, row.backward.y, row.forwardShift.y, row.backwardShift.y,
            row.forwardVariance.y, row.backwardVariance.y);
        const HairScatterBack backB = HairScatterMakeBack(row.forward.z, row.backward.z, row.forwardShift.z, row.backwardShift.z,
            row.forwardVariance.z, row.backwardVariance.z);
        // The lobe is wide: a good part of its Gaussian lies past the fibre's axis, on no direction. Like the fibre's own
        // lobes (HairBsdfForView) it is scaled back to its integral, by the share the green channel's has on real
        // directions, under either light.
        const float aroundDirect = HairScatterBackDistribution(geometry.cosPhi) * direct *
            HairBsdfLongitudinalViewScale(geometry.sinThetaO, backG.shift, HAIR_BSDF_SQRT(backG.variance + 1.0e-6f));
        const float aroundScattered = HairScatterBackDistributionScattered(geometry.cosPhi) *
            HairBsdfLongitudinalViewScale(geometry.sinThetaO, backG.shift, HAIR_BSDF_SQRT(backG.variance + variance.y + 1.0e-6f));
        const float3 back = HAIR_BSDF_FLOAT3(
            backR.attenuation * (aroundDirect * HairBsdfLongitudinal(sineSum, backR.shift, HAIR_BSDF_SQRT(backR.variance + 1.0e-6f)) +
                aroundScattered * scattered.x * HairBsdfLongitudinal(sineSum, backR.shift, HAIR_BSDF_SQRT(backR.variance + variance.x + 1.0e-6f))),
            backG.attenuation * (aroundDirect * HairBsdfLongitudinal(sineSum, backG.shift, HAIR_BSDF_SQRT(backG.variance + 1.0e-6f)) +
                aroundScattered * scattered.y * HairBsdfLongitudinal(sineSum, backG.shift, HAIR_BSDF_SQRT(backG.variance + variance.y + 1.0e-6f))),
            backB.attenuation * (aroundDirect * HairBsdfLongitudinal(sineSum, backB.shift, HAIR_BSDF_SQRT(backB.variance + 1.0e-6f)) +
                aroundScattered * scattered.z * HairBsdfLongitudinal(sineSum, backB.shift, HAIR_BSDF_SQRT(backB.variance + variance.z + 1.0e-6f))));
        result = result + back * densityFactor;
    }
    return result;
}

// The environment with the hair's occlusion: the weight of direction `index` of HAIR_BSDF_ENVIRONMENT_SAMPLES, which is
// HairBsdfEnvironmentDirection's, when `fibreCount` fibres are expected between the fibre and the environment that way.
// It takes the place of HairBsdfEnvironmentSample's weight, and of its two stand-ins for occlusion: R is whole, and TT
// is there, half of its attenuation on each of the two directions away from the viewer. `row` is the instance's scatter
// table at the view's inclination (the directions are on the cone the fibre mirrors the view into). With
// `dualScattering` the forward scattered light counts and the backscattering lobe is added, each quarter of the circle
// with its share of that lobe; without it the fibres in the way only shadow. The environment's radiance varies slowly,
// so nothing is widened here.
HAIR_SHARED_INLINE float3 HairBsdfEnvironmentWeightDual(HairBsdfLobes lobes, HairScatterRow row, float3 view, float cosThetaO,
    uint index, float3 direction, float fibreCount, float densityFactor, bool dualScattering, uint parts)
{
    const bool inner = index < 2u;
    const float r = (inner ? 0.353553391f : 0.146446609f) * HairBsdfAttenuationR(lobes, HAIR_BSDF_DOT(direction, view));
    float3 single = HAIR_BSDF_FLOAT3(r, r, r);
    if (inner) {
        single = single + HairBsdfAttenuationTRT(lobes, cosThetaO) * 0.375f;
    } else {
        single = single + HairBsdfAttenuationTT(lobes, cosThetaO) * 0.5f;
    }
    const HairScatterTransport transportR = HairScatterMakeTransport(fibreCount, row.forward.x, row.forwardVariance.x);
    if (!dualScattering) {
        return single * transportR.direct;
    }
    const HairScatterTransport transportG = HairScatterMakeTransport(fibreCount, row.forward.y, row.forwardVariance.y);
    const HairScatterTransport transportB = HairScatterMakeTransport(fibreCount, row.forward.z, row.forwardVariance.z);
    const float3 scattered = HAIR_BSDF_FLOAT3(transportR.scattered, transportG.scattered, transportB.scattered) * densityFactor;
    float3 arriving = HAIR_BSDF_FLOAT3(0.0f, 0.0f, 0.0f);
    if ((parts & HAIR_SCATTER_PART_DIRECT) != 0u) {
        arriving = arriving + HAIR_BSDF_FLOAT3(transportR.direct, transportR.direct, transportR.direct);
    }
    if ((parts & HAIR_SCATTER_PART_GLOBAL) != 0u) {
        arriving = arriving + scattered;
    }
    float3 weight = single * arriving;
    if ((parts & HAIR_SCATTER_PART_LOCAL) != 0u) {
        const HairScatterBack backR = HairScatterMakeBack(row.forward.x, row.backward.x, row.forwardShift.x, row.backwardShift.x,
            row.forwardVariance.x, row.backwardVariance.x);
        const HairScatterBack backG = HairScatterMakeBack(row.forward.y, row.backward.y, row.forwardShift.y, row.backwardShift.y,
            row.forwardVariance.y, row.backwardVariance.y);
        const HairScatterBack backB = HairScatterMakeBack(row.forward.z, row.backward.z, row.forwardShift.z, row.backwardShift.z,
            row.forwardVariance.z, row.backwardVariance.z);
        // The integrals of HairScatterBackDistribution over [0, pi / 2] and [pi / 2, pi].
        const float around = inner ? 0.409154943f : 0.0908450569f;
        weight = weight + HAIR_BSDF_FLOAT3(backR.attenuation, backG.attenuation, backB.attenuation) *
            (scattered + HAIR_BSDF_FLOAT3(transportR.direct, transportR.direct, transportR.direct)) * (around * densityFactor);
    }
    return weight;
}

#ifdef __cplusplus

// One row of an instance's scatter table: the material under light at inclination `theta` (radians from the plane
// normal to the fibre). Zinke's equations 6 and 12, by brute force: the scattering function itself summed over the
// outgoing directions of the half away from the light and of the half toward it. The fibre scatters light arriving
// along `incident` toward `outgoing` as HairBsdfEvaluate(view = incident, light = outgoing): the response is pbrt's f
// times the cosine of the second direction, and f is the same with the directions swapped. A half's shift and variance
// are the mean and the variance of sin(theta_i) + sin(theta_o) over its light. Zinke takes the average of the lobes'
// own variances; the variance of the mix also holds the spread of the lobes' means and what the end of the range (a
// direction cannot lie further along the fibre than the fibre) cuts off, and both are in the sum.
//
// The sum covers sin(theta_o) as far as five deviations of the widest lobe from where the lobes are centred, in steps of
// a third of the narrowest lobe's deviation.
inline HairScatterRow HairScatterMakeRow(const HairBsdfMaterial& material, float theta)
{
    constexpr int kPhiSteps = 48;
    const Float3 tangent{ 1.0f, 0.0f, 0.0f };
    const float sinTheta = std::sin(theta);
    const Float3 incident{ sinTheta, std::cos(theta), 0.0f };
    const HairBsdfLobes lobes = HairBsdfForView(HairBsdfMakeLobes(material), tangent, incident);
    const float reach = 5.0f * lobes.deviationTRT + 0.2f;
    const float low = HairBsdfClamp(-sinTheta - reach, -1.0f, 1.0f);
    const float high = HairBsdfClamp(-sinTheta + reach, -1.0f, 1.0f);
    const int sinSteps = static_cast<int>(HairBsdfClamp(std::ceil((high - low) / (lobes.deviationTT / 3.0f)), 8.0f, 2048.0f));
    double sums[2][3][3] = {}; // half (0 forward), channel, moment
    for (int i = 0; i < sinSteps; ++i) {
        const float sinOut = low + (high - low) * (static_cast<float>(i) + 0.5f) / static_cast<float>(sinSteps);
        const float cosOut = std::sqrt(1.0f - sinOut * sinOut);
        const double s = static_cast<double>(sinOut) + sinTheta;
        for (int j = 0; j < kPhiSteps; ++j) {
            const float phi = -HAIR_BSDF_PI + 2.0f * HAIR_BSDF_PI * (static_cast<float>(j) + 0.5f) / static_cast<float>(kPhiSteps);
            const float cosPhi = std::cos(phi);
            const Float3 value = HairBsdfEvaluate(lobes, tangent, incident, Float3{ sinOut, cosOut * cosPhi, cosOut * std::sin(phi) }, HAIR_LOBE_ALL);
            const double channels[3] = { value.x, value.y, value.z };
            double (&half)[3][3] = sums[cosPhi < 0.0f ? 0 : 1];
            for (int c = 0; c < 3; ++c) {
                half[c][0] += channels[c];
                half[c][1] += channels[c] * s;
                half[c][2] += channels[c] * s * s;
            }
        }
    }
    const double cell = (static_cast<double>(high) - low) / sinSteps * (2.0 * HAIR_BSDF_PI / kPhiSteps);
    HairScatterRow row{};
    const auto moments = [&](int half, Float3& attenuation, Float3& shift, Float3& variance) {
        float* outputs[3][3] = { { &attenuation.x, &shift.x, &variance.x }, { &attenuation.y, &shift.y, &variance.y },
            { &attenuation.z, &shift.z, &variance.z } };
        for (int c = 0; c < 3; ++c) {
            const double energy = sums[half][c][0];
            const double mean = energy > 0.0 ? sums[half][c][1] / energy : 0.0;
            const double spread = energy > 0.0 ? sums[half][c][2] / energy - mean * mean : 0.0;
            *outputs[c][0] = static_cast<float>(energy * cell);
            *outputs[c][1] = static_cast<float>(mean);
            *outputs[c][2] = static_cast<float>(spread > 1.0e-6 ? spread : 1.0e-6);
        }
    };
    moments(0, row.forward, row.forwardShift, row.forwardVariance);
    moments(1, row.backward, row.backwardShift, row.backwardVariance);
    return row;
}

using HairScatterTable = std::array<HairScatterRow, HAIR_SCATTER_TABLE_ROWS>;

inline HairScatterTable HairScatterMakeTable(const HairBsdfMaterial& material)
{
    HairScatterTable table;
    for (std::uint32_t index = 0u; index < HAIR_SCATTER_TABLE_ROWS; ++index) {
        table[index] = HairScatterMakeRow(material,
            HAIR_SCATTER_TABLE_MAX_THETA * static_cast<float>(index) / static_cast<float>(HAIR_SCATTER_TABLE_ROWS - 1u));
    }
    return table;
}

// The table at the sine of an inclination, as a shader reads it.
inline HairScatterRow HairScatterLookup(const HairScatterTable& table, float sinTheta)
{
    const float coordinate = HairScatterTableCoordinate(sinTheta);
    const auto first = static_cast<std::uint32_t>(coordinate);
    const std::uint32_t second = first + 1u < HAIR_SCATTER_TABLE_ROWS ? first + 1u : first;
    return HairScatterRowLerp(table[first], table[second], coordinate - static_cast<float>(first));
}

static_assert(sizeof(HairScatterRow) == 96u);

} // namespace br::hair
#endif

#endif // BR_HAIR_SCATTER_SHARED_H
