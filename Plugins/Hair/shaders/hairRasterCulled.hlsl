// The layered path's rasterizer with hidden hair culled (HAIR_RASTER_FLAG_CULLED). It fills the same targets as
// hairRaster.hlsl's layered form, from the same footprints, pixel for pixel: three layers of keys, the summed coverage,
// the marks of the vertices to light. What differs is which pixels it visits and in what order.
//
// The hidden depth. Hair.Occlusion holds, per pixel, the depth from which hair adds nothing to the pixel: at the start
// of the frame the opaque surface's, then lowered during the pass by hair itself in two ways.
//   - A strand that covers its pixel whole (coverage 1) lets nothing through, so the hidden depth falls to its own
//     (HAIR_RASTER_FLAG_OCCLUDE_OPAQUE). It adds HAIR_COVERAGE_OPAQUE to the pixel's coverage, not 1
//     (HAIR_RASTER_FLAG_OPAQUE_COVERAGE): if it ends in the pixel's three layers they are opaque with it, and if it ends
//     behind them the remainder is, so the composite's result does not depend on what is behind it.
//   - Every pixel has a split depth, a guess made from the frame before. Coverage is summed apart in front of it (near)
//     and behind it (far); the composite takes the two together. Once the near sum reaches the saturation
//     (HairFrameGPU::saturationUnits, ten pixels' worth) the hair in front of the split depth hides what is behind it to
//     within exp(-(saturation - 3)), whatever is behind, and the hidden depth falls to the split depth
//     (HAIR_RASTER_FLAG_OCCLUDE_SATURATED). A wrong guess costs time, never the image: too near and the near sum does not
//     saturate, too far and less is skipped.
// A pixel whose depth is at or behind the hidden depth is skipped: no coverage add, no layer write. The hidden depth only
// falls, and it is read by a plain load: a stale read draws a pixel it could have skipped.
//
// Chunks and tiles. A segment is cut into chunks of HAIR_RASTER_TILE_SIZE pixels along its major axis, aligned to the
// screen's tiles of that size. A chunk is the unit of work: one thread draws one chunk, so the lanes of a wave have work
// of the same length, and a hidden chunk costs one test, not its pixels. Hair.OcclusionCoarse holds the largest hidden
// depth of each tile: a chunk whose nearest point is behind that in every tile it touches is hidden.
//
// Two passes, so that nothing waits a frame to be seen.
//   HairClassifyCS      one thread per pool row: sorts its chunks into the first pass's item list and its late mask. The
//                       first pass gets the chunks that are probably in front (HAIR_RASTER_FLAG_FIRST_*): those the
//                       tile depths of the last frames do not hide, of clusters that drew a pixel lately, and a prefix
//                       of each cluster's strands, which the rank order spreads evenly over it.
//   HairRasterArgsCS    the first pass's dispatch size, from the list's length.
//   HairOcclusionInitCS the hidden depth and the split depth of every pixel of the tiles hair touches.
//   HairRasterItemsCS   the first pass: one thread per item.
//   HairOcclusionUpdateCS  the tile depths from the hidden depths the first pass left, and the next frame's split depths.
//   HairOcclusionHintCS the next frame's guess: each tile's largest depth over the last frames.
//   HairRasterLateCS    one thread per pool row with late chunks: each is tested against this frame's tile depths and
//                       drawn if it is not hidden.
// The frames before are only ever a guess at what to draw first. Whatever the first pass leaves out is tested against this
// frame's own hidden depths, so hair that comes into view is drawn in the frame it does.

#include "HairShaders/hairCommon.hlsli"
#include "HairShaders/hairShading.hlsli"

// HairRasterPass::Record.
#define HAIR_FRAME_STATE UintRootConstant0       // StructuredBuffer<HairFrameGPU>
#define HAIR_COUNTERS UintRootConstant1          // RWStructuredBuffer<uint>
#define HAIR_VISIBLE_CLUSTERS UintRootConstant2  // StructuredBuffer<HairVisibleCluster>
#define HAIR_POOL_VERTICES UintRootConstant3     // StructuredBuffer<HairPoolVertex>
#define HAIR_POOL_SEGMENT_IDS UintRootConstant4  // StructuredBuffer<uint>
#define HAIR_VISIBILITY UintRootConstant5        // RWTexture2D<uint64_t>: the nearest layer
#define HAIR_VISIBILITY_WIDTH UintRootConstant6
#define HAIR_VISIBILITY_HEIGHT UintRootConstant7
#define HAIR_GROUPS_PER_ROW UintRootConstant8    // a pool-row dispatch is this many groups wide
#define HAIR_COVERAGE UintRootConstant9          // RWTexture2D<uint>: coverage in front of the split depth
#define HAIR_LAYER1 UintRootConstant10           // RWTexture2D<uint64_t>
#define HAIR_LAYER2 UintRootConstant11
#define HAIR_POOL_COLORS UintRootConstant12      // RWStructuredBuffer<uint2>: the marks
#define HAIR_COVERAGE_FAR UintRootConstant13     // RWTexture2D<uint>: coverage behind the split depth
#define HAIR_OCCLUSION UintRootConstant14        // RWTexture2D<uint64_t>: hidden depth bits << 32 | split depth bits
#define HAIR_OCCLUSION_COARSE UintRootConstant15 // RWTexture2D<uint>: the largest hidden depth of each tile
#define HAIR_TILE_FLAGS UintRootConstant16       // RWTexture2D<uint>: the frame a chunk last touched the tile in
#define HAIR_SPLIT_GUESS UintRootConstant17      // RWTexture2D<uint>: the split depth the next frame starts from
#define HAIR_RASTER_ITEMS UintRootConstant18     // RWStructuredBuffer<uint>
#define HAIR_POOL_LATE UintRootConstant19        // RWStructuredBuffer<uint>: a row's late chunks
#define HAIR_RASTER_ARGS UintRootConstant20      // RWStructuredBuffer<uint>: the first pass's dispatch arguments
#define HAIR_CLUSTER_SEEN UintRootConstant21     // RWStructuredBuffer<uint>: the cluster state
#define HAIR_OCCLUSION_HINT UintRootConstant22   // RWTexture2D<uint>: each tile's largest hidden depth over the last frames

