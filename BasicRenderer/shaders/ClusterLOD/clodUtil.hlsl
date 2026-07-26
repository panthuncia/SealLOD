#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/waveIntrinsicsHelpers.hlsli"
#include "PerPassRootConstants/clodCreateCommandRootConstants.h"
#include "PerPassRootConstants/clodClearUintBufferRootConstants.h"
#include "PerPassRootConstants/clodHistogramRootConstants.h"
#include "PerPassRootConstants/clodPrefixScanRootConstants.h"
#include "PerPassRootConstants/clodPrefixOffsetsRootConstants.h"
#include "PerPassRootConstants/clodCompactionRootConstants.h"
#include "PerPassRootConstants/clodReyesCreateDispatchArgsRootConstants.h"
#include "PerPassRootConstants/clodReyesResetRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowAllocateRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowAdmitPagesRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowBuildPageListsRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowClearDirtyBitsRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowBuildActiveBlocksRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowApplyUpgradesRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowClearRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowFreeWrappedRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowGatherStatsRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowDirtyHierarchyRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowDeduplicatePredictedPagesRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowExpandPredictedPagesRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowInvalidateRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowMarkBlocksRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowBuildMarkTilesRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowMarkRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowResolveMarkedBlocksRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowSetupRootConstants.h"
#include "PerPassRootConstants/clodReyesSplitRootConstants.h"
#include "PerPassRootConstants/clodReyesReplayMergeRootConstants.h"
#include "include/indirectCommands.hlsli"
#include "include/clodStructs.hlsli"
#include "include/visibleClusterPacking.hlsli"

struct RasterBucketsHistogramIndirectCommand
{
    uint clusterCount;
    uint xDim;
    uint dispatchX, dispatchY, dispatchZ;
};

static const uint CLOD_TELEMETRY_DISABLED_DESCRIPTOR = 0xFFFFFFFFu;
static const uint WG_COUNTER_RASTER_SORT_HISTOGRAM_INPUTS = 109u;
static const uint WG_COUNTER_RASTER_SORT_HISTOGRAM_VOXEL_SKIPPED = 110u;
static const uint WG_COUNTER_RASTER_SORT_HISTOGRAM_REYES_SKIPPED = 111u;
static const uint WG_COUNTER_RASTER_SORT_HISTOGRAM_TRIANGLE_CONTRIBUTORS = 112u;
static const uint WG_COUNTER_RASTER_SORT_COMPACTION_INPUTS = 113u;
static const uint WG_COUNTER_RASTER_SORT_COMPACTION_VOXEL_SKIPPED = 114u;
static const uint WG_COUNTER_RASTER_SORT_COMPACTION_REYES_SKIPPED = 115u;
static const uint WG_COUNTER_RASTER_SORT_COMPACTION_TRIANGLE_EMITTED = 116u;

void CLodSortTelemetryAdd(uint descriptorIndex, uint counterIndex, uint value)
{
    if (descriptorIndex == CLOD_TELEMETRY_DISABLED_DESCRIPTOR || value == 0u)
    {
        return;
    }

    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[descriptorIndex];
    InterlockedAdd(telemetryCounters[counterIndex], value);
}

struct CLodVirtualShadowInvalidationInput
{
    uint perMeshInstanceBufferIndex;
    uint flags;
    uint pad0;
    uint pad1;
};

struct CLodVirtualShadowPredictiveInvalidationCandidate
{
    float4 worldCenterAndRadius;
    uint shadowClipmapIndex;
    uint sourceGroupGlobalIndex;
    uint perMeshInstanceBufferIndex;
    uint observedFrameIndex;
};

struct CLodVirtualShadowPredictedRawPage
{
    uint sourceGroupGlobalIndex;
    uint physicalPageIndex;
    uint allocationGeneration;
    uint contentGeneration;
    uint clipmapIndex;
    uint virtualAddress;
    uint perMeshInstanceBufferIndex;
    uint pad0;
};

struct CLodVirtualShadowPredictedPage
{
    uint sourceGroupGlobalIndex;
    uint physicalPageIndex;
    uint allocationGeneration;
    uint contentGeneration;
    uint clipmapIndex;
    uint virtualAddress;
    uint perMeshInstanceBufferIndex;
    uint pad0;
};

struct CLodVirtualShadowUpgradeInvalidationInput
{
    uint physicalPageIndex;
    uint allocationGeneration;
    uint contentGeneration;
    uint ownerClipmapIndex;
    uint ownerVirtualAddress;
    uint sourceGroupGlobalIndex;
    uint residencyGeneration;
    uint pad0;
};

static const uint kCLodVirtualShadowInvalidationFlagUsePreviousBounds = 0x1u;
static const uint kCLodVirtualShadowInvalidationFlagUseCurrentBounds = 0x2u;
static const uint kCLodVirtualShadowInvalidationFlagSkinned = 0x4u;
static const uint kCLodVirtualShadowInvalidationStatsReasonPredictive = 0x1u;
static const uint kCLodVirtualShadowInvalidationStatsReasonCurrentBounds = 0x2u;
static const uint kCLodVirtualShadowInvalidationStatsReasonPreviousBounds = 0x4u;
static const uint kCLodVirtualShadowInvalidationStatsReasonSkinned = 0x8u;
static const uint kCLodVirtualShadowPredictiveCandidateCapacity = (1u << 16);
static const uint kCLodVirtualShadowPredictiveRawPageCapacity = (1u << 20);
static const uint kCLodVirtualShadowFallbackDependencyHashCapacity = (1u << 17);
static const uint kCLodVirtualShadowPredictedPageEntriesPerClipmap =
    kCLodVirtualShadowMaxPageTableResolution * kCLodVirtualShadowMaxPageTableResolution;
static const uint kCLodVirtualShadowPredictedPageListCapacity =
    (1u << 16);
static const uint kCLodVirtualShadowPredictedPageBitsetWordCount =
    (kCLodVirtualShadowPredictedPageListCapacity + 31u) / 32u;
groupshared uint gCLodVirtualShadowShouldClearPage;
groupshared uint gCLodVirtualShadowMarkTileHasGeometry;
groupshared uint gCLodVirtualShadowMarkTileMinDepthBits;
groupshared uint gCLodVirtualShadowMarkTileMaxDepthBits;

[shader("compute")]
[numthreads(64, 1, 1)]
void ClearUintStructuredBufferCSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= CLOD_CLEAR_UINT_BUFFER_COUNT)
    {
        return;
    }

    RWStructuredBuffer<uint> outBuffer = ResourceDescriptorHeap[CLOD_CLEAR_UINT_BUFFER_DESCRIPTOR_INDEX];
    outBuffer[dtid.x] = CLOD_CLEAR_UINT_BUFFER_VALUE;
}

float CLodMaxAxisScale_RowVector(float4x4 M)
{
    float3 ax = float3(M[0][0], M[0][1], M[0][2]);
    float3 ay = float3(M[1][0], M[1][1], M[1][2]);
    float3 az = float3(M[2][0], M[2][1], M[2][2]);
    return max(length(ax), max(length(ay), length(az)));
}

bool CLodVirtualShadowShouldClearWrappedPage(int2 wrappedPageCoords, int clearOffsetX, int clearOffsetY, uint pageTableResolution)
{
    const int resolution = (int)pageTableResolution;
    if (resolution <= 0)
    {
        return false;
    }

    return
        ((clearOffsetX > 0) && (wrappedPageCoords.x < clearOffsetX)) ||
        ((clearOffsetX < 0) && (wrappedPageCoords.x > resolution + (clearOffsetX - 1))) ||
        ((clearOffsetY > 0) && (wrappedPageCoords.y < clearOffsetY)) ||
        ((clearOffsetY < 0) && (wrappedPageCoords.y > resolution + (clearOffsetY - 1)));
}

uint CLodVirtualShadowUnwrapPageCoord(uint wrappedCoord, uint offset, uint pageTableResolution)
{
    const uint wrappedOffset = pageTableResolution > 0u ? (offset % pageTableResolution) : 0u;
    return pageTableResolution > 0u
        ? ((wrappedCoord + wrappedOffset) % pageTableResolution)
        : 0u;
}

uint2 CLodVirtualShadowUnwrappedPageCoords(
    uint2 wrappedPageCoords,
    CLodVirtualShadowClipmapInfo clipmapInfo)
{
    return uint2(
        CLodVirtualShadowUnwrapPageCoord(wrappedPageCoords.x, clipmapInfo.pageOffsetX, clipmapInfo.pageTableResolution),
        CLodVirtualShadowUnwrapPageCoord(wrappedPageCoords.y, clipmapInfo.pageOffsetY, clipmapInfo.pageTableResolution));
}

#define CLOD_VSM_STATS_INCREMENT(field, clipmapIndex) \
    switch (clipmapIndex) \
    { \
    case 0u: InterlockedAdd(statsBuffer[0].field[0], 1u); break; \
    case 1u: InterlockedAdd(statsBuffer[0].field[1], 1u); break; \
    case 2u: InterlockedAdd(statsBuffer[0].field[2], 1u); break; \
    case 3u: InterlockedAdd(statsBuffer[0].field[3], 1u); break; \
    case 4u: InterlockedAdd(statsBuffer[0].field[4], 1u); break; \
    case 5u: InterlockedAdd(statsBuffer[0].field[5], 1u); break; \
    case 6u: InterlockedAdd(statsBuffer[0].field[6], 1u); break; \
    case 7u: InterlockedAdd(statsBuffer[0].field[7], 1u); break; \
    case 8u: InterlockedAdd(statsBuffer[0].field[8], 1u); break; \
    case 9u: InterlockedAdd(statsBuffer[0].field[9], 1u); break; \
    case 10u: InterlockedAdd(statsBuffer[0].field[10], 1u); break; \
    case 11u: InterlockedAdd(statsBuffer[0].field[11], 1u); break; \
    case 12u: InterlockedAdd(statsBuffer[0].field[12], 1u); break; \
    case 13u: InterlockedAdd(statsBuffer[0].field[13], 1u); break; \
    case 14u: InterlockedAdd(statsBuffer[0].field[14], 1u); break; \
    case 15u: InterlockedAdd(statsBuffer[0].field[15], 1u); break; \
    default: break; \
    }

