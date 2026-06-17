#ifndef __TERRAIN_RVT_COMMON_HLSLI__
#define __TERRAIN_RVT_COMMON_HLSLI__

#include "structs.hlsli"
#include "waveIntrinsicsHelpers.hlsli"

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
#define TERRAIN_RVT_VALIDATE_SAMPLE_OWNER 0
#define TERRAIN_RVT_ENABLE_HOT_SAMPLE_DEBUG 0
#define TERRAIN_RVT_ENABLE_DIRECT_FALLBACK 1
#define TERRAIN_RVT_ENABLE_COARSER_RESIDENT_FALLBACK 1
#define TERRAIN_RVT_ENABLE_WAVE_PAGE_LOOKUP 1
static const uint TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_MISSING = 0u;
static const uint TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_DIRECT = 1u;
static const uint TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_COARSER = 2u;

#if defined(TERRAIN_RVT_TELEMETRY)
#define TERRAIN_RVT_TELEMETRY_ENABLED(perFrame) ((perFrame).terrainRvtTelemetryEnabled != 0u)
#else
#define TERRAIN_RVT_TELEMETRY_ENABLED(perFrame) false
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

float TerrainRvtTerrainExtentWorld(TerrainSetInfo terrain)
{
    return max((float)terrain.regionCountX, (float)terrain.regionCountY) * terrain.regionSizeWorld;
}

uint TerrainRvtTerrainClipCount(TerrainRvtInfo info, TerrainSetInfo terrain)
{
    return min(max(info.mipCount, 1u), max(info.maxClipLevels, 1u));
}

float TerrainRvtFarPageWorldSize(TerrainRvtInfo info, TerrainSetInfo terrain)
{
    const float terrainExtent = TerrainRvtTerrainExtentWorld(terrain);
    // The clip page table is centered on the camera. If its full width is only
    // one terrain extent, the usable camera-to-edge coverage is roughly half
    // that. Size the final clip as a coverage diameter so the active ladder can
    // reach the full loaded-terrain distance.
    const float farCoverageDiameter = terrainExtent * 2.0f;
    const float farPageWorldSize = farCoverageDiameter / max((float)info.pageTableResolution, 1.0f);
    return max(TerrainRvtBasePageWorldSize(info), farPageWorldSize);
}

float TerrainRvtClipRatio(TerrainRvtInfo info, TerrainSetInfo terrain)
{
    const uint clipCount = TerrainRvtTerrainClipCount(info, terrain);
    return clipCount <= 1u ? 1.0f : 2.0f;
}

float TerrainRvtPageWorldSize(TerrainRvtInfo info, TerrainSetInfo terrain, uint clipLevel)
{
    const uint clipCount = TerrainRvtTerrainClipCount(info, terrain);
    const uint resolvedClipLevel = min(clipLevel, max(clipCount, 1u) - 1u);
    return TerrainRvtBasePageWorldSize(info) * exp2((float)resolvedClipLevel);
}

float TerrainRvtClipTexelWorldSize(TerrainRvtInfo info, TerrainSetInfo terrain, uint clipLevel)
{
    return TerrainRvtPageWorldSize(info, terrain, clipLevel) / max((float)info.pageSize, 1.0f);
}

uint2 TerrainRvtClipPageCount(TerrainRvtInfo info, TerrainSetInfo terrain, uint clipLevel)
{
    const float pageWorldSize = TerrainRvtPageWorldSize(info, terrain, clipLevel);
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    return max(uint2(1u, 1u), (uint2)ceil(terrainSize / max(pageWorldSize, 0.125f)));
}

uint TerrainRvtSelectClipForFootprintWorld(TerrainRvtInfo info, TerrainSetInfo terrain, uint terrainClipCount, float footprintWorld)
{
    const uint clipCount = min(max(terrainClipCount, 1u), max(info.maxClipLevels, 1u));
    const float targetTexelWorldSize = max(footprintWorld, 1.0e-4f);
    uint selectedClip = 0u;

    if (clipCount > 1u)
    {
        const float basePageWorldSize = TerrainRvtBasePageWorldSize(info);
        const float baseTexelWorldSize = basePageWorldSize / max((float)info.pageSize, 1.0f);
        const float clipFloat = log2(max(targetTexelWorldSize / max(baseTexelWorldSize, 1.0e-8f), 1.0e-8f));
        selectedClip = (uint)clamp(floor(clipFloat), 0.0f, (float)(clipCount - 1u));
    }

    const float biasedClip = (float)selectedClip + info.mipOffset;
    return min((uint)max(0.0f, floor(biasedClip)), clipCount - 1u);
}

