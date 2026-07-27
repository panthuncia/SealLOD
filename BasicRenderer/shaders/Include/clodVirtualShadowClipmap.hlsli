#ifndef __CLOD_VIRTUAL_SHADOW_CLIPMAP_HLSLI__
#define __CLOD_VIRTUAL_SHADOW_CLIPMAP_HLSLI__

static const uint kCLodVirtualShadowClipmapValidFlag = 0x1u;
static const uint kCLodVirtualShadowClipmapInvalidateFlag = 0x2u;
static const uint kCLodVirtualShadowAllocatedMask = 0x80000000u;
static const uint kCLodVirtualShadowDirtyMask = 0x40000000u;
static const uint kCLodVirtualShadowContentValidMask = 0x20000000u;
static const uint kCLodVirtualShadowVisitedMask = 0x10000000u;
static const uint kCLodVirtualShadowRerenderedThisFrameMask = 0x08000000u;
static const uint kCLodVirtualShadowAdmittedThisFrameMask = 0x04000000u;
static const uint kCLodVirtualShadowUpgradePendingMask = 0x02000000u;
static const uint kCLodVirtualShadowPhysicalPageIndexMask = 0x01FFFFFFu;
static const uint kCLodVirtualShadowPhysicalPageResidentFlag = 0x1u;
static const uint kCLodVirtualShadowPhysicalPageAllocationGenerationShift = 1u;
static const uint kCLodVirtualShadowPhysicalPageAllocationGenerationMask = 0xFFFFFFFEu;
static const uint kCLodVirtualShadowClearEpochPendingMask = 0x80000000u;

uint CLodVirtualShadowPhysicalPageAllocationGeneration(uint metadataFlags)
{
    return metadataFlags >> kCLodVirtualShadowPhysicalPageAllocationGenerationShift;
}

uint CLodVirtualShadowPhysicalPageMetadataFlags(uint allocationGeneration, bool resident)
{
    return (allocationGeneration << kCLodVirtualShadowPhysicalPageAllocationGenerationShift) |
        (resident ? kCLodVirtualShadowPhysicalPageResidentFlag : 0u);
}
static const uint kCLodVirtualShadowDefaultClipmapCount = 22u;
static const uint kCLodVirtualShadowClipmapCount = 22u;
static const uint CLodVirtualShadowDefaultClipmapCount = kCLodVirtualShadowDefaultClipmapCount;
static const uint CLodVirtualShadowMaxSupportedClipmapCount = kCLodVirtualShadowClipmapCount;
static const uint kCLodVirtualShadowPhysicalPageSize = 128u;
static const uint kCLodVirtualShadowFixedVirtualPageCountPerAxis = 128u;
static const uint kCLodVirtualShadowFixedVirtualResolution =
    kCLodVirtualShadowFixedVirtualPageCountPerAxis * kCLodVirtualShadowPhysicalPageSize;
static const uint kCLodVirtualShadowDefaultVirtualResolution = kCLodVirtualShadowFixedVirtualResolution;
static const uint kCLodVirtualShadowMaxVirtualResolution = kCLodVirtualShadowFixedVirtualResolution;
static const uint kCLodVirtualShadowDefaultPageTableResolution =
    kCLodVirtualShadowDefaultVirtualResolution / kCLodVirtualShadowPhysicalPageSize;
static const uint kCLodVirtualShadowMaxPageTableResolution =
    kCLodVirtualShadowMaxVirtualResolution / kCLodVirtualShadowPhysicalPageSize;
static const uint kCLodVirtualShadowDefaultPhysicalAtlasPagesWide = kCLodVirtualShadowMaxPageTableResolution;
static const uint kCLodVirtualShadowDefaultPhysicalAtlasPagesHigh = kCLodVirtualShadowMaxPageTableResolution;
static const uint kCLodVirtualShadowMaxPhysicalAtlasPagesWide = kCLodVirtualShadowMaxPageTableResolution;
static const uint kCLodVirtualShadowMaxPhysicalAtlasPagesHigh = kCLodVirtualShadowMaxPageTableResolution;
static const uint kCLodVirtualShadowDefaultPhysicalPageCount =
    kCLodVirtualShadowDefaultPhysicalAtlasPagesWide * kCLodVirtualShadowDefaultPhysicalAtlasPagesHigh;
