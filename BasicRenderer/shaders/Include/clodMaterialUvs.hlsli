#ifndef CLOD_MATERIAL_UVS_HLSLI
#define CLOD_MATERIAL_UVS_HLSLI

// Decodes each UV set a material samples, with derivatives from the pixel's barycentrics.
// Requires clodResolveMeshlet.hlsli.

#include "include/clodUvRemap.hlsli"

struct MeshletUvDecodeInfo
{
    CLodMeshletUvDescriptor uvDesc;
    uint uvBitstreamBase;
    bool isValid;
};

MeshletUvDecodeInfo LoadMeshletUvDecodeInfo(uint uvSetIndex, MeshletResolveData d)
{
    MeshletUvDecodeInfo uvInfo = (MeshletUvDecodeInfo)0;
    if (uvSetIndex >= d.uvSetCount)
    {
        return uvInfo;
    }

    uvInfo.uvDesc = LoadMeshletUvDescriptor(
        d.pagePoolSlabDescriptorIndex,
        d.pageByteOffset,
        d.uvDescriptorBase - d.pageByteOffset,
        d.uvSetCount,
        d.drawcallAndMeshlet.y,
        uvSetIndex);
    uvInfo.uvBitstreamBase = d.pageByteOffset + LoadPageUvBitstreamOffset(
        d.pagePoolSlabDescriptorIndex,
        d.pageByteOffset,
        d.uvBitstreamDirectoryBase - d.pageByteOffset,
        uvSetIndex);
    uvInfo.isValid = true;
    return uvInfo;
}

float2 DecodeCompressedUV(uint meshletLocalVertex, MeshletUvDecodeInfo uvInfo, MeshletResolveData d)
{
    if (!uvInfo.isValid)
    {
        return float2(0.0f, 0.0f);
    }

    const uint uvBitsU = CLodUvDescBitsU(uvInfo.uvDesc);
    const uint uvBitsV = CLodUvDescBitsV(uvInfo.uvDesc);

    uint bitsPerVertex = uvBitsU + uvBitsV;
    uint bitCursor = uvInfo.uvBitstreamBase * 8u + uvInfo.uvDesc.uvBitOffset + meshletLocalVertex * bitsPerVertex;

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    const uint2 encoded = ReadPackedBitPair32_BA(slab, bitCursor, uvBitsU, uvBitsV);
    const uint encodedU = encoded.x;
    const uint encodedV = encoded.y;

    return float2(
        uvInfo.uvDesc.uvMinU + float(encodedU) * uvInfo.uvDesc.uvScaleU,
        uvInfo.uvDesc.uvMinV + float(encodedV) * uvInfo.uvDesc.uvScaleV);
}

float2 DecodeCompressedUV(uint meshletLocalVertex, uint uvSetIndex, MeshletResolveData d)
{
    return DecodeCompressedUV(meshletLocalVertex, LoadMeshletUvDecodeInfo(uvSetIndex, d), d);
}

