#ifndef __CLOD_RESOLVE_COMMON_HLSLI__
#define __CLOD_RESOLVE_COMMON_HLSLI__

//#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/meshletCommon.hlsli"
#include "include/utilities.hlsli"
#include "include/terrainCommon.hlsli"
#include "include/waveIntrinsicsHelpers.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/reyesPatchCommon.hlsli"
#include "include/sggxCommon.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/vertexLayout.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"

#define CLOD_COMPRESSED_POSITIONS 1u
#define CLOD_COMPRESSED_NORMALS 4u

struct BarycentricDeriv
{
    float3 m_lambda;
    float3 m_ddx;
    float3 m_ddy;
};

struct ClodResolvedSample
{
    uint2 pixelCoords;
    float linearDepth;
    uint clusterIndex;
    uint meshletTriangleIndex;
    uint meshletIndex;
    float3 positionWS;
    float3 positionVS;
    float3 normalWSBase;
    float3 normalOS;
    float3 vertexColor;
    float3 dpdxWS;
    float3 dpdyWS;
    float2 motionVector;
    MaterialInfo materialInfo;
    uint materialFlags;
    MaterialInputs materialInputs;
};

struct ClodShadingSample
{
    float linearDepth;
    float3 positionWS;
    float3 positionVS;
    uint materialFlags;
    MaterialInputs materialInputs;
};

struct ClodGBufferColorSample
{
    float2 motionVector;
    MaterialInputs materialInputs;
};

struct ClodGBufferDebugSample
{
    uint meshletIndex;
    uint geometryGroupIndex;
    bool isVoxelPath;
    float3 normalOS;
    float2 materialDebugUv;
    float materialDebugUvValid;
    float2 materialDebugUvDerivative;
    float3 reyesDebugSourceBarycentrics;
    uint materialDebugFlags;
    float2 motionVector;
    MaterialInputs materialInputs;
};

struct ClodResolvedGBufferSample
{
    uint meshletIndex;
    uint geometryGroupIndex;
    bool isVoxelPath;
    float3 normalOS;
    float2 materialDebugUv;
    float materialDebugUvValid;
    float2 materialDebugUvDerivative;
    float3 reyesDebugSourceBarycentrics;
    uint materialDebugFlags;
    float2 motionVector;
    MaterialInputs materialInputs;
};

struct ClodResolvedCommonSample
{
    float linearDepth;
    uint clusterIndex;
    uint meshletTriangleIndex;
    uint meshletIndex;
    uint geometryGroupIndex;
    bool isVoxelPath;
    float3 positionWS;
    float3 positionVS;
    float3 normalWSBase;
    float3 normalOS;
    float3 vertexColor;
    float2 materialDebugUv;
    float materialDebugUvValid;
    float2 materialDebugUvDerivative;
    float3 reyesDebugSourceBarycentrics;
    float3 dpdxWS;
    float3 dpdyWS;
    float2 motionVector;
    MaterialInfo materialInfo;
    uint materialFlags;
    MaterialInputs materialInputs;
};

BarycentricDeriv CalcFullBary(float4 pt0, float4 pt1, float4 pt2, float2 pixelNdc, float2 winSize)
{
    BarycentricDeriv ret = (BarycentricDeriv)0;

    float3 invW = rcp(float3(pt0.w, pt1.w, pt2.w));

    float2 ndc0 = pt0.xy * invW.x;
    float2 ndc1 = pt1.xy * invW.y;
    float2 ndc2 = pt2.xy * invW.z;

    float invDet = rcp(determinant(float2x2(ndc2 - ndc1, ndc0 - ndc1)));
    ret.m_ddx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * invDet * invW;
    ret.m_ddy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * invDet * invW;
    float ddxSum = dot(ret.m_ddx, float3(1, 1, 1));
    float ddySum = dot(ret.m_ddy, float3(1, 1, 1));

    float2 deltaVec = pixelNdc - ndc0;
    float interpInvW = invW.x + deltaVec.x * ddxSum + deltaVec.y * ddySum;
    float interpW = rcp(interpInvW);

    ret.m_lambda.x = interpW * (invW[0] + deltaVec.x * ret.m_ddx.x + deltaVec.y * ret.m_ddy.x);
    ret.m_lambda.y = interpW * (0.0f + deltaVec.x * ret.m_ddx.y + deltaVec.y * ret.m_ddy.y);
    ret.m_lambda.z = interpW * (0.0f + deltaVec.x * ret.m_ddx.z + deltaVec.y * ret.m_ddy.z);

    ret.m_ddx *= (2.0f / winSize.x);
    ret.m_ddy *= (2.0f / winSize.y);
    ddxSum *= (2.0f / winSize.x);
    ddySum *= (2.0f / winSize.y);

    ret.m_ddy *= -1.0f;
    ddySum *= -1.0f;

    float interpW_ddx = 1.0f / (interpInvW + ddxSum);
    float interpW_ddy = 1.0f / (interpInvW + ddySum);

    ret.m_ddx = interpW_ddx * (ret.m_lambda * interpInvW + ret.m_ddx) - ret.m_lambda;
    ret.m_ddy = interpW_ddy * (ret.m_lambda * interpInvW + ret.m_ddy) - ret.m_lambda;

    return ret;
}

#if defined(VISUTIL_DOUBLE_SIDED_GBUFFER_RESOLVE)
bool ClodBarycentricDerivativesAreBackFacing(BarycentricDeriv bary)
{
    const float detBarycentricDxDy = bary.m_ddx.y * bary.m_ddy.z - bary.m_ddy.y * bary.m_ddx.z;
    return detBarycentricDxDy > 0.0f;
}
#endif

float3 InterpolateWithDeriv(BarycentricDeriv deriv, float v0, float v1, float v2)
{
    float3 mergedV = float3(v0, v1, v2);
    float3 ret;
    ret.x = dot(mergedV, deriv.m_lambda);
    ret.y = dot(mergedV, deriv.m_ddx);
    ret.z = dot(mergedV, deriv.m_ddy);
    return ret;
}

#if defined(PSO_TERRAIN)
void CLodResolveCameraRayWS(Camera cam, float2 pixelCenter, float2 winSize, out float3 rayOriginWS, out float3 rayDirectionWS)
{
    const float2 pixelUv = pixelCenter / winSize;
    const float2 ndc = float2(pixelUv.x * 2.0f - 1.0f, 1.0f - pixelUv.y * 2.0f);
    float4 viewNear = mul(float4(ndc, 0.0f, 1.0f), cam.projectionInverse);
    float4 viewFar = mul(float4(ndc, 1.0f, 1.0f), cam.projectionInverse);
    viewNear.xyz /= max(abs(viewNear.w), 1.0e-6f);
    viewFar.xyz /= max(abs(viewFar.w), 1.0e-6f);
    if (cam.isOrtho)
    {
        const float4 worldNear = mul(float4(viewNear.xyz, 1.0f), cam.viewInverse);
        rayOriginWS = worldNear.xyz / max(abs(worldNear.w), 1.0e-6f);
        rayDirectionWS = normalize(mul(float4(viewFar.xyz - viewNear.xyz, 0.0f), cam.viewInverse).xyz);
    }
    else
    {
        rayOriginWS = cam.positionWorldSpace.xyz;
        rayDirectionWS = normalize(mul(float4(viewFar.xyz, 0.0f), cam.viewInverse).xyz);
    }
}

bool CLodResolveIntersectRayWithPlane(
    float3 rayOriginWS,
    float3 rayDirectionWS,
    float3 planePointWS,
    float3 planeNormalWS,
    out float3 positionWS)
{
    positionWS = planePointWS;
    const float denom = dot(rayDirectionWS, planeNormalWS);
    if (abs(denom) <= 1.0e-5f)
    {
        return false;
    }

    const float t = dot(planePointWS - rayOriginWS, planeNormalWS) / denom;
    positionWS = rayOriginWS + rayDirectionWS * t;
    return all(isfinite(positionWS));
}

bool CLodResolveEstimateTerrainTangentPlaneDerivatives(
    Camera cam,
    uint2 pixel,
    float2 winSize,
    float3 positionWS,
    float3 normalWS,
    out float3 dpdxWS,
    out float3 dpdyWS)
{
    dpdxWS = 0.0f.xxx;
    dpdyWS = 0.0f.xxx;

    const float normalLengthSq = dot(normalWS, normalWS);
    if (normalLengthSq <= 1.0e-10f)
    {
        return false;
    }

    const float3 planeNormalWS = normalWS * rsqrt(normalLengthSq);
    const float2 pixelCenter = float2(pixel) + 0.5f;
    float3 rayOrigin0WS;
    float3 rayOriginXWS;
    float3 rayOriginYWS;
    float3 rayDirection0WS;
    float3 rayDirectionXWS;
    float3 rayDirectionYWS;
    CLodResolveCameraRayWS(cam, pixelCenter, winSize, rayOrigin0WS, rayDirection0WS);
    CLodResolveCameraRayWS(cam, pixelCenter + float2(1.0f, 0.0f), winSize, rayOriginXWS, rayDirectionXWS);
    CLodResolveCameraRayWS(cam, pixelCenter + float2(0.0f, 1.0f), winSize, rayOriginYWS, rayDirectionYWS);

    float3 planePosition0WS;
    float3 planePositionXWS;
    float3 planePositionYWS;
    if (!CLodResolveIntersectRayWithPlane(rayOrigin0WS, rayDirection0WS, positionWS, planeNormalWS, planePosition0WS) ||
        !CLodResolveIntersectRayWithPlane(rayOriginXWS, rayDirectionXWS, positionWS, planeNormalWS, planePositionXWS) ||
        !CLodResolveIntersectRayWithPlane(rayOriginYWS, rayDirectionYWS, positionWS, planeNormalWS, planePositionYWS))
    {
        return false;
    }

    dpdxWS = planePositionXWS - planePosition0WS;
    dpdyWS = planePositionYWS - planePosition0WS;
    return all(isfinite(dpdxWS)) && all(isfinite(dpdyWS));
}
#endif

