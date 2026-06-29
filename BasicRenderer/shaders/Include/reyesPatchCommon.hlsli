#ifndef __REYES_PATCH_COMMON_HLSLI__
#define __REYES_PATCH_COMMON_HLSLI__

#include "include/dynamicSwizzle.hlsli"
#include "include/terrainCommon.hlsli"
#include "include/clodReyesTransition.hlsli"

static const uint REYES_PATCH_BARYCENTRIC_COORD_MAX = (1u << 15u);
static const float REYES_PATCH_BARYCENTRIC_COORD_SCALE = float(REYES_PATCH_BARYCENTRIC_COORD_MAX);
static const float REYES_BARYCENTRIC_COORD_SCALE = REYES_PATCH_BARYCENTRIC_COORD_SCALE;
static const float REYES_SCREEN_SCALE_REFERENCE = 1080.0f;
static const float REYES_DICE_RATE_PIXELS = 1.0f;
static const float REYES_PROJECTED_PIXEL_TO_TESS_FACTOR_SCALE = 0.5f / REYES_DICE_RATE_PIXELS;
static const uint REYES_TESS_TABLE_LOOKUP_SIZE = 16u;
static const uint REYES_TESS_TABLE_MAX_SEGMENTS = 11u;
static const uint REYES_TESS_TABLE_FLIP_BIT = 1u << 15u;
static const uint CLodReyesMaxVisibilityMicroTrianglesPerPatch = 128u;
static const uint CLodReyesRasterBatchMicroTriangleCount = 16u;
static const uint CLodReyesHardwareRasterPackedEntryCount = 5u;
static const uint CLodReyesHardwareRasterMaxPackedMicroTriangles =
    CLodReyesHardwareRasterPackedEntryCount * CLodReyesRasterBatchMicroTriangleCount;
static const uint OBJECT_SURFACE_SAMPLING_ATLAS_BAKED_HEIGHT = 2u;

float ReyesViewportHeightPixels(CullingCameraInfo camera)
{
    return camera.viewportHeight > 0.0f ? camera.viewportHeight : REYES_SCREEN_SCALE_REFERENCE;
}

float ReyesDiceRatePixels(CullingCameraInfo camera)
{
    return camera.reyesDiceRatePixels > 0.0f ? camera.reyesDiceRatePixels : REYES_DICE_RATE_PIXELS;
}

float3 ReyesComputeVisibilityEdgeTessFactors(float3 worldPosition0, float3 worldPosition1, float3 worldPosition2, CullingCameraInfo camera)
{
    const float distance01 = max(camera.zNear, min(length(worldPosition0 - camera.positionWorldSpace.xyz), length(worldPosition1 - camera.positionWorldSpace.xyz)));
    const float distance12 = max(camera.zNear, min(length(worldPosition1 - camera.positionWorldSpace.xyz), length(worldPosition2 - camera.positionWorldSpace.xyz)));
    const float distance20 = max(camera.zNear, min(length(worldPosition2 - camera.positionWorldSpace.xyz), length(worldPosition0 - camera.positionWorldSpace.xyz)));

    const float edge01 = length(worldPosition0 - worldPosition1);
    const float edge12 = length(worldPosition1 - worldPosition2);
    const float edge20 = length(worldPosition2 - worldPosition0);

    const float scale = camera.projY * ReyesViewportHeightPixels(camera) * (0.5f / ReyesDiceRatePixels(camera));
    return max(float3(1.0f, 1.0f, 1.0f), float3(edge01 / distance01, edge12 / distance12, edge20 / distance20) * scale);
}

float3 ReyesDecodePatchBarycentrics(uint encoded)
{
    float u = (float) (encoded & 0xFFFFu) / REYES_PATCH_BARYCENTRIC_COORD_SCALE;
    float v = (float) (encoded >> 16u) / REYES_PATCH_BARYCENTRIC_COORD_SCALE;
    return float3(1.0f - u - v, u, v);
}

float3 ReyesPatchDomainUVToBarycentrics(float2 barycentricsUV)
{
    return float3(1.0f - barycentricsUV.x - barycentricsUV.y, barycentricsUV.x, barycentricsUV.y);
}

float2 ReyesPatchBarycentricsToUV(float3 barycentrics)
{
    return barycentrics.yz;
}

uint ReyesEncodePatchBarycentrics(float3 barycentrics)
{
    uint3 quantized = uint3(saturate(barycentrics) * REYES_PATCH_BARYCENTRIC_COORD_SCALE + 0.5f);
    if (quantized.x > max(quantized.y, quantized.z))
    {
        quantized.x = REYES_PATCH_BARYCENTRIC_COORD_MAX - quantized.y - quantized.z;
    }
    else if (quantized.y > quantized.z)
    {
        quantized.y = REYES_PATCH_BARYCENTRIC_COORD_MAX - quantized.x - quantized.z;
    }
    else
    {
        quantized.z = REYES_PATCH_BARYCENTRIC_COORD_MAX - quantized.x - quantized.y;
    }

    return quantized.y | (quantized.z << 16u);
}

uint ReyesClampTessTableFactor(uint factor)
{
    return clamp(factor, 1u, REYES_TESS_TABLE_MAX_SEGMENTS);
}

bool ReyesDomainTupleLexicographicallyLess(uint lhs0, uint lhs1, uint lhs2, uint rhs0, uint rhs1, uint rhs2)
{
    if (lhs0 != rhs0)
    {
        return lhs0 < rhs0;
    }
    if (lhs1 != rhs1)
    {
        return lhs1 < rhs1;
    }
    return lhs2 < rhs2;
}

bool ReyesDomainTupleLexicographicallyLess(float2 lhs0, float2 lhs1, float2 lhs2, float2 rhs0, float2 rhs1, float2 rhs2)
{
    return ReyesDomainTupleLexicographicallyLess(
        ReyesEncodePatchBarycentrics(ReyesPatchDomainUVToBarycentrics(lhs0)),
        ReyesEncodePatchBarycentrics(ReyesPatchDomainUVToBarycentrics(lhs1)),
        ReyesEncodePatchBarycentrics(ReyesPatchDomainUVToBarycentrics(lhs2)),
        ReyesEncodePatchBarycentrics(ReyesPatchDomainUVToBarycentrics(rhs0)),
        ReyesEncodePatchBarycentrics(ReyesPatchDomainUVToBarycentrics(rhs1)),
        ReyesEncodePatchBarycentrics(ReyesPatchDomainUVToBarycentrics(rhs2)));
}

bool ReyesHasCanonicalFactorMaxTie(uint3 factors)
{
    const uint maxFactor = max(factors.x, max(factors.y, factors.z));
    uint maxCount = 0u;
    maxCount += (factors.x == maxFactor) ? 1u : 0u;
    maxCount += (factors.y == maxFactor) ? 1u : 0u;
    maxCount += (factors.z == maxFactor) ? 1u : 0u;
    return maxCount > 1u;
}

