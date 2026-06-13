#ifndef __TERRAIN_RVT_COMMON_HLSLI__
#define __TERRAIN_RVT_COMMON_HLSLI__

#include "structs.hlsli"

static const uint TERRAIN_RVT_COUNTER_REQUEST_COUNT = 0u;
static const uint TERRAIN_RVT_COUNTER_GENERATION_COUNT = 1u;
static const uint TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT = 2u;
static const uint TERRAIN_RVT_COUNTER_OVERFLOW_COUNT = 3u;
static const uint TERRAIN_RVT_COUNTER_COUNT = 4u;
static const uint TERRAIN_RVT_TELEMETRY_MIP_BINS = 16u;
static const uint TERRAIN_RVT_FALLBACK_NONE = 0u;
static const uint TERRAIN_RVT_FALLBACK_DISABLED = 1u;
static const uint TERRAIN_RVT_FALLBACK_FORCED = 2u;
static const uint TERRAIN_RVT_FALLBACK_COMPUTE_PAGE = 3u;
static const uint TERRAIN_RVT_FALLBACK_PAGE_MISS = 4u;
static const uint TERRAIN_RVT_FALLBACK_OWNER_MISMATCH = 5u;

uint TerrainRvtDebugHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

uint TerrainRvtDebugPageStampByte(uint pageTableIndex)
{
    return TerrainRvtDebugHash(pageTableIndex) & 0xffu;
}

float TerrainRvtDebugPageStampValue(uint pageTableIndex)
{
    return (float)TerrainRvtDebugPageStampByte(pageTableIndex) / 255.0f;
}

uint TerrainRvtDebugDecodePageStampByte(float stamp)
{
    return min(255u, (uint)floor(saturate(stamp) * 255.0f + 0.5f));
}

float2 TerrainRvtSkyrimXYFromRendererPosition(float3 positionWS)
{
    return float2(positionWS.x, -positionWS.z);
}

float2 TerrainRvtSkyrimXYDerivativeFromRendererDerivative(float3 positionDerivativeWS)
{
    return float2(positionDerivativeWS.x, -positionDerivativeWS.z);
}

uint TerrainRvtMipAxis(TerrainRvtInfo info, uint mip)
{
    return max(1u, info.maxVirtualPagesPerAxis >> min(mip, 31u));
}

uint TerrainRvtPageTableEntriesPerTerrainSet(TerrainRvtInfo info)
{
    uint total = 0u;
    [loop]
    for (uint mip = 0u; mip < info.mipCount; ++mip)
    {
        const uint axis = TerrainRvtMipAxis(info, mip);
        total += axis * axis;
    }
    return total;
}

float TerrainRvtBasePageWorldSize(TerrainRvtInfo info)
{
    return max(info.basePageWorldSize, 1.0f);
}

float TerrainRvtPageWorldSize(TerrainRvtInfo info, uint mip)
{
    return TerrainRvtBasePageWorldSize(info) * (float)(1u << min(mip, 31u));
}

uint2 TerrainRvtBasePageCount(TerrainRvtInfo info, TerrainSetInfo terrain)
{
    const float basePageWorldSize = TerrainRvtBasePageWorldSize(info);
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    return max(uint2(1u, 1u), (uint2)ceil(terrainSize / basePageWorldSize));
}

uint2 TerrainRvtMipPageCount(TerrainRvtInfo info, TerrainSetInfo terrain, uint mip)
{
    const uint mipScale = 1u << min(mip, 31u);
    return max(uint2(1u, 1u), (TerrainRvtBasePageCount(info, terrain) + mipScale - 1u) >> min(mip, 31u));
}

uint TerrainRvtPageTableMipOffset(TerrainRvtInfo info, uint mip)
{
    uint offset = 0u;
    [loop]
    for (uint i = 0u; i < mip; ++i)
    {
        const uint axis = TerrainRvtMipAxis(info, i);
        offset += axis * axis;
    }
    return offset;
}

uint TerrainRvtMipFromPageTableIndex(TerrainRvtInfo info, uint pageTableIndex)
{
    const uint entriesPerTerrainSet = max(TerrainRvtPageTableEntriesPerTerrainSet(info), 1u);
    uint remaining = pageTableIndex % entriesPerTerrainSet;
    [loop]
    for (uint i = 0u; i < info.mipCount; ++i)
    {
        const uint axis = TerrainRvtMipAxis(info, i);
        const uint mipEntries = axis * axis;
        if (remaining < mipEntries)
        {
            return i;
        }
        remaining -= mipEntries;
    }
    return 0u;
}