// A pool row's segment, placed on screen exactly as hairRaster.hlsl places it.
struct HairRasterSegment
{
    uint segmentId;
    uint seenSlot;
    bool seen;
    uint strandIndex;
    uint prefixCount;
    float2 screen0;
    float2 delta;
    float inverseW0;
    float inverseW1;
    float trueHalfWidth0;
    float trueHalfWidth1;
    float halfWidth0;
    float halfWidth1;
    bool steep;
    float major0;
    float minor0;
    float minor1;
    float minorLast;
    float inverseMajorDelta;
    float inverseLengthSquared;
    float minorReach;
    int majorFirst;
    int majorEnd;
    bool roundStart;
    bool roundEnd;
};

// False for a row that starts no segment of this frame, or whose segment is off screen or behind the near plane.
bool HairLoadRasterSegment(uint row, HairFrameGPU frame, uint visibleClusterCount, out HairRasterSegment segment)
{
    segment = (HairRasterSegment)0;
    StructuredBuffer<uint> poolSegmentIds = ResourceDescriptorHeap[HAIR_POOL_SEGMENT_IDS];
    const uint segmentId = poolSegmentIds[row];
    if (segmentId == HAIR_INVALID_INDEX)
    {
        return false;
    }
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    const uint visibleIndex = segmentId >> HAIR_VIS_CLUSTER_SHIFT;
    if (visibleIndex >= visibleClusterCount)
    {
        return false;
    }
    const HairVisibleCluster visible = visibleClusters[visibleIndex];
    const uint clusterVertexCount = visible.strandCount * ((1u << visible.vertexLevel) + 1u);
    if (row < visible.poolVertexBase || row >= visible.poolVertexBase + clusterVertexCount)
    {
        return false;
    }
    segment.segmentId = segmentId;
    segment.seenSlot = visible.seenSlot;
    segment.seen = (visible.flags & HAIR_VISIBLE_FLAG_SEEN) != 0u;
    segment.strandIndex = (segmentId >> HAIR_VIS_SEGMENT_BITS) & ((1u << HAIR_VIS_STRAND_BITS) - 1u);
    segment.prefixCount = (visible.strandCount + 7u) >> 3u;

    StructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_POOL_VERTICES];
    const HairPoolVertex start = poolVertices[row];
    const HairPoolVertex end = poolVertices[row + 1u];
    const Camera camera = HairLoadMainCamera();
    float4 clip0 = mul(float4(start.position, 1.0f), camera.viewProjection);
    float4 clip1 = mul(float4(end.position, 1.0f), camera.viewProjection);
    float width0 = start.width;
    float width1 = end.width;
    if (clip0.w < camera.zNear && clip1.w < camera.zNear)
    {
        return false;
    }
    if (clip0.w < camera.zNear)
    {
        const float t = (camera.zNear - clip0.w) / (clip1.w - clip0.w);
        clip0 = lerp(clip0, clip1, t);
        width0 = lerp(width0, width1, t);
    }
    else if (clip1.w < camera.zNear)
    {
        const float t = (camera.zNear - clip1.w) / (clip0.w - clip1.w);
        clip1 = lerp(clip1, clip0, t);
        width1 = lerp(width1, width0, t);
    }

    const float2 size = float2((float)HAIR_VISIBILITY_WIDTH, (float)HAIR_VISIBILITY_HEIGHT);
    const float inverseW0 = 1.0f / clip0.w;
    const float inverseW1 = 1.0f / clip1.w;
    const float2 screen0 = float2(clip0.x * inverseW0 * 0.5f + 0.5f, 0.5f - clip0.y * inverseW0 * 0.5f) * size;
    const float2 screen1 = float2(clip1.x * inverseW1 * 0.5f + 0.5f, 0.5f - clip1.y * inverseW1 * 0.5f) * size;
    const float halfPixelsPerMetre = 0.5f * HairPixelsPerMetreAtUnitDepth(camera, size.y);
    segment.trueHalfWidth0 = width0 * halfPixelsPerMetre * inverseW0;
    segment.trueHalfWidth1 = width1 * halfPixelsPerMetre * inverseW1;
    segment.halfWidth0 = max(segment.trueHalfWidth0, 0.5f);
    segment.halfWidth1 = max(segment.trueHalfWidth1, 0.5f);
    const float maxHalfWidth = max(segment.halfWidth0, segment.halfWidth1);
    const float2 boundsMin = min(screen0, screen1) - maxHalfWidth;
    const float2 boundsMax = max(screen0, screen1) + maxHalfWidth;
    if (!(boundsMax.x >= 0.0f && boundsMax.y >= 0.0f && boundsMin.x < size.x && boundsMin.y < size.y))
    {
        return false;
    }

    const float2 delta = screen1 - screen0;
    segment.screen0 = screen0;
    segment.delta = delta;
    segment.inverseW0 = inverseW0;
    segment.inverseW1 = inverseW1;
    segment.steep = abs(delta.y) > abs(delta.x);
    segment.major0 = segment.steep ? screen0.y : screen0.x;
    const float major1 = segment.steep ? screen1.y : screen1.x;
    segment.minor0 = segment.steep ? screen0.x : screen0.y;
    segment.minor1 = segment.steep ? screen1.x : screen1.y;
    const float majorLast = (segment.steep ? size.y : size.x) - 1.0f;
    segment.minorLast = (segment.steep ? size.x : size.y) - 1.0f;
    const float majorDelta = major1 - segment.major0;
    segment.inverseMajorDelta = abs(majorDelta) > 1.0e-6f ? 1.0f / majorDelta : 0.0f;
    const float lengthSquared = dot(delta, delta);
    segment.inverseLengthSquared = lengthSquared > 1.0e-12f ? 1.0f / lengthSquared : 0.0f;
    // A pixel centre within a half-width of the centreline is, along the minor axis, within that half-width times
    // sqrt(1 + slope^2) of the centreline's place in its column (at most sqrt(2): the segment is at most 45 degrees off
    // the major axis); past an end it is within the half-width of the end. So a column's pixels are the centres in that
    // reach of the column's centre point, and no others are looked at.
    const float slope = (segment.minor1 - segment.minor0) * segment.inverseMajorDelta;
    segment.minorReach = maxHalfWidth * min(sqrt(1.0f + slope * slope), 1.4143f) + 1.0e-3f;
    segment.majorFirst = (int)clamp(floor(min(segment.major0, major1) - maxHalfWidth), 0.0f, majorLast);
    segment.majorEnd = (int)clamp(floor(max(segment.major0, major1) + maxHalfWidth), 0.0f, majorLast);
    // The layered path's joints: a pixel past one of the segment's ends is the neighbouring segment's, unless the strand
    // ends there.
    segment.roundStart = (segmentId & ((1u << HAIR_VIS_SEGMENT_BITS) - 1u)) == 0u;
    segment.roundEnd = poolSegmentIds[row + 1u] == HAIR_INVALID_INDEX;
    return true;
}