uint TerrainRvtWrapPageCoord(uint coord, uint origin, uint resolution)
{
    if (resolution == 0u)
    {
        return 0u;
    }
    if ((resolution & (resolution - 1u)) == 0u)
    {
        return coord & (resolution - 1u);
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
    const uint unwrappedOffset = wrappedCoord + resolution - originWrapped;
    const uint localOffset = (resolution & (resolution - 1u)) == 0u
        ? (unwrappedOffset & (resolution - 1u))
        : (unwrappedOffset % resolution);
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

struct TerrainRvtSampleContext
{
    TerrainRvtInfo info;
    TerrainSetInfo terrain;
    uint terrainSetIndex;
    float2 terrainOrigin;
    float2 terrainSize;
    uint terrainClipCount;
    uint valid;
};

TerrainRvtSampleContext TerrainRvtLoadSampleContext(uint terrainSetIndex)
{
    TerrainRvtSampleContext ctx = (TerrainRvtSampleContext)0;
    ctx.terrainSetIndex = terrainSetIndex;

    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    ctx.info = infoBuffer[0];
    if (terrainSetIndex >= ctx.info.maxTerrainSets)
    {
        return ctx;
    }

    ctx.terrain = terrainSets[terrainSetIndex];
    if (ctx.terrain.regionSizeWorld <= 0.0f ||
        ctx.terrain.regionCountX == 0u ||
        ctx.terrain.regionCountY == 0u ||
        ctx.info.pageSize == 0u ||
        ctx.info.pageTableResolution == 0u ||
        ctx.info.maxClipLevels == 0u)
    {
        return ctx;
    }

    StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
    const TerrainRvtClipInfo clip0 = clipInfos[TerrainRvtClipInfoIndex(ctx.info, terrainSetIndex, 0u)];
    ctx.terrainClipCount = min(clip0.terrainClipCount, ctx.info.maxClipLevels);
    if (clip0.valid == 0u || ctx.terrainClipCount == 0u)
    {
        return ctx;
    }

    ctx.terrainOrigin = float2(ctx.terrain.minRegionX, ctx.terrain.minRegionY) * ctx.terrain.regionSizeWorld;
    ctx.terrainSize = float2(ctx.terrain.regionCountX, ctx.terrain.regionCountY) * ctx.terrain.regionSizeWorld;
    ctx.valid = 1u;
    return ctx;
}

bool TerrainRvtTryPrepareSampleContextLocal(
    TerrainRvtSampleContext ctx,
    float2 skyrimXY,
    out float2 local)
{
    local = skyrimXY - ctx.terrainOrigin;
    return ctx.valid != 0u && all(local >= 0.0f.xx) && all(local < ctx.terrainSize);
}

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

    StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
    const TerrainRvtClipInfo clip0 = clipInfos[TerrainRvtClipInfoIndex(info, terrainSetIndex, 0u)];
    terrainClipCount = clip0.valid != 0u ? min(clip0.terrainClipCount, info.maxClipLevels) : 0u;
    if (terrainClipCount == 0u)
    {
        return false;
    }

    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    local = skyrimXY - terrainOrigin;
    return all(local >= 0.0f.xx) && all(local < terrainSize);
}

bool TerrainRvtTryComputePageInClipInfoLocal(
    TerrainRvtInfo info,
    TerrainRvtClipInfo clipInfo,
    float2 local,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex);

void TerrainRvtComputePageInClipInfoLocalUnchecked(
    TerrainRvtClipInfo clipInfo,
    float2 local,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex);

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
    return TerrainRvtTryComputePageInClipInfoLocal(info, clipInfo, local, pageCoord, pageUv, pageTableIndex);
}

bool TerrainRvtTryComputePageInClipInfoLocal(
    TerrainRvtInfo info,
    TerrainRvtClipInfo clipInfo,
    float2 local,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    pageCoord = 0u.xx;
    pageUv = 0.0f.xx;
    pageTableIndex = 0xffffffffu;

    if (clipInfo.valid == 0u || clipInfo.tableResolution == 0u)
    {
        return false;
    }

    const float2 pageFloat = local * clipInfo.invPageWorldSize;
    pageCoord = (uint2)pageFloat;
    if (any(pageCoord >= clipInfo.terrainPageCount) ||
        any(pageCoord < clipInfo.originPage) ||
        pageCoord.x >= clipInfo.originPage.x + clipInfo.tableResolution ||
        pageCoord.y >= clipInfo.originPage.y + clipInfo.tableResolution)
    {
        return false;
    }

    pageUv = pageFloat - (float2)pageCoord;
    pageTableIndex = TerrainRvtPageTableSlot(clipInfo, pageCoord);
    return pageTableIndex < info.maxVirtualPageTableEntries;
}

void TerrainRvtComputePageInClipInfoLocalUnchecked(
    TerrainRvtClipInfo clipInfo,
    float2 local,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    const float2 pageFloat = local * clipInfo.invPageWorldSize;
    pageCoord = (uint2)pageFloat;
    pageUv = pageFloat - (float2)pageCoord;
    pageTableIndex = TerrainRvtPageTableSlot(clipInfo, pageCoord);
}

bool TerrainRvtTryValidateComputedPageInClip(
    TerrainRvtInfo info,
    TerrainRvtClipInfo clipInfo,
    uint2 pageCoord,
    out uint pageTableIndex)
{
    pageTableIndex = 0xffffffffu;
    if (clipInfo.valid == 0u || clipInfo.tableResolution == 0u)
    {
        return false;
    }
    if (any(pageCoord >= clipInfo.terrainPageCount) ||
        any(pageCoord < clipInfo.originPage) ||
        pageCoord.x >= clipInfo.originPage.x + clipInfo.tableResolution ||
        pageCoord.y >= clipInfo.originPage.y + clipInfo.tableResolution)
    {
        return false;
    }

    pageTableIndex = TerrainRvtPageTableSlot(clipInfo, pageCoord);
    return pageTableIndex < info.maxVirtualPageTableEntries;
}

bool TerrainRvtTryComputePageLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    TerrainSetInfo terrain,
    float2 local,
    float2 skyrimXYDdx,
    float2 skyrimXYDdy,
    uint terrainClipCount,
    out uint mip,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    const float footprintWorldSq = max(dot(skyrimXYDdx, skyrimXYDdx), dot(skyrimXYDdy, skyrimXYDdy));
    mip = TerrainRvtSelectClipForFootprintWorld(info, terrain, terrainClipCount, sqrt(max(footprintWorldSq, 1.0e-8f)));

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
        terrain,
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

bool TerrainRvtLookupResidentPage(
    uint terrainSetIndex,
    uint clipLevel,
    uint2 pageCoord,
    uint pageTableIndex,
    uint requiredContentMask,
    out uint physicalPageIndex);

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

    uint entry = 0u;
    TerrainRvtPageTag tag = (TerrainRvtPageTag)0;
#if TERRAIN_RVT_ENABLE_WAVE_PAGE_LOOKUP
    const uint4 pageMask = WaveMatch(pageTableIndex);
    const uint leaderLane = WaveFirstLaneFromMask(pageMask);
    if (WaveGetLaneIndex() == leaderLane)
    {
        entry = pageTable[pageTableIndex];
        tag = pageTags[pageTableIndex];
    }
    entry = WaveReadLaneAt(entry, leaderLane);
    tag.terrainSetIndex = WaveReadLaneAt(tag.terrainSetIndex, leaderLane);
    tag.clipLevel = WaveReadLaneAt(tag.clipLevel, leaderLane);
    tag.pageX = WaveReadLaneAt(tag.pageX, leaderLane);
    tag.pageY = WaveReadLaneAt(tag.pageY, leaderLane);
#else
    entry = pageTable[pageTableIndex];
    tag = pageTags[pageTableIndex];
#endif

    if (!TerrainRvtPageTableHasContent(entry, requiredContentMask))
    {
        return false;
    }
    if (!TerrainRvtPageTagMatches(tag, terrainSetIndex, clipLevel, pageCoord))
    {
        return false;
    }
    physicalPageIndex = TerrainRvtPageTablePhysicalPage(entry);
    return true;
}

void TerrainRvtMarkVisited(uint pageTableIndex)
{
}

struct TerrainRvtPhysicalPageAtlasContext
{
    float2 atlasBaseUv;
    float2 pageUvScale;
    float poolIndex;
};

void TerrainRvtPhysicalPageAtlasContextForPage(
    TerrainRvtInfo info,
    uint physicalPageIndex,
    out TerrainRvtPhysicalPageAtlasContext atlasContext)
{
    StructuredBuffer<TerrainRvtPhysicalPageAtlasInfo> physicalPageAtlas =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageAtlas)];
    const TerrainRvtPhysicalPageAtlasInfo atlasInfo = physicalPageAtlas[min(physicalPageIndex, info.maxPhysicalPages - 1u)];
    atlasContext.atlasBaseUv = atlasInfo.atlasBaseUv;
    atlasContext.pageUvScale = atlasInfo.pageUvScale;
    atlasContext.poolIndex = atlasInfo.poolIndex;
}

void TerrainRvtPhysicalPageAtlasUv(
    TerrainRvtPhysicalPageAtlasContext atlasContext,
    float2 pageUv,
    out float3 atlasUv)
{
    atlasUv = float3(atlasContext.atlasBaseUv + pageUv * atlasContext.pageUvScale, atlasContext.poolIndex);
}

void TerrainRvtPhysicalPageUv(
    TerrainRvtInfo info,
    uint physicalPageIndex,
    float2 pageUv,
    out float3 atlasUv)
{
    TerrainRvtPhysicalPageAtlasContext atlasContext;
    TerrainRvtPhysicalPageAtlasContextForPage(info, physicalPageIndex, atlasContext);
    TerrainRvtPhysicalPageAtlasUv(atlasContext, pageUv, atlasUv);
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

bool TerrainRvtTryFindCoarserResidentFromLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    uint terrainClipCount,
    uint requestedMip,
    float2 local,
    uint requiredContentMask,
    out TerrainRvtAddress residentAddress,
    out uint physicalPageIndex);

bool TerrainRvtTryFindCoarserResidentFromPageLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    uint terrainClipCount,
    uint requestedMip,
    uint2 requestedPageCoord,
    float2 requestedPageUv,
    uint requiredContentMask,
    out TerrainRvtAddress residentAddress,
    out uint physicalPageIndex)
{
    residentAddress = (TerrainRvtAddress)0;
    residentAddress.pageTableIndex = 0xffffffffu;
    physicalPageIndex = 0u;

    StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
    const TerrainRvtClipInfo requestedClipInfo = clipInfos[TerrainRvtClipInfoIndex(info, terrainSetIndex, requestedMip)];
    if (requestedClipInfo.valid == 0u || requestedClipInfo.pageWorldSize <= 0.0f)
    {
        return false;
    }

    const float2 local = ((float2)requestedPageCoord + requestedPageUv) * requestedClipInfo.pageWorldSize;
    return TerrainRvtTryFindCoarserResidentFromLocal(
        terrainSetIndex,
        info,
        terrainClipCount,
        requestedMip,
        local,
        requiredContentMask,
        residentAddress,
        physicalPageIndex);
}