static const uint kCLodVirtualShadowMaxPhysicalPageCount =
    kCLodVirtualShadowMaxPhysicalAtlasPagesWide * kCLodVirtualShadowMaxPhysicalAtlasPagesHigh;
static const float kCLodVirtualShadowDefaultDirectionalLodBias = 0.0f;
static const uint kCLodVirtualShadowMovedInstanceBitCapacity = 1u << 20;
static const uint kCLodVirtualShadowMovedInstanceBitWordCount =
    (kCLodVirtualShadowMovedInstanceBitCapacity + 31u) / 32u;
static const uint kInvalidShadowCameraIndex = 0xFFFFFFFFu;
static const uint kCLodVirtualShadowMarkTileSize = 16u;
static const uint kCLodVirtualShadowBlockPagesPerAxis = 4u;
static const uint kCLodVirtualShadowBlockPackedPhysicalPageIndexCount =
    (kCLodVirtualShadowBlockPagesPerAxis * kCLodVirtualShadowBlockPagesPerAxis) / 2u;
static const uint kCLodVirtualShadowMaxBlocksPerAxis =
    (kCLodVirtualShadowMaxPageTableResolution + kCLodVirtualShadowBlockPagesPerAxis - 1u) / kCLodVirtualShadowBlockPagesPerAxis;
static const uint kCLodVirtualShadowMaxBlocksPerClipmap =
    kCLodVirtualShadowMaxBlocksPerAxis * kCLodVirtualShadowMaxBlocksPerAxis;
static const uint kCLodVirtualShadowMaxMarkedBlockCount =
    kCLodVirtualShadowMaxBlocksPerClipmap * kCLodVirtualShadowClipmapCount;

struct CLodVirtualShadowClipmapInfo
{
    float worldOriginX;
    float worldOriginY;
    float worldOriginZ;
    float texelWorldSize;
    uint pageOffsetX;
    uint pageOffsetY;
    uint pageTableLayer;
    uint shadowCameraBufferIndex;
    uint clipLevel;
    uint flags;
    int clearOffsetX;
    int clearOffsetY;
    float directionalLodBias;
    uint virtualResolution;
    uint pageTableResolution;
    uint physicalAtlasPagesWide;
    uint physicalAtlasPagesHigh;
    int unwrappedPageOffsetX;
    int unwrappedPageOffsetY;
};

struct CLodVirtualShadowMainCameraInfo
{
    float4 positionWorldSpace;
    row_major matrix viewInverse;
    row_major matrix projectionInverse;
};

struct CLodVirtualShadowCompactShadowCameraInfo
{
    row_major matrix view;
    row_major matrix projection;
    row_major matrix viewProjection;
    uint isOrtho;
    uint3 pad;
};

struct CLodVirtualShadowMarkClipmapData
{
    float texelWorldSize;
    uint pageOffsetX;
    uint pageOffsetY;
    uint pageTableLayer;
    uint flags;
    uint virtualResolution;
    uint pageTableResolution;
    uint physicalAtlasPagesWide;
    uint physicalAtlasPagesHigh;
    float directionalLodBias;
    int unwrappedPageOffsetX;
    int unwrappedPageOffsetY;
    row_major matrix shadowViewProjection;
};

struct CLodVirtualShadowMarkTileWorkItem
{
    uint tileCoordX;
    uint tileCoordY;
    uint minDepthBits;
    uint maxDepthBits;
};

struct CLodVirtualShadowRuntimeState
{
    uint clipmapCount;
    uint supportedClipmapCount;
    uint virtualResolution;
    uint pageTableResolution;
    uint physicalAtlasPagesWide;
    uint physicalAtlasPagesHigh;
    uint maxPhysicalPages;
    uint maxAllocationRequests;
    float directionalLodBias;
};

