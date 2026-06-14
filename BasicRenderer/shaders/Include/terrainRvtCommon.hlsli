#ifndef __TERRAIN_RVT_COMMON_HLSLI__
#define __TERRAIN_RVT_COMMON_HLSLI__

#include "structs.hlsli"

static const uint TERRAIN_RVT_COUNTER_REQUEST_COUNT = 0u;
static const uint TERRAIN_RVT_COUNTER_GENERATION_COUNT = 1u;
static const uint TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT = 2u;
static const uint TERRAIN_RVT_COUNTER_OVERFLOW_COUNT = 3u;
static const uint TERRAIN_RVT_COUNTER_REUSE_CURSOR = 4u;
static const uint TERRAIN_RVT_COUNTER_COUNT = 5u;
static const uint TERRAIN_RVT_TELEMETRY_MIP_BINS = 16u;
static const uint TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED = 0xfffffffeu;
static const uint TERRAIN_RVT_PHYSICAL_PAGE_OWNER_FREE = 0xffffffffu;
static const uint TERRAIN_RVT_REQUEST_MASK_CONTENT_MASK = 0x3u;
static const uint TERRAIN_RVT_REQUEST_MASK_APPENDED = 1u << 31;
static const uint TERRAIN_RVT_FALLBACK_NONE = 0u;
static const uint TERRAIN_RVT_FALLBACK_DISABLED = 1u;
static const uint TERRAIN_RVT_FALLBACK_FORCED = 2u;
static const uint TERRAIN_RVT_FALLBACK_COMPUTE_PAGE = 3u;
static const uint TERRAIN_RVT_FALLBACK_PAGE_MISS = 4u;
static const uint TERRAIN_RVT_FALLBACK_OWNER_MISMATCH = 5u;
#define TERRAIN_RVT_ENABLE_PAGE_STAMP_DEBUG 0
#define TERRAIN_RVT_VALIDATE_SAMPLE_OWNER 0
#define TERRAIN_RVT_ENABLE_HOT_SAMPLE_DEBUG 0
#define TERRAIN_RVT_ENABLE_DIRECT_FALLBACK 1
#define TERRAIN_RVT_ENABLE_COARSER_RESIDENT_FALLBACK 1

#if defined(TERRAIN_RVT_TELEMETRY)
#define TERRAIN_RVT_TELEMETRY_ENABLED(perFrame) ((perFrame).terrainRvtTelemetryEnabled != 0u)
#else
#define TERRAIN_RVT_TELEMETRY_ENABLED(perFrame) false
#endif

#if TERRAIN_RVT_ENABLE_PAGE_STAMP_DEBUG
uint TerrainRvtDebugHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float TerrainRvtDebugPageStampValue(uint pageTableIndex)
{
    return (float)(TerrainRvtDebugHash(pageTableIndex) & 0xffu) / 255.0f;
}

uint TerrainRvtDebugDecodePageStampByte(float stamp)
{
    return min(255u, (uint)floor(saturate(stamp) * 255.0f + 0.5f));
}
#endif

float2 TerrainRvtSkyrimXYFromRendererPosition(float3 positionWS)
{
    return float2(positionWS.x, -positionWS.z);
}

float2 TerrainRvtSkyrimXYDerivativeFromRendererDerivative(float3 positionDerivativeWS)
{
    return float2(positionDerivativeWS.x, -positionDerivativeWS.z);
}

uint TerrainRvtTelemetryMipBin(uint mip)
{
    return min(mip, TERRAIN_RVT_TELEMETRY_MIP_BINS - 1u);
}

float TerrainRvtBasePageWorldSize(TerrainRvtInfo info)
{
    return max(info.basePageWorldSize, 0.125f);
}

float TerrainRvtPageWorldSize(TerrainRvtInfo info, uint clipLevel)
{
    return TerrainRvtBasePageWorldSize(info) * (float)(1u << min(clipLevel, 31u));
}

uint2 TerrainRvtBasePageCount(TerrainRvtInfo info, TerrainSetInfo terrain)
{
    const float basePageWorldSize = TerrainRvtBasePageWorldSize(info);
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    return max(uint2(1u, 1u), (uint2)ceil(terrainSize / basePageWorldSize));
}

uint2 TerrainRvtClipPageCount(TerrainRvtInfo info, TerrainSetInfo terrain, uint clipLevel)
{
    const uint scale = 1u << min(clipLevel, 31u);
    return max(uint2(1u, 1u), (TerrainRvtBasePageCount(info, terrain) + scale - 1u) >> min(clipLevel, 31u));
}

uint TerrainRvtRequiredClipCount(TerrainRvtInfo info, TerrainSetInfo terrain)
{
    const float terrainExtent = max((float)terrain.regionCountX, (float)terrain.regionCountY) * terrain.regionSizeWorld;
    const float clipCoverage0 = (float)max(info.pageTableResolution, 1u) * TerrainRvtBasePageWorldSize(info);
    const float extraLevels = ceil(log2(max(terrainExtent / max(clipCoverage0, 1.0e-4f), 1.0f)));
    return min(info.maxClipLevels, 1u + (uint)max(extraLevels, 0.0f));
}

uint TerrainRvtTerrainClipCount(TerrainRvtInfo info, TerrainSetInfo terrain)
{
    const uint configuredClipCount = min(max(info.mipCount, 1u), max(info.maxClipLevels, 1u));
    return min(max(configuredClipCount, TerrainRvtRequiredClipCount(info, terrain)), max(info.maxClipLevels, 1u));
}

uint TerrainRvtWrapPageCoord(uint coord, uint origin, uint resolution)
{
    if (resolution == 0u)
    {
        return 0u;
    }
    return coord % resolution;
}

