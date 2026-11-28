// Tests of hair's self-shadowing and multiple scattering: the scatter table and the dual scattering response
// (HairScatterShared.h) and the density volume's arithmetic (HairVolumeShared.h), the texts the shaders compile, compiled
// here as C++. Run from HairCoreTests.cpp's main(). The numbers the tests print are the ones docs/hair.md quotes.

#include "Hair/HairScatterShared.h"
#include "Hair/HairVolumeShared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

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
    constexpr Float3 kTangent{ 1.0f, 0.0f, 0.0f };

    Float3 Direction(double sinTheta, double phi)
    {
        const double cosTheta = std::sqrt((std::max)(0.0, 1.0 - sinTheta * sinTheta));
        return { static_cast<float>(sinTheta), static_cast<float>(cosTheta * std::cos(phi)), static_cast<float>(cosTheta * std::sin(phi)) };
    }

    HairBsdfMaterial Material(float melanin, float betaM, float betaN)
    {
        HairBsdfMaterial material;
        material.sigmaA = HairSigmaAFromMelaninRedness(melanin, 0.0f);
        material.betaM = betaM;
        material.betaN = betaN;
        material.alpha = 2.0f * (HAIR_BSDF_PI / 180.0f);
        material.eta = 1.55f;
        return material;
    }

    double Channel(Float3 v, int channel) { return channel == 0 ? v.x : channel == 1 ? v.y : v.z; }

    // One half of the directions under light at one inclination, by brute force: the scattering function itself summed
    // over every outgoing direction of the half. The fibre scatters light arriving along `incident` toward `outgoing` as
    // HairBsdfEvaluate(view = incident, light = outgoing) (the response is pbrt's f times the cosine, and f is the same
    // with the directions swapped), so the sum over outgoing directions is the energy the half receives.
    struct HalfMoments {
        double energy[3] = {};
        double shift[3] = {};
        double variance[3] = {};
    };

    void Integrate(const HairBsdfMaterial& material, double theta, HalfMoments& forward, HalfMoments& backward)
    {
        constexpr int kSinSteps = 1200;
        constexpr int kPhiSteps = 96;
        const Float3 incident = Direction(std::sin(theta), 0.0);
        const HairBsdfLobes lobes = HairBsdfForView(HairBsdfMakeLobes(material), kTangent, incident);
        double sums[2][3][3] = {}; // half, channel, moment
        for (int i = 0; i < kSinSteps; ++i) {
            const double sinOut = -1.0 + 2.0 * (i + 0.5) / kSinSteps;
            const double s = sinOut + std::sin(theta);
            for (int j = 0; j < kPhiSteps; ++j) {
                const double phi = -kPi + 2.0 * kPi * (j + 0.5) / kPhiSteps;
                const Float3 value = HairBsdfEvaluate(lobes, kTangent, incident, Direction(sinOut, phi), HAIR_LOBE_ALL);
                const int half = std::cos(phi) < 0.0 ? 0 : 1;
                for (int c = 0; c < 3; ++c) {
                    const double e = Channel(value, c);
                    sums[half][c][0] += e;
                    sums[half][c][1] += e * s;
                    sums[half][c][2] += e * s * s;
                }
            }
        }
        const double cell = (2.0 / kSinSteps) * (2.0 * kPi / kPhiSteps);
        HalfMoments* halves[2] = { &forward, &backward };
        for (int half = 0; half < 2; ++half) {
            for (int c = 0; c < 3; ++c) {
                const double energy = sums[half][c][0];
                halves[half]->energy[c] = energy * cell;
                halves[half]->shift[c] = energy > 0.0 ? sums[half][c][1] / energy : 0.0;
                halves[half]->variance[c] = energy > 0.0 ? sums[half][c][2] / energy - halves[half]->shift[c] * halves[half]->shift[c] : 0.0;
            }
        }
    }

    // The table against the same integral done finely, in double precision, at inclinations between its rows: what the
    // table's coarser sum, its single precision and the interpolation between rows cost.
    void TestScatterTable()
    {
        struct Case { const char* name; float melanin; float betaM; float betaN; };
        const Case cases[] = {
            { "white", 0.0f, 0.3f, 0.3f }, { "blond", 0.15f, 0.3f, 0.3f }, { "brown", 2.0f, 0.3f, 0.3f }, { "black", 8.0f, 0.3f, 0.3f },
            { "blond smooth", 0.15f, 0.15f, 0.3f }, { "blond rough around", 0.15f, 0.3f, 0.6f }, { "brown smooth", 2.0f, 0.15f, 0.3f },
        };
        const double angles[] = { 0.0, 30.0, 60.0 };
        double worstEnergy = 0.0;
        double worstEnergyNear = 0.0; // inclinations up to 30 degrees
        double worstDeviation = 0.0;
        double worstShift = 0.0;
        std::cout << "scatter table against the integral of the scattering function, green channel\n"
                  << "  material, inclination: a_f table / integral, a_b table / integral, beta_f, beta_b, alpha_f, alpha_b\n";
        for (const Case& c : cases) {
            const HairBsdfMaterial material = Material(c.melanin, c.betaM, c.betaN);
            const HairScatterTable table = HairScatterMakeTable(material);
            for (const double degrees : angles) {
                const double theta = degrees * kPi / 180.0;
                HalfMoments forward, backward;
                Integrate(material, theta, forward, backward);
                const HairScatterRow row = HairScatterLookup(table, static_cast<float>(std::sin(theta)));
                std::cout << std::fixed << std::setprecision(3) << "  " << c.name << ", " << std::setprecision(0) << degrees << std::setprecision(3)
                          << ": " << row.forward.y << " / " << forward.energy[1] << ", " << row.backward.y << " / " << backward.energy[1]
                          << ", " << std::sqrt(row.forwardVariance.y) << " / " << std::sqrt(forward.variance[1])
                          << ", " << std::sqrt(row.backwardVariance.y) << " / " << std::sqrt(backward.variance[1])
                          << ", " << row.forwardShift.y << " / " << forward.shift[1] << ", " << row.backwardShift.y << " / " << backward.shift[1] << "\n";
                for (int channel = 0; channel < 3; ++channel) {
                    const double tableValues[2][3] = {
                        { Channel(row.forward, channel), Channel(row.forwardShift, channel), Channel(row.forwardVariance, channel) },
                        { Channel(row.backward, channel), Channel(row.backwardShift, channel), Channel(row.backwardVariance, channel) } };
                    const HalfMoments* halves[2] = { &forward, &backward };
                    for (int half = 0; half < 2; ++half) {
                        const double energy = halves[half]->energy[channel];
                        // The halves of a dark fibre that hold next to no light are not compared in relative terms.
                        const double energyError = std::abs(tableValues[half][0] - energy) / (std::max)(energy, 0.02);
                        worstEnergy = (std::max)(worstEnergy, energyError);
                        if (degrees <= 30.0) worstEnergyNear = (std::max)(worstEnergyNear, energyError);
                        if (energy > 0.02) {
                            worstShift = (std::max)(worstShift, std::abs(tableValues[half][1] - halves[half]->shift[channel]));
                            worstDeviation = (std::max)(worstDeviation,
                                std::abs(std::sqrt(tableValues[half][2]) / std::sqrt(halves[half]->variance[channel]) - 1.0));
                        }
                        CHECK(tableValues[half][0] >= 0.0 && tableValues[half][0] <= 1.0);
                        CHECK(tableValues[half][2] > 0.0);
                    }
                    // A fibre scatters no more than it receives.
                    CHECK(tableValues[0][0] + tableValues[1][0] <= 1.02);
                }
            }
        }
        std::cout << std::setprecision(3) << "  largest differences over every case and channel: attenuation " << worstEnergy * 100.0 << "% ("
                  << worstEnergyNear * 100.0 << "% up to 30 degrees), spread " << worstDeviation * 100.0 << "%, shift " << worstShift << "\n";
        CHECK(worstEnergyNear < 0.02);
        CHECK(worstEnergy < 0.05);
        CHECK(worstDeviation < 0.05);
        CHECK(worstShift < 0.01);

        // Dark hair lets almost nothing on, light hair most of it; and what a fibre sends on falls with its melanin.
        const HairScatterRow white = HairScatterLookup(HairScatterMakeTable(Material(0.0f, 0.3f, 0.3f)), 0.0f);
        const HairScatterRow blond = HairScatterLookup(HairScatterMakeTable(Material(0.15f, 0.3f, 0.3f)), 0.0f);
        const HairScatterRow brown = HairScatterLookup(HairScatterMakeTable(Material(2.0f, 0.3f, 0.3f)), 0.0f);
        const HairScatterRow black = HairScatterLookup(HairScatterMakeTable(Material(8.0f, 0.3f, 0.3f)), 0.0f);
        CHECK(white.forward.y > blond.forward.y && blond.forward.y > brown.forward.y && brown.forward.y > black.forward.y);
        CHECK(white.forward.y > 0.7f);
        CHECK(black.forward.y < 0.05f);
        const auto back = [](const HairScatterRow& row) {
            return HairScatterMakeBack(row.forward.y, row.backward.y, row.forwardShift.y, row.backwardShift.y, row.forwardVariance.y,
                row.backwardVariance.y);
        };
        std::cout << "  across the fibre, green: a_f white " << white.forward.y << ", blond " << blond.forward.y << ", brown " << brown.forward.y
                  << ", black " << black.forward.y << "; a_b " << white.backward.y << ", " << blond.backward.y << ", " << brown.backward.y << ", "
                  << black.backward.y << "\n  backscattering lobe A_b: white " << back(white).attenuation << ", blond " << back(blond).attenuation
                  << ", brown " << back(brown).attenuation << ", black " << back(black).attenuation << "; its deviation " << std::sqrt(back(white).variance)
                  << ", " << std::sqrt(back(blond).variance) << ", " << std::sqrt(back(brown).variance) << ", " << std::sqrt(back(black).variance) << "\n";
        CHECK(back(white).attenuation > back(blond).attenuation && back(blond).attenuation > back(brown).attenuation);
        CHECK(back(black).attenuation < 1.0e-3f);
    }

    // The response integrated over every direction of the light, for one view: the energy of the parts asked for.
    Float3 Energy(const HairBsdfMaterial& material, const HairScatterTable& table, double viewTheta, float fibreCount, std::uint32_t parts)
    {
        constexpr int kSinSteps = 240;
        constexpr int kPhiSteps = 64;
        const Float3 view = Direction(std::sin(viewTheta), 0.0);
        const HairBsdfLobes lobes = HairBsdfForView(HairBsdfMakeLobes(material), kTangent, view);
        double sum[3] = {};
        for (int i = 0; i < kSinSteps; ++i) {
            const double sinIn = -1.0 + 2.0 * (i + 0.5) / kSinSteps;
            const HairScatterRow row = HairScatterLookup(table, static_cast<float>(sinIn));
            for (int j = 0; j < kPhiSteps; ++j) {
                const Float3 value = HairBsdfEvaluateDual(lobes, row, kTangent, view, Direction(sinIn, -kPi + 2.0 * kPi * (j + 0.5) / kPhiSteps),
                    HAIR_LOBE_ALL, fibreCount, HAIR_SCATTER_DENSITY_FACTOR, parts);
                sum[0] += value.x;
                sum[1] += value.y;
                sum[2] += value.z;
            }
        }
        const double cell = (2.0 / kSinSteps) * (2.0 * kPi / kPhiSteps);
        return { static_cast<float>(sum[0] * cell), static_cast<float>(sum[1] * cell), static_cast<float>(sum[2] * cell) };
    }

    void TestDualScattering()
    {
        std::mt19937 random(20261127u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const auto randomDirection = [&] {
            const float z = 2.0f * unit(random) - 1.0f;
            const float phi = 2.0f * HAIR_BSDF_PI * unit(random);
            const float r = std::sqrt((std::max)(0.0f, 1.0f - z * z));
            return Float3{ r * std::cos(phi), r * std::sin(phi), z };
        };
        // Never negative, never not finite; and with no fibre in the way the direct part is the single fibre's response.
        HairScatterTable table = HairScatterMakeTable(Material(0.15f, 0.3f, 0.3f));
        for (int trial = 0; trial < 60000; ++trial) {
            const HairBsdfMaterial material = Material(8.0f * unit(random) * unit(random), 0.05f + 0.95f * unit(random), unit(random));
            // Mostly another material's table: the response has to hold for any row.
            if (trial % 500 == 0) table = HairScatterMakeTable(material);
            const Float3 tangent = randomDirection();
            const Float3 view = randomDirection();
            const Float3 light = randomDirection();
            const HairBsdfLobes lobes = HairBsdfForView(HairBsdfMakeLobes(material), tangent, view);
            const HairScatterRow row = HairScatterLookup(table, Dot(tangent, light));
            const float fibreCount = trial % 7 == 0 ? 0.0f : 40.0f * unit(random) * unit(random);
            const std::uint32_t parts = 1u + static_cast<std::uint32_t>(trial % 7);
            const Float3 value = HairBsdfEvaluateDual(lobes, row, tangent, view, light, HAIR_LOBE_ALL, fibreCount, HAIR_SCATTER_DENSITY_FACTOR, parts);
            CHECK(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z));
            CHECK(value.x >= 0.0f && value.y >= 0.0f && value.z >= 0.0f);
            const Float3 single = HairBsdfEvaluate(lobes, tangent, view, light, HAIR_LOBE_ALL);
            const Float3 direct = HairBsdfEvaluateDual(lobes, row, tangent, view, light, HAIR_LOBE_ALL, 0.0f, HAIR_SCATTER_DENSITY_FACTOR,
                HAIR_SCATTER_PART_DIRECT);
            CHECK(direct == single);
            for (std::uint32_t index = 0u; index < HAIR_BSDF_ENVIRONMENT_SAMPLES; ++index) {
                const Float3 direction = HairBsdfEnvironmentDirection(tangent, view, index);
                const float sinThetaO = HairBsdfClamp(Dot(tangent, view), -1.0f, 1.0f);
                const Float3 weight = HairBsdfEnvironmentWeightDual(lobes, row, view, std::sqrt(1.0f - sinThetaO * sinThetaO), index, direction,
                    fibreCount, HAIR_SCATTER_DENSITY_FACTOR, trial % 2 == 0, parts);
                CHECK(std::isfinite(weight.x) && std::isfinite(weight.y) && std::isfinite(weight.z));
                CHECK(weight.x >= 0.0f && weight.y >= 0.0f && weight.z >= 0.0f);
            }
        }

        // What hair in the way does to the light, in energy: the response summed over every light direction.
        struct Case { const char* name; float melanin; };
        const Case cases[] = { { "white", 0.0f }, { "blond", 0.15f }, { "brown", 2.0f }, { "black", 8.0f } };
        std::cout << "dual scattering, energy toward a view across the fibre summed over light directions, green channel\n"
                  << "  fibres in the way: single lobes with nothing in the way; then direct + forward scattered + backscattered = all, for 0, 1, 4 and 16 fibres\n";
        for (const Case& c : cases) {
            const HairBsdfMaterial material = Material(c.melanin, 0.3f, 0.3f);
            const HairScatterTable table = HairScatterMakeTable(material);
            const Float3 single = Energy(material, table, 0.0, 0.0f, HAIR_SCATTER_PART_DIRECT);
            std::cout << std::fixed << std::setprecision(3) << "  " << c.name << ": " << single.y << ";";
            float previous = 1.0e9f;
            for (const float fibres : { 0.0f, 1.0f, 4.0f, 16.0f }) {
                const Float3 direct = Energy(material, table, 0.0, fibres, HAIR_SCATTER_PART_DIRECT);
                const Float3 global = Energy(material, table, 0.0, fibres, HAIR_SCATTER_PART_GLOBAL);
                const Float3 local = Energy(material, table, 0.0, fibres, HAIR_SCATTER_PART_LOCAL);
                const Float3 all = Energy(material, table, 0.0, fibres, HAIR_SCATTER_PART_ALL);
                std::cout << "  " << direct.y << " + " << global.y << " + " << local.y << " = " << all.y << (fibres == 16.0f ? "\n" : ";");
                // The parts add up, and more hair in the way never means more light.
                CHECK(std::abs(direct.y + global.y + local.y - all.y) < 1.0e-3f);
                CHECK(all.y <= previous + 1.0e-4f);
                previous = all.y;
                if (fibres == 0.0f) {
                    // Nothing in the way: the single fibre's response, and around it the backscattering lobe, whose
                    // energy is its attenuation times the density factor (each lobe keeps its integral).
                    CHECK(std::abs(direct.y - single.y) < 1.0e-4f);
                    CHECK(global.y == 0.0f);
                    const HairScatterRow row = HairScatterLookup(table, 0.0f);
                    const HairScatterBack back = HairScatterMakeBack(row.forward.y, row.backward.y, row.forwardShift.y, row.backwardShift.y,
                        row.forwardVariance.y, row.backwardVariance.y);
                    CHECK(std::abs(local.y - HAIR_SCATTER_DENSITY_FACTOR * back.attenuation) < 0.03f * (std::max)(back.attenuation, 0.01f) + 2.0e-3f);
                }
                if (fibres == 16.0f) {
                    // Deep in the hair nothing arrives directly.
                    CHECK(direct.y < 1.0e-5f);
                }
            }
        }
        // Black hair in shadow of its own hair is black: four fibres in the way leave under a twentieth. Light hair is
        // lit through four fibres at over a quarter of what it has in the open.
        const auto remaining = [](float melanin, float fibres) {
            const HairBsdfMaterial material = Material(melanin, 0.3f, 0.3f);
            const HairScatterTable table = HairScatterMakeTable(material);
            return Energy(material, table, 0.0, fibres, HAIR_SCATTER_PART_ALL).y / Energy(material, table, 0.0, 0.0f, HAIR_SCATTER_PART_ALL).y;
        };
        const float blackLeft = remaining(8.0f, 4.0f);
        const float blondLeft = remaining(0.15f, 4.0f);
        std::cout << "  of the light in the open, behind four fibres: black " << blackLeft << ", blond " << blondLeft << "\n";
        CHECK(blackLeft < 0.05f);
        CHECK(blondLeft > 0.15f);
    }

    // A density volume on the CPU, built and marched with the functions the shaders use.
    struct Volume {
        std::uint32_t resolution = 0u;
        float voxelSize = 0.0f;
        std::vector<std::uint32_t> voxels;
        double deposited = 0.0; // units, as asked for (before rounding and before what falls outside)

        Volume(std::uint32_t res, float size) : resolution(res), voxelSize(size), voxels(static_cast<std::size_t>(res) * res * res, 0u) {}

        bool Contains(int x, int y, int z) const
        {
            const int r = static_cast<int>(resolution);
            return x >= 0 && y >= 0 && z >= 0 && x < r && y < r && z < r;
        }
        std::uint32_t& At(int x, int y, int z) { return voxels[static_cast<std::size_t>(x) + resolution * (static_cast<std::size_t>(y) + resolution * static_cast<std::size_t>(z))]; }

        // hairVolume.hlsl's HairVoxelizeCS, for one segment.
        void Deposit(Float3 a, Float3 b, float diameter, std::uint32_t row, bool trilinear)
        {
            const float length = Length(b - a);
            const std::uint32_t count = HairVolumeDepositCount(length / voxelSize);
            const float amount = HairVolumeSegmentDeposit(length, diameter, voxelSize) / static_cast<float>(count);
            for (std::uint32_t deposit = 0u; deposit < count; ++deposit) {
                const Float3 at = Lerp(a, b, (static_cast<float>(deposit) + 0.5f) / static_cast<float>(count)) * (1.0f / voxelSize);
                deposited += amount;
                if (trilinear) {
                    const Float3 centred = at - Float3{ 0.5f, 0.5f, 0.5f };
                    const Float3 low{ std::floor(centred.x), std::floor(centred.y), std::floor(centred.z) };
                    const Float3 share = centred - low;
                    for (std::uint32_t corner = 0u; corner < 8u; ++corner) {
                        const int x = static_cast<int>(low.x) + static_cast<int>(corner & 1u);
                        const int y = static_cast<int>(low.y) + static_cast<int>((corner >> 1u) & 1u);
                        const int z = static_cast<int>(low.z) + static_cast<int>(corner >> 2u);
                        const std::uint32_t units = HairVolumeRound(amount * HairVolumeCornerWeight(share.x, share.y, share.z, corner),
                            HairVolumeHash(row * 32u + deposit * 8u + corner));
                        if (units != 0u && Contains(x, y, z)) At(x, y, z) += units;
                    }
                } else {
                    const int x = static_cast<int>(std::floor(at.x)), y = static_cast<int>(std::floor(at.y)), z = static_cast<int>(std::floor(at.z));
                    const std::uint32_t units = HairVolumeRound(amount, HairVolumeHash(row * 32u + deposit * 8u));
                    if (units != 0u && Contains(x, y, z)) At(x, y, z) += units;
                }
            }
        }

        // hairVolume.hlsli's HairMarchVolume, for a ray that starts inside the box.
        float March(Float3 origin, Float3 direction, std::uint32_t steps, float bias, float offset)
        {
            const float extent = static_cast<float>(resolution) * voxelSize;
            float exit = 1.0e30f;
            const float o[3] = { origin.x, origin.y, origin.z };
            const float d[3] = { direction.x, direction.y, direction.z };
            for (int axis = 0; axis < 3; ++axis) {
                if (std::abs(d[axis]) < 1.0e-6f) continue;
                exit = (std::min)(exit, ((d[axis] > 0.0f ? extent : 0.0f) - o[axis]) / d[axis]);
            }
            const float start = bias * voxelSize;
            float fibres = 0.0f;
            if (!(exit > start)) return fibres;
            const int last = static_cast<int>(resolution) - 1;
            const auto voxel = [&](float x, float y, float z) {
                return At(std::clamp(static_cast<int>(x), 0, last), std::clamp(static_cast<int>(y), 0, last), std::clamp(static_cast<int>(z), 0, last));
            };
            for (std::uint32_t index = 0u; index < steps; ++index) {
                const Float3 at = (origin + direction * HairVolumeMarchDistance(index, steps, offset, start, exit)) * (1.0f / voxelSize);
                const float length = HairVolumeMarchLength(index, steps, start, exit);
                if (index >= HAIR_VOLUME_INTERPOLATED_SAMPLES) {
                    fibres += HairVolumeFibres(voxel(std::floor(at.x), std::floor(at.y), std::floor(at.z)), length);
                    continue;
                }
                // hairVolume.hlsli's HairVolumeSample, interpolated.
                const Float3 centred = at - Float3{ 0.5f, 0.5f, 0.5f };
                const Float3 low{ std::floor(centred.x), std::floor(centred.y), std::floor(centred.z) };
                const Float3 share = centred - low;
                float density = 0.0f;
                for (std::uint32_t corner = 0u; corner < 8u; ++corner) {
                    density += HairVolumeCornerWeight(share.x, share.y, share.z, corner) * static_cast<float>(
                        voxel(low.x + static_cast<float>(corner & 1u), low.y + static_cast<float>((corner >> 1u) & 1u), low.z + static_cast<float>(corner >> 2u)) &
                        HAIR_VOLUME_DENSITY_MASK);
                }
                fibres += HairVolumeFibresOfDensity(density, length);
            }
            return fibres;
        }
    };

    // A slab of straight parallel fibres of known density, voxelized and marched: the fibre count through it against
    // what the density says it is, and the same slab drawn with a fifth of its strands, each five times as wide.
    void TestVolumeSlab()
    {
        constexpr std::uint32_t kResolution = 32u;
        constexpr float kVoxel = 0.005f;           // a 16 cm box
        constexpr float kSlabLow = 0.06f;          // the slab: 2 cm thick, across x
        constexpr float kSlabHigh = 0.08f;
        constexpr float kDiameter = 80.0e-6f;
        constexpr std::uint32_t kStrands = 4000u;  // along z, from 1 cm to 15 cm, in 4 mm segments
        const float extent = static_cast<float>(kResolution) * kVoxel;
        // Length times diameter over volume, and the fibres a ray across the slab is expected to cross.
        const float density = static_cast<float>(kStrands) * kDiameter / ((kSlabHigh - kSlabLow) * extent);
        const float expected = density * HAIR_VOLUME_ISOTROPIC_SINE * (kSlabHigh - kSlabLow);

        const auto build = [&](std::uint32_t strands, float diameter, bool trilinear, std::uint32_t seed) {
            Volume volume(kResolution, kVoxel);
            std::mt19937 random(seed);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            std::uint32_t row = 0u;
            for (std::uint32_t strand = 0u; strand < strands; ++strand) {
                const float x = kSlabLow + (kSlabHigh - kSlabLow) * unit(random);
                const float y = extent * unit(random);
                for (int segment = 0; segment < 35; ++segment) {
                    volume.Deposit({ x, y, 0.01f + 0.004f * static_cast<float>(segment) }, { x, y, 0.01f + 0.004f * static_cast<float>(segment + 1) },
                        diameter, row++, trilinear);
                }
            }
            return volume;
        };
        // The mean and the spread of marches across the slab from in front of it, over places and offsets.
        const auto measure = [&](Volume& volume, Float3 direction, std::uint32_t steps, float fromX, float& mean, float& deviation) {
            std::mt19937 random(7u);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            double sum = 0.0, squares = 0.0;
            constexpr int kRays = 4000;
            for (int ray = 0; ray < kRays; ++ray) {
                const Float3 origin{ fromX, extent * (0.3f + 0.4f * unit(random)), extent * (0.3f + 0.4f * unit(random)) };
                const float fibres = volume.March(origin, direction, steps, 0.0f, unit(random));
                sum += fibres;
                squares += static_cast<double>(fibres) * fibres;
            }
            mean = static_cast<float>(sum / kRays);
            deviation = static_cast<float>(std::sqrt((std::max)(0.0, squares / kRays - (sum / kRays) * (sum / kRays))));
        };

        Volume full = build(kStrands, kDiameter, true, 1u);
        // What was deposited is what the strands hold: voxels sum to length times diameter over a voxel's volume.
        double total = 0.0;
        for (const std::uint32_t voxel : full.voxels) total += voxel & HAIR_VOLUME_DENSITY_MASK;
        const double held = static_cast<double>(kStrands) * 0.14 * kDiameter / (static_cast<double>(kVoxel) * kVoxel * kVoxel) * HAIR_VOLUME_DENSITY_UNIT;
        std::cout << std::fixed << std::setprecision(4) << "density volume, a slab of " << kStrands << " fibres: deposited " << total / held
                  << " of length times diameter over a voxel's volume\n";
        CHECK(std::abs(total / held - 1.0) < 0.01);

        std::cout << "  fibres across the slab, expected " << expected << " (transmittance " << std::exp(-expected) << "): marched\n";
        const Float3 across{ 1.0f, 0.0f, 0.0f };
        for (const std::uint32_t steps : { 4u, 8u, 16u, 64u }) {
            float mean = 0.0f, deviation = 0.0f;
            measure(full, across, steps, 0.01f, mean, deviation);
            std::cout << "    " << steps << " steps: " << mean << " +- " << deviation << " (transmittance " << std::exp(-mean) << ")\n";
            // The steps cover the ray whatever their number: the mean is the density's, and the offset's noise falls.
            CHECK(std::abs(mean / expected - 1.0f) < 0.03f);
        }
        // Obliquely the path through the slab is longer.
        {
            float mean = 0.0f, deviation = 0.0f;
            const Float3 oblique = Normalize(Float3{ 1.0f, 0.0f, 0.45f });
            measure(full, oblique, 16u, 0.01f, mean, deviation);
            const float longer = std::sqrt(1.0f + 0.45f * 0.45f);
            std::cout << "    at 24 degrees to that, 16 steps: " << mean << " +- " << deviation << ", expected " << expected * longer << "\n";
            CHECK(std::abs(mean / (expected * longer) - 1.0f) < 0.04f);
        }
        // The same slab at a lower level of detail: a fifth of the strands, each five times as wide.
        {
            Volume reduced = build(kStrands / 5u, kDiameter * 5.0f, true, 2u);
            float mean = 0.0f, deviation = 0.0f;
            measure(reduced, across, 16u, 0.01f, mean, deviation);
            std::cout << "    a fifth of the strands, five times as wide, 16 steps: " << mean << " +- " << deviation << "\n";
            CHECK(std::abs(mean / expected - 1.0f) < 0.03f);
        }
        // Each deposit into its nearest voxel alone: the same count.
        {
            Volume nearest = build(kStrands, kDiameter, false, 1u);
            float mean = 0.0f, deviation = 0.0f;
            measure(nearest, across, 16u, 0.01f, mean, deviation);
            std::cout << "    deposits to the nearest voxel, 16 steps: " << mean << " +- " << deviation << "\n";
            CHECK(std::abs(mean / expected - 1.0f) < 0.03f);
        }
        // From the slab's own lit face toward the light there is no hair in the way, but the deposits are spread over a
        // voxel either side, so a march from the face reads some: what the bias at a march's start is for.
        const Float3 toLight{ -1.0f, 0.0f, 0.0f };
        for (const float bias : { 0.0f, 0.5f, 1.0f, 1.5f }) {
            std::mt19937 random(11u);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            double face = 0.0, middle = 0.0;
            constexpr int kRays = 4000;
            for (int ray = 0; ray < kRays; ++ray) {
                const float y = extent * (0.3f + 0.4f * unit(random)), z = extent * (0.3f + 0.4f * unit(random));
                const float offset = unit(random);
                face += full.March({ kSlabLow, y, z }, toLight, 8u, bias, offset);
                middle += full.March({ 0.5f * (kSlabLow + kSlabHigh), y, z }, toLight, 8u, bias, offset);
            }
            std::cout << "    from the lit face, 8 steps, bias " << std::setprecision(1) << bias << std::setprecision(4) << ": " << face / kRays
                      << " fibres where there are none; from the middle of the slab " << middle / kRays << " where there are " << 0.5f * expected << "\n";
            if (bias == 1.0f) {
                CHECK(face / kRays < 0.05 * expected);
                CHECK(middle / kRays < 0.5 * expected);
            }
        }
    }

} // namespace

int RunHairScatterTests()
{
    TestScatterTable();
    TestDualScattering();
    TestVolumeSlab();
    return g_failures;
}