uint ReyesSelectCanonicalFactorRotation(uint3 factors, uint domainVertex0Encoded, uint domainVertex1Encoded, uint domainVertex2Encoded)
{
    const uint maxFactor = max(factors.x, max(factors.y, factors.z));

    uint bestRotation = 0u;
    bool hasBest = false;
    uint bestDomain0 = 0u;
    uint bestDomain1 = 0u;
    uint bestDomain2 = 0u;

    if (factors.x == maxFactor)
    {
        bestRotation = 0u;
        bestDomain0 = domainVertex0Encoded;
        bestDomain1 = domainVertex1Encoded;
        bestDomain2 = domainVertex2Encoded;
        hasBest = true;
    }

    if (factors.y == maxFactor &&
        (!hasBest || ReyesDomainTupleLexicographicallyLess(domainVertex1Encoded, domainVertex2Encoded, domainVertex0Encoded, bestDomain0, bestDomain1, bestDomain2)))
    {
        bestRotation = 1u;
        bestDomain0 = domainVertex1Encoded;
        bestDomain1 = domainVertex2Encoded;
        bestDomain2 = domainVertex0Encoded;
        hasBest = true;
    }

    if (factors.z == maxFactor &&
        (!hasBest || ReyesDomainTupleLexicographicallyLess(domainVertex2Encoded, domainVertex0Encoded, domainVertex1Encoded, bestDomain0, bestDomain1, bestDomain2)))
    {
        bestRotation = 2u;
    }

    return bestRotation;
}

uint ReyesSelectCanonicalFactorRotation(uint3 factors, float2 domainVertex0UV, float2 domainVertex1UV, float2 domainVertex2UV)
{
    const uint maxFactor = max(factors.x, max(factors.y, factors.z));

    uint bestRotation = 0u;
    bool hasBest = false;
    float2 bestDomain0 = float2(0.0f, 0.0f);
    float2 bestDomain1 = float2(0.0f, 0.0f);
    float2 bestDomain2 = float2(0.0f, 0.0f);

    if (factors.x == maxFactor)
    {
        bestRotation = 0u;
        bestDomain0 = domainVertex0UV;
        bestDomain1 = domainVertex1UV;
        bestDomain2 = domainVertex2UV;
        hasBest = true;
    }

    if (factors.y == maxFactor &&
        (!hasBest || ReyesDomainTupleLexicographicallyLess(domainVertex1UV, domainVertex2UV, domainVertex0UV, bestDomain0, bestDomain1, bestDomain2)))
    {
        bestRotation = 1u;
        bestDomain0 = domainVertex1UV;
        bestDomain1 = domainVertex2UV;
        bestDomain2 = domainVertex0UV;
        hasBest = true;
    }

    if (factors.z == maxFactor &&
        (!hasBest || ReyesDomainTupleLexicographicallyLess(domainVertex2UV, domainVertex0UV, domainVertex1UV, bestDomain0, bestDomain1, bestDomain2)))
    {
        bestRotation = 2u;
    }

    return bestRotation;
}

float ReyesPatchDomainSignedArea2(float2 baryUv0, float2 baryUv1, float2 baryUv2)
{
    return
        (baryUv1.x - baryUv0.x) * (baryUv2.y - baryUv0.y) -
        (baryUv1.y - baryUv0.y) * (baryUv2.x - baryUv0.x);
}
float ReyesPatchDomainSignedArea2(float3 bary0, float3 bary1, float3 bary2)
{
    return ReyesPatchDomainSignedArea2(bary0.yz, bary1.yz, bary2.yz);
}

float ReyesPatchDomainSignedArea2Encoded(uint encoded0, uint encoded1, uint encoded2)
{
    return ReyesPatchDomainSignedArea2(
        ReyesDecodePatchBarycentrics(encoded0),
        ReyesDecodePatchBarycentrics(encoded1),
        ReyesDecodePatchBarycentrics(encoded2));
}

bool ReyesPatchDomainHasValidSimplex(float3 bary0, float3 bary1, float3 bary2)
{
    return abs(ReyesPatchDomainSignedArea2(bary0, bary1, bary2)) > (1.0f / REYES_PATCH_BARYCENTRIC_COORD_SCALE);
}

bool ReyesPatchDomainHasValidSimplex(float2 baryUv0, float2 baryUv1, float2 baryUv2)
{
    return abs(ReyesPatchDomainSignedArea2(baryUv0, baryUv1, baryUv2)) > (1.0f / REYES_PATCH_BARYCENTRIC_COORD_SCALE);
}

bool ReyesPatchDomainHasValidSimplexEncoded(uint encoded0, uint encoded1, uint encoded2)
{
    return abs(ReyesPatchDomainSignedArea2Encoded(encoded0, encoded1, encoded2)) > (1.0f / REYES_PATCH_BARYCENTRIC_COORD_SCALE);
}

uint3 ReyesQuantizeTessTableFactors(float3 edgeFactors)
{
    return uint3(
        ReyesClampTessTableFactor((uint) ceil(max(edgeFactors.x, 1.0f))),
        ReyesClampTessTableFactor((uint) ceil(max(edgeFactors.y, 1.0f))),
        ReyesClampTessTableFactor((uint) ceil(max(edgeFactors.z, 1.0f))));
}

void ReyesRotatePatchDomainYZX(inout uint domainVertex0Encoded, inout uint domainVertex1Encoded, inout uint domainVertex2Encoded)
{
    const uint original0 = domainVertex0Encoded;
    domainVertex0Encoded = domainVertex1Encoded;
    domainVertex1Encoded = domainVertex2Encoded;
    domainVertex2Encoded = original0;
}

void ReyesRotatePatchDomainYZX(inout float2 domainVertex0UV, inout float2 domainVertex1UV, inout float2 domainVertex2UV)
{
    const float2 original0 = domainVertex0UV;
    domainVertex0UV = domainVertex1UV;
    domainVertex1UV = domainVertex2UV;
    domainVertex2UV = original0;
}

void ReyesRotatePatchDomainZXY(inout uint domainVertex0Encoded, inout uint domainVertex1Encoded, inout uint domainVertex2Encoded)
{
    const uint original0 = domainVertex0Encoded;
    domainVertex0Encoded = domainVertex2Encoded;
    domainVertex2Encoded = domainVertex1Encoded;
    domainVertex1Encoded = original0;
}

void ReyesRotatePatchDomainZXY(inout float2 domainVertex0UV, inout float2 domainVertex1UV, inout float2 domainVertex2UV)
{
    const float2 original0 = domainVertex0UV;
    domainVertex0UV = domainVertex2UV;
    domainVertex2UV = domainVertex1UV;
    domainVertex1UV = original0;
}

void ReyesCanonicalizeTessTableFactorsAndPatchDomain(inout uint3 factors, inout uint domainVertex0Encoded, inout uint domainVertex1Encoded, inout uint domainVertex2Encoded)
{
    const uint rotation = ReyesSelectCanonicalFactorRotation(factors, domainVertex0Encoded, domainVertex1Encoded, domainVertex2Encoded);
    if (rotation == 1u)
    {
        factors = factors.yzx;
        ReyesRotatePatchDomainYZX(domainVertex0Encoded, domainVertex1Encoded, domainVertex2Encoded);
    }
    else if (rotation == 2u)
    {
        factors = factors.zxy;
        ReyesRotatePatchDomainZXY(domainVertex0Encoded, domainVertex1Encoded, domainVertex2Encoded);
    }
}