struct MeshletResolveData {
    uint2 drawcallAndMeshlet;
    uint2 objAndMesh;
    uint4 meshInfo;
    uint materialDataIndex;
    uint vertexCount;
    uint triangleCount;
    uint bitsX;
    uint bitsY;
    uint bitsZ;
    int3 minQ;
    uint positionBitOffset;
    uint vertexAttributeOffset;
    uint triangleByteOffset;
    uint pageAttributeMask;
    uint uvSetCount;
    uint uvDescriptorBase;
    uint uvBitstreamDirectoryBase;
    uint pageByteOffset;
    uint positionBitstreamBase;
    uint normalArrayBase;
    uint tangentFrameArrayBase;
    uint colorArrayBase;
    uint jointArrayBase;
    uint weightArrayBase;
    uint triangleStreamBase;
    uint skinningInstanceSlot;
    uint compressedPositionQuantExp;
    uint pagePoolSlabDescriptorIndex;
};

uint ReadPackedBits32_BA(ByteAddressBuffer buf, uint startBit, uint bitCount)
{
    if (bitCount == 0u)
    {
        return 0u;
    }

    uint wordIndex = startBit >> 5;
    uint bitOffset = startBit & 31u;
    uint packed = buf.Load(wordIndex * 4u) >> bitOffset;
    if (bitOffset + bitCount > 32u)
    {
        packed |= buf.Load((wordIndex + 1u) * 4u) << (32u - bitOffset);
    }

    uint mask = (bitCount >= 32u) ? 0xffffffffu : ((1u << bitCount) - 1u);
    return packed & mask;
}

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

    ByteAddressBuffer slab = ResourceDescriptorHeap[d.pagePoolSlabDescriptorIndex];
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
}

float3 DecodeCompressedPosition(uint meshletLocalVertex, MeshletResolveData d)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[d.pagePoolSlabDescriptorIndex];
    return CLodLoadPagePosition(slab, d.compressedPositionQuantExp, d.positionBitstreamBase, d.positionBitOffset, meshletLocalVertex);
}

float2 UnpackSnorm16x2(uint packed)
{
    int signedPacked = asint(packed);
    int x = (signedPacked << 16) >> 16;
    int y = signedPacked >> 16;
    float sx = max(-1.0f, (float)x / 32767.0f);
    float sy = max(-1.0f, (float)y / 32767.0f);
    return float2(sx, sy);
}

float3 OctDecodeNormal(float2 e)
{
    float3 v = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f)
    {
        float2 folded = (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
        v.x = folded.x;
        v.y = folded.y;
    }
    return normalize(v);
}

float3 DecodeCompressedNormal(uint meshletLocalVertex, MeshletResolveData d)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[d.pagePoolSlabDescriptorIndex];
    uint addr = d.normalArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 4u;
    uint packed = slab.Load(addr);
    return OctDecodeNormal(UnpackSnorm16x2(packed));
}

void CLodBuildTangentAngleBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    normal = normalize(normal);
    float3 helper = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    tangent = normalize(cross(helper, normal));
    bitangent = normalize(cross(tangent, normal));
}

float4 DecodeCompressedTangentFrame(uint meshletLocalVertex, float3 normalOS, MeshletResolveData d)
{
    if ((d.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) == 0u)
    {
        return float4(1.0f, 0.0f, 0.0f, 0.0f);
    }

    uint addr = d.tangentFrameArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 4u;
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    uint packed = slab.Load(addr);
    float angle = (float(packed & 0xFFFFu) / 65535.0f) * 6.28318530718f;
    float handedness = ((packed & (1u << 16u)) != 0u) ? -1.0f : 1.0f;
    float s;
    float c;
    sincos(angle, s, c);
    float3 basisT;
    float3 basisB;
    CLodBuildTangentAngleBasis(normalOS, basisT, basisB);
    return float4(normalize(basisT * c + basisB * s), handedness);
}

float3 DecodeCompressedColor(uint meshletLocalVertex, MeshletResolveData d)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[d.pagePoolSlabDescriptorIndex];
    uint addr = d.colorArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 4u;
    uint packed = slab.Load(addr);
    return float3(
        float(packed & 0xFFu) / 255.0f,
        float((packed >> 8u) & 0xFFu) / 255.0f,
        float((packed >> 16u) & 0xFFu) / 255.0f);
}