bool TerrainRvtTryFindCoarserResidentFromLocal(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    uint terrainClipCount,
    uint requestedMip,
    float2 local,
    uint requiredContentMask,
    out TerrainRvtAddress residentAddress,
    out uint physicalPageIndex)
{
    residentAddress = (TerrainRvtAddress)0;
    residentAddress.pageTableIndex = 0xffffffffu;
    physicalPageIndex = 0u;

    StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
    [loop]
    for (uint sampleMip = requestedMip + 1u; sampleMip < terrainClipCount; ++sampleMip)
    {
        uint2 pageCoord;
        float2 pageUv;
        uint pageTableIndex;
        const TerrainRvtClipInfo clipInfo = clipInfos[TerrainRvtClipInfoIndex(info, terrainSetIndex, sampleMip)];
        if (!TerrainRvtTryComputePageInClipInfoLocal(
            info,
            clipInfo,
            local,
            pageCoord,
            pageUv,
            pageTableIndex))
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

void TerrainRvtRecordHeightComputedPageTelemetry(
    uint mip,
    uint requestedPageTableIndex,
    bool telemetryEnabled)
{
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleMipHistogram[TerrainRvtTelemetryMipBin(mip)], 1u);
        InterlockedXor(stats[0].heightSampleAttemptedPageXor, requestedPageTableIndex);
        InterlockedMin(stats[0].heightSampleAttemptedPageMin, requestedPageTableIndex);
        InterlockedMax(stats[0].heightSampleAttemptedPageMax, requestedPageTableIndex);
    }
}

void TerrainRvtRecordHeightPageMiss(
    uint requestedPageTableIndex,
    bool telemetryEnabled)
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
}

bool TerrainRvtTryResolveHeightResident(
    TerrainRvtSampleContext ctx,
    uint mip,
    uint2 requestedPageCoord,
    float2 requestedPageUv,
    uint requestedPageTableIndex,
    bool skipDirectLookup,
    bool hasLocal,
    float2 local,
    bool telemetryEnabled,
    out TerrainRvtAddress residentAddress,
    out uint physicalPageIndex)
{
    TerrainRvtRecordHeightComputedPageTelemetry(mip, requestedPageTableIndex, telemetryEnabled);

    residentAddress = (TerrainRvtAddress)0;
    residentAddress.terrainSetIndex = ctx.terrainSetIndex;
    residentAddress.clipLevel = mip;
    residentAddress.pageCoord = requestedPageCoord;
    residentAddress.pageUv = requestedPageUv;
    residentAddress.pageTableIndex = requestedPageTableIndex;
    physicalPageIndex = 0u;

    bool residentHit = false;
    bool cacheMatchedRequest = false;
    if (requestedPageTableIndex < ctx.info.maxVirtualPageTableEntries)
    {
        StructuredBuffer<TerrainRvtHeightResidentCacheEntry> heightResidentCache =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtHeightResidentCache)];
        const TerrainRvtHeightResidentCacheEntry cachedResident = heightResidentCache[requestedPageTableIndex];
        const bool cacheMatchesRequest =
            cachedResident.requestedTerrainSetIndex == ctx.terrainSetIndex &&
            cachedResident.requestedClipLevel == mip &&
            cachedResident.requestedPageX == requestedPageCoord.x &&
            cachedResident.requestedPageY == requestedPageCoord.y;
        if (cacheMatchesRequest)
        {
            cacheMatchedRequest = true;
            if (cachedResident.status == TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_MISSING)
            {
                TerrainRvtRecordHeightPageMiss(requestedPageTableIndex, telemetryEnabled);
                return false;
            }
            if (cachedResident.status == TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_DIRECT ||
                cachedResident.status == TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_COARSER)
            {
                residentAddress.terrainSetIndex = ctx.terrainSetIndex;
                residentAddress.clipLevel = cachedResident.residentClipLevel;
                residentAddress.pageCoord = uint2(cachedResident.residentPageX, cachedResident.residentPageY);
                residentAddress.pageTableIndex = cachedResident.residentPageTableIndex;
                physicalPageIndex = cachedResident.physicalPageIndex;

                if (cachedResident.status == TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_DIRECT)
                {
                    residentAddress.pageUv = requestedPageUv;
                    residentHit = true;
                }
                else
                {
                    StructuredBuffer<TerrainRvtClipInfo> clipInfos =
                        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
                    const TerrainRvtClipInfo requestedClipInfo =
                        clipInfos[TerrainRvtClipInfoIndex(ctx.info, ctx.terrainSetIndex, mip)];
                    const TerrainRvtClipInfo residentClipInfo =
                        clipInfos[TerrainRvtClipInfoIndex(ctx.info, ctx.terrainSetIndex, cachedResident.residentClipLevel)];
                    const float2 residentLocal = hasLocal
                        ? local
                        : ((float2)requestedPageCoord + requestedPageUv) * requestedClipInfo.pageWorldSize;
                    uint2 cachedPageCoord;
                    float2 cachedPageUv;
                    uint cachedPageTableIndex;
                    if (TerrainRvtTryComputePageInClipInfoLocal(
                            ctx.info,
                            residentClipInfo,
                            residentLocal,
                            cachedPageCoord,
                            cachedPageUv,
                            cachedPageTableIndex) &&
                        cachedPageTableIndex == cachedResident.residentPageTableIndex &&
                        all(cachedPageCoord == residentAddress.pageCoord))
                    {
                        residentAddress.pageUv = cachedPageUv;
                        residentHit = true;
                    }
                }
            }
        }
    }
    if (!residentHit && !cacheMatchedRequest && !skipDirectLookup)
    {
        residentHit = TerrainRvtLookupResidentPage(
            ctx.terrainSetIndex,
            mip,
            requestedPageCoord,
            requestedPageTableIndex,
            TERRAIN_RVT_CONTENT_HEIGHT,
            physicalPageIndex);
    }