uint TerrainRvtTelemetryMipBin(uint mip)
{
    return min(mip, TERRAIN_RVT_TELEMETRY_MIP_BINS - 1u);
}

uint TerrainRvtPackPageTableEntry(uint physicalPageIndex, uint contentMask)
{
    return TERRAIN_RVT_PAGE_VALID |
        ((contentMask & 0x3u) << TERRAIN_RVT_PAGE_CONTENT_SHIFT) |
        (physicalPageIndex & TERRAIN_RVT_PAGE_PHYSICAL_MASK);
}

bool TerrainRvtUnpackPageTableEntry(uint entry, uint requiredContentMask, out uint physicalPageIndex)
{
    physicalPageIndex = entry & TERRAIN_RVT_PAGE_PHYSICAL_MASK;
    const uint contentMask = (entry & TERRAIN_RVT_PAGE_CONTENT_MASK) >> TERRAIN_RVT_PAGE_CONTENT_SHIFT;
    return (entry & TERRAIN_RVT_PAGE_VALID) != 0u &&
        (contentMask & requiredContentMask) == requiredContentMask;
}

bool TerrainRvtTryComputePageAtMip(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    TerrainSetInfo terrain,
    float2 skyrimXY,
    uint mip,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex);

void TerrainRvtMarkPageTableIndex(uint pageTableIndex, uint contentMask);

bool TerrainRvtTryComputePage(
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
    if (terrain.regionSizeWorld <= 0.0f ||
        terrain.regionCountX == 0u ||
        terrain.regionCountY == 0u ||
        info.pageSize == 0u ||
        info.maxVirtualPagesPerAxis == 0u ||
        info.mipCount == 0u)
    {
        return false;
    }

    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    const float2 local = skyrimXY - terrainOrigin;
    if (any(local < 0.0f.xx) || any(local >= terrainSize))
    {
        return false;
    }

    const float texelWorldSize0 = max(TerrainRvtBasePageWorldSize(info) / max((float)info.pageSize, 1.0f), 1.0e-4f);
    const float footprintWorld = max(length(skyrimXYDdx), length(skyrimXYDdy));
    const float requestedMip = 0.5f + log2(max(footprintWorld / texelWorldSize0, 1.0e-4f));
    mip = min((uint)max(0.0f, floor(requestedMip)), info.mipCount - 1u);

    return TerrainRvtTryComputePageAtMip(0u, info, terrain, skyrimXY, mip, pageCoord, pageUv, pageTableIndex);
}

bool TerrainRvtTryComputePageAtMip(
    uint terrainSetIndex,
    TerrainRvtInfo info,
    TerrainSetInfo terrain,
    float2 skyrimXY,
    uint mip,
    out uint2 pageCoord,
    out float2 pageUv,
    out uint pageTableIndex)
{
    pageCoord = 0u.xx;
    pageUv = 0.0f.xx;
    pageTableIndex = 0xffffffffu;
    if (terrain.regionSizeWorld <= 0.0f ||
        terrain.regionCountX == 0u ||
        terrain.regionCountY == 0u ||
        info.pageSize == 0u ||
        info.maxVirtualPagesPerAxis == 0u ||
        info.mipCount == 0u)
    {
        return false;
    }

    mip = min(mip, info.mipCount - 1u);
    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    const float2 local = skyrimXY - terrainOrigin;
    if (any(local < 0.0f.xx) || any(local >= terrainSize))
    {
        return false;
    }

    const float pageWorldSize = TerrainRvtPageWorldSize(info, mip);
    const float2 pageFloat = local / pageWorldSize;
    pageCoord = (uint2)floor(pageFloat);
    const uint axis = TerrainRvtMipAxis(info, mip);
    const uint2 terrainMipPageCount = TerrainRvtMipPageCount(info, terrain, mip);
    if (pageCoord.x >= axis || pageCoord.y >= axis ||
        pageCoord.x >= terrainMipPageCount.x || pageCoord.y >= terrainMipPageCount.y)
    {
        return false;
    }

    pageUv = frac(pageFloat);
    const uint entriesPerTerrainSet = TerrainRvtPageTableEntriesPerTerrainSet(info);
    pageTableIndex = terrainSetIndex * entriesPerTerrainSet +
        TerrainRvtPageTableMipOffset(info, mip) +
        pageCoord.y * axis +
        pageCoord.x;
    return pageTableIndex < info.maxVirtualPageTableEntries;
}

