#ifndef __CLOD_VIRTUAL_SHADOW_CLIPMAP_HLSLI__
#define __CLOD_VIRTUAL_SHADOW_CLIPMAP_HLSLI__

static const uint kCLodVirtualShadowClipmapValidFlag = 0x1u;
static const uint kCLodVirtualShadowClipmapInvalidateFlag = 0x2u;
static const uint kCLodVirtualShadowClipmapDynamicSkinnedFlag = 0x4u;
// The slice belongs to a spot or point light face (a perspective map), not the sun's clipmap.
static const uint kCLodVirtualShadowClipmapLocalFlag = 0x8u;
// The local face is one of a cube's six (a point light, or a spot too wide for one face): +X, -X, +Y, -Y, +Z, -Z.
static const uint kCLodVirtualShadowClipmapCubeFlag = 0x10u;
static const uint kCLodVirtualShadowAllocatedMask = 0x80000000u;
static const uint kCLodVirtualShadowDirtyMask = 0x40000000u;
static const uint kCLodVirtualShadowContentValidMask = 0x20000000u;
static const uint kCLodVirtualShadowVisitedMask = 0x10000000u;
static const uint kCLodVirtualShadowRerenderedThisFrameMask = 0x08000000u;
static const uint kCLodVirtualShadowAdmittedThisFrameMask = 0x04000000u;
static const uint kCLodVirtualShadowUpgradePendingMask = 0x02000000u;
static const uint kCLodVirtualShadowDynamicContentMask = 0x01000000u;
static const uint kCLodVirtualShadowPhysicalPageIndexMask = 0x00FFFFFFu;
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
// Page-table slices. Each is a 16K canvas of 128x128 pages. The sun's clip i is slice i. Each spot or point light face
// owns two more after them: mip 0 fills the first, mips 1-7 are packed into the second
// (CLodVirtualShadowLocalMipPageOrigin). Page bookkeeping (blocks, receiver masks, page metadata owners) works per
// slice: "clipmapIndex" there is a slice index.
static const uint kCLodVirtualShadowMaxLocalFaceCount = 48u;
static const uint kCLodVirtualShadowLocalSlicesPerFace = 2u;
static const uint kCLodVirtualShadowLocalMipCount = 8u;
static const uint kCLodVirtualShadowFirstLocalSlice = kCLodVirtualShadowClipmapCount;
static const uint kCLodVirtualShadowMaxSliceCount =
    kCLodVirtualShadowClipmapCount + kCLodVirtualShadowMaxLocalFaceCount * kCLodVirtualShadowLocalSlicesPerFace;
// The view lookups' answer for a view that renders no slice.
static const uint kCLodVirtualShadowInvalidSlice = kCLodVirtualShadowMaxSliceCount;
// The clip info buffer holds one row per slice, then one view row per camera buffer index naming the slice that
// camera renders (CLodVirtualShadowFindClipmapIndexForView).
static const uint kCLodVirtualShadowMaxViewRowCount = 512u;
static const uint kCLodVirtualShadowViewRowBase = kCLodVirtualShadowMaxSliceCount;
static const uint kCLodVirtualShadowClipInfoRowCount = kCLodVirtualShadowViewRowBase + kCLodVirtualShadowMaxViewRowCount;
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
static const uint kCLodVirtualShadowReceiverSubpagesPerAxis = 4u;
static const uint kCLodVirtualShadowMaxReceiverPageCount =
    kCLodVirtualShadowMaxPageTableResolution *
    kCLodVirtualShadowMaxPageTableResolution *
    kCLodVirtualShadowMaxSliceCount;
static const uint kCLodVirtualShadowBlockPackedPhysicalPageIndexCount =
    (kCLodVirtualShadowBlockPagesPerAxis * kCLodVirtualShadowBlockPagesPerAxis) / 2u;
static const uint kCLodVirtualShadowMaxBlocksPerAxis =
    (kCLodVirtualShadowMaxPageTableResolution + kCLodVirtualShadowBlockPagesPerAxis - 1u) / kCLodVirtualShadowBlockPagesPerAxis;
