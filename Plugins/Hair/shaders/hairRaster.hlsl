// Compute rasterizer for strand hair: one thread per row of the strand pool. A row that starts a segment draws it into
// the hair visibility target as a screen-space trapezoid with round ends, at least one pixel wide. Every covered pixel is
// tested against the frame's opaque visibility buffer and written with InterlockedMin, so the nearest segment wins.
//
// The simple form only: no tiling, no binning, no coverage and no anti-aliasing. A segment tests at most
// HAIR_RASTER_MAX_PIXELS_PER_SEGMENT pixels.

#include "HairShaders/hairCommon.hlsli"

// HairRasterPass::Record. The camera and the opaque visibility buffer are bound by name.
#define HAIR_FRAME_STATE UintRootConstant0       // StructuredBuffer<HairFrameGPU>
#define HAIR_COUNTERS UintRootConstant1          // RWStructuredBuffer<uint>
#define HAIR_VISIBLE_CLUSTERS UintRootConstant2  // StructuredBuffer<HairVisibleCluster>
#define HAIR_POOL_VERTICES UintRootConstant3     // StructuredBuffer<HairPoolVertex>
#define HAIR_POOL_SEGMENT_IDS UintRootConstant4  // StructuredBuffer<uint>
#define HAIR_VISIBILITY UintRootConstant5        // RWTexture2D<uint64_t>
#define HAIR_VISIBILITY_WIDTH UintRootConstant6
#define HAIR_VISIBILITY_HEIGHT UintRootConstant7
#define HAIR_GROUPS_PER_ROW UintRootConstant8    // the dispatch is this many groups wide: rows keep it under 65536