uint TerrainRvtMipForWorldRect(TerrainRvtInfo info, float2 minSkyrimXY, float2 maxSkyrimXY, float targetPagesPerAxis)
{
    const float extent = max(maxSkyrimXY.x - minSkyrimXY.x, maxSkyrimXY.y - minSkyrimXY.y);
    const float targetPageWorldSize = TerrainRvtBasePageWorldSize(info) * max(targetPagesPerAxis, 1.0f);
    const float requestedMip = log2(max(extent / max(targetPageWorldSize, 1.0e-4f), 1.0f));
    return min((uint)max(0.0f, floor(requestedMip)), max(info.mipCount, 1u) - 1u);
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
    if (!TerrainRvtTryComputePage(
        info,
        terrain,
        TerrainRvtSkyrimXYFromRendererPosition(positionWS),
        TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdxWS),
        TerrainRvtSkyrimXYDerivativeFromRendererDerivative(dpdyWS),
        mip,
        pageCoord,
        pageUv,
        pageTableIndex))
    {
        return false;
    }

    const uint entriesPerTerrainSet = TerrainRvtPageTableEntriesPerTerrainSet(info);
    const uint localPageTableIndex = pageTableIndex;
    pageTableIndex = terrainSetIndex * entriesPerTerrainSet + localPageTableIndex;
    return pageTableIndex < info.maxVirtualPageTableEntries;
}

