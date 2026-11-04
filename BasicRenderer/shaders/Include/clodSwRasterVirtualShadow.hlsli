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

bool SWRasterWriteVirtualShadow(uint2 pixel, uint viewID, float linearDepth, bool dynamicLayer)
{
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_SW_VSM_CLIPMAP_INFO_DESCRIPTOR_INDEX];

    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    if (!CLodVirtualShadowTryGetClipmapInfoForView(viewID, clipmapInfos, clipmapInfo))
    {
        return false;
    }

    const float2 shadowUv = saturate((float2(pixel) + 0.5f) / max((float)clipmapInfo.virtualResolution, 1.0f));
    const uint2 virtualPageCoords = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(virtualPageCoords, clipmapInfo);

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_SW_VSM_PAGE_TABLE_DESCRIPTOR_INDEX];
    const uint3 pageCoords = uint3(wrappedPageCoords, clipmapInfo.pageTableLayer);
    const uint pageEntry = pageTable[pageCoords];
    if (!CLodVirtualShadowPageEntryCanRasterLayer(pageEntry, dynamicLayer))
    {
        return false;
    }

    const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
    const uint2 virtualTexelCoords = CLodVirtualShadowVirtualTexelCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 atlasPixel = CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapInfo);

    RWTexture2D<uint> physicalPages = ResourceDescriptorHeap[
        dynamicLayer
            ? CLOD_SW_VSM_DYNAMIC_PAGES_DESCRIPTOR_INDEX
            : CLOD_SW_VSM_STATIC_PAGES_DESCRIPTOR_INDEX];
    const float pageSpaceLinearDepth = dynamicLayer
        ? CLodVirtualShadowDepthToCachedPageSpace(
            linearDepth,
            physicalPageIndex,
            clipmapInfo.shadowCameraBufferIndex)
        : linearDepth;
    InterlockedMin(
        physicalPages[atlasPixel],
        CLodVirtualShadowEncodeDepth(pageSpaceLinearDepth, clipmapInfo));
    uint ignored = 0u;
    InterlockedOr(
        pageTable[pageCoords],
        dynamicLayer
            ? kCLodVirtualShadowDynamicContentMask
            : kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask,
        ignored);
    return true;
}

#endif // CLOD_SW_RASTER_VIRTUAL_SHADOW_HLSLI
