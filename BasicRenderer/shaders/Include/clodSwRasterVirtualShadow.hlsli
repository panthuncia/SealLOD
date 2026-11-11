#ifndef CLOD_SW_RASTER_VIRTUAL_SHADOW_HLSLI
#define CLOD_SW_RASTER_VIRTUAL_SHADOW_HLSLI

// Virtual shadow output for the software rasterizer. Requires clodVirtualShadowClipmap.hlsli,
// clodVirtualShadowDepth.hlsli and the raster or work-graph root constants.

#if defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
#define CLOD_SW_VSM_CLIPMAP_INFO_DESCRIPTOR_INDEX ResourceDescriptorIndex(Builtin::Shadows::CLodClipmapInfo)
#define CLOD_SW_VSM_PAGE_TABLE_DESCRIPTOR_INDEX CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX
#define CLOD_SW_VSM_STATIC_PAGES_DESCRIPTOR_INDEX CLOD_WG_VIRTUAL_SHADOW_PHYSICAL_PAGES_UAV_DESCRIPTOR_INDEX
#define CLOD_SW_VSM_DYNAMIC_PAGES_DESCRIPTOR_INDEX CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_PAGES_UAV_DESCRIPTOR_INDEX
#else
#define CLOD_SW_VSM_CLIPMAP_INFO_DESCRIPTOR_INDEX CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX
#define CLOD_SW_VSM_PAGE_TABLE_DESCRIPTOR_INDEX CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX
#define CLOD_SW_VSM_STATIC_PAGES_DESCRIPTOR_INDEX CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX
#define CLOD_SW_VSM_DYNAMIC_PAGES_DESCRIPTOR_INDEX CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX
#endif

// The slice a view renders: one load of its view row.
bool SWFindVirtualShadowClipmapInfo(
    uint viewID,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    out CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return CLodVirtualShadowTryGetClipmapInfoForView(viewID, clipmapInfos, clipmapInfo);
}

// A cluster's slice, resolved once per lane by the caller rather than per texel.
struct SWVirtualShadowTarget
{
    CLodVirtualShadowClipmapInfo clipmapInfo;
    // Power-of-two layouts (the fixed 128-page table of 128-texel pages, a power-of-two atlas)
    // address texels with shifts and masks. The general path's float UVs and integer divisions
    // give the same coordinates there, and stay for any other layout.
    bool powerOfTwo;
    uint2 pageWrapOffset;
    uint pageTableMask;
    uint atlasPagesWideShift;
};

SWVirtualShadowTarget SWBuildVirtualShadowTarget(CLodVirtualShadowClipmapInfo clipmapInfo)
{
    SWVirtualShadowTarget target;
    target.clipmapInfo = clipmapInfo;
    const uint pageTableResolution = clipmapInfo.pageTableResolution;
    const uint atlasPagesWide = clipmapInfo.physicalAtlasPagesWide;
    target.powerOfTwo =
        pageTableResolution != 0u && (pageTableResolution & (pageTableResolution - 1u)) == 0u &&
        clipmapInfo.virtualResolution == pageTableResolution * kCLodVirtualShadowPhysicalPageSize &&
        atlasPagesWide != 0u && (atlasPagesWide & (atlasPagesWide - 1u)) == 0u;
    target.pageTableMask = pageTableResolution - 1u;
    target.pageWrapOffset = uint2(clipmapInfo.pageOffsetX, clipmapInfo.pageOffsetY);
    target.atlasPagesWideShift = firstbitlow(atlasPagesWide);
    return target;
}

// A triangle's page, when its texel rectangle lies in one page: the per-triangle test has read the
// entry, so its texels need no page table load of their own.
struct SWVirtualShadowTrianglePage
{
    bool single;
    uint3 pageCoords;
    uint pageEntry;
};

uint2 SWVirtualShadowWrappedPageOfTexel(uint2 virtualTexelCoords, SWVirtualShadowTarget target)
{
    return ((virtualTexelCoords / kCLodVirtualShadowPhysicalPageSize) - target.pageWrapOffset) & target.pageTableMask;
}

// Whether any page under the texel rectangle can be rastered, and the page if there is only one.
bool SWVirtualShadowResolveTrianglePages(
    uint2 minPixel,
    uint2 maxPixel,
    SWVirtualShadowTarget target,
    bool dynamicLayer,
    RWTexture2DArray<uint> pageTable,
    out SWVirtualShadowTrianglePage trianglePage)
{
    trianglePage = (SWVirtualShadowTrianglePage)0;
    const CLodVirtualShadowClipmapInfo clipmapInfo = target.clipmapInfo;
    if (target.powerOfTwo)
    {
        const uint2 minTexel = min(minPixel, clipmapInfo.virtualResolution - 1u);
        const uint2 maxTexel = min(maxPixel, clipmapInfo.virtualResolution - 1u);
        if (all((minTexel / kCLodVirtualShadowPhysicalPageSize) == (maxTexel / kCLodVirtualShadowPhysicalPageSize)))
        {
            const uint3 pageCoords = uint3(SWVirtualShadowWrappedPageOfTexel(minTexel, target), clipmapInfo.pageTableLayer);
            const uint pageEntry = pageTable[pageCoords];
            trianglePage.single = true;
            trianglePage.pageCoords = pageCoords;
            trianglePage.pageEntry = pageEntry;
            return CLodVirtualShadowPageEntryCanRasterLayer(pageEntry, dynamicLayer);
        }
    }
    return CLodVirtualShadowAnyRenderablePageInPixelRect(minPixel, maxPixel, clipmapInfo, dynamicLayer, pageTable);
}