void ReyesCanonicalizeTessTableFactorsAndPatchDomain(inout uint3 factors, inout float2 domainVertex0UV, inout float2 domainVertex1UV, inout float2 domainVertex2UV)
{
    const uint rotation = ReyesSelectCanonicalFactorRotation(factors, domainVertex0UV, domainVertex1UV, domainVertex2UV);
    if (rotation == 1u)
    {
        factors = factors.yzx;
        ReyesRotatePatchDomainYZX(domainVertex0UV, domainVertex1UV, domainVertex2UV);
    }
    else if (rotation == 2u)
    {
        factors = factors.zxy;
        ReyesRotatePatchDomainZXY(domainVertex0UV, domainVertex1UV, domainVertex2UV);
    }
}

uint ReyesEncodeTessTableConfigFromFactors(uint3 factors)
{
    const uint rotation = ReyesSelectCanonicalFactorRotation(factors, 0u, 1u, 2u);
    if (rotation == 1u)
    {
        factors = factors.yzx;
    }
    else if (rotation == 2u)
    {
        factors = factors.zxy;
    }

    uint index =
        factors.x +
        factors.y * REYES_TESS_TABLE_LOOKUP_SIZE +
        factors.z * (REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE) -
        (1u + REYES_TESS_TABLE_LOOKUP_SIZE + REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE);

    if (factors.z > factors.y)
    {
        index |= REYES_TESS_TABLE_FLIP_BIT;
    }

    return index;
}

uint ReyesEncodeTessTableConfig(float3 edgeFactors)
{
    return ReyesEncodeTessTableConfigFromFactors(ReyesQuantizeTessTableFactors(edgeFactors));
}

uint ReyesEncodeCanonicalTessTableConfig(float3 edgeFactors, inout uint domainVertex0Encoded, inout uint domainVertex1Encoded, inout uint domainVertex2Encoded)
{
    uint3 factors = ReyesQuantizeTessTableFactors(edgeFactors);
    ReyesCanonicalizeTessTableFactorsAndPatchDomain(factors, domainVertex0Encoded, domainVertex1Encoded, domainVertex2Encoded);

    uint index =
        factors.x +
        factors.y * REYES_TESS_TABLE_LOOKUP_SIZE +
        factors.z * (REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE) -
        (1u + REYES_TESS_TABLE_LOOKUP_SIZE + REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE);

    if (factors.z > factors.y)
    {
        index |= REYES_TESS_TABLE_FLIP_BIT;
    }

    return index;
}

uint ReyesEncodeCanonicalTessTableConfig(float3 edgeFactors, inout float2 domainVertex0UV, inout float2 domainVertex1UV, inout float2 domainVertex2UV)
{
    uint3 factors = ReyesQuantizeTessTableFactors(edgeFactors);
    ReyesCanonicalizeTessTableFactorsAndPatchDomain(factors, domainVertex0UV, domainVertex1UV, domainVertex2UV);

    uint index =
        factors.x +
        factors.y * REYES_TESS_TABLE_LOOKUP_SIZE +
        factors.z * (REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE) -
        (1u + REYES_TESS_TABLE_LOOKUP_SIZE + REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE);

    if (factors.z > factors.y)
    {
        index |= REYES_TESS_TABLE_FLIP_BIT;
    }

    return index;
}

uint ReyesGetTessTableConfigIndex(uint tessTableConfigIndex)
{
    return tessTableConfigIndex & ~REYES_TESS_TABLE_FLIP_BIT;
}

bool ReyesIsTessTableConfigFlipped(uint tessTableConfigIndex)
{
    return (tessTableConfigIndex & REYES_TESS_TABLE_FLIP_BIT) != 0u;
}

uint3 ReyesDecodeTessTableFactors(uint tessTableConfigIndex)
{
    uint index = ReyesGetTessTableConfigIndex(tessTableConfigIndex);
    index += 1u + REYES_TESS_TABLE_LOOKUP_SIZE + REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE;

    const uint mask = REYES_TESS_TABLE_LOOKUP_SIZE - 1u;
    return uint3(
        index & mask,
        (index / REYES_TESS_TABLE_LOOKUP_SIZE) & mask,
        (index / (REYES_TESS_TABLE_LOOKUP_SIZE * REYES_TESS_TABLE_LOOKUP_SIZE)) & mask);
}

CLodReyesTessTableConfigEntry ReyesGetTessTableConfigEntry(StructuredBuffer<CLodReyesTessTableConfigEntry> configBuffer, uint tessTableConfigIndex)
{
    return configBuffer[ReyesGetTessTableConfigIndex(tessTableConfigIndex)];
}

float3 ReyesDecodeTessTableVertexBarycentrics(uint packedVertex)
{
    const float u = float(packedVertex & 0xFFFFu) / REYES_PATCH_BARYCENTRIC_COORD_SCALE;
    const float v = float(packedVertex >> 16u) / REYES_PATCH_BARYCENTRIC_COORD_SCALE;
    return float3(1.0f - u - v, u, v);
}

uint3 ReyesDecodeTessTableTriangleIndices(uint packedTriangle)
{
    return uint3(
        packedTriangle & 0xFFu,
        (packedTriangle >> 8u) & 0xFFu,
        (packedTriangle >> 16u) & 0xFFu);
}

float3 ReyesGetTessTableConfigVertexBarycentrics(
    StructuredBuffer<CLodReyesTessTableConfigEntry> configBuffer,
    StructuredBuffer<uint> vertexBuffer,
    uint tessTableConfigIndex,
    uint vertexIndex)
{
    const CLodReyesTessTableConfigEntry configEntry = ReyesGetTessTableConfigEntry(configBuffer, tessTableConfigIndex);
    float3 barycentrics = ReyesDecodeTessTableVertexBarycentrics(vertexBuffer[configEntry.firstVertex + vertexIndex]);
    if (ReyesIsTessTableConfigFlipped(tessTableConfigIndex))
    {
        barycentrics = barycentrics.yxz;
    }

    return barycentrics;
}

uint3 ReyesGetTessTableConfigTriangleVertexIndices(
    StructuredBuffer<CLodReyesTessTableConfigEntry> configBuffer,
    StructuredBuffer<uint> triangleBuffer,
    uint tessTableConfigIndex,
    uint triangleIndex)
{
    const CLodReyesTessTableConfigEntry configEntry = ReyesGetTessTableConfigEntry(configBuffer, tessTableConfigIndex);
    uint3 indices = ReyesDecodeTessTableTriangleIndices(triangleBuffer[configEntry.firstTriangle + triangleIndex]);
    if (ReyesIsTessTableConfigFlipped(tessTableConfigIndex))
    {
        indices = indices.xzy;
    }

    return indices;
}