uint TerrainRvtUnwrapLocalPageCoord(uint wrappedCoord, uint origin, uint resolution)
{
    if (resolution == 0u)
    {
        return 0u;
    }
    const uint originWrapped = origin % resolution;
    const uint localOffset = (wrappedCoord + resolution - originWrapped) % resolution;
    return origin + localOffset;
}

uint TerrainRvtClipInfoIndex(TerrainRvtInfo info, uint terrainSetIndex, uint clipLevel)
{
    return terrainSetIndex * max(info.maxClipLevels, 1u) + clipLevel;
}

uint TerrainRvtPageTableSlot(TerrainRvtClipInfo clipInfo, uint2 pageCoord)
{
    const uint resolution = max(clipInfo.tableResolution, 1u);
    const uint2 wrappedPage = uint2(
        TerrainRvtWrapPageCoord(pageCoord.x, clipInfo.originPage.x, resolution),
        TerrainRvtWrapPageCoord(pageCoord.y, clipInfo.originPage.y, resolution));
    return clipInfo.tableBaseSlot + wrappedPage.y * resolution + wrappedPage.x;
}

bool TerrainRvtPageTagMatches(TerrainRvtPageTag tag, uint terrainSetIndex, uint clipLevel, uint2 pageCoord)
{
    return tag.terrainSetIndex == terrainSetIndex &&
        tag.clipLevel == clipLevel &&
        tag.pageX == pageCoord.x &&
        tag.pageY == pageCoord.y;
}

uint TerrainRvtPackPageTableEntry(uint physicalPageIndex, uint contentMask)
{
    return TERRAIN_RVT_PAGE_VALID |
        TERRAIN_RVT_PAGE_VISITED |
        ((contentMask & 0x3u) << TERRAIN_RVT_PAGE_CONTENT_SHIFT) |
        (physicalPageIndex & TERRAIN_RVT_PAGE_PHYSICAL_MASK);
}

uint TerrainRvtPageTablePhysicalPage(uint entry)
{
    return entry & TERRAIN_RVT_PAGE_PHYSICAL_MASK;
}

uint TerrainRvtPageTableContentMask(uint entry)
{
    return (entry & TERRAIN_RVT_PAGE_CONTENT_MASK) >> TERRAIN_RVT_PAGE_CONTENT_SHIFT;
}

bool TerrainRvtPageTableHasContent(uint entry, uint requiredContentMask)
{
    const uint requiredBits = (requiredContentMask & 0x3u) << TERRAIN_RVT_PAGE_CONTENT_SHIFT;
    return (entry & TERRAIN_RVT_PAGE_VALID) != 0u &&
        (entry & requiredBits) == requiredBits;
}

bool TerrainRvtUnpackPageTableEntry(uint entry, uint requiredContentMask, out uint physicalPageIndex)
{
    if (!TerrainRvtPageTableHasContent(entry, requiredContentMask))
    {
        physicalPageIndex = 0u;
        return false;
    }
    physicalPageIndex = TerrainRvtPageTablePhysicalPage(entry);
    return true;
}

struct TerrainRvtAddress
{
    uint terrainSetIndex;
    uint clipLevel;
    uint2 pageCoord;
    float2 pageUv;
    uint pageTableIndex;
};

bool TerrainRvtTryPrepareSampleLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    TerrainSetInfo terrain,
    float2 skyrimXY,
    out float2 local,
    out uint terrainClipCount)
{
    local = 0.0f.xx;
    terrainClipCount = 0u;
    if (terrainSetIndex >= info.maxTerrainSets ||
        terrain.regionSizeWorld <= 0.0f ||
        terrain.regionCountX == 0u ||
        terrain.regionCountY == 0u ||
        info.pageSize == 0u ||
        info.pageTableResolution == 0u ||
        info.maxClipLevels == 0u)
    {
        return false;
    }

    terrainClipCount = TerrainRvtTerrainClipCount(info, terrain);
    if (terrainClipCount == 0u)
    {
        return false;
    }

    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    local = skyrimXY - terrainOrigin;
    return all(local >= 0.0f.xx) && all(local < terrainSize);
}

bool TerrainRvtTryComputePageAtClipLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    float2 local,
    uint clipLevel,
    uint terrainClipCount,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    pageCoord = 0u.xx;
    pageUv = 0.0f.xx;
    pageTableIndex = 0xffffffffu;

    const uint resolvedClipLevel = min(clipLevel, terrainClipCount - 1u);
    if (resolvedClipLevel >= info.maxClipLevels)
    {
        return false;
    }

    StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
    const TerrainRvtClipInfo clipInfo = clipInfos[TerrainRvtClipInfoIndex(info, terrainSetIndex, resolvedClipLevel)];
    if (clipInfo.valid == 0u || clipInfo.tableResolution == 0u)
    {
        return false;
    }

    const float invPageWorldSize = rcp(max(clipInfo.pageWorldSize, 0.125f));
    const float2 pageFloat = local * invPageWorldSize;
    pageCoord = (uint2)floor(pageFloat);
    if (any(pageCoord >= clipInfo.terrainPageCount) ||
        any(pageCoord < clipInfo.originPage) ||
        pageCoord.x >= clipInfo.originPage.x + clipInfo.tableResolution ||
        pageCoord.y >= clipInfo.originPage.y + clipInfo.tableResolution)
    {
        return false;
    }

    pageUv = frac(pageFloat);
    pageTableIndex = TerrainRvtPageTableSlot(clipInfo, pageCoord);
    return pageTableIndex < info.maxVirtualPageTableEntries;
}

bool TerrainRvtTryComputePageLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    float2 local,
    float2 skyrimXYDdx,
    float2 skyrimXYDdy,
    uint terrainClipCount,
    out uint mip,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    const float texelWorldSize0 = max(TerrainRvtBasePageWorldSize(info) / max((float)info.pageSize, 1.0f), 1.0e-4f);
    const float footprintWorld = max(length(skyrimXYDdx), length(skyrimXYDdy));
    const float requestedMip = 0.5f + log2(max(footprintWorld / texelWorldSize0, 1.0e-4f));
    mip = min((uint)max(0.0f, floor(requestedMip)), terrainClipCount - 1u);

    return TerrainRvtTryComputePageAtClipLocal(
        terrainSetIndex,
        info,
        local,
        mip,
        terrainClipCount,
        pageCoord,
        pageUv,
        pageTableIndex);
}

bool TerrainRvtTryComputePageAtClip(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    TerrainSetInfo terrain,
    float2 skyrimXY,
    uint clipLevel,
    out TerrainRvtAddress address)
{
    address = (TerrainRvtAddress)0;
    address.pageTableIndex = 0xffffffffu;
    address.terrainSetIndex = terrainSetIndex;

    float2 local;
    uint terrainClipCount;
    if (!TerrainRvtTryPrepareSampleLocal(terrainSetIndex, info, terrain, skyrimXY, local, terrainClipCount))
    {
        return false;
    }
    address.clipLevel = min(clipLevel, terrainClipCount - 1u);

    return TerrainRvtTryComputePageAtClipLocal(
        terrainSetIndex,
        info,
        local,
        address.clipLevel,
        terrainClipCount,
        address.pageCoord,
        address.pageUv,
        address.pageTableIndex);
}

bool TerrainRvtTryComputePage(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    TerrainSetInfo terrain,
    float2 skyrimXY,
    float2 skyrimXYDdx,
    float2 skyrimXYDdy,
    out uint mip,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    mip = 0u;
    pageCoord = 0u.xx;
    pageUv = 0.0f.xx;
    pageTableIndex = 0xffffffffu;
    float2 local;
    uint terrainClipCount;
    if (!TerrainRvtTryPrepareSampleLocal(terrainSetIndex, info, terrain, skyrimXY, local, terrainClipCount))
    {
        return false;
    }

    return TerrainRvtTryComputePageLocal(
        terrainSetIndex,
        info,
        local,
        skyrimXYDdx,
        skyrimXYDdy,
        terrainClipCount,
        mip,
        pageCoord,
        pageUv,
        pageTableIndex);
}

bool TerrainRvtTryComputePageFromPosition(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    out TerrainRvtInfo info,
    out TerrainSetInfo terrain,
    out uint mip,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    info = infoBuffer[0];
    terrain = terrainSets[terrainSetIndex];
    return TerrainRvtTryComputePage(
        terrainSetIndex,
        info,
        terrain,
        TerrainRvtSkyrimXYFromRendererPosition(positionWS),
        TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdxWS),
        TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdyWS),
        mip,
        pageCoord,
        pageUv,
        pageTableIndex);
}

bool TerrainRvtLookupResident(
    TerrainRvtAddress address,
    uint requiredContentMask,
    out uint physicalPageIndex)
{
    physicalPageIndex = 0u;
    StructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    StructuredBuffer<TerrainRvtPageTag> pageTags = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageKeys)];
    const TerrainRvtPageTag tag = pageTags[address.pageTableIndex];
    if (!TerrainRvtPageTagMatches(tag, address.terrainSetIndex, address.clipLevel, address.pageCoord))
    {
        return false;
    }
    const uint entry = pageTable[address.pageTableIndex];
    if (!TerrainRvtPageTableHasContent(entry, requiredContentMask))
    {
        return false;
    }
    physicalPageIndex = TerrainRvtPageTablePhysicalPage(entry);
    return true;
}

bool TerrainRvtLookupResidentPage(
    uint terrainSetIndex,
    uint clipLevel,
    uint2 pageCoord,
    uint pageTableIndex,
    uint requiredContentMask,
    out uint physicalPageIndex)
{
    physicalPageIndex = 0u;
    StructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    StructuredBuffer<TerrainRvtPageTag> pageTags = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageKeys)];
    const TerrainRvtPageTag tag = pageTags[pageTableIndex];
    if (!TerrainRvtPageTagMatches(tag, terrainSetIndex, clipLevel, pageCoord))
    {
        return false;
    }
    const uint entry = pageTable[pageTableIndex];
    if (!TerrainRvtPageTableHasContent(entry, requiredContentMask))
    {
        return false;
    }
    physicalPageIndex = TerrainRvtPageTablePhysicalPage(entry);
    return true;
}

void TerrainRvtMarkVisited(uint pageTableIndex)
{
}

void TerrainRvtPhysicalPageUv(
    TerrainRvtInfo info,
    uint physicalPageIndex,
    float2 pageUv,
    out float3 atlasUv)
{
    const uint tileSide = max(info.physicalTileTexelSide, 1u);
    const uint pagesWide = max(info.physicalAtlasPagesWide, 1u);
    const uint poolCount = max(info.physicalAtlasPoolCount, 1u);
    uint poolIndex = 0u;
    uint localPhysicalPageIndex = physicalPageIndex;
    if (poolCount > 1u)
    {
        const uint pagesPerPool = max(info.physicalAtlasPagesWide * info.physicalAtlasPagesHigh, 1u);
        poolIndex = min(physicalPageIndex / pagesPerPool, poolCount - 1u);
        localPhysicalPageIndex = physicalPageIndex - poolIndex * pagesPerPool;
    }
    const uint2 physicalPage = uint2(localPhysicalPageIndex % pagesWide, localPhysicalPageIndex / pagesWide);
    const float2 atlasTexel = (float2)physicalPage * (float)tileSide +
        (float)info.borderTexels +
        pageUv * (float)max(info.pageSize, 1u);
    const float2 atlasSize = float2(
        pagesWide * tileSide,
        max(info.physicalAtlasPagesHigh, 1u) * tileSide);
    atlasUv = float3(atlasTexel / atlasSize, (float)poolIndex);
}