static const uint kCLodVirtualShadowMaxBlocksPerClipmap =
    kCLodVirtualShadowMaxBlocksPerAxis * kCLodVirtualShadowMaxBlocksPerAxis;
static const uint kCLodVirtualShadowMaxMarkedBlockCount =
    kCLodVirtualShadowMaxBlocksPerClipmap * kCLodVirtualShadowMaxSliceCount;

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
    float depthNear;
    float depthRange;
};

struct CLodVirtualShadowMainCameraInfo
{
    float4 positionWorldSpace;
    row_major matrix viewInverse;
    row_major matrix projectionInverse;
    // The camera that rendered the linear depth page marking reads. Marking runs
    // before this frame writes that depth, so these are the previous frame's.
    row_major matrix markDepthViewInverse;
    row_major matrix markDepthProjectionInverse;
};

// Page marking reconstructs receivers from the primary linear depth, which still
// holds the previous frame when marking runs. Rays must come from the camera that
// rendered that depth; clip selection keeps the current camera position, as the
// receivers that sample this frame do.
CLodVirtualShadowMainCameraInfo CLodVirtualShadowMarkDepthCamera(CLodVirtualShadowMainCameraInfo camera)
{
    CLodVirtualShadowMainCameraInfo result = camera;
    result.viewInverse = camera.markDepthViewInverse;
    result.projectionInverse = camera.markDepthProjectionInverse;
    return result;
}

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
    uint invalidUpgradeDependencyCount;
    uint admittedPageCount;
    uint normalRenderedPageCount;
    uint upgradeRenderedPageCount;
    uint upgradeCandidateInputCount;
    uint upgradeRawPageCount;
    uint upgradeRawPageOverflowCount;
    uint readyUpgradePageCount;
    uint readyUpgradePageOverflowCount;
    uint upgradeCandidateAppendOverflowCount;
    uint cumulativeUpgradePageAdmittedCount;
    uint cumulativeUpgradePageRenderedCount;
    uint cumulativeUpgradeCandidateAppendOverflowCount;
    uint cumulativeUpgradeQueueOverflowCount;
    uint upgradeInvalidationInputCount;
    uint upgradeInvalidationRejectedInputCount;
    uint upgradeInvalidationAllocatedPageTouchCount;
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
    // Raster work dropped this frame (a cluster or voxel page that failed to load): no empty completion this frame.
    uint rasterWorkDroppedCount;
    // Admitted pages that every caster reaching them left without depth, completed as empty
    // (CLodVirtualShadowStampRenderedPageGenerationsCSMain).
    uint emptyAdmittedCompletedCount;
    // Upgrade pages waiting out CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_COOLDOWN_FRAMES; still pending.
    uint upgradeCooldownDeferredPageCount;
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
    // Receiver-side outcome of the primary deferred pass's directional lookup,
    // counted per pixel (only while frame telemetry is enabled).
    uint receiverLookupPixels;
    uint receiverPreferredSampledPixels;
    uint receiverFallbackOnePixels;
    uint receiverFallbackTwoPixels;
    uint receiverNoSamplePixels;
    uint receiverPreferredUnallocatedPixels;
    uint receiverPreferredNotVisitedPixels;
    uint receiverPreferredNoContentPixels;
    uint receiverPreferredTagMismatchPixels;
    uint receiverPreferredDepthMissingPixels;
    uint receiverTexelClearedPixels;
    uint receiverShadowedPixels;
    // Reprojected against the previous frame (CLodReceiverHistory).
    uint receiverHistoryValidPixels;
    uint receiverFlipPixels;
    uint receiverHardFlipPixels;
    uint receiverFlipClipChangedPixels;
    uint receiverFlipSamePagePixels;
    uint receiverFlipPageChangedPixels;
    uint receiverHardFlipSamePagePixels;
    uint receiverSoftOnlyFlipPixels;
    uint receiverFlipToLitPixels;
    uint receiverFlipPageChangedByClip[kCLodVirtualShadowClipmapCount];
    // Screen-centre probe: current/previous packed outcome and physical page.
    uint receiverProbe[4];
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