bool CLodVirtualShadowCompactCameraIsOrtho(CLodVirtualShadowCompactShadowCameraInfo cameraInfo)
{
    return cameraInfo.isOrtho != 0u;
}

bool CLodVirtualShadowMarkClipmapIsValid(CLodVirtualShadowMarkClipmapData clipmapData)
{
    return (clipmapData.flags & kCLodVirtualShadowClipmapValidFlag) != 0u;
}

struct CLodVirtualShadowStats
{
    uint activeClipmapCount;
    uint validClipmapCount;
    uint allocationRequestCount;
    uint allocationDispatchGroupCount;
    uint freePhysicalPageCount;
    uint reusablePhysicalPageCount;
    uint setupResetApplied;
    uint markRequestOverflowCount;
    uint setupResetForced;
    uint setupResetNoPreviousState;
    uint setupResetStructureMismatch;
    uint setupResetLightDirectionChanged;
    float currentAllocationPercentage;
    float targetPressureLodBias;
    float smoothedPressureLodBias;
    uint framesSinceOverBudget;
    uint configuredPageRenderBudget;
    uint configuredUpgradePageRenderBudget;
    uint normalEligiblePageCount;
    uint normalAdmittedPageCount;
    uint normalDeferredPageCount;
    uint upgradeEligiblePageCount;
    uint upgradeAdmittedPageCount;
    uint upgradeDeferredPageCount;
    uint pendingUpgradeDependencyCount;
    uint invalidUpgradeDependencyCount;
    uint admittedPageCount;
    uint admissionTicketCount;
    uint normalRenderedPageCount;
    uint upgradeRenderedPageCount;
    uint upgradeCandidateInputCount;
    uint upgradeCandidateRetainedCount;
    uint upgradeCandidateResidentCount;
    uint upgradeCandidateInvalidCount;
    uint upgradeRawPageCount;
    uint upgradeRawPageOverflowCount;
    uint readyUpgradePageCount;
    uint readyUpgradePageOverflowCount;
    uint upgradeCandidateAppendOverflowCount;
    uint cumulativeUpgradeCandidateResidentCount;
    uint cumulativeUpgradePageAdmittedCount;
    uint cumulativeUpgradePageRenderedCount;
    uint cumulativeUpgradeCandidateAppendOverflowCount;
    uint cumulativeUpgradeQueueOverflowCount;
    uint upgradeReadyConsumedAllocatedCount;
    uint upgradeReadyDroppedUnallocatedCount;
    uint upgradeInvalidationInputCount;
    uint upgradeInvalidationRejectedInputCount;
    uint upgradeInvalidationAllocatedPageTouchCount;
    uint upgradeInvalidationUnallocatedPageTouchCount;
    uint cumulativeUpgradeInvalidationInputCount;
    uint cumulativeUpgradeInvalidationAllocatedPageTouchCount;
    uint pageTableOwnerMismatchCount;
    uint contentValidOwnerMismatchCount;
    uint newlyAllocatedPageCount;
    uint physicalPageClearCount;
    uint dynamicPageClearCount;
    uint composedPageCount;
    uint markResidentTagMismatchCount;
    uint renderedWithoutMatchingClearCount;
    uint syntheticEmptyValidPageCount;
    uint blockExpandedRequestedRecordCount;
    uint blockExpandedCommittedRecordCount;
    uint blockExpandedDroppedRecordCount;
    uint pageJobRequestedRecordCount;
    uint pageJobCommittedRecordCount;
    uint pageJobDroppedRecordCount;
    uint pageJobDoubleSidedRecordCount;
    uint pageJobRasterJobCount;
    uint pageJobRasterTriangleCount;
    uint pageJobRasterDepthRejectedTriangleCount;
    uint pageJobRasterBackfaceRejectedTriangleCount;
    uint pageJobRasterCoveredPixelCount;
    uint pageJobRasterPageWriteCount;
    uint pageJobRasterClusterBoundsOverlapCount;
    uint pageJobRasterBboxRejectedTriangleCount;
    uint setupWrappedClearedPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint setupStaleDirtyClearedPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint markResidentCleanHits[kCLodVirtualShadowClipmapCount];
    uint markResidentDirtyHits[kCLodVirtualShadowClipmapCount];
    uint preAllocateNonZeroPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint preAllocateDirtyPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint selectedPixels[kCLodVirtualShadowClipmapCount];
    uint projectionRejectedPixels[kCLodVirtualShadowClipmapCount];
    uint requestedPages[kCLodVirtualShadowClipmapCount];
    uint nonZeroPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint allocatedPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint dirtyPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint clearedUnwrittenDirtyPages[kCLodVirtualShadowClipmapCount];
    uint visitedPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint visitedDirtyPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint predictiveInvalidatedPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint invalidatedCurrentBoundsPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint invalidatedPreviousBoundsPageTableEntries[kCLodVirtualShadowClipmapCount];
    uint deferredPreferredInactivePageSampleCount;
    uint deferredFallbackInactivePageSampleCount;
    uint deferredSmrtInactivePageSampleCount;
    uint deferredPreferredInactivePageRejectCount;
    uint deferredFallbackInactivePageRejectCount;
    uint deferredSmrtInactivePageRejectCount;
    uint trackedSkinnedPhysicalPagePlusOne;
    uint trackedSkinnedAtlasPixel;
    uint trackedSkinnedDepthBits;
    uint trackedComposeFramePlusOne;
    uint trackedComposeStaticDepthBits;
    uint trackedComposeDynamicBeforeBits;
    uint trackedComposeDynamicAfterBits;
    uint trackedDeferredSampleCount;
    uint trackedDeferredObservedDepthBits;
    uint trackedCompositionMismatchCount;
    uint trackedDeferredMismatchCount;
};

