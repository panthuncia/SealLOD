#include "Hair/GroomSource.h"

namespace br::hair {

std::string ValidateGroomSource(const GroomSource& groom)
{
    const std::size_t strandCount = groom.StrandCount();
    const std::size_t pointCount = groom.positions.size();
    if (strandCount == 0u) {
        return pointCount == 0u && groom.widths.empty() ? std::string{} : "groom has points but no strands";
    }
    if (groom.strandPointOffsets.front() != 0u || groom.strandPointOffsets.back() != pointCount) {
        return "strand point offsets do not span the point array";
    }
    for (std::size_t strand = 0; strand < strandCount; ++strand) {
        const std::uint32_t begin = groom.strandPointOffsets[strand];
        const std::uint32_t end = groom.strandPointOffsets[strand + 1u];
        if (end < begin || end - begin < 2u) {
            return "strand " + std::to_string(strand) + " has fewer than two points";
        }
    }
    if (groom.widths.size() != pointCount) {
        return "width count does not match point count";
    }
    if (!groom.colors.empty() && groom.colors.size() != pointCount) {
        return "colour count does not match point count";
    }
    const auto perStrandOk = [strandCount](std::size_t size) { return size == 0u || size == strandCount; };
    if (!perStrandOk(groom.guideFlags.size())) return "guide flag count does not match strand count";
    if (!perStrandOk(groom.groupIds.size())) return "group id count does not match strand count";
    if (!perStrandOk(groom.rootUVs.size())) return "root UV count does not match strand count";
    if (!perStrandOk(groom.closestGuides.size())) return "closest guide count does not match strand count";
    if (!perStrandOk(groom.closestGuideWeights.size())) return "closest guide weight count does not match strand count";
    if (groom.closestGuides.size() != groom.closestGuideWeights.size()) {
        return "closest guides and their weights must be supplied together";
    }
    for (const auto& guides : groom.closestGuides) {
        for (const std::int32_t guide : guides) {
            if (guide < -1 || guide >= static_cast<std::int64_t>(strandCount)) {
                return "closest guide index is out of range";
            }
        }
    }
    return {};
}

} // namespace br::hair