uint ReyesGetDicePatchSegments(CLodReyesDiceQueueEntry diceEntry)
{
    if (CLodReyesIsCoarseDirtyOnlyLeaf(diceEntry.flags))
    {
        return 1u;
    }

    const uint3 tessFactors = ReyesDecodeTessTableFactors(diceEntry.tessTableConfigIndex);
    const uint tessSegments = max(tessFactors.x, max(tessFactors.y, tessFactors.z));
    return max(1u, tessSegments);
}

uint ReyesGetDicePatchMicroTriangleCount(StructuredBuffer<CLodReyesTessTableConfigEntry> configBuffer, CLodReyesDiceQueueEntry diceEntry)
{
    if (CLodReyesIsCoarseDirtyOnlyLeaf(diceEntry.flags))
    {
        return 1u;
    }

    return ReyesGetTessTableConfigEntry(configBuffer, diceEntry.tessTableConfigIndex).numTriangles;
}

uint ReyesGetDicePatchVertexCount(StructuredBuffer<CLodReyesTessTableConfigEntry> configBuffer, CLodReyesDiceQueueEntry diceEntry)
{
    if (CLodReyesIsCoarseDirtyOnlyLeaf(diceEntry.flags))
    {
        return 3u;
    }

    return ReyesGetTessTableConfigEntry(configBuffer, diceEntry.tessTableConfigIndex).numVertices;
}

float3 ReyesMakePatchGridBarycentrics(uint col, uint row, uint tessSegments)
{
    const float invSegments = 1.0f / float(max(tessSegments, 1u));
    const float u = float(col) * invSegments;
    const float v = float(row) * invSegments;
    return float3(saturate(1.0f - u - v), u, v);
}

float3 ReyesComposeSourceBarycentricsPoint(float3 patchBary, float3 domain0, float3 domain1, float3 domain2)
{
    precise float3 sourceBarycentrics =
        domain0 * patchBary.x +
        domain1 * patchBary.y +
        domain2 * patchBary.z;
    return sourceBarycentrics;
}

float3 ReyesInterpolateFloat3Precise(float3 value0, float3 value1, float3 value2, float3 barycentrics)
{
    precise float3 result =
        value0 * barycentrics.x +
        value1 * barycentrics.y +
        value2 * barycentrics.z;
    return result;
}

float2 ReyesInterpolateFloat2Precise(float2 value0, float2 value1, float2 value2, float3 barycentrics)
{
    precise float2 result =
        value0 * barycentrics.x +
        value1 * barycentrics.y +
        value2 * barycentrics.z;
    return result;
}

float ReyesGeometricDisplacementGlobalScale(uint materialFlags)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    return (materialFlags & MATERIAL_TERRAIN) != 0u
        ? max(perFrameBuffer.terrainReyesDisplacementScale, 0.0f)
        : max(perFrameBuffer.objectReyesDisplacementScale, 0.0f);
}

float ReyesGeometricDisplacementGlobalScale(MaterialInfo materialInfo)
{
    return ReyesGeometricDisplacementGlobalScale(materialInfo.materialFlags);
}

float ReyesGeometricDisplacementGlobalScale(MaterialEvalInfo materialInfo)
{
    return ReyesGeometricDisplacementGlobalScale(materialInfo.materialFlags);
}

bool ReyesGeometricDisplacementEnabled(MaterialInfo materialInfo)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return false;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        return true;
    }

    return (materialInfo.materialFlags & MATERIAL_GEOMETRIC_DISPLACEMENT) != 0u &&
        (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) == 0u;
}

bool ReyesGeometricDisplacementEnabled(MaterialEvalInfo materialInfo)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return false;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        return true;
    }

    return (materialInfo.materialFlags & MATERIAL_GEOMETRIC_DISPLACEMENT) != 0u &&
        (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) == 0u;
}

float ReyesGeometricDisplacementMagnitude(MaterialInfo materialInfo)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    return max(abs(materialInfo.geometricDisplacementMin), abs(materialInfo.geometricDisplacementMax)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo);
}

float ReyesGeometricDisplacementMagnitude(MaterialEvalInfo materialInfo)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    return max(abs(materialInfo.geometricDisplacementMin), abs(materialInfo.geometricDisplacementMax)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo);
}

float ReyesEstimateWorldUnitsPerPixel(CullingCameraInfo camera, float depth)
{
    const float projectionScale = max(abs(camera.projY), 1.0e-5f) * (0.5f * ReyesViewportHeightPixels(camera));
    if (camera.isOrtho != 0u)
    {
        return ReyesDiceRatePixels(camera) / max(projectionScale, 1.0e-5f);
    }

    return ReyesDiceRatePixels(camera) * max(depth, max(camera.zNear, 1.0e-3f)) / projectionScale;
}

float ReyesEstimatePointDepth(CullingCameraInfo camera, float3 positionWS)
{
    const float3 cameraToPoint = positionWS - camera.positionWorldSpace.xyz;
    const float viewDepth = abs(dot(cameraToPoint, normalize(camera.viewForwardWorld.xyz)));
    return max(viewDepth, max(camera.zNear, 1.0e-3f));
}

void ReyesEstimateStableTerrainDerivatives(CullingCameraInfo camera, float3 positionWS, out float3 dpdxWS, out float3 dpdyWS)
{
    const float worldUnitsPerPixel = ReyesEstimateWorldUnitsPerPixel(camera, ReyesEstimatePointDepth(camera, positionWS));
    dpdxWS = camera.viewRightWorld.xyz * worldUnitsPerPixel;
    dpdyWS = camera.viewUpWorld.xyz * worldUnitsPerPixel;
}

float2 ReyesEstimateUvDerivative(MaterialInfo materialInfo, CullingCameraInfo camera, float depth)
{
    const float worldUnitsPerPixel = ReyesEstimateWorldUnitsPerPixel(camera, depth);
    return max(materialInfo.reyesUvDensity, 1.0e-6f.xx) * worldUnitsPerPixel;
}

float2 ReyesEstimateUvDerivative(MaterialEvalInfo materialInfo, CullingCameraInfo camera, float depth)
{
    const float worldUnitsPerPixel = ReyesEstimateWorldUnitsPerPixel(camera, depth);
    return max(materialInfo.reyesUvDensity, 1.0e-6f.xx) * worldUnitsPerPixel;
}

float2 ReyesProjectToReferencePixel(CullingCameraInfo camera, float3 positionWS, out bool valid)
{
    const float4 clip = mul(float4(positionWS, 1.0f), camera.viewProjection);
    valid = abs(clip.w) > 1.0e-5f;
    const float2 ndc = valid ? clip.xy / clip.w : 0.0f.xx;
    const float aspect = max(abs(camera.projY / max(camera.projX, 1.0e-5f)), 1.0e-5f);
    const float viewportHeight = ReyesViewportHeightPixels(camera);
    return float2(
        (ndc.x * 0.5f + 0.5f) * viewportHeight * aspect,
        (0.5f - ndc.y * 0.5f) * viewportHeight);
}