// The view row of a camera buffer index: Valid in flags, and the first slice that camera renders in pageTableLayer
// (VirtualShadowMapSetupPass writes them). A local face's mips 1-7 render into the slice after that one.
uint CLodVirtualShadowViewRowIndex(uint viewID)
{
    return kCLodVirtualShadowViewRowBase + viewID;
}

uint CLodVirtualShadowSliceFromViewRow(uint flags, uint pageTableLayer)
{
    return (flags & kCLodVirtualShadowClipmapValidFlag) != 0u && pageTableLayer < kCLodVirtualShadowMaxSliceCount
        ? pageTableLayer
        : kCLodVirtualShadowInvalidSlice;
}

// The slice viewID renders (its first, for a local face), or kCLodVirtualShadowInvalidSlice when it renders none.
uint CLodVirtualShadowFindClipmapIndexForView(uint viewID, StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos)
{
    if (viewID >= kCLodVirtualShadowMaxViewRowCount)
    {
        return kCLodVirtualShadowInvalidSlice;
    }
    const uint row = CLodVirtualShadowViewRowIndex(viewID);
    return CLodVirtualShadowSliceFromViewRow(clipmapInfos[row].flags, clipmapInfos[row].pageTableLayer);
}

uint CLodVirtualShadowFindClipmapIndexForView(uint viewID, RWStructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos)
{
    if (viewID >= kCLodVirtualShadowMaxViewRowCount)
    {
        return kCLodVirtualShadowInvalidSlice;
    }
    const uint row = CLodVirtualShadowViewRowIndex(viewID);
    return CLodVirtualShadowSliceFromViewRow(clipmapInfos[row].flags, clipmapInfos[row].pageTableLayer);
}