float2 TerrainRvtPhysicalTileUv(TerrainRvtInfo info, float2 pageUv)
{
    return ((float)info.borderTexels + pageUv * (float)max(info.pageSize, 1u)) /
        (float)max(info.physicalTileTexelSide, 1u);
}

bool TerrainRvtValidatePhysicalOwner(uint physicalPageIndex, uint pageTableIndex)
{
    StructuredBuffer<uint4> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    const uint4 owner = physicalPageOwner[physicalPageIndex];
    return (owner.z & TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT) != 0u && owner.x == pageTableIndex;
}

bool TerrainRvtTryFindResident(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    TerrainSetInfo terrain,
    float3 positionWS,
    uint requestedMip,
    uint requiredContentMask,
    out TerrainRvtAddress residentAddress,
    out uint physicalPageIndex)
{
    residentAddress = (TerrainRvtAddress)0;
    residentAddress.pageTableIndex = 0xffffffffu;
    physicalPageIndex = 0u;
    const float2 skyrimXY = TerrainRvtSkyrimXYFromRendererPosition(positionWS);
    float2 local;
    uint terrainClipCount;
    if (!TerrainRvtTryPrepareSampleLocal(terrainSetIndex, info, terrain, skyrimXY, local, terrainClipCount))
    {
        return false;
    }

    [loop]
    for (uint sampleMip = requestedMip; sampleMip < terrainClipCount; ++sampleMip)
    {
        uint2 pageCoord;
        float2 pageUv;
        uint pageTableIndex;
        if (!TerrainRvtTryComputePageAtClipLocal(terrainSetIndex, info, local, sampleMip, terrainClipCount, pageCoord, pageUv, pageTableIndex))
        {
            continue;
        }
        if (TerrainRvtLookupResidentPage(terrainSetIndex, sampleMip, pageCoord, pageTableIndex, requiredContentMask, physicalPageIndex))
        {
            residentAddress.terrainSetIndex = terrainSetIndex;
            residentAddress.clipLevel = sampleMip;
            residentAddress.pageCoord = pageCoord;
            residentAddress.pageUv = pageUv;
            residentAddress.pageTableIndex = pageTableIndex;
            return true;
        }
    }
    return false;
}

bool TerrainRvtTryFindResidentLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    float2 local,
    uint terrainClipCount,
    uint requestedMip,
    uint requiredContentMask,
    out TerrainRvtAddress residentAddress,
    out uint physicalPageIndex)
{
    residentAddress = (TerrainRvtAddress)0;
    residentAddress.pageTableIndex = 0xffffffffu;
    physicalPageIndex = 0u;

    [loop]
    for (uint sampleMip = requestedMip; sampleMip < terrainClipCount; ++sampleMip)
    {
        uint2 pageCoord;
        float2 pageUv;
        uint pageTableIndex;
        if (!TerrainRvtTryComputePageAtClipLocal(terrainSetIndex, info, local, sampleMip, terrainClipCount, pageCoord, pageUv, pageTableIndex))
        {
            continue;
        }
        if (TerrainRvtLookupResidentPage(terrainSetIndex, sampleMip, pageCoord, pageTableIndex, requiredContentMask, physicalPageIndex))
        {
            residentAddress.terrainSetIndex = terrainSetIndex;
            residentAddress.clipLevel = sampleMip;
            residentAddress.pageCoord = pageCoord;
            residentAddress.pageUv = pageUv;
            residentAddress.pageTableIndex = pageTableIndex;
            return true;
        }
    }
    return false;
}

void TerrainRvtMarkPageTableIndex(uint pageTableIndex, uint contentMask);