bool ReyesEstimateFloat2DerivativesFromPatch(
    CullingCameraInfo camera,
    float3 position0WS,
    float3 position1WS,
    float3 position2WS,
    float2 value0,
    float2 value1,
    float2 value2,
    out float2 dValueDx,
    out float2 dValueDy)
{
    dValueDx = 0.0f.xx;
    dValueDy = 0.0f.xx;

    bool valid0;
    bool valid1;
    bool valid2;
    const float2 screen0 = ReyesProjectToReferencePixel(camera, position0WS, valid0);
    const float2 screen1 = ReyesProjectToReferencePixel(camera, position1WS, valid1);
    const float2 screen2 = ReyesProjectToReferencePixel(camera, position2WS, valid2);
    if (!valid0 || !valid1 || !valid2)
    {
        return false;
    }

    const float2 screenEdge1 = screen1 - screen0;
    const float2 screenEdge2 = screen2 - screen0;
    const float det = screenEdge1.x * screenEdge2.y - screenEdge1.y * screenEdge2.x;
    if (abs(det) <= 1.0e-5f)
    {
        return false;
    }

    const float2 valueEdge1 = value1 - value0;
    const float2 valueEdge2 = value2 - value0;
    const float invDet = rcp(det);
    dValueDx = (screenEdge2.y * valueEdge1 - screenEdge1.y * valueEdge2) * invDet;
    dValueDy = (-screenEdge2.x * valueEdge1 + screenEdge1.x * valueEdge2) * invDet;

    return all(isfinite(dValueDx)) && all(isfinite(dValueDy));
}

bool ReyesEstimateTerrainDerivativesFromPatch(
    CullingCameraInfo camera,
    float3 position0WS,
    float3 position1WS,
    float3 position2WS,
    out float3 dpdxWS,
    out float3 dpdyWS)
{
    dpdxWS = 0.0f.xxx;
    dpdyWS = 0.0f.xxx;

    float2 dTerrainDx;
    float2 dTerrainDy;
    const bool valid = ReyesEstimateFloat2DerivativesFromPatch(
        camera,
        position0WS,
        position1WS,
        position2WS,
        TerrainSkyrimXYFromRendererPosition(position0WS),
        TerrainSkyrimXYFromRendererPosition(position1WS),
        TerrainSkyrimXYFromRendererPosition(position2WS),
        dTerrainDx,
        dTerrainDy);
    if (!valid)
    {
        return false;
    }

    dpdxWS = float3(dTerrainDx.x, 0.0f, -dTerrainDx.y);
    dpdyWS = float3(dTerrainDy.x, 0.0f, -dTerrainDy.y);
    return all(isfinite(dpdxWS)) && all(isfinite(dpdyWS));
}

bool ReyesEstimateUvDerivativesFromPatch(
    CullingCameraInfo camera,
    float3 position0WS,
    float3 position1WS,
    float3 position2WS,
    float2 uv0,
    float2 uv1,
    float2 uv2,
    out float2 dUVdx,
    out float2 dUVdy)
{
    return ReyesEstimateFloat2DerivativesFromPatch(
        camera,
        position0WS,
        position1WS,
        position2WS,
        uv0,
        uv1,
        uv2,
        dUVdx,
        dUVdy);
}

float ReyesSampleMaterialDisplacementOffset(
    MaterialInfo materialInfo,
    float2 uv,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
    const float2 sampleUv = saturate(uv);
    if (materialInfo.objectSurfaceSamplingMode == OBJECT_SURFACE_SAMPLING_ATLAS_BAKED_HEIGHT)
    {
        const float heightValue = saturate(heightTexture.SampleLevel(heightSampler, sampleUv, 0.0f).r);
        return lerp(
            materialInfo.geometricDisplacementMin,
            materialInfo.geometricDisplacementMax,
            heightValue);
    }

    const float4 heightSample = useUvDerivatives
        ? heightTexture.SampleGrad(heightSampler, uv, dUVdx, dUVdy)
        : heightTexture.SampleLevel(heightSampler, uv, 0.0f);
    const float heightValue = saturate(heightSample.r);
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleMaterialDisplacementOffset(
    MaterialEvalInfo materialInfo,
    float2 uv,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
    const float2 sampleUv = saturate(uv);
    if (materialInfo.objectSurfaceSamplingMode == OBJECT_SURFACE_SAMPLING_ATLAS_BAKED_HEIGHT)
    {
        const float heightValue = saturate(heightTexture.SampleLevel(heightSampler, sampleUv, 0.0f).r);
        return lerp(
            materialInfo.geometricDisplacementMin,
            materialInfo.geometricDisplacementMax,
            heightValue);
    }

    const float4 heightSample = useUvDerivatives
        ? heightTexture.SampleGrad(heightSampler, uv, dUVdx, dUVdy)
        : heightTexture.SampleLevel(heightSampler, uv, 0.0f);
    const float heightValue = saturate(heightSample.r);
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleMaterialDisplacementOffset(MaterialInfo materialInfo, float2 uv, CullingCameraInfo camera, float depth)
{
    const float2 uvDerivative = ReyesEstimateUvDerivative(materialInfo, camera, depth);
    return ReyesSampleMaterialDisplacementOffset(materialInfo, uv, true, float2(uvDerivative.x, 0.0f), float2(0.0f, uvDerivative.y));
}

float ReyesSampleMaterialDisplacementOffset(MaterialEvalInfo materialInfo, float2 uv, CullingCameraInfo camera, float depth)
{
    const float2 uvDerivative = ReyesEstimateUvDerivative(materialInfo, camera, depth);
    return ReyesSampleMaterialDisplacementOffset(materialInfo, uv, true, float2(uvDerivative.x, 0.0f), float2(0.0f, uvDerivative.y));
}

float ReyesSampleDisplacementOffset(MaterialInfo materialInfo, float3 positionOS, float2 uv, CullingCameraInfo camera, float depth)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        float3 dpdxWS;
        float3 dpdyWS;
        ReyesEstimateStableTerrainDerivatives(camera, positionOS, dpdxWS, dpdyWS);
        const float heightValue = saturate(TerrainSampleGeometricHeightRvtOnlyOrDirectFallback(materialInfo.terrainSetIndex, positionOS, dpdxWS, dpdyWS));
        return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
    }

    return ReyesSampleMaterialDisplacementOffset(materialInfo, uv, camera, depth);
}

float ReyesSampleDisplacementOffset(
    MaterialInfo materialInfo,
    float3 positionOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        // Geometric displacement must be edge-deterministic. Per-microtriangle
        // projected derivatives can choose different RVT clips for the same
        // shared edge point, so terrain RVT mip selection uses the point itself.
        float3 terrainDpdxWS;
        float3 terrainDpdyWS;
        ReyesEstimateStableTerrainDerivatives(camera, positionOS, terrainDpdxWS, terrainDpdyWS);
        const float heightValue = saturate(TerrainSampleGeometricHeightRvtOnlyOrDirectFallback(materialInfo.terrainSetIndex, positionOS, terrainDpdxWS, terrainDpdyWS));
        return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
    }

    return useUvDerivatives
        ? ReyesSampleMaterialDisplacementOffset(materialInfo, uv, true, dUVdx, dUVdy)
        : ReyesSampleMaterialDisplacementOffset(materialInfo, uv, camera, depth);
}

