#pragma once

#include "Hair/HairLodShared.h"

#include <cstdint>

namespace br::hair {

// How many strands of a cluster a view draws at detail level `detailLevel` (l in (0,1]): ceil(l * (N + delta_c)),
// clamped to [1, N]. The cluster stores its strands sorted by rank, so the drawn strands are the first `count`:
// the set at a lower l is always a prefix of the set at a higher l, and l = 1 draws all of them.
// The rule itself is HairLodStrandCountShared (HairLodShared.h), which the select shader compiles too.
inline std::uint32_t HairLodStrandCount(float detailLevel, std::uint32_t strandCount, float randomOffset)
{
    return HairLodStrandCountShared(detailLevel, strandCount, randomOffset);
}

} // namespace br::hair