#if TERRAIN_RVT_ENABLE_COARSER_RESIDENT_FALLBACK
    if (!residentHit && !cacheMatchedRequest)
    {
        residentHit = hasLocal
            ? TerrainRvtTryFindCoarserResidentFromLocal(
                ctx.terrainSetIndex,
                ctx.info,
                ctx.terrainClipCount,
                mip,
                local,
                TERRAIN_RVT_CONTENT_HEIGHT,
                residentAddress,
                physicalPageIndex)
            : TerrainRvtTryFindCoarserResidentFromPageLocal(
                ctx.terrainSetIndex,
                ctx.info,
                ctx.terrainClipCount,
                mip,
                requestedPageCoord,
                requestedPageUv,
                TERRAIN_RVT_CONTENT_HEIGHT,
                residentAddress,
                physicalPageIndex);
    }
#endif

    if (!residentHit)
    {
        TerrainRvtRecordHeightPageMiss(requestedPageTableIndex, telemetryEnabled);
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
    return true;
}

bool TerrainRvtTrySampleHeightResident(
    TerrainRvtSampleContext ctx,
    TerrainRvtAddress residentAddress,
    uint physicalPageIndex,
    bool telemetryEnabled,
    out float heightValue)
{
    heightValue = 0.0f;

    TerrainRvtMarkVisited(residentAddress.pageTableIndex);
    float3 atlasUv;
    TerrainRvtPhysicalPageUv(ctx.info, physicalPageIndex, residentAddress.pageUv, atlasUv);
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

bool TerrainRvtTrySampleHeightContextComputedPageLocal(
    TerrainRvtSampleContext ctx,
    uint mip,
    uint2 requestedPageCoord,
    float2 requestedPageUv,
    uint requestedPageTableIndex,
    float2 local,
    bool skipDirectLookup,
    bool telemetryEnabled,
    out float heightValue)
{
    TerrainRvtAddress residentAddress;
    uint physicalPageIndex;
    if (!TerrainRvtTryResolveHeightResident(
        ctx,
        mip,
        requestedPageCoord,
        requestedPageUv,
        requestedPageTableIndex,
        skipDirectLookup,
        true,
        local,
        telemetryEnabled,
        residentAddress,
        physicalPageIndex))
    {
        heightValue = 0.0f;
        return false;
    }

    return TerrainRvtTrySampleHeightResident(ctx, residentAddress, physicalPageIndex, telemetryEnabled, heightValue);
}

bool TerrainRvtTrySampleHeightContextComputedPage(
    TerrainRvtSampleContext ctx,
    uint mip,
    uint2 requestedPageCoord,
    float2 requestedPageUv,
    uint requestedPageTableIndex,
    bool telemetryEnabled,
    out float heightValue)
{
    TerrainRvtAddress residentAddress;
    uint physicalPageIndex;
    if (!TerrainRvtTryResolveHeightResident(
        ctx,
        mip,
        requestedPageCoord,
        requestedPageUv,
        requestedPageTableIndex,
        false,
        false,
        0.0f.xx,
        telemetryEnabled,
        residentAddress,
        physicalPageIndex))
    {
        heightValue = 0.0f;
        return false;
    }

    return TerrainRvtTrySampleHeightResident(ctx, residentAddress, physicalPageIndex, telemetryEnabled, heightValue);
}

void TerrainRvtRecordSharedHeightComputedPageTelemetry(
    uint mip,
    uint pageTableIndex,
    bool telemetryEnabled)
{
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleMipHistogram[TerrainRvtTelemetryMipBin(mip)], 3u);
        InterlockedXor(stats[0].heightSampleAttemptedPageXor, pageTableIndex);
        InterlockedMin(stats[0].heightSampleAttemptedPageMin, pageTableIndex);
        InterlockedMax(stats[0].heightSampleAttemptedPageMax, pageTableIndex);
    }
}

void TerrainRvtRecordHeightSampleAttempts3(bool telemetryEnabled)
{
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleAttempts, 3u);
        InterlockedAdd(stats[0].heightFastSampleAttempts, 3u);
    }
}

void TerrainRvtRecordHeightComputePageFailure(bool telemetryEnabled)
{
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightFallbacks, 1u);
        InterlockedAdd(stats[0].heightComputePageFailures, 1u);
    }
}