struct CLodVirtualShadowBlockMeta
{
    uint packedVirtualBlockOrigin;
    uint packedWrappedBlockOrigin;
    uint activePageMask;
    uint packedActiveRectAndFlags;
    uint packedPhysicalPageIndices[kCLodVirtualShadowBlockPackedPhysicalPageIndexCount];
};

bool CLodVirtualShadowClipmapIsValid(CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return (clipmapInfo.flags & kCLodVirtualShadowClipmapValidFlag) != 0u &&
        clipmapInfo.shadowCameraBufferIndex != kInvalidShadowCameraIndex;
}

bool CLodVirtualShadowTryGetClipmapInfoForView(
    uint viewID,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    out CLodVirtualShadowClipmapInfo clipmapInfo)
{
    clipmapInfo = (CLodVirtualShadowClipmapInfo)0;

    [unroll]
    for (uint candidateIndex = 0u; candidateIndex < kCLodVirtualShadowClipmapCount; ++candidateIndex)
    {
        const CLodVirtualShadowClipmapInfo candidate = clipmapInfos[candidateIndex];
        if (!CLodVirtualShadowClipmapIsValid(candidate))
        {
            continue;
        }

        if (candidate.shadowCameraBufferIndex == viewID)
        {
            clipmapInfo = candidate;
            return true;
        }
    }

    return false;
}

bool CLodVirtualShadowPageEntryCanRaster(uint pageEntry)
{
    const uint requiredMask =
        kCLodVirtualShadowAllocatedMask |
        kCLodVirtualShadowDirtyMask |
        kCLodVirtualShadowAdmittedThisFrameMask;
    return (pageEntry & requiredMask) == requiredMask;
}

bool CLodVirtualShadowPageEntryIsDynamicActive(uint pageEntry)
{
    const uint requiredMask =
        kCLodVirtualShadowAllocatedMask |
        kCLodVirtualShadowVisitedMask;
    return (pageEntry & requiredMask) == requiredMask;
}

bool CLodVirtualShadowPageEntryCanRasterLayer(
    uint pageEntry,
    bool dynamicLayer)
{
    return dynamicLayer
        ? CLodVirtualShadowPageEntryIsDynamicActive(pageEntry)
        : CLodVirtualShadowPageEntryCanRaster(pageEntry);
}