bool TerrainRvtTrySampleHeightFast(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    out float heightValue)
{
    heightValue = 0.0f;
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = TERRAIN_RVT_TELEMETRY_ENABLED(perFrame);
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleAttempts, 1u);
        InterlockedAdd(stats[0].heightFastSampleAttempts, 1u);
    }

    TerrainRvtInfo info;
    TerrainSetInfo terrain;
    uint mip;
    uint2 requestedPageCoord;
    float2 requestedPageUv;
    uint requestedPageTableIndex;
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    info = infoBuffer[0];
    terrain = terrainSets[terrainSetIndex];

    const float2 skyrimXY = TerrainRvtSkyrimXYFromRendererPosition(positionWS);
    float2 local;
    uint terrainClipCount;
    if (!TerrainRvtTryPrepareSampleLocal(terrainSetIndex, info, terrain, skyrimXY, local, terrainClipCount) ||
        !TerrainRvtTryComputePageLocal(
            terrainSetIndex,
            info,
            local,
            TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdxWS),
            TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdyWS),
            terrainClipCount,
            mip,
            requestedPageCoord,
            requestedPageUv,
            requestedPageTableIndex))
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].heightFallbacks, 1u);
            InterlockedAdd(stats[0].heightComputePageFailures, 1u);
        }
        return false;
    }
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleMipHistogram[TerrainRvtTelemetryMipBin(mip)], 1u);
        InterlockedXor(stats[0].heightSampleAttemptedPageXor, requestedPageTableIndex);
        InterlockedMin(stats[0].heightSampleAttemptedPageMin, requestedPageTableIndex);
        InterlockedMax(stats[0].heightSampleAttemptedPageMax, requestedPageTableIndex);
    }
    TerrainRvtAddress residentAddress = (TerrainRvtAddress)0;
    residentAddress.terrainSetIndex = terrainSetIndex;
    residentAddress.clipLevel = mip;
    residentAddress.pageCoord = requestedPageCoord;
    residentAddress.pageUv = requestedPageUv;
    residentAddress.pageTableIndex = requestedPageTableIndex;
    uint physicalPageIndex = 0u;
    if (!TerrainRvtLookupResidentPage(terrainSetIndex, mip, requestedPageCoord, requestedPageTableIndex, TERRAIN_RVT_CONTENT_HEIGHT, physicalPageIndex)
#if TERRAIN_RVT_ENABLE_COARSER_RESIDENT_FALLBACK
        && !TerrainRvtTryFindResidentLocal(terrainSetIndex, info, local, terrainClipCount, mip + 1u, TERRAIN_RVT_CONTENT_HEIGHT, residentAddress, physicalPageIndex)
#endif
        )
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].heightFallbacks, 1u);
            InterlockedAdd(stats[0].heightPageTableMisses, 1u);
            InterlockedXor(stats[0].heightSamplePageMissRequestedPageXor, requestedPageTableIndex);
            InterlockedMin(stats[0].heightSamplePageMissRequestedPageMin, requestedPageTableIndex);
            InterlockedMax(stats[0].heightSamplePageMissRequestedPageMax, requestedPageTableIndex);
        }
        return false;
    }
#if TERRAIN_RVT_VALIDATE_SAMPLE_OWNER
    if (!TerrainRvtValidatePhysicalOwner(physicalPageIndex, residentAddress.pageTableIndex))
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].heightFallbacks, 1u);
            InterlockedAdd(stats[0].heightOwnerMismatches, 1u);
        }
        return false;
    }
#endif

    TerrainRvtMarkVisited(residentAddress.pageTableIndex);
    float3 atlasUv;
    TerrainRvtPhysicalPageUv(info, physicalPageIndex, residentAddress.pageUv, atlasUv);
    Texture2DArray<float> heightAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtHeightAtlas)];
    heightValue = saturate(heightAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f));
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleHits, 1u);
        InterlockedAdd(stats[0].heightFastSampleHits, 1u);
    }
    return true;
}

bool TerrainRvtTrySampleHeight(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    out float heightValue)
{
    heightValue = 0.0f;
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = TERRAIN_RVT_TELEMETRY_ENABLED(perFrame);
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightFullSampleAttempts, 1u);
    }
    if (perFrame.terrainRvtEnabled == 0u)
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].heightFallbacks, 1u);
            InterlockedAdd(stats[0].heightDisabledFallbacks, 1u);
        }
        return false;
    }
    if (perFrame.terrainRvtForceDirectFallback != 0u)
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].heightFallbacks, 1u);
            InterlockedAdd(stats[0].heightForcedFallbacks, 1u);
        }
        return false;
    }
    const bool result = TerrainRvtTrySampleHeightFast(terrainSetIndex, positionWS, dpdxWS, dpdyWS, heightValue);
    if (result && telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightFullSampleHits, 1u);
    }
    return result;
}

struct TerrainRvtMaterialSample
{
    float3 albedo;
    float3 albedoPoint;
    float3 normalTS;
    float3 normalWS;
    float roughness;
    float metallic;
    float ambientOcclusion;
    uint requestedMip;
    uint residentMip;
    uint requestedPageTableIndex;
    uint residentPageTableIndex;
    uint physicalPageIndex;
    uint atlasPoolIndex;
    uint ownerPageTableIndex;
    uint fallbackReason;
    uint2 pageCoord;
    float2 pageUv;
    float3 atlasUv;
    float2 physicalTileUv;
    float pageStamp;
    float expectedPageStamp;
    float pageStampDelta;
};

float3x3 TerrainRvtTerrainBasis(float3 normalWS)
{
    float3 tangentWS = cross(float3(0.0f, 0.0f, -1.0f), normalWS);
    if (dot(tangentWS, tangentWS) < 1.0e-5f)
    {
        tangentWS = cross(float3(1.0f, 0.0f, 0.0f), normalWS);
    }
    tangentWS = normalize(tangentWS);
    float3 bitangentWS = normalize(cross(normalWS, tangentWS));
    return float3x3(tangentWS, bitangentWS, normalWS);
}

float3 TerrainRvtTangentToWorldNormal(float3 normalTS, float3 normalWSBase)
{
    return normalize(mul(normalize(normalTS), TerrainRvtTerrainBasis(normalize(normalWSBase))));
}

float3 TerrainRvtWorldToTangentNormal(float3 normalWS, float3 normalWSBase)
{
    return normalize(mul(normalize(normalWS), transpose(TerrainRvtTerrainBasis(normalize(normalWSBase)))));
}