// Chunk c of a segment covers the major-axis pixels of screen tile (majorFirst >> shift) + c that the segment reaches.
uint HairChunkCount(HairRasterSegment segment)
{
    return (uint)((segment.majorEnd >> HAIR_RASTER_TILE_SHIFT) - (segment.majorFirst >> HAIR_RASTER_TILE_SHIFT)) + 1u;
}

void HairChunkMajorRange(HairRasterSegment segment, uint chunk, out int first, out int last)
{
    const int tileFirst = ((segment.majorFirst >> HAIR_RASTER_TILE_SHIFT) + (int)chunk) << HAIR_RASTER_TILE_SHIFT;
    first = max(segment.majorFirst, tileFirst);
    last = min(segment.majorEnd, tileFirst + (int)HAIR_RASTER_TILE_SIZE - 1);
}

int HairMinorFirst(HairRasterSegment segment, int major, out int minorEnd)
{
    const float majorCenter = (float)major + 0.5f;
    const float minorCenter = lerp(segment.minor0, segment.minor1, saturate((majorCenter - segment.major0) * segment.inverseMajorDelta));
    // Pixel n's centre is at n + 0.5. An empty range (the reach falls between two centres) has minorEnd below the first.
    minorEnd = (int)min(floor(minorCenter + segment.minorReach - 0.5f), segment.minorLast);
    return (int)max(ceil(minorCenter - segment.minorReach - 0.5f), 0.0f);
}

float HairAlong(HairRasterSegment segment, int major, int minor)
{
    const int2 pixel = segment.steep ? int2(minor, major) : int2(major, minor);
    return dot(float2(pixel) + 0.5f - segment.screen0, segment.delta) * segment.inverseLengthSquared;
}

// The tiles a chunk's pixels can lie in, and a depth no pixel of the chunk is in front of.
struct HairChunkBounds
{
    int majorTile;
    int minorTileFirst;
    int minorTileLast;
    uint depthBits;
};

HairChunkBounds HairBoundChunk(HairRasterSegment segment, uint chunk)
{
    int first;
    int last;
    HairChunkMajorRange(segment, chunk, first, last);
    int minorEndA;
    int minorEndB;
    const int minorFirstA = HairMinorFirst(segment, first, minorEndA);
    const int minorFirstB = HairMinorFirst(segment, last, minorEndB);
    // A column's range can be empty, and then lies off its ends: the chunk's is kept on screen.
    const int minorFirst = clamp(min(minorFirstA, minorFirstB), 0, (int)segment.minorLast);
    const int minorEnd = clamp(max(minorEndA, minorEndB), 0, (int)segment.minorLast);
    // The pixels' parameters along the segment lie between those of the four corners, and 1 / w is linear in it.
    const float a0 = saturate(HairAlong(segment, first, minorFirst));
    const float a1 = saturate(HairAlong(segment, first, minorEnd));
    const float a2 = saturate(HairAlong(segment, last, minorFirst));
    const float a3 = saturate(HairAlong(segment, last, minorEnd));
    const float tLow = min(min(a0, a1), min(a2, a3));
    const float tHigh = max(max(a0, a1), max(a2, a3));
    const float inverseW = max(lerp(segment.inverseW0, segment.inverseW1, tLow), lerp(segment.inverseW0, segment.inverseW1, tHigh));
    HairChunkBounds bounds;
    bounds.majorTile = first >> HAIR_RASTER_TILE_SHIFT;
    bounds.minorTileFirst = minorFirst >> HAIR_RASTER_TILE_SHIFT;
    bounds.minorTileLast = minorEnd >> HAIR_RASTER_TILE_SHIFT;
    // A few units nearer than computed, for the rounding of the corner arithmetic against a pixel's own.
    const uint depthBits = HairDepthBits(1.0f / inverseW);
    bounds.depthBits = depthBits > 8u ? depthBits - 8u : 0u;
    return bounds;
}