float ReyesSampleDisplacementOffset(MaterialEvalInfo materialInfo, float3 positionOS, float2 uv, CullingCameraInfo camera, float depth)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        float3 dpdxWS;
        float3 dpdyWS;
        ReyesEstimateStableTerrainDerivatives(camera, positionOS, dpdxWS, dpdyWS);
        const float heightValue = saturate(TerrainSampleGeometricHeightRvtOnlyOrDirectFallback(materialInfo.terrainSetIndex, positionOS, dpdxWS, dpdyWS));
        return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
    }

    return ReyesSampleMaterialDisplacementOffset(materialInfo, uv, camera, depth);
}

float ReyesSampleDisplacementOffset(
    MaterialEvalInfo materialInfo,
    float3 positionOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        // Geometric displacement must be edge-deterministic. Per-microtriangle
        // projected derivatives can choose different RVT clips for the same
        // shared edge point, so terrain RVT mip selection uses the point itself.
        float3 terrainDpdxWS;
        float3 terrainDpdyWS;
        ReyesEstimateStableTerrainDerivatives(camera, positionOS, terrainDpdxWS, terrainDpdyWS);
        const float heightValue = saturate(TerrainSampleGeometricHeightRvtOnlyOrDirectFallback(materialInfo.terrainSetIndex, positionOS, terrainDpdxWS, terrainDpdyWS));
        return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
    }

    return useUvDerivatives
        ? ReyesSampleMaterialDisplacementOffset(materialInfo, uv, true, dUVdx, dUVdy)
        : ReyesSampleMaterialDisplacementOffset(materialInfo, uv, camera, depth);
}

void ReyesObjectSurfaceDerivativeBasis(float3 normalOS, out float3 dpdxOS, out float3 dpdyOS)
{
    const float3 n = normalize(normalOS);
    const float3 up = abs(n.z) < 0.9f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    dpdxOS = normalize(cross(up, n));
    dpdyOS = normalize(cross(n, dpdxOS));
}

float ReyesSampleObjectSurfaceDisplacementOffset(MaterialInfo materialInfo, float3 positionOS, float3 normalOS)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }
    Texture2D<float> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
    float3 dpdxOS;
    float3 dpdyOS;
    ReyesObjectSurfaceDerivativeBasis(normalOS, dpdxOS, dpdyOS);
    MaterialInputs unusedMaterialInputs = (MaterialInputs)0;
    InitializeMaterialSelectedMipDebug(unusedMaterialInputs);
    const float heightValue = saturate(ObjectSurfaceSampleTriplanarHeight(
        heightTexture,
        heightSampler,
        materialInfo.heightStreamingTextureID,
        positionOS,
        normalOS,
        dpdxOS,
        dpdyOS,
        max(materialInfo.objectSurfaceTexelDensity, 1.0e-6f),
        unusedMaterialInputs));
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleObjectSurfaceDisplacementOffset(MaterialEvalInfo materialInfo, float3 positionOS, float3 normalOS)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }
    Texture2D<float> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
    float3 dpdxOS;
    float3 dpdyOS;
    ReyesObjectSurfaceDerivativeBasis(normalOS, dpdxOS, dpdyOS);
    MaterialInputs unusedMaterialInputs = (MaterialInputs)0;
    InitializeMaterialSelectedMipDebug(unusedMaterialInputs);
    const float heightValue = saturate(ObjectSurfaceSampleTriplanarHeight(
        heightTexture,
        heightSampler,
        materialInfo.heightStreamingTextureID,
        positionOS,
        normalOS,
        dpdxOS,
        dpdyOS,
        max(materialInfo.objectSurfaceTexelDensity, 1.0e-6f),
        unusedMaterialInputs));
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleObjectSurfaceBlendDisplacementOffset(MaterialInfo materialInfo, float3 positionOS, float3 normalOS, float blendWeight)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }
    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo material0 = materialDataBuffer[materialInfo.objectBlendMaterialIndex0];
    MaterialInfo material1 = materialDataBuffer[materialInfo.objectBlendMaterialIndex1];
    const float offset0 = ReyesSampleObjectSurfaceDisplacementOffset(material0, positionOS, normalOS);
    const float offset1 = ReyesSampleObjectSurfaceDisplacementOffset(material1, positionOS, normalOS);
    return lerp(offset0, offset1, saturate(blendWeight));
}

float ReyesSampleObjectSurfaceBlendDisplacementOffset(MaterialEvalInfo materialInfo, float3 positionOS, float3 normalOS, float blendWeight)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }
    StructuredBuffer<MaterialEvalInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialEvalDataBuffer)];
    MaterialEvalInfo material0 = materialDataBuffer[materialInfo.objectBlendMaterialIndex0];
    MaterialEvalInfo material1 = materialDataBuffer[materialInfo.objectBlendMaterialIndex1];
    const float offset0 = ReyesSampleObjectSurfaceDisplacementOffset(material0, positionOS, normalOS);
    const float offset1 = ReyesSampleObjectSurfaceDisplacementOffset(material1, positionOS, normalOS);
    return lerp(offset0, offset1, saturate(blendWeight));
}

float3 ReyesApplyGeometricDisplacement(MaterialInfo materialInfo, float3 positionOS, float3 normalOS, float2 uv, CullingCameraInfo camera, float depth)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialInfo materialInfo,
    float3 positionOS,
    float3 positionWS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo) *
        CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, positionWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialInfo materialInfo,
    float3 positionOS,
    float3 positionWS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useUvDerivatives,
        dUVdx,
        dUVdy)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo) *
        CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, positionWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialInfo materialInfo,
    float3 positionOS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useUvDerivatives,
        dUVdx,
        dUVdy)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(MaterialEvalInfo materialInfo, float3 positionOS, float3 normalOS, float2 uv, CullingCameraInfo camera, float depth)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialEvalInfo materialInfo,
    float3 positionOS,
    float3 positionWS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo) *
        CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, positionWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialEvalInfo materialInfo,
    float3 positionOS,
    float3 positionWS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useUvDerivatives,
        dUVdx,
        dUVdy)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo) *
        CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, positionWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialEvalInfo materialInfo,
    float3 positionOS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useUvDerivatives,
        dUVdx,
        dUVdy)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo);
    return positionOS + normalize(normalOS) * displacementOffset;
}

