#ifndef CLOD_MATERIAL_UVS_HLSLI
#define CLOD_MATERIAL_UVS_HLSLI

// Decodes each UV set a material samples, with derivatives from the pixel's barycentrics.
// Requires clodResolveMeshlet.hlsli.

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
    uint encodedU = ReadPackedBits32_BA(slab, bitCursor, uvBitsU);
    bitCursor += uvBitsU;
    uint encodedV = ReadPackedBits32_BA(slab, bitCursor, uvBitsV);

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

    cache.samples[cache.count] = sample;
    cache.count++;
}

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

#endif // CLOD_MATERIAL_UVS_HLSLI