void TerrainRvtSampleHeightResident3(
    TerrainRvtInfo info,
    uint physicalPageIndex,
    float2 pageUv0,
    float2 pageUv1,
    float2 pageUv2,
    out float height0,
    out float height1,
    out float height2)
{
    TerrainRvtPhysicalPageAtlasContext atlasContext;
    TerrainRvtPhysicalPageAtlasContextForPage(info, physicalPageIndex, atlasContext);
    float3 atlasUv0;
    float3 atlasUv1;
    float3 atlasUv2;
    TerrainRvtPhysicalPageAtlasUv(atlasContext, pageUv0, atlasUv0);
    TerrainRvtPhysicalPageAtlasUv(atlasContext, pageUv1, atlasUv1);
    TerrainRvtPhysicalPageAtlasUv(atlasContext, pageUv2, atlasUv2);
    Texture2DArray<float> heightAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtHeightAtlas)];
    height0 = saturate(heightAtlas.SampleLevel(g_linearClamp, atlasUv0, 0.0f));
    height1 = saturate(heightAtlas.SampleLevel(g_linearClamp, atlasUv1, 0.0f));
    height2 = saturate(heightAtlas.SampleLevel(g_linearClamp, atlasUv2, 0.0f));
}

void TerrainRvtTrySampleHeightContextAtClipInfo3(
    TerrainRvtSampleContext ctx,
    TerrainRvtClipInfo clipInfo,
    float3 position0WS,
    float3 position1WS,
    float3 position2WS,
    bool telemetryEnabled,
    out bool hit0,
    out bool hit1,
    out bool hit2,
    out float height0,
    out float height1,
    out float height2)
{
    hit0 = false;
    hit1 = false;
    hit2 = false;
    height0 = 0.0f;
    height1 = 0.0f;
    height2 = 0.0f;

    TerrainRvtRecordHeightSampleAttempts3(telemetryEnabled);

    const float2 skyrimXY0 = TerrainRvtSkyrimXYFromRendererPosition(position0WS);
    const float2 skyrimXY1 = TerrainRvtSkyrimXYFromRendererPosition(position1WS);
    const float2 skyrimXY2 = TerrainRvtSkyrimXYFromRendererPosition(position2WS);
    const float2 local0 = skyrimXY0 - ctx.terrainOrigin;
    const float2 local1 = skyrimXY1 - ctx.terrainOrigin;
    const float2 local2 = skyrimXY2 - ctx.terrainOrigin;

    uint2 requestedPageCoord0;
    uint2 requestedPageCoord1;
    uint2 requestedPageCoord2;
    float2 requestedPageUv0;
    float2 requestedPageUv1;
    float2 requestedPageUv2;
    uint requestedPageTableIndex0;
    uint requestedPageTableIndex1;
    uint requestedPageTableIndex2;
    TerrainRvtComputePageInClipInfoLocalUnchecked(
        clipInfo,
        local0,
        requestedPageCoord0,
        requestedPageUv0,
        requestedPageTableIndex0);
    TerrainRvtComputePageInClipInfoLocalUnchecked(
        clipInfo,
        local1,
        requestedPageCoord1,
        requestedPageUv1,
        requestedPageTableIndex1);
    TerrainRvtComputePageInClipInfoLocalUnchecked(
        clipInfo,
        local2,
        requestedPageCoord2,
        requestedPageUv2,
        requestedPageTableIndex2);
    const bool computed0 = requestedPageTableIndex0 < ctx.info.maxVirtualPageTableEntries;
    const bool computed1 = requestedPageTableIndex1 < ctx.info.maxVirtualPageTableEntries;
    const bool computed2 = requestedPageTableIndex2 < ctx.info.maxVirtualPageTableEntries;

    if (!computed0)
    {
        TerrainRvtRecordHeightComputePageFailure(telemetryEnabled);
    }
    if (!computed1)
    {
        TerrainRvtRecordHeightComputePageFailure(telemetryEnabled);
    }
    if (!computed2)
    {
        TerrainRvtRecordHeightComputePageFailure(telemetryEnabled);
    }

    const uint mip = min(clipInfo.clipLevel, ctx.terrainClipCount - 1u);
    const bool sharedRequestedPage = computed0 && computed1 && computed2 &&
        requestedPageTableIndex0 == requestedPageTableIndex1 &&
        requestedPageTableIndex0 == requestedPageTableIndex2 &&
        all(requestedPageCoord0 == requestedPageCoord1) &&
        all(requestedPageCoord0 == requestedPageCoord2);

    if (sharedRequestedPage)
    {
        StructuredBuffer<TerrainRvtHeightResidentCacheEntry> heightResidentCache =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtHeightResidentCache)];
        const TerrainRvtHeightResidentCacheEntry cachedResident = heightResidentCache[requestedPageTableIndex0];
        if (cachedResident.status == TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_DIRECT)
        {
            TerrainRvtRecordSharedHeightComputedPageTelemetry(mip, requestedPageTableIndex0, telemetryEnabled);
            TerrainRvtMarkVisited(cachedResident.residentPageTableIndex);
            TerrainRvtSampleHeightResident3(
                ctx.info,
                cachedResident.physicalPageIndex,
                requestedPageUv0,
                requestedPageUv1,
                requestedPageUv2,
                height0,
                height1,
                height2);
            if (telemetryEnabled)
            {
                RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                InterlockedAdd(stats[0].heightSampleHits, 3u);
                InterlockedAdd(stats[0].heightFastSampleHits, 3u);
            }
            hit0 = true;
            hit1 = true;
            hit2 = true;
            return;
        }
        if (cachedResident.status == TERRAIN_RVT_HEIGHT_RESIDENT_CACHE_COARSER)
        {
            StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
            const TerrainRvtClipInfo residentClipInfo = clipInfos[TerrainRvtClipInfoIndex(ctx.info, ctx.terrainSetIndex, cachedResident.residentClipLevel)];
            uint2 residentPageCoord0;
            uint2 residentPageCoord1;
            uint2 residentPageCoord2;
            float2 residentPageUv0;
            float2 residentPageUv1;
            float2 residentPageUv2;
            uint residentPageTableIndex0;
            uint residentPageTableIndex1;
            uint residentPageTableIndex2;
            TerrainRvtComputePageInClipInfoLocalUnchecked(
                residentClipInfo,
                local0,
                residentPageCoord0,
                residentPageUv0,
                residentPageTableIndex0);
            TerrainRvtComputePageInClipInfoLocalUnchecked(
                residentClipInfo,
                local1,
                residentPageCoord1,
                residentPageUv1,
                residentPageTableIndex1);
            TerrainRvtComputePageInClipInfoLocalUnchecked(
                residentClipInfo,
                local2,
                residentPageCoord2,
                residentPageUv2,
                residentPageTableIndex2);

            TerrainRvtRecordSharedHeightComputedPageTelemetry(mip, requestedPageTableIndex0, telemetryEnabled);
            TerrainRvtMarkVisited(cachedResident.residentPageTableIndex);
            TerrainRvtSampleHeightResident3(
                ctx.info,
                cachedResident.physicalPageIndex,
                residentPageUv0,
                residentPageUv1,
                residentPageUv2,
                height0,
                height1,
                height2);
            if (telemetryEnabled)
            {
                RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                InterlockedAdd(stats[0].heightSampleHits, 3u);
                InterlockedAdd(stats[0].heightFastSampleHits, 3u);
            }
            hit0 = true;
            hit1 = true;
            hit2 = true;
            return;
        }
    }

    if (computed0 && !hit0)
    {
        hit0 = TerrainRvtTrySampleHeightContextComputedPageLocal(
            ctx,
            mip,
            requestedPageCoord0,
            requestedPageUv0,
            requestedPageTableIndex0,
            local0,
            false,
            telemetryEnabled,
            height0);
    }
    if (computed1 && !hit1)
    {
        hit1 = TerrainRvtTrySampleHeightContextComputedPageLocal(
            ctx,
            mip,
            requestedPageCoord1,
            requestedPageUv1,
            requestedPageTableIndex1,
            local1,
            false,
            telemetryEnabled,
            height1);
    }
    if (computed2 && !hit2)
    {
        hit2 = TerrainRvtTrySampleHeightContextComputedPageLocal(
            ctx,
            mip,
            requestedPageCoord2,
            requestedPageUv2,
            requestedPageTableIndex2,
            local2,
            false,
            telemetryEnabled,
            height2);
    }
}