uint2 HairTileCoordinate(HairRasterSegment segment, int majorTile, int minorTile)
{
    return segment.steep ? uint2(minorTile, majorTile) : uint2(majorTile, minorTile);
}

// Whether the tile depths hide the whole chunk. A chunk over more tiles than this is not tested.
#define HAIR_CHUNK_MAX_TILES 6

bool HairChunkHidden(HairRasterSegment segment, HairChunkBounds bounds, RWTexture2D<uint> coarse)
{
    if (bounds.minorTileLast - bounds.minorTileFirst >= HAIR_CHUNK_MAX_TILES)
    {
        return false;
    }
    for (int minorTile = bounds.minorTileFirst; minorTile <= bounds.minorTileLast; ++minorTile)
    {
        if (bounds.depthBits < coarse[HairTileCoordinate(segment, bounds.majorTile, minorTile)])
        {
            return false;
        }
    }
    return true;
}

struct HairRasterCounts
{
    uint tested;
    uint covered;
    float coverageSum;
    uint occluded;
    uint layerRejected;
    uint written;
    uint adds;
    uint hiddenWrites;
    uint saturated;
    uint waveIterations;
    bool capped;
};

// Draws the pixels of one chunk: hairRaster.hlsl's loop over a part of the major axis, with the hidden depth in place of
// the opaque visibility buffer.
void HairRasterChunk(HairRasterSegment segment, uint chunk, HairFrameGPU frame, inout HairRasterCounts counts)
{
    RWTexture2D<uint64_t> hairVisibility = ResourceDescriptorHeap[HAIR_VISIBILITY];
    RWTexture2D<uint64_t> hairLayer1 = ResourceDescriptorHeap[HAIR_LAYER1];
    RWTexture2D<uint64_t> hairLayer2 = ResourceDescriptorHeap[HAIR_LAYER2];
    RWTexture2D<uint> coverageNear = ResourceDescriptorHeap[HAIR_COVERAGE];
    RWTexture2D<uint> coverageFar = ResourceDescriptorHeap[HAIR_COVERAGE_FAR];
    RWTexture2D<uint64_t> occlusion = ResourceDescriptorHeap[HAIR_OCCLUSION];
    const bool opaqueCoverage = (frame.rasterFlags & HAIR_RASTER_FLAG_OPAQUE_COVERAGE) != 0u;
    const bool occludeOpaque = (frame.rasterFlags & HAIR_RASTER_FLAG_OCCLUDE_OPAQUE) != 0u;
    const bool occludeSaturated = (frame.rasterFlags & HAIR_RASTER_FLAG_OCCLUDE_SATURATED) != 0u;
    const bool profile = (frame.rasterFlags & HAIR_RASTER_FLAG_PROFILE) != 0u;
    int majorFirst;
    int majorLast;
    HairChunkMajorRange(segment, chunk, majorFirst, majorLast);
    uint tested = 0u;
    for (int major = majorFirst; major <= majorLast && !counts.capped; ++major)
    {
        int minorEnd;
        const int minorFirst = HairMinorFirst(segment, major, minorEnd);
        for (int minor = minorFirst; minor <= minorEnd; ++minor)
        {
            if (tested >= HAIR_RASTER_MAX_PIXELS_PER_SEGMENT)
            {
                counts.capped = true;
                break;
            }
            ++tested;
            if (profile && WaveIsFirstLane())
            {
                counts.waveIterations += WaveGetLaneCount();
            }
            const int2 pixel = segment.steep ? int2(minor, major) : int2(major, minor);
            const float2 pixelCenter = float2(pixel) + 0.5f;
            const float along = dot(pixelCenter - segment.screen0, segment.delta) * segment.inverseLengthSquared;
            const float t = saturate(along);
            const float2 offset = pixelCenter - (segment.screen0 + segment.delta * t);
            const float halfWidth = lerp(segment.halfWidth0, segment.halfWidth1, t);
            if (dot(offset, offset) > halfWidth * halfWidth)
            {
                continue;
            }
            if (!((along >= 0.0f || segment.roundStart) && (along < 1.0f || segment.roundEnd)))
            {
                continue;
            }
            const uint depthBits = HairDepthBits(1.0f / lerp(segment.inverseW0, segment.inverseW1, t));
            const uint64_t hidden = occlusion[pixel];
            if (depthBits >= (uint)(hidden >> 32u))
            {
                ++counts.occluded;
                continue;
            }
            const uint splitBits = (uint)hidden & HAIR_DEPTH_BITS_FAR;
            const float coverage = saturate(lerp(segment.trueHalfWidth0, segment.trueHalfWidth1, t) / halfWidth);
            ++counts.covered;
            counts.coverageSum += coverage;
            const bool opaque = coverage >= 1.0f;
            // A strand that covers the pixel whole and hides what is behind it: the hidden depth falls to its own and the
            // pixel is marked saturated, which stands for its coverage; there is nothing to add. In the layers it needs
            // only its own place: whatever it pushes out of a layer is behind it, and adds nothing to the pixel.
            const bool occluder = opaque && occludeOpaque;
            if (occluder)
            {
                InterlockedMin(occlusion[pixel], ((uint64_t)depthBits << 32u) | (uint64_t)(splitBits | HAIR_OCCLUSION_SATURATED));
                ++counts.hiddenWrites;
            }
            else
            {
                const uint units = opaque && opaqueCoverage ? (uint)(HAIR_COVERAGE_OPAQUE * HAIR_COVERAGE_UNIT) : (uint)(coverage * HAIR_COVERAGE_UNIT + 0.5f);
                ++counts.adds;
                if (depthBits < splitBits)
                {
                    uint before;
                    InterlockedAdd(coverageNear[pixel], units, before);
                    if (occludeSaturated && before < frame.saturationUnits && before + units >= frame.saturationUnits)
                    {
                        // The one add that takes the near sum past the saturation: everything from the split depth on is hidden.
                        InterlockedMin(occlusion[pixel], ((uint64_t)splitBits << 32u) | (uint64_t)(splitBits | HAIR_OCCLUSION_SATURATED));
                        ++counts.saturated;
                    }
                }
                else
                {
                    InterlockedAdd(coverageFar[pixel], units);
                }
            }
            // A strand of no width (the boundary strand at the start of its fade) is in no layer.
            if (!(coverage > 0.0f))
            {
                continue;
            }
            uint64_t key = HairPackVisKey(depthBits, segment.segmentId);
            if (key >= hairLayer2[pixel])
            {
                ++counts.layerRejected;
                continue;
            }
            ++counts.written;
            uint64_t held;
            InterlockedMin(hairVisibility[pixel], key, held);
            if (occluder && held > key)
            {
                continue;
            }
            key = max(key, held);
            if (key != 0xFFFFFFFFFFFFFFFFull)
            {
                InterlockedMin(hairLayer1[pixel], key, held);
                if (occluder && held > key)
                {
                    continue;
                }
                key = max(key, held);
                if (key != 0xFFFFFFFFFFFFFFFFull)
                {
                    InterlockedMin(hairLayer2[pixel], key);
                }
            }
        }
    }
    counts.tested += tested;
}