void AppendClodMaterialUvSample(
    inout MaterialUvCache cache,
    uint uvSetIndex,
    uint3 triIdx,
    MeshletUvDecodeInfo uvInfo,
    MeshletResolveData md,
    BarycentricDeriv bary)
{
    if (cache.count >= MATERIAL_MAX_UNIQUE_UV_SETS)
    {
        return;
    }

    float2 uv0 = DecodeCompressedUV(triIdx.x, uvInfo, md);
    float2 uv1 = DecodeCompressedUV(triIdx.y, uvInfo, md);
    float2 uv2 = DecodeCompressedUV(triIdx.z, uvInfo, md);

    float3 interpU = InterpolateWithDeriv(bary, uv0.x, uv1.x, uv2.x);
    float3 interpV = InterpolateWithDeriv(bary, uv0.y, uv1.y, uv2.y);

    MaterialUvSample sample = (MaterialUvSample)0;
    sample.uvSetIndex = uvSetIndex;
    sample.uv = float2(interpU.x, interpV.x);
    sample.dUVdx = float2(interpU.y, interpV.y);
    sample.dUVdy = float2(interpU.z, interpV.z);
#if CLOD_UV_REMAP
    // Stage 6 step 2: a remapped cluster's UV set 0 comes from its remap block (clodUvRemap.hlsli), derivatives through the remap.
    if (uvSetIndex == 0u && uvInfo.uvDesc.reserved0 != 0u && g_clodUvRemapTriangle != CLOD_UV_REMAP_UNKNOWN_TRIANGLE)
    {
        const bool swapped = (g_clodUvRemapTriangle & CLOD_UV_REMAP_SWAPPED_CORNERS) != 0u;
        const float3 lambda = swapped ? bary.m_lambda.xzy : bary.m_lambda;
        const float3 lambdaDdx = swapped ? bary.m_ddx.xzy : bary.m_ddx;
        const float3 lambdaDdy = swapped ? bary.m_ddy.xzy : bary.m_ddy;
        ByteAddressBuffer remapSlab = ResourceDescriptorHeap[NonUniformResourceIndex(md.pagePoolSlabDescriptorIndex)];
        const CLodUvRemapSample remap = CLodEvaluateUvRemap(
            remapSlab, md.pageByteOffset + uvInfo.uvDesc.reserved0, g_clodUvRemapTriangle & ~CLOD_UV_REMAP_SWAPPED_CORNERS, lambda.y, lambda.z);
        if (remap.remapped)
        {
            sample.uv = remap.uv;
            sample.dUVdx = remap.dUvDb1 * lambdaDdx.y + remap.dUvDb2 * lambdaDdx.z;
            sample.dUVdy = remap.dUvDb1 * lambdaDdy.y + remap.dUvDb2 * lambdaDdy.z;
        }
    }
#endif

#if defined(MATERIAL_EVAL_SINGLE_UV_SET)
    if (cache.count == 0u) // One UV set: only the first append holds a set the slots can bind (MaterialUvCacheAt).
    {
        cache.samples[0] = sample;
    }
#else
    cache.samples[cache.count] = sample;
#endif
    cache.count++;
}

// The eval builder's UV set -> cache index map. With one UV set it is one value, so no array is indexed at run time.
#if defined(MATERIAL_EVAL_SINGLE_UV_SET)
#define CLOD_EVAL_UV_SET_MAP_DECLARE uint clodEvalSingleCacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX
#define CLOD_EVAL_UV_SET_MAP_GET(uvSet) clodEvalSingleCacheIndex
#define CLOD_EVAL_UV_SET_MAP_SET(uvSet, value) clodEvalSingleCacheIndex = (value)
#else
#define CLOD_EVAL_UV_SET_MAP_DECLARE uint cacheIndexByUvSet[MATERIAL_MAX_UNIQUE_UV_SETS];     [unroll] for (uint uvSetInit = 0u; uvSetInit < MATERIAL_MAX_UNIQUE_UV_SETS; ++uvSetInit) cacheIndexByUvSet[uvSetInit] = MATERIAL_INVALID_UV_CACHE_INDEX
#define CLOD_EVAL_UV_SET_MAP_GET(uvSet) cacheIndexByUvSet[uvSet]
#define CLOD_EVAL_UV_SET_MAP_SET(uvSet, value) cacheIndexByUvSet[uvSet] = (value)
#endif