SkinningInfluences DecodePackedJoints(uint meshletLocalVertex, MeshletResolveData d)
{
    SkinningInfluences skinning;
    skinning.joints0 = uint4(0, 0, 0, 0);
    skinning.joints1 = uint4(0, 0, 0, 0);
    skinning.weights0 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    skinning.weights1 = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if ((d.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[d.pagePoolSlabDescriptorIndex];
    uint addr = d.jointArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.joints0 = LoadUint4(addr, slab);
    skinning.joints1 = LoadUint4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences DecodePackedWeights(uint meshletLocalVertex, MeshletResolveData d, SkinningInfluences skinning)
{
    if ((d.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[d.pagePoolSlabDescriptorIndex];
    uint addr = d.weightArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.weights0 = LoadFloat4(addr, slab);
    skinning.weights1 = LoadFloat4(addr + 16u, slab);
    return skinning;
}

void ApplyClodSkinningToFrame(uint meshletLocalVertex, MeshletResolveData d, inout float3 positionOS, inout float3 normalOS, inout float4 tangentOS)
{
    if ((d.meshInfo.y & VERTEX_SKINNED) == 0u)
    {
        return;
    }

    SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, d);
    skinning = DecodePackedWeights(meshletLocalVertex, d, skinning);
    float4x4 skinMatrix = BuildSkinMatrix(d.skinningInstanceSlot, skinning);
    positionOS = mul(float4(positionOS, 1.0f), skinMatrix).xyz;
    normalOS = mul(normalOS, (float3x3)skinMatrix);
    tangentOS.xyz = mul(tangentOS.xyz, (float3x3)skinMatrix);
}

void ApplyClodSkinning(uint meshletLocalVertex, MeshletResolveData d, inout float3 positionOS, inout float3 normalOS)
{
    float4 unusedTangent = float4(1.0f, 0.0f, 0.0f, 0.0f);
    ApplyClodSkinningToFrame(meshletLocalVertex, d, positionOS, normalOS, unusedTangent);
}

MeshletResolveData LoadMeshletResolveData_Wave(uint clusterIndex)
{
    MeshletResolveData d = (MeshletResolveData)0;

    uint4 mask = WaveMatch(clusterIndex);
    uint leader = WaveFirstLaneFromMask(mask);
    bool isLeader = (WaveGetLaneIndex() == leader);

    if (isLeader)
    {
        ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
        StructuredBuffer<PerMeshBuffer> perMeshBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];

        const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
        d.drawcallAndMeshlet.x = CLodVisibleClusterInstanceID(packedCluster);
        d.drawcallAndMeshlet.y = CLodVisibleClusterLocalMeshletIndex(packedCluster);

        PerMeshInstanceBuffer inst = LoadMeshTemplateForDraw(d.drawcallAndMeshlet.x);
        d.objAndMesh = uint2(d.drawcallAndMeshlet.x, inst.perMeshBufferIndex);
        d.skinningInstanceSlot = inst.skinningInstanceSlot;

        PerMeshBuffer mesh = perMeshBuffer[d.objAndMesh.y];

        const uint pageSlabDesc = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
        const uint pageSlabOff = CLodVisibleClusterPageSlabByteOffset(packedCluster);

        CLodPageHeader hdr = LoadPageHeader(pageSlabDesc, pageSlabOff);
        CLodMeshletDescriptor desc = LoadMeshletDescriptor(
            pageSlabDesc, pageSlabOff, hdr.descriptorOffset, d.drawcallAndMeshlet.y);

        d.meshInfo = uint4(mesh.vertexByteSize, mesh.vertexFlags, mesh.numVertices, 0);
        d.materialDataIndex = mesh.materialDataIndex;

        d.vertexCount = CLodDescVertexCount(desc);
        d.triangleCount = CLodDescTriangleCount(desc);
        d.bitsX = CLodDescBitsX(desc);
        d.bitsY = CLodDescBitsY(desc);
        d.bitsZ = CLodDescBitsZ(desc);
        d.minQ = int3(desc.minQx, desc.minQy, desc.minQz);
        d.positionBitOffset = desc.positionBitOffset;
        d.vertexAttributeOffset = desc.vertexAttributeOffset;
        d.triangleByteOffset = desc.triangleByteOffset;
        d.pageAttributeMask = hdr.attributeMask;
        d.uvSetCount = hdr.uvSetCount;

        d.pageByteOffset = pageSlabOff;
        d.uvDescriptorBase = pageSlabOff + hdr.uvDescriptorOffset;
        d.uvBitstreamDirectoryBase = pageSlabOff + hdr.uvBitstreamDirectoryOffset;
        d.positionBitstreamBase = pageSlabOff + hdr.positionBitstreamOffset;
        d.normalArrayBase = pageSlabOff + hdr.normalArrayOffset;
        d.tangentFrameArrayBase = pageSlabOff + hdr.tangentFrameArrayOffset;
        d.colorArrayBase = pageSlabOff + hdr.colorArrayOffset;
        d.jointArrayBase = pageSlabOff + hdr.jointArrayOffset;
        d.weightArrayBase = pageSlabOff + hdr.weightArrayOffset;
        d.triangleStreamBase = pageSlabOff + hdr.triangleStreamOffset;

        d.compressedPositionQuantExp = hdr.compressedPositionQuantExp;
        d.pagePoolSlabDescriptorIndex = pageSlabDesc;
    }

    d.drawcallAndMeshlet = WaveReadLaneAt(d.drawcallAndMeshlet, leader);
    d.objAndMesh = WaveReadLaneAt(d.objAndMesh, leader);
    d.meshInfo = WaveReadLaneAt(d.meshInfo, leader);
    d.materialDataIndex = WaveReadLaneAt(d.materialDataIndex, leader);
    d.vertexCount = WaveReadLaneAt(d.vertexCount, leader);
    d.triangleCount = WaveReadLaneAt(d.triangleCount, leader);
    d.bitsX = WaveReadLaneAt(d.bitsX, leader);
    d.bitsY = WaveReadLaneAt(d.bitsY, leader);
    d.bitsZ = WaveReadLaneAt(d.bitsZ, leader);
    d.minQ.x = WaveReadLaneAt(d.minQ.x, leader);
    d.minQ.y = WaveReadLaneAt(d.minQ.y, leader);
    d.minQ.z = WaveReadLaneAt(d.minQ.z, leader);
    d.positionBitOffset = WaveReadLaneAt(d.positionBitOffset, leader);
    d.vertexAttributeOffset = WaveReadLaneAt(d.vertexAttributeOffset, leader);
    d.triangleByteOffset = WaveReadLaneAt(d.triangleByteOffset, leader);
    d.pageAttributeMask = WaveReadLaneAt(d.pageAttributeMask, leader);
    d.uvSetCount = WaveReadLaneAt(d.uvSetCount, leader);
    d.uvDescriptorBase = WaveReadLaneAt(d.uvDescriptorBase, leader);
    d.uvBitstreamDirectoryBase = WaveReadLaneAt(d.uvBitstreamDirectoryBase, leader);
    d.pageByteOffset = WaveReadLaneAt(d.pageByteOffset, leader);
    d.positionBitstreamBase = WaveReadLaneAt(d.positionBitstreamBase, leader);
    d.normalArrayBase = WaveReadLaneAt(d.normalArrayBase, leader);
    d.tangentFrameArrayBase = WaveReadLaneAt(d.tangentFrameArrayBase, leader);
    d.colorArrayBase = WaveReadLaneAt(d.colorArrayBase, leader);
    d.jointArrayBase = WaveReadLaneAt(d.jointArrayBase, leader);
    d.weightArrayBase = WaveReadLaneAt(d.weightArrayBase, leader);
    d.triangleStreamBase = WaveReadLaneAt(d.triangleStreamBase, leader);
    d.compressedPositionQuantExp = WaveReadLaneAt(d.compressedPositionQuantExp, leader);
    d.pagePoolSlabDescriptorIndex = WaveReadLaneAt(d.pagePoolSlabDescriptorIndex, leader);
    d.skinningInstanceSlot = WaveReadLaneAt(d.skinningInstanceSlot, leader);

    return d;
}

uint3 CLodDecodeVoxelCubeCoord(uint packedCoord)
{
    return uint3(packedCoord & 0x3FFu, (packedCoord >> 10u) & 0x3FFu, (packedCoord >> 20u) & 0x3FFu);
}

bool ResolveVoxelMaskTest(uint2 mask, uint bitIndex)
{
    return bitIndex < 32u ? ((mask.x & (1u << bitIndex)) != 0u) : ((mask.y & (1u << (bitIndex - 32u))) != 0u);
}

bool ResolveRayBoxIntersect(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tEnter, out float tExit)
{
    tEnter = 0.0f;
    tExit = 3.402823e+38f;

    [unroll]
    for (uint axis = 0u; axis < 3u; ++axis)
    {
        const float origin = rayOrigin[axis];
        const float dir = rayDir[axis];
        const float bMin = boxMin[axis];
        const float bMax = boxMax[axis];

        if (abs(dir) <= 1.0e-8f)
        {
            if (origin < bMin || origin > bMax)
            {
                return false;
            }
            continue;
        }

        const float invDir = 1.0f / dir;
        float t0 = (bMin - origin) * invDir;
        float t1 = (bMax - origin) * invDir;
        if (t0 > t1)
        {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }

        tEnter = max(tEnter, t0);
        tExit = min(tExit, t1);
        if (tExit < tEnter)
        {
            return false;
        }
    }

    return tExit >= 0.0f;
}

bool ResolveRaycastVoxelCubeDDA(float3 rayOrigin, float3 rayDir, uint2 occupancyMask, out float tHit)
{
    tHit = 0.0f;

    float tEnter = 0.0f;
    float tExit = 0.0f;
    if (!ResolveRayBoxIntersect(rayOrigin, rayDir, float3(0.0f, 0.0f, 0.0f), float3(4.0f, 4.0f, 4.0f), tEnter, tExit))
    {
        return false;
    }

    float currentT = max(tEnter, 0.0f) + 1.0e-4f;
    const float3 p = clamp(rayOrigin + rayDir * currentT, float3(0.0f, 0.0f, 0.0f), float3(3.9999f, 3.9999f, 3.9999f));
    int3 cell = int3(floor(p));
    const int3 stepDir = int3(rayDir.x >= 0.0f ? 1 : -1, rayDir.y >= 0.0f ? 1 : -1, rayDir.z >= 0.0f ? 1 : -1);

    const float largeT = 3.402823e+38f;
    float3 nextBoundary = float3(
        stepDir.x > 0 ? float(cell.x + 1) : float(cell.x),
        stepDir.y > 0 ? float(cell.y + 1) : float(cell.y),
        stepDir.z > 0 ? float(cell.z + 1) : float(cell.z));
    float3 tMax = float3(
        abs(rayDir.x) > 1.0e-8f ? (nextBoundary.x - rayOrigin.x) / rayDir.x : largeT,
        abs(rayDir.y) > 1.0e-8f ? (nextBoundary.y - rayOrigin.y) / rayDir.y : largeT,
        abs(rayDir.z) > 1.0e-8f ? (nextBoundary.z - rayOrigin.z) / rayDir.z : largeT);
    float3 tDelta = float3(
        abs(rayDir.x) > 1.0e-8f ? abs(1.0f / rayDir.x) : largeT,
        abs(rayDir.y) > 1.0e-8f ? abs(1.0f / rayDir.y) : largeT,
        abs(rayDir.z) > 1.0e-8f ? abs(1.0f / rayDir.z) : largeT);

    [loop]
    for (uint iter = 0u; iter < 16u; ++iter)
    {
        if (any(cell < 0) || any(cell >= 4))
        {
            break;
        }

        const uint cellIndex = (uint)cell.x | ((uint)cell.y << 2u) | ((uint)cell.z << 4u);
        if (ResolveVoxelMaskTest(occupancyMask, cellIndex))
        {
            tHit = currentT;
            return true;
        }

        if (tMax.x <= tMax.y && tMax.x <= tMax.z)
        {
            if (tMax.x > tExit)
            {
                break;
            }
            currentT = tMax.x + 1.0e-4f;
            cell.x += stepDir.x;
            tMax.x += tDelta.x;
        }
        else if (tMax.y <= tMax.z)
        {
            if (tMax.y > tExit)
            {
                break;
            }
            currentT = tMax.y + 1.0e-4f;
            cell.y += stepDir.y;
            tMax.y += tDelta.y;
        }
        else
        {
            if (tMax.z > tExit)
            {
                break;
            }
            currentT = tMax.z + 1.0e-4f;
            cell.z += stepDir.z;
            tMax.z += tDelta.z;
        }
    }

    return false;
}

MaterialInputs BuildVoxelMaterialInputs(
    MaterialInfo materialInfo,
    uint materialFlags,
    float3 normalWS,
    float3 posWS,
    float2 uv,
    float2 dUVdx,
    float2 dUVdy,
    float opacity)
{
    MaterialUvCache uvCache = BuildSingleUvCache(uv, dUVdx, dUVdy);
    const uint voxelMaterialFlags = materialFlags & ~(MATERIAL_NORMAL_MAP | MATERIAL_PARALLAX | MATERIAL_GEOMETRIC_DISPLACEMENT);
    MaterialUvBindings uvBindings = BuildMaterialUvBindings(materialInfo, voxelMaterialFlags, uvCache);

    MaterialInputs inputs;
    SampleMaterialFromUvCacheRuntime(
        uvCache,
        uvBindings,
        normalWS,
        posWS,
        float3(1.0f, 1.0f, 1.0f),
        materialInfo,
        voxelMaterialFlags,
        float3(0.0f, 0.0f, 0.0f),
        float3(0.0f, 0.0f, 0.0f),
        inputs);
    inputs.opacity *= opacity;
    return inputs;
}

uint CLodVoxelLocalCellIndex(int3 cell)
{
    return (uint)cell.x | ((uint)cell.y << 2u) | ((uint)cell.z << 4u);
}

void CLodVoxelAccumulateNeighborUvDelta(
    GroupPageMapEntry pageEntry,
    CLodVoxelPageHeader pageHeader,
    CLodVoxelCubeRecord cube,
    int3 neighborCell,
    float2 uv,
    inout float maxUvDelta)
{
    if (any(neighborCell < int3(0, 0, 0)) || any(neighborCell > int3(3, 3, 3)))
    {
        return;
    }

    const uint neighborIndex = CLodVoxelLocalCellIndex(neighborCell);
    if (!ResolveVoxelMaskTest(cube.occupancyMask, neighborIndex))
    {
        return;
    }

    const CLodVoxelAttributeSample neighborSample =
        CLodLoadVoxelAttributeSampleFromPage(pageEntry, pageHeader, cube, neighborIndex);
    maxUvDelta = max(maxUvDelta, length(neighborSample.uv - uv));
}

float CLodVoxelEstimateUvFootprint(
    GroupPageMapEntry pageEntry,
    CLodVoxelPageHeader pageHeader,
    CLodVoxelCubeRecord cube,
    int3 cell,
    float2 uv,
    uint groupResolution)
{
    float maxUvDelta = 0.0f;
    CLodVoxelAccumulateNeighborUvDelta(pageEntry, pageHeader, cube, cell + int3(-1, 0, 0), uv, maxUvDelta);
    CLodVoxelAccumulateNeighborUvDelta(pageEntry, pageHeader, cube, cell + int3(1, 0, 0), uv, maxUvDelta);
    CLodVoxelAccumulateNeighborUvDelta(pageEntry, pageHeader, cube, cell + int3(0, -1, 0), uv, maxUvDelta);
    CLodVoxelAccumulateNeighborUvDelta(pageEntry, pageHeader, cube, cell + int3(0, 1, 0), uv, maxUvDelta);
    CLodVoxelAccumulateNeighborUvDelta(pageEntry, pageHeader, cube, cell + int3(0, 0, -1), uv, maxUvDelta);
    CLodVoxelAccumulateNeighborUvDelta(pageEntry, pageHeader, cube, cell + int3(0, 0, 1), uv, maxUvDelta);

    const float fallbackUvDelta = rcp((float)max(groupResolution, 1u));
    return max(maxUvDelta, fallbackUvDelta);
}

float2 CLodProjectWorldToPixel(float3 worldPosition, float4x4 viewProj, float2 winSize)
{
    float4 clip = mul(float4(worldPosition, 1.0f), viewProj);
    const float invW = rcp(max(abs(clip.w), 1.0e-6f));
    const float2 ndc = clip.xy * invW;
    return float2(
        (ndc.x * 0.5f + 0.5f) * winSize.x,
        (0.5f - ndc.y * 0.5f) * winSize.y);
}

float CLodVoxelEstimatePixelFootprint(
    float3 objectPosition,
    float voxelWidth,
    float4x4 localToWorld,
    float4x4 viewProj,
    float2 winSize)
{
    const float3 centerWorld = mul(float4(objectPosition, 1.0f), localToWorld).xyz;
    const float2 centerPixel = CLodProjectWorldToPixel(centerWorld, viewProj, winSize);
    const float3 xWorld = mul(float4(objectPosition + float3(voxelWidth, 0.0f, 0.0f), 1.0f), localToWorld).xyz;
    const float3 yWorld = mul(float4(objectPosition + float3(0.0f, voxelWidth, 0.0f), 1.0f), localToWorld).xyz;
    const float3 zWorld = mul(float4(objectPosition + float3(0.0f, 0.0f, voxelWidth), 1.0f), localToWorld).xyz;
    float maxPixelDelta = max(
        length(CLodProjectWorldToPixel(xWorld, viewProj, winSize) - centerPixel),
        length(CLodProjectWorldToPixel(yWorld, viewProj, winSize) - centerPixel));
    maxPixelDelta = max(maxPixelDelta, length(CLodProjectWorldToPixel(zWorld, viewProj, winSize) - centerPixel));

    return max(maxPixelDelta, 0.25f);
}

float2 ComputeClodMotionVector(float3 posOS, float3 worldPosition, float4x4 prevModel, float4x4 unjitteredViewProj, float4x4 prevUnjitteredViewProj);

uint CLodVoxelHash(uint v)
{
    v ^= v >> 16u;
    v *= 0x7FEB352Du;
    v ^= v >> 15u;
    v *= 0x846CA68Bu;
    v ^= v >> 16u;
    return v;
}

float CLodVoxelHashToUnitFloat(uint v)
{
    return (float)(CLodVoxelHash(v) & 0x00FFFFFFu) / 16777216.0f;
}

uint CLodVoxelHilbertIndex(uint posX, uint posY)
{
    const uint width = 64u;
    uint index = 0u;
    posX &= width - 1u;
    posY &= width - 1u;
    for (uint curLevel = width / 2u; curLevel > 0u; curLevel /= 2u)
    {
        const uint regionX = (posX & curLevel) != 0u;
        const uint regionY = (posY & curLevel) != 0u;
        index += curLevel * curLevel * ((3u * regionX) ^ regionY);
        if (regionY == 0u)
        {
            if (regionX != 0u)
            {
                posX = (width - 1u) - posX;
                posY = (width - 1u) - posY;
            }

            const uint temp = posX;
            posX = posY;
            posY = temp;
        }
    }
    return index;
}

float2 CLodVoxelR2Sequence(uint n)
{
    return frac(float2(
        0.5f + (float)n * 0.7548776662466927f,
        0.5f + (float)n * 0.5698402909980532f));
}

float2 CLodVoxelSGGXSpatiotemporalSample(uint2 pixel, uint frameIndex, uint stableSeed)
{
    const uint hilbertIndex = CLodVoxelHilbertIndex(pixel.x, pixel.y);
    const uint temporalIndex = frameIndex & 63u;
    const float2 screenTemporal = CLodVoxelR2Sequence(hilbertIndex + 288u * temporalIndex);
    const float2 stableOffset = float2(
        CLodVoxelHashToUnitFloat(stableSeed),
        CLodVoxelHashToUnitFloat(stableSeed ^ 0xD1B54A35u));
    return frac(screenTemporal + stableOffset);
}

void CLodVoxelBuildOrthonormalBasis(float3 direction, out float3 tangent, out float3 bitangent)
{
    SGGXBuildOrthonormalBasis(direction, tangent, bitangent);
}

float3 CLodVoxelMulSGGX(float3 sDiag, float3 sOff, float3 v)
{
    return SGGXMul(sDiag, sOff, v);
}

float3 CLodVoxelDecodeOctAxis(float2 encoded)
{
    return SGGXDecodeOctAxis(encoded);
}

float3 CLodVoxelMulAxialSGGX(float4 sggxAxisAndSigmas, float3 v)
{
    return SGGXMulAxial(sggxAxisAndSigmas, v);
}

float CLodVoxelDetSGGX(float3 sDiag, float3 sOff)
{
    return SGGXDet(sDiag, sOff);
}

float3 CLodVoxelDominantSGGXNormal(float3 sDiag, float3 sOff)
{
    return SGGXDominantNormal(sDiag, sOff);
}

float3 CLodVoxelDominantAxialSGGXNormal(float4 sggxAxisAndSigmas)
{
    return SGGXDominantAxialNormal(sggxAxisAndSigmas);
}

float3 CLodVoxelSampleSGGXVNDF(float4 sggxAxisAndSigmas, float3 wi, float u1, float u2)
{
    return SGGXSampleVNDF(sggxAxisAndSigmas, wi, u1, u2);
}

float3x3 CLodAssemblyAwareNormalMatrix(PerObjectBuffer obj)
{
    return transpose((float3x3)obj.modelInverse);
}

bool ResolveClodVoxelCommonSampleFromPackedCluster(
    uint4 packedCluster,
    uint visibleClusterIndex,
    uint primID,
    float linearDepth,
    uint2 pixel,
    Camera cam,
    out ClodResolvedCommonSample sample)
{
    sample = (ClodResolvedCommonSample)0;

    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<CLodMeshMetadata> metadataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];

    const uint instanceIndex = CLodVisibleClusterInstanceID(packedCluster);
    const uint localGroupId = CLodVisibleClusterGroupID(packedCluster);
    const uint localPageIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageLocalClusterIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);

    if (VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return false;
    }
    StructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint assemblyTransformIndex = visibleClusterTransformIndices[visibleClusterIndex];

    const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDraw(instanceIndex);
    const PerObjectBuffer obj = LoadInstanceTransformForDrawWithAssemblyTransform(instanceIndex, assemblyTransformIndex);
    const PerMeshBuffer mesh = perMeshBuffer[instanceData.perMeshBufferIndex];
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceIndex);
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup group = groups[metadata.groupsBase + localGroupId];
    if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
        localPageIndex < group.pageMapBase ||
        localPageIndex >= group.pageMapBase + group.pageCount)
    {
        return false;
    }

    GroupPageMapEntry pageEntry = CLodLoadVoxelPageMapEntry(metadata, group, localPageIndex);
    CLodVoxelPageHeader pageHeader = CLodLoadVoxelPageHeader(pageEntry.slabDescriptorIndex, pageEntry.slabByteOffset);
    if (pageHeader.magic != CLOD_VOXEL_PAGE_MAGIC ||
        pageLocalClusterIndex >= pageHeader.clusterCount)
    {
        return false;
    }

    const CLodVoxelClusterRecord voxelCluster = CLodLoadVoxelClusterFromPage(
        pageEntry.slabDescriptorIndex,
        pageEntry.slabByteOffset,
        pageHeader.clusterRecordsOffset,
        pageLocalClusterIndex);
    const uint cubeLocalIndex = primID;
    if (cubeLocalIndex >= voxelCluster.cubeCount)
    {
        return false;
    }
    const CLodVoxelCubeRecord cube = CLodLoadVoxelCubeFromPage(
        pageEntry.slabDescriptorIndex,
        pageEntry.slabByteOffset,
        pageHeader.cubeRecordsOffset,
        voxelCluster.firstCube + cubeLocalIndex);
    const uint3 cubeCoord = CLodDecodeVoxelCubeCoord(cube.cubeCoord);
    const float voxelWidth = voxelCluster.aabbMinAndVoxelWidth.w;
    if (voxelWidth <= 0.0f)
    {
        return false;
    }
    const float3 cubeMinObject = voxelCluster.aabbMinAndVoxelWidth.xyz + float3(cubeCoord) * (voxelWidth * 4.0f);

    float4x4 skinMatrix = IdentitySkinMatrix();
    float4x4 inverseSkinMatrix = IdentitySkinMatrix();
    if (cube.dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
    {
        skinMatrix = LoadBoneSkinMatrix(instanceData.skinningInstanceSlot, cube.dominantBoneIndex);
        inverseSkinMatrix = LoadBoneInverseSkinMatrix(instanceData.skinningInstanceSlot, cube.dominantBoneIndex);
    }
    const row_major matrix localToWorld = mul(skinMatrix, obj.model);
    const row_major matrix worldToLocal = mul(obj.modelInverse, inverseSkinMatrix);

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[0];
    const float2 winSize = float2(perFrame.screenResX, perFrame.screenResY);
    const float2 pixelUv = (float2(pixel) + 0.5f) / winSize;
    const float2 ndc = float2(pixelUv.x * 2.0f - 1.0f, 1.0f - pixelUv.y * 2.0f);
    float4 viewNear = mul(float4(ndc, 0.0f, 1.0f), cam.projectionInverse);
    viewNear.xyz /= max(viewNear.w, 1e-6f);
    float4 worldNear = mul(float4(viewNear.xyz, 1.0f), cam.viewInverse);
    const float3 worldPoint = worldNear.xyz / max(worldNear.w, 1e-6f);
    const float3 rayOriginWS = cam.positionWorldSpace.xyz;
    const float3 rayOriginObject = mul(float4(rayOriginWS, 1.0f), worldToLocal).xyz;
    const float3 localPoint = mul(float4(worldPoint, 1.0f), worldToLocal).xyz;
    const float3 rayDirObject = normalize(localPoint - rayOriginObject);
    const float3 rayOriginCube = (rayOriginObject - cubeMinObject) / voxelWidth;
    const float3 rayDirCube = rayDirObject / voxelWidth;

    float tHitCube = 0.0f;
    if (!ResolveRaycastVoxelCubeDDA(rayOriginCube, rayDirCube, cube.occupancyMask, tHitCube))
    {
        return false;
    }

    const float3 objectPosition = rayOriginObject + rayDirObject * tHitCube;
    const float3 skinnedObjectPosition = mul(float4(objectPosition, 1.0f), skinMatrix).xyz;
    const float3 worldPosition = mul(float4(objectPosition, 1.0f), localToWorld).xyz;

    const int3 cell = clamp(int3(floor((objectPosition - cubeMinObject) / voxelWidth)), int3(0, 0, 0), int3(3, 3, 3));
    const uint cellIndex = (uint)cell.x | ((uint)cell.y << 2u) | ((uint)cell.z << 4u);

    CLodVoxelAttributeSample attributeSample = CLodLoadVoxelAttributeSampleFromPage(pageEntry, pageHeader, cube, cellIndex);
    const float4 sggxAxisAndSigmas = float4(
        attributeSample.sggxAxisAndSigmas.xy,
        max(attributeSample.sggxAxisAndSigmas.zw, float2(1.0e-4f, 1.0e-4f)));
    const uint sampleSeed = CLodVoxelHash(
        localGroupId * 0x9E3779B9u ^
        localPageIndex * 0x85EBCA6Bu ^
        pageLocalClusterIndex * 0xC2B2AE35u ^
        cube.cubeCoord * 0x27D4EB2Fu ^
        primID * 0x68BC21EBu ^
        cellIndex * 0xB5297A4Du);
    const float2 sggxXi = CLodVoxelSGGXSpatiotemporalSample(pixel, perFrame.frameIndex, sampleSeed);
    const float3 viewDirObject = -rayDirObject;
    float3 normalOS = CLodVoxelSampleSGGXVNDF(
        sggxAxisAndSigmas,
        viewDirObject,
        sggxXi.x,
        sggxXi.y);
    normalOS = dot(normalOS, viewDirObject) >= 0.0f ? normalOS : -normalOS;
    normalOS = normalize(mul(normalOS, (float3x3)skinMatrix));
    const float3x3 normalMatrix = CLodAssemblyAwareNormalMatrix(obj);
    const float3 normalWS = normalize(mul(normalOS, normalMatrix));

    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[mesh.materialDataIndex];
    const uint materialFlags = materialInfo.materialFlags;
    const float4x4 viewProj = mul(cam.view, cam.projection);
    const float uvFootprint = CLodVoxelEstimateUvFootprint(
        pageEntry,
        pageHeader,
        cube,
        cell,
        attributeSample.uv,
        voxelCluster.resolution);
    const float pixelFootprint = CLodVoxelEstimatePixelFootprint(
        objectPosition,
        voxelWidth,
        localToWorld,
        viewProj,
        winSize);
    const float uvPerPixel = uvFootprint / pixelFootprint;
    const float2 voxelDUdx = float2(uvPerPixel, 0.0f);
    const float2 voxelDUdy = float2(0.0f, uvPerPixel);

    sample.linearDepth = linearDepth;
    sample.clusterIndex = visibleClusterIndex;
    sample.meshletTriangleIndex = cubeLocalIndex;
    sample.meshletIndex = pageLocalClusterIndex;
    sample.geometryGroupIndex = localGroupId;
    sample.isVoxelPath = true;
    sample.positionWS = worldPosition;
    sample.positionVS = mul(float4(worldPosition, 1.0f), cam.view).xyz;
    sample.normalWSBase = normalWS;
    sample.normalOS = normalOS;
    sample.vertexColor = 1.0f.xxx;
    sample.dpdxWS = 0.0f.xxx;
    sample.dpdyWS = 0.0f.xxx;
    sample.motionVector = ComputeClodMotionVector(
        skinnedObjectPosition,
        worldPosition,
        obj.prevModel,
        mul(cam.view, cam.unjitteredProjection),
        mul(cam.prevView, cam.prevUnjitteredProjection));
#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
    sample.materialInfo = (MaterialInfo)0;
#else
    sample.materialInfo = materialInfo;
#endif
    sample.materialFlags = materialFlags;
    sample.materialInputs = BuildVoxelMaterialInputs(
        materialInfo,
        materialFlags,
        normalWS,
        worldPosition,
        attributeSample.uv,
        voxelDUdx,
        voxelDUdy,
        saturate(attributeSample.opacity));
    return true;
}