uint CLodVirtualShadowPackAbsolutePageTag(
    uint2 logicalPageCoords,
    int unwrappedPageOffsetX,
    int unwrappedPageOffsetY)
{
    const int2 absolutePageCoords =
        int2(logicalPageCoords) -
        int2(unwrappedPageOffsetX, unwrappedPageOffsetY);
    return
        (uint(absolutePageCoords.x) & 0xFFFFu) |
        ((uint(absolutePageCoords.y) & 0xFFFFu) << 16u);
}

uint CLodVirtualShadowWrapPageCoord(uint coord, uint offset, uint pageTableResolution)
{
    const uint wrappedOffset = pageTableResolution > 0u ? (offset % pageTableResolution) : 0u;
    return pageTableResolution > 0u
        ? ((coord + pageTableResolution - wrappedOffset) % pageTableResolution)
        : 0u;
}

uint2 CLodVirtualShadowVirtualPageCoordsFromUv(float2 shadowUv, uint pageTableResolution)
{
    const float2 clampedUv = saturate(shadowUv);
    return min(
        (uint2)(clampedUv * pageTableResolution),
        max(pageTableResolution, 1u) - 1u);
}

uint2 CLodVirtualShadowVirtualPageCoordsFromUv(float2 shadowUv, CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return CLodVirtualShadowVirtualPageCoordsFromUv(shadowUv, clipmapInfo.pageTableResolution);
}

uint2 CLodVirtualShadowVirtualPageCoordsFromUv(float2 shadowUv, CLodVirtualShadowMarkClipmapData clipmapData)
{
    return CLodVirtualShadowVirtualPageCoordsFromUv(shadowUv, clipmapData.pageTableResolution);
}

uint2 CLodVirtualShadowWrappedPageCoords(
    uint2 virtualPageCoords,
    CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return uint2(
        CLodVirtualShadowWrapPageCoord(virtualPageCoords.x, clipmapInfo.pageOffsetX, clipmapInfo.pageTableResolution),
        CLodVirtualShadowWrapPageCoord(virtualPageCoords.y, clipmapInfo.pageOffsetY, clipmapInfo.pageTableResolution));
}

uint2 CLodVirtualShadowWrappedPageCoords(
    uint2 virtualPageCoords,
    CLodVirtualShadowMarkClipmapData clipmapData)
{
    return uint2(
        CLodVirtualShadowWrapPageCoord(virtualPageCoords.x, clipmapData.pageOffsetX, clipmapData.pageTableResolution),
        CLodVirtualShadowWrapPageCoord(virtualPageCoords.y, clipmapData.pageOffsetY, clipmapData.pageTableResolution));
}

uint2 CLodVirtualShadowVirtualTexelCoordsFromUv(float2 shadowUv, uint virtualResolution)
{
    const float2 clampedUv = saturate(shadowUv);
    return min(
        (uint2)(clampedUv * virtualResolution),
        max(virtualResolution, 1u) - 1u);
}

uint2 CLodVirtualShadowVirtualTexelCoordsFromUv(float2 shadowUv, CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return CLodVirtualShadowVirtualTexelCoordsFromUv(shadowUv, clipmapInfo.virtualResolution);
}

uint2 CLodVirtualShadowVirtualTexelCoordsFromUv(float2 shadowUv, CLodVirtualShadowMarkClipmapData clipmapData)
{
    return CLodVirtualShadowVirtualTexelCoordsFromUv(shadowUv, clipmapData.virtualResolution);
}

uint2 CLodVirtualShadowVirtualPageCoordsFromPixel(uint2 pixel, CLodVirtualShadowClipmapInfo clipmapInfo)
{
    const uint clampedResolution = max(clipmapInfo.virtualResolution, 1u);
    const uint clampedPageTableResolution = max(clipmapInfo.pageTableResolution, 1u);
    const uint2 clampedPixel = min(pixel, clampedResolution - 1u);
    return min(
        clampedPixel / kCLodVirtualShadowPhysicalPageSize,
        clampedPageTableResolution - 1u);
}

