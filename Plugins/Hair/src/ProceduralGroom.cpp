#include "Hair/ProceduralGroom.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace br::hair {

namespace {

    constexpr float kTwoPi = 6.28318530717958647692f;

    // splitmix64: the same sequence on every platform, unlike the <random> distributions.
    struct Random {
        std::uint64_t state;

        Random(std::uint32_t seed, std::uint32_t strand)
            : state((static_cast<std::uint64_t>(seed) << 32) | strand)
        {
        }

        // Uniform in [0, 1).
        float Next()
        {
            state += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
        }
    };

    void ReserveStrands(GroomSource& groom, const ProceduralGroomSettings& settings)
    {
        const std::size_t pointCount = static_cast<std::size_t>(settings.strandCount) * settings.pointsPerStrand;
        groom.strandPointOffsets.reserve(settings.strandCount + 1u);
        groom.positions.reserve(pointCount);
        groom.widths.reserve(pointCount);
        groom.strandPointOffsets.push_back(0u);
    }

    // What a drooping strand must stay outside of: a sphere, or (radius 0) the plane through its root.
    struct Obstacle {
        Float3 center;
        float radius = 0.0f;
    };

    // Appends one strand that starts at `root` and grows along the unit `normal`.
    void AppendStrand(GroomSource& groom, const ProceduralGroomSettings& settings, Random& random, Float3 root, Float3 normal,
                      const Obstacle& obstacle)
    {
        const float length = settings.strandLength * (1.0f - settings.lengthVariation * random.Next());
        const float frameAngle = kTwoPi * random.Next();
        const float phase = kTwoPi * random.Next();

        // Tangent frame turned by a random angle around the normal, so neighbouring waves are not aligned.
        Float3 tangent, bitangent;
        BuildTangentFrame(normal, tangent, bitangent);
        const Float3 side = tangent * std::cos(frameAngle) + bitangent * std::sin(frameAngle);
        const Float3 up = Cross(normal, side);

        // Droop: the growth direction is pulled toward droopDirection step by step, and the strand lies a little above
        // the obstacle where it would otherwise enter it. A strand growing straight against the pull is started slightly
        // to its side, or it would stand balanced.
        const bool droops = settings.droop > 0.0f;
        const Float3 down = Normalize(settings.droopDirection);
        const float clearance = droops ? obstacle.radius * (0.01f + 0.04f * random.Next()) : 0.0f;
        const auto keepOutside = [&](Float3 position, float margin) {
            if (obstacle.radius > 0.0f) {
                const Float3 offset = position - obstacle.center;
                const float distance = Length(offset);
                const float least = obstacle.radius + margin;
                return distance < least && distance > 0.0f ? obstacle.center + offset * (least / distance) : position;
            }
            const float height = Dot(position - root, normal);
            return height < 0.0f ? position - normal * height : position;
        };
        Float3 direction = droops ? Normalize(normal + side * 0.05f) : normal;
        Float3 center = root;

        const std::uint32_t pointCount = settings.pointsPerStrand;
        for (std::uint32_t point = 0; point < pointCount; ++point) {
            const float t = static_cast<float>(point) / static_cast<float>(pointCount - 1u);
            Float3 position = root + normal * (length * t);
            if (droops) {
                if (point != 0u) {
                    const float step = 1.0f / static_cast<float>(pointCount - 1u);
                    direction = Normalize(direction + down * (settings.droop * step));
                    center = keepOutside(center + direction * (length * step), clearance);
                }
                position = center;
            }

            // The lateral offset ramps in over the first quarter so the root stays on the surface.
            const float amplitude = settings.waveAmplitude * std::min(1.0f, t * 4.0f);
            const float angle = kTwoPi * settings.waveCount * t + phase;
            if (settings.style == ProceduralHairStyle::Wavy) {
                position = position + side * (amplitude * std::sin(angle));
            } else if (settings.style == ProceduralHairStyle::Curly) {
                position = position + side * (amplitude * std::cos(angle)) + up * (amplitude * std::sin(angle));
            }

            groom.positions.push_back(droops && point != 0u ? keepOutside(position, 0.0f) : position);
            groom.widths.push_back(settings.rootWidth + (settings.tipWidth - settings.rootWidth) * t);
        }
        groom.strandPointOffsets.push_back(static_cast<std::uint32_t>(groom.positions.size()));
    }

} // namespace