bool TerrainRvtTrySampleHeightContext(
    TerrainRvtSampleContext ctx,
    float3 positionWS,
    float2 skyrimXYDdx,
    float2 skyrimXYDdy,
    bool telemetryEnabled,
    out float heightValue)
{
    heightValue = 0.0f;
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleAttempts, 1u);
        InterlockedAdd(stats[0].heightFastSampleAttempts, 1u);
    }

    uint mip;
    uint2 requestedPageCoord;
    float2 requestedPageUv;
    uint requestedPageTableIndex;

    const float2 skyrimXY = TerrainRvtSkyrimXYFromRendererPosition(positionWS);
    float2 local;
    if (!TerrainRvtTryPrepareSampleContextLocal(ctx, skyrimXY, local) ||
        !TerrainRvtTryComputePageLocal(
            ctx.terrainSetIndex,
            ctx.info,
            ctx.terrain,
            local,
            skyrimXYDdx,
            skyrimXYDdy,
            ctx.terrainClipCount,
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

    return TerrainRvtTrySampleHeightContextComputedPage(
        ctx,
        mip,
        requestedPageCoord,
        requestedPageUv,
        requestedPageTableIndex,
        telemetryEnabled,
        heightValue);
}

bool TerrainRvtTrySampleHeightScaleFast(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    out float heightScale)
{
    heightScale = 0.0f;
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    TerrainRvtInfo info = infoBuffer[0];
    TerrainSetInfo terrain = terrainSets[terrainSetIndex];

    const float2 skyrimXY = TerrainRvtSkyrimXYFromRendererPosition(positionWS);
    float2 local;
    uint terrainClipCount;
    uint mip;
    uint2 requestedPageCoord;
    float2 requestedPageUv;
    uint requestedPageTableIndex;
    if (!TerrainRvtTryPrepareSampleLocal(terrainSetIndex, info, terrain, skyrimXY, local, terrainClipCount) ||
        !TerrainRvtTryComputePageLocal(
            terrainSetIndex,
            info,
            terrain,
            local,
            TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdxWS),
            TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdyWS),
            terrainClipCount,
            mip,
            requestedPageCoord,
            requestedPageUv,
            requestedPageTableIndex))
    {
        return false;
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
        && !TerrainRvtTryFindCoarserResidentFromPageLocal(
            terrainSetIndex,
            info,
            terrainClipCount,
            mip,
            requestedPageCoord,
            requestedPageUv,
            TERRAIN_RVT_CONTENT_MATERIAL,
            residentAddress,
            physicalPageIndex)
#endif
        )
    {
        return false;
    }

    TerrainRvtMarkVisited(residentAddress.pageTableIndex);
    float3 atlasUv;
    TerrainRvtPhysicalPageUv(info, physicalPageIndex, residentAddress.pageUv, atlasUv);
    Texture2DArray<float4> materialAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtMaterialAtlas)];
    heightScale = materialAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f).a;
    return heightScale > 1.0e-5f;
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
    float heightScale;
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
            terrain,
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
        && !TerrainRvtTryFindCoarserResidentFromPageLocal(
            terrainSetIndex,
            info,
            terrainClipCount,
            mip,
            requestedPageCoord,
            requestedPageUv,
            TERRAIN_RVT_CONTENT_MATERIAL,
            residentAddress,
            physicalPageIndex)
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
    sampleOut.heightScale = materialParams.a;
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