void BuildClodMaterialUvData(
    MaterialInfo materialInfo,
    uint materialFlags,
    MeshletResolveData md,
    uint3 triIdx,
    BarycentricDeriv bary,
    out MaterialUvCache cache,
    out MaterialUvBindings bindings)
{
    cache = (MaterialUvCache)0;
    InitializeMaterialUvBindings(bindings);

    uint cacheIndexByUvSet[MATERIAL_MAX_UNIQUE_UV_SETS];

    [unroll]
    for (uint uvSetIndex = 0u; uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS; ++uvSetIndex)
    {
        cacheIndexByUvSet[uvSetIndex] = MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        const MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        if (!MaterialSlotEnabled(materialInfo, materialFlags, textureSlot))
        {
            continue;
        }

        const uint uvSetIndex = MaterialSlotUvSetIndex(materialInfo, textureSlot);
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS &&
            cacheIndexByUvSet[uvSetIndex] != MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            continue;
        }

        const uint cacheIndex = cache.count;
        const MeshletUvDecodeInfo uvInfo = LoadMeshletUvDecodeInfo(uvSetIndex, md);
        AppendClodMaterialUvSample(cache, uvSetIndex, triIdx, uvInfo, md, bary);
        if (cache.count > cacheIndex && uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndexByUvSet[uvSetIndex] = cacheIndex;
        }
    }

    const OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(materialInfo);
    const uint uvSetIndices[6] = {
        openPBRMaterialInfo.coatColorUvSetIndex,
        openPBRMaterialInfo.coatWeightUvSetIndex,
        openPBRMaterialInfo.coatRoughnessUvSetIndex,
        openPBRMaterialInfo.fuzzColorUvSetIndex,
        openPBRMaterialInfo.fuzzWeightUvSetIndex,
        openPBRMaterialInfo.fuzzRoughnessUvSetIndex
    };
    const uint textureIndices[6] = {
        openPBRMaterialInfo.coatColorTextureIndex,
        openPBRMaterialInfo.coatWeightTextureIndex,
        openPBRMaterialInfo.coatRoughnessTextureIndex,
        openPBRMaterialInfo.fuzzColorTextureIndex,
        openPBRMaterialInfo.fuzzWeightTextureIndex,
        openPBRMaterialInfo.fuzzRoughnessTextureIndex
    };
    const uint samplerIndices[6] = {
        openPBRMaterialInfo.coatColorSamplerIndex,
        openPBRMaterialInfo.coatWeightSamplerIndex,
        openPBRMaterialInfo.coatRoughnessSamplerIndex,
        openPBRMaterialInfo.fuzzColorSamplerIndex,
        openPBRMaterialInfo.fuzzWeightSamplerIndex,
        openPBRMaterialInfo.fuzzRoughnessSamplerIndex
    };

    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        if (!HasOpenPBRTexture(textureIndices[i], samplerIndices[i]))
        {
            continue;
        }

        const uint uvSetIndex = uvSetIndices[i];
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS &&
            cacheIndexByUvSet[uvSetIndex] != MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            continue;
        }

        const uint cacheIndex = cache.count;
        const MeshletUvDecodeInfo uvInfo = LoadMeshletUvDecodeInfo(uvSetIndex, md);
        AppendClodMaterialUvSample(cache, uvSetIndex, triIdx, uvInfo, md, bary);
        if (cache.count > cacheIndex && uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndexByUvSet[uvSetIndex] = cacheIndex;
        }
    }

    const uint uv0CacheIndex = cacheIndexByUvSet[0u];

    [unroll]
    for (uint slot = 0u; slot < OPENPBR_TEXTURE_SLOT_COUNT; ++slot)
    {
        if (!HasOpenPBRTexture(textureIndices[slot], samplerIndices[slot]))
        {
            continue;
        }

        uint cacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
        const uint uvSetIndex = uvSetIndices[slot];
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndex = cacheIndexByUvSet[uvSetIndex];
        }
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.openPBRCacheIndexBySlot[slot] = cacheIndex;
    }

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        const MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        if (!MaterialSlotEnabled(materialInfo, materialFlags, textureSlot))
        {
            continue;
        }

        uint cacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
        const uint uvSetIndex = MaterialSlotUvSetIndex(materialInfo, textureSlot);
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndex = cacheIndexByUvSet[uvSetIndex];
        }
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.cacheIndexBySlot[slot] = cacheIndex;
    }
    if ((materialFlags & MATERIAL_NORMAL_MAP) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_NORMAL];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }
    else if ((materialFlags & MATERIAL_PARALLAX) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    if ((materialFlags & (MATERIAL_PARALLAX | MATERIAL_GEOMETRIC_DISPLACEMENT)) != 0u)
    {
        bindings.heightCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasHeightSource = bindings.heightCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    ApplyMaterialUvTransformToCache(cache, materialInfo);
}

void BuildClodMaterialUvData(
    MaterialEvalInfo materialInfo,
    uint materialFlags,
    MeshletResolveData md,
    uint3 triIdx,
    BarycentricDeriv bary,
    out MaterialUvCache cache,
    out MaterialUvBindings bindings)
{
    cache = (MaterialUvCache)0;
    InitializeMaterialUvBindings(bindings);

    CLOD_EVAL_UV_SET_MAP_DECLARE;
#if defined(MATERIAL_EVAL_SINGLE_UV_SET)
    uint singleUvSet = MATERIAL_INVALID_UV_CACHE_INDEX;
#endif

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        const MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        bool slotEnabled = MaterialSlotEnabled(materialInfo, materialFlags, textureSlot);
        switch (textureSlot)
        {
        case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
#if defined(PSO_BASE_COLOR_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_OPACITY:
#if defined(PSO_OPACITY_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_METALLIC:
#if defined(PSO_METALLIC_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_ROUGHNESS:
#if defined(PSO_ROUGHNESS_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_NORMAL:
#if defined(PSO_NORMAL_MAP)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_AO:
#if defined(PSO_AO_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_EMISSIVE:
#if defined(PSO_EMISSIVE_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_HEIGHT:
#if defined(PSO_PARALLAX)
            slotEnabled =
                (((materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) == 0u) ||
                 ((materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u));
#endif
            break;
        default:
            break;
        }
        if (!slotEnabled)
        {
            continue;
        }

        const uint uvSetIndex = MaterialSlotUvSetIndex(materialInfo, textureSlot);
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS &&
            CLOD_EVAL_UV_SET_MAP_GET(uvSetIndex) != MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            continue;
        }

#if defined(MATERIAL_EVAL_SINGLE_UV_SET)
        // Every bound slot shares the first one's set: note it, and decode it once after the loop (an append here was
        // inlined once a slot, four UV decodes in a four-texture variant).
        if (singleUvSet == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            singleUvSet = uvSetIndex;
        }
#else
        const uint cacheIndex = cache.count;
        const MeshletUvDecodeInfo uvInfo = LoadMeshletUvDecodeInfo(uvSetIndex, md);
        AppendClodMaterialUvSample(cache, uvSetIndex, triIdx, uvInfo, md, bary);
        if (cache.count > cacheIndex && uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            CLOD_EVAL_UV_SET_MAP_SET(uvSetIndex, cacheIndex);
        }
#endif
    }

#if defined(MATERIAL_EVAL_SINGLE_UV_SET)
    if (singleUvSet != MATERIAL_INVALID_UV_CACHE_INDEX)
    {
        AppendClodMaterialUvSample(cache, singleUvSet, triIdx, LoadMeshletUvDecodeInfo(singleUvSet, md), md, bary);
        CLOD_EVAL_UV_SET_MAP_SET(singleUvSet, 0u);
    }
#endif
    const uint uv0CacheIndex = CLOD_EVAL_UV_SET_MAP_GET(0u);

#if defined(PSO_OPENPBR_COAT_COLOR_TEXTURE) || defined(PSO_OPENPBR_COAT_WEIGHT_TEXTURE) || defined(PSO_OPENPBR_COAT_ROUGHNESS_TEXTURE) || defined(PSO_OPENPBR_FUZZ_COLOR_TEXTURE) || defined(PSO_OPENPBR_FUZZ_WEIGHT_TEXTURE) || defined(PSO_OPENPBR_FUZZ_ROUGHNESS_TEXTURE)
    const OpenPBRMaterialInfo openPBRMaterialInfo = LoadOpenPBRMaterialInfo(materialInfo.openPBRMaterialDataIndex);
    const uint uvSetIndices[6] = {
        openPBRMaterialInfo.coatColorUvSetIndex,
        openPBRMaterialInfo.coatWeightUvSetIndex,
        openPBRMaterialInfo.coatRoughnessUvSetIndex,
        openPBRMaterialInfo.fuzzColorUvSetIndex,
        openPBRMaterialInfo.fuzzWeightUvSetIndex,
        openPBRMaterialInfo.fuzzRoughnessUvSetIndex
    };
    const uint textureIndices[6] = {
        openPBRMaterialInfo.coatColorTextureIndex,
        openPBRMaterialInfo.coatWeightTextureIndex,
        openPBRMaterialInfo.coatRoughnessTextureIndex,
        openPBRMaterialInfo.fuzzColorTextureIndex,
        openPBRMaterialInfo.fuzzWeightTextureIndex,
        openPBRMaterialInfo.fuzzRoughnessTextureIndex
    };
    const uint samplerIndices[6] = {
        openPBRMaterialInfo.coatColorSamplerIndex,
        openPBRMaterialInfo.coatWeightSamplerIndex,
        openPBRMaterialInfo.coatRoughnessSamplerIndex,
        openPBRMaterialInfo.fuzzColorSamplerIndex,
        openPBRMaterialInfo.fuzzWeightSamplerIndex,
        openPBRMaterialInfo.fuzzRoughnessSamplerIndex
    };

    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        if (!HasOpenPBRTexture(textureIndices[i], samplerIndices[i]))
        {
            continue;
        }

        const uint uvSetIndex = uvSetIndices[i];
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS &&
            CLOD_EVAL_UV_SET_MAP_GET(uvSetIndex) != MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            continue;
        }