uint3 DecodeTriangleCompact(uint triLocalIndex, MeshletResolveData d)
{
    uint triOffset = d.triangleStreamBase + d.triangleByteOffset + triLocalIndex * 3u;

    ByteAddressBuffer slab = ResourceDescriptorHeap[d.pagePoolSlabDescriptorIndex];
    uint alignedOffset = (triOffset / 4u) * 4u;
    uint firstWord = slab.Load(alignedOffset);
    uint byteOffset = triOffset % 4u;

    uint b0 = (firstWord >> (byteOffset * 8u)) & 0xFFu;
    uint b1, b2;

    if (byteOffset <= 1u)
    {
        b1 = (firstWord >> ((byteOffset + 1u) * 8u)) & 0xFFu;
        b2 = (firstWord >> ((byteOffset + 2u) * 8u)) & 0xFFu;
    }
    else if (byteOffset == 2u)
    {
        b1 = (firstWord >> ((byteOffset + 1u) * 8u)) & 0xFFu;
        uint secondWord = slab.Load(alignedOffset + 4u);
        b2 = secondWord & 0xFFu;
    }
    else
    {
        uint secondWord = slab.Load(alignedOffset + 4u);
        b1 = secondWord & 0xFFu;
        b2 = (secondWord >> 8u) & 0xFFu;
    }

    return uint3(b0, b1, b2);
}

