// Tests of the hair scattering function (HairBsdfShared.h): the text the shade pass compiles, compiled here as C++. Run
// from HairCoreTests.cpp's main(). The integrals are midpoint sums over sin(theta_i) and phi; the numbers the tests
// print are the ones docs/hair.md quotes.

#include "Hair/HairBsdfShared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>

using namespace br::hair;

namespace {

    int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << "(" << __LINE__ << "): check failed: " #condition "\n"; \
            ++g_failures; \
        } \
    } while (false)

    constexpr double kPi = 3.14159265358979323846;

    struct Rgb {
        double r = 0.0, g = 0.0, b = 0.0;
        Rgb& operator+=(Rgb o) { r += o.r; g += o.g; b += o.b; return *this; }
        Rgb operator*(double s) const { return { r * s, g * s, b * s }; }
        double Max() const { return (std::max)({ r, g, b }); }
        double Min() const { return (std::min)({ r, g, b }); }
    };
    Rgb ToRgb(Float3 v) { return { v.x, v.y, v.z }; }

    // The frame the tests work in: the fibre along x, as pbrt has it.
    constexpr Float3 kTangent{ 1.0f, 0.0f, 0.0f };

    Float3 Direction(double sinTheta, double phi)
    {
        const double cosTheta = std::sqrt((std::max)(0.0, 1.0 - sinTheta * sinTheta));
        return { static_cast<float>(sinTheta), static_cast<float>(cosTheta * std::cos(phi)), static_cast<float>(cosTheta * std::sin(phi)) };
    }

    HairBsdfMaterial Material(Float3 sigmaA, float betaM, float betaN, float alphaDegrees = 2.0f, float eta = 1.55f)
    {
        HairBsdfMaterial material;
        material.sigmaA = sigmaA;
        material.betaM = betaM;
        material.betaN = betaN;
        material.alpha = alphaDegrees * (HAIR_BSDF_PI / 180.0f);
        material.eta = eta;
        return material;
    }

    Float3 RandomDirection(std::mt19937& random)
    {
        std::uniform_real_distribution<double> unit(-1.0, 1.0);
        return Direction(unit(random), unit(random) * kPi);
    }

    // The integral over every incident direction of `function(light)`, with the solid angle measure: d(sin theta) dphi.
    template<class Function>
    Rgb IntegrateSphere(Function&& function, int sineSteps = 2000, int phiSteps = 360)
    {
        Rgb sum;
        for (int i = 0; i < sineSteps; ++i) {
            const double sinTheta = -1.0 + 2.0 * (i + 0.5) / sineSteps;
            for (int j = 0; j < phiSteps; ++j) {
                const double phi = -kPi + 2.0 * kPi * (j + 0.5) / phiSteps;
                sum += function(Direction(sinTheta, phi));
            }
        }
        return sum * ((2.0 / sineSteps) * (2.0 * kPi / phiSteps));
    }

    // ---- pbrt-v4's HairBxDF::f, as a reference -------------------------------------------------------------------
    // A transcription of HairBxDF::f and what it calls (src/pbrt/bxdfs.h, bxdfs.cpp, util/math.h, util/scattering.h;
    // pbrt-v4 is by Matt Pharr, Wenzel Jakob and Greg Humphreys, Apache License 2.0) in double precision, with RGB for
    // the spectrum and the lobes returned apart: [0] R, [1] TT, [2] TRT, [3] the remaining orders. Each is f times
    // |cos(theta_i)|, the quantity HairBsdfEvaluate returns. The fibre runs along x.
    namespace pbrt {
        double Sqr(double x) { return x * x; }
        double SafeSqrt(double x) { return std::sqrt((std::max)(0.0, x)); }
        double SafeASin(double x) { return std::asin(std::clamp(x, -1.0, 1.0)); }
        double I0(double x)
        {
            double val = 0.0, x2i = 1.0;
            std::int64_t ifact = 1;
            int i4 = 1;
            for (int i = 0; i < 10; ++i) {
                if (i > 1) ifact *= i;
                val += x2i / (i4 * Sqr(static_cast<double>(ifact)));
                x2i *= x * x;
                i4 *= 4;
            }
            return val;
        }
        double LogI0(double x) { return x > 12.0 ? x + 0.5 * (-std::log(2.0 * kPi) + std::log(1.0 / x) + 1.0 / (8.0 * x)) : std::log(I0(x)); }
        double Mp(double cosTheta_i, double cosTheta_o, double sinTheta_i, double sinTheta_o, double v)
        {
            const double a = cosTheta_i * cosTheta_o / v, b = sinTheta_i * sinTheta_o / v;
            return v <= 0.1 ? std::exp(LogI0(a) - b - 1.0 / v + 0.6931 + std::log(1.0 / (2.0 * v)))
                            : (std::exp(-b) * I0(a)) / (std::sinh(1.0 / v) * 2.0 * v);
        }
        double FrDielectric(double cosTheta_i, double eta)
        {
            cosTheta_i = std::clamp(cosTheta_i, -1.0, 1.0);
            if (cosTheta_i < 0.0) {
                eta = 1.0 / eta;
                cosTheta_i = -cosTheta_i;
            }
            const double sin2Theta_t = (1.0 - Sqr(cosTheta_i)) / Sqr(eta);
            if (sin2Theta_t >= 1.0) return 1.0;
            const double cosTheta_t = SafeSqrt(1.0 - sin2Theta_t);
            const double r_parl = (eta * cosTheta_i - cosTheta_t) / (eta * cosTheta_i + cosTheta_t);
            const double r_perp = (cosTheta_i - eta * cosTheta_t) / (cosTheta_i + eta * cosTheta_t);
            return (Sqr(r_parl) + Sqr(r_perp)) / 2.0;
        }
        double Logistic(double x, double s)
        {
            x = std::abs(x);
            return std::exp(-x / s) / (s * Sqr(1.0 + std::exp(-x / s)));
        }
        double LogisticCDF(double x, double s) { return 1.0 / (1.0 + std::exp(-x / s)); }
        double TrimmedLogistic(double x, double s, double a, double b) { return Logistic(x, s) / (LogisticCDF(b, s) - LogisticCDF(a, s)); }
        double Np(double phi, int p, double s, double gamma_o, double gamma_t)
        {
            double dphi = phi - (2.0 * p * gamma_t - 2.0 * gamma_o + p * kPi);
            while (dphi > kPi) dphi -= 2.0 * kPi;
            while (dphi < -kPi) dphi += 2.0 * kPi;
            return TrimmedLogistic(dphi, s, -kPi, kPi);
        }

        constexpr int pMax = 3;

        std::array<Rgb, pMax + 1> HairF(Float3 wo, Float3 wi, double h, double eta, Rgb sigma_a, double beta_m, double beta_n, double alphaDegrees)
        {
            double v[pMax + 1];
            v[0] = Sqr(0.726 * beta_m + 0.812 * Sqr(beta_m) + 3.7 * std::pow(beta_m, 20));
            v[1] = 0.25 * v[0];
            v[2] = 4.0 * v[0];
            v[3] = v[2];
            const double s = 0.626657069 * (0.265 * beta_n + 1.194 * Sqr(beta_n) + 5.372 * std::pow(beta_n, 22));
            double sin2kAlpha[pMax], cos2kAlpha[pMax];
            sin2kAlpha[0] = std::sin(alphaDegrees * kPi / 180.0);
            cos2kAlpha[0] = SafeSqrt(1.0 - Sqr(sin2kAlpha[0]));
            for (int i = 1; i < pMax; ++i) {
                sin2kAlpha[i] = 2.0 * cos2kAlpha[i - 1] * sin2kAlpha[i - 1];
                cos2kAlpha[i] = Sqr(cos2kAlpha[i - 1]) - Sqr(sin2kAlpha[i - 1]);
            }

            const double sinTheta_o = wo.x;
            const double cosTheta_o = SafeSqrt(1.0 - Sqr(sinTheta_o));
            const double phi_o = std::atan2(wo.z, wo.y);
            const double gamma_o = SafeASin(h);
            const double sinTheta_i = wi.x;
            const double cosTheta_i = SafeSqrt(1.0 - Sqr(sinTheta_i));
            const double phi_i = std::atan2(wi.z, wi.y);
            const double sinTheta_t = sinTheta_o / eta;
            const double cosTheta_t = SafeSqrt(1.0 - Sqr(sinTheta_t));
            const double etap = SafeSqrt(Sqr(eta) - Sqr(sinTheta_o)) / cosTheta_o;
            const double sinGamma_t = h / etap;
            const double cosGamma_t = SafeSqrt(1.0 - Sqr(sinGamma_t));
            const double gamma_t = SafeASin(sinGamma_t);
            const double length = 2.0 * cosGamma_t / cosTheta_t;
            const Rgb T{ std::exp(-sigma_a.r * length), std::exp(-sigma_a.g * length), std::exp(-sigma_a.b * length) };

            // Ap.
            std::array<Rgb, pMax + 1> ap;
            const double f = FrDielectric(cosTheta_o * SafeSqrt(1.0 - Sqr(h)), eta);
            ap[0] = { f, f, f };
            ap[1] = T * Sqr(1.0 - f);
            ap[2] = { ap[1].r * T.r * f, ap[1].g * T.g * f, ap[1].b * T.b * f };
            ap[3] = { ap[2].r * f * T.r / (1.0 - T.r * f), ap[2].g * f * T.g / (1.0 - T.g * f), ap[2].b * f * T.b / (1.0 - T.b * f) };

            const double phi = phi_i - phi_o;
            std::array<Rgb, pMax + 1> lobes;
            for (int p = 0; p < pMax; ++p) {
                double sinThetap_o, cosThetap_o;
                if (p == 0) {
                    sinThetap_o = sinTheta_o * cos2kAlpha[1] - cosTheta_o * sin2kAlpha[1];
                    cosThetap_o = cosTheta_o * cos2kAlpha[1] + sinTheta_o * sin2kAlpha[1];
                } else if (p == 1) {
                    sinThetap_o = sinTheta_o * cos2kAlpha[0] + cosTheta_o * sin2kAlpha[0];
                    cosThetap_o = cosTheta_o * cos2kAlpha[0] - sinTheta_o * sin2kAlpha[0];
                } else {
                    sinThetap_o = sinTheta_o * cos2kAlpha[2] + cosTheta_o * sin2kAlpha[2];
                    cosThetap_o = cosTheta_o * cos2kAlpha[2] - sinTheta_o * sin2kAlpha[2];
                }
                cosThetap_o = std::abs(cosThetap_o);
                lobes[p] = ap[p] * (Mp(cosTheta_i, cosThetap_o, sinTheta_i, sinThetap_o, v[p]) * Np(phi, p, s, gamma_o, gamma_t));
            }
            lobes[pMax] = ap[pMax] * (Mp(cosTheta_i, cosTheta_o, sinTheta_i, sinTheta_o, v[pMax]) / (2.0 * kPi));
            return lobes; // f * |cos(theta_i)|: pbrt divides this sum by |cos(theta_i)|
        }
    } // namespace pbrt

    void TestParameterMappings()
    {
        // Chiang's mappings, against pbrt's expressions written out again.
        for (const float b : { 0.05f, 0.1f, 0.25f, 0.3f, 0.6f, 0.9f, 1.0f }) {
            const double variance = pbrt::Sqr(0.726 * b + 0.812 * b * b + 3.7 * std::pow(static_cast<double>(b), 20));
            const double deviation = HairBsdfLongitudinalDeviation(b);
            CHECK(std::abs(deviation * deviation - variance) <= 1.0e-5 * (std::max)(1.0, variance));
            const double scale = 0.626657069 * (0.265 * b + 1.194 * b * b + 5.372 * std::pow(static_cast<double>(b), 22));
            CHECK(std::abs(HairBsdfAzimuthalScale(b) - scale) <= 1.0e-5 * (std::max)(1.0, scale));
        }
        // The lobes' widths and shifts keep pbrt's ratios.
        const HairBsdfLobes lobes = HairBsdfMakeLobes(Material({}, 0.3f, 0.3f));
        CHECK(lobes.deviationTT == 0.5f * lobes.deviationR && lobes.deviationTRT == 2.0f * lobes.deviationR);
        CHECK(std::abs(lobes.shiftR + std::sin(4.0 * kPi / 180.0)) < 1.0e-6 && std::abs(lobes.shiftTT - std::sin(2.0 * kPi / 180.0)) < 1.0e-6 &&
              std::abs(lobes.shiftTRT - std::sin(8.0 * kPi / 180.0)) < 1.0e-6);
        CHECK(std::abs(lobes.f0 - 0.04652f) < 1.0e-5f); // ((1 - 1.55) / (1 + 1.55))^2
        // pbrt-v4's figure 9.47 gives these absorptions for black and brown hair: eumelanin 8 and 2.
        const Float3 black = HairSigmaAFromMelanin(8.0f, 0.0f);
        const Float3 brown = HairSigmaAFromMelaninRedness(2.0f, 0.0f);
        CHECK(std::abs(black.x - 3.35f) < 0.01f && std::abs(black.y - 5.58f) < 0.01f && std::abs(black.z - 10.96f) < 0.01f);
        CHECK(std::abs(brown.x - 0.84f) < 0.01f && std::abs(brown.y - 1.39f) < 0.01f && std::abs(brown.z - 2.74f) < 0.01f);
        const Float3 red = HairSigmaAFromMelaninRedness(1.0f, 1.0f);
        CHECK(red.x == 0.187f && red.y == 0.4f && red.z == 1.05f);
        // The colour fit: white absorbs nothing, a darker colour more, and the fit inverts to the colour.
        CHECK(HairSigmaAFromColorChannel(1.0f, 0.3f) == 0.0f);
        float previous = 0.0f;
        for (const float c : { 0.9f, 0.5f, 0.2f, 0.05f, 0.01f }) {
            const float sigma = HairSigmaAFromColorChannel(c, 0.3f);
            CHECK(sigma > previous);
            previous = sigma;
            const double b = 0.3;
            const double denominator = 5.969 - 0.215 * b + 2.532 * b * b - 10.73 * b * b * b + 5.574 * b * b * b * b + 0.245 * b * b * b * b * b;
            CHECK(std::abs(std::exp(-std::sqrt(static_cast<double>(sigma)) * denominator) - c) < 1.0e-4);
        }
    }

    void TestPieces()
    {
        // Karis's fit of the modified index of refraction (slide 26) is within the 0.68% the slide states of the exact
        // form the attenuation uses, for eta 1.55.
        double worstEtaFit = 0.0;
        for (int i = 0; i <= 850; ++i) {
            const double thetaD = i * 0.1 * kPi / 180.0;
            const double exact = std::sqrt(1.55 * 1.55 - pbrt::Sqr(std::sin(thetaD))) / std::cos(thetaD);
            worstEtaFit = (std::max)(worstEtaFit, std::abs((1.19 / std::cos(thetaD) + 0.36 * std::cos(thetaD)) / exact - 1.0));
        }
        CHECK(worstEtaFit < 0.0068);

        // The Fresnel term averaged across the fibre is the mean of Schlick's over the offsets.
        const float f0 = 0.0465f;
        double worstMean = 0.0;
        for (int i = 0; i <= 20; ++i) {
            const float c = i / 20.0f;
            double mean = 0.0;
            const int steps = 4000;
            for (int j = 0; j < steps; ++j) {
                const double h = (j + 0.5) / steps;
                mean += HairBsdfFresnel(f0, static_cast<float>(c * std::sqrt(1.0 - h * h)));
            }
            worstMean = (std::max)(worstMean, std::abs(mean / steps - HairBsdfFresnelAcrossFibre(f0, c)));
        }
        CHECK(worstMean < 2.0e-4);

        // The three distributions around the fibre: R and TT integrate to 1, TRT to three quarters, at every roughness.
        double worstTT = 0.0, worstTRT = 0.0, integralR = 0.0;
        const int steps = 20000;
        for (int j = 0; j < steps; ++j) integralR += HairBsdfDistributionR(static_cast<float>(std::cos(-kPi + 2.0 * kPi * (j + 0.5) / steps)));
        integralR *= 2.0 * kPi / steps;
        CHECK(std::abs(integralR - 1.0) < 1.0e-3);
        for (const float betaN : { 0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f }) {
            const HairBsdfLobes lobes = HairBsdfMakeLobes(Material({}, 0.3f, betaN));
            double integralTT = 0.0, integralTRT = 0.0;
            for (int j = 0; j < steps; ++j) {
                const float cosPhi = static_cast<float>(std::cos(-kPi + 2.0 * kPi * (j + 0.5) / steps));
                integralTT += HairBsdfDistributionTT(lobes, cosPhi);
                integralTRT += HairBsdfDistributionTRT(lobes, cosPhi);
            }
            worstTT = (std::max)(worstTT, std::abs(integralTT * 2.0 * kPi / steps - 1.0));
            worstTRT = (std::max)(worstTRT, std::abs(integralTRT * 2.0 * kPi / steps - 0.75));
        }
        CHECK(worstTT < 2.0e-3);
        CHECK(worstTRT < 0.01);
        // At the roughness Karis's constants are for, the TT and TRT distributions are his (slides 29 and 32) to a few
        // percent of their peaks: exp(-3.65 cos(phi) - 3.98) and exp(17 cos(phi) - 16.78).
        const HairBsdfLobes smooth = HairBsdfMakeLobes(Material({}, 0.3f, 0.0f));
        double worstKarisTT = 0.0, worstKarisTRT = 0.0;
        for (int j = 0; j <= 360; ++j) {
            const double cosPhi = std::cos(j * kPi / 180.0);
            worstKarisTT = (std::max)(worstKarisTT, std::abs(HairBsdfDistributionTT(smooth, static_cast<float>(cosPhi)) - std::exp(-3.65 * cosPhi - 3.98)));
            worstKarisTRT = (std::max)(worstKarisTRT, std::abs(HairBsdfDistributionTRT(smooth, static_cast<float>(cosPhi)) - std::exp(17.0 * cosPhi - 16.78)));
        }
        CHECK(worstKarisTT < 0.03);  // of a peak of 0.71
        CHECK(worstKarisTRT < 0.03); // of a peak of 1.25
        std::cout << "  hair bsdf pieces: eta' fit " << worstEtaFit * 100.0 << "% off at most; Fresnel across the fibre " << worstMean
                  << "; TT integral off by " << worstTT << ", TRT by " << worstTRT << "; against Karis's TT " << worstKarisTT << ", TRT "
                  << worstKarisTRT << "\n";
    }

    void TestNonNegativeAndSymmetric()
    {
        std::mt19937 random(7u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        double worstSymmetry = 0.0, worstReciprocity = 0.0, worstViewAsymmetry = 0.0;
        int negative = 0, notFinite = 0, asymmetric = 0, compared = 0;
        for (int i = 0; i < 200000; ++i) {
            const HairBsdfMaterial material = Material(HairSigmaAFromMelaninRedness(unit(random) * 8.0f, unit(random)), unit(random), unit(random),
                unit(random) * 6.0f, 1.3f + unit(random) * 0.5f);
            const HairBsdfLobes lobes = HairBsdfMakeLobes(material);
            // Any tangent, not just the test frame's.
            const Float3 tangent = RandomDirection(random);
            const Float3 view = RandomDirection(random);
            const Float3 light = RandomDirection(random);
            const uint mask = 1u + static_cast<uint>(random() % 7u);
            const Float3 value = HairBsdfEvaluate(lobes, tangent, view, light, mask);
            const Float3 swapped = HairBsdfEvaluate(lobes, tangent, light, view, mask);
            if (!(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z))) ++notFinite;
            if (value.x < 0.0f || value.y < 0.0f || value.z < 0.0f) ++negative;
            // The response is the same with the two directions swapped ...
            const double scale = (std::max)({ static_cast<double>(value.x), static_cast<double>(swapped.x), 1.0e-6 });
            worstSymmetry = (std::max)(worstSymmetry, std::abs(static_cast<double>(value.x) - swapped.x) / scale);
            // ... so the BSDF itself, the response over cos(theta_i), is not reciprocal: the two differ by the ratio of
            // the cosines.
            const double cosI = std::sqrt((std::max)(0.0, 1.0 - pbrt::Sqr(Dot(tangent, light))));
            const double cosO = std::sqrt((std::max)(0.0, 1.0 - pbrt::Sqr(Dot(tangent, view))));
            if (value.x > 1.0e-4f && cosI > 0.05 && cosO > 0.05) {
                worstReciprocity = (std::max)(worstReciprocity, std::abs((value.x / cosI) / (swapped.x / cosO) - cosO / cosI) / (cosO / cosI));
            }
            // With the lobes scaled for the view (HairBsdfForView) the swap is no longer exact: measured, not required.
            const Float3 viewed = HairBsdfEvaluate(HairBsdfForView(lobes, tangent, view), tangent, view, light, mask);
            const Float3 viewedSwapped = HairBsdfEvaluate(HairBsdfForView(lobes, tangent, light), tangent, light, view, mask);
            if (!(std::isfinite(viewed.x) && std::isfinite(viewed.y) && std::isfinite(viewed.z))) ++notFinite;
            if (viewed.x < 0.0f || viewed.y < 0.0f || viewed.z < 0.0f) ++negative;
            CHECK(viewed.x >= value.x * 0.9999f && viewed.x <= value.x * 10.001f + 1.0e-30f);
            if (viewed.x > 1.0e-3f || viewedSwapped.x > 1.0e-3f) {
                const double ratio = (std::max)(viewed.x, viewedSwapped.x) / (std::max)(1.0e-9f, (std::min)(viewed.x, viewedSwapped.x));
                worstViewAsymmetry = (std::max)(worstViewAsymmetry, ratio);
                ++compared;
                if (ratio > 1.05) ++asymmetric;
            }
        }
        CHECK(negative == 0);
        CHECK(notFinite == 0);
        CHECK(worstSymmetry < 1.0e-3);
        CHECK(worstReciprocity < 1.0e-3);
        std::cout << "  hair bsdf: 200,000 random evaluations, none negative, none not finite; swapped directions differ by "
                  << worstSymmetry << " of the value at most before the lobes are scaled for the view; scaled, " << asymmetric << " of "
                  << compared << " pairs above 0.001 differ by more than 5%, the worst " << worstViewAsymmetry << " times\n";
    }

    // The fibre's albedo for a view at inclination thetaO: the response integrated over every incident direction.
    Rgb Albedo(const HairBsdfLobes& lobes, double thetaODegrees, uint mask)
    {
        const Float3 view = Direction(std::sin(thetaODegrees * kPi / 180.0), 0.0);
        const HairBsdfLobes forView = HairBsdfForView(lobes, kTangent, view);
        return IntegrateSphere([&](Float3 light) { return ToRgb(HairBsdfEvaluate(forView, kTangent, view, light, mask)); });
    }

    void TestWhiteFurnace()
    {
        // No absorption: the fibre must not return more than it receives.
        constexpr double kTolerance = 0.01;
        struct Roughness { float betaM, betaN; };
        double highest = 0.0, lowest = 1.0;
        std::cout << "  hair bsdf white furnace (no absorption), R + TT + TRT by view inclination 0, 30, 60, 75, 85 degrees:\n";
        for (const Roughness roughness : { Roughness{ 0.1f, 0.1f }, Roughness{ 0.3f, 0.3f }, Roughness{ 0.6f, 0.6f }, Roughness{ 0.9f, 0.9f },
                 Roughness{ 0.1f, 0.9f }, Roughness{ 0.9f, 0.1f } }) {
            const HairBsdfLobes lobes = HairBsdfMakeLobes(Material({}, roughness.betaM, roughness.betaN));
            std::cout << "    betaM " << roughness.betaM << " betaN " << roughness.betaN << ":";
            for (const double thetaO : { 0.0, 30.0, 60.0, 75.0, 85.0 }) {
                const double r = Albedo(lobes, thetaO, HAIR_LOBE_R).r;
                const double tt = Albedo(lobes, thetaO, HAIR_LOBE_TT).r;
                const double trt = Albedo(lobes, thetaO, HAIR_LOBE_TRT).r;
                const double total = r + tt + trt;
                highest = (std::max)(highest, total);
                lowest = (std::min)(lowest, total);
                CHECK(total <= 1.0 + kTolerance);
                CHECK(total > 0.2);
                std::cout << std::fixed << std::setprecision(3) << "  " << total << " (" << r << " + " << tt << " + " << trt << ")";
            }
            std::cout << "\n";
        }
        std::cout << std::defaultfloat << std::setprecision(6) << "    highest " << highest << ", lowest " << lowest << " (tolerance: 1 + " << kTolerance << ")\n";
    }

    void TestAbsorptionMonotonic()
    {
        // More melanin never brightens: the response in any direction, each channel, and the albedo.
        std::mt19937 random(11u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float amounts[] = { 0.0f, 0.03f, 0.15f, 0.6f, 2.0f, 8.0f };
        int rises = 0;
        for (int i = 0; i < 20000; ++i) {
            const float redness = unit(random);
            const float betaM = unit(random), betaN = unit(random);
            const Float3 view = RandomDirection(random), light = RandomDirection(random);
            Float3 previous{ 1.0e30f, 1.0e30f, 1.0e30f };
            for (const float melanin : amounts) {
                const Float3 value = HairBsdfEvaluate(HairBsdfMakeLobes(Material(HairSigmaAFromMelaninRedness(melanin, redness), betaM, betaN)),
                    kTangent, view, light, HAIR_LOBE_ALL);
                if (value.x > previous.x * 1.00001f || value.y > previous.y * 1.00001f || value.z > previous.z * 1.00001f) ++rises;
                previous = value;
            }
        }
        CHECK(rises == 0);
        std::cout << "  hair bsdf albedo (R + TT + TRT) at view inclination 30 degrees, betaM 0.3, betaN 0.3, by melanin:\n";
        Rgb previousAlbedo{ 2.0, 2.0, 2.0 };
        for (const float melanin : amounts) {
            const Rgb albedo = Albedo(HairBsdfMakeLobes(Material(HairSigmaAFromMelaninRedness(melanin, 0.0f), 0.3f, 0.3f)), 30.0, HAIR_LOBE_ALL);
            CHECK(albedo.r < previousAlbedo.r && albedo.g < previousAlbedo.g && albedo.b < previousAlbedo.b);
            // Blue is absorbed most and red least.
            CHECK(melanin == 0.0f || (albedo.r > albedo.g && albedo.g > albedo.b));
            previousAlbedo = albedo;
            std::cout << std::fixed << std::setprecision(3) << "    melanin " << melanin << ": " << albedo.r << ", " << albedo.g << ", " << albedo.b << "\n";
        }
        std::cout << std::defaultfloat << std::setprecision(6);
    }

    // The far-field response pbrt's HairBxDF gives: its f |cos(theta_i)| averaged over the offsets h across the fibre.
    std::array<Rgb, 4> PbrtAlbedo(Rgb sigmaA, double betaM, double betaN, double thetaODegrees)
    {
        const Float3 view = Direction(std::sin(thetaODegrees * kPi / 180.0), 0.0);
        std::array<Rgb, 4> sums;
        constexpr int offsets = 24, sineSteps = 400, phiSteps = 96;
        for (int k = 0; k < offsets; ++k) {
            const double h = -1.0 + 2.0 * (k + 0.5) / offsets;
            for (int i = 0; i < sineSteps; ++i) {
                const double sinTheta = -1.0 + 2.0 * (i + 0.5) / sineSteps;
                for (int j = 0; j < phiSteps; ++j) {
                    const auto lobes = pbrt::HairF(view, Direction(sinTheta, -kPi + 2.0 * kPi * (j + 0.5) / phiSteps), h, 1.55, sigmaA, betaM, betaN, 2.0);
                    for (int p = 0; p < 4; ++p) sums[p] += lobes[p];
                }
            }
        }
        for (auto& sum : sums) sum = sum * ((2.0 / sineSteps) * (2.0 * kPi / phiSteps) / offsets);
        return sums;
    }

    void TestAgainstPbrt()
    {
        // Not a check of equality: the two models differ by design (this one is three fitted far-field lobes). What is
        // compared is each lobe's energy, and it is printed for the document; the checks only bound it loosely.
        struct Colour { const char* name; float melanin; };
        std::cout << "  hair bsdf against pbrt's HairBxDF averaged across the fibre (betaM 0.3, betaN 0.3, alpha 2, eta 1.55): each lobe's\n"
                     "  energy, green channel, ours / pbrt's, at view inclinations 0, 40 and 70 degrees; pbrt's remaining orders last:\n";
        for (const Colour colour : { Colour{ "white", 0.0f }, Colour{ "blond", 0.15f }, Colour{ "brown", 2.0f }, Colour{ "black", 8.0f } }) {
            const Float3 sigmaA = HairSigmaAFromMelaninRedness(colour.melanin, 0.0f);
            const HairBsdfLobes lobes = HairBsdfMakeLobes(Material(sigmaA, 0.3f, 0.3f));
            std::cout << "    " << colour.name << ":";
            for (const double thetaO : { 0.0, 40.0, 70.0 }) {
                const auto reference = PbrtAlbedo(ToRgb(sigmaA), 0.3, 0.3, thetaO);
                const double r = Albedo(lobes, thetaO, HAIR_LOBE_R).g;
                const double tt = Albedo(lobes, thetaO, HAIR_LOBE_TT).g;
                const double trt = Albedo(lobes, thetaO, HAIR_LOBE_TRT).g;
                std::cout << std::fixed << std::setprecision(3) << "  R " << r << "/" << reference[0].g << " TT " << tt << "/" << reference[1].g
                          << " TRT " << trt << "/" << reference[2].g << " (" << reference[3].g << ")";
                // The same order of magnitude, lobe by lobe, wherever the lobe carries anything.
                CHECK(r < 2.5 * reference[0].g + 0.01 && r > 0.4 * reference[0].g - 0.01);
                CHECK(tt < 2.5 * reference[1].g + 0.01 && tt > 0.4 * reference[1].g - 0.01);
                CHECK(trt < 2.5 * reference[2].g + 0.02 && trt > 0.25 * reference[2].g - 0.02);
            }
            std::cout << "\n";
        }
        std::cout << std::defaultfloat << std::setprecision(6);
    }

    void TestEnvironment()
    {
        // The four directions against the integral they stand for: the response without TT, R scaled by
        // saturate(dot(light, view) + 1), against environments that vary as second-order spherical harmonics do.
        std::mt19937 random(23u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        double worst = 0.0, sumError = 0.0, worstUniform = 0.0;
        int count = 0;
        for (int i = 0; i < 24; ++i) {
            const Float3 view = Direction(std::sin((unit(random) * 150.0 - 75.0) * kPi / 180.0), unit(random) * 2.0 * kPi);
            const HairBsdfLobes lobes = HairBsdfForView(HairBsdfMakeLobes(Material(HairSigmaAFromMelaninRedness(unit(random) * 3.0f, unit(random)),
                0.15f + 0.5f * unit(random), unit(random))), kTangent, view);
            // A sky: brighter toward `up`, with a quadratic term. Non-negative everywhere. The first of each four is uniform.
            const Float3 up = RandomDirection(random);
            const float linear = (i % 4 == 0) ? 0.0f : 0.6f * unit(random);
            const float quadratic = (i % 4 == 0) ? 0.0f : 0.3f * unit(random);
            const auto radiance = [&](Float3 direction) {
                const float c = Dot(direction, up);
                return 1.0 + linear * c + quadratic * (3.0f * c * c - 1.0f) * 0.5f;
            };
            const Rgb reference = IntegrateSphere([&](Float3 light) {
                const double occlusion = std::clamp(static_cast<double>(Dot(light, view)) + 1.0, 0.0, 1.0);
                Rgb value = ToRgb(HairBsdfEvaluate(lobes, kTangent, view, light, HAIR_LOBE_R)) * occlusion;
                value += ToRgb(HairBsdfEvaluate(lobes, kTangent, view, light, HAIR_LOBE_TRT));
                return value * radiance(light);
            }, 1200, 240);
            Rgb approximation;
            for (uint sample = 0u; sample < HAIR_BSDF_ENVIRONMENT_SAMPLES; ++sample) {
                Float3 direction;
                const Float3 weight = HairBsdfEnvironmentSample(lobes, kTangent, view, sample, direction);
                CHECK(std::abs(Length(direction) - 1.0f) < 1.0e-4f);
                CHECK(weight.x >= 0.0f && weight.y >= 0.0f && weight.z >= 0.0f);
                approximation += ToRgb(weight) * radiance(direction);
            }
            const double error = std::abs(approximation.g - reference.g) / reference.g;
            worst = (std::max)(worst, error);
            if (i % 4 == 0) worstUniform = (std::max)(worstUniform, error);
            sumError += error;
            ++count;
        }
        CHECK(worst < 0.35);
        std::cout << "  hair environment: four directions against the integral, green channel, " << count << " cases: mean error "
                  << sumError / count * 100.0 << "%, worst " << worst * 100.0 << "%, worst under a uniform sky " << worstUniform * 100.0 << "%\n";
    }

} // namespace

int RunHairBsdfTests()
{
    TestParameterMappings();
    TestPieces();
    TestNonNegativeAndSymmetric();
    TestWhiteFurnace();
    TestAbsorptionMonotonic();
    TestAgainstPbrt();
    TestEnvironment();
    return g_failures;
}
