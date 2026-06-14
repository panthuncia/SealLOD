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

float ReyesEstimateWorldUnitsPerPixel(CullingCameraInfo camera, float depth)
{
    const float projectionScale = max(abs(camera.projY), 1.0e-5f) * (0.5f * REYES_SCREEN_SCALE_REFERENCE);
    if (camera.isOrtho != 0u)
    {
        return REYES_DICE_RATE_PIXELS / max(projectionScale, 1.0e-5f);
    }

    return REYES_DICE_RATE_PIXELS * max(depth, max(camera.zNear, 1.0e-3f)) / projectionScale;
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
    return float2(
        (ndc.x * 0.5f + 0.5f) * REYES_SCREEN_SCALE_REFERENCE * aspect,
        (0.5f - ndc.y * 0.5f) * REYES_SCREEN_SCALE_REFERENCE);
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

    const float2 terrain0 = TerrainSkyrimXYFromRendererPosition(position0WS);
    const float2 terrainEdge1 = TerrainSkyrimXYFromRendererPosition(position1WS) - terrain0;
    const float2 terrainEdge2 = TerrainSkyrimXYFromRendererPosition(position2WS) - terrain0;
    const float invDet = rcp(det);
    const float2 dTerrainDx = (screenEdge2.y * terrainEdge1 - screenEdge1.y * terrainEdge2) * invDet;
    const float2 dTerrainDy = (-screenEdge2.x * terrainEdge1 + screenEdge1.x * terrainEdge2) * invDet;

    dpdxWS = float3(dTerrainDx.x, 0.0f, -dTerrainDx.y);
    dpdyWS = float3(dTerrainDy.x, 0.0f, -dTerrainDy.y);
    return all(isfinite(dpdxWS)) && all(isfinite(dpdyWS));
}

float ReyesSampleMaterialDisplacementOffset(MaterialInfo materialInfo, float2 uv, CullingCameraInfo camera, float depth)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return 0.0f;
    }

    const bool heightFromBaseAlpha = (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u;
    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(
        heightFromBaseAlpha ? materialInfo.baseColorTextureIndex : materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(
        heightFromBaseAlpha ? materialInfo.baseColorSamplerIndex : materialInfo.heightSamplerIndex)];
    const float2 uvDerivative = ReyesEstimateUvDerivative(materialInfo, camera, depth);
    const float4 heightSample = heightTexture.SampleGrad(heightSampler, uv, float2(uvDerivative.x, 0.0f), float2(0.0f, uvDerivative.y));
    const uint heightChannel = heightFromBaseAlpha ? 3u : materialInfo.heightChannel;
    const float heightValue = saturate(DynamicSwizzle(heightSample, heightChannel));
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleMaterialDisplacementOffset(MaterialEvalInfo materialInfo, float2 uv, CullingCameraInfo camera, float depth)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return 0.0f;
    }

    const bool heightFromBaseAlpha = (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u;
    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(
        heightFromBaseAlpha ? materialInfo.baseColorTextureIndex : materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(
        heightFromBaseAlpha ? materialInfo.baseColorSamplerIndex : materialInfo.heightSamplerIndex)];
    const float2 uvDerivative = ReyesEstimateUvDerivative(materialInfo, camera, depth);
    const float4 heightSample = heightTexture.SampleGrad(heightSampler, uv, float2(uvDerivative.x, 0.0f), float2(0.0f, uvDerivative.y));
    const uint heightChannel = heightFromBaseAlpha ? 3u : materialInfo.heightChannel;
    const float heightValue = saturate(DynamicSwizzle(heightSample, heightChannel));
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleDisplacementOffset(MaterialInfo materialInfo, float3 positionOS, float2 uv, CullingCameraInfo camera, float depth)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
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
    bool useTerrainDerivatives,
    float3 terrainDpdxWS,
    float3 terrainDpdyWS)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return 0.0f;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        // Geometric displacement must be edge-deterministic. Per-microtriangle
        // projected derivatives can choose different RVT clips for the same
        // shared edge point, so terrain RVT mip selection uses the point itself.
        ReyesEstimateStableTerrainDerivatives(camera, positionOS, terrainDpdxWS, terrainDpdyWS);
        const float heightValue = saturate(TerrainSampleGeometricHeightRvtOnlyOrDirectFallback(materialInfo.terrainSetIndex, positionOS, terrainDpdxWS, terrainDpdyWS));
        return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
    }

    return ReyesSampleMaterialDisplacementOffset(materialInfo, uv, camera, depth);
}

float ReyesSampleDisplacementOffset(MaterialEvalInfo materialInfo, float3 positionOS, float2 uv, CullingCameraInfo camera, float depth)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
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
    bool useTerrainDerivatives,
    float3 terrainDpdxWS,
    float3 terrainDpdyWS)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return 0.0f;
    }

    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u)
    {
        // Geometric displacement must be edge-deterministic. Per-microtriangle
        // projected derivatives can choose different RVT clips for the same
        // shared edge point, so terrain RVT mip selection uses the point itself.
        ReyesEstimateStableTerrainDerivatives(camera, positionOS, terrainDpdxWS, terrainDpdyWS);
        const float heightValue = saturate(TerrainSampleGeometricHeightRvtOnlyOrDirectFallback(materialInfo.terrainSetIndex, positionOS, terrainDpdxWS, terrainDpdyWS));
        return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
    }

    return ReyesSampleMaterialDisplacementOffset(materialInfo, uv, camera, depth);
}