float2 ComputeClodMotionVector(float3 posOS, float3 worldPosition, float4x4 prevModel, float4x4 unjitteredViewProj, float4x4 prevUnjitteredViewProj)
{
    float4 clipCur = mul(float4(worldPosition, 1.0f), unjitteredViewProj);
    float3 prevWorldPosition = mul(float4(posOS, 1.0f), prevModel).xyz;
    float4 clipPrev = mul(float4(prevWorldPosition, 1.0f), prevUnjitteredViewProj);

    float2 ndcCur = clipCur.xy / clipCur.w;
    float2 ndcPrev = clipPrev.xy / clipPrev.w;
    return ndcCur - ndcPrev;
}

float3 ReyesDecodeBarycentrics(float2 barycentricsUV)
{
    return ReyesPatchDomainUVToBarycentrics(barycentricsUV);
}

BarycentricDeriv ReyesComposeSourceBarycentrics(BarycentricDeriv patchBary, float3 domain0, float3 domain1, float3 domain2)
{
    BarycentricDeriv sourceBary = (BarycentricDeriv)0;
    sourceBary.m_lambda =
        domain0 * patchBary.m_lambda.x +
        domain1 * patchBary.m_lambda.y +
        domain2 * patchBary.m_lambda.z;
    sourceBary.m_ddx =
        domain0 * patchBary.m_ddx.x +
        domain1 * patchBary.m_ddx.y +
        domain2 * patchBary.m_ddx.z;
    sourceBary.m_ddy =
        domain0 * patchBary.m_ddy.x +
        domain1 * patchBary.m_ddy.y +
        domain2 * patchBary.m_ddy.z;
    return sourceBary;
}

bool ResolveClodCommonSampleFromVisKeyWithFace(uint64_t vis, uint2 pixel, bool isBackface, out ClodResolvedCommonSample sample)
{
    sample = (ClodResolvedCommonSample)0;

    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        return false;
    }

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[0];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera cam = cameras[perFrame.mainCameraIndex];

    float depth;
    uint clusterIndex;
    uint meshletTriangleIndex;
    UnpackVisKey(vis, depth, clusterIndex, meshletTriangleIndex);

#if defined(PSO_CLOD_REYES_PATCH)
    const bool isReyesPatch = true;
#else
    const bool isReyesPatch = false;
#endif
    float3 patchDomain0 = float3(1.0f, 0.0f, 0.0f);
    float3 patchDomain1 = float3(0.0f, 1.0f, 0.0f);
    float3 patchDomain2 = float3(0.0f, 0.0f, 1.0f);
    float3 microTrianglePatchDomain0 = patchDomain0;
    float3 microTrianglePatchDomain1 = patchDomain1;
    float3 microTrianglePatchDomain2 = patchDomain2;
    uint reyesMicroTriangleIndex = meshletTriangleIndex;