// What a thread leaves when its chunks are drawn: the marks of its segment's vertices, its cluster's stamp, and the
// frame's counters, one atomic a wave each. `lateChunks` is the late pass's count of chunks it drew.
void HairFinishRaster(uint row, HairRasterSegment segment, HairFrameGPU frame, HairRasterCounts counts, bool late, uint lateChunks)
{
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    if (counts.written != 0u)
    {
        // Plain stores of one value: two segments that share a vertex write the same mark.
        RWStructuredBuffer<uint2> poolColors = ResourceDescriptorHeap[HAIR_POOL_COLORS];
        poolColors[row] = HairShadeMark(frame.frameNumber);
        poolColors[row + 1u] = HairShadeMark(frame.frameNumber);
    }
    if (counts.covered != 0u && segment.seenSlot < frame.clusterSeenCapacity)
    {
        RWStructuredBuffer<uint> clusterSeen = ResourceDescriptorHeap[HAIR_CLUSTER_SEEN];
        const uint stateRow = segment.seenSlot * HAIR_CLUSTER_STATE_WORDS + HAIR_CLUSTER_STATE_SEEN;
        if (clusterSeen[stateRow] != frame.frameNumber)
        {
            clusterSeen[stateRow] = frame.frameNumber;
        }
    }
    const float rounding = (float)((segment.segmentId * 747796405u + frame.frameNumber * 2891336453u) >> 8u) * (1.0f / 16777216.0f);
    const uint coverageUnits = (uint)(counts.coverageSum * HAIR_COVERAGE_COUNTER_UNIT + rounding);
    const uint tested = WaveActiveSum(counts.tested);
    const uint covered = WaveActiveSum(counts.covered);
    const uint coverage = WaveActiveSum(coverageUnits);
    const uint occluded = WaveActiveSum(counts.occluded);
    const uint layerRejected = WaveActiveSum(counts.layerRejected);
    const uint written = WaveActiveSum(counts.written);
    const uint drawn = WaveActiveCountBits(counts.written != 0u);
    const uint adds = WaveActiveSum(counts.adds);
    const uint hiddenWrites = WaveActiveSum(counts.hiddenWrites);
    const uint saturated = WaveActiveSum(counts.saturated);
    const uint waveIterations = WaveActiveSum(counts.waveIterations);
    const uint capped = WaveActiveCountBits(counts.capped);
    const uint lateDrawn = WaveActiveSum(lateChunks);
    if (WaveIsFirstLane())
    {
        if (tested != 0u) InterlockedAdd(counters[HAIR_COUNTER_PIXELS_TESTED], tested);
        if (covered != 0u) InterlockedAdd(counters[HAIR_COUNTER_PIXELS_COVERED], covered);
        if (coverage != 0u) InterlockedAdd(counters[HAIR_COUNTER_COVERAGE_SUM], coverage);
        if (occluded != 0u) InterlockedAdd(counters[HAIR_COUNTER_PIXELS_OCCLUDED], occluded);
        if (layerRejected != 0u) InterlockedAdd(counters[HAIR_COUNTER_LAYER_REJECTED], layerRejected);
        if (written != 0u) InterlockedAdd(counters[HAIR_COUNTER_PIXELS_WRITTEN], written);
        if (drawn != 0u) InterlockedAdd(counters[HAIR_COUNTER_SEGMENTS_DRAWN], drawn);
        if (adds != 0u) InterlockedAdd(counters[HAIR_COUNTER_COVERAGE_ADDS], adds);
        if (hiddenWrites != 0u) InterlockedAdd(counters[HAIR_COUNTER_HIDDEN_WRITES], hiddenWrites);
        if (saturated != 0u) InterlockedAdd(counters[HAIR_COUNTER_PIXELS_SATURATED], saturated);
        if (waveIterations != 0u) InterlockedAdd(counters[HAIR_COUNTER_WAVE_ITERATIONS], waveIterations);
        if (capped != 0u) InterlockedAdd(counters[HAIR_COUNTER_SEGMENTS_CAPPED], capped);
        if (late && written != 0u) InterlockedAdd(counters[HAIR_COUNTER_LATE_PIXELS_WRITTEN], written);
        if (lateDrawn != 0u) InterlockedAdd(counters[HAIR_COUNTER_CHUNKS_LATE_DRAWN], lateDrawn);
    }
}