void ReyesApplyGeometricDisplacement3(
    MaterialEvalInfo materialInfo,
    inout float3 position0OS,
    inout float3 position1OS,
    inout float3 position2OS,
    float3 position0WS,
    float3 position1WS,
    float3 position2WS,
    float3 normal0OS,
    float3 normal1OS,
    float3 normal2OS,
    float2 uv0,
    float2 uv1,
    float2 uv2,
    CullingCameraInfo camera,
    float depth,
    bool useUvDerivatives,
    float2 dUVdx,
    float2 dUVdy,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        float3 terrainDpdx0WS;
        float3 terrainDpdy0WS;
        float3 terrainDpdx1WS;
        float3 terrainDpdy1WS;
        float3 terrainDpdx2WS;
        float3 terrainDpdy2WS;
        ReyesEstimateStableTerrainDerivatives(camera, position0OS, terrainDpdx0WS, terrainDpdy0WS);
        ReyesEstimateStableTerrainDerivatives(camera, position1OS, terrainDpdx1WS, terrainDpdy1WS);
        ReyesEstimateStableTerrainDerivatives(camera, position2OS, terrainDpdx2WS, terrainDpdy2WS);

        float height0;
        float height1;
        float height2;
        TerrainSampleGeometricHeightRvtOnlyOrDirectFallback3(
            materialInfo.terrainSetIndex,
            position0OS,
            position1OS,
            position2OS,
            terrainDpdx0WS,
            terrainDpdy0WS,
            terrainDpdx1WS,
            terrainDpdy1WS,
            terrainDpdx2WS,
            terrainDpdy2WS,
            height0,
            height1,
            height2);

        const float displacementScale = ReyesGeometricDisplacementGlobalScale(materialInfo);
        position0OS += normalize(normal0OS) *
            (lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height0)) *
            displacementScale *
            CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, position0WS));
        position1OS += normalize(normal1OS) *
            (lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height1)) *
            displacementScale *
            CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, position1WS));
        position2OS += normalize(normal2OS) *
            (lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height2)) *
            displacementScale *
            CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, position2WS));
        return;
    }

    position0OS = ReyesApplyGeometricDisplacement(materialInfo, position0OS, position0WS, normal0OS, uv0, camera, depth, useUvDerivatives, dUVdx, dUVdy, reyesFadeStartDistance, reyesFadeEndDistance);
    position1OS = ReyesApplyGeometricDisplacement(materialInfo, position1OS, position1WS, normal1OS, uv1, camera, depth, useUvDerivatives, dUVdx, dUVdy, reyesFadeStartDistance, reyesFadeEndDistance);
    position2OS = ReyesApplyGeometricDisplacement(materialInfo, position2OS, position2WS, normal2OS, uv2, camera, depth, useUvDerivatives, dUVdx, dUVdy, reyesFadeStartDistance, reyesFadeEndDistance);
}

float ReyesSampleMaterialDisplacementOffset(MaterialInfo materialInfo, float2 uv)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.heightSamplerIndex)];
    const float2 sampleUv = materialInfo.objectSurfaceSamplingMode == OBJECT_SURFACE_SAMPLING_ATLAS_BAKED_HEIGHT
        ? saturate(uv)
        : uv;
    if (materialInfo.objectSurfaceSamplingMode == OBJECT_SURFACE_SAMPLING_ATLAS_BAKED_HEIGHT)
    {
        const float heightValue = saturate(heightTexture.SampleLevel(heightSampler, sampleUv, 0.0f).r);
        return lerp(
            materialInfo.geometricDisplacementMin,
            materialInfo.geometricDisplacementMax,
            heightValue);
    }

    const float heightValue = saturate(heightTexture.SampleLevel(heightSampler, sampleUv, 0.0f).r);
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleDisplacementOffset(MaterialInfo materialInfo, float3 positionOS, float2 uv)
{
    if (!ReyesGeometricDisplacementEnabled(materialInfo))
    {
        return 0.0f;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        const float heightValue = saturate(TerrainSampleGeometricHeightDirect(materialInfo.terrainSetIndex, positionOS));
        return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
    }

    return ReyesSampleMaterialDisplacementOffset(materialInfo, uv);
}

float3 ReyesApplyGeometricDisplacement(MaterialInfo materialInfo, float3 positionOS, float3 normalOS, float2 uv)
{
    const float displacementOffset = (((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC_BLEND) != 0u)
        ? ReyesSampleObjectSurfaceBlendDisplacementOffset(materialInfo, positionOS, normalOS, uv.x)
        : ((materialInfo.materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u)
        ? ReyesSampleObjectSurfaceDisplacementOffset(materialInfo, positionOS, normalOS)
        : ReyesSampleDisplacementOffset(materialInfo, positionOS, uv)) *
        ReyesGeometricDisplacementGlobalScale(materialInfo);
    return positionOS + normalize(normalOS) * displacementOffset;
}

void ReyesEvaluateDisplacedPatchTriangle(
    MaterialInfo materialInfo,
    bool displacementEnabled,
    CullingCameraInfo camera,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance,
    row_major matrix objectModelMatrix,
    float patchDepth,
    float3 sourcePosition0,
    float3 sourcePosition1,
    float3 sourcePosition2,
    float3 sourceNormal0,
    float3 sourceNormal1,
    float3 sourceNormal2,
    float2 sourceUv0,
    float2 sourceUv1,
    float2 sourceUv2,
    float3 domain0,
    float3 domain1,
    float3 domain2,
    float3 patchBary0,
    float3 patchBary1,
    float3 patchBary2,
    out float3 sourceBary0,
    out float3 sourceBary1,
    out float3 sourceBary2,
    out float3 patchPosition0,
    out float3 patchPosition1,
    out float3 patchPosition2)
{
    sourceBary0 = ReyesComposeSourceBarycentricsPoint(patchBary0, domain0, domain1, domain2);
    sourceBary1 = ReyesComposeSourceBarycentricsPoint(patchBary1, domain0, domain1, domain2);
    sourceBary2 = ReyesComposeSourceBarycentricsPoint(patchBary2, domain0, domain1, domain2);

    patchPosition0 = ReyesInterpolateFloat3Precise(sourcePosition0, sourcePosition1, sourcePosition2, sourceBary0);
    patchPosition1 = ReyesInterpolateFloat3Precise(sourcePosition0, sourcePosition1, sourcePosition2, sourceBary1);
    patchPosition2 = ReyesInterpolateFloat3Precise(sourcePosition0, sourcePosition1, sourcePosition2, sourceBary2);

    if (displacementEnabled)
    {
        const float3 patchNormal0 = normalize(ReyesInterpolateFloat3Precise(sourceNormal0, sourceNormal1, sourceNormal2, sourceBary0));
        const float3 patchNormal1 = normalize(ReyesInterpolateFloat3Precise(sourceNormal0, sourceNormal1, sourceNormal2, sourceBary1));
        const float3 patchNormal2 = normalize(ReyesInterpolateFloat3Precise(sourceNormal0, sourceNormal1, sourceNormal2, sourceBary2));

        if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
        {
            float3 terrainDpdx0WS;
            float3 terrainDpdy0WS;
            float3 terrainDpdx1WS;
            float3 terrainDpdy1WS;
            float3 terrainDpdx2WS;
            float3 terrainDpdy2WS;
            ReyesEstimateStableTerrainDerivatives(camera, patchPosition0, terrainDpdx0WS, terrainDpdy0WS);
            ReyesEstimateStableTerrainDerivatives(camera, patchPosition1, terrainDpdx1WS, terrainDpdy1WS);
            ReyesEstimateStableTerrainDerivatives(camera, patchPosition2, terrainDpdx2WS, terrainDpdy2WS);

            float height0;
            float height1;
            float height2;
            TerrainSampleGeometricHeightRvtOnlyOrDirectFallback3(
                materialInfo.terrainSetIndex,
                patchPosition0,
                patchPosition1,
                patchPosition2,
                terrainDpdx0WS,
                terrainDpdy0WS,
                terrainDpdx1WS,
                terrainDpdy1WS,
                terrainDpdx2WS,
                terrainDpdy2WS,
                height0,
                height1,
                height2);
            const float3 patchPosition0WS = mul(float4(patchPosition0, 1.0f), objectModelMatrix).xyz;
            const float3 patchPosition1WS = mul(float4(patchPosition1, 1.0f), objectModelMatrix).xyz;
            const float3 patchPosition2WS = mul(float4(patchPosition2, 1.0f), objectModelMatrix).xyz;
            patchPosition0 += patchNormal0 * lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height0)) *
                ReyesGeometricDisplacementGlobalScale(materialInfo) *
                CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, patchPosition0WS);
            patchPosition1 += patchNormal1 * lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height1)) *
                ReyesGeometricDisplacementGlobalScale(materialInfo) *
                CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, patchPosition1WS);
            patchPosition2 += patchNormal2 * lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height2)) *
                ReyesGeometricDisplacementGlobalScale(materialInfo) *
                CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, patchPosition2WS);
        }
        else
        {
            const float2 patchUv0 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary0);
            const float2 patchUv1 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary1);
            const float2 patchUv2 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary2);
            const float3 patchPosition0WS = mul(float4(patchPosition0, 1.0f), objectModelMatrix).xyz;
            const float3 patchPosition1WS = mul(float4(patchPosition1, 1.0f), objectModelMatrix).xyz;
            const float3 patchPosition2WS = mul(float4(patchPosition2, 1.0f), objectModelMatrix).xyz;
            float2 dUVdx = 0.0f.xx;
            float2 dUVdy = 0.0f.xx;
            const bool useUvDerivatives = ReyesEstimateUvDerivativesFromPatch(
                camera,
                patchPosition0WS,
                patchPosition1WS,
                patchPosition2WS,
                patchUv0,
                patchUv1,
                patchUv2,
                dUVdx,
                dUVdy);
            patchPosition0 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition0, patchPosition0WS, patchNormal0, patchUv0, camera, patchDepth, useUvDerivatives, dUVdx, dUVdy, reyesFadeStartDistance, reyesFadeEndDistance);
            patchPosition1 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition1, patchPosition1WS, patchNormal1, patchUv1, camera, patchDepth, useUvDerivatives, dUVdx, dUVdy, reyesFadeStartDistance, reyesFadeEndDistance);
            patchPosition2 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition2, patchPosition2WS, patchNormal2, patchUv2, camera, patchDepth, useUvDerivatives, dUVdx, dUVdy, reyesFadeStartDistance, reyesFadeEndDistance);
        }
    }
}

