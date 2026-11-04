#ifndef CLOD_RASTER_VIRTUAL_SHADOW_HLSLI
#define CLOD_RASTER_VIRTUAL_SHADOW_HLSLI

// Virtual shadow helpers for the CLod mesh shader: clipping a cluster to its page block, and
// dropping triangles that touch no page that needs rendering.
// Requires clodRasterizationRootConstants.h.

#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/visibleClusterPacking.hlsli"

float4 CLodVirtualShadowBuildBlockClipDistances(float4 clipPosition, uint shadowVsmPayload)
{
    if (!CLodVisibleClusterHasVsmBlockDataFromPayload(shadowVsmPayload))
    {
        return clipPosition.w.xxxx;
    }

    const uint2 blockOrigin = CLodVisibleClusterVsmBlockOriginPageCoordFromPayload(shadowVsmPayload);
    const uint packedActiveRect = CLodVisibleClusterVsmActiveRectFromPayload(shadowVsmPayload);
    const uint2 rectMin = CLodVirtualShadowUnpackBlockActiveRectMin(packedActiveRect);
    const uint2 rectMax = CLodVirtualShadowUnpackBlockActiveRectMax(packedActiveRect);
    const float pageTableResolution = max((float)CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_RESOLUTION, 1.0f);
    const float2 uvMin = float2(blockOrigin + rectMin) / pageTableResolution;
    const float2 uvMax = float2(blockOrigin + rectMax + 1u) / pageTableResolution;
    const float ndcMinX = uvMin.x * 2.0f - 1.0f;
    const float ndcMaxX = uvMax.x * 2.0f - 1.0f;
    const float ndcMaxY = 1.0f - uvMin.y * 2.0f;
    const float ndcMinY = 1.0f - uvMax.y * 2.0f;
    return float4(
        clipPosition.x - ndcMinX * clipPosition.w,
        ndcMaxX * clipPosition.w - clipPosition.x,
        ndcMaxY * clipPosition.w - clipPosition.y,
        clipPosition.y - ndcMinY * clipPosition.w);
}

bool CLodTriangleTouchesRenderableVirtualShadowPages(
    float4 p0,
    float4 p1,
    float4 p2,
    ClodViewRasterInfo rasterInfo,
    uint shadowVsmPayload,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable)
{
    // Keep triangles that straddle the near plane rather than risking false rejects from a bbox
    // built from invalid post-divide coordinates.
    if (p0.w <= 0.0f || p1.w <= 0.0f || p2.w <= 0.0f)
    {
        return true;
    }

    const float visWidth = float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX);
    const float visHeight = float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY);
    const float scissorMinX = float(rasterInfo.scissorMinX);
    const float scissorMinY = float(rasterInfo.scissorMinY);

    const float2 ndc0 = p0.xy / p0.w;
    const float2 ndc1 = p1.xy / p1.w;
    const float2 ndc2 = p2.xy / p2.w;

    const float2 s0 = float2(
        (ndc0.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc0.y) * 0.5f * visHeight + scissorMinY);
    const float2 s1 = float2(
        (ndc1.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc1.y) * 0.5f * visHeight + scissorMinY);
    const float2 s2 = float2(
        (ndc2.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc2.y) * 0.5f * visHeight + scissorMinY);

    int2 minPx = int2(floor(min(min(s0, s1), s2)));
    int2 maxPx = int2(floor(max(max(s0, s1), s2)));

    minPx = max(minPx, int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(clipmapInfo.virtualResolution) - 1, int(clipmapInfo.virtualResolution) - 1));
    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
        return false;
    }

    uint2 minPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(minPx), clipmapInfo);
    uint2 maxPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(maxPx), clipmapInfo);

    if (CLodVisibleClusterHasVsmBlockDataFromPayload(shadowVsmPayload))
    {
        const uint2 blockCoord = CLodVisibleClusterVsmBlockCoordFromPayload(shadowVsmPayload);
        const uint2 minTouchedBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(minPageCoords);
        const uint2 maxTouchedBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(maxPageCoords);
        if (any(blockCoord < minTouchedBlockCoord) || any(blockCoord > maxTouchedBlockCoord))
        {
            return false;
        }

        const uint packedActiveRect = CLodVisibleClusterVsmActiveRectFromPayload(shadowVsmPayload);
        const uint2 blockOriginPageCoord = CLodVisibleClusterVsmBlockOriginPageCoordFromPayload(shadowVsmPayload);
        const uint2 blockMaxPageCoord = blockOriginPageCoord + uint2(kCLodVirtualShadowBlockPagesPerAxis - 1u, kCLodVirtualShadowBlockPagesPerAxis - 1u);
        const uint2 activeMinPageCoord = blockOriginPageCoord + CLodVirtualShadowUnpackBlockActiveRectMin(packedActiveRect);
        const uint2 activeMaxPageCoord = blockOriginPageCoord + CLodVirtualShadowUnpackBlockActiveRectMax(packedActiveRect);

        if (minPageCoords.x > blockMaxPageCoord.x || minPageCoords.y > blockMaxPageCoord.y ||
            maxPageCoords.x < blockOriginPageCoord.x || maxPageCoords.y < blockOriginPageCoord.y)
        {
            return false;
        }

        if (minPageCoords.x > activeMaxPageCoord.x || minPageCoords.y > activeMaxPageCoord.y ||
            maxPageCoords.x < activeMinPageCoord.x || maxPageCoords.y < activeMinPageCoord.y)
        {
            return false;
        }

        minPageCoords = max(minPageCoords, activeMinPageCoord);
        maxPageCoords = min(maxPageCoords, activeMaxPageCoord);
    }

    return CLodVirtualShadowAnyRenderablePageInPageRect(
        minPageCoords,
        maxPageCoords,
        clipmapInfo,
#if defined(PSO_SKINNED)
        true,
#else
        false,
#endif
        pageTable);
}

#endif // CLOD_RASTER_VIRTUAL_SHADOW_HLSLI