uint HairVisibleClusterCount(HairFrameGPU frame)
{
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    return min(counters[HAIR_COUNTER_CLUSTERS_VISIBLE], frame.visibleClusterCapacity);
}

uint HairPoolVertexCount(HairFrameGPU frame)
{
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    return min(counters[HAIR_COUNTER_POOL_VERTICES], frame.poolVertexCapacity);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairClassifyCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint row = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    if (row + 1u >= HairPoolVertexCount(frame))
    {
        return;
    }
    HairRasterSegment segment;
    const bool valid = HairLoadRasterSegment(row, frame, HairVisibleClusterCount(frame), segment);
    // The guess at what is hidden: the tile depths of the frames before (HairOcclusionHintCS).
    RWTexture2D<uint> coarse = ResourceDescriptorHeap[HAIR_OCCLUSION_HINT];
    RWTexture2D<uint> tileFlags = ResourceDescriptorHeap[HAIR_TILE_FLAGS];

    // Bit i: chunk i; the last bit: every chunk from that one on.
    uint firstMask = 0u;
    uint lateMask = 0u;
    uint chunkCount = 0u;
    if (valid)
    {
        chunkCount = HairChunkCount(segment);
        const bool usePrevious = (frame.rasterFlags & HAIR_RASTER_FLAG_FIRST_PREVIOUS) != 0u;
        const bool usePrefix = (frame.rasterFlags & HAIR_RASTER_FLAG_FIRST_PREFIX) != 0u;
        const bool everything = !usePrevious && !usePrefix;
        const bool prefix = usePrefix && segment.strandIndex < segment.prefixCount;
        for (uint chunk = 0u; chunk < chunkCount; ++chunk)
        {
            const HairChunkBounds bounds = HairBoundChunk(segment, chunk);
            // The tiles the frame has hair in.
            for (int minorTile = bounds.minorTileFirst; minorTile <= bounds.minorTileLast; ++minorTile)
            {
                const uint2 tile = HairTileCoordinate(segment, bounds.majorTile, minorTile);
                if (tileFlags[tile] != frame.frameNumber)
                {
                    tileFlags[tile] = frame.frameNumber;
                }
            }
            const uint bit = 1u << min(chunk, HAIR_RASTER_LATE_BITS - 1u);
            bool first = everything || prefix;
            if (!first && usePrevious && segment.seen)
            {
                // Hidden in every one of the last frames: left for the late pass, which will most likely find it hidden.
                first = !HairChunkHidden(segment, bounds, coarse);
            }
            if (first)
            {
                firstMask |= bit;
            }
        }
        // The chunks that share the last bit go together.
        const uint drawn = chunkCount >= HAIR_RASTER_LATE_BITS ? 0xFFFFFFFFu : (1u << chunkCount) - 1u;
        lateMask = drawn & ~firstMask;
    }

    const uint sharedChunks = chunkCount >= HAIR_RASTER_LATE_BITS ? chunkCount - (HAIR_RASTER_LATE_BITS - 1u) : 1u;
    uint itemCount = countbits(firstMask & 0x7FFFFFFFu) + ((firstMask >> 31u) != 0u ? sharedChunks : 0u);
    const uint waveItems = WaveActiveSum(itemCount);
    const uint before = WavePrefixSum(itemCount);
    uint waveFirst = 0u;
    if (waveItems != 0u && WaveIsFirstLane())
    {
        InterlockedAdd(counters[HAIR_COUNTER_RASTER_ITEMS], waveItems, waveFirst);
    }
    waveFirst = WaveReadLaneFirst(waveFirst);
    bool overflow = false;
    if (itemCount != 0u)
    {
        if (waveFirst + before + itemCount > frame.rasterItemCapacity)
        {
            // No room: the late pass draws them. The rows of the list this thread was given and does not fill hold
            // nothing.
            RWStructuredBuffer<uint> unused = ResourceDescriptorHeap[HAIR_RASTER_ITEMS];
            for (uint slot = waveFirst + before; slot < min(waveFirst + before + itemCount, frame.rasterItemCapacity); ++slot)
            {
                unused[slot] = HAIR_INVALID_INDEX;
            }
            overflow = true;
            lateMask |= firstMask;
            itemCount = 0u;
        }
        else
        {
            RWStructuredBuffer<uint> items = ResourceDescriptorHeap[HAIR_RASTER_ITEMS];
            uint next = waveFirst + before;
            for (uint chunk = 0u; chunk < chunkCount; ++chunk)
            {
                if ((firstMask & (1u << min(chunk, HAIR_RASTER_LATE_BITS - 1u))) != 0u)
                {
                    items[next++] = row | (chunk << HAIR_RASTER_ITEM_ROW_BITS);
                }
            }
        }
    }
    RWStructuredBuffer<uint> poolLate = ResourceDescriptorHeap[HAIR_POOL_LATE];
    poolLate[row] = lateMask;

    const uint lateChunks = countbits(lateMask & 0x7FFFFFFFu) + ((lateMask >> 31u) != 0u ? sharedChunks : 0u);
    const uint chunks = WaveActiveSum(chunkCount);
    const uint late = WaveActiveSum(lateChunks);
    const uint overflowed = WaveActiveSum(overflow ? lateChunks : 0u);
    if (WaveIsFirstLane())
    {
        if (chunks != 0u) InterlockedAdd(counters[HAIR_COUNTER_CHUNKS], chunks);
        if (late != 0u) InterlockedAdd(counters[HAIR_COUNTER_CHUNKS_LATE], late);
        if (overflowed != 0u) InterlockedAdd(counters[HAIR_COUNTER_RASTER_ITEMS_OVERFLOW], overflowed);
    }
}