// A covered texel's place in the physical atlas.
struct SWVirtualShadowTexel
{
    uint3 pageCoords;
    uint pageEntry;
    uint physicalPageIndex;
    uint2 atlasPixel;
};

// False when the texel's page cannot be rastered this frame.
bool SWResolveVirtualShadowTexel(
    uint2 pixel,
    SWVirtualShadowTarget target,
    SWVirtualShadowTrianglePage trianglePage,
    bool dynamicLayer,
    out SWVirtualShadowTexel texel)
{
    texel = (SWVirtualShadowTexel)0;
    const CLodVirtualShadowClipmapInfo clipmapInfo = target.clipmapInfo;
    uint2 virtualTexelCoords;
    if (trianglePage.single)
    {
        // Single-page triangles are only resolved on the power-of-two path.
        virtualTexelCoords = min(pixel, clipmapInfo.virtualResolution - 1u);
        texel.pageCoords = trianglePage.pageCoords;
        texel.pageEntry = trianglePage.pageEntry;
    }
    else
    {
        uint2 wrappedPageCoords;
        if (target.powerOfTwo)
        {
            virtualTexelCoords = min(pixel, clipmapInfo.virtualResolution - 1u);
            wrappedPageCoords = SWVirtualShadowWrappedPageOfTexel(virtualTexelCoords, target);
        }
        else
        {
            const float2 shadowUv = saturate((float2(pixel) + 0.5f) / max((float)clipmapInfo.virtualResolution, 1.0f));
            wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(
                CLodVirtualShadowVirtualPageCoordsFromUv(shadowUv, clipmapInfo), clipmapInfo);
            virtualTexelCoords = CLodVirtualShadowVirtualTexelCoordsFromUv(shadowUv, clipmapInfo);
        }
        RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_SW_VSM_PAGE_TABLE_DESCRIPTOR_INDEX];
        texel.pageCoords = uint3(wrappedPageCoords, clipmapInfo.pageTableLayer);
        texel.pageEntry = pageTable[texel.pageCoords];
    }
    if (!CLodVirtualShadowPageEntryCanRasterLayer(texel.pageEntry, dynamicLayer))
    {
        return false;
    }

    texel.physicalPageIndex = texel.pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
    texel.atlasPixel = target.powerOfTwo
        ? uint2(
            (texel.physicalPageIndex & (clipmapInfo.physicalAtlasPagesWide - 1u)) * kCLodVirtualShadowPhysicalPageSize +
                (virtualTexelCoords.x % kCLodVirtualShadowPhysicalPageSize),
            (texel.physicalPageIndex >> target.atlasPagesWideShift) * kCLodVirtualShadowPhysicalPageSize +
                (virtualTexelCoords.y % kCLodVirtualShadowPhysicalPageSize))
        : CLodVirtualShadowPhysicalAtlasPixel(texel.physicalPageIndex, virtualTexelCoords, clipmapInfo);
    return true;
}

RWTexture2D<uint> SWVirtualShadowPhysicalPages(bool dynamicLayer)
{
    return ResourceDescriptorHeap[
        dynamicLayer
            ? CLOD_SW_VSM_DYNAMIC_PAGES_DESCRIPTOR_INDEX
            : CLOD_SW_VSM_STATIC_PAGES_DESCRIPTOR_INDEX];
}

uint SWEncodeVirtualShadowTexelDepth(
    SWVirtualShadowTexel texel,
    SWVirtualShadowTarget target,
    float linearDepth,
    bool dynamicLayer)
{
    const float pageSpaceLinearDepth = dynamicLayer
        ? CLodVirtualShadowDepthToCachedPageSpace(
            linearDepth,
            texel.physicalPageIndex,
            target.clipmapInfo.shadowCameraBufferIndex)
        : linearDepth;
    return CLodVirtualShadowEncodeDepth(pageSpaceLinearDepth, target.clipmapInfo);
}

// Whether a texel already holds depth at or nearer than encodedDepth. Depths only decrease during
// the raster, so a stale read is never nearer than the truth: a true result is always right, and a
// false one only costs the work it would have saved.
bool SWVirtualShadowTexelOccluded(SWVirtualShadowTexel texel, uint encodedDepth, bool dynamicLayer)
{
    return SWVirtualShadowPhysicalPages(dynamicLayer)[texel.atlasPixel] <= encodedDepth;
}

void SWCommitVirtualShadowTexel(SWVirtualShadowTexel texel, uint encodedDepth, bool dynamicLayer)
{
    InterlockedMin(SWVirtualShadowPhysicalPages(dynamicLayer)[texel.atlasPixel], encodedDepth);
    // The bits are only ever set, so an entry that already has them needs no atomic. A stale read
    // costs one redundant OR; without the test every covered texel was an atomic on its page entry.
    const uint contentMask = dynamicLayer
        ? kCLodVirtualShadowDynamicContentMask
        : kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask;
    if ((texel.pageEntry & contentMask) != contentMask)
    {
        RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_SW_VSM_PAGE_TABLE_DESCRIPTOR_INDEX];
        uint ignored = 0u;
        InterlockedOr(pageTable[texel.pageCoords], contentMask, ignored);
    }
}

#endif // CLOD_SW_RASTER_VIRTUAL_SHADOW_HLSLI