#if defined(MATERIAL_EVAL_SINGLE_UV_SET)
        if (cache.count == 0u) // Only OpenPBR textures are bound.
        {
            AppendClodMaterialUvSample(cache, uvSetIndex, triIdx, LoadMeshletUvDecodeInfo(uvSetIndex, md), md, bary);
            CLOD_EVAL_UV_SET_MAP_SET(uvSetIndex, 0u);
        }
#else
        const uint cacheIndex = cache.count;
        const MeshletUvDecodeInfo uvInfo = LoadMeshletUvDecodeInfo(uvSetIndex, md);
        AppendClodMaterialUvSample(cache, uvSetIndex, triIdx, uvInfo, md, bary);
        if (cache.count > cacheIndex && uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            CLOD_EVAL_UV_SET_MAP_SET(uvSetIndex, cacheIndex);
        }
#endif
    }

    [unroll]
    for (uint slot = 0u; slot < OPENPBR_TEXTURE_SLOT_COUNT; ++slot)
    {
        if (!HasOpenPBRTexture(textureIndices[slot], samplerIndices[slot]))
        {
            continue;
        }

        uint cacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
        const uint uvSetIndex = uvSetIndices[slot];
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndex = CLOD_EVAL_UV_SET_MAP_GET(uvSetIndex);
        }
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.openPBRCacheIndexBySlot[slot] = cacheIndex;
    }