// The first pass's dispatch arguments: one thread per item, HAIR_GROUPS_PER_ROW groups to a row.
[shader("compute")]
[numthreads(1, 1, 1)]
void HairRasterArgsCS()
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    RWStructuredBuffer<uint> arguments = ResourceDescriptorHeap[HAIR_RASTER_ARGS];
    const uint items = min(counters[HAIR_COUNTER_RASTER_ITEMS], frameState[0].rasterItemCapacity);
    const uint groups = (items + 63u) / 64u;
    arguments[0] = min(groups, HAIR_GROUPS_PER_ROW);
    arguments[1] = (groups + HAIR_GROUPS_PER_ROW - 1u) / HAIR_GROUPS_PER_ROW;
    arguments[2] = 1u;
}

// One group per tile. In a tile hair touches this frame: the hidden depth starts as the opaque surface's, and the split
// depth is the guess the frame before left.
[shader("compute")]
[numthreads(HAIR_RASTER_TILE_SIZE, HAIR_RASTER_TILE_SIZE, 1)]
void HairOcclusionInitCS(uint3 groupId : SV_GroupID, uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWTexture2D<uint> tileFlags = ResourceDescriptorHeap[HAIR_TILE_FLAGS];
    if (tileFlags[groupId.xy] != frameState[0].frameNumber)
    {
        return;
    }
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= HAIR_VISIBILITY_WIDTH || pixel.y >= HAIR_VISIBILITY_HEIGHT)
    {
        return;
    }
    Texture2D<uint64_t> opaqueVisibility =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    RWTexture2D<uint64_t> occlusion = ResourceDescriptorHeap[HAIR_OCCLUSION];
    RWTexture2D<uint> splitGuess = ResourceDescriptorHeap[HAIR_SPLIT_GUESS];
    // The opaque buffer's "nothing" is all ones: clamped, so that no real depth reaches it.
    const uint hiddenBits = min(HairVisKeyDepthBits(opaqueVisibility[pixel]), HAIR_DEPTH_BITS_FAR);
    const uint splitBits = (frameState[0].rasterFlags & HAIR_RASTER_FLAG_OCCLUDE_SATURATED) != 0u ? (splitGuess[pixel] & HAIR_DEPTH_BITS_FAR) : HAIR_DEPTH_BITS_FAR;
    occlusion[pixel] = ((uint64_t)hiddenBits << 32u) | (uint64_t)splitBits;
}

// One group per tile, after the first pass. A tile hair touches gets the largest hidden depth of its pixels, for the late
// pass and for the next frame's first pass, and its pixels their next split depths; any other tile hides nothing.
[shader("compute")]
[numthreads(HAIR_RASTER_TILE_SIZE, HAIR_RASTER_TILE_SIZE, 1)]
void HairOcclusionUpdateCS(uint3 groupId : SV_GroupID, uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWTexture2D<uint> tileFlags = ResourceDescriptorHeap[HAIR_TILE_FLAGS];
    RWTexture2D<uint> coarse = ResourceDescriptorHeap[HAIR_OCCLUSION_COARSE];
    const HairFrameGPU frame = frameState[0];
    if (tileFlags[groupId.xy] != frame.frameNumber)
    {
        if (groupIndex == 0u)
        {
            coarse[groupId.xy] = HAIR_DEPTH_BITS_FAR;
        }
        return;
    }
    const uint2 pixel = dispatchThreadId.xy;
    uint hiddenBits = 0u;
    if (pixel.x < HAIR_VISIBILITY_WIDTH && pixel.y < HAIR_VISIBILITY_HEIGHT)
    {
        RWTexture2D<uint64_t> occlusion = ResourceDescriptorHeap[HAIR_OCCLUSION];
        const uint64_t hidden = occlusion[pixel];
        hiddenBits = (uint)(hidden >> 32u);
        if ((frame.rasterFlags & HAIR_RASTER_FLAG_OCCLUDE_SATURATED) != 0u)
        {
            // The next frame's split depth: the nearest depth the hair in front of which saturates the pixel, searched
            // from frame to frame. Depths are measured from the pixel's nearest strand.
            RWTexture2D<uint64_t> hairVisibility = ResourceDescriptorHeap[HAIR_VISIBILITY];
            RWTexture2D<uint> coverageNear = ResourceDescriptorHeap[HAIR_COVERAGE];
            RWTexture2D<uint> coverageFar = ResourceDescriptorHeap[HAIR_COVERAGE_FAR];
            RWTexture2D<uint> splitGuess = ResourceDescriptorHeap[HAIR_SPLIT_GUESS];
            const uint64_t nearest = hairVisibility[pixel];
            const uint splitBits = (uint)hidden & HAIR_DEPTH_BITS_FAR;
            uint guess = HAIR_DEPTH_BITS_FAR;
            if (nearest != 0xFFFFFFFFFFFFFFFFull)
            {
                const float front = asfloat(HairVisKeyDepthBits(nearest) << 1u);
                const float nearUnits = (float)coverageNear[pixel];
                const float saturation = (float)frame.saturationUnits;
                // A guess that had to move further is held there: a pixel whose hair is a dense sheet would otherwise
                // go back and forth across it, saturated one frame and not the next. Now and then a pixel is let try
                // again, so that a hold does not outlive what it was made for.
                const bool retry = ((frame.frameNumber + pixel.x * 7u + pixel.y * 13u) & 63u) == 0u;
                const bool held = (splitGuess[pixel] & HAIR_SPLIT_GUESS_HOLD) != 0u && !retry;
                if (splitBits >= HAIR_DEPTH_BITS_FAR)
                {
                    // Everything was in front of the split. Saturated: a first guess close behind the nearest strand.
                    guess = nearUnits >= saturation ? HairDepthBits(front * 1.02f) : HAIR_DEPTH_BITS_FAR;
                }
                else
                {
                    const float span = max(asfloat(splitBits << 1u) - front, front * 0.002f);
                    if (nearUnits < saturation)
                    {
                        // Not saturated in front of the split: further, and no further than half the pixel's depth again.
                        const float grown = front + span * 1.5f;
                        guess = grown > front * 1.5f ? HAIR_DEPTH_BITS_FAR : (HairDepthBits(grown) | HAIR_SPLIT_GUESS_HOLD);
                    }
                    else if (nearUnits >= 2.0f * saturation && !held)
                    {
                        guess = HairDepthBits(front + span * 0.8f); // well past the saturation: nearer
                    }
                    else
                    {
                        guess = splitBits | (held ? HAIR_SPLIT_GUESS_HOLD : 0u);
                    }
                }
            }
            splitGuess[pixel] = guess;
        }
    }
    const uint waveMax = WaveActiveMax(hiddenBits);
    if (WaveIsFirstLane())
    {
        InterlockedMax(coarse[groupId.xy], waveMax);
    }
    if (groupIndex == 0u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
        InterlockedAdd(counters[HAIR_COUNTER_HAIR_TILES], 1u);
    }
}