bool CLodVirtualShadowTryGetClipmapInfoForView(
    uint viewID,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    out CLodVirtualShadowClipmapInfo clipmapInfo)
{
    const uint clipmapIndex = CLodVirtualShadowFindClipmapIndexForView(viewID, clipmapInfos);
    if (clipmapIndex >= kCLodVirtualShadowMaxSliceCount)
    {
        clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
        return false;
    }
    clipmapInfo = clipmapInfos[clipmapIndex];
    return true;
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

bool CLodVirtualShadowPageEntryHasSampleableContent(uint pageEntry)
{
    // Lookups sample the transient composite atlas, not the persistent static
    // atlas. A page therefore becomes sampleable only after this frame's mark
    // pass visited it and the clear pass initialized its transient copy. Static
    // ContentValid alone is not enough: an unvisited fallback page can retain
    // arbitrary dynamic texels from an earlier frame or physical-page owner.
    // Dynamic-only pages remain supported once their current-frame raster sets
    // DynamicContent.
    return CLodVirtualShadowPageEntryIsDynamicActive(pageEntry) &&
        (pageEntry &
            (kCLodVirtualShadowContentValidMask |
             kCLodVirtualShadowDynamicContentMask)) != 0u;
}

// The page hierarchy carries both raster domains in one R32_UINT texture.
// Keeping the domains as independent bits lets rigid/static and animated
// casters share the hierarchy build and traversal machinery without making
// every rigid caster visit every cached page each frame.
static const uint kCLodVirtualShadowHierarchyStaticMask = 1u;
static const uint kCLodVirtualShadowHierarchyDynamicMask = 2u;

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

bool CLodVirtualShadowClipmapUsesDynamicSkinnedCasters(CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return (clipmapInfo.flags & kCLodVirtualShadowClipmapDynamicSkinnedFlag) != 0u;
}

bool CLodVirtualShadowClipmapIsLocal(CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return (clipmapInfo.flags & kCLodVirtualShadowClipmapLocalFlag) != 0u;
}

// A fallback candidate's shadowClipmapIndex: the slice in the low bits and, for a local face, the coarsest mip whose
// pages need the refined group, plus one, in the high byte. Coarser mips' LOD threshold already accepts the group the
// candidate stood in with, so upgrading their pages would redraw them for nothing. Zero there means every mip.
static const uint kCLodVirtualShadowCandidateSliceMask = 0x00FFFFFFu;
static const uint kCLodVirtualShadowCandidateMipShift = 24u;

uint CLodVirtualShadowPackCandidateSlice(uint slice, uint coarsestMip)
{
    return (slice & kCLodVirtualShadowCandidateSliceMask) | ((min(coarsestMip, 7u) + 1u) << kCLodVirtualShadowCandidateMipShift);
}

uint CLodVirtualShadowCandidateSlice(uint packed)
{
    return packed & kCLodVirtualShadowCandidateSliceMask;
}

uint CLodVirtualShadowCandidateCoarsestMip(uint packed)
{
    const uint encoded = packed >> kCLodVirtualShadowCandidateMipShift;
    return encoded == 0u ? 7u : encoded - 1u;
}

// Where a local face's mip lives: which of the face's two slices (0 or 1), and its page rectangle there. Mips 1-7 are
// packed into the second slice at origins that are multiples of their own size, so no 4x4-page block, and no texel of
// a dirty-hierarchy mip up to a mip's own top level, mixes two mips.
uint CLodVirtualShadowLocalMipSliceOffset(uint mip)
{
    return mip == 0u ? 0u : 1u;
}

uint CLodVirtualShadowLocalMipPagesPerAxis(uint mip)
{
    return kCLodVirtualShadowMaxPageTableResolution >> min(mip, kCLodVirtualShadowLocalMipCount - 1u);
}

uint2 CLodVirtualShadowLocalMipPageOrigin(uint mip)
{
    if (mip <= 1u)
    {
        return uint2(0u, 0u);
    }
    if (mip >= kCLodVirtualShadowLocalMipCount - 1u)
    {
        // At (126, 0) the last mip's single page would share mip 6's block at (124, 0).
        return uint2(kCLodVirtualShadowMaxPageTableResolution - 4u, 4u);
    }
    return uint2(kCLodVirtualShadowMaxPageTableResolution - (kCLodVirtualShadowMaxPageTableResolution >> (mip - 1u)), 0u);
}

// The mip a page of a local face's slice belongs to, or kCLodVirtualShadowLocalMipCount for a page no mip uses.
uint CLodVirtualShadowLocalMipForSlicePage(uint sliceOffset, uint2 pageCoord)
{
    if (sliceOffset == 0u)
    {
        return 0u;
    }
    [unroll]
    for (uint mip = 1u; mip < kCLodVirtualShadowLocalMipCount; ++mip)
    {
        const uint2 origin = CLodVirtualShadowLocalMipPageOrigin(mip);
        const uint size = CLodVirtualShadowLocalMipPagesPerAxis(mip);
        if (all(pageCoord >= origin) && all(pageCoord < origin + size))
        {
            return mip;
        }
    }
    return kCLodVirtualShadowLocalMipCount;
}

// The mip of a local light face whose texels best match a receiver's screen pixel: the finest whose texel is at least
// the pixel's size, so marking and lookup ask for the same pages. pixelWorldSize is the receiver pixel's width in the
// world; faceTexelWorldSize is the face's mip 0 texel at the receiver's depth.
float CLodVirtualShadowLocalContinuousMip(float pixelWorldSize, float faceTexelWorldSize, float lodBias)
{
    return log2(max(pixelWorldSize, 1.0e-8f) / max(faceTexelWorldSize, 1.0e-8f)) + lodBias;
}

uint CLodVirtualShadowLocalSelectMip(float continuousMip)
{
    return (uint)clamp(floor(continuousMip), 0.0f, (float)(kCLodVirtualShadowLocalMipCount - 1u));
}

// A face uv (0..1 across the face) as a uv across the slice canvas that holds mip.
float2 CLodVirtualShadowLocalMipCanvasUv(float2 faceUv, uint mip)
{
    const float pages = (float)kCLodVirtualShadowMaxPageTableResolution;
    // Just short of 1, so a uv on the face's far edge stays on the mip's last page, not the next mip's first.
    const float2 clampedUv = min(saturate(faceUv), 0.99999f.xx);
    return ((float2)CLodVirtualShadowLocalMipPageOrigin(mip) +
        clampedUv * (float)CLodVirtualShadowLocalMipPagesPerAxis(mip)) / pages;
}

uint2 CLodVirtualShadowReceiverSubpageCoordsFromUv(float2 shadowUv, uint pageTableResolution)
{
    const uint resolution = max(pageTableResolution, 1u) *
        kCLodVirtualShadowReceiverSubpagesPerAxis;
    return min((uint2)(saturate(shadowUv) * resolution), resolution - 1u);
}

uint2 CLodVirtualShadowReceiverSubpageCoordsFromUv(
    float2 shadowUv,
    uint pageTableResolution,
    uint subpagesPerAxis)
{
    const uint resolution = max(pageTableResolution, 1u) * max(subpagesPerAxis, 1u);
    return min((uint2)(saturate(shadowUv) * resolution), resolution - 1u);
}

uint2 CLodVirtualShadowReceiverMaskForPageRect(
    uint2 globalSubpageMin,
    uint2 globalSubpageMax,
    uint2 pageCoord,
    uint subpagesPerAxis)
{
    const uint2 pageOrigin = pageCoord * subpagesPerAxis;
    if (any(globalSubpageMax < pageOrigin) ||
        any(globalSubpageMin >= pageOrigin + subpagesPerAxis))
    {
        return uint2(0u, 0u);
    }
    const uint2 localMin = max(globalSubpageMin, pageOrigin) - pageOrigin;
    const uint2 localMax = min(globalSubpageMax, pageOrigin + subpagesPerAxis - 1u) - pageOrigin;
    const uint width = localMax.x - localMin.x + 1u;
    const uint rowBits = ((1u << width) - 1u) << localMin.x;
    uint2 result = uint2(0u, 0u);
    [unroll]
    for (uint y = 0u; y < 8u; ++y)
    {
        if (y < subpagesPerAxis && y >= localMin.y && y <= localMax.y)
        {
            const uint bitOffset = y * subpagesPerAxis;
            if (bitOffset < 32u)
            {
                result.x |= rowBits << bitOffset;
            }
            else
            {
                result.y |= rowBits << (bitOffset - 32u);
            }
        }
    }
    return result;
}

uint CLodVirtualShadowReceiverPageLinearIndex(uint2 pageCoord, uint clipmapIndex)
{
    return clipmapIndex *
            (kCLodVirtualShadowMaxPageTableResolution *
                kCLodVirtualShadowMaxPageTableResolution) +
        pageCoord.y * kCLodVirtualShadowMaxPageTableResolution +
        pageCoord.x;
}

uint CLodVirtualShadowBlockMaskForPageRect(
    uint2 logicalPageMin,
    uint2 logicalPageMax,
    uint2 blockOriginPage)
{
    uint activeMask = 0u;
    [unroll]
    for (uint localPageY = 0u;
         localPageY < kCLodVirtualShadowBlockPagesPerAxis;
         ++localPageY)
    {
        [unroll]
        for (uint localPageX = 0u;
             localPageX < kCLodVirtualShadowBlockPagesPerAxis;
             ++localPageX)
        {
            const uint2 logicalPageCoord =
                blockOriginPage + uint2(localPageX, localPageY);
            if (all(logicalPageCoord >= logicalPageMin) &&
                all(logicalPageCoord <= logicalPageMax))
            {
                activeMask |= 1u <<
                    (localPageY * kCLodVirtualShadowBlockPagesPerAxis +
                        localPageX);
            }
        }
    }
    return activeMask;
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

// A block's active-block metadata also holds which of its pages are active, one bit per page (bit y * 4 + x) above
// bit 16: its active rect bounds them, but can hold inactive pages too.
static const uint kCLodVirtualShadowBlockActivePageMaskShift = 16u;

uint CLodVirtualShadowBlockActivePageMask(uint packedActiveBlock)
{
    return packedActiveBlock >> kCLodVirtualShadowBlockActivePageMaskShift;
}

// The pages of a rect of a block's pages, as an active page mask.
uint CLodVirtualShadowBlockPageRectMask(uint2 minLocalPageCoord, uint2 maxLocalPageCoord)
{
    const uint rowBits = ((1u << (maxLocalPageCoord.x + 1u)) - 1u) & ~((1u << minLocalPageCoord.x) - 1u);
    uint mask = 0u;
    [unroll]
    for (uint y = 0u; y < kCLodVirtualShadowBlockPagesPerAxis; ++y)
    {
        if (y >= minLocalPageCoord.y && y <= maxLocalPageCoord.y)
        {
            mask |= rowBits << (y * kCLodVirtualShadowBlockPagesPerAxis);
        }
    }
    return mask;
}

// The bounding rect of the pages in a non-empty page mask.
void CLodVirtualShadowBlockPageMaskBounds(uint pageMask, out uint2 minLocalPageCoord, out uint2 maxLocalPageCoord)
{
    const uint columns = (pageMask | (pageMask >> 4u) | (pageMask >> 8u) | (pageMask >> 12u)) & 0xFu;
    const uint rows =
        ((pageMask & 0x000Fu) != 0u ? 1u : 0u) | ((pageMask & 0x00F0u) != 0u ? 2u : 0u) |
        ((pageMask & 0x0F00u) != 0u ? 4u : 0u) | ((pageMask & 0xF000u) != 0u ? 8u : 0u);
    minLocalPageCoord = uint2(firstbitlow(columns), firstbitlow(rows));
    maxLocalPageCoord = uint2(firstbithigh(columns), firstbithigh(rows));
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

float CLodVirtualShadowContinuousClipmapLevel(
    float3 positionWS,
    float3 cameraPositionWS,
    float clip0TexelWorldSize,
    float directionalLodBias,
    uint activeClipmapCount)
{
    if (activeClipmapCount == 0u)
    {
        return 0.0f;
    }

    const float pageCount = (float)kCLodVirtualShadowFixedVirtualPageCountPerAxis;
    const float scaleRatio = pageCount > 0.0f ? max((pageCount - 2.0f) / pageCount, 0.0f) : 1.0f;
    const float clip0FrustumScale = 0.5f * max(clip0TexelWorldSize, 1.0e-5f) * (float)kCLodVirtualShadowFixedVirtualResolution;
    const float baseScale = max(clip0FrustumScale * scaleRatio, 1.0e-5f);
    const float distanceFromCamera = length(positionWS - cameraPositionWS);
    // The configured bias is baked into clip0TexelWorldSize. Keeping the same
    // full (fractional) term here cancels that physical scale for clip ownership
    // while the selected clip's texel/page footprint changes continuously.
    const float clipLevel =
        log2(max(distanceFromCamera / baseScale, 1.0f)) +
        directionalLodBias;
    return clamp(
        clipLevel,
        0.0f,
        (float)(activeClipmapCount - 1u));
}

float CLodVirtualShadowContinuousTexelWorldSize(
    float3 positionWS,
    float3 cameraPositionWS,
    float clip0TexelWorldSize,
    float directionalLodBias,
    uint activeClipmapCount)
{
    return max(clip0TexelWorldSize, 1.0e-5f) * exp2(
        CLodVirtualShadowContinuousClipmapLevel(
            positionWS,
            cameraPositionWS,
            clip0TexelWorldSize,
            directionalLodBias,
            activeClipmapCount));
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

    const float clipLevel = ceil(
        CLodVirtualShadowContinuousClipmapLevel(
            positionWS,
            cameraPositionWS,
            clip0TexelWorldSize,
            directionalLodBias,
            activeClipmapCount));
    return min((uint)clipLevel, activeClipmapCount - 1u);
}

#endif
