#ifndef HAIR_HIT_HLSLI
#define HAIR_HIT_HLSLI

// The point of a strand a pixel shows, found again from the pixel's visibility key as the rasterizer placed the segment
// (hairRaster.hlsl). The resolve, the composite and the debug overlay share it.
//
// The including file defines, before including this one: HAIR_VISIBLE_CLUSTERS (StructuredBuffer<HairVisibleCluster>),
// HAIR_POOL_VERTICES (StructuredBuffer<HairPoolVertex>), HAIR_VISIBILITY_WIDTH and HAIR_VISIBILITY_HEIGHT.

#include "HairShaders/hairCommon.hlsli"

struct HairHit
{
    HairVisibleCluster cluster;
    uint strandIndex;
    uint segmentIndex;
    uint poolRow;           // the segment's first vertex in the strand pool; its second is the next row
    float parameter;        // along the segment, 0 at its first vertex; perspective correct
    float3 position;        // world space, on the centreline
    float3 previousPosition;
    float3 tangent;         // unit, toward the tip
    float width;            // diameter, metres
    float across;           // where the pixel is across the strand: -1 and 1 at its edges, 0 on the centreline and for a
                            // strand thinner than a pixel
    float3 side;            // unit, world space: the direction `across` grows in
    float coverage;         // the share of this pixel the strand covers, exactly as the rasterizer counted it: its true
                            // width over the footprint's, which is at least a pixel
};

HairHit HairReconstructHit(uint2 pixel, uint64_t key, Camera camera)
{
    HairHit hit;
    uint visibleIndex;
    HairUnpackSegmentId((uint)key, visibleIndex, hit.strandIndex, hit.segmentIndex);
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    StructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_POOL_VERTICES];
    hit.cluster = visibleClusters[visibleIndex];
    const uint row = hit.cluster.poolVertexBase + hit.strandIndex * ((1u << hit.cluster.vertexLevel) + 1u) + hit.segmentIndex;
    hit.poolRow = row;
    HairPoolVertex start = poolVertices[row];
    HairPoolVertex end = poolVertices[row + 1u];

    float4 clip0 = mul(float4(start.position, 1.0f), camera.viewProjection);
    float4 clip1 = mul(float4(end.position, 1.0f), camera.viewProjection);
    // A segment that crosses the near plane was drawn from where it crosses.
    float clipped0 = 0.0f;
    float clipped1 = 1.0f;
    if (clip0.w < camera.zNear)
    {
        clipped0 = (camera.zNear - clip0.w) / (clip1.w - clip0.w);
        clip0 = lerp(clip0, clip1, clipped0);
    }
    else if (clip1.w < camera.zNear)
    {
        clipped1 = 1.0f - (camera.zNear - clip1.w) / (clip0.w - clip1.w);
        clip1 = lerp(clip0, clip1, clipped1);
    }

    const float2 size = float2((float)HAIR_VISIBILITY_WIDTH, (float)HAIR_VISIBILITY_HEIGHT);
    const float inverseW0 = 1.0f / clip0.w;
    const float inverseW1 = 1.0f / clip1.w;
    const float2 screen0 = float2(clip0.x * inverseW0 * 0.5f + 0.5f, 0.5f - clip0.y * inverseW0 * 0.5f) * size;
    const float2 screen1 = float2(clip1.x * inverseW1 * 0.5f + 0.5f, 0.5f - clip1.y * inverseW1 * 0.5f) * size;
    const float2 delta = screen1 - screen0;
    const float lengthSquared = dot(delta, delta);
    const float2 pixelCenter = float2(pixel) + 0.5f;
    const float screenParameter = lengthSquared > 1.0e-12f ? saturate(dot(pixelCenter - screen0, delta) / lengthSquared) : 0.0f;
    // Linear on screen to linear in space, then back onto the unclipped segment.
    const float clippedParameter = screenParameter * inverseW1 / lerp(inverseW0, inverseW1, screenParameter);
    hit.parameter = lerp(clipped0, clipped1, clippedParameter);

    hit.position = lerp(start.position, end.position, hit.parameter);
    hit.previousPosition = lerp(start.previousPosition, end.previousPosition, hit.parameter);
    hit.width = lerp(start.width, end.width, hit.parameter);
    const float3 along = end.position - start.position;
    hit.tangent = dot(along, along) > 1.0e-20f ? normalize(along) : float3(0.0f, 1.0f, 0.0f);

    // Across the strand: the pixel's offset from the centreline over the strand's half-width on screen. A strand under a
    // pixel wide was drawn a pixel wide, and the offset says nothing about where on it the pixel is.
    const float2 screenSide = lengthSquared > 1.0e-12f ? float2(-delta.y, delta.x) * rsqrt(lengthSquared) : float2(1.0f, 0.0f);
    const float halfPixelsPerMetre = 0.5f * HairPixelsPerMetreAtUnitDepth(camera, size.y);
    const float halfWidthPixels = hit.width * halfPixelsPerMetre * lerp(inverseW0, inverseW1, screenParameter);
    const float offset = dot(pixelCenter - (screen0 + delta * screenParameter), screenSide);
    hit.across = clamp(offset / max(halfWidthPixels, 0.5f), -1.0f, 1.0f) * saturate(halfWidthPixels * 2.0f - 1.0f);
    // The world direction that screenSide is the image of: across the strand, facing neither toward nor away.
    const float3 toCamera = normalize(camera.positionWorldSpace.xyz - hit.position);
    const float3 sideways = cross(hit.tangent, toCamera);
    hit.side = dot(sideways, sideways) > 1.0e-12f ? normalize(sideways) : float3(1.0f, 0.0f, 0.0f);
    const float4 sideClip = mul(float4(hit.side, 0.0f), camera.viewProjection);
    if (dot(float2(sideClip.x, -sideClip.y), screenSide) < 0.0f)
    {
        hit.side = -hit.side;
    }

    // The rasterizer's coverage, with its arithmetic: half-widths of the (clipped) ends on screen, each at least half a
    // pixel for the footprint, both interpolated linearly on screen.
    const float trueHalfWidth0 = lerp(start.width, end.width, clipped0) * halfPixelsPerMetre * inverseW0;
    const float trueHalfWidth1 = lerp(start.width, end.width, clipped1) * halfPixelsPerMetre * inverseW1;
    const float footprintHalfWidth = lerp(max(trueHalfWidth0, 0.5f), max(trueHalfWidth1, 0.5f), screenParameter);
    hit.coverage = saturate(lerp(trueHalfWidth0, trueHalfWidth1, screenParameter) / footprintHalfWidth);
    return hit;
}

#endif // HAIR_HIT_HLSLI
