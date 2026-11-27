#pragma once

#include "Hair/HairMath.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace br::hair {

// Importer-neutral groom: what every importer (procedural, .hair, Alembic) produces and the builder consumes.
// Strands are polylines ordered root to tip. Units and space are the source's; nothing here is resampled or sorted.
struct GroomSource {
    // Strand s owns points [strandPointOffsets[s], strandPointOffsets[s + 1]). Size is strand count + 1 (or empty).
    std::vector<std::uint32_t> strandPointOffsets;

    // Per point.
    std::vector<Float3> positions;
    std::vector<float> widths; // strand diameter at the point

    // Optional per point: empty when the source has none.
    std::vector<Float3> colors;

    // Optional per strand: each is empty when the source has none, otherwise one entry per strand.
    std::vector<std::uint8_t> guideFlags;                      // 1 = authored guide (groom_guide)
    std::vector<std::int32_t> groupIds;                        // groom_group_id
    std::vector<Float2> rootUVs;                               // groom_root_uv
    std::vector<std::array<std::int32_t, 3>> closestGuides;    // groom_closest_guides: strand indices, -1 = none
    std::vector<std::array<float, 3>> closestGuideWeights;     // groom_guide_weights

    std::uint32_t StrandCount() const
    {
        return strandPointOffsets.empty() ? 0u : static_cast<std::uint32_t>(strandPointOffsets.size() - 1u);
    }
    std::uint32_t PointCount() const { return static_cast<std::uint32_t>(positions.size()); }
    std::uint32_t StrandPointOffset(std::uint32_t strand) const { return strandPointOffsets[strand]; }
    std::uint32_t StrandPointCount(std::uint32_t strand) const
    {
        return strandPointOffsets[strand + 1u] - strandPointOffsets[strand];
    }
};

// Checks array sizes, offsets and that every strand has at least two points. Returns an empty string when the groom
// is consistent, otherwise a description of the first problem.
std::string ValidateGroomSource(const GroomSource& groom);

} // namespace br::hair