// One thread per tile, after HairOcclusionUpdateCS: the first pass's guess for the next frame. A tile's hidden depths
// move from frame to frame even in a still view: the camera is jittered by a fraction of a pixel for the upscaler, and
// each jitter sees through different gaps between strands. A chunk one frame's depths hide the next frame's may show,
// and the late pass, the slower of the two, then draws it. So the guess is the largest tile depth of the last frames:
// it follows a depth that grows at once, and comes back down to a smaller one keeping HairFrameGPU::hintDecay of the
// difference a frame. A tile that hid nothing (HAIR_DEPTH_BITS_FAR) counts as four times this frame's depth, and so
// stays out of the guess for some frames.
[shader("compute")]
[numthreads(8, 8, 1)]
void HairOcclusionHintCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2D<uint> coarse = ResourceDescriptorHeap[HAIR_OCCLUSION_COARSE];
    RWTexture2D<uint> hint = ResourceDescriptorHeap[HAIR_OCCLUSION_HINT];
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    const float decay = frameState[0].hintDecay;
    const uint2 tile = dispatchThreadId.xy;
    if (tile.x >= (HAIR_VISIBILITY_WIDTH + HAIR_RASTER_TILE_SIZE - 1u) / HAIR_RASTER_TILE_SIZE ||
        tile.y >= (HAIR_VISIBILITY_HEIGHT + HAIR_RASTER_TILE_SIZE - 1u) / HAIR_RASTER_TILE_SIZE)
    {
        return;
    }
    const uint current = coarse[tile];
    uint next = HAIR_DEPTH_BITS_FAR;
    if (current < HAIR_DEPTH_BITS_FAR)
    {
        const float depth = asfloat(current << 1u);
        const uint previous = hint[tile];
        const float before = previous >= HAIR_DEPTH_BITS_FAR ? 4.0f * depth : asfloat(previous << 1u);
        // Not a number (a texel never written) fails the comparison and gives this frame's depth.
        next = before > depth ? min(HairDepthBits(depth + (before - depth) * decay), HAIR_DEPTH_BITS_FAR) : current;
    }
    hint[tile] = next;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairRasterItemsCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint index = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    if (index >= min(counters[HAIR_COUNTER_RASTER_ITEMS], frame.rasterItemCapacity))
    {
        return;
    }
    RWStructuredBuffer<uint> items = ResourceDescriptorHeap[HAIR_RASTER_ITEMS];
    const uint item = items[index];
    if (item == HAIR_INVALID_INDEX)
    {
        return;
    }
    const uint row = item & ((1u << HAIR_RASTER_ITEM_ROW_BITS) - 1u);
    HairRasterSegment segment;
    if (!HairLoadRasterSegment(row, frame, HairVisibleClusterCount(frame), segment))
    {
        return;
    }
    HairRasterCounts counts = (HairRasterCounts)0;
    HairRasterChunk(segment, item >> HAIR_RASTER_ITEM_ROW_BITS, frame, counts);
    HairFinishRaster(row, segment, frame, counts, false, 0u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairRasterLateCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    const HairFrameGPU frame = frameState[0];
    const uint row = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    if (row + 1u >= HairPoolVertexCount(frame))
    {
        return;
    }
    RWStructuredBuffer<uint> poolLate = ResourceDescriptorHeap[HAIR_POOL_LATE];
    const uint lateMask = poolLate[row];
    if (lateMask == 0u)
    {
        return;
    }
    HairRasterSegment segment;
    if (!HairLoadRasterSegment(row, frame, HairVisibleClusterCount(frame), segment))
    {
        return;
    }
    RWTexture2D<uint> coarse = ResourceDescriptorHeap[HAIR_OCCLUSION_COARSE];
    HairRasterCounts counts = (HairRasterCounts)0;
    uint drawn = 0u;
    const uint chunkCount = HairChunkCount(segment);
    for (uint chunk = 0u; chunk < chunkCount; ++chunk)
    {
        if ((lateMask & (1u << min(chunk, HAIR_RASTER_LATE_BITS - 1u))) == 0u)
        {
            continue;
        }
        if ((frame.rasterFlags & HAIR_RASTER_FLAG_SKIP_LATE) != 0u || HairChunkHidden(segment, HairBoundChunk(segment, chunk), coarse))
        {
            continue;
        }
        ++drawn;
        HairRasterChunk(segment, chunk, frame, counts);
    }
    HairFinishRaster(row, segment, frame, counts, true, drawn);
}
