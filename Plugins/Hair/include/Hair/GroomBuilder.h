#pragma once

#include "Hair/GroomSource.h"
#include "Hair/HairGPUTypes.h"
#include "Hair/HairMath.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace br::hair {

// The skinned surface the roots attach to. Triangle list, counter-clockwise front faces, same space and units
// (metres) as the groom.
struct ScalpMesh {
    std::vector<Float3> positions;
    std::vector<std::uint32_t> indices;
    std::vector<std::array<std::uint32_t, 4>> joints; // per vertex
    std::vector<std::array<float, 4>> weights;        // per vertex
};

struct GroomBuildSettings {
    // Strands are resampled to 2^k + 1 points. k is chosen per cluster from how much its strands turn (one segment
    // per maxSegmentAngleRadians, never more segments than the source strand has) and clamped to [min, max].
    std::uint32_t minVertexLevel = 3u;
    std::uint32_t maxVertexLevel = 5u; // at most HAIR_MAX_VERTEX_LEVEL
    float maxSegmentAngleRadians = 0.35f;

    // Guide-capable set: the first maxGuideCount ranks (clamped to the strand count). Level j ends at rank
    // guideLevelBase * 4^j, so the levels hold base, 3 * base, 12 * base, ... guides.
    std::uint32_t maxGuideCount = 4096u;
    std::uint32_t guideLevelBase = 64u;

    // Clusters group strands by root position and by tip offset (tip - root) scaled by this weight.
    float clusterShapeWeight = 0.5f;

    std::uint32_t seed = 1u; // per-cluster and per-guide random offsets
};

// A built groom: exactly the arrays the GPU consumes, plus the guide level table. Everything is in groom rest space,
// in metres.
struct BuiltGroom {
    Float3 boundsMin;
    Float3 boundsMax;

    std::vector<HairClusterHeader> clusters;
    std::vector<HairClusterAggregate> aggregates; // one per cluster
    std::vector<HairStrandRecord> strands;        // cluster by cluster, sorted by rank inside each cluster
    std::vector<HairRootBinding> rootBindings;    // one per strand
    std::vector<HairGuideBinding> guideBindings;  // one per guide-capable strand, indexed by rank
    std::vector<std::uint32_t> guideStrandIndices; // guide (rank) -> index into `strands`
    std::vector<std::uint32_t> guideLevelOffsets;  // level j owns guides [offsets[j], offsets[j + 1]); level count + 1 entries
    std::vector<HairPackedPoint> points;

    std::uint32_t GuideCount() const { return static_cast<std::uint32_t>(guideBindings.size()); }
    std::uint32_t GuideLevelCount() const
    {
        return guideLevelOffsets.empty() ? 0u : static_cast<std::uint32_t>(guideLevelOffsets.size() - 1u);
    }
};

// Build by-products that are not part of the groom.
struct GroomBuildInfo {
    std::vector<std::uint32_t> sourceStrandIndices; // built strand -> strand of the GroomSource
};

// Builds `groom` from `source`. With a scalp mesh each root takes the skin weights of its nearest scalp triangle;
// without one (nullptr) roots bind rigidly to joint 0. Authored closest guides are used for a strand when all of
// them landed in the guide-capable set; otherwise its guides are the nearest roots. Returns false and sets `error`
// (when given) for an inconsistent or empty source, a malformed scalp mesh, or settings out of range; `groom` is
// left empty in that case.
bool BuildGroom(const GroomSource& source,
                const ScalpMesh* scalp,
                const GroomBuildSettings& settings,
                BuiltGroom& groom,
                std::string* error = nullptr,
                GroomBuildInfo* info = nullptr);

// Checks array sizes and that every index in the groom is in range (clusters partition the strands, strands fit the
// point array, guides reference the level below). Returns an empty string when consistent, otherwise the first
// problem. The .groom loader runs this on everything it reads.
std::string ValidateBuiltGroom(const BuiltGroom& groom);

// Resamples one source strand to 2^vertexLevel + 1 points evenly spaced by arc length. The first and last points are
// the source's root and tip; widths are interpolated the same way.
void ResampleStrand(const GroomSource& source,
                    std::uint32_t strand,
                    std::uint32_t vertexLevel,
                    std::vector<Float3>& positions,
                    std::vector<float>& widths);

inline std::uint32_t HairStrandPointCount(std::uint32_t vertexLevel) { return (1u << vertexLevel) + 1u; }

inline Float3 DecodeHairPointPosition(const HairClusterHeader& cluster, HairPackedPoint point)
{
    return { cluster.quantizationOrigin.x + cluster.quantizationScale.x * static_cast<float>(point.xy & 0xFFFFu),
             cluster.quantizationOrigin.y + cluster.quantizationScale.y * static_cast<float>(point.xy >> 16u),
             cluster.quantizationOrigin.z + cluster.quantizationScale.z * static_cast<float>(point.zw & 0xFFFFu) };
}

inline float DecodeHairPointWidth(const HairStrandRecord& strand, HairPackedPoint point)
{
    return strand.width * (static_cast<float>(point.zw >> 16u) * (1.0f / 65535.0f));
}

} // namespace br::hair
