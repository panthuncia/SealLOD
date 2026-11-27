#ifndef BR_HAIR_GPU_TYPES_H
#define BR_HAIR_GPU_TYPES_H

// Groom structs shared by the CPU builder and the hair shaders. This one file is meant to compile as both C++ and
// HLSL: the struct bodies use only HLSL spellings (uint, float, float3, fixed arrays), and everything C++-only sits
// behind __cplusplus. Layout is the HLSL structured-buffer layout: 4-byte scalars, no implicit padding, so every
// struct is padded by hand to a multiple of 16 bytes.
//
// First drafts for the POC: the builder step may change them. Indices are groom-relative.

#ifdef __cplusplus
#include "Hair/HairMath.h"

#include <cstdint>

namespace br::hair {

using uint = std::uint32_t;
using float3 = Float3;
#endif

#define HAIR_INVALID_INDEX 0xFFFFFFFFu
#define HAIR_MAX_STRANDS_PER_CLUSTER 64u
#define HAIR_AGGREGATE_CONTROL_POINT_COUNT 4u
#define HAIR_MAX_VERTEX_LEVEL 10u // strands have at most 2^10 + 1 points

// One spatial cluster of up to HAIR_MAX_STRANDS_PER_CLUSTER strands, stored sorted by rank.
// A view draws ceil(l * (strandCount + randomOffset)) of them, l in (0,1].
struct HairClusterHeader {
    float3 boundsCenter; // bounding sphere and box share a centre, in groom rest space; they bound strand centrelines
    float boundsRadius;
    float3 boundsExtent; // box half-extents
    uint strandOffset;   // first HairStrandRecord of the cluster
    float3 quantizationOrigin; // position = quantizationOrigin + quantizationScale * (uint16 xyz)
    uint strandCount;
    float3 quantizationScale;
    float randomOffset; // delta_c in [0,1): staggers strand drops between clusters
    uint vertexLevel;   // k: every strand of the cluster has 2^k + 1 points
    uint parentIndex;   // reserved for a culling hierarchy; HAIR_INVALID_INDEX in the flat POC list
    uint pad0;
    uint pad1;
};

// One render strand. Its root binding is the HairRootBinding at the same index.
struct HairStrandRecord {
    uint pointOffset; // first HairPackedPoint of the strand; it has 2^vertexLevel + 1 of them (see its cluster)
    uint rank;        // global progressive rank, 0 = kept longest
    float width;      // widest diameter along the strand, before LOD width scaling
    uint guideIndices[3]; // guide-capable strands (by rank) this strand interpolates from, HAIR_INVALID_INDEX when unused
    float guideWeights[3]; // sum to 1; a guide-capable strand binds to itself with weight 1
    uint clusterIndex;
    uint pad0;
    uint pad1;
};

// One strand point, quantized in its cluster's frame:
//   position = quantizationOrigin + quantizationScale * float3(xy & 0xFFFF, xy >> 16, zw & 0xFFFF)
//   diameter = strand width * (zw >> 16) / 65535
struct HairPackedPoint {
    uint xy;
    uint zw;
};

// Simulation binding of one guide-capable strand (the first Gmax ranks), indexed by rank. The groom's
// guide-strand-index array maps the same index to the strand's HairStrandRecord.
// Guide g at level k is simulated when sFrac >= level + randomOffset; otherwise it is interpolated from its parents.
struct HairGuideBinding {
    uint level;            // 0 = coarsest; level 0 guides have no parents
    uint parentIndices[3]; // guide-capable strands in level - 1, HAIR_INVALID_INDEX when unused
    float parentWeights[3];
    float randomOffset; // delta_g in [0,1): promotes guides one at a time
};

// Attachment of one strand root to the skinned scalp, transferred at import from the nearest scalp triangle.
struct HairRootBinding {
    uint joints[4];
    float weights[4];
    float3 restPosition;
    uint triangleIndex; // nearest scalp triangle, HAIR_INVALID_INDEX without a scalp mesh; unused until morph targets
    float3 restNormal;  // unit
    float barycentricU; // root = a + (b - a) * u + (c - a) * v on that triangle
    float3 restTangent; // unit, perpendicular to restNormal
    float barycentricV;
};

struct HairAggregateControlPoint {
    float3 position; // on the cluster centreline, groom rest space
    float pad0;
    float3 axisMajor; // PCA cross-section ellipse: direction scaled by the semi-axis length (two standard deviations)
    float pad1;
    float3 axisMinor;
    float pad2;
};

// Aggregate of a cluster's strands (Huang et al. 2025): what the LOD metric and a later far tier read.
struct HairClusterAggregate {
    HairAggregateControlPoint controlPoints[HAIR_AGGREGATE_CONTROL_POINT_COUNT]; // root to tip
    uint strandCount;
    float density; // strands per unit cross-section area (mean over the control points), 0 when the area is 0
    uint pad0;
    uint pad1;
};

#ifdef __cplusplus
static_assert(sizeof(HairClusterHeader) == 80u);
static_assert(sizeof(HairStrandRecord) == 48u);
static_assert(sizeof(HairPackedPoint) == 8u); // the one exception to the 16-byte rule: a plain two-uint stream
static_assert(sizeof(HairGuideBinding) == 32u);
static_assert(sizeof(HairRootBinding) == 80u);
static_assert(sizeof(HairAggregateControlPoint) == 48u);
static_assert(sizeof(HairClusterAggregate) == 208u);

} // namespace br::hair
#endif

#endif // BR_HAIR_GPU_TYPES_H