float3 ReyesApplyGeometricDisplacement(MaterialInfo materialInfo, float3 positionOS, float3 normalOS, float2 uv, CullingCameraInfo camera, float depth)
{
    const float displacementOffset = ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth);
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
    const float displacementOffset = ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth) *
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
    bool useTerrainDerivatives,
    float3 terrainDpdxWS,
    float3 terrainDpdyWS,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance)
{
    const float displacementOffset = ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useTerrainDerivatives,
        terrainDpdxWS,
        terrainDpdyWS) * CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, positionWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialInfo materialInfo,
    float3 positionOS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useTerrainDerivatives,
    float3 terrainDpdxWS,
    float3 terrainDpdyWS)
{
    const float displacementOffset = ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useTerrainDerivatives,
        terrainDpdxWS,
        terrainDpdyWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(MaterialEvalInfo materialInfo, float3 positionOS, float3 normalOS, float2 uv, CullingCameraInfo camera, float depth)
{
    const float displacementOffset = ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth);
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
    const float displacementOffset = ReyesSampleDisplacementOffset(materialInfo, positionOS, uv, camera, depth) *
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
    bool useTerrainDerivatives,
    float3 terrainDpdxWS,
    float3 terrainDpdyWS,
    float reyesFadeStartDistance,
    float reyesFadeEndDistance)
{
    const float displacementOffset = ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useTerrainDerivatives,
        terrainDpdxWS,
        terrainDpdyWS) * CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, positionWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float3 ReyesApplyGeometricDisplacement(
    MaterialEvalInfo materialInfo,
    float3 positionOS,
    float3 normalOS,
    float2 uv,
    CullingCameraInfo camera,
    float depth,
    bool useTerrainDerivatives,
    float3 terrainDpdxWS,
    float3 terrainDpdyWS)
{
    const float displacementOffset = ReyesSampleDisplacementOffset(
        materialInfo,
        positionOS,
        uv,
        camera,
        depth,
        useTerrainDerivatives,
        terrainDpdxWS,
        terrainDpdyWS);
    return positionOS + normalize(normalOS) * displacementOffset;
}

float ReyesSampleMaterialDisplacementOffset(MaterialInfo materialInfo, float2 uv)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return 0.0f;
    }

    const bool heightFromBaseAlpha = (materialInfo.materialFlags & MATERIAL_HEIGHT_FROM_BASE_ALPHA) != 0u;
    Texture2D<float4> heightTexture = ResourceDescriptorHeap[NonUniformResourceIndex(
        heightFromBaseAlpha ? materialInfo.baseColorTextureIndex : materialInfo.heightMapIndex)];
    SamplerState heightSampler = SamplerDescriptorHeap[NonUniformResourceIndex(
        heightFromBaseAlpha ? materialInfo.baseColorSamplerIndex : materialInfo.heightSamplerIndex)];
    const float4 heightSample = heightTexture.SampleLevel(heightSampler, uv, 0.0f);
    const uint heightChannel = heightFromBaseAlpha ? 3u : materialInfo.heightChannel;
    const float heightValue = saturate(DynamicSwizzle(heightSample, heightChannel));
    return lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, heightValue);
}

float ReyesSampleDisplacementOffset(MaterialInfo materialInfo, float3 positionOS, float2 uv)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
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
    const float displacementOffset = ReyesSampleDisplacementOffset(materialInfo, positionOS, uv);
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
                CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, patchPosition0WS);
            patchPosition1 += patchNormal1 * lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height1)) *
                CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, patchPosition1WS);
            patchPosition2 += patchNormal2 * lerp(materialInfo.geometricDisplacementMin, materialInfo.geometricDisplacementMax, saturate(height2)) *
                CLodReyesDisplacementFade(reyesFadeStartDistance, reyesFadeEndDistance, camera, patchPosition2WS);
        }
        else
        {
            float3 terrainDpdxWS = 0.0f.xxx;
            float3 terrainDpdyWS = 0.0f.xxx;
            const bool useTerrainDerivatives = ReyesEstimateTerrainDerivativesFromPatch(
                camera,
                patchPosition0,
                patchPosition1,
                patchPosition2,
                terrainDpdxWS,
                terrainDpdyWS);
            const float2 patchUv0 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary0);
            const float2 patchUv1 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary1);
            const float2 patchUv2 = ReyesInterpolateFloat2Precise(sourceUv0, sourceUv1, sourceUv2, sourceBary2);
            const float3 patchPosition0WS = mul(float4(patchPosition0, 1.0f), objectModelMatrix).xyz;
            const float3 patchPosition1WS = mul(float4(patchPosition1, 1.0f), objectModelMatrix).xyz;
            const float3 patchPosition2WS = mul(float4(patchPosition2, 1.0f), objectModelMatrix).xyz;
            patchPosition0 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition0, patchPosition0WS, patchNormal0, patchUv0, camera, patchDepth, useTerrainDerivatives, terrainDpdxWS, terrainDpdyWS, reyesFadeStartDistance, reyesFadeEndDistance);
            patchPosition1 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition1, patchPosition1WS, patchNormal1, patchUv1, camera, patchDepth, useTerrainDerivatives, terrainDpdxWS, terrainDpdyWS, reyesFadeStartDistance, reyesFadeEndDistance);
            patchPosition2 = ReyesApplyGeometricDisplacement(materialInfo, patchPosition2, patchPosition2WS, patchNormal2, patchUv2, camera, patchDepth, useTerrainDerivatives, terrainDpdxWS, terrainDpdyWS, reyesFadeStartDistance, reyesFadeEndDistance);
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