#if defined(PSO_CLOD_REYES_PATCH)
    if (clusterIndex < VISBUF_REYES_PATCH_INDEX_BASE || VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return false;
    }
    if (clusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE && VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
        if (VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX == 0xFFFFFFFFu ||
            VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX == 0xFFFFFFFFu ||
            VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
        {
            return false;
        }

        StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[VISBUF_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
        StructuredBuffer<uint> tessTableVertices = ResourceDescriptorHeap[VISBUF_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX];
        StructuredBuffer<uint> tessTableTriangles = ResourceDescriptorHeap[VISBUF_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX];
        CLodReyesDiceQueueEntry diceEntry = diceQueue[clusterIndex - VISBUF_REYES_PATCH_INDEX_BASE];
        clusterIndex = diceEntry.visibleClusterIndex;
        meshletTriangleIndex = diceEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
        patchDomain0 = ReyesDecodeBarycentrics(diceEntry.domainVertex0UV);
        patchDomain1 = ReyesDecodeBarycentrics(diceEntry.domainVertex1UV);
        patchDomain2 = ReyesDecodeBarycentrics(diceEntry.domainVertex2UV);

        const uint microTriangleCount = ReyesGetDicePatchMicroTriangleCount(tessTableConfigs, diceEntry);
        if (reyesMicroTriangleIndex >= microTriangleCount)
        {
            return false;
        }

        ReyesDecodeMicroTrianglePatchDomain(
            tessTableConfigs,
            tessTableVertices,
            tessTableTriangles,
            reyesMicroTriangleIndex,
            diceEntry,
            microTrianglePatchDomain0,
            microTrianglePatchDomain1,
            microTrianglePatchDomain2);
    }
#else
    if (clusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE)
    {
        return false;
    }
#endif

    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedVisibleCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
#if defined(PSO_CLOD_VOXEL)
    if (!CLodVisibleClusterIsVoxel(packedVisibleCluster))
    {
        return false;
    }
    return ResolveClodVoxelCommonSampleFromPackedCluster(
        packedVisibleCluster,
        clusterIndex,
        meshletTriangleIndex,
        depth,
        pixel,
        cam,
        sample);
#else
    if (CLodVisibleClusterIsVoxel(packedVisibleCluster))
    {
        return false;
    }
#endif

    uint assemblyTransformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
    if (VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> visibleClusterTransformIndices =
            ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
        assemblyTransformIndex = visibleClusterTransformIndices[clusterIndex];
    }

    MeshletResolveData md = LoadMeshletResolveData_Wave(clusterIndex);
    if (meshletTriangleIndex >= md.triangleCount)
    {
        return false;
    }

    uint3 triIdx = DecodeTriangleCompact(meshletTriangleIndex, md);

    if (isBackface)
    {
        const uint triTmp = triIdx.y;
        triIdx.y = triIdx.z;
        triIdx.z = triTmp;

        const float3 patchDomainTmp = patchDomain1;
        patchDomain1 = patchDomain2;
        patchDomain2 = patchDomainTmp;

        const float3 microTrianglePatchDomainTmp = microTrianglePatchDomain1;
        microTrianglePatchDomain1 = microTrianglePatchDomain2;
        microTrianglePatchDomain2 = microTrianglePatchDomainTmp;
    }

    float3 p0 = DecodeCompressedPosition(triIdx.x, md);
    float3 p1 = DecodeCompressedPosition(triIdx.y, md);
    float3 p2 = DecodeCompressedPosition(triIdx.z, md);
    float3 n0 = DecodeCompressedNormal(triIdx.x, md);
    float3 n1 = DecodeCompressedNormal(triIdx.y, md);
    float3 n2 = DecodeCompressedNormal(triIdx.z, md);
    float4 t0 = DecodeCompressedTangentFrame(triIdx.x, n0, md);
    float4 t1 = DecodeCompressedTangentFrame(triIdx.y, n1, md);
    float4 t2 = DecodeCompressedTangentFrame(triIdx.z, n2, md);
    float3 c0 = 1.0f.xxx;
    float3 c1 = 1.0f.xxx;
    float3 c2 = 1.0f.xxx;
#if defined(PSO_CLOD_VERTEX_COLOR)
    const bool hasVertexColor = true;
    c0 = DecodeCompressedColor(triIdx.x, md);
    c1 = DecodeCompressedColor(triIdx.y, md);
    c2 = DecodeCompressedColor(triIdx.z, md);
#else
    const bool hasVertexColor = (md.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u;
    if (hasVertexColor)
    {
        c0 = DecodeCompressedColor(triIdx.x, md);
        c1 = DecodeCompressedColor(triIdx.y, md);
        c2 = DecodeCompressedColor(triIdx.z, md);
    }
#endif
#if defined(PSO_CLOD_SKINNING)
    ApplyClodSkinningToFrame(triIdx.x, md, p0, n0, t0);
    ApplyClodSkinningToFrame(triIdx.y, md, p1, n1, t1);
    ApplyClodSkinningToFrame(triIdx.z, md, p2, n2, t2);
#endif

    PerObjectBuffer obj = LoadInstanceTransformForDrawWithAssemblyTransform(md.objAndMesh.x, assemblyTransformIndex);
#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
    StructuredBuffer<MaterialEvalInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialEvalDataBuffer)];
    MaterialEvalInfo materialInfo = materialDataBuffer[md.materialDataIndex];
#else
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[md.materialDataIndex];
#endif
    uint materialFlags = materialInfo.materialFlags;
    const float reyesObjectNormalMapBlend = isReyesPatch
        ? saturate(asfloat(VISBUF_REYES_OBJECT_NORMAL_MAP_BLEND_AS_UINT))
        : 1.0f;
    const bool useReyesGeometricNormal = isReyesPatch && reyesObjectNormalMapBlend < 0.999f;
    if (isReyesPatch && reyesObjectNormalMapBlend <= 1.0e-4f)
    {
        materialFlags &= ~MATERIAL_NORMAL_MAP;
        materialInfo.materialFlags = materialFlags;
    }
    float4x4 viewProj = mul(cam.view, cam.projection);
    float4x4 objectToClip = mul(obj.model, viewProj);
    float4 clip0 = mul(float4(p0, 1.0f), objectToClip);
    float4 clip1 = mul(float4(p1, 1.0f), objectToClip);
    float4 clip2 = mul(float4(p2, 1.0f), objectToClip);
    float3 evalPos0 = p0;
    float3 evalPos1 = p1;
    float3 evalPos2 = p2;
    BarycentricDeriv sourceDerivativeBary = (BarycentricDeriv)0;
    bool useSourceDerivativeBary = false;

    if (isReyesPatch)
    {
        const float3 sourcePatchBary0 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain0, patchDomain0, patchDomain1, patchDomain2);
        const float3 sourcePatchBary1 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain1, patchDomain0, patchDomain1, patchDomain2);
        const float3 sourcePatchBary2 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain2, patchDomain0, patchDomain1, patchDomain2);

        float3 patchPos0 = p0 * sourcePatchBary0.x + p1 * sourcePatchBary0.y + p2 * sourcePatchBary0.z;
        float3 patchPos1 = p0 * sourcePatchBary1.x + p1 * sourcePatchBary1.y + p2 * sourcePatchBary1.z;
        float3 patchPos2 = p0 * sourcePatchBary2.x + p1 * sourcePatchBary2.y + p2 * sourcePatchBary2.z;

        if (materialInfo.geometricDisplacementEnabled != 0u)
        {
            CullingCameraInfo reyesCamera = (CullingCameraInfo)0;
            reyesCamera.positionWorldSpace = cam.positionWorldSpace;
            reyesCamera.projX = cam.projection._11;
            reyesCamera.projY = cam.projection._22;
            reyesCamera.zNear = cam.zNear;
            reyesCamera.isOrtho = cam.isOrtho ? 1u : 0u;
            reyesCamera.viewportWidth = float(cam.depthResX);
            reyesCamera.viewportHeight = float(cam.depthResY);
            reyesCamera.reyesDiceRatePixels = 1.0f;
            reyesCamera.viewRightWorld = float4(cam.viewInverse._11, cam.viewInverse._12, cam.viewInverse._13, 0.0f);
            reyesCamera.viewUpWorld = float4(cam.viewInverse._21, cam.viewInverse._22, cam.viewInverse._23, 0.0f);
            reyesCamera.viewForwardWorld = float4(cam.viewInverse._31, cam.viewInverse._32, cam.viewInverse._33, 0.0f);
            reyesCamera.viewProjection = mul(cam.view, cam.projection);
            const row_major matrix objectToView = mul(obj.model, cam.view);
            const float patchDepth = max(
                (-mul(float4(p0, 1.0f), objectToView).z
                + -mul(float4(p1, 1.0f), objectToView).z
                + -mul(float4(p2, 1.0f), objectToView).z) / 3.0f,
                max(cam.zNear, 1.0e-3f));
            const float2 heightUv0 = DecodeCompressedUV(triIdx.x, materialInfo.heightUvSetIndex, md);
            const float2 heightUv1 = DecodeCompressedUV(triIdx.y, materialInfo.heightUvSetIndex, md);
            const float2 heightUv2 = DecodeCompressedUV(triIdx.z, materialInfo.heightUvSetIndex, md);
            const float3 patchNormal0 = normalize(n0 * sourcePatchBary0.x + n1 * sourcePatchBary0.y + n2 * sourcePatchBary0.z);
            const float3 patchNormal1 = normalize(n0 * sourcePatchBary1.x + n1 * sourcePatchBary1.y + n2 * sourcePatchBary1.z);
            const float3 patchNormal2 = normalize(n0 * sourcePatchBary2.x + n1 * sourcePatchBary2.y + n2 * sourcePatchBary2.z);
            const float2 patchUv0 = heightUv0 * sourcePatchBary0.x + heightUv1 * sourcePatchBary0.y + heightUv2 * sourcePatchBary0.z;
            const float2 patchUv1 = heightUv0 * sourcePatchBary1.x + heightUv1 * sourcePatchBary1.y + heightUv2 * sourcePatchBary1.z;
            const float2 patchUv2 = heightUv0 * sourcePatchBary2.x + heightUv1 * sourcePatchBary2.y + heightUv2 * sourcePatchBary2.z;
            const float3 patchPos0WS = mul(float4(patchPos0, 1.0f), obj.model).xyz;
            const float3 patchPos1WS = mul(float4(patchPos1, 1.0f), obj.model).xyz;
            const float3 patchPos2WS = mul(float4(patchPos2, 1.0f), obj.model).xyz;
            float2 dUVdx = 0.0f.xx;
            float2 dUVdy = 0.0f.xx;
            const bool useUvDerivatives = ReyesEstimateUvDerivativesFromPatch(
                reyesCamera,
                patchPos0WS,
                patchPos1WS,
                patchPos2WS,
                patchUv0,
                patchUv1,
                patchUv2,
                dUVdx,
                dUVdy);
            ReyesApplyGeometricDisplacement3(
                materialInfo,
                patchPos0,
                patchPos1,
                patchPos2,
                patchPos0WS,
                patchPos1WS,
                patchPos2WS,
                patchNormal0,
                patchNormal1,
                patchNormal2,
                patchUv0,
                patchUv1,
                patchUv2,
                reyesCamera,
                patchDepth,
                useUvDerivatives,
                dUVdx,
                dUVdy,
                perFrame.heightFadeStartDistance,
                perFrame.heightFadeEndDistance);
        }

        evalPos0 = patchPos0;
        evalPos1 = patchPos1;
        evalPos2 = patchPos2;
        clip0 = mul(float4(evalPos0, 1.0f), objectToClip);
        clip1 = mul(float4(evalPos1, 1.0f), objectToClip);
        clip2 = mul(float4(evalPos2, 1.0f), objectToClip);

        if (materialInfo.geometricDisplacementEnabled != 0u)
        {
            materialFlags &= ~MATERIAL_PARALLAX;
            materialInfo.materialFlags = materialFlags;
        }
    }

    float2 winSize = float2(perFrame.screenResX, perFrame.screenResY);
    float2 pixelUv = (float2(pixel) + 0.5f) / winSize;
    float2 pixelNdc = float2(pixelUv.x * 2.0f - 1.0f, (1.0f - pixelUv.y) * 2.0f - 1.0f);

    BarycentricDeriv bary = CalcFullBary(clip0, clip1, clip2, pixelNdc, winSize);
    BarycentricDeriv positionBary = bary;
    if (isReyesPatch)
    {
        const float3 sourcePatchBary0 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain0, patchDomain0, patchDomain1, patchDomain2);
        const float3 sourcePatchBary1 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain1, patchDomain0, patchDomain1, patchDomain2);
        const float3 sourcePatchBary2 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain2, patchDomain0, patchDomain1, patchDomain2);
        sourceDerivativeBary = ReyesComposeSourceBarycentrics(bary, sourcePatchBary0, sourcePatchBary1, sourcePatchBary2);
        useSourceDerivativeBary = true;
        bary = sourceDerivativeBary;
    }

