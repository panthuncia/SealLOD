#ifndef VIRTUAL_SHADOW_CASTER_HLSLI
#define VIRTUAL_SHADOW_CASTER_HLSLI

// Public directional-VSM caster contract. Providers own their geometry,
// culling and command formats; this file owns layer selection and depth output.
static const uint kVirtualShadowCasterMobilityRigid = 0u;
static const uint kVirtualShadowCasterMobilitySkinnedOrDeformable = 1u;

bool VirtualShadowCasterUsesDynamicLayer(uint mobility, CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return mobility == kVirtualShadowCasterMobilitySkinnedOrDeformable &&
        (clipmapInfo.flags & kCLodVirtualShadowClipmapDynamicSkinnedFlag) != 0u;
}

bool VirtualShadowCasterActiveBlockAcceptsPage(
    uint2 virtualPageCoords,
    uint2 blockCoord,
    uint packedActiveRect)
{
    if (any(CLodVirtualShadowBlockCoordFromPageCoord(virtualPageCoords) != blockCoord))
    {
        return false;
    }
    const uint2 blockOrigin = blockCoord * kCLodVirtualShadowBlockPagesPerAxis;
    return CLodVirtualShadowBlockActiveRectContainsPage(
        packedActiveRect,
        virtualPageCoords - blockOrigin);
}

float4 VirtualShadowCasterBuildBlockClipDistances(
    float4 clipPosition,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    uint2 blockCoord,
    uint packedActiveRect)
{
    const uint2 blockOrigin = CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord);
    const uint2 rectMin = CLodVirtualShadowUnpackBlockActiveRectMin(packedActiveRect);
    const uint2 rectMax = CLodVirtualShadowUnpackBlockActiveRectMax(packedActiveRect);
    const float pageTableResolution = max((float)clipmapInfo.pageTableResolution, 1.0f);
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

bool VirtualShadowCasterWriteDepth(
    float2 shadowUv,
    float linearDepth,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    bool dynamicLayer,
    uint pageTableDescriptor,
    uint staticPagesDescriptor,
    uint dynamicPagesDescriptor)
{
    const uint2 virtualPageCoords = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(virtualPageCoords, clipmapInfo);
    const uint3 pageCoords = uint3(wrappedPageCoords, clipmapInfo.pageTableLayer);
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[pageTableDescriptor];
    const uint pageEntry = pageTable[pageCoords];
    if (!CLodVirtualShadowPageEntryCanRasterLayer(pageEntry, dynamicLayer))
    {
        return false;
    }

    const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
    const uint2 virtualTexelCoords = CLodVirtualShadowVirtualTexelCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 atlasPixel = CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapInfo);
    RWTexture2D<uint> physicalPages = ResourceDescriptorHeap[
        dynamicLayer ? dynamicPagesDescriptor : staticPagesDescriptor];
    const float pageSpaceLinearDepth = dynamicLayer
        ? CLodVirtualShadowDepthToCachedPageSpace(
            linearDepth,
            physicalPageIndex,
            clipmapInfo.shadowCameraBufferIndex)
        : linearDepth;
    if (!isfinite(pageSpaceLinearDepth) || pageSpaceLinearDepth <= 0.0f)
    {
        return false;
    }

    InterlockedMin(physicalPages[atlasPixel], CLodVirtualShadowEncodeDepth(pageSpaceLinearDepth, clipmapInfo));

    const uint packedPageCoords =
        (pageCoords.x & 0xFFFu) |
        ((pageCoords.y & 0xFFFu) << 12u) |
        ((pageCoords.z & 0xFFu) << 24u);
    const uint4 pageMatchMask = WaveMatch(packedPageCoords);
    uint pageLeaderLane = pageMatchMask.x != 0u ? firstbitlow(pageMatchMask.x) :
        pageMatchMask.y != 0u ? 32u + firstbitlow(pageMatchMask.y) :
        pageMatchMask.z != 0u ? 64u + firstbitlow(pageMatchMask.z) :
        96u + firstbitlow(pageMatchMask.w);
    if (WaveGetLaneIndex() == pageLeaderLane)
    {
        uint ignored = 0u;
        InterlockedOr(
            pageTable[pageCoords],
            dynamicLayer
                ? kCLodVirtualShadowDynamicContentMask
                : kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask,
            ignored);
    }
    return true;
}

#endif