#endif

    [unroll]
    for (uint slot = 0u; slot < MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
    {
        const MaterialTextureSlot textureSlot = (MaterialTextureSlot)slot;
        bool slotEnabled = MaterialSlotEnabled(materialInfo, materialFlags, textureSlot);
        switch (textureSlot)
        {
        case MATERIAL_TEXTURE_SLOT_BASE_COLOR:
#if defined(PSO_BASE_COLOR_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_OPACITY:
#if defined(PSO_OPACITY_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_METALLIC:
#if defined(PSO_METALLIC_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_ROUGHNESS:
#if defined(PSO_ROUGHNESS_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_NORMAL:
#if defined(PSO_NORMAL_MAP)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_AO:
#if defined(PSO_AO_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_EMISSIVE:
#if defined(PSO_EMISSIVE_TEXTURE)
            slotEnabled = true;
#endif
            break;
        case MATERIAL_TEXTURE_SLOT_HEIGHT:
#if defined(PSO_PARALLAX)
            slotEnabled =
                (((materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) == 0u) ||
                 ((materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u));
#endif
            break;
        default:
            break;
        }
        if (!slotEnabled)
        {
            continue;
        }

        uint cacheIndex = MATERIAL_INVALID_UV_CACHE_INDEX;
        const uint uvSetIndex = MaterialSlotUvSetIndex(materialInfo, textureSlot);
        if (uvSetIndex < MATERIAL_MAX_UNIQUE_UV_SETS)
        {
            cacheIndex = CLOD_EVAL_UV_SET_MAP_GET(uvSetIndex);
        }
        if (cacheIndex == MATERIAL_INVALID_UV_CACHE_INDEX)
        {
            cacheIndex = uv0CacheIndex;
        }

        bindings.cacheIndexBySlot[slot] = cacheIndex;
    }
    if ((materialFlags & MATERIAL_NORMAL_MAP) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_NORMAL];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }
    else if ((materialFlags & MATERIAL_PARALLAX) != 0u)
    {
        bindings.tbnCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasTbnSource = bindings.tbnCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    if ((materialFlags & (MATERIAL_PARALLAX | MATERIAL_GEOMETRIC_DISPLACEMENT)) != 0u)
    {
        bindings.heightCacheIndex = bindings.cacheIndexBySlot[MATERIAL_TEXTURE_SLOT_HEIGHT];
        bindings.hasHeightSource = bindings.heightCacheIndex != MATERIAL_INVALID_UV_CACHE_INDEX;
    }

    ApplyMaterialUvTransformToCache(cache, materialInfo);
}

#endif // CLOD_MATERIAL_UVS_HLSLI