uint2 CLodVirtualShadowBlockCoordFromPageCoord(uint2 pageCoord)
{
    return pageCoord / kCLodVirtualShadowBlockPagesPerAxis;
}

uint2 CLodVirtualShadowBlockOriginFromBlockCoord(uint2 blockCoord)
{
    return blockCoord * kCLodVirtualShadowBlockPagesPerAxis;
}

uint CLodVirtualShadowBlockLinearIndex(uint2 blockCoord, uint clipmapIndex)
{
    return clipmapIndex * kCLodVirtualShadowMaxBlocksPerClipmap +
        blockCoord.y * kCLodVirtualShadowMaxBlocksPerAxis +
        blockCoord.x;
}

void CLodVirtualShadowUnpackBlockLinearIndex(uint blockLinearIndex, out uint2 blockCoord, out uint clipmapIndex)
{
    clipmapIndex = blockLinearIndex / kCLodVirtualShadowMaxBlocksPerClipmap;
    const uint blockIndexWithinClipmap = blockLinearIndex % kCLodVirtualShadowMaxBlocksPerClipmap;
    blockCoord = uint2(
        blockIndexWithinClipmap % kCLodVirtualShadowMaxBlocksPerAxis,
        blockIndexWithinClipmap / kCLodVirtualShadowMaxBlocksPerAxis);
}

uint CLodVirtualShadowBlockLocalPageIndex(uint2 localPageCoord)
{
    return localPageCoord.x + localPageCoord.y * kCLodVirtualShadowBlockPagesPerAxis;
}

uint CLodVirtualShadowPackBlockPageCoords(uint2 pageCoords)
{
    return (pageCoords.x & 0xFFFFu) | ((pageCoords.y & 0xFFFFu) << 16u);
}

uint2 CLodVirtualShadowUnpackBlockPageCoords(uint packedPageCoords)
{
    return uint2(packedPageCoords & 0xFFFFu, (packedPageCoords >> 16u) & 0xFFFFu);
}

uint CLodVirtualShadowPackBlockActiveRect(uint2 minLocalPageCoord, uint2 maxLocalPageCoord, bool overflowed)
{
    return
        ((minLocalPageCoord.x & 0x3u) << 0u) |
        ((minLocalPageCoord.y & 0x3u) << 2u) |
        ((maxLocalPageCoord.x & 0x3u) << 4u) |
        ((maxLocalPageCoord.y & 0x3u) << 6u) |
        ((overflowed ? 1u : 0u) << 8u);
}

uint2 CLodVirtualShadowUnpackBlockActiveRectMin(uint packedActiveRect)
{
    return uint2((packedActiveRect >> 0u) & 0x3u, (packedActiveRect >> 2u) & 0x3u);
}

uint2 CLodVirtualShadowUnpackBlockActiveRectMax(uint packedActiveRect)
{
    return uint2((packedActiveRect >> 4u) & 0x3u, (packedActiveRect >> 6u) & 0x3u);
}

bool CLodVirtualShadowBlockActiveRectOverflowed(uint packedActiveRect)
{
    return ((packedActiveRect >> 8u) & 0x1u) != 0u;
}

bool CLodVirtualShadowBlockActiveRectContainsPage(uint packedActiveRect, uint2 localPageCoord)
{
    const uint2 minLocalPageCoord = CLodVirtualShadowUnpackBlockActiveRectMin(packedActiveRect);
    const uint2 maxLocalPageCoord = CLodVirtualShadowUnpackBlockActiveRectMax(packedActiveRect);
    return all(localPageCoord >= minLocalPageCoord) && all(localPageCoord <= maxLocalPageCoord);
}

