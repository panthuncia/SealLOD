// Builder, LOD-invariant and .groom tests. Run from HairCoreTests.cpp's main().

#include "Hair/GroomBuilder.h"
#include "Hair/GroomFile.h"
#include "Hair/GroomSource.h"
#include "Hair/HairGPUTypes.h"
#include "Hair/HairLod.h"
#include "Hair/ProceduralGroom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace br::hair;

namespace {

    int g_failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __FILE__ << "(" << __LINE__ << "): check failed: " #condition << " [" << g_label << "]\n"; \
            ++g_failures; \
        } \
    } while (false)

    std::string g_label;

    constexpr float kPi = 3.14159265358979323846f;

    // Stated bounds.
    // Prefix coverage: the largest gap from any root to the nearest root of a rank prefix of size m is at most this
    // many times sqrt(rootArea / m), the spacing of m roots on a square lattice over the same area. A perfect
    // hexagonal layout would reach 0.62; greedy farthest-point ordering guarantees twice the optimum (1.24), and
    // these fixtures measure about 0.9.
    constexpr float kPrefixCoverageFactor = 1.25f;
    // Resampling: every source point is within this distance of the built strand. Straight strands must match to
    // quantization. The curved fixtures have a 10 mm amplitude and only 8 source points per wave or turn, and the
    // builder re-spaces the same number of points by arc length, which cuts those 45-degree corners by up to
    // ~1.7 mm. A densely sampled source is limited by the builder's own segment budget instead (one segment per
    // maxSegmentAngleRadians of turning: about 0.45 mm of sag on these curves) (TestResampleConverges).
    constexpr float kResampleToleranceCurved = 0.002f;
    constexpr float kResampleToleranceStraight = 0.00001f;
    constexpr float kResampleToleranceDense = 0.0005f;

    float DistanceToSegment(Float3 p, Float3 a, Float3 b)
    {
        const Float3 ab = b - a;
        const float lengthSquared = Dot(ab, ab);
        const float t = lengthSquared > 0.0f ? std::clamp(Dot(p - a, ab) / lengthSquared, 0.0f, 1.0f) : 0.0f;
        return Length(p - (a + ab * t));
    }

    float DistanceToPolyline(Float3 p, std::span<const Float3> line)
    {
        float best = Length(p - line[0]);
        for (std::size_t i = 0; i + 1u < line.size(); ++i) {
            best = std::min(best, DistanceToSegment(p, line[i], line[i + 1u]));
        }
        return best;
    }

    float Sum(const float* values, std::size_t count)
    {
        float sum = 0.0f;
        for (std::size_t i = 0; i < count; ++i) {
            sum += values[i];
        }
        return sum;
    }

    // A hemisphere of radius 0.1 around the origin, facing +Y, with skin weights that differ per vertex.
    ScalpMesh MakeDomeScalp()
    {
        constexpr std::uint32_t kRings = 8u, kSegments = 16u;
        ScalpMesh scalp;
        const auto addVertex = [&scalp](Float3 position) {
            const std::uint32_t vertex = static_cast<std::uint32_t>(scalp.positions.size());
            scalp.positions.push_back(position);
            scalp.joints.push_back({ vertex % 5u, vertex % 5u + 1u, 7u, 0u });
            scalp.weights.push_back({ 0.6f, 0.3f, 0.1f, 0.0f });
        };
        addVertex({ 0.0f, 0.1f, 0.0f });
        for (std::uint32_t ring = 1; ring <= kRings; ++ring) {
            const float theta = 0.5f * kPi * static_cast<float>(ring) / kRings;
            for (std::uint32_t segment = 0; segment < kSegments; ++segment) {
                const float phi = 2.0f * kPi * static_cast<float>(segment) / kSegments;
                addVertex({ 0.1f * std::sin(theta) * std::cos(phi), 0.1f * std::cos(theta), 0.1f * std::sin(theta) * std::sin(phi) });
            }
        }
        const auto addTriangle = [&scalp](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            // Wind counter-clockwise seen from outside the sphere.
            const Float3 normal = Cross(scalp.positions[b] - scalp.positions[a], scalp.positions[c] - scalp.positions[a]);
            if (Dot(normal, scalp.positions[a] + scalp.positions[b] + scalp.positions[c]) < 0.0f) {
                std::swap(b, c);
            }
            scalp.indices.insert(scalp.indices.end(), { a, b, c });
        };
        const auto ringVertex = [](std::uint32_t ring, std::uint32_t segment) { return 1u + (ring - 1u) * kSegments + segment % kSegments; };
        for (std::uint32_t segment = 0; segment < kSegments; ++segment) {
            addTriangle(0u, ringVertex(1u, segment), ringVertex(1u, segment + 1u));
            for (std::uint32_t ring = 1; ring < kRings; ++ring) {
                addTriangle(ringVertex(ring, segment), ringVertex(ring + 1u, segment), ringVertex(ring + 1u, segment + 1u));
                addTriangle(ringVertex(ring, segment), ringVertex(ring + 1u, segment + 1u), ringVertex(ring, segment + 1u));
            }
        }
        return scalp;
    }

    float MeshArea(const ScalpMesh& scalp)
    {
        float area = 0.0f;
        for (std::size_t i = 0; i < scalp.indices.size(); i += 3u) {
            const Float3 a = scalp.positions[scalp.indices[i]];
            area += 0.5f * Length(Cross(scalp.positions[scalp.indices[i + 1u]] - a, scalp.positions[scalp.indices[i + 2u]] - a));
        }
        return area;
    }

    std::vector<Float3> DecodeStrand(const BuiltGroom& groom, std::uint32_t strand)
    {
        const HairClusterHeader& cluster = groom.clusters[groom.strands[strand].clusterIndex];
        std::vector<Float3> points(HairStrandPointCount(cluster.vertexLevel));
        for (std::size_t i = 0; i < points.size(); ++i) {
            points[i] = DecodeHairPointPosition(cluster, groom.points[groom.strands[strand].pointOffset + i]);
        }
        return points;
    }

    struct BuildStats {
        float maxSourceDeviation = 0.0f; // source point to built strand
        float maxCoverageRatio = 0.0f;   // prefix gap over sqrt(area / m)
    };

    // Every invariant of a built groom. `rootArea` is the area the roots were scattered over.
    BuildStats CheckBuiltGroom(const GroomSource& source,
                               const ScalpMesh* scalp,
                               const GroomBuildSettings& settings,
                               const BuiltGroom& groom,
                               const GroomBuildInfo& info,
                               float rootArea,
                               float resampleTolerance)
    {
        BuildStats stats;
        const std::uint32_t strandCount = source.StrandCount();
        CHECK(ValidateBuiltGroom(groom).empty());
        CHECK(groom.strands.size() == strandCount && groom.rootBindings.size() == strandCount && info.sourceStrandIndices.size() == strandCount);
        if (!ValidateBuiltGroom(groom).empty() || groom.strands.size() != strandCount || info.sourceStrandIndices.size() != strandCount) {
            return stats; // the checks below index through these arrays
        }
        const std::uint32_t guideCount = groom.GuideCount();

        // Points: 2^k + 1 per strand, packed back to back; quantization and resampling error.
        {
            bool contiguous = true, levels = true, quantized = true, widths = true, endpoints = true, builtOnSource = true;
            std::uint32_t nextPoint = 0u;
            std::vector<Float3> reference;
            std::vector<float> referenceWidths;
            for (const HairClusterHeader& cluster : groom.clusters) {
                levels = levels && cluster.vertexLevel >= settings.minVertexLevel && cluster.vertexLevel <= settings.maxVertexLevel;
                const Float3 scale = cluster.quantizationScale;
                for (std::uint32_t strand = cluster.strandOffset; strand < cluster.strandOffset + cluster.strandCount; ++strand) {
                    const HairStrandRecord& record = groom.strands[strand];
                    contiguous = contiguous && record.pointOffset == nextPoint;
                    nextPoint += HairStrandPointCount(cluster.vertexLevel);

                    const std::uint32_t sourceStrand = info.sourceStrandIndices[strand];
                    const std::span<const Float3> sourcePoints(source.positions.data() + source.StrandPointOffset(sourceStrand),
                                                               source.StrandPointCount(sourceStrand));
                    ResampleStrand(source, sourceStrand, cluster.vertexLevel, reference, referenceWidths);
                    const std::vector<Float3> decoded = DecodeStrand(groom, strand);
                    endpoints = endpoints && reference.front() == sourcePoints.front() && reference.back() == sourcePoints.back();
                    for (std::size_t i = 0; i < decoded.size(); ++i) {
                        // Half a quantization step per axis, plus float rounding of the encode and decode.
                        const Float3 error = decoded[i] - reference[i];
                        quantized = quantized && std::abs(error.x) <= 0.51f * scale.x + 1.0e-7f && std::abs(error.y) <= 0.51f * scale.y + 1.0e-7f &&
                                    std::abs(error.z) <= 0.51f * scale.z + 1.0e-7f;
                        const float width = DecodeHairPointWidth(record, groom.points[record.pointOffset + i]);
                        widths = widths && std::abs(width - referenceWidths[i]) <= record.width * (0.51f / 65535.0f) + 1.0e-12f;
                        builtOnSource = builtOnSource && DistanceToPolyline(decoded[i], sourcePoints) <= Length(scale) + 1.0e-6f;
                    }
                    for (const Float3 point : sourcePoints) {
                        stats.maxSourceDeviation = std::max(stats.maxSourceDeviation, DistanceToPolyline(point, decoded));
                    }
                }
            }
            CHECK(contiguous);
            CHECK(nextPoint == groom.points.size());
            CHECK(levels);
            CHECK(quantized);
            CHECK(widths);
            CHECK(endpoints);
            CHECK(builtOnSource);
            CHECK(stats.maxSourceDeviation <= resampleTolerance);
        }

        // Rank: a permutation, flagged guides first, and every prefix covers the roots.
        std::vector<Float3> rootByRank(strandCount);
        {
            std::vector<std::uint8_t> seen(strandCount, 0u);
            bool permutation = true, flaggedFirst = true;
            const std::uint32_t flaggedCount = static_cast<std::uint32_t>(std::count(source.guideFlags.begin(), source.guideFlags.end(), std::uint8_t{ 1u }));
            for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
                const std::uint32_t rank = groom.strands[strand].rank;
                permutation = permutation && rank < strandCount && seen[rank] == 0u;
                seen[rank] = 1u;
                rootByRank[rank] = groom.rootBindings[strand].restPosition;
                const bool flagged = !source.guideFlags.empty() && source.guideFlags[info.sourceStrandIndices[strand]] != 0u;
                flaggedFirst = flaggedFirst && (rank < flaggedCount) == flagged;
            }
            CHECK(permutation);
            CHECK(flaggedFirst);

            // Flagged guides are placed wherever the author put them, so coverage is only promised without them.
            if (flaggedCount == 0u && rootArea > 0.0f) {
                for (const std::uint32_t prefix : { 16u, 64u, 256u, 1024u, 4096u }) {
                    if (prefix * 2u > strandCount) {
                        break;
                    }
                    float gapSquared = 0.0f;
                    for (std::uint32_t rank = prefix; rank < strandCount; ++rank) {
                        float nearest = 1.0e30f;
                        for (std::uint32_t kept = 0; kept < prefix; ++kept) {
                            const Float3 offset = rootByRank[rank] - rootByRank[kept];
                            nearest = std::min(nearest, Dot(offset, offset));
                        }
                        gapSquared = std::max(gapSquared, nearest);
                    }
                    stats.maxCoverageRatio = std::max(stats.maxCoverageRatio, std::sqrt(gapSquared) / std::sqrt(rootArea / static_cast<float>(prefix)));
                }
                CHECK(stats.maxCoverageRatio <= kPrefixCoverageFactor);
            }
        }

        // Clusters: a partition of the source strands, capped, rank-sorted, with bounds around their strands.
        {
            std::vector<std::uint8_t> seen(strandCount, 0u);
            bool partition = true, capped = true, sorted = true, bounded = true, offsets = true, parents = true;
            for (const std::uint32_t sourceStrand : info.sourceStrandIndices) {
                partition = partition && sourceStrand < strandCount && seen[sourceStrand] == 0u;
                seen[sourceStrand] = 1u;
            }
            for (const HairClusterHeader& cluster : groom.clusters) {
                capped = capped && cluster.strandCount >= 1u && cluster.strandCount <= HAIR_MAX_STRANDS_PER_CLUSTER;
                offsets = offsets && cluster.randomOffset >= 0.0f && cluster.randomOffset < 1.0f;
                parents = parents && cluster.parentIndex == HAIR_INVALID_INDEX;
                const float slack = 1.0e-6f;
                for (std::uint32_t strand = cluster.strandOffset; strand < cluster.strandOffset + cluster.strandCount; ++strand) {
                    sorted = sorted && (strand == cluster.strandOffset || groom.strands[strand - 1u].rank < groom.strands[strand].rank);
                    for (const Float3 point : DecodeStrand(groom, strand)) {
                        const Float3 offset = point - cluster.boundsCenter;
                        bounded = bounded && std::abs(offset.x) <= cluster.boundsExtent.x + slack && std::abs(offset.y) <= cluster.boundsExtent.y + slack &&
                                  std::abs(offset.z) <= cluster.boundsExtent.z + slack && Length(offset) <= cluster.boundsRadius + slack;
                        bounded = bounded && point.x >= groom.boundsMin.x - slack && point.y >= groom.boundsMin.y - slack && point.z >= groom.boundsMin.z - slack &&
                                  point.x <= groom.boundsMax.x + slack && point.y <= groom.boundsMax.y + slack && point.z <= groom.boundsMax.z + slack;
                    }
                }
            }
            CHECK(partition);
            CHECK(capped);
            CHECK(sorted);
            CHECK(bounded);
            CHECK(offsets);
            CHECK(parents);
        }

        // LOD: the drawn count is monotonic in l and reaches N at l = 1. The drawn strands are the first `count`
        // of the rank-sorted cluster, so the set at a lower l is a subset of the set at a higher l, and it is
        // always the lowest-ranked strands of the cluster.
        {
            bool monotonic = true, full = true, lowestRanks = true;
            std::uint32_t previousTotal = 0u;
            for (std::uint32_t step = 1; step <= 100u; ++step) {
                const float l = static_cast<float>(step) / 100.0f;
                std::uint32_t total = 0u;
                for (const HairClusterHeader& cluster : groom.clusters) {
                    const std::uint32_t count = HairLodStrandCount(l, cluster.strandCount, cluster.randomOffset);
                    const std::uint32_t lower = HairLodStrandCount(l - 0.01f, cluster.strandCount, cluster.randomOffset);
                    monotonic = monotonic && lower <= count && count >= 1u && count <= cluster.strandCount;
                    full = full && (step != 100u || count == cluster.strandCount);
                    if (count < cluster.strandCount) {
                        // Sorted by rank (checked above): the last drawn strand outranks every undrawn one.
                        lowestRanks = lowestRanks &&
                                      groom.strands[cluster.strandOffset + count - 1u].rank < groom.strands[cluster.strandOffset + count].rank;
                    }
                    total += count;
                }
                monotonic = monotonic && total >= previousTotal;
                previousTotal = total;
            }
            CHECK(monotonic);
            CHECK(full);
            CHECK(lowestRanks);
            CHECK(previousTotal == strandCount);
        }

        // Guide levels and bindings.
        {
            CHECK(guideCount == std::min(settings.maxGuideCount, strandCount));
            bool sizes = groom.guideLevelOffsets.front() == 0u && groom.guideLevelOffsets.back() == guideCount;
            std::uint64_t expectedEnd = settings.guideLevelBase;
            for (std::uint32_t level = 0; level < groom.GuideLevelCount(); ++level, expectedEnd *= 4u) {
                sizes = sizes && groom.guideLevelOffsets[level + 1u] == std::min<std::uint64_t>(expectedEnd, guideCount);
            }
            CHECK(sizes);

            bool parents = true, weights = true, offsets = true, mapping = true;
            for (std::uint32_t guide = 0; guide < guideCount; ++guide) {
                const HairGuideBinding& binding = groom.guideBindings[guide];
                offsets = offsets && binding.randomOffset >= 0.0f && binding.randomOffset < 1.0f;
                mapping = mapping && groom.strands[groom.guideStrandIndices[guide]].rank == guide;
                if (binding.level == 0u) {
                    parents = parents && guide < groom.guideLevelOffsets[1] && binding.parentIndices[0] == HAIR_INVALID_INDEX &&
                              binding.parentIndices[1] == HAIR_INVALID_INDEX && binding.parentIndices[2] == HAIR_INVALID_INDEX;
                    weights = weights && Sum(binding.parentWeights, 3u) == 0.0f;
                    continue;
                }
                const std::uint32_t below = groom.guideLevelOffsets[binding.level - 1u], own = groom.guideLevelOffsets[binding.level];
                parents = parents && guide >= own && guide < groom.guideLevelOffsets[binding.level + 1u];
                const std::uint32_t expectedParents = std::min(3u, own - below);
                for (std::uint32_t i = 0; i < 3u; ++i) {
                    const std::uint32_t parent = binding.parentIndices[i];
                    if (i < expectedParents) {
                        parents = parents && parent >= below && parent < own && groom.guideBindings[parent].level == binding.level - 1u;
                        weights = weights && binding.parentWeights[i] > 0.0f;
                    } else {
                        parents = parents && parent == HAIR_INVALID_INDEX && binding.parentWeights[i] == 0.0f;
                    }
                }
                weights = weights && std::abs(Sum(binding.parentWeights, 3u) - 1.0f) <= 1.0e-5f;
            }
            CHECK(parents);
            CHECK(weights);
            CHECK(offsets);
            CHECK(mapping);

            bool strandGuides = true, selfBound = true;
            for (const HairStrandRecord& record : groom.strands) {
                if (record.rank < guideCount) {
                    selfBound = selfBound && record.guideIndices[0] == record.rank && record.guideWeights[0] == 1.0f &&
                                record.guideIndices[1] == HAIR_INVALID_INDEX && record.guideIndices[2] == HAIR_INVALID_INDEX &&
                                record.guideWeights[1] == 0.0f && record.guideWeights[2] == 0.0f;
                    continue;
                }
                strandGuides = strandGuides && record.guideIndices[0] < guideCount && std::abs(Sum(record.guideWeights, 3u) - 1.0f) <= 1.0e-5f;
                for (std::uint32_t i = 0; i < 3u; ++i) {
                    strandGuides = strandGuides && (record.guideIndices[i] < guideCount || (record.guideIndices[i] == HAIR_INVALID_INDEX && record.guideWeights[i] == 0.0f));
                }
            }
            CHECK(strandGuides);
            CHECK(selfBound);
        }

        // Root bindings.
        {
            bool weights = true, rest = true, frame = true, rigid = true, nearestTriangle = true, skin = true;
            for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
                const HairRootBinding& binding = groom.rootBindings[strand];
                const std::uint32_t sourceStrand = info.sourceStrandIndices[strand];
                const Float3 root = source.positions[source.StrandPointOffset(sourceStrand)];
                weights = weights && std::abs(Sum(binding.weights, 4u) - 1.0f) <= 1.0e-5f;
                rest = rest && binding.restPosition == root;
                // The quantized first point is the root to within a quantization step.
                rest = rest && Length(DecodeStrand(groom, strand)[0] - root) <= Length(groom.clusters[groom.strands[strand].clusterIndex].quantizationScale) + 1.0e-7f;
                frame = frame && std::abs(Length(binding.restNormal) - 1.0f) <= 1.0e-4f && std::abs(Length(binding.restTangent) - 1.0f) <= 1.0e-4f &&
                        std::abs(Dot(binding.restNormal, binding.restTangent)) <= 1.0e-4f;
                if (scalp == nullptr) {
                    const Float3 firstSegment = source.positions[source.StrandPointOffset(sourceStrand) + 1u] - root;
                    rigid = rigid && binding.joints[0] == 0u && binding.weights[0] == 1.0f && binding.triangleIndex == HAIR_INVALID_INDEX &&
                            (Length(firstSegment) == 0.0f || Dot(binding.restNormal, Normalize(firstSegment)) > 0.9999f);
                    continue;
                }

                // Exhaustive nearest triangle.
                float nearest = 1.0e30f;
                const auto corner = [&](std::uint32_t triangle, std::uint32_t i) { return scalp->positions[scalp->indices[triangle * 3u + i]]; };
                for (std::uint32_t triangle = 0; triangle < scalp->indices.size() / 3u; ++triangle) {
                    float u, v;
                    nearest = std::min(nearest, Length(root - ClosestPointOnTriangle(root, corner(triangle, 0u), corner(triangle, 1u), corner(triangle, 2u), u, v)));
                }
                const std::uint32_t bound = binding.triangleIndex;
                if (bound >= scalp->indices.size() / 3u) {
                    nearestTriangle = false;
                    continue;
                }
                const Float3 a = corner(bound, 0u), b = corner(bound, 1u), c = corner(bound, 2u);
                const Float3 onTriangle = a + (b - a) * binding.barycentricU + (c - a) * binding.barycentricV;
                nearestTriangle = nearestTriangle && Length(root - onTriangle) <= nearest + 1.0e-6f && binding.barycentricU >= 0.0f &&
                                  binding.barycentricV >= 0.0f && binding.barycentricU + binding.barycentricV <= 1.0f + 1.0e-5f;
                frame = frame && Dot(binding.restNormal, Normalize(Cross(b - a, c - a))) > 0.9999f;

                // Interpolated skin: every influence comes from a corner of the bound triangle, largest first.
                for (std::uint32_t slot = 0; slot < 4u; ++slot) {
                    if (binding.weights[slot] == 0.0f) {
                        continue;
                    }
                    bool fromCorner = false;
                    for (std::uint32_t i = 0; i < 3u; ++i) {
                        const auto& joints = scalp->joints[scalp->indices[bound * 3u + i]];
                        fromCorner = fromCorner || std::find(joints.begin(), joints.end(), binding.joints[slot]) != joints.end();
                    }
                    skin = skin && fromCorner && (slot == 0u || binding.weights[slot] <= binding.weights[slot - 1u]);
                }
            }
            CHECK(weights);
            CHECK(rest);
            CHECK(frame);
            CHECK(rigid);
            CHECK(nearestTriangle);
            CHECK(skin);
        }

        // Aggregates.
        {
            bool counts = true, inside = true, orthogonal = true, ordered = true, density = true;
            for (std::size_t clusterIndex = 0; clusterIndex < groom.clusters.size(); ++clusterIndex) {
                const HairClusterHeader& cluster = groom.clusters[clusterIndex];
                const HairClusterAggregate& aggregate = groom.aggregates[clusterIndex];
                counts = counts && aggregate.strandCount == cluster.strandCount;
                density = density && std::isfinite(aggregate.density) && aggregate.density >= 0.0f;
                for (const HairAggregateControlPoint& point : aggregate.controlPoints) {
                    const Float3 offset = point.position - cluster.boundsCenter;
                    inside = inside && std::abs(offset.x) <= cluster.boundsExtent.x + 1.0e-6f && std::abs(offset.y) <= cluster.boundsExtent.y + 1.0e-6f &&
                             std::abs(offset.z) <= cluster.boundsExtent.z + 1.0e-6f;
                    const float major = Length(point.axisMajor), minor = Length(point.axisMinor);
                    orthogonal = orthogonal && std::abs(Dot(point.axisMajor, point.axisMinor)) <= 1.0e-4f * major * minor + 1.0e-12f;
                    ordered = ordered && major >= minor && major <= 2.0f * Length(cluster.boundsExtent) + 1.0e-6f;
                }
            }
            CHECK(counts);
            CHECK(inside);
            CHECK(orthogonal);
            CHECK(ordered);
            CHECK(density);
        }
        return stats;
    }

    const std::filesystem::path& GroomPath()
    {
        static const std::filesystem::path path = std::filesystem::temp_directory_path() / "br_hair_core_tests.groom";
        return path;
    }

    std::vector<char> ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        std::vector<char> bytes(file ? static_cast<std::size_t>(file.tellg()) : 0u);
        file.seekg(0);
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return bytes;
    }

    void WriteBytes(const std::filesystem::path& path, const std::vector<char>& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    template <typename T>
    bool SameBytes(const std::vector<T>& a, const std::vector<T>& b)
    {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
    }

    bool SameArrays(const BuiltGroom& a, const BuiltGroom& b)
    {
        return SameBytes(a.clusters, b.clusters) && SameBytes(a.aggregates, b.aggregates) && SameBytes(a.strands, b.strands) &&
               SameBytes(a.rootBindings, b.rootBindings) && SameBytes(a.guideBindings, b.guideBindings) &&
               SameBytes(a.guideStrandIndices, b.guideStrandIndices) && SameBytes(a.guideLevelOffsets, b.guideLevelOffsets) &&
               SameBytes(a.points, b.points) && a.boundsMin == b.boundsMin && a.boundsMax == b.boundsMax;
    }

    void CheckRoundTrip(const BuiltGroom& groom)
    {
        std::string error;
        CHECK(SaveGroomFile(GroomPath(), groom, &error));
        BuiltGroom loaded;
        CHECK(LoadGroomFile(GroomPath(), loaded, &error));
        CHECK(error.empty());
        CHECK(SameArrays(groom, loaded));

        // Saving what was loaded reproduces the file byte for byte.
        const std::vector<char> first = ReadBytes(GroomPath());
        CHECK(SaveGroomFile(GroomPath(), loaded));
        CHECK(first == ReadBytes(GroomPath()));
        CHECK(first.size() == sizeof(GroomFileHeader) + groom.clusters.size() * sizeof(HairClusterHeader) +
                                  groom.aggregates.size() * sizeof(HairClusterAggregate) + groom.strands.size() * sizeof(HairStrandRecord) +
                                  groom.rootBindings.size() * sizeof(HairRootBinding) + groom.guideBindings.size() * sizeof(HairGuideBinding) +
                                  (groom.guideStrandIndices.size() + groom.guideLevelOffsets.size()) * sizeof(std::uint32_t) +
                                  groom.points.size() * sizeof(HairPackedPoint));
    }

    void ExpectRejected(const std::vector<char>& bytes, const char* expectedInError, int line)
    {
        WriteBytes(GroomPath(), bytes);
        BuiltGroom groom;
        groom.points.push_back({}); // must be cleared on failure
        std::string error;
        const bool loaded = LoadGroomFile(GroomPath(), groom, &error);
        if (loaded || error.find(expectedInError) == std::string::npos || !groom.points.empty() || !groom.clusters.empty()) {
            std::cerr << __FILE__ << "(" << line << "): expected rejection mentioning \"" << expectedInError << "\", got "
                      << (loaded ? "a successful load" : "\"" + error + "\"") << "\n";
            ++g_failures;
        }
    }

    void TestGroomFileRejections(const BuiltGroom& groom)
    {
        g_label = "groom file rejections";
        CHECK(SaveGroomFile(GroomPath(), groom));
        const std::vector<char> good = ReadBytes(GroomPath());
        GroomFileHeader header;
        std::memcpy(&header, good.data(), sizeof(header));
        const auto withHeader = [&good](const GroomFileHeader& changed) {
            std::vector<char> bytes = good;
            std::memcpy(bytes.data(), &changed, sizeof(changed));
            return bytes;
        };
        const auto arrayDesc = [](GroomFileHeader& h, GroomFileArray array) -> GroomFileArrayDesc& { return h.arrays[static_cast<std::uint32_t>(array)]; };
        // Overwrites one value inside an array of the file.
        const auto poke = [&](GroomFileArray array, std::size_t element, std::size_t fieldOffset, std::uint32_t value) {
            std::vector<char> bytes = good;
            const GroomFileArrayDesc& desc = header.arrays[static_cast<std::uint32_t>(array)];
            std::memcpy(bytes.data() + desc.byteOffset + element * desc.stride + fieldOffset, &value, sizeof(value));
            return bytes;
        };

        {
            BuiltGroom missing;
            std::string error;
            CHECK(!LoadGroomFile(std::filesystem::temp_directory_path() / "br_hair_core_tests_missing.groom", missing, &error));
            CHECK(error.find("cannot open") != std::string::npos);
        }
        ExpectRejected({}, "header", __LINE__);
        ExpectRejected(std::vector<char>(good.begin(), good.begin() + sizeof(GroomFileHeader) - 1u), "header", __LINE__);
        ExpectRejected(std::vector<char>(good.begin(), good.end() - 1), "truncated", __LINE__);
        ExpectRejected(std::vector<char>(good.begin(), good.begin() + good.size() / 2u), "truncated", __LINE__);
        {
            std::vector<char> padded = good;
            padded.push_back(0);
            ExpectRejected(padded, "padded", __LINE__);
        }
        {
            GroomFileHeader changed = header;
            changed.magic[0] = 'X';
            ExpectRejected(withHeader(changed), "magic", __LINE__);
            changed = header;
            changed.schemaVersion = kGroomSchemaVersion + 1u;
            ExpectRejected(withHeader(changed), "schema version", __LINE__);
            changed = header;
            changed.headerBytes += 16u;
            ExpectRejected(withHeader(changed), "header size", __LINE__);
            changed = header;
            arrayDesc(changed, GroomFileArray::Strands).stride = 40u;
            ExpectRejected(withHeader(changed), "element size in the strand array", __LINE__);
            changed = header;
            arrayDesc(changed, GroomFileArray::Points).count += 1u;
            ExpectRejected(withHeader(changed), "point array lies outside", __LINE__);
            changed = header;
            arrayDesc(changed, GroomFileArray::Points).count = 0xFFFFFFFFu;
            ExpectRejected(withHeader(changed), "point array lies outside", __LINE__);
            changed = header;
            arrayDesc(changed, GroomFileArray::Clusters).byteOffset = 0xFFFFFFFFFFFFFFF0ull;
            ExpectRejected(withHeader(changed), "cluster array lies outside", __LINE__);
            changed = header;
            arrayDesc(changed, GroomFileArray::Clusters).byteOffset = 8u; // inside the header
            ExpectRejected(withHeader(changed), "cluster array lies outside", __LINE__);
            // In-bounds arrays whose sizes disagree with each other.
            changed = header;
            arrayDesc(changed, GroomFileArray::RootBindings).count -= 1u;
            ExpectRejected(withHeader(changed), "root binding count", __LINE__);
            changed = header;
            arrayDesc(changed, GroomFileArray::Points).count -= 1u;
            ExpectRejected(withHeader(changed), "runs past the point array", __LINE__);
        }
        // Indices inside the arrays that point out of range.
        ExpectRejected(poke(GroomFileArray::Clusters, 0u, offsetof(HairClusterHeader, strandCount), HAIR_MAX_STRANDS_PER_CLUSTER + 1u),
                       "invalid strand count", __LINE__);
        ExpectRejected(poke(GroomFileArray::Clusters, 1u, offsetof(HairClusterHeader, strandOffset), 0u), "does not continue", __LINE__);
        ExpectRejected(poke(GroomFileArray::Clusters, 0u, offsetof(HairClusterHeader, vertexLevel), 31u), "vertex level", __LINE__);
        ExpectRejected(poke(GroomFileArray::Strands, 5u, offsetof(HairStrandRecord, pointOffset), 0xFFFFFFF0u), "runs past the point array", __LINE__);
        ExpectRejected(poke(GroomFileArray::Strands, 5u, offsetof(HairStrandRecord, guideIndices), groom.GuideCount()), "outside the guide-capable set",
                       __LINE__);
        ExpectRejected(poke(GroomFileArray::GuideBindings, groom.GuideCount() - 1u, offsetof(HairGuideBinding, parentIndices), groom.GuideCount() - 1u),
                       "parent outside the level below", __LINE__);
        ExpectRejected(poke(GroomFileArray::GuideStrandIndices, 0u, 0u, static_cast<std::uint32_t>(groom.strands.size())), "does not map", __LINE__);
        ExpectRejected(poke(GroomFileArray::GuideLevelOffsets, 1u, 0u, 0u), "empty or out of range", __LINE__);

        std::error_code ignored;
        std::filesystem::remove(GroomPath(), ignored);
    }

    GroomBuildSettings SmallTestSettings()
    {
        GroomBuildSettings settings;
        settings.maxGuideCount = 1024u;
        settings.guideLevelBase = 16u; // levels end at 16, 64, 256, 1024
        return settings;
    }

    void TestBuildAllStyles()
    {
        const ScalpMesh dome = MakeDomeScalp();
        const float domeArea = MeshArea(dome);
        BuiltGroom keptForFileTests;
        for (const bool withScalp : { false, true }) {
            for (const ProceduralHairStyle style : { ProceduralHairStyle::Straight, ProceduralHairStyle::Wavy, ProceduralHairStyle::Curly }) {
                const char* styleName = style == ProceduralHairStyle::Straight ? "straight" : style == ProceduralHairStyle::Wavy ? "wavy" : "curly";
                g_label = std::string(styleName) + (withScalp ? " on scalp mesh" : " on sphere cap");

                ProceduralGroomSettings generator;
                generator.strandCount = 3000u;
                generator.pointsPerStrand = 33u;
                generator.style = style;
                generator.seed = 11u;
                generator.capAngleRadians = 1.2f;
                const GroomSource source = withScalp ? GenerateGroomOnTriangles(generator, dome.positions, dome.indices) : GenerateGroomOnSphereCap(generator);
                const float area = withScalp ? domeArea : 2.0f * kPi * generator.capRadius * generator.capRadius * (1.0f - std::cos(generator.capAngleRadians));

                const GroomBuildSettings settings = SmallTestSettings();
                BuiltGroom groom;
                GroomBuildInfo info;
                std::string error;
                CHECK(BuildGroom(source, withScalp ? &dome : nullptr, settings, groom, &error, &info));
                CHECK(error.empty());
                const bool straight = style == ProceduralHairStyle::Straight;
                const BuildStats stats = CheckBuiltGroom(source, withScalp ? &dome : nullptr, settings, groom, info, area,
                                                         straight ? kResampleToleranceStraight : kResampleToleranceCurved);
                CHECK(groom.GuideLevelCount() == 4u);

                // Curved strands keep more points than straight ones.
                std::uint32_t minLevel = 99u, maxLevel = 0u;
                for (const HairClusterHeader& cluster : groom.clusters) {
                    minLevel = std::min(minLevel, cluster.vertexLevel);
                    maxLevel = std::max(maxLevel, cluster.vertexLevel);
                }
                CHECK(straight ? maxLevel == settings.minVertexLevel : minLevel > settings.minVertexLevel);

                // On the scalp mesh the roots were generated on the triangles, so the bound point is the root itself.
                if (withScalp) {
                    bool onSurface = true;
                    for (const HairRootBinding& binding : groom.rootBindings) {
                        const Float3 a = dome.positions[dome.indices[binding.triangleIndex * 3u]];
                        const Float3 b = dome.positions[dome.indices[binding.triangleIndex * 3u + 1u]];
                        const Float3 c = dome.positions[dome.indices[binding.triangleIndex * 3u + 2u]];
                        onSurface = onSurface && Length(a + (b - a) * binding.barycentricU + (c - a) * binding.barycentricV - binding.restPosition) <= 1.0e-6f;
                    }
                    CHECK(onSurface);
                }

                // Deterministic.
                BuiltGroom again;
                CHECK(BuildGroom(source, withScalp ? &dome : nullptr, settings, again));
                CHECK(SameArrays(groom, again));

                CheckRoundTrip(groom);
                std::cout << "  build " << g_label << ": " << groom.clusters.size() << " clusters, vertex levels " << minLevel << ".." << maxLevel
                          << ", max source deviation " << stats.maxSourceDeviation * 1000.0f << " mm, max prefix gap " << stats.maxCoverageRatio
                          << " x sqrt(area/m)\n";
                if (withScalp && style == ProceduralHairStyle::Curly) {
                    keptForFileTests = std::move(groom);
                }
            }
        }
        TestGroomFileRejections(keptForFileTests);
    }

    // Authored guide flags and closest guides.
    void TestAuthoredGuides()
    {
        g_label = "authored guides";
        ProceduralGroomSettings generator;
        generator.strandCount = 2000u;
        generator.pointsPerStrand = 9u;
        GroomSource source = GenerateGroomOnSphereCap(generator);

        // Every 7th strand is a guide (286 of them); every strand names three of them with weights 2:1:1, except
        // that every 5th strand names only two.
        const std::uint32_t strandCount = source.StrandCount();
        source.guideFlags.assign(strandCount, 0u);
        std::vector<std::int32_t> flagged;
        for (std::uint32_t strand = 0; strand < strandCount; strand += 7u) {
            source.guideFlags[strand] = 1u;
            flagged.push_back(static_cast<std::int32_t>(strand));
        }
        source.closestGuides.resize(strandCount);
        source.closestGuideWeights.assign(strandCount, { 2.0f, 1.0f, 1.0f });
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            source.closestGuides[strand] = { flagged[strand % flagged.size()], flagged[(strand * 3u + 1u) % flagged.size()],
                                             strand % 5u == 0u ? -1 : flagged[(strand * 5u + 2u) % flagged.size()] };
        }

        // 256 guide-capable strands: fewer than the flagged ones, so some authored guides are not guide-capable.
        GroomBuildSettings settings;
        settings.maxGuideCount = 256u;
        settings.guideLevelBase = 64u;
        BuiltGroom groom;
        GroomBuildInfo info;
        CHECK(BuildGroom(source, nullptr, settings, groom, nullptr, &info));
        CheckBuiltGroom(source, nullptr, settings, groom, info, 0.0f, kResampleToleranceStraight);
        if (groom.strands.size() != strandCount) {
            return;
        }

        std::vector<std::uint32_t> rankOfSource(strandCount);
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            rankOfSource[info.sourceStrandIndices[strand]] = groom.strands[strand].rank;
        }
        bool authoredKept = true;
        std::uint32_t authoredCount = 0u, derivedCount = 0u;
        for (std::uint32_t strand = 0; strand < strandCount; ++strand) {
            const HairStrandRecord& record = groom.strands[strand];
            if (record.rank < groom.GuideCount()) {
                continue;
            }
            const auto& guides = source.closestGuides[info.sourceStrandIndices[strand]];
            bool allCapable = true;
            for (const std::int32_t guide : guides) {
                allCapable = allCapable && (guide < 0 || rankOfSource[guide] < groom.GuideCount());
            }
            if (!allCapable) {
                ++derivedCount;
                continue;
            }
            ++authoredCount;
            const bool two = guides[2] < 0;
            authoredKept = authoredKept && record.guideIndices[0] == rankOfSource[guides[0]] && record.guideIndices[1] == rankOfSource[guides[1]] &&
                           record.guideIndices[2] == (two ? HAIR_INVALID_INDEX : rankOfSource[guides[2]]) &&
                           std::abs(record.guideWeights[0] - (two ? 2.0f / 3.0f : 0.5f)) <= 1.0e-6f &&
                           std::abs(record.guideWeights[1] - (two ? 1.0f / 3.0f : 0.25f)) <= 1.0e-6f &&
                           std::abs(record.guideWeights[2] - (two ? 0.0f : 0.25f)) <= 1.0e-6f;
        }
        CHECK(authoredKept);
        CHECK(authoredCount > 100u && derivedCount > 100u); // both paths ran
    }

    // With enough source points the error is set by the vertex level the builder picks, not by the source spacing.
    void TestResampleConverges()
    {
        for (const ProceduralHairStyle style : { ProceduralHairStyle::Wavy, ProceduralHairStyle::Curly }) {
            g_label = style == ProceduralHairStyle::Wavy ? "dense wavy" : "dense curly";
            ProceduralGroomSettings generator;
            generator.strandCount = 500u;
            generator.pointsPerStrand = 257u;
            generator.style = style;
            const GroomSource source = GenerateGroomOnSphereCap(generator);
            GroomBuildSettings settings = SmallTestSettings();
            settings.maxVertexLevel = 7u;
            BuiltGroom groom;
            GroomBuildInfo info;
            CHECK(BuildGroom(source, nullptr, settings, groom, nullptr, &info));
            const BuildStats stats = CheckBuiltGroom(source, nullptr, settings, groom, info, 0.0f, kResampleToleranceDense);
            std::cout << "  build " << g_label << " (257 source points, vertex level " << groom.clusters[0].vertexLevel << "): max source deviation "
                      << stats.maxSourceDeviation * 1000.0f << " mm\n";
        }
    }

    void TestLodHelper()
    {
        g_label = "lod helper";
        CHECK(HairLodStrandCount(0.5f, 0u, 0.3f) == 0u);
        for (const std::uint32_t strandCount : { 1u, 2u, 7u, 33u, 64u }) {
            for (const float offset : { 0.0f, 0.37f, 0.999f }) {
                bool monotonic = true, inRange = true;
                std::uint32_t previous = 0u;
                for (std::uint32_t step = 0; step <= 1000u; ++step) {
                    const std::uint32_t count = HairLodStrandCount(static_cast<float>(step) / 1000.0f, strandCount, offset);
                    monotonic = monotonic && count >= previous;
                    inRange = inRange && count >= 1u && count <= strandCount;
                    previous = count;
                }
                CHECK(monotonic);
                CHECK(inRange);
                CHECK(previous == strandCount);
                CHECK(HairLodStrandCount(0.0f, strandCount, offset) == 1u);
                CHECK(HairLodStrandCount(2.0f, strandCount, offset) == strandCount);
            }
        }
        // The plan's rule, spelled out: ceil(0.5 * (10 + 0.4)) = 6.
        CHECK(HairLodStrandCount(0.5f, 10u, 0.4f) == 6u);
        CHECK(HairLodStrandCount(0.5f, 10u, 0.0f) == 5u);
    }

    // HairLodShared.h is the source the select shader compiles. HairLodStrandCount must be that rule, and the rule must
    // be the plan's: checked against the formula written out here.
    void TestSharedLodRules()
    {
        g_label = "shared lod rules";
        for (const std::uint32_t strandCount : { 1u, 2u, 7u, 33u, 64u }) {
            for (const float offset : { 0.0f, 0.37f, 0.999f }) {
                bool sameAsHelper = true, sameAsFormula = true, weightInRange = true, continuous = true, monotonic = true;
                float previousDrawn = 0.0f;
                for (std::uint32_t step = 0; step <= 4000u; ++step) {
                    const float detail = static_cast<float>(step) / 4000.0f;
                    const std::uint32_t count = HairLodStrandCountShared(detail, strandCount, offset);
                    sameAsHelper = sameAsHelper && count == HairLodStrandCount(detail, strandCount, offset);
                    const float formula = std::ceil(detail * (static_cast<float>(strandCount) + offset));
                    const std::uint32_t expected =
                        formula > 1.0f ? (std::min)(static_cast<std::uint32_t>(formula), strandCount) : 1u;
                    sameAsFormula = sameAsFormula && count == expected;
                    // The boundary strand's weight makes the drawn amount continuous: whole strands plus a fraction.
                    const float weight = HairLodBoundaryWeight(detail, strandCount, offset);
                    weightInRange = weightInRange && weight > 0.0f && weight <= 1.0f;
                    const float drawn = static_cast<float>(count - 1u) + weight;
                    monotonic = monotonic && drawn >= previousDrawn - 1.0e-4f;
                    continuous = continuous && (step == 0u || drawn - previousDrawn <= (static_cast<float>(strandCount) + 1.0f) / 4000.0f + 1.0e-3f);
                    previousDrawn = drawn;
                }
                CHECK(sameAsHelper);
                CHECK(sameAsFormula);
                CHECK(weightInRange);
                CHECK(monotonic);
                CHECK(continuous);
                CHECK(HairLodBoundaryWeight(1.0f, strandCount, offset) == 1.0f);
            }
        }
        CHECK(std::fabs(HairLodBoundaryWeight(0.5f, 10u, 0.4f) - 0.2f) < 1.0e-5f); // 5.2 strands: five whole, one at 0.2

        CHECK(HairLodDetailLevel(0.5f, 1.0f, 0.02f) == 0.5f);
        CHECK(HairLodDetailLevel(3.0f, 1.0f, 0.02f) == 1.0f);
        CHECK(HairLodDetailLevel(0.001f, 1.0f, 0.02f) == 0.02f);
        CHECK(HairLodDetailLevel(1.0f, 2.0f, 0.02f) == 0.5f);
        // The power form: exponent 1 is the rule above exactly; a higher one draws less, down to the floor, and still
        // every strand from the same spacing up.
        for (const float spacing : { 0.001f, 0.1f, 0.5f, 0.99f, 1.0f, 3.0f }) {
            CHECK(HairLodDetailLevelPower(spacing, 1.0f, 0.02f, 1.0f) == HairLodDetailLevel(spacing, 1.0f, 0.02f));
            CHECK(HairLodDetailLevelPower(spacing, 1.0f, 0.02f, 2.0f) <= HairLodDetailLevel(spacing, 1.0f, 0.02f));
            CHECK(HairLodDetailLevelPower(spacing, 1.0f, 0.02f, 2.0f) >= 0.02f);
        }
        CHECK(HairLodDetailLevelPower(0.5f, 1.0f, 0.02f, 2.0f) == 0.25f);
        CHECK(HairLodDetailLevelPower(1.0f, 1.0f, 0.02f, 2.0f) == 1.0f);
        CHECK(HairLodWidthScale(0.25f, 8.0f) == 4.0f);
        CHECK(HairLodWidthScale(0.01f, 8.0f) == 8.0f);
        CHECK(HairLodWidthScale(1.0f, 8.0f) == 1.0f);
        // The exact scale: drawn strands (whole ones plus the boundary strand's weight) times it is the cluster's strand
        // count, down to the one strand a cluster always draws, until the cap.
        for (const std::uint32_t strandCount : { 1u, 7u, 40u, 64u }) {
            for (const float offset : { 0.0f, 0.37f, 0.99f }) {
                for (float l = 0.001f; l <= 1.0f; l *= 1.3f) {
                    const std::uint32_t count = HairLodStrandCountShared(l, strandCount, offset);
                    const float drawn = static_cast<float>(count - 1u) + HairLodBoundaryWeight(l, strandCount, offset);
                    const float scale = HairLodCoverageWidthScale(l, strandCount, offset, 1000.0f);
                    CHECK(std::fabs(drawn * scale - static_cast<float>(strandCount)) < 1.0e-3f * static_cast<float>(strandCount));
                    CHECK(HairLodCoverageWidthScale(l, strandCount, offset, 4.0f) <= 4.0f);
                }
                CHECK(HairLodCoverageWidthScale(1.0f, strandCount, offset, 64.0f) == 1.0f);
            }
        }

        // Vertex level: 2^f segments of the target length, never above the stored level; the drawn level less the
        // geomorph is f, so the shape is continuous where the drawn level steps.
        CHECK(HairLodVertexLevel(64.0f, 4.0f, 5u) == 4.0f);
        CHECK(HairLodVertexLevel(6400.0f, 4.0f, 5u) == 5.0f);
        CHECK(HairLodVertexLevel(2.0f, 4.0f, 5u) == 0.0f);
        bool levelMonotonic = true, levelConsistent = true;
        float previousLevel = 0.0f;
        for (std::uint32_t step = 1; step <= 2000u; ++step) {
            const float level = HairLodVertexLevel(static_cast<float>(step), 4.0f, 5u);
            const std::uint32_t drawn = HairLodDrawnVertexLevel(level);
            const float geomorph = HairLodGeomorph(level);
            levelMonotonic = levelMonotonic && level >= previousLevel;
            levelConsistent = levelConsistent && drawn <= 5u && geomorph >= 0.0f && geomorph < 1.0f &&
                std::fabs(static_cast<float>(drawn) - geomorph - level) < 1.0e-5f;
            previousLevel = level;
        }
        CHECK(levelMonotonic);
        CHECK(levelConsistent);
    }

    void TestSmallAndInvalidInputs()
    {
        g_label = "small and invalid inputs";
        const GroomBuildSettings settings;
        BuiltGroom groom;
        GroomBuildInfo info;
        std::string error;

        // Fewer strands than one guide level or one cluster; two-point strands.
        for (const std::uint32_t strandCount : { 1u, 2u, 10u, 100u }) {
            ProceduralGroomSettings generator;
            generator.strandCount = strandCount;
            generator.pointsPerStrand = 2u;
            const GroomSource source = GenerateGroomOnSphereCap(generator);
            CHECK(BuildGroom(source, nullptr, settings, groom, &error, &info));
            CheckBuiltGroom(source, nullptr, settings, groom, info, 0.0f, kResampleToleranceStraight);
            CHECK(groom.GuideCount() == strandCount);
            CHECK(groom.GuideLevelCount() == (strandCount <= 64u ? 1u : 2u));
            CheckRoundTrip(groom);
        }

        // Coincident roots and a zero-length strand must not break the grid or the frames.
        {
            GroomSource source;
            source.strandPointOffsets = { 0u, 2u, 4u, 6u };
            source.positions = { { 0, 0, 0 }, { 0, 0.1f, 0 }, { 0, 0, 0 }, { 0.1f, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
            source.widths.assign(6u, 0.0001f);
            CHECK(BuildGroom(source, nullptr, settings, groom, &error, &info));
            CheckBuiltGroom(source, nullptr, settings, groom, info, 0.0f, kResampleToleranceStraight);
        }

        ProceduralGroomSettings generator;
        generator.strandCount = 20u;
        const GroomSource source = GenerateGroomOnSphereCap(generator);
        const auto expectFailure = [&](const GroomSource& badSource, const ScalpMesh* scalp, const GroomBuildSettings& badSettings, const char* expected, int line) {
            BuiltGroom failed;
            failed.points.push_back({});
            std::string message;
            if (BuildGroom(badSource, scalp, badSettings, failed, &message) || message.find(expected) == std::string::npos || !failed.points.empty()) {
                std::cerr << __FILE__ << "(" << line << "): expected a build failure mentioning \"" << expected << "\", got \"" << message << "\"\n";
                ++g_failures;
            }
        };
        expectFailure(GroomSource{}, nullptr, settings, "no strands", __LINE__);
        {
            GroomSource bad = source;
            bad.widths.pop_back();
            expectFailure(bad, nullptr, settings, "width count", __LINE__);
            bad = source;
            bad.positions[3].y = std::numeric_limits<float>::infinity();
            expectFailure(bad, nullptr, settings, "non-finite", __LINE__);
        }
        {
            GroomBuildSettings bad = settings;
            bad.maxVertexLevel = HAIR_MAX_VERTEX_LEVEL + 1u;
            expectFailure(source, nullptr, bad, "vertex levels", __LINE__);
            bad = settings;
            bad.guideLevelBase = 0u;
            expectFailure(source, nullptr, bad, "at least 1", __LINE__);
        }
        {
            ScalpMesh bad = MakeDomeScalp();
            bad.indices.pop_back();
            expectFailure(source, &bad, settings, "triangle list", __LINE__);
            bad = MakeDomeScalp();
            bad.indices[4] = static_cast<std::uint32_t>(bad.positions.size());
            expectFailure(source, &bad, settings, "out of range", __LINE__);
            bad = MakeDomeScalp();
            bad.weights.pop_back();
            expectFailure(source, &bad, settings, "joints and weights", __LINE__);
        }
    }

    // The numbers a separate tool would print, for a head-sized groom with the default settings.
    void PrintLargeGroomSummary()
    {
        g_label = "50k summary";
        ProceduralGroomSettings generator;
        generator.strandCount = 50000u;
        generator.pointsPerStrand = 33u;
        generator.style = ProceduralHairStyle::Curly;
        const GroomSource source = GenerateGroomOnSphereCap(generator);
        const ScalpMesh dome = MakeDomeScalp();

        const GroomBuildSettings settings;
        BuiltGroom groom;
        GroomBuildInfo info;
        const auto start = std::chrono::steady_clock::now();
        CHECK(BuildGroom(source, &dome, settings, groom, nullptr, &info));
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        CHECK(ValidateBuiltGroom(groom).empty());
        CheckRoundTrip(groom);

        const auto bytes = [](const auto& values) { return values.size() * sizeof(values[0]); };
        std::cout << "  50k curly groom (33 source points per strand, " << dome.indices.size() / 3u << "-triangle scalp, default settings):\n"
                  << "    strands " << groom.strands.size() << ", points " << groom.points.size() << ", clusters " << groom.clusters.size() << "\n"
                  << "    guide-capable " << groom.GuideCount() << " in " << groom.GuideLevelCount() << " levels:";
        for (std::uint32_t level = 0; level < groom.GuideLevelCount(); ++level) {
            std::cout << " " << groom.guideLevelOffsets[level + 1u] - groom.guideLevelOffsets[level];
        }
        std::cout << "\n    bytes: clusters " << bytes(groom.clusters) << ", aggregates " << bytes(groom.aggregates) << ", strands " << bytes(groom.strands)
                  << ", root bindings " << bytes(groom.rootBindings) << ", guide bindings " << bytes(groom.guideBindings) << ", guide strand indices "
                  << bytes(groom.guideStrandIndices) << ", points " << bytes(groom.points) << "\n"
                  << "    .groom file " << std::filesystem::file_size(GroomPath()) << " bytes, build " << buildMs << " ms\n";

        // Strands drawn per detail level: what the LOD rule does to this groom.
        std::cout << "    strands drawn at l = 1, 0.5, 0.25, 0.1, 0.02:";
        for (const float l : { 1.0f, 0.5f, 0.25f, 0.1f, 0.02f }) {
            std::size_t drawn = 0u;
            for (const HairClusterHeader& cluster : groom.clusters) {
                drawn += HairLodStrandCount(l, cluster.strandCount, cluster.randomOffset);
            }
            std::cout << " " << drawn;
        }
        std::cout << "\n";

        std::error_code ignored;
        std::filesystem::remove(GroomPath(), ignored);
    }

} // namespace

int RunGroomBuilderTests()
{
    TestLodHelper();
    TestSharedLodRules();
    TestBuildAllStyles();
    TestResampleConverges();
    TestAuthoredGuides();
    TestSmallAndInvalidInputs();
    PrintLargeGroomSummary();
    return g_failures;
}
