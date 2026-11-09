// Terrain RVT block compression (TerrainRvt::CompressionEnabled). TerrainRvtGeneratePagesCS writes each generated page's
// tile, uncompressed, to its generation's slice of the scratch textures; this pass encodes the tile's 4x4 blocks and
// stores them straight into the atlases' pages, through a UAV of one R32G32B32A32_UINT texel per block (relaxed format
// casting). Albedo and material are BC7 (mode 6), the normal's xy BC5; the height atlas stays R16F and is written by
// generation. One thread per block per atlas, atlas-major so a wave encodes one format.
#define TERRAIN_RVT_GENERATION 1
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/materialFlags.hlsli"
#include "include/visUtilCommon.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/clodResolveCommon.hlsli"
#include "include/terrainCommon.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"
#include "include/bc7Mode6.hlsli"
#include "include/bc5Encode.hlsli"

static const uint TERRAIN_RVT_ENCODE_MAX_DISPATCH_GROUPS_X = 65535u;
static const uint TERRAIN_RVT_ENCODE_ALBEDO = 0u;
static const uint TERRAIN_RVT_ENCODE_NORMAL = 1u;
static const uint TERRAIN_RVT_ENCODE_MATERIAL = 2u;

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtEncodePagesCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    StructuredBuffer<TerrainRvtGenerationRequest> generationList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerationList)];
    StructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];

    const TerrainRvtInfo info = infoBuffer[0];
    const uint generationCount = min(
        min(counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT], info.maxGenerationEntries),
        info.maxGeneratedPagesPerFrame);
    const uint blocksPerSide = info.physicalTileTexelSide / 4u;
    const uint blocksPerPage = blocksPerSide * blocksPerSide;
    const uint blocksPerAtlas = generationCount * blocksPerPage;
    const uint linearIndex = tid.x + tid.y * TERRAIN_RVT_ENCODE_MAX_DISPATCH_GROUPS_X * 64u;
    if (blocksPerPage == 0u || linearIndex >= blocksPerAtlas * 3u)
    {
        return;
    }
    const uint atlasKind = linearIndex / blocksPerAtlas;
    const uint inAtlas = linearIndex - atlasKind * blocksPerAtlas;
    const uint generationIndex = inAtlas / blocksPerPage;
    const uint blockInPage = inAtlas - generationIndex * blocksPerPage;
    const uint2 block = uint2(blockInPage % blocksPerSide, blockInPage / blocksPerSide);

    const TerrainRvtGenerationRequest generation = generationList[generationIndex];
    if ((generation.contentMask & TERRAIN_RVT_CONTENT_MATERIAL) == 0u)
    {
        return;
    }
    uint2 tileOrigin;
    uint atlasPoolIndex;
    if (!TerrainRvtGenerationTile(info, generation, clipInfos, tileOrigin, atlasPoolIndex))
    {
        return;
    }
    const uint3 atlasBlock = uint3(tileOrigin / 4u + block, atlasPoolIndex);
    const uint2 texelBase = block * 4u;

    if (atlasKind == TERRAIN_RVT_ENCODE_NORMAL)
    {
        StructuredBuffer<uint> scratch = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtScratchNormal)];
        float red[16];
        float green[16];
        [unroll]
        for (uint texel = 0u; texel < 16u; ++texel)
        {
            const float2 value = TerrainRvtUnpackUnorm2x16Bytes(
                scratch[TerrainRvtScratchIndex(info, generationIndex, texelBase + uint2(texel & 3u, texel >> 2u))]);
            red[texel] = value.x;
            green[texel] = value.y;
        }
        RWTexture2DArray<uint4> atlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtNormalAtlas)];
        atlas[atlasBlock] = Bc5EncodeBlock(red, green);
        return;
    }

    const bool albedo = atlasKind == TERRAIN_RVT_ENCODE_ALBEDO;
    const uint scratchIndex = albedo
        ? ResourceDescriptorIndex(Builtin::Terrain::RvtScratchAlbedo)
        : ResourceDescriptorIndex(Builtin::Terrain::RvtScratchMaterial);
    StructuredBuffer<uint> scratch = ResourceDescriptorHeap[scratchIndex];
    float4 samples[16];
    [unroll]
    for (uint texel = 0u; texel < 16u; ++texel)
    {
        samples[texel] = TerrainRvtUnpackUnorm4x8Bytes(
            scratch[TerrainRvtScratchIndex(info, generationIndex, texelBase + uint2(texel & 3u, texel >> 2u))]);
        // Albedo's alpha is unused: a constant one leaves the fit to the color.
        if (albedo) samples[texel].a = 255.0f;
    }
    const BC7Mode6Block encoded = EncodeMode6Samples(samples);
    const uint atlasIndex = albedo
        ? ResourceDescriptorIndex(Builtin::Terrain::RvtAlbedoAtlas)
        : ResourceDescriptorIndex(Builtin::Terrain::RvtMaterialAtlas);
    RWTexture2DArray<uint4> atlas = ResourceDescriptorHeap[atlasIndex];
    atlas[atlasBlock] = encoded.words;
}