void CLodVirtualShadowStatsIncrementSelected(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(selectedPixels, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementProjectionRejected(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(projectionRejectedPixels, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementRequestedPages(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(requestedPages, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementNonZero(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(nonZeroPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementPreAllocateNonZero(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(preAllocateNonZeroPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementAllocated(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    switch (clipmapIndex)
    {
    case 0u: InterlockedAdd(statsBuffer[0].allocatedPageTableEntries[0], 1u); break;
    case 1u: InterlockedAdd(statsBuffer[0].allocatedPageTableEntries[1], 1u); break;
    case 2u: InterlockedAdd(statsBuffer[0].allocatedPageTableEntries[2], 1u); break;
    case 3u: InterlockedAdd(statsBuffer[0].allocatedPageTableEntries[3], 1u); break;
    case 4u: InterlockedAdd(statsBuffer[0].allocatedPageTableEntries[4], 1u); break;
    case 5u: InterlockedAdd(statsBuffer[0].allocatedPageTableEntries[5], 1u); break;
    }
}

void CLodVirtualShadowWaveReserveRequestSlot(
    RWStructuredBuffer<uint> allocationCountBuffer,
    uint capacity,
    bool contributes,
    out uint slot,
    out bool valid)
{
    slot = 0u;
    valid = false;

    const uint4 mask = WaveActiveBallot(contributes);
    const uint count = CountBits128(mask);
    if (count == 0u || !contributes)
    {
        return;
    }

    const uint leaderLane = WaveFirstLaneFromMask(mask);
    const uint laneRank = GetLaneRankInGroup(mask, WaveGetLaneIndex());

    uint baseSlot = 0u;
    if (WaveGetLaneIndex() == leaderLane)
    {
        InterlockedAdd(allocationCountBuffer[0], count, baseSlot);
    }
    baseSlot = WaveReadLaneAt(baseSlot, leaderLane);

    slot = baseSlot + laneRank;
    valid = slot < capacity;
}

void CLodVirtualShadowWaveReservePageListSlot(
    RWStructuredBuffer<uint4> pageListHeader,
    uint componentIndex,
    uint capacity,
    bool contributes,
    out uint slot,
    out bool valid)
{
    slot = 0u;
    valid = false;

    const uint4 mask = WaveActiveBallot(contributes);
    const uint count = CountBits128(mask);
    if (count == 0u || !contributes)
    {
        return;
    }

    const uint leaderLane = WaveFirstLaneFromMask(mask);
    const uint laneRank = GetLaneRankInGroup(mask, WaveGetLaneIndex());

    uint baseSlot = 0u;
    if (WaveGetLaneIndex() == leaderLane)
    {
        switch (componentIndex)
        {
        case 0u:
            InterlockedAdd(pageListHeader[0].x, count, baseSlot);
            break;
        case 1u:
            InterlockedAdd(pageListHeader[0].y, count, baseSlot);
            break;
        default:
            break;
        }
    }
    baseSlot = WaveReadLaneAt(baseSlot, leaderLane);

    slot = baseSlot + laneRank;
    valid = slot < capacity;
}

void CLodVirtualShadowStatsIncrementDirty(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(dirtyPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementPreAllocateDirty(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(preAllocateDirtyPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementClearedUnwrittenDirty(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(clearedUnwrittenDirtyPages, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementVisited(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(visitedPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementVisitedDirty(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(visitedDirtyPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementPredictiveInvalidated(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(predictiveInvalidatedPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementInvalidatedCurrentBounds(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(invalidatedCurrentBoundsPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementInvalidatedPreviousBounds(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(invalidatedPreviousBoundsPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementInvalidatedSkinned(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(invalidatedSkinnedPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementInvalidatedByReason(
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer,
    uint clipmapIndex,
    uint reasonFlags)
{
    if ((reasonFlags & kCLodVirtualShadowInvalidationStatsReasonPredictive) != 0u)
    {
        CLodVirtualShadowStatsIncrementPredictiveInvalidated(statsBuffer, clipmapIndex);
    }
    if ((reasonFlags & kCLodVirtualShadowInvalidationStatsReasonCurrentBounds) != 0u)
    {
        CLodVirtualShadowStatsIncrementInvalidatedCurrentBounds(statsBuffer, clipmapIndex);
    }
    if ((reasonFlags & kCLodVirtualShadowInvalidationStatsReasonPreviousBounds) != 0u)
    {
        CLodVirtualShadowStatsIncrementInvalidatedPreviousBounds(statsBuffer, clipmapIndex);
    }
    if ((reasonFlags & kCLodVirtualShadowInvalidationStatsReasonSkinned) != 0u)
    {
        CLodVirtualShadowStatsIncrementInvalidatedSkinned(statsBuffer, clipmapIndex);
    }
}

bool CLodProjectWorldToShadowUv(float3 positionWS, CLodVirtualShadowCompactShadowCameraInfo shadowCamera, out float2 shadowUv);
bool CLodProjectWorldToShadowUv(float3 positionWS, row_major matrix shadowViewProjection, out float2 shadowUv);
float3 CLodProjectWorldToShadowProjected(float3 positionWS, row_major matrix shadowViewProjection);

void CLodVirtualShadowStatsIncrementSetupWrappedClear(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(setupWrappedClearedPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementSetupStaleDirtyClear(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(setupStaleDirtyClearedPageTableEntries, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementMarkResidentCleanHit(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(markResidentCleanHits, clipmapIndex);
}

void CLodVirtualShadowStatsIncrementMarkResidentDirtyHit(RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer, uint clipmapIndex)
{
    CLOD_VSM_STATS_INCREMENT(markResidentDirtyHits, clipmapIndex);
}

float CLodVirtualShadowAllocationPercentage(CLodVirtualShadowStats previousStats, uint physicalPageCount)
{
    const float safePhysicalPageCount = max((float)physicalPageCount, 1.0f);
    const float requestCount = (float)previousStats.allocationRequestCount;
    return requestCount / safePhysicalPageCount;
}

float CLodVirtualShadowPressureFeedbackLodBias(float allocationPercentage, float autoLodBiasScale)
{
    const float positiveScale = max(autoLodBiasScale, 0.0f);
    if (positiveScale <= 0.0f)
    {
        return 0.0f;
    }

    const float targetAllocationPercentage = 0.8f;
    const float allocationRatio = max(allocationPercentage / targetAllocationPercentage, 0.0001f);
    return min(max(log2(allocationRatio), 0.0f) * positiveScale, 8.0f);
}

float CLodVirtualShadowSmoothPressureLodBias(
    float previousSmoothedPressureLodBias,
    float targetPressureLodBias,
    float allocationPercentage,
    uint markRequestOverflowCount,
    uint framesSinceOverBudget)
{
    const float previousBias = max(previousSmoothedPressureLodBias, 0.0f);
    const float targetBias = max(targetPressureLodBias, 0.0f);
    const float delta = targetBias - previousBias;
    const float deadZone = 0.02f;
    if (abs(delta) <= deadZone)
    {
        return previousBias;
    }

    const float targetAllocationPercentage = 0.8f;
    const float recoveryAllocationPercentage = 0.5f;
    const float hardPressureAllocationPercentage = 1.0f;
    const float rampUpAlpha = 0.6f;
    const float rampDownAlpha = 0.02f;
    const uint rampDownHoldFrames = 32u;
    const bool overflowed = markRequestOverflowCount > 0u;
    const bool hardPressured = allocationPercentage >= hardPressureAllocationPercentage;
    const bool overBudget = overflowed || allocationPercentage >= targetAllocationPercentage;
    const bool recoverySafe = !overflowed && allocationPercentage <= recoveryAllocationPercentage;

    if (targetBias > previousBias || hardPressured)
    {
        return clamp(lerp(previousBias, targetBias, rampUpAlpha), 0.0f, 8.0f);
    }

    if (overBudget || !recoverySafe || framesSinceOverBudget < rampDownHoldFrames)
    {
        return previousBias;
    }

    return clamp(lerp(previousBias, targetBias, rampDownAlpha), 0.0f, 8.0f);
}

#undef CLOD_VSM_STATS_INCREMENT

uint CLodCalculateShadowCascadeIndex(float depth, uint numCascadeSplits, float4 cascadeSplits)
{
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        if (i >= numCascadeSplits)
        {
            break;
        }

        if (depth < cascadeSplits[i])
        {
            return i;
        }
    }

    return numCascadeSplits > 0u ? (numCascadeSplits - 1u) : 0u;
}

float3 CLodWorldSpaceFromScreenUv(float2 screenUv, float linearDepth, CLodVirtualShadowMainCameraInfo camera)
{
    const float2 ndc = float2(screenUv.x * 2.0f - 1.0f, (1.0f - screenUv.y) * 2.0f - 1.0f);
    const float4 clipPos = float4(ndc, 1.0f, 1.0f);
    const float4 viewPosH = mul(clipPos, camera.projectionInverse);
    const float3 positionVS = viewPosH.xyz * linearDepth;
    return mul(float4(positionVS, 1.0f), camera.viewInverse).xyz;
}

float3 CLodRayPlaneIntersection(float3 rayDirection, float3 rayOrigin, float3 planeNormal, float3 planeOrigin)
{
    const float denom = dot(planeNormal, rayDirection);
    if (abs(denom) <= 1.0e-5f)
    {
        return planeOrigin;
    }

    const float t = dot(planeOrigin - rayOrigin, planeNormal) / denom;
    return rayOrigin + t * rayDirection;
}

bool CLodProjectWorldToShadowUv(float3 positionWS, CLodVirtualShadowCompactShadowCameraInfo shadowCamera, out float2 shadowUv)
{
    const float4 shadowClip = mul(float4(positionWS, 1.0f), shadowCamera.viewProjection);
    const float safeW = max(abs(shadowClip.w), 1.0e-6f);
    const float3 projected = shadowClip.xyz / safeW;

    if (projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f)
    {
        shadowUv = 0.0f.xx;
        return false;
    }

    shadowUv = projected.xy * 0.5f + 0.5f;
    shadowUv.y = 1.0f - shadowUv.y;
    return true;
}

bool CLodProjectWorldToShadowUv(float3 positionWS, row_major matrix shadowViewProjection, out float2 shadowUv)
{
    const float4 shadowClip = mul(float4(positionWS, 1.0f), shadowViewProjection);
    const float safeW = max(abs(shadowClip.w), 1.0e-6f);
    const float3 projected = shadowClip.xyz / safeW;

    if (projected.x < -1.0f || projected.x > 1.0f ||
        projected.y < -1.0f || projected.y > 1.0f ||
        projected.z < 0.0f || projected.z > 1.0f)
    {
        shadowUv = 0.0f.xx;
        return false;
    }

    shadowUv = projected.xy * 0.5f + 0.5f;
    shadowUv.y = 1.0f - shadowUv.y;
    return true;
}

float3 CLodProjectWorldToShadowProjected(float3 positionWS, row_major matrix shadowViewProjection)
{
    const float4 shadowClip = mul(float4(positionWS, 1.0f), shadowViewProjection);
    const float safeW = max(abs(shadowClip.w), 1.0e-6f);
    return shadowClip.xyz / safeW;
}

float4 CLodDirectionalShadowPageViewRow(CLodVirtualShadowCompactShadowCameraInfo shadowCamera)
{
    return float4(
        shadowCamera.view[3][0],
        shadowCamera.view[3][1],
        shadowCamera.view[3][2],
        shadowCamera.view[3][3]);
}

uint CLodDirectionalShadowPageViewInfoIndex(uint2 wrappedPageCoords, uint clipmapIndex, uint pageTableResolution)
{
    return clipmapIndex * (pageTableResolution * pageTableResolution) +
        wrappedPageCoords.y * pageTableResolution +
        wrappedPageCoords.x;
}

uint CLodVirtualShadowBlockMaskForPageRect(
    uint2 logicalPageMin,
    uint2 logicalPageMax,
    uint2 blockOriginPage)
{
    uint activeMask = 0u;

    [unroll]
    for (uint localPageY = 0u; localPageY < kCLodVirtualShadowBlockPagesPerAxis; ++localPageY)
    {
        [unroll]
        for (uint localPageX = 0u; localPageX < kCLodVirtualShadowBlockPagesPerAxis; ++localPageX)
        {
            const uint2 logicalPageCoord = blockOriginPage + uint2(localPageX, localPageY);
            const bool insideRect =
                all(logicalPageCoord >= logicalPageMin) &&
                all(logicalPageCoord <= logicalPageMax);
            if (insideRect)
            {
                activeMask |= 1u << CLodVirtualShadowBlockLocalPageIndex(uint2(localPageX, localPageY));
            }
        }
    }

    return activeMask;
}

void CLodMarkVirtualShadowWrappedPage(
    uint2 wrappedPageCoords,
    uint clipmapIndex,
    uint pageTableResolution,
    uint maxRequestCount,
    uint activeClipmapCount,
    uint absolutePageTag,
    RWStructuredBuffer<uint4> allocationRequests,
    RWStructuredBuffer<uint> allocationCountBuffer,
    RWTexture2DArray<uint> pageTable,
    RWStructuredBuffer<uint> dirtyFlags,
    RWStructuredBuffer<float4> directionalPageViewInfo,
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer)
{
    const uint3 pageCoords = uint3(wrappedPageCoords, clipmapIndex);
    const uint virtualAddress = wrappedPageCoords.y * pageTableResolution + wrappedPageCoords.x;

    const uint currentPageState = pageTable[pageCoords];
    if ((currentPageState & kCLodVirtualShadowAllocatedMask) != 0u)
    {
        const uint physicalPageIndex =
            currentPageState &
            kCLodVirtualShadowPhysicalPageIndexMask;
        const uint cachedAbsolutePageTag =
            asuint(directionalPageViewInfo[physicalPageIndex].w);
        const bool absolutePageChanged =
            cachedAbsolutePageTag != absolutePageTag;
        uint ignoredVisited = 0u;
        InterlockedOr(
            pageTable[pageCoords],
            kCLodVirtualShadowVisitedMask |
                (absolutePageChanged ? kCLodVirtualShadowDirtyMask : 0u),
            ignoredVisited);
        if (absolutePageChanged)
        {
            // This wrapped table coordinate now represents a different
            // absolute clipmap page. Never expose the previous contents under
            // the new semantic tag while the rerender is budget-deferred.
            uint ignoredValid = 0u;
            InterlockedAnd(
                pageTable[pageCoords],
                ~kCLodVirtualShadowContentValidMask,
                ignoredValid);
            InterlockedOr(
                dirtyFlags[physicalPageIndex >> 5u],
                1u << (physicalPageIndex & 31u));
            InterlockedAdd(
                statsBuffer[0].markResidentTagMismatchCount,
                1u);
            CLodVirtualShadowStatsIncrementMarkResidentDirtyHit(
                statsBuffer,
                clipmapIndex);
        }
        else if ((currentPageState & kCLodVirtualShadowDirtyMask) != 0u)
        {
            CLodVirtualShadowStatsIncrementMarkResidentDirtyHit(statsBuffer, clipmapIndex);
            InterlockedOr(dirtyFlags[physicalPageIndex >> 5u], 1u << (physicalPageIndex & 31u));
        }
        else
        {
            CLodVirtualShadowStatsIncrementMarkResidentCleanHit(statsBuffer, clipmapIndex);
        }
        return;
    }

    if ((currentPageState & kCLodVirtualShadowDirtyMask) != 0u)
    {
        uint ignoredVisited = 0u;
        InterlockedOr(pageTable[pageCoords], kCLodVirtualShadowVisitedMask, ignoredVisited);
        return;
    }

    uint previousPageState = 0u;
    InterlockedOr(pageTable[pageCoords], kCLodVirtualShadowDirtyMask | kCLodVirtualShadowVisitedMask, previousPageState);
    if ((previousPageState & kCLodVirtualShadowAllocatedMask) != 0u)
    {
        const uint physicalPageIndex = previousPageState & kCLodVirtualShadowPhysicalPageIndexMask;
        InterlockedOr(dirtyFlags[physicalPageIndex >> 5u], 1u << (physicalPageIndex & 31u));
        return;
    }

    if ((previousPageState & kCLodVirtualShadowDirtyMask) != 0u)
    {
        return;
    }

    uint requestIndex = 0u;
    bool requestIndexValid = false;
    CLodVirtualShadowWaveReserveRequestSlot(
        allocationCountBuffer,
        maxRequestCount,
        true,
        requestIndex,
        requestIndexValid);

    const bool requestOverflowed = !requestIndexValid;
    const uint4 overflowMask = WaveActiveBallot(requestOverflowed);
    const uint overflowCount = CountBits128(overflowMask);
    if (overflowCount > 0u)
    {
        const uint overflowLeader = WaveFirstLaneFromMask(overflowMask);
        if (WaveGetLaneIndex() == overflowLeader)
        {
            InterlockedAdd(statsBuffer[0].markRequestOverflowCount, overflowCount);
        }
    }

    if (requestOverflowed)
    {
        uint ignored = 0u;
        InterlockedAnd(pageTable[pageCoords], ~(kCLodVirtualShadowDirtyMask | kCLodVirtualShadowVisitedMask), ignored);
        return;
    }

    allocationRequests[requestIndex] = uint4(
        virtualAddress,
        clipmapIndex,
        activeClipmapCount - clipmapIndex,
        absolutePageTag);
    CLodVirtualShadowStatsIncrementRequestedPages(statsBuffer, clipmapIndex);
}

bool CLodInvalidateVirtualShadowWrappedPage(
    uint2 wrappedPageCoords,
    uint clipmapIndex,
    uint pageTableResolution,
    uint frameIndex,
    float4 directionalPageViewRow,
    RWTexture2DArray<uint> pageTable,
    RWStructuredBuffer<uint> dirtyFlags,
    RWStructuredBuffer<uint4> pageMetadata,
    RWStructuredBuffer<float4> directionalPageViewInfo,
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer,
    uint reasonFlags)
{
    (void)frameIndex;
    (void)directionalPageViewRow;
    (void)dirtyFlags;

    const uint3 pageCoords = uint3(wrappedPageCoords, clipmapIndex);
    uint previousPageState = 0u;
    InterlockedAnd(pageTable[pageCoords], 0u, previousPageState);
    if ((previousPageState & kCLodVirtualShadowAllocatedMask) == 0u)
    {
        return false;
    }

    const uint physicalPageIndex = previousPageState & kCLodVirtualShadowPhysicalPageIndexMask;
    const uint allocationGeneration =
        CLodVirtualShadowPhysicalPageAllocationGeneration(pageMetadata[physicalPageIndex].z);
    pageMetadata[physicalPageIndex] = uint4(
        0u,
        0u,
        CLodVirtualShadowPhysicalPageMetadataFlags(allocationGeneration, false),
        0u);
    directionalPageViewInfo[physicalPageIndex] = 0.0f.xxxx;
    CLodVirtualShadowStatsIncrementInvalidatedByReason(statsBuffer, clipmapIndex, reasonFlags);
    return true;
}

bool CLodProjectSphereToShadowUvBounds(float3 centerWS, float radiusWS, CLodVirtualShadowCompactShadowCameraInfo shadowCamera, out float2 uvMin, out float2 uvMax)
{
    float2 ndcMin = 1.0e30f.xx;
    float2 ndcMax = -1.0e30f.xx;
    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        const float3 cornerOffset = float3(
            (cornerIndex & 1u) != 0u ? radiusWS : -radiusWS,
            (cornerIndex & 2u) != 0u ? radiusWS : -radiusWS,
            (cornerIndex & 4u) != 0u ? radiusWS : -radiusWS);
        const float4 clipCorner =
            mul(float4(centerWS + cornerOffset, 1.0f), shadowCamera.viewProjection);
        const float2 ndcCorner = CLodVirtualShadowCompactCameraIsOrtho(shadowCamera)
            ? clipCorner.xy
            : clipCorner.xy / max(abs(clipCorner.w), 1.0e-6f);
        ndcMin = min(ndcMin, ndcCorner);
        ndcMax = max(ndcMax, ndcCorner);
    }

    if (ndcMax.x < -1.0f || ndcMin.x > 1.0f ||
        ndcMax.y < -1.0f || ndcMin.y > 1.0f)
    {
        uvMin = 0.0f.xx;
        uvMax = 0.0f.xx;
        return false;
    }

    ndcMin = clamp(ndcMin, -1.0f.xx, 1.0f.xx);
    ndcMax = clamp(ndcMax, -1.0f.xx, 1.0f.xx);
    uvMin = float2(ndcMin.x * 0.5f + 0.5f, 1.0f - (ndcMax.y * 0.5f + 0.5f));
    uvMax = float2(ndcMax.x * 0.5f + 0.5f, 1.0f - (ndcMin.y * 0.5f + 0.5f));
    return true;
}

bool CLodFindVirtualShadowClipmapByShadowView(
    uint shadowViewId,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    out uint outClipmapIndex,
    out CLodVirtualShadowClipmapInfo outClipmapInfo)
{
    [unroll]
    for (uint clipmapIndex = 0u; clipmapIndex < CLodVirtualShadowMaxSupportedClipmapCount; ++clipmapIndex)
    {
        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
        if (CLodVirtualShadowClipmapIsValid(clipmapInfo) && clipmapInfo.shadowCameraBufferIndex == shadowViewId)
        {
            outClipmapIndex = clipmapIndex;
            outClipmapInfo = clipmapInfo;
            return true;
        }
    }

    outClipmapIndex = 0u;
    outClipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    return false;
}

uint CLodVirtualShadowWrappedVirtualAddress(uint2 wrappedPageCoords, uint pageTableResolution)
{
    return wrappedPageCoords.x + wrappedPageCoords.y * pageTableResolution;
}

void CLodAppendVirtualShadowPredictedRawPage(
    uint2 wrappedPageCoords,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    uint sourceGroupGlobalIndex,
    uint perMeshInstanceBufferIndex,
    uint pageEntry,
    uint4 pageMeta,
    RWStructuredBuffer<CLodVirtualShadowPredictedRawPage> rawPages,
    RWStructuredBuffer<uint> rawPageCount)
{
    uint rawPageIndex = 0u;
    InterlockedAdd(rawPageCount[0], 1u, rawPageIndex);
    if (rawPageIndex < kCLodVirtualShadowPredictiveRawPageCapacity)
    {
        CLodVirtualShadowPredictedRawPage rawPage;
        rawPage.sourceGroupGlobalIndex = sourceGroupGlobalIndex;
        rawPage.physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
        rawPage.allocationGeneration =
            CLodVirtualShadowPhysicalPageAllocationGeneration(pageMeta.z);
        rawPage.contentGeneration = pageMeta.y;
        rawPage.clipmapIndex = clipmapInfo.pageTableLayer;
        rawPage.virtualAddress =
            CLodVirtualShadowWrappedVirtualAddress(wrappedPageCoords, clipmapInfo.pageTableResolution);
        rawPage.perMeshInstanceBufferIndex = perMeshInstanceBufferIndex;
        rawPage.pad0 = 0u;
        rawPages[rawPageIndex] = rawPage;
    }
}

uint CLodVirtualShadowPredictedPageKey(CLodVirtualShadowPredictedRawPage rawPage)
{
    return rawPage.clipmapIndex * kCLodVirtualShadowPredictedPageEntriesPerClipmap + rawPage.virtualAddress;
}

bool CLodTrySetPredictedPageBit(RWStructuredBuffer<uint> bitsetWords, uint key)
{
    const uint wordIndex = key >> 5u;
    const uint bitMask = 1u << (key & 31u);
    uint previousWord = 0u;
    InterlockedOr(bitsetWords[wordIndex], bitMask, previousWord);
    return (previousWord & bitMask) == 0u;
}

void CLodInvalidateVirtualShadowSphere(
    float3 centerWS,
    float radiusWS,
    uint clipmapCount,
    uint frameIndex,
    uint reasonFlags,
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> shadowCameras,
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos,
    RWTexture2DArray<uint> pageTable,
    RWStructuredBuffer<uint> dirtyFlags,
    RWStructuredBuffer<uint4> pageMetadata,
    RWStructuredBuffer<float4> directionalPageViewInfo,
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer)
{
    [loop]
    for (uint clipmapIndex = 0u; clipmapIndex < clipmapCount; ++clipmapIndex)
    {
        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
        if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
        {
            continue;
        }

        const CLodVirtualShadowCompactShadowCameraInfo shadowCamera = shadowCameras[clipmapIndex];
        float2 uvMin;
        float2 uvMax;
        if (!CLodProjectSphereToShadowUvBounds(centerWS, radiusWS, shadowCamera, uvMin, uvMax))
        {
            continue;
        }

        const uint2 logicalPageMin = CLodVirtualShadowVirtualPageCoordsFromUv(uvMin, clipmapInfo);
        const uint2 logicalPageMax = CLodVirtualShadowVirtualPageCoordsFromUv(uvMax, clipmapInfo);
        const uint2 pageMin = CLodVirtualShadowWrappedPageCoords(logicalPageMin, clipmapInfo);
        const uint2 pageMax = CLodVirtualShadowWrappedPageCoords(logicalPageMax, clipmapInfo);
        const float4 directionalPageViewRow = CLodDirectionalShadowPageViewRow(shadowCamera);

        const bool wrapsX = pageMin.x > pageMax.x;
        const bool wrapsY = pageMin.y > pageMax.y;
        const uint segmentCountX = wrapsX ? 2u : 1u;
        const uint segmentCountY = wrapsY ? 2u : 1u;

        [loop]
        for (uint segmentY = 0u; segmentY < segmentCountY; ++segmentY)
        {
            const uint yStart = (segmentY == 0u) ? pageMin.y : 0u;
            const uint yEnd = (!wrapsY || segmentY == 0u) ? pageMax.y : (CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_TABLE_RESOLUTION - 1u);

            [loop]
            for (uint segmentX = 0u; segmentX < segmentCountX; ++segmentX)
            {
                const uint xStart = (segmentX == 0u) ? pageMin.x : 0u;
                const uint xEnd = (!wrapsX || segmentX == 0u) ? pageMax.x : (CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_TABLE_RESOLUTION - 1u);

                [loop]
                for (uint pageY = yStart; pageY <= yEnd; ++pageY)
                {
                    [loop]
                    for (uint pageX = xStart; pageX <= xEnd; ++pageX)
                    {
                        CLodInvalidateVirtualShadowWrappedPage(
                            uint2(pageX, pageY),
                            clipmapInfo.pageTableLayer,
                            clipmapInfo.pageTableResolution,
                            frameIndex,
                            directionalPageViewRow,
                            pageTable,
                            dirtyFlags,
                            pageMetadata,
                            directionalPageViewInfo,
                            statsBuffer,
                            reasonFlags);
                    }
                }
            }
        }
    }
}

bool CLodIsVisibleClusterOwnedByReyes(uint visibleClusterIndex, uint ownershipDescriptorIndex)
{
    StructuredBuffer<uint> ownershipWords = ResourceDescriptorHeap[ownershipDescriptorIndex];
    const uint ownershipWord = ownershipWords[visibleClusterIndex >> 5u];
    return ((ownershipWord >> (visibleClusterIndex & 31u)) & 1u) != 0u;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ClearReyesOwnershipBitsetCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint wordIndex = dispatchThreadId.x;
    if (wordIndex >= CLOD_REYES_RESET_OWNERSHIP_BITSET_WORD_COUNT)
    {
        return;
    }

    RWStructuredBuffer<uint> ownershipWords = ResourceDescriptorHeap[CLOD_REYES_RESET_OWNERSHIP_BITSET_DESCRIPTOR_INDEX];
    ownershipWords[wordIndex] = 0u;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ClearReyesQueueCountersCSMain()
{
    RWStructuredBuffer<uint> fullClusterCounter = ResourceDescriptorHeap[CLOD_REYES_RESET_FULL_CLUSTER_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> ownedClusterCounter = ResourceDescriptorHeap[CLOD_REYES_RESET_OWNED_CLUSTER_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> splitQueueCounterA = ResourceDescriptorHeap[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_A_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> splitQueueOverflowA = ResourceDescriptorHeap[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_A_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> splitQueueCounterB = ResourceDescriptorHeap[CLOD_REYES_RESET_SPLIT_QUEUE_COUNTER_B_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> splitQueueOverflowB = ResourceDescriptorHeap[CLOD_REYES_RESET_SPLIT_QUEUE_OVERFLOW_B_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> diceQueueCounter = ResourceDescriptorHeap[CLOD_REYES_RESET_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> diceQueueOverflow = ResourceDescriptorHeap[CLOD_REYES_RESET_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];

    fullClusterCounter[0] = 0u;
    ownedClusterCounter[0] = 0u;
    splitQueueCounterA[0] = 0u;
    splitQueueOverflowA[0] = 0u;
    splitQueueCounterB[0] = 0u;
    splitQueueOverflowB[0] = 0u;
    if (CLOD_REYES_RESET_CLEAR_DICE_QUEUE_COUNTER != 0u)
    {
        diceQueueCounter[0] = 0u;
    }
    diceQueueOverflow[0] = 0u;

    if (CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> replaySplitCounter = ResourceDescriptorHeap[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX];
        replaySplitCounter[0] = 0u;
    }
    if (CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> replaySplitOverflow = ResourceDescriptorHeap[CLOD_REYES_RESET_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];
        replaySplitOverflow[0] = 0u;
    }
    if (CLOD_REYES_RESET_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> replayDiceCounter = ResourceDescriptorHeap[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
        replayDiceCounter[0] = 0u;
    }
    if (CLOD_REYES_RESET_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> replayDiceOverflow = ResourceDescriptorHeap[CLOD_REYES_RESET_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];
        replayDiceOverflow[0] = 0u;
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ClearReyesSplitOutputCountersCSMain()
{
    RWStructuredBuffer<uint> outputSplitQueueCounter = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> outputSplitQueueOverflowCounter = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];

    outputSplitQueueCounter[0] = 0u;
    outputSplitQueueOverflowCounter[0] = 0u;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void BuildReyesDispatchArgsCSMain()
{
    StructuredBuffer<uint> sourceCounterBuffer = ResourceDescriptorHeap[CLOD_REYES_CREATE_DISPATCH_ARGS_SOURCE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesDispatchIndirectCommand> indirectArgsBuffer = ResourceDescriptorHeap[CLOD_REYES_CREATE_DISPATCH_ARGS_OUTPUT_DESCRIPTOR_INDEX];

    uint workItemCount = sourceCounterBuffer.Load(0);
    if (CLOD_REYES_CREATE_DISPATCH_ARGS_SOURCE_BASE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> sourceBaseCounterBuffer = ResourceDescriptorHeap[CLOD_REYES_CREATE_DISPATCH_ARGS_SOURCE_BASE_COUNTER_DESCRIPTOR_INDEX];
        const uint baseWorkItemCount = sourceBaseCounterBuffer.Load(0);
        workItemCount = (workItemCount > baseWorkItemCount) ? (workItemCount - baseWorkItemCount) : 0u;
    }
    const uint threadsPerGroup = max(CLOD_REYES_CREATE_DISPATCH_ARGS_THREADS_PER_GROUP, 1u);
    workItemCount = min(workItemCount, CLOD_REYES_CREATE_DISPATCH_ARGS_MAX_WORK_ITEM_COUNT);

    indirectArgsBuffer[0].dispatchX = (workItemCount + threadsPerGroup - 1u) / threadsPerGroup;
    indirectArgsBuffer[0].dispatchY = 1u;
    indirectArgsBuffer[0].dispatchZ = 1u;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void MergeReyesReplaySplitQueueCSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    StructuredBuffer<CLodReyesSplitQueueEntry> sourceQueue =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_SOURCE_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> sourceCounter =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_SOURCE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesSplitQueueEntry> destQueue =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_DEST_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> destCounter =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_DEST_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> destOverflow =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_DEST_OVERFLOW_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_TELEMETRY_DESCRIPTOR_INDEX];

    const uint sourceIndex = dispatchThreadID.x;
    const uint sourceCount = sourceCounter[0];
    if (sourceIndex >= sourceCount)
    {
        return;
    }

    uint destIndex = 0u;
    InterlockedAdd(destCounter[0], 1u, destIndex);
    if (destIndex >= CLOD_REYES_REPLAY_MERGE_CAPACITY)
    {
        InterlockedAdd(destOverflow[0], 1u);
        InterlockedAdd(telemetryBuffer[0].replaySplitQueueOverflowCount, 1u);
        return;
    }

    destQueue[destIndex] = sourceQueue[sourceIndex];
    InterlockedAdd(telemetryBuffer[0].replaySplitMergeCount, 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void MergeReyesReplayDiceQueueCSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    StructuredBuffer<CLodReyesDiceQueueEntry> sourceQueue =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_SOURCE_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> sourceCounter =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_SOURCE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesDiceQueueEntry> destQueue =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_DEST_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> destCounter =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_DEST_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> destOverflow =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_DEST_OVERFLOW_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer =
        ResourceDescriptorHeap[CLOD_REYES_REPLAY_MERGE_TELEMETRY_DESCRIPTOR_INDEX];

    const uint sourceIndex = dispatchThreadID.x;
    const uint sourceCount = sourceCounter[0];
    if (sourceIndex >= sourceCount)
    {
        return;
    }

    uint destIndex = 0u;
    InterlockedAdd(destCounter[0], 1u, destIndex);
    if (destIndex >= CLOD_REYES_REPLAY_MERGE_CAPACITY)
    {
        InterlockedAdd(destOverflow[0], 1u);
        InterlockedAdd(telemetryBuffer[0].replayDiceQueueOverflowCount, 1u);
        return;
    }

    destQueue[destIndex] = sourceQueue[sourceIndex];
    InterlockedAdd(telemetryBuffer[0].replayDiceMergeCount, 1u);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodVirtualShadowClearPhysicalPagesCSMain(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    if (groupId.x >= CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_PAGE_COUNT)
    {
        return;
    }

    RWTexture2D<uint> physicalPages = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_PAGES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_CLEAR_STATS_DESCRIPTOR_INDEX];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    if (groupThreadId.x == 0u && groupThreadId.y == 0u)
    {
        const uint physicalPageIndex = groupId.x;
        const uint dirtyWordIndex = physicalPageIndex >> 5u;
        const uint dirtyBitMask = 1u << (physicalPageIndex & 31u);
        const uint dirtyWord = dirtyFlags[dirtyWordIndex];
        gCLodVirtualShadowShouldClearPage = (dirtyWord & dirtyBitMask) != 0u ? 1u : 0u;
        if (gCLodVirtualShadowShouldClearPage != 0u)
        {
            InterlockedAdd(statsBuffer[0].physicalPageClearCount, 1u);
            uint ignored = 0u;
            InterlockedAnd(dirtyFlags[dirtyWordIndex], ~dirtyBitMask, ignored);

            // Clearing the physical page invalidates the cached depth contents.
            // Keep Dirty so the page can be rerendered, but drop ContentValid so
            // lookups never sample a cleared atlas page as if it were still valid.
            const uint4 meta = pageMetadata[physicalPageIndex];
            if ((meta.z & kCLodVirtualShadowPhysicalPageResidentFlag) != 0u)
            {
                uint4 clearedMeta = meta;
                clearedMeta.y =
                    kCLodVirtualShadowClearEpochPendingMask |
                    (perFrameBuffer.frameIndex &
                        ~kCLodVirtualShadowClearEpochPendingMask);
                pageMetadata[physicalPageIndex] = clearedMeta;
                const uint virtualAddress = meta.x;
                const uint clipmapIndex = meta.w;
                const uint pageX = virtualAddress % CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_TABLE_RESOLUTION;
                const uint pageY = virtualAddress / CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_TABLE_RESOLUTION;
                StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
                    ResourceDescriptorHeap[
                        CLOD_VIRTUAL_SHADOW_CLEAR_CLIPMAP_INFO_DESCRIPTOR_INDEX];
                const CLodVirtualShadowClipmapInfo clipmapInfo =
                    clipmapInfos[clipmapIndex];
                const uint2 logicalPageCoords =
                    CLodVirtualShadowUnwrappedPageCoords(
                        uint2(pageX, pageY),
                        clipmapInfo);
                const uint pageTag =
                    CLodVirtualShadowPackAbsolutePageTag(
                        logicalPageCoords,
                        clipmapInfo.unwrappedPageOffsetX,
                        clipmapInfo.unwrappedPageOffsetY);
                StructuredBuffer<Camera> shadowCameras =
                    ResourceDescriptorHeap[
                        ResourceDescriptorIndex(Builtin::CameraBuffer)];
                RWStructuredBuffer<float4> pageViewInfo =
                    ResourceDescriptorHeap[
                        CLOD_VIRTUAL_SHADOW_CLEAR_PAGE_VIEW_INFO_DESCRIPTOR_INDEX];
                pageViewInfo[physicalPageIndex] =
                    float4(
                        0.0f,
                        0.0f,
                        shadowCameras[
                            clipmapInfo.shadowCameraBufferIndex].view[3].z,
                        asfloat(pageTag));
                uint ignoredAnd = 0u;
                InterlockedAnd(
                    pageTable[uint3(pageX, pageY, clipmapIndex)],
                    ~(kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask),
                    ignoredAnd);

                uint ignoredOr = 0u;
                InterlockedOr(pageTable[uint3(pageX, pageY, clipmapIndex)], kCLodVirtualShadowDirtyMask, ignoredOr);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gCLodVirtualShadowShouldClearPage == 0u)
    {
        return;
    }

    const uint atlasPageX = groupId.x % CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_ATLAS_PAGES_WIDE;
    const uint atlasPageY = groupId.x / CLOD_VIRTUAL_SHADOW_CLEAR_PHYSICAL_ATLAS_PAGES_WIDE;
    const uint2 atlasBasePixel = uint2(
        atlasPageX * kCLodVirtualShadowPhysicalPageSize,
        atlasPageY * kCLodVirtualShadowPhysicalPageSize);

    for (uint localY = groupThreadId.y; localY < kCLodVirtualShadowPhysicalPageSize; localY += 8u)
    {
        for (uint localX = groupThreadId.x; localX < kCLodVirtualShadowPhysicalPageSize; localX += 8u)
        {
            physicalPages[atlasBasePixel + uint2(localX, localY)] = 0x7F7FFFFFu;
        }
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodVirtualShadowFreeWrappedPagesCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.z >= CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_CLIPMAP_COUNT ||
        dispatchThreadId.x >= CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_RESOLUTION ||
        dispatchThreadId.y >= CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_RESOLUTION)
    {
        return;
    }

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_STATS_DESCRIPTOR_INDEX];

    const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[dispatchThreadId.z];
    if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
    {
        return;
    }

    const uint2 logicalPageCoords = dispatchThreadId.xy;
    if (!CLodVirtualShadowShouldClearWrappedPage(
            int2(logicalPageCoords),
            clipmapInfo.clearOffsetX,
            clipmapInfo.clearOffsetY,
            CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PAGE_TABLE_RESOLUTION))
    {
        return;
    }

    CLodVirtualShadowStatsIncrementSetupWrappedClear(statsBuffer, dispatchThreadId.z);

    const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(logicalPageCoords, clipmapInfo);
    const uint3 pageCoords = uint3(wrappedPageCoords, clipmapInfo.pageTableLayer);
    const uint pageEntry = pageTable[pageCoords];
    if ((pageEntry & kCLodVirtualShadowAllocatedMask) != 0u)
    {
        const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
        if (physicalPageIndex < CLOD_VIRTUAL_SHADOW_FREE_WRAPPED_PHYSICAL_PAGE_COUNT)
        {
            const uint allocationGeneration =
                CLodVirtualShadowPhysicalPageAllocationGeneration(pageMetadata[physicalPageIndex].z);
            pageMetadata[physicalPageIndex] = uint4(
                0u,
                0u,
                CLodVirtualShadowPhysicalPageMetadataFlags(allocationGeneration, false),
                0u);
        }
    }

    pageTable[pageCoords] = 0u;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodVirtualShadowSetupCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.z >= CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT ||
        dispatchThreadId.x >= CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION ||
        dispatchThreadId.y >= CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION)
    {
        return;
    }

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> allocationCount = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_ALLOCATION_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_STATS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowMarkClipmapData> markClipmapDataBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_MARK_CLIPMAP_DATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowRuntimeState> runtimeStateBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_RUNTIME_STATE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> fallbackCandidateCount =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_SETUP_FALLBACK_CANDIDATE_COUNT_DESCRIPTOR_INDEX];

    const uint existingPageEntry = pageTable[dispatchThreadId];

    if (CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES != 0u)
    {
        pageTable[dispatchThreadId] = 0u;
    }
    else
    {
        const bool staleDirtyPage =
            (existingPageEntry & kCLodVirtualShadowDirtyMask) != 0u &&
            (((existingPageEntry & kCLodVirtualShadowAllocatedMask) == 0u) ||
                (existingPageEntry & kCLodVirtualShadowAdmittedThisFrameMask) != 0u) &&
            (existingPageEntry & kCLodVirtualShadowRerenderedThisFrameMask) == 0u;
        if (staleDirtyPage)
        {
            uint updatedPageEntry = existingPageEntry & ~(
                kCLodVirtualShadowVisitedMask |
                kCLodVirtualShadowRerenderedThisFrameMask |
                kCLodVirtualShadowAdmittedThisFrameMask);

            if ((existingPageEntry & kCLodVirtualShadowAllocatedMask) != 0u)
            {
                // The physical page was cleared but never rewritten. Keep the page
                // dirty so a later mark can rerender it, and drop ContentValid so
                // shadow lookups cannot treat cleared depth as usable data.
                updatedPageEntry &= ~kCLodVirtualShadowContentValidMask;
            }
            else
            {
                // Unallocated dirty pages were requested but never assigned a
                // physical page. Clear Dirty so a future mark pass can enqueue a
                // fresh allocation request once depth becomes valid again.
                updatedPageEntry &= ~kCLodVirtualShadowDirtyMask;
            }

            pageTable[dispatchThreadId] = updatedPageEntry;
            CLodVirtualShadowStatsIncrementSetupStaleDirtyClear(statsBuffer, dispatchThreadId.z);
            if ((existingPageEntry & kCLodVirtualShadowContentValidMask) == 0u)
            {
                CLodVirtualShadowStatsIncrementClearedUnwrittenDirty(statsBuffer, dispatchThreadId.z);
            }
        }
        else if ((existingPageEntry & (
                kCLodVirtualShadowVisitedMask |
                kCLodVirtualShadowRerenderedThisFrameMask |
                kCLodVirtualShadowAdmittedThisFrameMask)) != 0u)
        {
            pageTable[dispatchThreadId] =
                existingPageEntry & ~(
                    kCLodVirtualShadowVisitedMask |
                    kCLodVirtualShadowRerenderedThisFrameMask |
                    kCLodVirtualShadowAdmittedThisFrameMask);
        }
    }

    if (dispatchThreadId.z != 0u)
    {
        return;
    }

    const uint linearIndex = dispatchThreadId.y * CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION + dispatchThreadId.x;
    if (linearIndex == 0u)
    {
        const CLodVirtualShadowStats previousStats = statsBuffer[0];
        const float baseDirectionalLodBias = clipmapInfos[0].directionalLodBias;
        const float currentAllocationPercentage = CLodVirtualShadowAllocationPercentage(
            previousStats,
            CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT);
        const float targetPressureLodBias = CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_ENABLED != 0u
            ? CLodVirtualShadowPressureFeedbackLodBias(
                currentAllocationPercentage,
                asfloat(CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_SCALE_AS_UINT))
            : 0.0f;
        const float previousSmoothedPressureLodBias = CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES != 0u
            ? 0.0f
            : previousStats.smoothedPressureLodBias;
        const uint previousFramesSinceOverBudget = CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES != 0u
            ? 0u
            : previousStats.framesSinceOverBudget;
        const bool recoverySafe =
            currentAllocationPercentage <= 0.5f &&
            previousStats.markRequestOverflowCount == 0u;
        const uint framesSinceOverBudget = recoverySafe
            ? min(previousFramesSinceOverBudget + 1u, 0xFFFFFFFFu)
            : 0u;
        const float smoothedPressureLodBias = CLOD_VIRTUAL_SHADOW_SETUP_AUTO_BIAS_ENABLED != 0u
            ? CLodVirtualShadowSmoothPressureLodBias(
                previousSmoothedPressureLodBias,
                targetPressureLodBias,
                currentAllocationPercentage,
                previousStats.markRequestOverflowCount,
                framesSinceOverBudget)
            : 0.0f;
        const float effectiveDirectionalLodBias = baseDirectionalLodBias + smoothedPressureLodBias;

        [unroll]
        for (uint clipmapIndex = 0u; clipmapIndex < CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT; ++clipmapIndex)
        {
            CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
            clipmapInfo.directionalLodBias = effectiveDirectionalLodBias;
            clipmapInfos[clipmapIndex] = clipmapInfo;

            CLodVirtualShadowMarkClipmapData markClipmapData = markClipmapDataBuffer[clipmapIndex];
            markClipmapData.directionalLodBias = effectiveDirectionalLodBias;
            markClipmapDataBuffer[clipmapIndex] = markClipmapData;
        }

        CLodVirtualShadowRuntimeState runtimeState = runtimeStateBuffer[0];
        runtimeState.directionalLodBias = effectiveDirectionalLodBias;
        runtimeStateBuffer[0] = runtimeState;

        statsBuffer[0] = (CLodVirtualShadowStats)0;
        if (CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES == 0u)
        {
            statsBuffer[0].cumulativeUpgradePageAdmittedCount =
                previousStats.cumulativeUpgradePageAdmittedCount;
            statsBuffer[0].cumulativeUpgradePageRenderedCount =
                previousStats.cumulativeUpgradePageRenderedCount;
            statsBuffer[0].cumulativeUpgradeInvalidationInputCount =
                previousStats.cumulativeUpgradeInvalidationInputCount;
            statsBuffer[0].cumulativeUpgradeInvalidationAllocatedPageTouchCount =
                previousStats.cumulativeUpgradeInvalidationAllocatedPageTouchCount;
        }
        if (CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES != 0u)
        {
            statsBuffer[0].setupResetApplied = 1u;
        }
        statsBuffer[0].setupResetForced = CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_FORCED;
        statsBuffer[0].setupResetNoPreviousState = CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_NO_PREVIOUS_STATE;
        statsBuffer[0].setupResetStructureMismatch = CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_STRUCTURE_MISMATCH;
        statsBuffer[0].setupResetLightDirectionChanged = CLOD_VIRTUAL_SHADOW_SETUP_RESET_REASON_LIGHT_DIRECTION_CHANGED;
        statsBuffer[0].currentAllocationPercentage = currentAllocationPercentage;
        statsBuffer[0].targetPressureLodBias = targetPressureLodBias;
        statsBuffer[0].smoothedPressureLodBias = smoothedPressureLodBias;
        statsBuffer[0].framesSinceOverBudget = framesSinceOverBudget;
    }
    if (linearIndex < CLOD_VIRTUAL_SHADOW_SETUP_PHYSICAL_PAGE_COUNT)
    {
        if (CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES != 0u)
        {
            pageMetadata[linearIndex] = uint4(0u, 0u, 0u, 0u);
        }
        else
        {
            const uint4 meta = pageMetadata[linearIndex];
            if ((meta.z & kCLodVirtualShadowPhysicalPageResidentFlag) != 0u &&
                meta.w < CLOD_VIRTUAL_SHADOW_SETUP_CLIPMAP_COUNT)
            {
                const CLodVirtualShadowClipmapInfo ownerClipmapInfo = clipmapInfos[meta.w];
                const uint ownerVirtualAddress = meta.x;
                const uint2 ownerWrappedPageCoords = uint2(
                    ownerVirtualAddress % CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION,
                    ownerVirtualAddress / CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION);
                const uint2 ownerLogicalPageCoords = CLodVirtualShadowUnwrappedPageCoords(
                    ownerWrappedPageCoords,
                    ownerClipmapInfo);
                if (CLodVirtualShadowShouldClearWrappedPage(
                        int2(ownerLogicalPageCoords),
                        ownerClipmapInfo.clearOffsetX,
                        ownerClipmapInfo.clearOffsetY,
                        CLOD_VIRTUAL_SHADOW_SETUP_PAGE_TABLE_RESOLUTION))
                {
                    const uint allocationGeneration =
                        CLodVirtualShadowPhysicalPageAllocationGeneration(meta.z);
                    pageMetadata[linearIndex] = uint4(
                        0u,
                        0u,
                        CLodVirtualShadowPhysicalPageMetadataFlags(allocationGeneration, false),
                        0u);
                }
            }
        }
    }

    if (linearIndex < CLOD_VIRTUAL_SHADOW_SETUP_DIRTY_WORD_COUNT)
    {
        dirtyFlags[linearIndex] = 0u;
    }

    if (linearIndex == 0u)
    {
        allocationCount[0] = 0u;
        if (CLOD_VIRTUAL_SHADOW_SETUP_RESET_RESOURCES != 0u)
        {
            fallbackCandidateCount[0] = 0u;
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowClearPredictedPageDedupStateCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint64_t> scratchBitset =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_SCRATCH_BITSET_DESCRIPTOR_INDEX];
    const uint wordIndex = dispatchThreadId.x;
    if (wordIndex < kCLodVirtualShadowFallbackDependencyHashCapacity)
    {
        scratchBitset[wordIndex] = 0xFFFFFFFFFFFFFFFFull;
    }
    if (wordIndex == 0u)
    {
        RWStructuredBuffer<uint> outputPageCount =
            ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGE_COUNT_DESCRIPTOR_INDEX];
        outputPageCount[0] = 0u;
    }
}

void CLodVirtualShadowQueueFallbackDependencyRetry(
    CLodVirtualShadowPredictedRawPage rawPage,
    RWTexture2DArray<uint> pageTable,
    RWStructuredBuffer<uint4> pageMetadata,
    RWStructuredBuffer<uint> dirtyFlags,
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer)
{
    if (rawPage.physicalPageIndex >=
        CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PHYSICAL_PAGE_COUNT)
    {
        return;
    }
    const uint2 pageCoords = uint2(
        rawPage.virtualAddress % kCLodVirtualShadowMaxPageTableResolution,
        rawPage.virtualAddress / kCLodVirtualShadowMaxPageTableResolution);
    const uint3 tableCoords = uint3(pageCoords, rawPage.clipmapIndex);
    const uint pageEntry = pageTable[tableCoords];
    const uint4 meta = pageMetadata[rawPage.physicalPageIndex];
    if ((pageEntry & (kCLodVirtualShadowAllocatedMask |
            kCLodVirtualShadowContentValidMask |
            kCLodVirtualShadowRerenderedThisFrameMask)) !=
            (kCLodVirtualShadowAllocatedMask |
                kCLodVirtualShadowContentValidMask |
                kCLodVirtualShadowRerenderedThisFrameMask) ||
        (pageEntry & kCLodVirtualShadowPhysicalPageIndexMask) !=
            rawPage.physicalPageIndex ||
        meta.x != rawPage.virtualAddress ||
        meta.w != rawPage.clipmapIndex ||
        CLodVirtualShadowPhysicalPageAllocationGeneration(meta.z) !=
            rawPage.allocationGeneration ||
        meta.y != rawPage.contentGeneration)
    {
        return;
    }

    uint ignored = 0u;
    InterlockedOr(
        pageTable[tableCoords],
        kCLodVirtualShadowDirtyMask,
        ignored);
    InterlockedOr(
        dirtyFlags[rawPage.physicalPageIndex >> 5u],
        1u << (rawPage.physicalPageIndex & 31u));
    InterlockedAdd(statsBuffer[0].invalidUpgradeDependencyCount, 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowDeduplicatePredictedPagesCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<CLodVirtualShadowPredictedRawPage> rawPages =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> rawPageCountBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_RAW_PAGE_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint64_t> scratchBitset =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_SCRATCH_BITSET_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowPredictedPage> outputPages =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> outputPageCount =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_OUTPUT_PAGE_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_STATS_DESCRIPTOR_INDEX];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    RWTexture2DArray<uint> pageTable =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DEDUPLICATE_DIRTY_FLAGS_DESCRIPTOR_INDEX];

    const uint rawPageCount = min(rawPageCountBuffer[0], kCLodVirtualShadowPredictiveRawPageCapacity);
    const uint rawPageIndex = dispatchThreadId.x;
    if (rawPageIndex >= rawPageCount)
    {
        return;
    }

    const CLodVirtualShadowPredictedRawPage rawPage = rawPages[rawPageIndex];
    if (rawPage.clipmapIndex >= kCLodVirtualShadowClipmapCount ||
        rawPage.virtualAddress >= kCLodVirtualShadowPredictedPageEntriesPerClipmap)
    {
        return;
    }

    const uint64_t dependencyKey =
        (uint64_t(rawPage.sourceGroupGlobalIndex) << 32u) |
        uint64_t(rawPage.physicalPageIndex);
    uint hashSlot = uint(
        (dependencyKey ^ (dependencyKey >> 33u) ^ (dependencyKey >> 17u)) &
        uint64_t(kCLodVirtualShadowFallbackDependencyHashCapacity - 1u));
    bool inserted = false;
    [loop]
    for (uint probe = 0u; probe < 64u; ++probe)
    {
        uint64_t previousKey = 0u;
        InterlockedCompareExchange(
            scratchBitset[hashSlot],
            0xFFFFFFFFFFFFFFFFull,
            dependencyKey,
            previousKey);
        if (previousKey == 0xFFFFFFFFFFFFFFFFull)
        {
            inserted = true;
            break;
        }
        if (previousKey == dependencyKey)
        {
            return;
        }
        hashSlot = (hashSlot + 1u) &
            (kCLodVirtualShadowFallbackDependencyHashCapacity - 1u);
    }
    if (!inserted)
    {
        InterlockedAdd(statsBuffer[0].readyUpgradePageOverflowCount, 1u);
        CLodVirtualShadowQueueFallbackDependencyRetry(
            rawPage,
            pageTable,
            pageMetadata,
            dirtyFlags,
            statsBuffer);
        return;
    }

    uint outputPageIndex = 0u;
    InterlockedAdd(outputPageCount[0], 1u, outputPageIndex);
    if (outputPageIndex >= kCLodVirtualShadowPredictedPageListCapacity)
    {
        InterlockedAdd(statsBuffer[0].readyUpgradePageOverflowCount, 1u);
        InterlockedAdd(statsBuffer[0].cumulativeUpgradeQueueOverflowCount, 1u);
        CLodVirtualShadowQueueFallbackDependencyRetry(
            rawPage,
            pageTable,
            pageMetadata,
            dirtyFlags,
            statsBuffer);
        return;
    }

    CLodVirtualShadowPredictedPage outputPage;
    outputPage.sourceGroupGlobalIndex = rawPage.sourceGroupGlobalIndex;
    outputPage.physicalPageIndex = rawPage.physicalPageIndex;
    outputPage.allocationGeneration = rawPage.allocationGeneration;
    outputPage.contentGeneration = rawPage.contentGeneration;
    outputPage.clipmapIndex = rawPage.clipmapIndex;
    outputPage.virtualAddress = rawPage.virtualAddress;
    outputPage.perMeshInstanceBufferIndex = rawPage.perMeshInstanceBufferIndex;
    outputPage.pad0 = perFrameBuffer.frameIndex;
    outputPages[outputPageIndex] = outputPage;
    InterlockedAdd(statsBuffer[0].readyUpgradePageCount, 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowStampRenderedPageGenerationsCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2DArray<uint> pageTable =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_METADATA_DESCRIPTOR_INDEX];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    const uint physicalPageIndex = dispatchThreadId.x;
    if (physicalPageIndex == 0u)
    {
        RWStructuredBuffer<uint> rawPageCount =
            ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGE_COUNT_DESCRIPTOR_INDEX];
        rawPageCount[0] = 0u;
    }
    if (physicalPageIndex >= CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PHYSICAL_PAGE_COUNT)
    {
        return;
    }

    uint4 meta = pageMetadata[physicalPageIndex];
    if ((meta.z & kCLodVirtualShadowPhysicalPageResidentFlag) == 0u ||
        meta.w >= CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_COUNT)
    {
        return;
    }

    const uint resolution = kCLodVirtualShadowMaxPageTableResolution;
    const uint2 wrappedPageCoords = uint2(meta.x % resolution, meta.x / resolution);
    const uint pageEntry = pageTable[uint3(wrappedPageCoords, meta.w)];
    if ((pageEntry & (kCLodVirtualShadowAllocatedMask |
            kCLodVirtualShadowContentValidMask |
            kCLodVirtualShadowRerenderedThisFrameMask)) !=
        (kCLodVirtualShadowAllocatedMask |
            kCLodVirtualShadowContentValidMask |
            kCLodVirtualShadowRerenderedThisFrameMask) ||
        (pageEntry & kCLodVirtualShadowPhysicalPageIndexMask) != physicalPageIndex)
    {
        return;
    }

    const uint expectedClearEpoch =
        kCLodVirtualShadowClearEpochPendingMask |
        (perFrameBuffer.frameIndex &
            ~kCLodVirtualShadowClearEpochPendingMask);
    if (meta.y != expectedClearEpoch)
    {
        pageTable[uint3(wrappedPageCoords, meta.w)] =
            (pageEntry | kCLodVirtualShadowDirtyMask) &
            ~(kCLodVirtualShadowContentValidMask |
                kCLodVirtualShadowRerenderedThisFrameMask);
        RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
            ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_STATS_DESCRIPTOR_INDEX];
        InterlockedAdd(
            statsBuffer[0].renderedWithoutMatchingClearCount,
            1u);
        return;
    }

    meta.y = perFrameBuffer.frameIndex;
    pageMetadata[physicalPageIndex] = meta;

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[
            CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    const CLodVirtualShadowClipmapInfo clipmapInfo =
        clipmapInfos[meta.w];
    StructuredBuffer<Camera> shadowCameras =
        ResourceDescriptorHeap[
            ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const uint2 logicalPageCoords =
        CLodVirtualShadowUnwrappedPageCoords(
            wrappedPageCoords,
            clipmapInfo);
    const uint pageTag =
        CLodVirtualShadowPackAbsolutePageTag(
            logicalPageCoords,
            clipmapInfo.unwrappedPageOffsetX,
            clipmapInfo.unwrappedPageOffsetY);
    RWStructuredBuffer<float4> pageViewInfo =
        ResourceDescriptorHeap[
            CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_VIEW_INFO_DESCRIPTOR_INDEX];
    const Camera shadowCamera =
        shadowCameras[clipmapInfo.shadowCameraBufferIndex];
    pageViewInfo[physicalPageIndex] =
        float4(0.0f, 0.0f, shadowCamera.view[3].z, asfloat(pageTag));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowExpandPredictedPagesCSMain(
    uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWStructuredBuffer<CLodVirtualShadowPredictiveInvalidationCandidate> candidates =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> candidateCountBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATE_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowPredictedRawPage> rawPages =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> rawPageCount =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGE_COUNT_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> scratchBitset =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_SCRATCH_BITSET_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_STATS_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PAGE_METADATA_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> shadowCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactShadowCameras)];
    (void)scratchBitset;

    const uint appendedCandidateCount = candidateCountBuffer[0];
    const uint candidateCount = min(appendedCandidateCount, kCLodVirtualShadowPredictiveCandidateCapacity);
    const uint candidateIndex = dispatchThreadId.x;
    if (candidateIndex >= candidateCount)
    {
        return;
    }
    const CLodVirtualShadowPredictiveInvalidationCandidate candidate = candidates[candidateIndex];
    const uint clipmapIndex = candidate.shadowClipmapIndex;
    if (clipmapIndex >= kCLodVirtualShadowClipmapCount)
    {
        return;
    }
    const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
    if (!CLodVirtualShadowClipmapIsValid(clipmapInfo))
    {
        return;
    }

    float2 uvMin;
    float2 uvMax;
    if (!CLodProjectSphereToShadowUvBounds(
            candidate.worldCenterAndRadius.xyz,
            candidate.worldCenterAndRadius.w,
            shadowCameras[clipmapIndex],
            uvMin,
            uvMax))
    {
        return;
    }

    const uint2 logicalPageMin = CLodVirtualShadowVirtualPageCoordsFromUv(uvMin, clipmapInfo);
    const uint2 logicalPageMax = CLodVirtualShadowVirtualPageCoordsFromUv(uvMax, clipmapInfo);
    const uint2 pageMin = CLodVirtualShadowWrappedPageCoords(logicalPageMin, clipmapInfo);
    const uint2 pageMax = CLodVirtualShadowWrappedPageCoords(logicalPageMax, clipmapInfo);
    const bool wrapsX = pageMin.x > pageMax.x;
    const bool wrapsY = pageMin.y > pageMax.y;
    const uint segmentCountX = wrapsX ? 2u : 1u;
    const uint segmentCountY = wrapsY ? 2u : 1u;

        [loop]
        for (uint segmentY = 0u; segmentY < segmentCountY; ++segmentY)
        {
            const uint yStart = (segmentY == 0u) ? pageMin.y : 0u;
            const uint yEnd = !wrapsY
                ? pageMax.y
                : (segmentY == 0u
                    ? clipmapInfo.pageTableResolution - 1u
                    : pageMax.y);
            [loop]
            for (uint segmentX = 0u; segmentX < segmentCountX; ++segmentX)
            {
                const uint xStart = (segmentX == 0u) ? pageMin.x : 0u;
                const uint xEnd = !wrapsX
                    ? pageMax.x
                    : (segmentX == 0u
                        ? clipmapInfo.pageTableResolution - 1u
                        : pageMax.x);
                [loop]
                for (uint pageY = yStart; pageY <= yEnd; ++pageY)
                {
                    [loop]
                    for (uint pageX = xStart; pageX <= xEnd; ++pageX)
                    {
                        const uint2 wrappedPageCoords = uint2(pageX, pageY);
                        const uint3 pageCoords = uint3(
                            wrappedPageCoords,
                            clipmapInfo.pageTableLayer);
                        const uint pageEntry = pageTable[pageCoords];
                        if ((pageEntry & (kCLodVirtualShadowAllocatedMask |
                                kCLodVirtualShadowContentValidMask |
                                kCLodVirtualShadowRerenderedThisFrameMask)) !=
                            (kCLodVirtualShadowAllocatedMask |
                                kCLodVirtualShadowContentValidMask |
                                kCLodVirtualShadowRerenderedThisFrameMask))
                        {
                            continue;
                        }
                        const uint physicalPageIndex =
                            pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
                        if (physicalPageIndex >=
                            CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_PHYSICAL_PAGE_COUNT)
                        {
                            continue;
                        }
                        const uint4 pageMeta = pageMetadata[physicalPageIndex];
                        const uint virtualAddress =
                            CLodVirtualShadowWrappedVirtualAddress(
                                wrappedPageCoords,
                                clipmapInfo.pageTableResolution);
                        if ((pageMeta.z & kCLodVirtualShadowPhysicalPageResidentFlag) == 0u ||
                            pageMeta.x != virtualAddress ||
                            pageMeta.w != clipmapInfo.pageTableLayer)
                        {
                            continue;
                        }
                        CLodAppendVirtualShadowPredictedRawPage(
                            wrappedPageCoords,
                            clipmapInfo,
                            candidate.sourceGroupGlobalIndex,
                            candidate.perMeshInstanceBufferIndex,
                            pageEntry,
                            pageMeta,
                            rawPages,
                            rawPageCount);
                    }
                }
            }
        }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void CLodVirtualShadowResetFallbackCandidateCountCSMain()
{
    RWStructuredBuffer<uint> candidateCountBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_CANDIDATE_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> rawPageCount =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_RAW_PAGE_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_EXPAND_PREDICTED_PAGES_STATS_DESCRIPTOR_INDEX];
    const uint appendedCandidateCount = candidateCountBuffer[0];
    const uint emittedRawPageCount = rawPageCount[0];
    statsBuffer[0].upgradeCandidateInputCount = min(
        appendedCandidateCount,
        kCLodVirtualShadowPredictiveCandidateCapacity);
    statsBuffer[0].upgradeCandidateAppendOverflowCount =
        appendedCandidateCount > kCLodVirtualShadowPredictiveCandidateCapacity
        ? appendedCandidateCount - kCLodVirtualShadowPredictiveCandidateCapacity
        : 0u;
    statsBuffer[0].upgradeRawPageCount = min(
        emittedRawPageCount,
        kCLodVirtualShadowPredictiveRawPageCapacity);
    statsBuffer[0].upgradeRawPageOverflowCount =
        emittedRawPageCount > kCLodVirtualShadowPredictiveRawPageCapacity
        ? emittedRawPageCount - kCLodVirtualShadowPredictiveRawPageCapacity
        : 0u;
    statsBuffer[0].cumulativeUpgradeCandidateAppendOverflowCount +=
        statsBuffer[0].upgradeCandidateAppendOverflowCount;
    statsBuffer[0].cumulativeUpgradeQueueOverflowCount +=
        statsBuffer[0].upgradeRawPageOverflowCount;
    candidateCountBuffer[0] = 0u;
}

[shader("compute")]
[numthreads(kCLodVirtualShadowMarkTileSize, kCLodVirtualShadowMarkTileSize, 1)]
void CLodVirtualShadowBuildMarkTilesCSMain(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0u)
    {
        gCLodVirtualShadowMarkTileHasGeometry = 0u;
        gCLodVirtualShadowMarkTileMinDepthBits = 0x7F7FFFFFu;
        gCLodVirtualShadowMarkTileMaxDepthBits = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = groupId.xy * kCLodVirtualShadowMarkTileSize + groupThreadId.xy;
    if (pixel.x < CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_SCREEN_WIDTH &&
        pixel.y < CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_SCREEN_HEIGHT)
    {
        Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
        const uint depthBits = asuint(depthTexture[pixel]);
        if (depthBits != 0x7F7FFFFFu)
        {
            InterlockedOr(gCLodVirtualShadowMarkTileHasGeometry, 1u);
            InterlockedMin(gCLodVirtualShadowMarkTileMinDepthBits, depthBits);
            InterlockedMax(gCLodVirtualShadowMarkTileMaxDepthBits, depthBits);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (groupIndex == 0u && gCLodVirtualShadowMarkTileHasGeometry != 0u)
    {
        RWStructuredBuffer<CLodVirtualShadowMarkTileWorkItem> tileWorkBuffer =
            ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_TILE_WORK_DESCRIPTOR_INDEX];
        RWStructuredBuffer<uint> tileCountBuffer =
            ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_TILE_COUNT_DESCRIPTOR_INDEX];

        uint tileIndex = 0u;
        InterlockedAdd(tileCountBuffer[0], 1u, tileIndex);
        if (tileIndex < CLOD_VIRTUAL_SHADOW_BUILD_MARK_TILES_MAX_TILE_COUNT)
        {
            CLodVirtualShadowMarkTileWorkItem tileWorkItem;
            tileWorkItem.tileCoordX = groupId.x;
            tileWorkItem.tileCoordY = groupId.y;
            tileWorkItem.minDepthBits = gCLodVirtualShadowMarkTileMinDepthBits;
            tileWorkItem.maxDepthBits = gCLodVirtualShadowMarkTileMaxDepthBits;
            tileWorkBuffer[tileIndex] = tileWorkItem;
        }
    }
}

void CLodVirtualShadowAccumulateTileClipmapRange(
    float2 sampleUv,
    float depth,
    CLodVirtualShadowMainCameraInfo mainCamera,
    float clip0TexelWorldSize,
    float directionalLodBias,
    uint activeClipmapCount,
    inout uint minClipmapIndex,
    inout uint maxClipmapIndex)
{
    const float3 positionWS = CLodWorldSpaceFromScreenUv(sampleUv, depth, mainCamera);
    const uint clipmapIndex = CLodVirtualShadowSelectClipmapIndex(
        positionWS,
        mainCamera.positionWorldSpace.xyz,
        clip0TexelWorldSize,
        directionalLodBias,
        activeClipmapCount);
    minClipmapIndex = min(minClipmapIndex, clipmapIndex);
    maxClipmapIndex = max(maxClipmapIndex, clipmapIndex);
}

float CLodViewRayLengthFromScreenUv(float2 screenUv, CLodVirtualShadowMainCameraInfo camera)
{
    const float2 ndc = float2(screenUv.x * 2.0f - 1.0f, (1.0f - screenUv.y) * 2.0f - 1.0f);
    const float4 clipPos = float4(ndc, 1.0f, 1.0f);
    const float4 viewPosH = mul(clipPos, camera.projectionInverse);
    return max(length(viewPosH.xyz), 1.0e-5f);
}

float CLodVirtualShadowRepresentativeDepthForClipmap(
    float2 sampleUv,
    float minDepth,
    float maxDepth,
    CLodVirtualShadowMainCameraInfo mainCamera,
    float clip0TexelWorldSize,
    float directionalLodBias,
    uint clipmapIndex)
{
    const float pageCount = (float)kCLodVirtualShadowFixedVirtualPageCountPerAxis;
    const float scaleRatio = pageCount > 0.0f ? max((pageCount - 2.0f) / pageCount, 0.0f) : 1.0f;
    const float clip0FrustumScale = 0.5f * max(clip0TexelWorldSize, 1.0e-5f) * (float)kCLodVirtualShadowFixedVirtualResolution;
    const float baseScale = max(clip0FrustumScale * scaleRatio, 1.0e-5f);
    const float clipLevelCenter = (float)clipmapIndex - directionalLodBias - 0.5f;
    const float representativeDistance = baseScale * exp2(clipLevelCenter);
    const float representativeDepth = representativeDistance / CLodViewRayLengthFromScreenUv(sampleUv, mainCamera);
    return clamp(representativeDepth, min(minDepth, maxDepth), max(minDepth, maxDepth));
}

void CLodVirtualShadowAccumulateTileProjectedBounds(
    float2 sampleUv,
    float depth,
    CLodVirtualShadowMainCameraInfo mainCamera,
    row_major matrix shadowViewProjection,
    inout float2 shadowUvMin,
    inout float2 shadowUvMax,
    inout float minProjectedZ,
    inout float maxProjectedZ)
{
    const float3 positionWS = CLodWorldSpaceFromScreenUv(sampleUv, depth, mainCamera);
    const float3 projected = CLodProjectWorldToShadowProjected(positionWS, shadowViewProjection);
    minProjectedZ = min(minProjectedZ, projected.z);
    maxProjectedZ = max(maxProjectedZ, projected.z);

    float2 shadowUv = projected.xy * 0.5f + 0.5f;
    shadowUv.y = 1.0f - shadowUv.y;
    shadowUvMin = min(shadowUvMin, shadowUv);
    shadowUvMax = max(shadowUvMax, shadowUv);
}

void CLodVirtualShadowAccumulateTileProjectedBoundsForClipmap(
    float2 sampleUv,
    float depth,
    uint targetClipmapIndex,
    CLodVirtualShadowMainCameraInfo mainCamera,
    float clip0TexelWorldSize,
    float directionalLodBias,
    uint activeClipmapCount,
    row_major matrix shadowViewProjection,
    inout float2 shadowUvMin,
    inout float2 shadowUvMax,
    inout float minProjectedZ,
    inout float maxProjectedZ,
    inout bool hasSample)
{
    const float3 positionWS = CLodWorldSpaceFromScreenUv(sampleUv, depth, mainCamera);
    const uint selectedClipmapIndex = CLodVirtualShadowSelectClipmapIndex(
        positionWS,
        mainCamera.positionWorldSpace.xyz,
        clip0TexelWorldSize,
        directionalLodBias,
        activeClipmapCount);
    if (selectedClipmapIndex != targetClipmapIndex)
    {
        return;
    }

    const float3 projected = CLodProjectWorldToShadowProjected(positionWS, shadowViewProjection);
    minProjectedZ = min(minProjectedZ, projected.z);
    maxProjectedZ = max(maxProjectedZ, projected.z);

    float2 shadowUv = projected.xy * 0.5f + 0.5f;
    shadowUv.y = 1.0f - shadowUv.y;
    shadowUvMin = min(shadowUvMin, shadowUv);
    shadowUvMax = max(shadowUvMax, shadowUv);
    hasSample = true;
}

[shader("compute")]
[numthreads(128, 1, 1)]
void CLodVirtualShadowMarkBlocksCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> tileCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_COUNT_DESCRIPTOR_INDEX];
    const uint tileCount = tileCountBuffer.Load(0);
    const uint tileIndex = dispatchThreadId.x;
    if (tileIndex >= tileCount)
    {
        return;
    }

    StructuredBuffer<CLodVirtualShadowMarkTileWorkItem> tileWorkBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_TILE_WORK_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowMainCameraInfo> compactMainCameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactMainCamera)];
    StructuredBuffer<CLodVirtualShadowMarkClipmapData> markClipmapDataBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> markedBlockMasks = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_MASK_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> markedBlockList = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_LIST_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> markedBlockCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_COUNT_DESCRIPTOR_INDEX];

    const CLodVirtualShadowMarkTileWorkItem tileWork = tileWorkBuffer[tileIndex];
    const float minDepth = asfloat(tileWork.minDepthBits);
    const float maxDepth = asfloat(tileWork.maxDepthBits);
    if (tileWork.minDepthBits == 0x7F7FFFFFu)
    {
        return;
    }

    const uint activeClipmapCount = CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_ACTIVE_CLIPMAP_COUNT;
    if (activeClipmapCount == 0u)
    {
        return;
    }

    const float2 screenSize = float2(CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_WIDTH, CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_HEIGHT);
    const uint2 tilePixelMin = uint2(tileWork.tileCoordX, tileWork.tileCoordY) * kCLodVirtualShadowMarkTileSize;
    const uint2 tilePixelMax = min(
        tilePixelMin + kCLodVirtualShadowMarkTileSize,
        uint2(CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_WIDTH, CLOD_VIRTUAL_SHADOW_MARK_BLOCKS_SCREEN_HEIGHT));

    const float2 tileUvMin = float2(tilePixelMin) / screenSize;
    const float2 tileUvMax = float2(tilePixelMax) / screenSize;
    const float2 tileUvCenter = 0.5f * (tileUvMin + tileUvMax);

    const CLodVirtualShadowMainCameraInfo mainCamera = compactMainCameraBuffer[0];

    uint minClipmapIndex = activeClipmapCount - 1u;
    uint maxClipmapIndex = 0u;
#define CLOD_ACCUMULATE_TILE_CLIPMAP(sampleUvValue, depthValue) \
    CLodVirtualShadowAccumulateTileClipmapRange( \
        sampleUvValue, \
        depthValue, \
        mainCamera, \
        markClipmapDataBuffer[0].texelWorldSize, \
        markClipmapDataBuffer[0].directionalLodBias, \
        activeClipmapCount, \
        minClipmapIndex, \
        maxClipmapIndex)

    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMin, minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMax.x, tileUvMin.y), minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMin.x, tileUvMax.y), minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMax, minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvCenter, minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMin, maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMax.x, tileUvMin.y), maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMin.x, tileUvMax.y), maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMax, maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvCenter, maxDepth);

#undef CLOD_ACCUMULATE_TILE_CLIPMAP

    [loop]
    for (uint clipmapIndex = minClipmapIndex; clipmapIndex <= maxClipmapIndex; ++clipmapIndex)
    {
        const CLodVirtualShadowMarkClipmapData clipmapData = markClipmapDataBuffer[clipmapIndex];
        if (!CLodVirtualShadowMarkClipmapIsValid(clipmapData))
        {
            continue;
        }

        float2 shadowUvMin = float2(1.0e30f, 1.0e30f);
        float2 shadowUvMax = float2(-1.0e30f, -1.0e30f);
        float minProjectedZ = 1.0e30f;
        float maxProjectedZ = -1.0e30f;
        bool hasClipmapSample = false;

#define CLOD_ACCUMULATE_TILE_BOUNDS(sampleUvValue, depthValue) \
        CLodVirtualShadowAccumulateTileProjectedBoundsForClipmap( \
            sampleUvValue, \
            depthValue, \
            clipmapIndex, \
            mainCamera, \
            markClipmapDataBuffer[0].texelWorldSize, \
            markClipmapDataBuffer[0].directionalLodBias, \
            activeClipmapCount, \
            clipmapData.shadowViewProjection, \
            shadowUvMin, \
            shadowUvMax, \
            minProjectedZ, \
            maxProjectedZ, \
            hasClipmapSample)

        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMin, minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMax.x, tileUvMin.y), minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMin.x, tileUvMax.y), minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMax, minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvCenter, minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMin, maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMax.x, tileUvMin.y), maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMin.x, tileUvMax.y), maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMax, maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvCenter, maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(
            tileUvMin,
            CLodVirtualShadowRepresentativeDepthForClipmap(
                tileUvMin,
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            float2(tileUvMax.x, tileUvMin.y),
            CLodVirtualShadowRepresentativeDepthForClipmap(
                float2(tileUvMax.x, tileUvMin.y),
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            float2(tileUvMin.x, tileUvMax.y),
            CLodVirtualShadowRepresentativeDepthForClipmap(
                float2(tileUvMin.x, tileUvMax.y),
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            tileUvMax,
            CLodVirtualShadowRepresentativeDepthForClipmap(
                tileUvMax,
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            tileUvCenter,
            CLodVirtualShadowRepresentativeDepthForClipmap(
                tileUvCenter,
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));

#undef CLOD_ACCUMULATE_TILE_BOUNDS

        if (!hasClipmapSample)
        {
            continue;
        }

        if (maxProjectedZ < 0.0f || minProjectedZ > 1.0f)
        {
            continue;
        }

        const float2 pagePaddingUv = 0.5f / max((float)clipmapData.pageTableResolution, 1.0f);
        shadowUvMin = saturate(shadowUvMin - pagePaddingUv);
        shadowUvMax = saturate(shadowUvMax + pagePaddingUv);

        const uint2 logicalPageMin = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUvMin, clipmapData);
        const uint2 logicalPageMax = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUvMax, clipmapData);
        const uint2 logicalBlockMin = CLodVirtualShadowBlockCoordFromPageCoord(logicalPageMin);
        const uint2 logicalBlockMax = CLodVirtualShadowBlockCoordFromPageCoord(logicalPageMax);

        [loop]
        for (uint blockY = logicalBlockMin.y; blockY <= logicalBlockMax.y; ++blockY)
        {
            [loop]
            for (uint blockX = logicalBlockMin.x; blockX <= logicalBlockMax.x; ++blockX)
            {
                const uint2 blockCoord = uint2(blockX, blockY);
                const uint2 blockOriginPage = CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord);
                const uint activeMask = CLodVirtualShadowBlockMaskForPageRect(
                    logicalPageMin,
                    logicalPageMax,
                    blockOriginPage);
                if (activeMask == 0u)
                {
                    continue;
                }

                const uint blockLinearIndex = CLodVirtualShadowBlockLinearIndex(blockCoord, clipmapIndex);
                uint previousMask = 0u;
                InterlockedOr(markedBlockMasks[blockLinearIndex], activeMask, previousMask);
                if (previousMask == 0u)
                {
                    uint blockListIndex = 0u;
                    InterlockedAdd(markedBlockCountBuffer[0], 1u, blockListIndex);
                    if (blockListIndex < kCLodVirtualShadowMaxMarkedBlockCount)
                    {
                        markedBlockList[blockListIndex] = blockLinearIndex;
                    }
                }
            }
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowResolveMarkedBlocksCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> markedBlockMasks = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MASK_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> markedBlockList = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_LIST_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> markedBlockCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> allocationRequests = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUESTS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> allocationCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_REQUEST_COUNT_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<float4> directionalPageViewInfo = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_PAGE_VIEW_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_STATS_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowMarkClipmapData> markClipmapDataBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_CLIPMAP_DATA_DESCRIPTOR_INDEX];

    const uint markedBlockCount = min(markedBlockCountBuffer.Load(0), kCLodVirtualShadowMaxMarkedBlockCount);
    const uint markedBlockIndex = dispatchThreadId.x;
    if (markedBlockIndex >= markedBlockCount)
    {
        return;
    }

    const uint blockLinearIndex = markedBlockList[markedBlockIndex];
    uint2 blockCoord = uint2(0u, 0u);
    uint clipmapIndex = 0u;
    CLodVirtualShadowUnpackBlockLinearIndex(blockLinearIndex, blockCoord, clipmapIndex);
    if (clipmapIndex >= CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_ACTIVE_CLIPMAP_COUNT)
    {
        return;
    }

    const CLodVirtualShadowMarkClipmapData clipmapData = markClipmapDataBuffer[clipmapIndex];
    if (!CLodVirtualShadowMarkClipmapIsValid(clipmapData))
    {
        return;
    }

    const uint activeMask = markedBlockMasks[blockLinearIndex];
    if (activeMask == 0u)
    {
        return;
    }

    const uint2 blockOriginPage = CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord);
    [unroll]
    for (uint localPageY = 0u; localPageY < kCLodVirtualShadowBlockPagesPerAxis; ++localPageY)
    {
        [unroll]
        for (uint localPageX = 0u; localPageX < kCLodVirtualShadowBlockPagesPerAxis; ++localPageX)
        {
            const uint2 localPageCoord = uint2(localPageX, localPageY);
            const uint localPageBit = 1u << CLodVirtualShadowBlockLocalPageIndex(localPageCoord);
            if ((activeMask & localPageBit) == 0u)
            {
                continue;
            }

            const uint2 logicalPageCoord = blockOriginPage + localPageCoord;
            if (logicalPageCoord.x >= clipmapData.pageTableResolution || logicalPageCoord.y >= clipmapData.pageTableResolution)
            {
                continue;
            }

            CLodMarkVirtualShadowWrappedPage(
                CLodVirtualShadowWrappedPageCoords(logicalPageCoord, clipmapData),
                clipmapData.pageTableLayer,
                clipmapData.pageTableResolution,
                CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_MAX_REQUEST_COUNT,
                CLOD_VIRTUAL_SHADOW_RESOLVE_MARKED_BLOCKS_ACTIVE_CLIPMAP_COUNT,
                CLodVirtualShadowPackAbsolutePageTag(
                    logicalPageCoord,
                    clipmapData.unwrappedPageOffsetX,
                    clipmapData.unwrappedPageOffsetY),
                allocationRequests,
                allocationCountBuffer,
                pageTable,
                dirtyFlags,
                directionalPageViewInfo,
                statsBuffer);
        }
    }
}

[shader("compute")]
[numthreads(128, 1, 1)]
void CLodVirtualShadowMarkPagesCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> tileCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_TILE_COUNT_DESCRIPTOR_INDEX];
    const uint tileCount = tileCountBuffer.Load(0);
    const uint tileIndex = dispatchThreadId.x;
    if (tileIndex >= tileCount)
    {
        return;
    }

    StructuredBuffer<CLodVirtualShadowMarkTileWorkItem> tileWorkBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_TILE_WORK_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowMainCameraInfo> compactMainCameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactMainCamera)];
    StructuredBuffer<CLodVirtualShadowMarkClipmapData> markClipmapDataBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_CLIPMAP_DATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> allocationRequests = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_REQUESTS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> allocationCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_REQUEST_COUNT_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<float4> directionalPageViewInfo = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_PAGE_VIEW_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_MARK_STATS_DESCRIPTOR_INDEX];

    const CLodVirtualShadowMarkTileWorkItem tileWork = tileWorkBuffer[tileIndex];
    const float minDepth = asfloat(tileWork.minDepthBits);
    const float maxDepth = asfloat(tileWork.maxDepthBits);
    if (tileWork.minDepthBits == 0x7F7FFFFFu)
    {
        return;
    }

    const uint activeClipmapCount = CLOD_VIRTUAL_SHADOW_MARK_ACTIVE_CLIPMAP_COUNT;
    if (activeClipmapCount == 0u)
    {
        return;
    }

    const float2 screenSize = float2(CLOD_VIRTUAL_SHADOW_MARK_SCREEN_WIDTH, CLOD_VIRTUAL_SHADOW_MARK_SCREEN_HEIGHT);
    const uint2 tilePixelMin = uint2(tileWork.tileCoordX, tileWork.tileCoordY) * kCLodVirtualShadowMarkTileSize;
    const uint2 tilePixelMax = min(
        tilePixelMin + kCLodVirtualShadowMarkTileSize,
        uint2(CLOD_VIRTUAL_SHADOW_MARK_SCREEN_WIDTH, CLOD_VIRTUAL_SHADOW_MARK_SCREEN_HEIGHT));

    const float2 tileUvMin = float2(tilePixelMin) / screenSize;
    const float2 tileUvMax = float2(tilePixelMax) / screenSize;
    const float2 tileUvCenter = 0.5f * (tileUvMin + tileUvMax);

    const CLodVirtualShadowMainCameraInfo mainCamera = compactMainCameraBuffer[0];

    uint minClipmapIndex = activeClipmapCount - 1u;
    uint maxClipmapIndex = 0u;
#define CLOD_ACCUMULATE_TILE_CLIPMAP(sampleUvValue, depthValue) \
    CLodVirtualShadowAccumulateTileClipmapRange( \
        sampleUvValue, \
        depthValue, \
        mainCamera, \
        markClipmapDataBuffer[0].texelWorldSize, \
        markClipmapDataBuffer[0].directionalLodBias, \
        activeClipmapCount, \
        minClipmapIndex, \
        maxClipmapIndex)

    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMin, minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMax.x, tileUvMin.y), minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMin.x, tileUvMax.y), minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMax, minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvCenter, minDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMin, maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMax.x, tileUvMin.y), maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(float2(tileUvMin.x, tileUvMax.y), maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvMax, maxDepth);
    CLOD_ACCUMULATE_TILE_CLIPMAP(tileUvCenter, maxDepth);

#undef CLOD_ACCUMULATE_TILE_CLIPMAP

    [loop]
    for (uint clipmapIndex = minClipmapIndex; clipmapIndex <= maxClipmapIndex; ++clipmapIndex)
    {
        const CLodVirtualShadowMarkClipmapData clipmapData = markClipmapDataBuffer[clipmapIndex];
        if (!CLodVirtualShadowMarkClipmapIsValid(clipmapData))
        {
            continue;
        }

        float2 shadowUvMin = float2(1.0e30f, 1.0e30f);
        float2 shadowUvMax = float2(-1.0e30f, -1.0e30f);
        float minProjectedZ = 1.0e30f;
        float maxProjectedZ = -1.0e30f;
        bool hasClipmapSample = false;

#define CLOD_ACCUMULATE_TILE_BOUNDS(sampleUvValue, depthValue) \
        CLodVirtualShadowAccumulateTileProjectedBoundsForClipmap( \
            sampleUvValue, \
            depthValue, \
            clipmapIndex, \
            mainCamera, \
            markClipmapDataBuffer[0].texelWorldSize, \
            markClipmapDataBuffer[0].directionalLodBias, \
            activeClipmapCount, \
            clipmapData.shadowViewProjection, \
            shadowUvMin, \
            shadowUvMax, \
            minProjectedZ, \
            maxProjectedZ, \
            hasClipmapSample)

        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMin, minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMax.x, tileUvMin.y), minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMin.x, tileUvMax.y), minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMax, minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvCenter, minDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMin, maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMax.x, tileUvMin.y), maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(float2(tileUvMin.x, tileUvMax.y), maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvMax, maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(tileUvCenter, maxDepth);
        CLOD_ACCUMULATE_TILE_BOUNDS(
            tileUvMin,
            CLodVirtualShadowRepresentativeDepthForClipmap(
                tileUvMin,
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            float2(tileUvMax.x, tileUvMin.y),
            CLodVirtualShadowRepresentativeDepthForClipmap(
                float2(tileUvMax.x, tileUvMin.y),
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            float2(tileUvMin.x, tileUvMax.y),
            CLodVirtualShadowRepresentativeDepthForClipmap(
                float2(tileUvMin.x, tileUvMax.y),
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            tileUvMax,
            CLodVirtualShadowRepresentativeDepthForClipmap(
                tileUvMax,
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));
        CLOD_ACCUMULATE_TILE_BOUNDS(
            tileUvCenter,
            CLodVirtualShadowRepresentativeDepthForClipmap(
                tileUvCenter,
                minDepth,
                maxDepth,
                mainCamera,
                markClipmapDataBuffer[0].texelWorldSize,
                markClipmapDataBuffer[0].directionalLodBias,
                clipmapIndex));

#undef CLOD_ACCUMULATE_TILE_BOUNDS

        if (!hasClipmapSample)
        {
            continue;
        }

        if (maxProjectedZ < 0.0f || minProjectedZ > 1.0f)
        {
            continue;
        }

        const float2 pagePaddingUv = 0.5f / max((float)clipmapData.pageTableResolution, 1.0f);
        shadowUvMin = saturate(shadowUvMin - pagePaddingUv);
        shadowUvMax = saturate(shadowUvMax + pagePaddingUv);

        const uint2 logicalPageMin = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUvMin, clipmapData);
        const uint2 logicalPageMax = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUvMax, clipmapData);
        for (uint pageY = logicalPageMin.y; pageY <= logicalPageMax.y; ++pageY)
        {
            for (uint pageX = logicalPageMin.x; pageX <= logicalPageMax.x; ++pageX)
            {
                CLodMarkVirtualShadowWrappedPage(
                    CLodVirtualShadowWrappedPageCoords(uint2(pageX, pageY), clipmapData),
                    clipmapData.pageTableLayer,
                    clipmapData.pageTableResolution,
                    CLOD_VIRTUAL_SHADOW_MARK_MAX_REQUEST_COUNT,
                    activeClipmapCount,
                    CLodVirtualShadowPackAbsolutePageTag(
                        uint2(pageX, pageY),
                        clipmapData.unwrappedPageOffsetX,
                        clipmapData.unwrappedPageOffsetY),
                    allocationRequests,
                    allocationCountBuffer,
                    pageTable,
                    dirtyFlags,
                    directionalPageViewInfo,
                    statsBuffer);
            }
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowInvalidatePagesCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo>
        compactShadowCameraBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(
                Builtin::Shadows::CLodCompactShadowCameras)];
    StructuredBuffer<CLodVirtualShadowInvalidationInput> invalidationInputs = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_INPUTS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> invalidationCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_INPUT_COUNT_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<float4> directionalPageViewInfo = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_PAGE_VIEW_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_INVALIDATE_STATS_DESCRIPTOR_INDEX];

    const uint inputIndex = dispatchThreadId.x;
    const uint inputCount = invalidationCountBuffer.Load(0);
    if (inputIndex >= inputCount)
    {
        return;
    }

    const CLodVirtualShadowInvalidationInput input = invalidationInputs[inputIndex];
    const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDraw(input.perMeshInstanceBufferIndex);
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(input.perMeshInstanceBufferIndex);
    const float baseRadius = instanceData.boundingSphere.sphere.w;
    const float skinnedScale = ((input.flags & kCLodVirtualShadowInvalidationFlagSkinned) != 0u)
        ? max(instanceData.skinnedBoundsScale, 1.0f)
        : 1.0f;
    const uint clipmapCount = min(CLOD_VIRTUAL_SHADOW_INVALIDATE_CLIPMAP_COUNT, kCLodVirtualShadowClipmapCount);

    if ((input.flags & kCLodVirtualShadowInvalidationFlagUseCurrentBounds) != 0u)
    {
        const float currentScale = CLodMaxAxisScale_RowVector(objectData.model) * skinnedScale;
        const float3 currentCenter = mul(float4(instanceData.boundingSphere.sphere.xyz, 1.0f), objectData.model).xyz;
        CLodInvalidateVirtualShadowSphere(
            currentCenter,
            baseRadius * currentScale,
            clipmapCount,
            perFrameBuffer.frameIndex,
            kCLodVirtualShadowInvalidationStatsReasonCurrentBounds |
                (((input.flags & kCLodVirtualShadowInvalidationFlagSkinned) != 0u) ? kCLodVirtualShadowInvalidationStatsReasonSkinned : 0u),
            compactShadowCameraBuffer,
            clipmapInfos,
            pageTable,
            dirtyFlags,
            pageMetadata,
            directionalPageViewInfo,
            statsBuffer);
    }

    if ((input.flags & kCLodVirtualShadowInvalidationFlagUsePreviousBounds) != 0u)
    {
        const float previousScale = CLodMaxAxisScale_RowVector(objectData.prevModel) * skinnedScale;
        const float3 previousCenter = mul(float4(instanceData.boundingSphere.sphere.xyz, 1.0f), objectData.prevModel).xyz;
        CLodInvalidateVirtualShadowSphere(
            previousCenter,
            baseRadius * previousScale,
            clipmapCount,
            perFrameBuffer.frameIndex,
            kCLodVirtualShadowInvalidationStatsReasonPreviousBounds |
                (((input.flags & kCLodVirtualShadowInvalidationFlagSkinned) != 0u) ? kCLodVirtualShadowInvalidationStatsReasonSkinned : 0u),
            compactShadowCameraBuffer,
            clipmapInfos,
            pageTable,
            dirtyFlags,
            pageMetadata,
            directionalPageViewInfo,
            statsBuffer);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowBuildPageListsCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_DESCRIPTOR_INDEX];
    StructuredBuffer<uint4> pageMetadata = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> freePhysicalPages = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_FREE_PAGES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> reusablePhysicalPages = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_REUSABLE_PAGES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageListHeader = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_HEADER_DESCRIPTOR_INDEX];

    if (dispatchThreadId.x == 0u)
    {
        pageListHeader[0] = uint4(0u, 0u, 0u, 0u);
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint physicalPageIndex = dispatchThreadId.x;
        physicalPageIndex < CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PHYSICAL_PAGE_COUNT;
        physicalPageIndex += 64u)
    {
        const uint4 meta = pageMetadata[physicalPageIndex];
        const uint metaFlags = meta.z;
        const bool appendFree = (metaFlags & kCLodVirtualShadowPhysicalPageResidentFlag) == 0u;
        if (appendFree)
        {
            uint freeIndex = 0u;
            bool freeIndexValid = false;
            CLodVirtualShadowWaveReservePageListSlot(
                pageListHeader,
                0u,
                CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PHYSICAL_PAGE_COUNT,
                true,
                freeIndex,
                freeIndexValid);
            if (freeIndexValid)
            {
                freePhysicalPages[freeIndex] = physicalPageIndex;
            }

            continue;
        }

        const uint ownerVirtualAddress = meta.x;
        const uint ownerClipmapIndex = meta.w;
        const uint ownerPageX = ownerVirtualAddress % CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_RESOLUTION;
        const uint ownerPageY = ownerVirtualAddress / CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_RESOLUTION;
        bool visitedThisFrame = false;
        if (ownerClipmapIndex < kCLodVirtualShadowClipmapCount &&
            ownerPageY < CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PAGE_TABLE_RESOLUTION)
        {
            const uint pageEntry = pageTable.Load(int4(ownerPageX, ownerPageY, ownerClipmapIndex, 0));
            visitedThisFrame =
                (pageEntry & (kCLodVirtualShadowAllocatedMask | kCLodVirtualShadowVisitedMask)) ==
                    (kCLodVirtualShadowAllocatedMask | kCLodVirtualShadowVisitedMask) &&
                (pageEntry & kCLodVirtualShadowPhysicalPageIndexMask) == physicalPageIndex;
        }

        const bool appendReusable = !visitedThisFrame;
        if (appendReusable)
        {
            uint reusableIndex = 0u;
            bool reusableIndexValid = false;
            CLodVirtualShadowWaveReservePageListSlot(
                pageListHeader,
                1u,
                CLOD_VIRTUAL_SHADOW_BUILD_PAGE_LISTS_PHYSICAL_PAGE_COUNT,
                true,
                reusableIndex,
                reusableIndexValid);
            if (reusableIndexValid)
            {
                reusablePhysicalPages[reusableIndex] = physicalPageIndex;
            }
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowAllocatePagesCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<uint4> allocationRequests = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUESTS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> allocationCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_REQUEST_COUNT_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> freePhysicalPages = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_FREE_PAGES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> reusablePhysicalPages = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_REUSABLE_PAGES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint4> pageListHeader = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_LIST_HEADER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ALLOCATE_STATS_DESCRIPTOR_INDEX];

    const uint requestIndex = dispatchThreadId.x;
    const uint requestCount = allocationCountBuffer.Load(0);
    const uint totalBudget = CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_RENDER_BUDGET;
    if (requestIndex == 0u)
    {
        statsBuffer[0].configuredPageRenderBudget = totalBudget;
    }
    if (requestIndex >= requestCount)
    {
        return;
    }
    const uint freePageCount = min(pageListHeader[0].x, CLOD_VIRTUAL_SHADOW_ALLOCATE_PHYSICAL_PAGE_COUNT);
    const uint reusablePageCount = min(pageListHeader[0].y, CLOD_VIRTUAL_SHADOW_ALLOCATE_PHYSICAL_PAGE_COUNT);

    uint selectedPhysicalPageIndex = CLOD_VIRTUAL_SHADOW_ALLOCATE_PHYSICAL_PAGE_COUNT;
    if (requestIndex < freePageCount)
    {
        selectedPhysicalPageIndex = freePhysicalPages[requestIndex];
    }
    else if ((requestIndex - freePageCount) < reusablePageCount)
    {
        selectedPhysicalPageIndex = reusablePhysicalPages[requestIndex - freePageCount];
    }
    else
    {
        return;
    }

    const uint4 request = allocationRequests[requestIndex];
    const uint virtualAddress = request.x;
    const uint clipmapIndex = request.y;
    if (clipmapIndex >= CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_COUNT)
    {
        return;
    }

    const uint pageX = virtualAddress % CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION;
    const uint pageY = virtualAddress / CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION;
    if (pageY >= CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION ||
        selectedPhysicalPageIndex >= CLOD_VIRTUAL_SHADOW_ALLOCATE_PHYSICAL_PAGE_COUNT)
    {
        return;
    }

    // A new mapping has no valid contents. Reserve its normal admission ticket
    // before changing either owner so every allocated page is guaranteed to be
    // cleared and rasterized in this frame. This also makes allocation and
    // retry admission share one normal budget instead of allowing allocation
    // to create deferred mappings whose physical contents belong to an old
    // virtual page.
    uint normalTicket = 0u;
    InterlockedAdd(statsBuffer[0].normalEligiblePageCount, 1u, normalTicket);
    if (totalBudget != 0u && normalTicket >= totalBudget)
    {
        InterlockedAdd(statsBuffer[0].normalDeferredPageCount, 1u);
        return;
    }

    const uint4 previousMeta = pageMetadata[selectedPhysicalPageIndex];
    if ((previousMeta.z & kCLodVirtualShadowPhysicalPageResidentFlag) != 0u &&
        (previousMeta.x != virtualAddress || previousMeta.w != clipmapIndex))
    {
        const uint previousVirtualAddress = previousMeta.x;
        const uint previousClipmapIndex = previousMeta.w;
        const uint previousPageX = previousVirtualAddress % CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION;
        const uint previousPageY = previousVirtualAddress / CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION;
        if (previousPageY < CLOD_VIRTUAL_SHADOW_ALLOCATE_PAGE_TABLE_RESOLUTION &&
            previousClipmapIndex < CLOD_VIRTUAL_SHADOW_ALLOCATE_CLIPMAP_COUNT)
        {
            pageTable[uint3(previousPageX, previousPageY, previousClipmapIndex)] = 0u;
        }
    }

    const uint pageEntry = selectedPhysicalPageIndex |
        kCLodVirtualShadowAllocatedMask |
        kCLodVirtualShadowDirtyMask |
        kCLodVirtualShadowVisitedMask |
        kCLodVirtualShadowAdmittedThisFrameMask;
    pageTable[uint3(pageX, pageY, clipmapIndex)] = pageEntry;
    const uint nextAllocationGeneration =
        CLodVirtualShadowPhysicalPageAllocationGeneration(previousMeta.z) + 1u;
    pageMetadata[selectedPhysicalPageIndex] = uint4(
        virtualAddress,
        0u,
        CLodVirtualShadowPhysicalPageMetadataFlags(nextAllocationGeneration, true),
        clipmapIndex);
    InterlockedOr(dirtyFlags[selectedPhysicalPageIndex >> 5u], 1u << (selectedPhysicalPageIndex & 31u));
    InterlockedAdd(statsBuffer[0].admittedPageCount, 1u);
    InterlockedAdd(statsBuffer[0].normalAdmittedPageCount, 1u);
    InterlockedAdd(statsBuffer[0].newlyAllocatedPageCount, 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowApplyExactUpgradesCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<CLodVirtualShadowUpgradeInvalidationInput> inputs =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_INPUTS_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint4> pageMetadata =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_PAGE_METADATA_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_STATS_DESCRIPTOR_INDEX];
    const uint inputIndex = dispatchThreadId.x;
    const uint inputCount = CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_INPUT_COUNT;
    if (inputIndex == 0u)
    {
        InterlockedAdd(
            statsBuffer[0].upgradeInvalidationInputCount,
            inputCount);
        InterlockedAdd(
            statsBuffer[0].cumulativeUpgradeInvalidationInputCount,
            inputCount);
    }
    if (inputIndex >= inputCount)
    {
        return;
    }

    const CLodVirtualShadowUpgradeInvalidationInput input = inputs[inputIndex];
    if (input.physicalPageIndex >= kCLodVirtualShadowMaxPhysicalPageCount ||
        input.ownerClipmapIndex >= CLOD_VIRTUAL_SHADOW_APPLY_UPGRADES_CLIPMAP_COUNT ||
        input.ownerVirtualAddress >=
            kCLodVirtualShadowMaxPageTableResolution *
            kCLodVirtualShadowMaxPageTableResolution)
    {
        InterlockedAdd(statsBuffer[0].upgradeInvalidationRejectedInputCount, 1u);
        return;
    }

    const uint4 meta = pageMetadata[input.physicalPageIndex];
    if ((meta.z & kCLodVirtualShadowPhysicalPageResidentFlag) == 0u ||
        meta.x != input.ownerVirtualAddress ||
        meta.w != input.ownerClipmapIndex ||
        CLodVirtualShadowPhysicalPageAllocationGeneration(meta.z) !=
            input.allocationGeneration ||
        meta.y != input.contentGeneration)
    {
        InterlockedAdd(statsBuffer[0].upgradeInvalidationRejectedInputCount, 1u);
        return;
    }

    const uint2 wrappedPageCoords = uint2(
        input.ownerVirtualAddress % kCLodVirtualShadowMaxPageTableResolution,
        input.ownerVirtualAddress / kCLodVirtualShadowMaxPageTableResolution);
    const uint3 pageCoords = uint3(wrappedPageCoords, input.ownerClipmapIndex);
    uint previousEntry = 0u;
    InterlockedOr(
        pageTable[pageCoords],
        kCLodVirtualShadowDirtyMask | kCLodVirtualShadowUpgradePendingMask,
        previousEntry);
    if ((previousEntry & (kCLodVirtualShadowAllocatedMask |
            kCLodVirtualShadowContentValidMask)) !=
        (kCLodVirtualShadowAllocatedMask |
            kCLodVirtualShadowContentValidMask) ||
        (previousEntry & kCLodVirtualShadowPhysicalPageIndexMask) !=
            input.physicalPageIndex)
    {
        InterlockedAdd(statsBuffer[0].upgradeInvalidationRejectedInputCount, 1u);
        return;
    }
    InterlockedOr(
        dirtyFlags[input.physicalPageIndex >> 5u],
        1u << (input.physicalPageIndex & 31u));
    InterlockedAdd(statsBuffer[0].upgradeInvalidationAllocatedPageTouchCount, 1u);
    InterlockedAdd(
        statsBuffer[0].cumulativeUpgradeInvalidationAllocatedPageTouchCount,
        1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowAdmitPagesCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint resolution = CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_RESOLUTION;
    const uint virtualAddress = dispatchThreadId.x;
    if (virtualAddress >= resolution * resolution)
    {
        return;
    }

    RWTexture2DArray<uint> pageTable =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ADMIT_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ADMIT_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_ADMIT_STATS_DESCRIPTOR_INDEX];

    const uint2 pageCoords = uint2(virtualAddress % resolution, virtualAddress / resolution);
    const uint3 tableCoords = uint3(pageCoords, CLOD_VIRTUAL_SHADOW_ADMIT_CLIPMAP_INDEX);
    if (virtualAddress == 0u &&
        CLOD_VIRTUAL_SHADOW_ADMIT_CLIPMAP_INDEX == 0u &&
        CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_PHASE != 0u)
    {
        // Admission always dispatches, unlike indirect allocation.  Publish
        // configured budgets here so idle frames do not misleadingly report
        // them as unlimited after the per-frame stats reset.
        statsBuffer[0].configuredPageRenderBudget =
            CLOD_VIRTUAL_SHADOW_ADMIT_NORMAL_BUDGET;
        statsBuffer[0].configuredUpgradePageRenderBudget =
            CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_BUDGET;
    }
    const uint pageEntry = pageTable[tableCoords];
    if ((pageEntry & (kCLodVirtualShadowAllocatedMask | kCLodVirtualShadowDirtyMask)) !=
        (kCLodVirtualShadowAllocatedMask | kCLodVirtualShadowDirtyMask))
    {
        return;
    }
    // A wrapped table coordinate is only meaningful after the current mark
    // traversal has visited it and refreshed its absolute page tag/view row.
    // Admitting an old, unvisited dirty entry lets current-camera raster data
    // overwrite a cached page that still advertises its previous semantic tag.
    if ((pageEntry & kCLodVirtualShadowVisitedMask) == 0u)
    {
        const uint physicalPageIndex =
            pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
        InterlockedAnd(
            dirtyFlags[physicalPageIndex >> 5u],
            ~(1u << (physicalPageIndex & 31u)));
        return;
    }
    // Fresh mappings reserve their normal admission ticket during allocation.
    // Do not count or admit them a second time during the page-table scan.
    if ((pageEntry & kCLodVirtualShadowAdmittedThisFrameMask) != 0u)
    {
        return;
    }

    const bool upgrade = (pageEntry & kCLodVirtualShadowUpgradePendingMask) != 0u;
    if (upgrade != (CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_PHASE != 0u))
    {
        return;
    }

    uint classTicket = 0u;
    if (upgrade)
    {
        InterlockedAdd(statsBuffer[0].upgradeEligiblePageCount, 1u, classTicket);
        if (CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_BUDGET != 0u &&
            classTicket >= CLOD_VIRTUAL_SHADOW_ADMIT_UPGRADE_BUDGET)
        {
            InterlockedAdd(statsBuffer[0].upgradeDeferredPageCount, 1u);
            const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
            InterlockedAnd(dirtyFlags[physicalPageIndex >> 5u], ~(1u << (physicalPageIndex & 31u)));
            return;
        }
    }
    else
    {
        InterlockedAdd(statsBuffer[0].normalEligiblePageCount, 1u, classTicket);
        if (CLOD_VIRTUAL_SHADOW_ADMIT_NORMAL_BUDGET != 0u &&
            classTicket >= CLOD_VIRTUAL_SHADOW_ADMIT_NORMAL_BUDGET)
        {
            InterlockedAdd(statsBuffer[0].normalDeferredPageCount, 1u);
            const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
            InterlockedAnd(dirtyFlags[physicalPageIndex >> 5u], ~(1u << (physicalPageIndex & 31u)));
            return;
        }
    }

    uint ignored = 0u;
    InterlockedOr(pageTable[tableCoords], kCLodVirtualShadowAdmittedThisFrameMask, ignored);
    // Budget deferral deliberately removes this bit so the clear pass preserves
    // the page's cached contents. Restore it whenever the page is admitted on a
    // later frame; otherwise rasterization depth-mins into those old contents.
    const uint admittedPhysicalPageIndex =
        pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
    InterlockedOr(
        dirtyFlags[admittedPhysicalPageIndex >> 5u],
        1u << (admittedPhysicalPageIndex & 31u));
    InterlockedAdd(statsBuffer[0].admittedPageCount, 1u);
    if (upgrade)
    {
        InterlockedAdd(statsBuffer[0].upgradeAdmittedPageCount, 1u);
        InterlockedAdd(statsBuffer[0].cumulativeUpgradePageAdmittedCount, 1u);
    }
    else
    {
        InterlockedAdd(statsBuffer[0].normalAdmittedPageCount, 1u);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodVirtualShadowBuildDirtyHierarchyCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.z >= CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_COUNT)
    {
        return;
    }

    const uint dstResolution = (CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_IS_PAGE_TABLE != 0u)
        ? CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION
        : max(CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION >> 1u, 1u);
    if (dispatchThreadId.x >= dstResolution || dispatchThreadId.y >= dstResolution)
    {
        return;
    }

    RWTexture2DArray<uint> destTexture = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_DEST_DESCRIPTOR_INDEX];
    uint dirtyValue = 0u;

    if (CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_IS_PAGE_TABLE != 0u)
    {
        // The page table is stored in wrapped (toroidal) coordinate space, but the
        // dirty hierarchy must be in logical (camera-projected) space so that the
        // sphere-vs-hierarchy culling query, which uses camera-derived UVs, samples
        // the correct texels.  Map the logical thread position to wrapped page coords
        // before reading the page table (matching Timberdoodle's gen_dirty_bit_hiz).
        Texture2DArray<uint> sourceTexture = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_DESCRIPTOR_INDEX];
        StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
            ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_INFO_DESCRIPTOR_INDEX];
        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[dispatchThreadId.z];
        const uint2 wrappedCoords = CLodVirtualShadowWrappedPageCoords(dispatchThreadId.xy, clipmapInfo);
        const uint srcValue = sourceTexture.Load(int4(wrappedCoords, dispatchThreadId.z, 0));
        dirtyValue = CLodVirtualShadowPageEntryCanRaster(srcValue) ? 1u : 0u;
    }
    else
    {
        RWTexture2DArray<uint> sourceTexture = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_DESCRIPTOR_INDEX];
        const uint2 srcBase = dispatchThreadId.xy * 2u;
        [unroll]
        for (uint offsetY = 0u; offsetY < 2u; ++offsetY)
        {
            [unroll]
            for (uint offsetX = 0u; offsetX < 2u; ++offsetX)
            {
                const uint srcX = min(srcBase.x + offsetX, CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION - 1u);
                const uint srcY = min(srcBase.y + offsetY, CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION - 1u);
                const uint srcValue = sourceTexture[uint3(srcX, srcY, dispatchThreadId.z)];
                dirtyValue = max(dirtyValue, srcValue != 0u ? 1u : 0u);
            }
        }
    }

    destTexture[dispatchThreadId] = dirtyValue;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodVirtualShadowBuildNonRasterableHierarchyCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.z >= CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_COUNT)
    {
        return;
    }

    const uint dstResolution = (CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_IS_PAGE_TABLE != 0u)
        ? CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION
        : max(CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION >> 1u, 1u);
    if (dispatchThreadId.x >= dstResolution || dispatchThreadId.y >= dstResolution)
    {
        return;
    }

    RWTexture2DArray<uint> destTexture = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_DEST_DESCRIPTOR_INDEX];
    uint nonRasterableValue = 0u;

    if (CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_IS_PAGE_TABLE != 0u)
    {
        Texture2DArray<uint> sourceTexture = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_DESCRIPTOR_INDEX];
        StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
            ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_CLIPMAP_INFO_DESCRIPTOR_INDEX];
        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[dispatchThreadId.z];
        const uint2 wrappedCoords = CLodVirtualShadowWrappedPageCoords(dispatchThreadId.xy, clipmapInfo);
        const uint srcValue = sourceTexture.Load(int4(wrappedCoords, dispatchThreadId.z, 0));
        nonRasterableValue = CLodVirtualShadowPageEntryCanRaster(srcValue) ? 0u : 1u;
    }
    else
    {
        RWTexture2DArray<uint> sourceTexture = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_DESCRIPTOR_INDEX];
        const uint2 srcBase = dispatchThreadId.xy * 2u;
        [unroll]
        for (uint offsetY = 0u; offsetY < 2u; ++offsetY)
        {
            [unroll]
            for (uint offsetX = 0u; offsetX < 2u; ++offsetX)
            {
                const uint srcX = min(srcBase.x + offsetX, CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION - 1u);
                const uint srcY = min(srcBase.y + offsetY, CLOD_VIRTUAL_SHADOW_DIRTY_HIERARCHY_SOURCE_RESOLUTION - 1u);
                const uint srcValue = sourceTexture[uint3(srcX, srcY, dispatchThreadId.z)];
                nonRasterableValue = max(nonRasterableValue, srcValue != 0u ? 1u : 0u);
            }
        }
    }

    destTexture[dispatchThreadId] = nonRasterableValue;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodVirtualShadowClearDirtyBitsCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.z >= kCLodVirtualShadowClipmapCount ||
        dispatchThreadId.x >= CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_PAGE_TABLE_RESOLUTION ||
        dispatchThreadId.y >= CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_PAGE_TABLE_RESOLUTION)
    {
        return;
    }

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> dirtyFlags =
        ResourceDescriptorHeap[
            CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_DIRTY_FLAGS_DESCRIPTOR_INDEX];
    const uint3 pageCoords = dispatchThreadId;
    uint pageEntry = pageTable[pageCoords];
    if ((pageEntry & kCLodVirtualShadowAllocatedMask) == 0u)
    {
        return;
    }

    uint ignoredPrev = 0u;
    if ((pageEntry & kCLodVirtualShadowRerenderedThisFrameMask) == 0u)
    {
        // Page-job raster dispatches exact page/cluster pairs. Reaching this
        // pass therefore proves that an admitted page with no depth writes is
        // a successfully rendered empty page, not failed work. Treating a
        // fragment write as the completion signal made empty pages retry
        // forever and hid the defect in hardware mode through extreme
        // overdraw.
        if (CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_COMPLETE_EMPTY_ADMITTED_PAGES != 0u &&
            (pageEntry & kCLodVirtualShadowAdmittedThisFrameMask) != 0u)
        {
            InterlockedOr(
                pageTable[pageCoords],
                kCLodVirtualShadowContentValidMask |
                    kCLodVirtualShadowRerenderedThisFrameMask,
                ignoredPrev);
            pageEntry |=
                kCLodVirtualShadowContentValidMask |
                kCLodVirtualShadowRerenderedThisFrameMask;
        }
        else
        {
        // Admission is a one-frame raster permission.  A page that emitted no
        // depth remains dirty for retry, but must compete for the next frame's
        // budget instead of rasterizing indefinitely outside the scheduler.
            if ((pageEntry & kCLodVirtualShadowAdmittedThisFrameMask) != 0u)
            {
                InterlockedAnd(
                    pageTable[pageCoords],
                    ~(kCLodVirtualShadowAdmittedThisFrameMask |
                        kCLodVirtualShadowContentValidMask),
                    ignoredPrev);
            }
            return;
        }
    }

    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
        ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_CLEAR_DIRTY_BITS_STATS_DESCRIPTOR_INDEX];
    const uint physicalPageIndex =
        pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
    const uint retryBit =
        1u << (physicalPageIndex & 31u);
    const bool dependencyCaptureRetry =
        (dirtyFlags[physicalPageIndex >> 5u] & retryBit) != 0u;
    if ((pageEntry & kCLodVirtualShadowUpgradePendingMask) != 0u)
    {
        InterlockedAdd(statsBuffer[0].upgradeRenderedPageCount, 1u);
        InterlockedAdd(statsBuffer[0].cumulativeUpgradePageRenderedCount, 1u);
    }
    else
    {
        InterlockedAdd(statsBuffer[0].normalRenderedPageCount, 1u);
    }
    const uint completedClearMask =
        kCLodVirtualShadowAdmittedThisFrameMask |
        kCLodVirtualShadowUpgradePendingMask |
        (dependencyCaptureRetry ? 0u : kCLodVirtualShadowDirtyMask);
    InterlockedAnd(pageTable[pageCoords], ~completedClearMask, ignoredPrev);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowBuildActiveBlocksCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint blockLinearIndex = dispatchThreadId.x;
    if (blockLinearIndex >= CLOD_VSM_BUILD_ACTIVE_BLOCKS_COUNT)
    {
        return;
    }

    uint2 blockCoord;
    uint clipmapIndex;
    CLodVirtualShadowUnpackBlockLinearIndex(blockLinearIndex, blockCoord, clipmapIndex);

    RWStructuredBuffer<uint> output =
        ResourceDescriptorHeap[CLOD_VSM_BUILD_ACTIVE_BLOCKS_OUTPUT_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_VSM_BUILD_ACTIVE_BLOCKS_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
    const uint blocksPerAxis =
        (clipmapInfo.pageTableResolution + kCLodVirtualShadowBlockPagesPerAxis - 1u) /
        kCLodVirtualShadowBlockPagesPerAxis;
    if (!CLodVirtualShadowClipmapIsValid(clipmapInfo) ||
        blockCoord.x >= blocksPerAxis ||
        blockCoord.y >= blocksPerAxis)
    {
        output[blockLinearIndex] = 0xFFFFFFFFu;
        return;
    }

    Texture2DArray<uint> pageTable =
        ResourceDescriptorHeap[CLOD_VSM_BUILD_ACTIVE_BLOCKS_PAGE_TABLE_DESCRIPTOR_INDEX];
    const uint2 blockOriginPageCoord =
        CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord);
    uint2 minLocalPageCoord = uint2(3u, 3u);
    uint2 maxLocalPageCoord = uint2(0u, 0u);
    bool anyActive = false;
    [unroll]
    for (uint localY = 0u; localY < kCLodVirtualShadowBlockPagesPerAxis; ++localY)
    {
        [unroll]
        for (uint localX = 0u; localX < kCLodVirtualShadowBlockPagesPerAxis; ++localX)
        {
            const uint2 localPageCoord = uint2(localX, localY);
            const uint2 pageCoord = blockOriginPageCoord + localPageCoord;
            if (pageCoord.x >= clipmapInfo.pageTableResolution ||
                pageCoord.y >= clipmapInfo.pageTableResolution)
            {
                continue;
            }
            const uint2 wrappedPageCoord =
                CLodVirtualShadowWrappedPageCoords(pageCoord, clipmapInfo);
            const uint pageEntry =
                pageTable[uint3(wrappedPageCoord, clipmapInfo.pageTableLayer)];
            if (CLodVirtualShadowPageEntryCanRaster(pageEntry))
            {
                anyActive = true;
                minLocalPageCoord = min(minLocalPageCoord, localPageCoord);
                maxLocalPageCoord = max(maxLocalPageCoord, localPageCoord);
            }
        }
    }
    output[blockLinearIndex] = anyActive
        ? CLodVirtualShadowPackBlockActiveRect(
            minLocalPageCoord, maxLocalPageCoord, false)
        : 0xFFFFFFFFu;
}

uint CLodGetHistogramVisibleClusterReadIndex(uint linearizedID)
{
    uint readBase = 0u;
    if (CLOD_HISTOGRAM_READ_BASE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> readBaseCounter = ResourceDescriptorHeap[CLOD_HISTOGRAM_READ_BASE_COUNTER_DESCRIPTOR_INDEX];
        readBase = readBaseCounter.Load(0);
    }

    if ((CLOD_HISTOGRAM_READ_MODE_FLAGS & CLOD_HISTOGRAM_READ_FLAG_REVERSED) != 0u)
    {
        return CLOD_HISTOGRAM_READ_CAPACITY - 1u - (readBase + linearizedID);
    }

    return readBase + linearizedID;
}

uint CLodGetCompactionVisibleClusterReadIndex(uint linearizedID)
{
    uint readBase = 0u;
    if (CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> readBaseCounter = ResourceDescriptorHeap[CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX];
        readBase = readBaseCounter.Load(0);
    }

    if ((CLOD_COMPACTION_READ_MODE_FLAGS & CLOD_COMPACTION_READ_FLAG_REVERSED) != 0u)
    {
        return CLOD_COMPACTION_READ_CAPACITY - 1u - (readBase + linearizedID);
    }

    return readBase + linearizedID;
}

#define CLUSTER_HISTOGRAM_GROUP_SIZE 8

// Single-thread shader to create a command for histogram eval
[shader("compute")]
[numthreads(1, 1, 1)]
void CreateRasterBucketsHistogramCommandCSMain()
{
    RWStructuredBuffer<RasterBucketsHistogramIndirectCommand> outCommand = ResourceDescriptorHeap[CLOD_CREATE_RASTER_BUCKET_HISTOGRAM_COMMAND_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> clusterCountBuffer = ResourceDescriptorHeap[CLOD_CREATE_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];

    // Given the cluster count, find dispatch dimensions that minimizes wasted threads
    // Producers use monotonic counters and may report more work than fit in the
    // destination buffer. Never turn that overflow value into an unbounded
    // indirect dispatch.
    uint clusterCount = min(clusterCountBuffer.Load(0), CLOD_CREATE_VISIBLE_CLUSTERS_CAPACITY);
    uint numBuckets = CLOD_CREATE_NUM_RASTER_BUCKETS;
    uint totalItems = max(clusterCount, numBuckets);

    uint groupsNeeded = (totalItems + CLUSTER_HISTOGRAM_GROUP_SIZE - 1u) / CLUSTER_HISTOGRAM_GROUP_SIZE;

    const uint kMaxDim = 65535u;
    uint dispatchX = (uint) ceil(sqrt((float) groupsNeeded));
    if (dispatchX > kMaxDim)
    {
        dispatchX = kMaxDim;
    }

    uint dispatchY = (groupsNeeded + dispatchX - 1u) / dispatchX;
    if (dispatchY > kMaxDim)
    {
        dispatchY = kMaxDim;
    }

    outCommand[0].clusterCount = clusterCount;
    outCommand[0].xDim = dispatchX;
    outCommand[0].dispatchX = dispatchX;
    outCommand[0].dispatchY = dispatchY;
    outCommand[0].dispatchZ = 1;

    if (CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX != 0xFFFFFFFFu &&
        CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<CLodReplayBufferState> replayStateBuffer = ResourceDescriptorHeap[CLOD_CREATE_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
        RWStructuredBuffer<CLodNodeGpuInput> nodeInputs = ResourceDescriptorHeap[CLOD_CREATE_WORKGRAPH_NODE_INPUTS_DESCRIPTOR_INDEX];

        const CLodReplayBufferState replayState = replayStateBuffer[0];

        const uint nodeReplayCount = min(replayState.nodeWriteCount, CLOD_NODE_REPLAY_CAPACITY);
        const uint meshletReplayCount = min(replayState.meshletWriteCount, CLOD_MESHLET_REPLAY_CAPACITY);
        const uint reyesSplitReplayCount = min(replayState.reyesSplitWriteCount, CLOD_REYES_SPLIT_REPLAY_CAPACITY);
        const uint reyesDiceReplayCount = min(replayState.reyesDiceWriteCount, CLOD_REYES_DICE_REPLAY_CAPACITY);

        // Slot 0 is CLodMultiNodeGpuInput (CPU initialized); slot 1+ are CLodNodeGpuInput records.
        // Entry point 1 = TraverseNodes (node replay region, 12-byte stride).
        // Entry point 2 = ClusterCull1  (meshlet replay region, 24-byte stride).
        // recordsAddress for meshlet region is patched by C++ to include CLOD_REPLAY_MESHLET_REGION_OFFSET.
        nodeInputs[1].numRecords = nodeReplayCount;
        nodeInputs[1].recordStride = CLOD_NODE_REPLAY_STRIDE_BYTES;
        nodeInputs[2].numRecords = meshletReplayCount;
        nodeInputs[2].recordStride = CLOD_MESHLET_REPLAY_STRIDE_BYTES;
        nodeInputs[3].numRecords = reyesSplitReplayCount;
        nodeInputs[3].recordStride = CLOD_REYES_SPLIT_REPLAY_STRIDE_BYTES;
        nodeInputs[4].numRecords = reyesDiceReplayCount;
        nodeInputs[4].recordStride = CLOD_REYES_DICE_REPLAY_STRIDE_BYTES;
    }
}

// IndirectCommandSignatureRootConstant0 = cluster count
// IndirectCommandSignatureRootConstant1 = xDim
[shader("compute")]
[numthreads(CLUSTER_HISTOGRAM_GROUP_SIZE, 1, 1)]
void ClusterRasterBucketsHistogramCSMain(uint3 DTid : SV_DispatchThreadID)
{
    // Linearize the 2D dispatch thread ID
    // Root constant is dispatchX in groups, convert to thread width.
    uint xDimThreads = IndirectCommandSignatureRootConstant1 * CLUSTER_HISTOGRAM_GROUP_SIZE;
    uint linearizedID = DTid.x + DTid.y * xDimThreads;
    StructuredBuffer<uint> clusterCountBuffer = ResourceDescriptorHeap[CLOD_HISTOGRAM_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    uint readBase = 0u;
    if (CLOD_HISTOGRAM_READ_BASE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> readBaseCounter = ResourceDescriptorHeap[CLOD_HISTOGRAM_READ_BASE_COUNTER_DESCRIPTOR_INDEX];
        readBase = readBaseCounter.Load(0);
    }
    const uint readableCount = (readBase < CLOD_HISTOGRAM_READ_CAPACITY)
        ? (CLOD_HISTOGRAM_READ_CAPACITY - readBase)
        : 0u;
    uint clusterCount = min(clusterCountBuffer.Load(0), readableCount);
    
    if (linearizedID >= clusterCount) {
        return;
    }

    const uint visibleClusterReadIndex = CLodGetHistogramVisibleClusterReadIndex(linearizedID);
    CLodSortTelemetryAdd(CLOD_HISTOGRAM_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_HISTOGRAM_INPUTS, 1u);
    if ((CLOD_HISTOGRAM_READ_MODE_FLAGS & CLOD_HISTOGRAM_READ_FLAG_SKIP_REYES_OWNED) != 0u &&
        CLodIsVisibleClusterOwnedByReyes(visibleClusterReadIndex, CLOD_HISTOGRAM_REYES_OWNERSHIP_BITSET_DESCRIPTOR_INDEX)) {
        CLodSortTelemetryAdd(CLOD_HISTOGRAM_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_HISTOGRAM_REYES_SKIPPED, 1u);
        return;
    }

    ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_HISTOGRAM_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, visibleClusterReadIndex);
    if (CLodVisibleClusterIsVoxel(packedCluster))
    {
        CLodSortTelemetryAdd(CLOD_HISTOGRAM_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_HISTOGRAM_VOXEL_SKIPPED, 1u);
        return;
    }

    // TODO: Remove load chain
    uint instanceIndex = CLodVisibleClusterInstanceID(packedCluster);
    uint perMeshIndex = LoadMeshTemplateForDraw(instanceIndex).perMeshBufferIndex;
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    uint rasterBucketIndex = perMeshBuffer[perMeshIndex].rasterBucketIndex;
    if (rasterBucketIndex >= CLOD_HISTOGRAM_NUM_RASTER_BUCKETS)
    {
        return;
    }

    // Group threads in the wave by matId
    uint4 mask = WaveMatch(rasterBucketIndex);

    if (IsWaveGroupLeader(mask))
    {
        uint groupSize = CountBits128(mask);
        RWStructuredBuffer<uint> histogramBuffer = ResourceDescriptorHeap[CLOD_HISTOGRAM_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
        InterlockedAdd(histogramBuffer[rasterBucketIndex], groupSize);
        CLodSortTelemetryAdd(CLOD_HISTOGRAM_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_HISTOGRAM_TRIANGLE_CONTRIBUTORS, groupSize);
    }
}

// Prefix sum for raster bucket histogram

static const uint CLOD_BUCKET_BLOCK_SIZE = 1024; // must be power-of-two
static const uint CLOD_BUCKET_SCAN_THREADS = 256;

groupshared uint s_bucketData[CLOD_BUCKET_BLOCK_SIZE];
groupshared uint s_bucketScan[CLOD_BUCKET_BLOCK_SIZE];
groupshared uint s_bucketBlockSum;

void CLODExclusiveScanBlock(uint Nblock, uint threadId)
{
    if (Nblock == 0)
    {
        return;
    }

    uint P = 1;
    while (P < Nblock)
    {
        P <<= 1;
    }

    for (uint i = threadId; i < Nblock; i += CLOD_BUCKET_SCAN_THREADS)
    {
        s_bucketScan[i] = s_bucketData[i];
    }
    for (uint i = threadId + Nblock; i < P; i += CLOD_BUCKET_SCAN_THREADS)
    {
        s_bucketScan[i] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 1; stride < P; stride <<= 1)
    {
        uint idx = (threadId + 1) * (stride << 1) - 1;
        if (idx < P)
        {
            s_bucketScan[idx] += s_bucketScan[idx - stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (threadId == 0)
    {
        s_bucketScan[P - 1] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = (P >> 1); stride >= 1; stride >>= 1)
    {
        uint idx = (threadId + 1) * (stride << 1) - 1;
        if (idx < P)
        {
            uint t = s_bucketScan[idx - stride];
            s_bucketScan[idx - stride] = s_bucketScan[idx];
            s_bucketScan[idx] += t;
        }
        GroupMemoryBarrierWithGroupSync();

        if (stride == 1)
            break;
    }
}

// Root constants:
// UintRootConstant0 = NumBuckets
[numthreads(CLOD_BUCKET_SCAN_THREADS, 1, 1)]
void RasterBucketsBlockScanCS(uint3 groupThreadId : SV_GroupThreadID,
                              uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_PREFIX_SCAN_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[CLOD_PREFIX_SCAN_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> blockSums = ResourceDescriptorHeap[CLOD_PREFIX_SCAN_RASTER_BUCKETS_BLOCK_SUMS_DESCRIPTOR_INDEX];

    uint N = CLOD_PREFIX_SCAN_NUM_BUCKETS;
    uint blockId = groupId.x;
    uint localTid = groupThreadId.x;
    uint base = blockId * CLOD_BUCKET_BLOCK_SIZE;

    for (uint i = localTid; i < CLOD_BUCKET_BLOCK_SIZE; i += CLOD_BUCKET_SCAN_THREADS)
    {
        uint globalIdx = base + i;
        s_bucketData[i] = (globalIdx < N) ? histogram[globalIdx] : 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint Nblock = 0;
    if (N > base)
        Nblock = min(CLOD_BUCKET_BLOCK_SIZE, N - base);

    CLODExclusiveScanBlock(Nblock, localTid);

    for (uint i = localTid; i < Nblock; i += CLOD_BUCKET_SCAN_THREADS)
    {
        offsets[base + i] = s_bucketScan[i];
    }

    uint localSum = 0;
    for (uint i = localTid; i < Nblock; i += CLOD_BUCKET_SCAN_THREADS)
    {
        localSum += s_bucketData[i];
    }

    if (localTid == 0)
        s_bucketBlockSum = 0;
    GroupMemoryBarrierWithGroupSync();

    InterlockedAdd(s_bucketBlockSum, localSum);
    GroupMemoryBarrierWithGroupSync();

    if (localTid == 0)
    {
        uint numBlocks = (N + CLOD_BUCKET_BLOCK_SIZE - 1) / CLOD_BUCKET_BLOCK_SIZE;
        if (blockId < numBlocks)
        {
            blockSums[blockId] = s_bucketBlockSum;
        }
    }
}

// Root constants:
// UintRootConstant0 = NumBuckets
// UintRootConstant1 = NumBlocks
[numthreads(CLOD_BUCKET_SCAN_THREADS, 1, 1)]
void RasterBucketsBlockOffsetsCS(uint3 groupThreadId : SV_GroupThreadID,
                                 uint3 groupId : SV_GroupID)
{
    if (groupId.x != 0 || groupId.y != 0 || groupId.z != 0)
        return;

    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> blockSums = ResourceDescriptorHeap[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_BLOCK_SUMS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> scannedBlockSums = ResourceDescriptorHeap[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_SCANNED_BLOCK_SUMS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> totalOut = ResourceDescriptorHeap[CLOD_PREFIX_OFFSETS_RASTER_BUCKETS_TOTAL_COUNT_DESCRIPTOR_INDEX];

    uint N = CLOD_PREFIX_OFFSETS_NUM_BUCKETS;
    uint B = CLOD_PREFIX_OFFSETS_NUM_BLOCKS;

    uint localTid = groupThreadId.x;

    for (uint i = localTid; i < B; i += CLOD_BUCKET_SCAN_THREADS)
    {
        s_bucketData[i] = blockSums[i];
    }
    GroupMemoryBarrierWithGroupSync();

    CLODExclusiveScanBlock(B, localTid);

    for (uint i = localTid; i < B; i += CLOD_BUCKET_SCAN_THREADS)
    {
        scannedBlockSums[i] = s_bucketScan[i];
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint elem = localTid; elem < N; elem += CLOD_BUCKET_SCAN_THREADS)
    {
        uint blockId = elem / CLOD_BUCKET_BLOCK_SIZE;
        uint prefix = s_bucketScan[blockId];
        offsets[elem] += prefix;
    }

    if (localTid == 0 && B > 0)
    {
        uint total = s_bucketScan[B - 1] + s_bucketData[B - 1];
        totalOut[0] = total;
    }
}

uint GetRasterBucketIndexFromInstance(
    uint instanceID,
    out PerMeshInstanceBuffer instanceData)
{
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];

    instanceData = LoadMeshTemplateForDraw(instanceID);
    const PerMeshBuffer meshData = perMeshBuffer[instanceData.perMeshBufferIndex];
    return meshData.rasterBucketIndex;
}

static const uint CLOD_COMPACTION_GROUP_SIZE = CLUSTER_HISTOGRAM_GROUP_SIZE;

[shader("compute")]
[numthreads(CLOD_COMPACTION_GROUP_SIZE, 1, 1)]
void CompactClustersAndBuildIndirectArgsCS(uint3 dtid : SV_DispatchThreadID)
{
    uint xDimThreads = IndirectCommandSignatureRootConstant1 * CLOD_COMPACTION_GROUP_SIZE;
    uint linearizedID = dtid.x + dtid.y * xDimThreads;

    StructuredBuffer<uint> visibleClusterCountBuffer = ResourceDescriptorHeap[CLOD_COMPACTION_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    uint readBase = 0u;
    if (CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> readBaseCounter = ResourceDescriptorHeap[CLOD_COMPACTION_READ_BASE_COUNTER_DESCRIPTOR_INDEX];
        readBase = readBaseCounter.Load(0);
    }
    const uint readableCount = (readBase < CLOD_COMPACTION_READ_CAPACITY)
        ? (CLOD_COMPACTION_READ_CAPACITY - readBase)
        : 0u;
    const uint clusterCount = min(visibleClusterCountBuffer.Load(0), readableCount);

    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_COMPACTION_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint numBucketsPacked = CLOD_COMPACTION_NUM_RASTER_BUCKETS;
    const bool appendToExisting = ((numBucketsPacked & 0x80000000u) != 0u);
    const uint numBuckets = (numBucketsPacked & 0x7FFFFFFFu);

    uint baseClusterOffset = 0u;
    if (appendToExisting)
    {
        StructuredBuffer<uint> appendBaseCounter = ResourceDescriptorHeap[CLOD_COMPACTION_APPEND_BASE_COUNTER_DESCRIPTOR_INDEX];
        baseClusterOffset = appendBaseCounter.Load(0);
    }

    if (linearizedID < clusterCount)
    {
        const uint sourceClusterIndex = CLodGetCompactionVisibleClusterReadIndex(linearizedID);
        CLodSortTelemetryAdd(CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_COMPACTION_INPUTS, 1u);
        bool shouldCompactCluster = true;
        if ((CLOD_COMPACTION_READ_MODE_FLAGS & CLOD_COMPACTION_READ_FLAG_SKIP_REYES_OWNED) != 0u &&
            CLodIsVisibleClusterOwnedByReyes(sourceClusterIndex, CLOD_COMPACTION_REYES_OWNERSHIP_BITSET_DESCRIPTOR_INDEX))
        {
            CLodSortTelemetryAdd(CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_COMPACTION_REYES_SKIPPED, 1u);
            shouldCompactCluster = false;
        }

        ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_COMPACTION_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
        StructuredBuffer<uint> visibleClusterTransformIndices =
            ResourceDescriptorHeap[CLOD_COMPACTION_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
        RWByteAddressBuffer compactedClusters = ResourceDescriptorHeap[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
        RWStructuredBuffer<uint> compactedClusterTransformIndices =
            ResourceDescriptorHeap[CLOD_COMPACTION_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
        StructuredBuffer<uint> offsets = ResourceDescriptorHeap[CLOD_COMPACTION_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX];
        RWStructuredBuffer<uint> writeCursor = ResourceDescriptorHeap[CLOD_COMPACTION_RASTER_BUCKETS_WRITE_CURSOR_DESCRIPTOR_INDEX];
        const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, sourceClusterIndex);
        if (shouldCompactCluster && CLodVisibleClusterIsVoxel(packedCluster))
        {
            CLodSortTelemetryAdd(CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_COMPACTION_VOXEL_SKIPPED, 1u);
            shouldCompactCluster = false;
        }

        if (shouldCompactCluster)
        {
            const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
            PerMeshInstanceBuffer meshInstance;
            uint bucketIndex = GetRasterBucketIndexFromInstance(instanceID, meshInstance);
            shouldCompactCluster = bucketIndex < numBuckets;
            if (shouldCompactCluster)
            {
                uint localOffset = 0;
                InterlockedAdd(writeCursor[bucketIndex], 1, localOffset);

                const uint bucketBase = offsets[bucketIndex];
                const uint outputOffset = bucketBase + localOffset;
                const bool outputOffsetValid = outputOffset >= bucketBase;
                const uint dst = baseClusterOffset + outputOffset;
                const bool dstValid = outputOffsetValid && dst >= baseClusterOffset && dst < CLOD_COMPACTION_READ_CAPACITY;
                if (dstValid)
                {
                    CLodStoreVisibleClusterPackedWordsRW(compactedClusters, dst, packedCluster);
                    compactedClusterTransformIndices[dst] = visibleClusterTransformIndices[sourceClusterIndex];
                    if ((CLOD_COMPACTION_READ_MODE_FLAGS & CLOD_COMPACTION_READ_FLAG_BUILD_SW_DISPATCH) != 0u)
                    {
                        RWStructuredBuffer<CLodSoftwareRasterMapping> softwareRasterMapping =
                            ResourceDescriptorHeap[CLOD_COMPACTION_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX];
                        CLodSoftwareRasterMapping mapping;
                        mapping.unsortedClusterIndex = sourceClusterIndex;
                        mapping.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(
                            instanceID,
                            meshInstance.skinningInstanceSlot);
                        softwareRasterMapping[dst] = mapping;
                    }
                    else
                    {
                        RWStructuredBuffer<uint> sortedToUnsortedMapping =
                            ResourceDescriptorHeap[CLOD_COMPACTION_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX];
                        sortedToUnsortedMapping[dst] = sourceClusterIndex;
                    }
                    CLodSortTelemetryAdd(CLOD_COMPACTION_TELEMETRY_DESCRIPTOR_INDEX, WG_COUNTER_RASTER_SORT_COMPACTION_TRIANGLE_EMITTED, 1u);
                }
            }
        }
    }

    if (linearizedID < numBuckets)
    {
        StructuredBuffer<uint> offsets = ResourceDescriptorHeap[CLOD_COMPACTION_RASTER_BUCKETS_OFFSETS_DESCRIPTOR_INDEX];
        RWStructuredBuffer<RasterizeClustersCommand> outArgs = ResourceDescriptorHeap[CLOD_COMPACTION_RASTER_BUCKETS_INDIRECT_ARGS_DESCRIPTOR_INDEX];

        const uint bucketOffset = offsets[linearizedID];
        const uint bucketBase = baseClusterOffset + bucketOffset;
        const uint availableCount = (bucketBase >= baseClusterOffset && bucketBase < CLOD_COMPACTION_READ_CAPACITY)
            ? (CLOD_COMPACTION_READ_CAPACITY - bucketBase)
            : 0u;
        uint count = min(histogram[linearizedID], availableCount);

        RasterizeClustersCommand cmd = (RasterizeClustersCommand)0;
        if (count > 0)
        {
            const uint kMaxDim = 65535u;
            uint dispatchItemCount = count;
            if ((CLOD_COMPACTION_READ_MODE_FLAGS & CLOD_COMPACTION_READ_FLAG_BUILD_SW_DISPATCH) != 0u)
            {
                dispatchItemCount *= SW_RASTER_GROUPS_PER_CLUSTER;
            }

            uint dispatchX = (uint) ceil(sqrt((float) dispatchItemCount));
            if (dispatchX > kMaxDim)
            {
                dispatchX = kMaxDim;
            }
            uint dispatchY = (dispatchItemCount + dispatchX - 1u) / dispatchX;
            if (dispatchY > kMaxDim)
            {
                dispatchY = kMaxDim;
            }

            cmd.baseClusterOffset = bucketBase; // base offset
            cmd.xDim = dispatchX;              // xDim for 2D linearization
            cmd.rasterBucketID = linearizedID;   // bucket index

            cmd.dispatchX = dispatchX;
            cmd.dispatchY = dispatchY;
            cmd.dispatchZ = 1;
        }
        outArgs[linearizedID] = cmd;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodVirtualShadowGatherStatsCSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    Texture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> allocationCountBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_GATHER_STATS_ALLOCATION_COUNT_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesDispatchIndirectCommand> allocationIndirectArgsBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_GATHER_STATS_ALLOCATION_INDIRECT_ARGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint4> pageListHeaderBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_LIST_HEADER_DESCRIPTOR_INDEX];
    StructuredBuffer<uint4> pageMetadata = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_METADATA_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_GATHER_STATS_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer = ResourceDescriptorHeap[CLOD_VIRTUAL_SHADOW_GATHER_STATS_STATS_DESCRIPTOR_INDEX];
    const bool capturePreAllocateState = CLOD_VIRTUAL_SHADOW_GATHER_STATS_CAPTURE_PRE_ALLOCATE_STATE != 0u;

    if (dispatchThreadId.x == 0u && dispatchThreadId.y == 0u && dispatchThreadId.z == 0u)
    {
        if (capturePreAllocateState)
        {
            statsBuffer[0].activeClipmapCount = min(perFrameBuffer.numDirectionalClipmaps, CLOD_VIRTUAL_SHADOW_GATHER_STATS_CLIPMAP_COUNT);
            statsBuffer[0].allocationRequestCount = allocationCountBuffer.Load(0);
            statsBuffer[0].allocationDispatchGroupCount = allocationIndirectArgsBuffer[0].dispatchX;

            const uint4 pageListHeader = pageListHeaderBuffer[0];
            statsBuffer[0].freePhysicalPageCount = pageListHeader.x;
            statsBuffer[0].reusablePhysicalPageCount = pageListHeader.y;

            uint validClipmapCount = 0u;
            [unroll]
            for (uint clipmapIndex = 0u; clipmapIndex < kCLodVirtualShadowClipmapCount; ++clipmapIndex)
            {
                if (CLodVirtualShadowClipmapIsValid(clipmapInfos[clipmapIndex]))
                {
                    validClipmapCount += 1u;
                }
            }
            statsBuffer[0].validClipmapCount = validClipmapCount;
        }
    }

    if (dispatchThreadId.z >= CLOD_VIRTUAL_SHADOW_GATHER_STATS_CLIPMAP_COUNT ||
        dispatchThreadId.x >= CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_RESOLUTION ||
        dispatchThreadId.y >= CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_RESOLUTION)
    {
        return;
    }

    const uint pageEntry = pageTable.Load(int4(dispatchThreadId, 0));
    if (pageEntry == 0u)
    {
        return;
    }

    const uint virtualAddress = dispatchThreadId.y * CLOD_VIRTUAL_SHADOW_GATHER_STATS_PAGE_TABLE_RESOLUTION + dispatchThreadId.x;
    const bool isAllocated = (pageEntry & kCLodVirtualShadowAllocatedMask) != 0u;
    const bool isDirty = (pageEntry & kCLodVirtualShadowDirtyMask) != 0u;
    const bool wasVisited = (pageEntry & kCLodVirtualShadowVisitedMask) != 0u;
    if (isAllocated)
    {
        const uint physicalPageIndex =
            pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
        bool ownerMatches = physicalPageIndex <
            kCLodVirtualShadowMaxPhysicalPageCount;
        if (ownerMatches)
        {
            const uint4 meta = pageMetadata[physicalPageIndex];
            ownerMatches =
                (meta.z & kCLodVirtualShadowPhysicalPageResidentFlag) != 0u &&
                meta.x == virtualAddress &&
                meta.w == dispatchThreadId.z;
        }
        if (!ownerMatches)
        {
            InterlockedAdd(statsBuffer[0].pageTableOwnerMismatchCount, 1u);
            if ((pageEntry & kCLodVirtualShadowContentValidMask) != 0u)
            {
                InterlockedAdd(
                    statsBuffer[0].contentValidOwnerMismatchCount,
                    1u);
            }
        }
        else if (!capturePreAllocateState &&
            (pageEntry & kCLodVirtualShadowContentValidMask) != 0u &&
            (pageMetadata[physicalPageIndex].y &
                kCLodVirtualShadowClearEpochPendingMask) != 0u)
        {
            InterlockedAdd(
                statsBuffer[0].syntheticEmptyValidPageCount,
                1u);
        }
    }

    if (capturePreAllocateState)
    {
        CLodVirtualShadowStatsIncrementPreAllocateNonZero(statsBuffer, dispatchThreadId.z);
        if (isDirty)
        {
            CLodVirtualShadowStatsIncrementPreAllocateDirty(statsBuffer, dispatchThreadId.z);
        }
    }
    else
    {
        CLodVirtualShadowStatsIncrementNonZero(statsBuffer, dispatchThreadId.z);
        if (wasVisited)
        {
            CLodVirtualShadowStatsIncrementVisited(statsBuffer, dispatchThreadId.z);
            if (isDirty)
            {
                CLodVirtualShadowStatsIncrementVisitedDirty(statsBuffer, dispatchThreadId.z);
            }
        }
        if (isAllocated)
        {
            CLodVirtualShadowStatsIncrementAllocated(statsBuffer, dispatchThreadId.z);
        }
        if (isDirty)
        {
            CLodVirtualShadowStatsIncrementDirty(statsBuffer, dispatchThreadId.z);
        }
    }
}