#if defined(VISUTIL_DOUBLE_SIDED_GBUFFER_RESOLVE)
    const bool clodGBufferResolveBackface = ClodBarycentricDerivativesAreBackFacing(bary);
#endif

    float3 interpPosX = InterpolateWithDeriv(positionBary, evalPos0.x, evalPos1.x, evalPos2.x);
    float3 interpPosY = InterpolateWithDeriv(positionBary, evalPos0.y, evalPos1.y, evalPos2.y);
    float3 interpPosZ = InterpolateWithDeriv(positionBary, evalPos0.z, evalPos1.z, evalPos2.z);

    float3 posOS = float3(interpPosX.x, interpPosY.x, interpPosZ.x);
    float3 dpdxOS = float3(interpPosX.y, interpPosY.y, interpPosZ.y);
    float3 dpdyOS = float3(interpPosX.z, interpPosY.z, interpPosZ.z);
    if (useSourceDerivativeBary)
    {
        float3 sourceInterpPosX = InterpolateWithDeriv(sourceDerivativeBary, p0.x, p1.x, p2.x);
        float3 sourceInterpPosY = InterpolateWithDeriv(sourceDerivativeBary, p0.y, p1.y, p2.y);
        float3 sourceInterpPosZ = InterpolateWithDeriv(sourceDerivativeBary, p0.z, p1.z, p2.z);
        dpdxOS = float3(sourceInterpPosX.y, sourceInterpPosY.y, sourceInterpPosZ.y);
        dpdyOS = float3(sourceInterpPosX.z, sourceInterpPosY.z, sourceInterpPosZ.z);
    }

    float3 worldPosition = mul(float4(posOS, 1.0f), obj.model).xyz;

    float3x3 model3x3 = (float3x3)obj.model;
    float3 dpdx = 0.0f.xxx;
    float3 dpdy = 0.0f.xxx;
    uint derivativeMaterialFlags = MATERIAL_NORMAL_MAP | MATERIAL_PARALLAX;
#if defined(PSO_TERRAIN)
    derivativeMaterialFlags |= MATERIAL_TERRAIN;
#endif
    if ((materialFlags & derivativeMaterialFlags) != 0u)
    {
        dpdx = mul(dpdxOS, model3x3);
        dpdy = mul(dpdyOS, model3x3);
    }

    float interpNX = InterpolateWithDeriv(bary, n0.x, n1.x, n2.x).x;
    float interpNY = InterpolateWithDeriv(bary, n0.y, n1.y, n2.y).x;
    float interpNZ = InterpolateWithDeriv(bary, n0.z, n1.z, n2.z).x;
    float3 normalOS = normalize(float3(interpNX, interpNY, interpNZ));
    float3 vertexColor = 1.0f.xxx;
    if (hasVertexColor)
    {
        vertexColor = float3(
            InterpolateWithDeriv(bary, c0.x, c1.x, c2.x).x,
            InterpolateWithDeriv(bary, c0.y, c1.y, c2.y).x,
            InterpolateWithDeriv(bary, c0.z, c1.z, c2.z).x);
    }
    float3x3 normalMatrix = CLodAssemblyAwareNormalMatrix(obj);
    const float3 interpolatedNormalOS = normalOS;
    const float3 interpolatedWorldNormal = normalize(mul(interpolatedNormalOS, normalMatrix));
    if (useReyesGeometricNormal)
    {
        const float3 geometricNormalRawOS = cross(evalPos1 - evalPos0, evalPos2 - evalPos0);
        if (dot(geometricNormalRawOS, geometricNormalRawOS) > 1.0e-16f)
        {
            const float3 geometricNormalOS = normalize(geometricNormalRawOS);
            normalOS = dot(geometricNormalOS, normalOS) < 0.0f ? -geometricNormalOS : geometricNormalOS;
        }
    }
    float3 worldNormal = normalize(mul(normalOS, normalMatrix));
    float4 tangentWS = float4(1.0f, 0.0f, 0.0f, 0.0f);
    if ((md.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) != 0u)
    {
        float interpTX = InterpolateWithDeriv(bary, t0.x, t1.x, t2.x).x;
        float interpTY = InterpolateWithDeriv(bary, t0.y, t1.y, t2.y).x;
        float interpTZ = InterpolateWithDeriv(bary, t0.z, t1.z, t2.z).x;
        float interpTW = InterpolateWithDeriv(bary, t0.w, t1.w, t2.w).x;
        tangentWS = float4(normalize(mul(float3(interpTX, interpTY, interpTZ), normalMatrix)), interpTW < 0.0f ? -1.0f : 1.0f);
    }

    MaterialInputs materialInputs;
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    float2 materialDebugUv = 0.0f.xx;
    float materialDebugUvValid = 0.0f;
    float2 materialDebugUvDerivative = 0.0f.xx;
    float3 reyesDebugSourceBarycentrics = bary.m_lambda;
#endif
#if defined(PSO_TERRAIN)
    if (!isReyesPatch)
    {
        float3 terrainDpdxWS;
        float3 terrainDpdyWS;
        if (CLodResolveEstimateTerrainTangentPlaneDerivatives(cam, pixel, winSize, worldPosition, worldNormal, terrainDpdxWS, terrainDpdyWS))
        {
            dpdx = terrainDpdxWS;
            dpdy = terrainDpdyWS;
        }
    }

    materialInputs = (MaterialInputs)0;
    InitializeMaterialSelectedMipDebug(materialInputs);
    ApplyTerrainMaterial(materialInfo, worldPosition, dpdx, dpdy, worldNormal, vertexColor, materialInputs);
    if (useReyesGeometricNormal && (materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        const float reyesTerrainNormalBlend = saturate(asfloat(VISBUF_REYES_TERRAIN_NORMAL_BLEND_AS_UINT));
        if (reyesTerrainNormalBlend > 1.0e-4f)
        {
            float3 lowFrequencyTerrainNormalTS;
            float3 lowFrequencyTerrainNormalWS;
            TerrainRvtMaterialSample rvtBaseSample = (TerrainRvtMaterialSample)0;
            rvtBaseSample.fallbackReason = materialInputs.terrainRvtFallbackReason;
            rvtBaseSample.requestedMip = materialInputs.terrainRvtRequestedMip;
            rvtBaseSample.local = materialInputs.terrainRvtLocal;
            rvtBaseSample.terrainClipCount = materialInputs.terrainRvtTerrainClipCount;
            if (TerrainRvtTrySampleNormalBiasedFromMaterialSample(
                materialInfo.terrainSetIndex,
                rvtBaseSample,
                interpolatedWorldNormal,
                VISBUF_REYES_TERRAIN_NORMAL_MIP_BIAS,
                lowFrequencyTerrainNormalTS,
                lowFrequencyTerrainNormalWS))
            {
                const float highFrequencyNormalLengthSq = dot(materialInputs.normalWS, materialInputs.normalWS);
                const float3 highFrequencyTerrainNormalWS = highFrequencyNormalLengthSq > 1.0e-8f
                    ? materialInputs.normalWS * rsqrt(highFrequencyNormalLengthSq)
                    : worldNormal;
                materialInputs.normalWS = normalize(lerp(highFrequencyTerrainNormalWS, lowFrequencyTerrainNormalWS, reyesTerrainNormalBlend));
            }
        }
    }
#else
    const bool isObjectTriplanarMaterial = (materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u;
    const bool needsObjectSpaceNormalUv =
        (materialFlags & (MATERIAL_NORMAL_MAP | MATERIAL_OBJECT_SPACE_NORMAL_MAP)) ==
        (MATERIAL_NORMAL_MAP | MATERIAL_OBJECT_SPACE_NORMAL_MAP);
#if defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    const bool needsMaterialDebugUv = false;
#else
    const bool needsMaterialDebugUv = true;
#endif
    const bool needsMaterialUvData = !isObjectTriplanarMaterial || needsObjectSpaceNormalUv || needsMaterialDebugUv;
    MaterialUvCache uvCache = (MaterialUvCache)0;
    MaterialUvBindings uvBindings;
    if (needsMaterialUvData)
    {
        BuildClodMaterialUvData(materialInfo, materialFlags, md, triIdx, bary, uvCache, uvBindings);
    }
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    if (uvCache.count > 0u)
    {
        materialDebugUv = uvCache.samples[0].uv;
        materialDebugUvValid = 1.0f;
        materialDebugUvDerivative = float2(
            length(uvCache.samples[0].dUVdx),
            length(uvCache.samples[0].dUVdy));
    }
#endif
#if defined(VISUTIL_SPECIALIZED_MATERIAL_EVAL)
    if (isObjectTriplanarMaterial)
    {
        SampleObjectTriplanarStochasticMaterial(
            worldNormal,
            normalOS,
            posOS,
            vertexColor,
            dpdxOS,
            dpdyOS,
            normalMatrix,
            materialInfo,
            materialFlags,
            materialInputs);
    }
    else
    {
        SampleMaterialEvalFromUvCache(
            uvCache,
            uvBindings,
            worldNormal,
            worldPosition,
            vertexColor,
            materialInfo,
            materialFlags,
            dpdx,
            dpdy,
            materialInputs);
    }
#else
    if (isObjectTriplanarMaterial)
    {
        SampleObjectTriplanarStochasticMaterial(
            worldNormal,
            normalOS,
            posOS,
            vertexColor,
            dpdxOS,
            dpdyOS,
            normalMatrix,
            materialInfo,
            materialFlags,
            materialInputs);
    }
    else
    {
        SampleMaterialFromUvCacheRuntimeWithVertexTangent(
            uvCache,
            uvBindings,
            worldNormal,
            tangentWS,
            worldPosition,
            vertexColor,
            materialInfo,
            materialFlags,
            dpdx,
            dpdy,
            materialInputs);
    }
    #endif
#endif

#if !defined(PSO_TERRAIN)
    if (needsObjectSpaceNormalUv)
    {
#if defined(PSO_NORMAL_MAP)
        const MaterialUvSample objectNormalUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_NORMAL);
        Texture2D<float4> objectNormalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.normalTextureIndex)];
        SamplerState objectNormalSamplerState = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.normalSamplerIndex)];
        const float4 objectNormalSample = SampleMaterialTexture2DGrad(
            objectNormalTexture,
            objectNormalSamplerState,
            materialInfo.normalStreamingTextureID,
            objectNormalUv.uv,
            objectNormalUv.dUVdx,
            objectNormalUv.dUVdy,
            materialInputs);
        const float3 objectNormalOS = DecodeMaterialNormalSample(objectNormalSample, materialInfo.normalChannels, materialFlags);
        materialInputs.normalWS = normalize(mul(objectNormalOS, normalMatrix));