void ReyesEvaluateDisplacedPatchTriangle(
    MaterialInfo materialInfo,
    bool displacementEnabled,
    float3 sourcePosition0,
    float3 sourcePosition1,
    float3 sourcePosition2,
    float3 sourceNormal0,
    float3 sourceNormal1,
    float3 sourceNormal2,
    float2 sourceUv0,
    float2 sourceUv1,
    float2 sourceUv2,
    float3 domain0,
    float3 domain1,
    float3 domain2,
    float3 patchBary0,
    float3 patchBary1,
    float3 patchBary2,
    out float3 sourceBary0,
    out float3 sourceBary1,
    out float3 sourceBary2,
    out float3 patchPosition0,
    out float3 patchPosition1,
    out float3 patchPosition2)
{
    sourceBary0 = ReyesComposeSourceBarycentricsPoint(patchBary0, domain0, domain1, domain2);
    sourceBary1 = ReyesComposeSourceBarycentricsPoint(patchBary1, domain0, domain1, domain2);
    sourceBary2 = ReyesComposeSourceBarycentricsPoint(patchBary2, domain0, domain1, domain2);

    patchPosition0 = ReyesInterpolateFloat3Precise(sourcePosition0, sourcePosition1, sourcePosition2, sourceBary0);
    patchPosition1 = ReyesInterpolateFloat3Precise(sourcePosition0, sourcePosition1, sourcePosition2, sourceBary1);
    patchPosition2 = ReyesInterpolateFloat3Precise(sourcePosition0, sourcePosition1, sourcePosition2, sourceBary2);

    if (displacementEnabled)
    {
        const float3 patchNormal0 = normalize(ReyesInterpolateFloat3Precise(sourceNormal0, sourceNormal1, sourceNormal2, sourceBary0));
        const float3 patchNormal1 = normalize(ReyesInterpolateFloat3Precise(sourceNormal0, sourceNormal1, sourceNormal2, sourceBary1));
        const float3 patchNormal2 = normalize(ReyesInterpolateFloat3Precise(sourceNormal0, sourceNormal1, sourceNormal2, sourceBary2));
        const float2 patchUv0 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary0);
        const float2 patchUv1 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary1);
        const float2 patchUv2 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary2);
        patchPosition0 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition0, patchNormal0, patchUv0);
        patchPosition1 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition1, patchNormal1, patchUv1);
        patchPosition2 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition2, patchNormal2, patchUv2);
    }
}

void ReyesDecodeMicroTrianglePatchDomain(
    StructuredBuffer<CLodReyesTessTableConfigEntry> configBuffer,
    StructuredBuffer<uint> vertexBuffer,
    StructuredBuffer<uint> triangleBuffer,
    uint triIndex,
    CLodReyesDiceQueueEntry diceEntry,
    out float3 bary0,
    out float3 bary1,
    out float3 bary2)
{
    if (CLodReyesIsCoarseDirtyOnlyLeaf(diceEntry.flags))
    {
        bary0 = float3(1.0f, 0.0f, 0.0f);
        bary1 = float3(0.0f, 1.0f, 0.0f);
        bary2 = float3(0.0f, 0.0f, 1.0f);
        return;
    }

    const uint3 triangleIndices = ReyesGetTessTableConfigTriangleVertexIndices(configBuffer, triangleBuffer, diceEntry.tessTableConfigIndex, triIndex);
    bary0 = ReyesGetTessTableConfigVertexBarycentrics(configBuffer, vertexBuffer, diceEntry.tessTableConfigIndex, triangleIndices.x);
    bary1 = ReyesGetTessTableConfigVertexBarycentrics(configBuffer, vertexBuffer, diceEntry.tessTableConfigIndex, triangleIndices.y);
    bary2 = ReyesGetTessTableConfigVertexBarycentrics(configBuffer, vertexBuffer, diceEntry.tessTableConfigIndex, triangleIndices.z);
}

#endif // __REYES_PATCH_COMMON_HLSLI__
