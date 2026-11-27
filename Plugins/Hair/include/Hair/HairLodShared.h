#ifndef BR_HAIR_LOD_SHARED_H
#define BR_HAIR_LOD_SHARED_H

// The LOD rules, written once for C++ and HLSL so the builder's tests, the CPU and the select shader cannot drift.
// Like HairGPUTypes.h this compiles as both: the bodies use only spellings both languages have (C-style casts, uint,
// float), and the few intrinsics go through the macros below.
//
// HLSL includes HairGPUTypes.h before this file (hairStructs.hlsli).

#ifdef __cplusplus
#include "Hair/HairGPUTypes.h"

#include <cmath>

#define HAIR_SHARED_INLINE inline
#define HAIR_SHARED_CEIL(x) std::ceil(x)
#define HAIR_SHARED_LOG2(x) std::log2(x)
#define HAIR_SHARED_POW(x, y) std::pow(x, y)

namespace br::hair {
#else
#define HAIR_SHARED_INLINE
#define HAIR_SHARED_CEIL(x) ceil(x)
#define HAIR_SHARED_LOG2(x) log2(x)
#define HAIR_SHARED_POW(x, y) pow(x, y)
#endif

// A cluster's detail level l in [minDetail, 1] from the spacing of its strands on screen: every strand is drawn while
// neighbouring strands are at least lodErrorPixels apart, and the drawn share falls in proportion below that.
HAIR_SHARED_INLINE float HairLodDetailLevel(float strandSpacingPixels, float lodErrorPixels, float minDetail)
{
    const float level = strandSpacingPixels / (lodErrorPixels > 1.0e-6f ? lodErrorPixels : 1.0e-6f);
    if (!(level > minDetail)) {
        return minDetail;
    }
    return level < 1.0f ? level : 1.0f;
}

// The same with the share raised to `exponent`. At 1 it is HairLodDetailLevel: the drawn strands fall as one over the
// distance. At 2 they fall as its square, as the area the cluster covers does, and the kept strands' width
// (HairLodWidthScale) has to make up the coverage.
HAIR_SHARED_INLINE float HairLodDetailLevelPower(float strandSpacingPixels, float lodErrorPixels, float minDetail, float exponent)
{
    const float share = strandSpacingPixels / (lodErrorPixels > 1.0e-6f ? lodErrorPixels : 1.0e-6f);
    if (exponent == 1.0f || !(share > 0.0f) || !(share < 1.0f)) {
        return HairLodDetailLevel(strandSpacingPixels, lodErrorPixels, minDetail);
    }
    const float level = HAIR_SHARED_POW(share, exponent);
    return level > minDetail ? level : minDetail;
}

// How many strands of a cluster a view draws at detail level l: ceil(l * (N + delta_c)), clamped to [1, N]. The cluster
// stores its strands sorted by rank, so the drawn strands are its first `count`.
HAIR_SHARED_INLINE uint HairLodStrandCountShared(float detailLevel, uint strandCount, float randomOffset)
{
    if (strandCount == 0u) {
        return 0u;
    }
    const float count = HAIR_SHARED_CEIL(detailLevel * ((float)strandCount + randomOffset));
    if (!(count > 1.0f)) {
        return 1u;
    }
    return count >= (float)strandCount ? strandCount : (uint)count;
}

// The width factor of the last drawn strand, in (0, 1]: the fraction of a strand that l * (N + delta_c) has over the
// strands before it. It is what makes the drawn count continuous: a strand grows from zero width as l rises and the
// next one starts when it is whole. 1 when only one strand is drawn or every strand is.
HAIR_SHARED_INLINE float HairLodBoundaryWeight(float detailLevel, uint strandCount, float randomOffset)
{
    const float exact = detailLevel * ((float)strandCount + randomOffset);
    const uint count = HairLodStrandCountShared(detailLevel, strandCount, randomOffset);
    if (count <= 1u || exact >= (float)strandCount) {
        return 1.0f;
    }
    const float weight = exact - (float)(count - 1u);
    return weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
}

// How much a drawn strand is widened to keep the cluster's coverage: min(1 / l, maxWidthScale).
HAIR_SHARED_INLINE float HairLodWidthScale(float detailLevel, float maxWidthScale)
{
    const float scale = 1.0f / (detailLevel > 1.0e-6f ? detailLevel : 1.0e-6f);
    return scale < maxWidthScale ? scale : maxWidthScale;
}

// The width scale that keeps a cluster's coverage exactly: its strands over the strands' worth it draws, whole strands
// plus the boundary strand's weight, which is l * (N + delta_c) held to [1, N]. HairLodWidthScale's 1 / l is that only
// while more than one strand is drawn and delta_c is ignored: it overshoots by delta_c / N, and without bound once the
// cluster is down to its one strand. It is what partial coverage uses; a strand under the rasterizer's one-pixel floor
// did not show the difference before.
HAIR_SHARED_INLINE float HairLodCoverageWidthScale(float detailLevel, uint strandCount, float randomOffset, float maxWidthScale)
{
    float drawn = detailLevel * ((float)strandCount + randomOffset);
    drawn = drawn < 1.0f ? 1.0f : (drawn > (float)strandCount ? (float)strandCount : drawn);
    const float scale = (float)strandCount / drawn;
    return scale < maxWidthScale ? scale : maxWidthScale;
}

// The fractional vertex level f in [0, storedLevel] of a strand that is strandLengthPixels long on screen: 2^f
// segments of segmentPixels each. The drawn level is ceil(f) (HairLodDrawnVertexLevel), and the rest is the geomorph.
HAIR_SHARED_INLINE float HairLodVertexLevel(float strandLengthPixels, float segmentPixels, uint storedLevel)
{
    const float segments = strandLengthPixels / (segmentPixels > 1.0e-6f ? segmentPixels : 1.0e-6f);
    if (!(segments > 1.0f)) {
        return 0.0f;
    }
    const float level = HAIR_SHARED_LOG2(segments);
    return level < (float)storedLevel ? level : (float)storedLevel;
}

// The level drawn for fractional level f: a strand has 2^level segments.
HAIR_SHARED_INLINE uint HairLodDrawnVertexLevel(float fractionalLevel)
{
    return (uint)HAIR_SHARED_CEIL(fractionalLevel);
}

// How far the odd vertices of the drawn level have moved to the midpoint of their neighbours, in [0, 1): 0 at a whole
// level, and towards 1 the strand takes the shape of the level below, where those vertices are dropped.
HAIR_SHARED_INLINE float HairLodGeomorph(float fractionalLevel)
{
    return (float)HairLodDrawnVertexLevel(fractionalLevel) - fractionalLevel;
}

#ifdef __cplusplus
} // namespace br::hair
#endif

#endif // BR_HAIR_LOD_SHARED_H