bool TerrainRvtTrySampleMaterial(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    float3 normalWSBase,
    out TerrainRvtMaterialSample sampleOut)
{
    sampleOut = (TerrainRvtMaterialSample)0;
    sampleOut.requestedMip = MATERIAL_DEBUG_INVALID_MIP_LEVEL;
    sampleOut.residentMip = MATERIAL_DEBUG_INVALID_MIP_LEVEL;
    sampleOut.requestedPageTableIndex = 0xffffffffu;
    sampleOut.residentPageTableIndex = 0xffffffffu;
    sampleOut.physicalPageIndex = 0xffffffffu;
    sampleOut.atlasPoolIndex = 0xffffffffu;
    sampleOut.ownerPageTableIndex = 0xffffffffu;
    sampleOut.pageCoord = 0xffffffffu.xx;

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = TERRAIN_RVT_TELEMETRY_ENABLED(perFrame);
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].materialSampleAttempts, 1u);
    }
    if (perFrame.terrainRvtEnabled == 0u)
    {
        sampleOut.fallbackReason = TERRAIN_RVT_FALLBACK_DISABLED;
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].materialFallbacks, 1u);
            InterlockedAdd(stats[0].materialDisabledFallbacks, 1u);
        }
        return false;
    }
    if (perFrame.terrainRvtForceDirectFallback != 0u)
    {
        sampleOut.fallbackReason = TERRAIN_RVT_FALLBACK_FORCED;
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].materialFallbacks, 1u);
            InterlockedAdd(stats[0].materialForcedFallbacks, 1u);
        }
        return false;
    }

    TerrainRvtInfo info;
    TerrainSetInfo terrain;
    uint mip;
    uint2 requestedPageCoord;
    float2 requestedPageUv;
    uint requestedPageTableIndex;
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    info = infoBuffer[0];
    terrain = terrainSets[terrainSetIndex];

    const float2 skyrimXY = TerrainRvtSkyrimXYFromRendererPosition(positionWS);
    float2 local;
    uint terrainClipCount;
    if (!TerrainRvtTryPrepareSampleLocal(terrainSetIndex, info, terrain, skyrimXY, local, terrainClipCount) ||
        !TerrainRvtTryComputePageLocal(
            terrainSetIndex,
            info,
            local,
            TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdxWS),
            TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdyWS),
            terrainClipCount,
            mip,
            requestedPageCoord,
            requestedPageUv,
            requestedPageTableIndex))
    {
        sampleOut.fallbackReason = TERRAIN_RVT_FALLBACK_COMPUTE_PAGE;
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].materialFallbacks, 1u);
            InterlockedAdd(stats[0].materialComputePageFailures, 1u);
        }
        return false;
    }

    sampleOut.requestedMip = mip;
    sampleOut.requestedPageTableIndex = requestedPageTableIndex;
    TerrainRvtMarkPageTableIndex(requestedPageTableIndex, TERRAIN_RVT_CONTENT_MATERIAL | TERRAIN_RVT_CONTENT_HEIGHT);
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].materialSampleMipHistogram[TerrainRvtTelemetryMipBin(mip)], 1u);
        InterlockedXor(stats[0].materialSampleAttemptedPageXor, requestedPageTableIndex);
        InterlockedMin(stats[0].materialSampleAttemptedPageMin, requestedPageTableIndex);
        InterlockedMax(stats[0].materialSampleAttemptedPageMax, requestedPageTableIndex);
    }

    TerrainRvtAddress residentAddress = (TerrainRvtAddress)0;
    residentAddress.terrainSetIndex = terrainSetIndex;
    residentAddress.clipLevel = mip;
    residentAddress.pageCoord = requestedPageCoord;
    residentAddress.pageUv = requestedPageUv;
    residentAddress.pageTableIndex = requestedPageTableIndex;
    uint physicalPageIndex = 0u;
    if (!TerrainRvtLookupResidentPage(terrainSetIndex, mip, requestedPageCoord, requestedPageTableIndex, TERRAIN_RVT_CONTENT_MATERIAL, physicalPageIndex)
#if TERRAIN_RVT_ENABLE_COARSER_RESIDENT_FALLBACK
        && !TerrainRvtTryFindResidentLocal(terrainSetIndex, info, local, terrainClipCount, mip + 1u, TERRAIN_RVT_CONTENT_MATERIAL, residentAddress, physicalPageIndex)
#endif
        )
    {
        sampleOut.fallbackReason = TERRAIN_RVT_FALLBACK_PAGE_MISS;
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].materialFallbacks, 1u);
            InterlockedAdd(stats[0].materialPageTableMisses, 1u);
            InterlockedXor(stats[0].materialSamplePageMissRequestedPageXor, requestedPageTableIndex);
            InterlockedMin(stats[0].materialSamplePageMissRequestedPageMin, requestedPageTableIndex);
            InterlockedMax(stats[0].materialSamplePageMissRequestedPageMax, requestedPageTableIndex);
        }
        return false;
    }

#if TERRAIN_RVT_VALIDATE_SAMPLE_OWNER
    StructuredBuffer<uint4> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    const uint4 owner = physicalPageOwner[physicalPageIndex];
    sampleOut.ownerPageTableIndex = owner.x;
    sampleOut.physicalPageIndex = physicalPageIndex;
    if ((owner.z & TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT) == 0u || owner.x != residentAddress.pageTableIndex)
    {
        sampleOut.fallbackReason = TERRAIN_RVT_FALLBACK_OWNER_MISMATCH;
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].materialFallbacks, 1u);
            InterlockedAdd(stats[0].materialOwnerMismatches, 1u);
        }
        return false;
    }
#else
    sampleOut.ownerPageTableIndex = residentAddress.pageTableIndex;
#endif

    TerrainRvtMarkVisited(residentAddress.pageTableIndex);
    float3 atlasUv;
    TerrainRvtPhysicalPageUv(info, physicalPageIndex, residentAddress.pageUv, atlasUv);
    const uint atlasPoolIndex = (uint)atlasUv.z;
    Texture2DArray<float4> albedoAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtAlbedoAtlas)];
    Texture2DArray<float4> normalAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtNormalAtlas)];
    Texture2DArray<float4> materialAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtMaterialAtlas)];

    sampleOut.albedo = albedoAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f).rgb;