bool TerrainRvtTrySampleNormalBiased(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    float3 normalWSBase,
    uint mipBias,
    out float3 normalTS,
    out float3 normalWS)
{
    normalTS = float3(0.0f, 0.0f, 1.0f);
    normalWS = normalize(normalWSBase);

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (perFrame.terrainRvtEnabled == 0u || perFrame.terrainRvtForceDirectFallback != 0u)
    {
        return false;
    }

    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    const TerrainRvtInfo info = infoBuffer[0];
    const TerrainSetInfo terrain = terrainSets[terrainSetIndex];

    const float2 skyrimXY = TerrainRvtSkyrimXYFromRendererPosition(positionWS);
    float2 local;
    uint terrainClipCount;
    if (!TerrainRvtTryPrepareSampleLocal(terrainSetIndex, info, terrain, skyrimXY, local, terrainClipCount))
    {
        return false;
    }

    uint baseMip;
    uint2 basePageCoord;
    float2 basePageUv;
    uint basePageTableIndex;
    if (!TerrainRvtTryComputePageLocal(
        terrainSetIndex,
        info,
        terrain,
        local,
        TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdxWS),
        TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdyWS),
        terrainClipCount,
        baseMip,
        basePageCoord,
        basePageUv,
        basePageTableIndex))
    {
        return false;
    }

    const uint biasedMip = min(baseMip + mipBias, terrainClipCount - 1u);
    uint2 requestedPageCoord;
    float2 requestedPageUv;
    uint requestedPageTableIndex;
    if (!TerrainRvtTryComputePageAtClipLocal(
        terrainSetIndex,
        info,
        local,
        biasedMip,
        terrainClipCount,
        requestedPageCoord,
        requestedPageUv,
        requestedPageTableIndex))
    {
        return false;
    }

    TerrainRvtMarkPageTableIndex(requestedPageTableIndex, TERRAIN_RVT_CONTENT_MATERIAL);

    TerrainRvtAddress residentAddress = (TerrainRvtAddress)0;
    residentAddress.terrainSetIndex = terrainSetIndex;
    residentAddress.clipLevel = biasedMip;
    residentAddress.pageCoord = requestedPageCoord;
    residentAddress.pageUv = requestedPageUv;
    residentAddress.pageTableIndex = requestedPageTableIndex;

    uint physicalPageIndex = 0u;
    if (!TerrainRvtLookupResidentPage(terrainSetIndex, biasedMip, requestedPageCoord, requestedPageTableIndex, TERRAIN_RVT_CONTENT_MATERIAL, physicalPageIndex)
#if TERRAIN_RVT_ENABLE_COARSER_RESIDENT_FALLBACK
        && !TerrainRvtTryFindCoarserResidentFromPageLocal(
            terrainSetIndex,
            info,
            terrainClipCount,
            biasedMip,
            requestedPageCoord,
            requestedPageUv,
            TERRAIN_RVT_CONTENT_MATERIAL,
            residentAddress,
            physicalPageIndex)
#endif
        )
    {
        return false;
    }

    TerrainRvtMarkVisited(residentAddress.pageTableIndex);
    float3 atlasUv;
    TerrainRvtPhysicalPageUv(info, physicalPageIndex, residentAddress.pageUv, atlasUv);
    Texture2DArray<float4> normalAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtNormalAtlas)];
    normalTS = normalize(normalAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f).xyz * 2.0f - 1.0f);
    normalWS = TerrainRvtTangentToWorldNormal(normalTS, normalWSBase);
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

uint TerrainRvtMipForWorldRect(TerrainRvtInfo info, TerrainSetInfo terrain, float2 minSkyrimXY, float2 maxSkyrimXY, float targetPagesPerAxis)
{
    const float extent = max(maxSkyrimXY.x - minSkyrimXY.x, maxSkyrimXY.y - minSkyrimXY.y);
    const uint terrainClipCount = TerrainRvtTerrainClipCount(info, terrain);
    return TerrainRvtSelectClipForFootprintWorld(
        info,
        terrain,
        terrainClipCount,
        extent / max(targetPagesPerAxis * (float)info.pageSize, 1.0f));
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
    StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
    const TerrainRvtClipInfo clipInfo = clipInfos[TerrainRvtClipInfoIndex(info, terrainSetIndex, mip)];
    if (clipInfo.valid == 0u)
    {
        return;
    }

    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float pageWorldSize = clipInfo.pageWorldSize;
    int2 minPage = (int2)floor((minSkyrimXY - terrainOrigin) / pageWorldSize);
    int2 maxPage = (int2)floor((maxSkyrimXY - terrainOrigin) / pageWorldSize);
    const uint2 terrainPageCount = clipInfo.terrainPageCount;
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