#endif
    }

    if (useReyesGeometricNormal)
    {
        const float normalMapLengthSq = dot(materialInputs.normalWS, materialInputs.normalWS);
        const float3 materialNormalWS = normalMapLengthSq > 1.0e-8f
            ? materialInputs.normalWS * rsqrt(normalMapLengthSq)
            : worldNormal;
        materialInputs.normalWS = normalize(lerp(worldNormal, materialNormalWS, reyesObjectNormalMapBlend));
    }
#endif

    float3 positionVS = mul(float4(worldPosition, 1.0f), cam.view).xyz;

    sample.linearDepth = depth;
    sample.clusterIndex = clusterIndex;
    sample.meshletTriangleIndex = meshletTriangleIndex;
    sample.meshletIndex = md.drawcallAndMeshlet.y;
    sample.geometryGroupIndex = CLodVisibleClusterGroupID(packedVisibleCluster);
    sample.positionWS = worldPosition;
    sample.positionVS = positionVS;
    sample.normalWSBase = worldNormal;
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    sample.normalOS = normalOS;
#endif
    sample.vertexColor = vertexColor;
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    sample.materialDebugUv = materialDebugUv;
    sample.materialDebugUvValid = materialDebugUvValid;
    sample.materialDebugUvDerivative = materialDebugUvDerivative;
    sample.reyesDebugSourceBarycentrics = reyesDebugSourceBarycentrics;
#endif
    sample.dpdxWS = dpdx;
    sample.dpdyWS = dpdy;
    sample.motionVector = ComputeClodMotionVector(
        posOS,
        worldPosition,
        obj.prevModel,
        mul(cam.view, cam.unjitteredProjection),
        mul(cam.prevView, cam.prevUnjitteredProjection));
#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
    sample.materialInfo = (MaterialInfo)0;
#else
    sample.materialInfo = materialInfo;
#endif
    sample.materialFlags = materialFlags;
    sample.materialInputs = materialInputs;
#if defined(VISUTIL_DOUBLE_SIDED_GBUFFER_RESOLVE)
    if (clodGBufferResolveBackface)
    {
        sample.normalWSBase = -sample.normalWSBase;
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
        sample.normalOS = -sample.normalOS;
#endif
        sample.materialInputs.normalWS = -sample.materialInputs.normalWS;
    }
#endif
    return true;
}

bool ResolveClodSampleFromVisKeyWithFace(uint64_t vis, uint2 pixel, bool isBackface, out ClodResolvedSample sample)
{
    sample = (ClodResolvedSample)0;

    ClodResolvedCommonSample resolvedSample;
    if (!ResolveClodCommonSampleFromVisKeyWithFace(vis, pixel, isBackface, resolvedSample))
    {
        return false;
    }

    sample.pixelCoords = pixel;
    sample.linearDepth = resolvedSample.linearDepth;
    sample.clusterIndex = resolvedSample.clusterIndex;
    sample.meshletTriangleIndex = resolvedSample.meshletTriangleIndex;
    sample.meshletIndex = resolvedSample.meshletIndex;
    sample.positionWS = resolvedSample.positionWS;
    sample.positionVS = resolvedSample.positionVS;
    sample.normalWSBase = resolvedSample.normalWSBase;
    sample.normalOS = resolvedSample.normalOS;
    sample.vertexColor = resolvedSample.vertexColor;
    sample.dpdxWS = resolvedSample.dpdxWS;
    sample.dpdyWS = resolvedSample.dpdyWS;
    sample.motionVector = resolvedSample.motionVector;
    sample.materialInfo = resolvedSample.materialInfo;
    sample.materialFlags = resolvedSample.materialFlags;
    sample.materialInputs = resolvedSample.materialInputs;
    return true;
}

bool ResolveClodGBufferSampleFromVisKeyWithFace(uint64_t vis, uint2 pixel, bool isBackface, out ClodResolvedGBufferSample sample)
{
    sample = (ClodResolvedGBufferSample)0;

    ClodResolvedCommonSample resolvedSample;
    if (!ResolveClodCommonSampleFromVisKeyWithFace(vis, pixel, isBackface, resolvedSample))
    {
        return false;
    }

    sample.meshletIndex = resolvedSample.meshletIndex;
    sample.geometryGroupIndex = resolvedSample.geometryGroupIndex;
    sample.isVoxelPath = resolvedSample.isVoxelPath;
    sample.normalOS = resolvedSample.normalOS;
    sample.materialDebugUv = resolvedSample.materialDebugUv;
    sample.materialDebugUvValid = resolvedSample.materialDebugUvValid;
    sample.materialDebugUvDerivative = resolvedSample.materialDebugUvDerivative;
    sample.reyesDebugSourceBarycentrics = resolvedSample.reyesDebugSourceBarycentrics;
    sample.materialDebugFlags = resolvedSample.materialFlags;
    sample.motionVector = resolvedSample.motionVector;
    sample.materialInputs = resolvedSample.materialInputs;
    return true;
}

bool ResolveClodShadingSampleFromVisKeyWithFace(uint64_t vis, uint2 pixel, bool isBackface, out ClodShadingSample sample)
{
    ClodResolvedSample resolvedSample;
    if (!ResolveClodSampleFromVisKeyWithFace(vis, pixel, isBackface, resolvedSample))
    {
        sample = (ClodShadingSample)0;
        return false;
    }

    sample.linearDepth = resolvedSample.linearDepth;
    sample.positionWS = resolvedSample.positionWS;
    sample.positionVS = resolvedSample.positionVS;
    sample.materialFlags = resolvedSample.materialFlags;
    sample.materialInputs = resolvedSample.materialInputs;
    return true;
}

bool ResolveClodShadingSampleFromVisKey(uint64_t vis, uint2 pixel, out ClodShadingSample sample)
{
    return ResolveClodShadingSampleFromVisKeyWithFace(vis, pixel, false, sample);
}

bool ResolveClodGBufferColorSampleFromVisKeyWithFace(uint64_t vis, uint2 pixel, bool isBackface, out ClodGBufferColorSample sample)
{
    ClodResolvedCommonSample resolvedSample;
    if (!ResolveClodCommonSampleFromVisKeyWithFace(vis, pixel, isBackface, resolvedSample))
    {
        sample = (ClodGBufferColorSample)0;
        return false;
    }

    sample.motionVector = resolvedSample.motionVector;
    sample.materialInputs = resolvedSample.materialInputs;
    return true;
}

bool ResolveClodGBufferColorSampleFromVisKey(uint64_t vis, uint2 pixel, out ClodGBufferColorSample sample)
{
    return ResolveClodGBufferColorSampleFromVisKeyWithFace(vis, pixel, false, sample);
}

bool ResolveClodGBufferDebugSampleFromVisKeyWithFace(uint64_t vis, uint2 pixel, bool isBackface, out ClodGBufferDebugSample sample)
{
    ClodResolvedGBufferSample resolvedSample;
    if (!ResolveClodGBufferSampleFromVisKeyWithFace(vis, pixel, isBackface, resolvedSample))
    {
        sample = (ClodGBufferDebugSample)0;
        return false;
    }

    sample.meshletIndex = resolvedSample.meshletIndex;
    sample.geometryGroupIndex = resolvedSample.geometryGroupIndex;
    sample.isVoxelPath = resolvedSample.isVoxelPath;
    sample.normalOS = resolvedSample.normalOS;
    sample.materialDebugUv = resolvedSample.materialDebugUv;
    sample.materialDebugUvValid = resolvedSample.materialDebugUvValid;
    sample.materialDebugUvDerivative = resolvedSample.materialDebugUvDerivative;
    sample.reyesDebugSourceBarycentrics = resolvedSample.reyesDebugSourceBarycentrics;
    sample.materialDebugFlags = resolvedSample.materialDebugFlags;
    sample.motionVector = resolvedSample.motionVector;
    sample.materialInputs = resolvedSample.materialInputs;
    return true;
}

bool ResolveClodGBufferDebugSampleFromVisKey(uint64_t vis, uint2 pixel, out ClodGBufferDebugSample sample)
{
    return ResolveClodGBufferDebugSampleFromVisKeyWithFace(vis, pixel, false, sample);
}

bool ResolveClodSampleFromVisKey(uint64_t vis, uint2 pixel, out ClodResolvedSample sample)
{
    return ResolveClodSampleFromVisKeyWithFace(vis, pixel, false, sample);
}

#endif // __CLOD_RESOLVE_COMMON_HLSLI__