#if TERRAIN_RVT_ENABLE_HOT_SAMPLE_DEBUG
    sampleOut.albedoPoint = albedoAtlas.SampleLevel(g_pointClamp, atlasUv, 0.0f).rgb;
#endif
    sampleOut.normalTS = normalize(normalAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f).xyz * 2.0f - 1.0f);
    sampleOut.normalWS = TerrainRvtTangentToWorldNormal(sampleOut.normalTS, normalWSBase);
    const float4 materialParams = materialAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f);
    sampleOut.roughness = materialParams.r;
    sampleOut.metallic = materialParams.g;
    sampleOut.ambientOcclusion = materialParams.b;
    sampleOut.residentMip = residentAddress.clipLevel;
    sampleOut.residentPageTableIndex = residentAddress.pageTableIndex;
    sampleOut.physicalPageIndex = physicalPageIndex;
    sampleOut.atlasPoolIndex = atlasPoolIndex;
    sampleOut.fallbackReason = TERRAIN_RVT_FALLBACK_NONE;
    sampleOut.pageCoord = residentAddress.pageCoord;
    sampleOut.pageUv = residentAddress.pageUv;
    sampleOut.atlasUv = atlasUv;
#if TERRAIN_RVT_ENABLE_HOT_SAMPLE_DEBUG
    sampleOut.physicalTileUv = TerrainRvtPhysicalTileUv(info, residentAddress.pageUv);
#endif

    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].materialSampleHits, 1u);
        InterlockedXor(stats[0].materialSampleRequestedPageXor, sampleOut.requestedPageTableIndex);
        InterlockedXor(stats[0].materialSampleResidentPageXor, sampleOut.residentPageTableIndex);
        InterlockedXor(stats[0].materialSamplePhysicalPageXor, sampleOut.physicalPageIndex);
        InterlockedMin(stats[0].materialSampleRequestedPageMin, sampleOut.requestedPageTableIndex);
        InterlockedMax(stats[0].materialSampleRequestedPageMax, sampleOut.requestedPageTableIndex);
        InterlockedMin(stats[0].materialSampleResidentPageMin, sampleOut.residentPageTableIndex);
        InterlockedMax(stats[0].materialSampleResidentPageMax, sampleOut.residentPageTableIndex);
        InterlockedMin(stats[0].materialSamplePhysicalPageMin, sampleOut.physicalPageIndex);
        InterlockedMax(stats[0].materialSamplePhysicalPageMax, sampleOut.physicalPageIndex);
        if (sampleOut.residentMip != sampleOut.requestedMip)
        {
            InterlockedAdd(stats[0].materialSampleCoarserResidentHits, 1u);
        }
        InterlockedOr(stats[0].materialSampleAtlasPoolMask, 1u << min(sampleOut.atlasPoolIndex, 31u));
    }
    return true;
}

void TerrainRvtMarkPageTableIndex(uint pageTableIndex, uint contentMask)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    TerrainRvtInfo info = infoBuffer[0];
    if (pageTableIndex >= info.maxVirtualPageTableEntries)
    {
        return;
    }

    StructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    const uint currentEntry = pageTable[pageTableIndex];
    uint physicalPageIndex = 0u;
    uint currentContentMask = 0u;
    bool ownerValid = false;
    if ((currentEntry & TERRAIN_RVT_PAGE_VALID) != 0u)
    {
        physicalPageIndex = TerrainRvtPageTablePhysicalPage(currentEntry);
        currentContentMask = TerrainRvtPageTableContentMask(currentEntry);
        if (physicalPageIndex < info.maxPhysicalPages)
        {
#if TERRAIN_RVT_VALIDATE_SAMPLE_OWNER
            StructuredBuffer<uint4> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
            const uint4 owner = physicalPageOwner[physicalPageIndex];
            ownerValid = (owner.z & TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT) != 0u && owner.x == pageTableIndex;
#else
            ownerValid = true;
#endif
            if (ownerValid && (currentContentMask & contentMask) == contentMask)
            {
                ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
                if (TERRAIN_RVT_TELEMETRY_ENABLED(perFrame))
                {
                    RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                    InterlockedAdd(stats[0].residentHits, 1u);
                }
                TerrainRvtMarkVisited(pageTableIndex);
            }
        }
    }

    RWStructuredBuffer<uint> requestMasks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestMasks)];
    RWStructuredBuffer<TerrainRvtPageRequest> requestList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestList)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = TERRAIN_RVT_TELEMETRY_ENABLED(perFrame);

    const uint contentBits = contentMask & TERRAIN_RVT_REQUEST_MASK_CONTENT_MASK;
    uint previousMask = 0u;
    InterlockedOr(requestMasks[pageTableIndex], contentBits, previousMask);
    const uint previousContentMask = previousMask & TERRAIN_RVT_REQUEST_MASK_CONTENT_MASK;
    const uint requestedMask = (previousContentMask | contentBits) & TERRAIN_RVT_REQUEST_MASK_CONTENT_MASK;
    const uint newlyMarkedMask = contentBits & ~previousContentMask;
    const bool residentSatisfied = ownerValid && ((currentContentMask & requestedMask) == requestedMask);
    if (newlyMarkedMask == 0u || residentSatisfied)
    {
        return;
    }
    const uint missingMask = ownerValid ? (requestedMask & ~currentContentMask) : requestedMask;

    StructuredBuffer<TerrainRvtPageTag> pageTags = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageKeys)];
    const TerrainRvtPageTag tag = pageTags[pageTableIndex];

    if (telemetryEnabled && (missingMask & TERRAIN_RVT_CONTENT_HEIGHT) != 0u)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightRequests, 1u);
        InterlockedAdd(stats[0].heightRequestMipHistogram[TerrainRvtTelemetryMipBin(tag.clipLevel)], 1u);
    }
    if (telemetryEnabled && (missingMask & TERRAIN_RVT_CONTENT_MATERIAL) != 0u)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].materialRequests, 1u);
        InterlockedAdd(stats[0].materialRequestMipHistogram[TerrainRvtTelemetryMipBin(tag.clipLevel)], 1u);
    }
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedXor(stats[0].requestPageTableXor, pageTableIndex);
        InterlockedMin(stats[0].requestPageTableMin, pageTableIndex);
        InterlockedMax(stats[0].requestPageTableMax, pageTableIndex);
    }

    uint appendPreviousMask = 0u;
    InterlockedOr(requestMasks[pageTableIndex], TERRAIN_RVT_REQUEST_MASK_APPENDED, appendPreviousMask);
    if ((appendPreviousMask & TERRAIN_RVT_REQUEST_MASK_APPENDED) != 0u)
    {
        return;
    }

    uint requestIndex = 0u;
    InterlockedAdd(counters[TERRAIN_RVT_COUNTER_REQUEST_COUNT], 1u, requestIndex);
    if (requestIndex < info.maxRequests)
    {
        TerrainRvtPageRequest request;
        request.pageTableIndex = pageTableIndex;
        request.terrainSetIndex = tag.terrainSetIndex;
        request.clipLevel = tag.clipLevel;
        request.contentMask = missingMask;
        request.pageX = tag.pageX;
        request.pageY = tag.pageY;
        request.pad0 = 0u;
        request.pad1 = 0u;
        requestList[requestIndex] = request;
    }
    else
    {
        InterlockedAdd(counters[TERRAIN_RVT_COUNTER_OVERFLOW_COUNT], 1u);
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].requestOverflows, 1u);
        }
    }
}