GroomSource GenerateGroomOnSphereCap(const ProceduralGroomSettings& settings)
{
    GroomSource groom;
    if (settings.strandCount == 0u || settings.pointsPerStrand < 2u) {
        return groom;
    }
    ReserveStrands(groom, settings);

    const Float3 axis = Normalize(settings.capAxis);
    Float3 tangent, bitangent;
    BuildTangentFrame(axis, tangent, bitangent);
    const float cosCapAngle = std::cos(settings.capAngleRadians);

    for (std::uint32_t strand = 0; strand < settings.strandCount; ++strand) {
        Random random(settings.seed, strand);
        // Uniform over the cap's area: uniform in cos(theta) and in phi.
        const float cosTheta = 1.0f - random.Next() * (1.0f - cosCapAngle);
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const float phi = kTwoPi * random.Next();
        const Float3 normal = Normalize(axis * cosTheta + tangent * (sinTheta * std::cos(phi)) + bitangent * (sinTheta * std::sin(phi)));
        AppendStrand(groom, settings, random, settings.capCenter + normal * settings.capRadius, normal,
                     Obstacle{ settings.capCenter, settings.capRadius });
    }
    return groom;
}

GroomSource GenerateGroomOnTriangles(const ProceduralGroomSettings& settings,
                                     std::span<const Float3> positions,
                                     std::span<const std::uint32_t> indices)
{
    GroomSource groom;
    if (settings.strandCount == 0u || settings.pointsPerStrand < 2u || indices.empty() || indices.size() % 3u != 0u) {
        return groom;
    }
    for (const std::uint32_t index : indices) {
        if (index >= positions.size()) {
            return groom;
        }
    }

    // Running area total per triangle, for area-weighted selection.
    const std::size_t triangleCount = indices.size() / 3u;
    std::vector<double> cumulativeArea(triangleCount);
    double totalArea = 0.0;
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
        const Float3 a = positions[indices[triangle * 3u]];
        const Float3 b = positions[indices[triangle * 3u + 1u]];
        const Float3 c = positions[indices[triangle * 3u + 2u]];
        totalArea += 0.5 * static_cast<double>(Length(Cross(b - a, c - a)));
        cumulativeArea[triangle] = totalArea;
    }
    if (!(totalArea > 0.0)) {
        return groom;
    }
    ReserveStrands(groom, settings);

    for (std::uint32_t strand = 0; strand < settings.strandCount; ++strand) {
        Random random(settings.seed, strand);
        const double pick = static_cast<double>(random.Next()) * totalArea;
        // upper_bound skips zero-area triangles, whose running total equals their predecessor's.
        const std::size_t triangle = std::min<std::size_t>(
            static_cast<std::size_t>(std::upper_bound(cumulativeArea.begin(), cumulativeArea.end(), pick) - cumulativeArea.begin()),
            triangleCount - 1u);
        const Float3 a = positions[indices[triangle * 3u]];
        const Float3 b = positions[indices[triangle * 3u + 1u]];
        const Float3 c = positions[indices[triangle * 3u + 2u]];

        // Uniform barycentrics: fold the unit square onto the triangle.
        float u = random.Next();
        float v = random.Next();
        if (u + v > 1.0f) {
            u = 1.0f - u;
            v = 1.0f - v;
        }
        const Float3 root = a + (b - a) * u + (c - a) * v;
        AppendStrand(groom, settings, random, root, Normalize(Cross(b - a, c - a)), Obstacle{});
    }
    return groom;
}

} // namespace br::hair