void TerrainRvtPhysicalPageUv(
    TerrainRvtInfo info,
    uint physicalPageIndex,
    float2 pageUv,
    out float3 atlasUv)
{
    const uint tileSide = max(info.physicalTileTexelSide, 1u);
    const uint pagesWide = max(info.physicalAtlasPagesWide, 1u);
    const uint pagesPerPool = max(info.physicalAtlasPagesWide * info.physicalAtlasPagesHigh, 1u);
    const uint poolIndex = min(physicalPageIndex / pagesPerPool, max(info.physicalAtlasPoolCount, 1u) - 1u);
    const uint localPhysicalPageIndex = physicalPageIndex - poolIndex * pagesPerPool;
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

bool TerrainRvtTrySampleHeight(
    uint terrainSetIndex,
    float3 positionWS,
    float3 dpdxWS,
    float3 dpdyWS,
    out float heightValue)
{
    heightValue = 0.0f;
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = perFrame.terrainRvtTelemetryEnabled != 0u;
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleAttempts, 1u);
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

    TerrainRvtInfo info;
    TerrainSetInfo terrain;
    uint mip;
    uint2 pageCoord;
    float2 pageUv;
    uint pageTableIndex;
    if (!TerrainRvtTryComputePageFromPosition(terrainSetIndex, positionWS, dpdxWS, dpdyWS, info, terrain, mip, pageCoord, pageUv, pageTableIndex))
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
    }

    StructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    uint physicalPageIndex = 0u;
    bool foundResidentPage = false;
    [loop]
    for (uint sampleMip = mip; sampleMip < info.mipCount; ++sampleMip)
    {
        if (!TerrainRvtTryComputePageAtMip(
                terrainSetIndex,
                info,
                terrain,
                TerrainRvtSkyrimXYFromRendererPosition(positionWS),
                sampleMip,
                pageCoord,
                pageUv,
                pageTableIndex))
        {
            continue;
        }
        if (TerrainRvtUnpackPageTableEntry(pageTable[pageTableIndex], TERRAIN_RVT_CONTENT_HEIGHT, physicalPageIndex))
        {
            foundResidentPage = true;
            break;
        }
    }
    if (!foundResidentPage)
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].heightFallbacks, 1u);
            InterlockedAdd(stats[0].heightPageTableMisses, 1u);
        }
        return false;
    }

    StructuredBuffer<uint> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    const uint ownerPageTableIndex = physicalPageOwner[physicalPageIndex];
    if (ownerPageTableIndex != pageTableIndex)
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].heightFallbacks, 1u);
            InterlockedAdd(stats[0].heightOwnerMismatches, 1u);
        }
        return false;
    }

    float3 atlasUv;
    TerrainRvtPhysicalPageUv(info, physicalPageIndex, pageUv, atlasUv);
    Texture2DArray<float> heightAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtHeightAtlas)];
    heightValue = saturate(heightAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f));
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightSampleHits, 1u);
    }
    return true;
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
    const bool telemetryEnabled = perFrame.terrainRvtTelemetryEnabled != 0u;
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
    uint2 pageCoord;
    float2 pageUv;
    uint pageTableIndex;
    if (!TerrainRvtTryComputePageFromPosition(terrainSetIndex, positionWS, dpdxWS, dpdyWS, info, terrain, mip, pageCoord, pageUv, pageTableIndex))
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
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].materialSampleMipHistogram[TerrainRvtTelemetryMipBin(mip)], 1u);
    }

    const uint requestedPageTableIndex = pageTableIndex;
    sampleOut.requestedMip = mip;
    sampleOut.residentMip = mip;
    sampleOut.requestedPageTableIndex = requestedPageTableIndex;
    sampleOut.residentPageTableIndex = pageTableIndex;
    sampleOut.pageCoord = pageCoord;
    sampleOut.pageUv = pageUv;
    TerrainRvtMarkPageTableIndex(requestedPageTableIndex, TERRAIN_RVT_CONTENT_MATERIAL | TERRAIN_RVT_CONTENT_HEIGHT);
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedXor(stats[0].materialSampleAttemptedPageXor, requestedPageTableIndex);
        InterlockedMin(stats[0].materialSampleAttemptedPageMin, requestedPageTableIndex);
        InterlockedMax(stats[0].materialSampleAttemptedPageMax, requestedPageTableIndex);
    }
    StructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    uint physicalPageIndex = 0u;
    uint residentMip = mip;
    uint residentPageTableIndex = pageTableIndex;
    uint2 residentPageCoord = pageCoord;
    float2 residentPageUv = pageUv;
    bool foundResidentPage = false;
    [loop]
    for (uint sampleMip = mip; sampleMip < info.mipCount; ++sampleMip)
    {
        if (!TerrainRvtTryComputePageAtMip(
                terrainSetIndex,
                info,
                terrain,
                TerrainRvtSkyrimXYFromRendererPosition(positionWS),
                sampleMip,
                pageCoord,
                pageUv,
                pageTableIndex))
        {
            continue;
        }
        if (TerrainRvtUnpackPageTableEntry(pageTable[pageTableIndex], TERRAIN_RVT_CONTENT_MATERIAL, physicalPageIndex))
        {
            residentMip = sampleMip;
            residentPageTableIndex = pageTableIndex;
            residentPageCoord = pageCoord;
            residentPageUv = pageUv;
            foundResidentPage = true;
            break;
        }
    }
    if (!foundResidentPage)
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

    StructuredBuffer<uint> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    const uint ownerPageTableIndex = physicalPageOwner[physicalPageIndex];
    sampleOut.ownerPageTableIndex = ownerPageTableIndex;
    sampleOut.physicalPageIndex = physicalPageIndex;
    if (ownerPageTableIndex != residentPageTableIndex)
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

    float3 atlasUv;
    TerrainRvtPhysicalPageUv(info, physicalPageIndex, residentPageUv, atlasUv);
    const uint pagesPerPool = max(info.physicalAtlasPagesWide * info.physicalAtlasPagesHigh, 1u);
    const uint atlasPoolIndex = min(physicalPageIndex / pagesPerPool, max(info.physicalAtlasPoolCount, 1u) - 1u);
    Texture2DArray<float4> albedoAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtAlbedoAtlas)];
    Texture2DArray<float4> normalAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtNormalAtlas)];
    Texture2DArray<float4> materialAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtMaterialAtlas)];

    sampleOut.albedo = albedoAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f).rgb;
    sampleOut.albedoPoint = albedoAtlas.SampleLevel(g_pointClamp, atlasUv, 0.0f).rgb;
    sampleOut.normalTS = normalize(normalAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f).xyz * 2.0f - 1.0f);
    sampleOut.normalWS = TerrainRvtTangentToWorldNormal(sampleOut.normalTS, normalWSBase);
    const float4 materialParams = materialAtlas.SampleLevel(g_linearClamp, atlasUv, 0.0f);
    sampleOut.roughness = materialParams.r;
    sampleOut.metallic = materialParams.g;
    sampleOut.ambientOcclusion = materialParams.b;
    sampleOut.pageStamp = materialParams.a;
    sampleOut.expectedPageStamp = TerrainRvtDebugPageStampValue(residentPageTableIndex);
    sampleOut.pageStampDelta = abs(sampleOut.pageStamp - sampleOut.expectedPageStamp);
    sampleOut.requestedMip = mip;
    sampleOut.residentMip = residentMip;
    sampleOut.requestedPageTableIndex = requestedPageTableIndex;
    sampleOut.residentPageTableIndex = residentPageTableIndex;
    sampleOut.physicalPageIndex = physicalPageIndex;
    sampleOut.atlasPoolIndex = atlasPoolIndex;
    sampleOut.ownerPageTableIndex = ownerPageTableIndex;
    sampleOut.fallbackReason = TERRAIN_RVT_FALLBACK_NONE;
    sampleOut.pageCoord = residentPageCoord;
    sampleOut.pageUv = residentPageUv;
    sampleOut.atlasUv = atlasUv;
    sampleOut.physicalTileUv = TerrainRvtPhysicalTileUv(info, residentPageUv);
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
        if (TerrainRvtDebugDecodePageStampByte(sampleOut.pageStamp) != TerrainRvtDebugPageStampByte(sampleOut.residentPageTableIndex))
        {
            InterlockedAdd(stats[0].materialSamplePageStampMismatches, 1u);
        }
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

    RWStructuredBuffer<uint> requestMasks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestMasks)];
    RWStructuredBuffer<uint> requestList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestList)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = perFrame.terrainRvtTelemetryEnabled != 0u;
    const uint mipBin = TerrainRvtTelemetryMipBin(TerrainRvtMipFromPageTableIndex(info, pageTableIndex));

    uint previousMask = 0u;
    InterlockedOr(requestMasks[pageTableIndex], contentMask, previousMask);
    if ((previousMask & contentMask) == contentMask)
    {
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].residentHits, 1u);
        }
        return;
    }

    if (telemetryEnabled && (contentMask & TERRAIN_RVT_CONTENT_HEIGHT) != 0u)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].heightRequests, 1u);
        InterlockedAdd(stats[0].heightRequestMipHistogram[mipBin], 1u);
    }
    if (telemetryEnabled && (contentMask & TERRAIN_RVT_CONTENT_MATERIAL) != 0u)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].materialRequests, 1u);
        InterlockedAdd(stats[0].materialRequestMipHistogram[mipBin], 1u);
    }
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedXor(stats[0].requestPageTableXor, pageTableIndex);
        InterlockedMin(stats[0].requestPageTableMin, pageTableIndex);
        InterlockedMax(stats[0].requestPageTableMax, pageTableIndex);
    }
    if (previousMask == 0u)
    {
        uint requestIndex = 0u;
        InterlockedAdd(counters[TERRAIN_RVT_COUNTER_REQUEST_COUNT], 1u, requestIndex);
        if (requestIndex < info.maxRequests)
        {
            requestList[requestIndex] = pageTableIndex;
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
        if (perFrame.terrainRvtTelemetryEnabled != 0u)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].markComputePageFailures, 1u);
        }
        return false;
    }
    TerrainRvtMarkPageTableIndex(pageTableIndex, contentMask);
    return true;
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
    const bool telemetryEnabled = perFrame.terrainRvtTelemetryEnabled != 0u;
    if (telemetryEnabled)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].markWorldRectCalls, 1u);
    }
    TerrainRvtInfo info = infoBuffer[0];
    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    if (terrain.regionSizeWorld <= 0.0f || info.maxVirtualPagesPerAxis == 0u)
    {
        return;
    }

    mip = min(mip, max(info.mipCount, 1u) - 1u);
    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float pageWorldSize = TerrainRvtPageWorldSize(info, mip);
    int2 minPage = (int2)floor((minSkyrimXY - terrainOrigin) / pageWorldSize);
    int2 maxPage = (int2)floor((maxSkyrimXY - terrainOrigin) / pageWorldSize);
    const uint axis = TerrainRvtMipAxis(info, mip);
    const uint2 terrainMipPageCount = TerrainRvtMipPageCount(info, terrain, mip);
    const int2 maxValidPage = int2(min(uint2(axis, axis), terrainMipPageCount) - 1u);
    minPage = clamp(minPage, int2(0, 0), maxValidPage);
    maxPage = clamp(maxPage, int2(0, 0), maxValidPage);

    const uint mipOffset = terrainSetIndex * TerrainRvtPageTableEntriesPerTerrainSet(info) + TerrainRvtPageTableMipOffset(info, mip);
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
            TerrainRvtMarkPageTableIndex(mipOffset + (uint)y * axis + (uint)x, contentMask);
        }
    }
}

#endif // __TERRAIN_RVT_COMMON_HLSLI__