bool TerrainRvtMarkPosition(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    uint contentMask)
{
    TerrainRvtInfo info;
    TerrainSetInfo terrain;
    uint mip;
    uint2 pageCoord;
    float2 pageUv;
    uint pageTableIndex;
    if (!TerrainRvtTryComputePageFromPosition(terrainSetIndex, positionWS, dpdxWS, dpdyWS, info, terrain, mip, pageCoord, pageUv, pageTableIndex))
    {
        ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
        if (TERRAIN_RVT_TELEMETRY_ENABLED(perFrame))
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].markComputePageFailures, 1u);
        }
        return false;
    }
    TerrainRvtMarkPageTableIndex(pageTableIndex, contentMask);
    return true;
}

uint TerrainRvtMipForWorldRect(TerrainRvtInfo info, float2 minSkyrimXY, float2 maxSkyrimXY, float targetPagesPerAxis)
{
    const float extent = max(maxSkyrimXY.x - minSkyrimXY.x, maxSkyrimXY.y - minSkyrimXY.y);
    const float targetPageWorldSize = TerrainRvtBasePageWorldSize(info) * max(targetPagesPerAxis, 1.0f);
    const float requestedMip = log2(max(extent / max(targetPageWorldSize, 1.0e-4f), 1.0f));
    return min((uint)max(0.0f, floor(requestedMip)), max(info.mipCount, 1u) - 1u);
}

void TerrainRvtMarkWorldRect(
    uint terrainSetIndex,
    float2 minSkyrimXY,
    float2 maxSkyrimXY,
    uint mip,
    uint contentMask)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = TERRAIN_RVT_TELEMETRY_ENABLED(perFrame);
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].markWorldRectCalls, 1u);
    }

    TerrainRvtInfo info = infoBuffer[0];
    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    if (terrain.regionSizeWorld <= 0.0f || info.pageTableResolution == 0u)
    {
        return;
    }

    const uint terrainClipCount = TerrainRvtTerrainClipCount(info, terrain);
    mip = min(mip, terrainClipCount - 1u);
    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float pageWorldSize = TerrainRvtPageWorldSize(info, mip);
    int2 minPage = (int2)floor((minSkyrimXY - terrainOrigin) / pageWorldSize);
    int2 maxPage = (int2)floor((maxSkyrimXY - terrainOrigin) / pageWorldSize);
    const uint2 terrainPageCount = TerrainRvtClipPageCount(info, terrain, mip);
    const int2 maxValidPage = int2(terrainPageCount - 1u);
    minPage = clamp(minPage, int2(0, 0), maxValidPage);
    maxPage = clamp(maxPage, int2(0, 0), maxValidPage);

    const uint pageCount = (uint)((maxPage.x - minPage.x + 1) * (maxPage.y - minPage.y + 1));
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].markWorldRectPages, pageCount);
    }

    [loop]
    for (int y = minPage.y; y <= maxPage.y; ++y)
    {
        [loop]
        for (int x = minPage.x; x <= maxPage.x; ++x)
        {
            TerrainRvtAddress address;
            const float2 pageCenterSkyrimXY = terrainOrigin + (float2((float)x + 0.5f, (float)y + 0.5f) * pageWorldSize);
            if (TerrainRvtTryComputePageAtClip(terrainSetIndex, info, terrain, pageCenterSkyrimXY, mip, address))
            {
                TerrainRvtMarkPageTableIndex(address.pageTableIndex, contentMask);
            }
        }
    }
}

#endif // __TERRAIN_RVT_COMMON_HLSLI__