[shader("compute")]
[numthreads(64, 1, 1)]
void HairRasterCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint row = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    const uint poolVertexCount = min(counters[HAIR_COUNTER_POOL_VERTICES], frame.poolVertexCapacity);
    if (row + 1u >= poolVertexCount)
    {
        return;
    }
    StructuredBuffer<uint> poolSegmentIds = ResourceDescriptorHeap[HAIR_POOL_SEGMENT_IDS];
    const uint segmentId = poolSegmentIds[row];
    if (segmentId == HAIR_INVALID_INDEX)
    {
        return;
    }

    // A dropped cluster leaves rows of an earlier frame in its range. A row is this frame's when the visible cluster
    // its id names owns it.
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    const uint visibleIndex = segmentId >> HAIR_VIS_CLUSTER_SHIFT;
    if (visibleIndex >= min(counters[HAIR_COUNTER_CLUSTERS_VISIBLE], frame.visibleClusterCapacity))
    {
        return;
    }
    const HairVisibleCluster visible = visibleClusters[visibleIndex];
    const uint clusterVertexCount = visible.strandCount * ((1u << visible.vertexLevel) + 1u);
    if (row < visible.poolVertexBase || row >= visible.poolVertexBase + clusterVertexCount)
    {
        return;
    }

    StructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_POOL_VERTICES];
    const HairPoolVertex start = poolVertices[row];
    const HairPoolVertex end = poolVertices[row + 1u];
    const Camera camera = HairLoadMainCamera();

    // The main view is a perspective view: clip w is the linear view depth.
    float4 clip0 = mul(float4(start.position, 1.0f), camera.viewProjection);
    float4 clip1 = mul(float4(end.position, 1.0f), camera.viewProjection);
    float width0 = start.width;
    float width1 = end.width;
    if (clip0.w < camera.zNear && clip1.w < camera.zNear)
    {
        return;
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

    // Screen positions as the opaque rasterizers place them, and half-widths of at least half a pixel.
    const float2 size = float2((float)HAIR_VISIBILITY_WIDTH, (float)HAIR_VISIBILITY_HEIGHT);
    const float inverseW0 = 1.0f / clip0.w;
    const float inverseW1 = 1.0f / clip1.w;
    const float2 screen0 = float2(clip0.x * inverseW0 * 0.5f + 0.5f, 0.5f - clip0.y * inverseW0 * 0.5f) * size;
    const float2 screen1 = float2(clip1.x * inverseW1 * 0.5f + 0.5f, 0.5f - clip1.y * inverseW1 * 0.5f) * size;
    const float halfPixelsPerMetre = 0.5f * HairPixelsPerMetreAtUnitDepth(camera, size.y);
    const float halfWidth0 = max(width0 * halfPixelsPerMetre * inverseW0, 0.5f);
    const float halfWidth1 = max(width1 * halfPixelsPerMetre * inverseW1, 0.5f);
    const float maxHalfWidth = max(halfWidth0, halfWidth1);
    const float2 boundsMin = min(screen0, screen1) - maxHalfWidth;
    const float2 boundsMax = max(screen0, screen1) + maxHalfWidth;
    // Written so a position that is not a number is rejected too.
    if (!(boundsMax.x >= 0.0f && boundsMax.y >= 0.0f && boundsMin.x < size.x && boundsMin.y < size.y))
    {
        return;
    }

    // Walk the pixels along the segment's longer screen axis ("major"). In each column of that walk only the pixels
    // near the centreline are tested, so a long diagonal segment costs its own area, not its bounding box's.
    const float2 delta = screen1 - screen0;
    const bool steep = abs(delta.y) > abs(delta.x);
    const float major0 = steep ? screen0.y : screen0.x;
    const float major1 = steep ? screen1.y : screen1.x;
    const float minor0 = steep ? screen0.x : screen0.y;
    const float minor1 = steep ? screen1.x : screen1.y;
    const float majorLast = (steep ? size.y : size.x) - 1.0f;
    const float minorLast = (steep ? size.x : size.y) - 1.0f;
    const float majorDelta = major1 - major0;
    const float inverseMajorDelta = abs(majorDelta) > 1.0e-6f ? 1.0f / majorDelta : 0.0f;
    const float lengthSquared = dot(delta, delta);
    const float inverseLengthSquared = lengthSquared > 1.0e-12f ? 1.0f / lengthSquared : 0.0f;
    // The segment is at most 45 degrees off the major axis, so its edge is within sqrt(2) half-widths of the
    // centreline along the minor axis; the half pixel reaches the pixel centres.
    const float minorReach = maxHalfWidth * 1.4143f + 0.5f;
    const int majorFirst = (int)clamp(floor(min(major0, major1) - maxHalfWidth), 0.0f, majorLast);
    const int majorEnd = (int)clamp(floor(max(major0, major1) + maxHalfWidth), 0.0f, majorLast);

    Texture2D<uint64_t> opaqueVisibility =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    RWTexture2D<uint64_t> hairVisibility = ResourceDescriptorHeap[HAIR_VISIBILITY];

    uint tested = 0u;
    uint written = 0u;
    uint occluded = 0u;
    bool capped = false;
    for (int major = majorFirst; major <= majorEnd && !capped; ++major)
    {
        const float majorCenter = (float)major + 0.5f;
        const float minorCenter = lerp(minor0, minor1, saturate((majorCenter - major0) * inverseMajorDelta));
        const int minorFirst = (int)clamp(floor(minorCenter - minorReach), 0.0f, minorLast);
        const int minorEnd = (int)clamp(floor(minorCenter + minorReach), 0.0f, minorLast);
        for (int minor = minorFirst; minor <= minorEnd; ++minor)
        {
            if (tested >= HAIR_RASTER_MAX_PIXELS_PER_SEGMENT)
            {
                capped = true;
                break;
            }
            ++tested;
            const int2 pixel = steep ? int2(minor, major) : int2(major, minor);
            const float2 pixelCenter = float2(pixel) + 0.5f;
            // Covered when the pixel centre is within the segment's half-width of its nearest point on the centreline.
            const float t = saturate(dot(pixelCenter - screen0, delta) * inverseLengthSquared);
            const float2 offset = pixelCenter - (screen0 + delta * t);
            const float halfWidth = lerp(halfWidth0, halfWidth1, t);
            if (dot(offset, offset) > halfWidth * halfWidth)
            {
                continue;
            }
            // 1 / w is linear on screen.
            const uint depthBits = HairDepthBits(1.0f / lerp(inverseW0, inverseW1, t));
            if (depthBits >= HairVisKeyDepthBits(opaqueVisibility[pixel]))
            {
                ++occluded;
                continue;
            }
            InterlockedMin(hairVisibility[pixel], HairPackVisKey(depthBits, segmentId));
            ++written;
        }
    }

    if (written != 0u)
    {
        InterlockedAdd(counters[HAIR_COUNTER_SEGMENTS_DRAWN], 1u);
        InterlockedAdd(counters[HAIR_COUNTER_PIXELS_WRITTEN], written);
    }
    if (occluded != 0u)
    {
        InterlockedAdd(counters[HAIR_COUNTER_PIXELS_OCCLUDED], occluded);
    }
    if (capped)
    {
        InterlockedAdd(counters[HAIR_COUNTER_SEGMENTS_CAPPED], 1u);
    }
}