bool CLodVirtualShadowAnyRenderablePageInPixelRect(
    uint2 minPixel,
    uint2 maxPixel,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    bool dynamicLayer,
    RWTexture2DArray<uint> pageTable)
{
    if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
    {
        return false;
    }

    if (minPixel.x > maxPixel.x || minPixel.y > maxPixel.y)
    {
        return false;
    }

    const uint2 minPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(minPixel, clipmapInfo);
    const uint2 maxPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(maxPixel, clipmapInfo);

    [loop]
    for (uint pageY = minPageCoords.y; pageY <= maxPageCoords.y; ++pageY)
    {
        [loop]
        for (uint pageX = minPageCoords.x; pageX <= maxPageCoords.x; ++pageX)
        {
            const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(uint2(pageX, pageY), clipmapInfo);
            const uint pageEntry = pageTable[uint3(wrappedPageCoords, clipmapInfo.pageTableLayer)];
            if (CLodVirtualShadowPageEntryCanRasterLayer(pageEntry, dynamicLayer))
            {
                return true;
            }
        }
    }

    return false;
}

bool CLodVirtualShadowAnyRenderablePageInPageRect(
    uint2 minPageCoords,
    uint2 maxPageCoords,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    bool dynamicLayer,
    RWTexture2DArray<uint> pageTable)
{
    if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
    {
        return false;
    }

    if (minPageCoords.x > maxPageCoords.x || minPageCoords.y > maxPageCoords.y)
    {
        return false;
    }

    [loop]
    for (uint pageY = minPageCoords.y; pageY <= maxPageCoords.y; ++pageY)
    {
        [loop]
        for (uint pageX = minPageCoords.x; pageX <= maxPageCoords.x; ++pageX)
        {
            const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(uint2(pageX, pageY), clipmapInfo);
            const uint pageEntry = pageTable[uint3(wrappedPageCoords, clipmapInfo.pageTableLayer)];
            if (CLodVirtualShadowPageEntryCanRasterLayer(pageEntry, dynamicLayer))
            {
                return true;
            }
        }
    }

    return false;
}

uint2 CLodVirtualShadowPhysicalAtlasPixel(uint physicalPageIndex, uint2 virtualTexelCoords, uint physicalAtlasPagesWide)
{
    const uint atlasPageX = physicalAtlasPagesWide > 0u ? (physicalPageIndex % physicalAtlasPagesWide) : 0u;
    const uint atlasPageY = physicalAtlasPagesWide > 0u ? (physicalPageIndex / physicalAtlasPagesWide) : 0u;
    return uint2(
        atlasPageX * kCLodVirtualShadowPhysicalPageSize + (virtualTexelCoords.x % kCLodVirtualShadowPhysicalPageSize),
        atlasPageY * kCLodVirtualShadowPhysicalPageSize + (virtualTexelCoords.y % kCLodVirtualShadowPhysicalPageSize));
}

uint2 CLodVirtualShadowPhysicalAtlasPixel(uint physicalPageIndex, uint2 virtualTexelCoords, CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapInfo.physicalAtlasPagesWide);
}

uint2 CLodVirtualShadowPhysicalAtlasPixel(uint physicalPageIndex, uint2 virtualTexelCoords, CLodVirtualShadowMarkClipmapData clipmapData)
{
    return CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapData.physicalAtlasPagesWide);
}

uint CLodVirtualShadowSelectClipmapIndex(
    float3 positionWS,
    float3 cameraPositionWS,
    float clip0TexelWorldSize,
    float directionalLodBias,
    uint activeClipmapCount)
{
    if (activeClipmapCount == 0u)
    {
        return 0u;
    }

    const float pageCount = (float)kCLodVirtualShadowFixedVirtualPageCountPerAxis;
    const float scaleRatio = pageCount > 0.0f ? max((pageCount - 2.0f) / pageCount, 0.0f) : 1.0f;
    const float clip0FrustumScale = 0.5f * max(clip0TexelWorldSize, 1.0e-5f) * (float)kCLodVirtualShadowFixedVirtualResolution;
    const float baseScale = max(clip0FrustumScale * scaleRatio, 1.0e-5f);
    const float distanceFromCamera = length(positionWS - cameraPositionWS);
    const float clipLevel = ceil(log2(max(distanceFromCamera / baseScale, 1.0f))) + directionalLodBias;
    return min((uint)max(clipLevel, 0.0f), activeClipmapCount - 1u);
}

#endif
