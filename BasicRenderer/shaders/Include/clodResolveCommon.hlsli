#ifndef __CLOD_RESOLVE_COMMON_HLSLI__
#define __CLOD_RESOLVE_COMMON_HLSLI__

//#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "Common/defines.h"
#include "include/cbuffers.hlsli"
#include "include/loadingUtils.hlsli"
#include "include/vertex.hlsli"
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

#include "include/clodResolveTypes.hlsli"
#include "include/clodBarycentrics.hlsli"
#include "include/clodResolveTerrainDerivatives.hlsli"
#include "include/clodResolveMeshlet.hlsli"
#include "include/clodMaterialUvs.hlsli"
#include "include/clodResolveVoxel.hlsli"
#include "include/clodResolveReyes.hlsli"

#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
typedef MaterialEvalInfo ClodResolveMaterialInfo;
#else
typedef MaterialInfo ClodResolveMaterialInfo;
#endif

// Reyes patches write a patch index to the visibility buffer. This swaps it for the source cluster
// and triangle, and finds which part of that triangle the micro-triangle covers.
bool CLodResolvePatchKey(
    inout uint clusterIndex,
    inout uint meshletTriangleIndex,
    inout float3 patchDomain0,
    inout float3 patchDomain1,
    inout float3 patchDomain2,
    inout float3 microTrianglePatchDomain0,
    inout float3 microTrianglePatchDomain1,
    inout float3 microTrianglePatchDomain2)
{
    const uint reyesMicroTriangleIndex = meshletTriangleIndex;
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

    return true;
}

// The attributes of one triangle's three vertices, skinned where the material needs it.
struct ClodResolvedTriangle
{
    float3 p0, p1, p2;
    float3 previousP0, previousP1, previousP2;
    float3 n0, n1, n2;
    float4 t0, t1, t2;
    float3 c0, c1, c2;
    bool hasVertexColor;
};

ClodResolvedTriangle CLodResolveLoadTriangle(uint3 triIdx, MeshletResolveData md, uint assemblyTransformIndex)
{
    ClodResolvedTriangle tri;
    tri.p0 = DecodeCompressedPosition(triIdx.x, md);
    tri.p1 = DecodeCompressedPosition(triIdx.y, md);
    tri.p2 = DecodeCompressedPosition(triIdx.z, md);
    tri.previousP0 = tri.p0;
    tri.previousP1 = tri.p1;
    tri.previousP2 = tri.p2;
    tri.n0 = DecodeCompressedNormal(triIdx.x, md);
    tri.n1 = DecodeCompressedNormal(triIdx.y, md);
    tri.n2 = DecodeCompressedNormal(triIdx.z, md);
    tri.t0 = DecodeCompressedTangentFrame(triIdx.x, tri.n0, md);
    tri.t1 = DecodeCompressedTangentFrame(triIdx.y, tri.n1, md);
    tri.t2 = DecodeCompressedTangentFrame(triIdx.z, tri.n2, md);
    tri.c0 = 1.0f.xxx;
    tri.c1 = 1.0f.xxx;
    tri.c2 = 1.0f.xxx;
#if defined(PSO_CLOD_VERTEX_COLOR)
    tri.hasVertexColor = true;
    tri.c0 = DecodeCompressedColor(triIdx.x, md);
    tri.c1 = DecodeCompressedColor(triIdx.y, md);
    tri.c2 = DecodeCompressedColor(triIdx.z, md);
#else
    tri.hasVertexColor = (md.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u;
    if (tri.hasVertexColor)
    {
        tri.c0 = DecodeCompressedColor(triIdx.x, md);
        tri.c1 = DecodeCompressedColor(triIdx.y, md);
        tri.c2 = DecodeCompressedColor(triIdx.z, md);
    }
#endif
#if defined(PSO_CLOD_SKINNING)
    ApplyPreviousClodSkinningToPosition(triIdx.x, md, assemblyTransformIndex, tri.previousP0);
    ApplyPreviousClodSkinningToPosition(triIdx.y, md, assemblyTransformIndex, tri.previousP1);
    ApplyPreviousClodSkinningToPosition(triIdx.z, md, assemblyTransformIndex, tri.previousP2);
    ApplyClodSkinningToFrame(triIdx.x, md, assemblyTransformIndex, tri.p0, tri.n0, tri.t0);
    ApplyClodSkinningToFrame(triIdx.y, md, assemblyTransformIndex, tri.p1, tri.n1, tri.t1);
    ApplyClodSkinningToFrame(triIdx.z, md, assemblyTransformIndex, tri.p2, tri.n2, tri.t2);
#endif

    return tri;
}

// The micro-triangle's corners, in the source triangle's object space, with displacement applied.
void CLodResolveBuildReyesPatchPositions(
    ClodResolvedTriangle tri,
    uint3 triIdx,
    MeshletResolveData md,
    PerObjectBuffer obj,
    Camera cam,
    PerFrameBuffer perFrame,
    ClodResolveMaterialInfo materialInfo,
    MaterialEvalInfo reyesMaterialInfo,
    float3 patchDomain0,
    float3 patchDomain1,
    float3 patchDomain2,
    float3 microTrianglePatchDomain0,
    float3 microTrianglePatchDomain1,
    float3 microTrianglePatchDomain2,
    out float3 patchPos0,
    out float3 patchPos1,
    out float3 patchPos2)
{
    const float3 sourcePatchBary0 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain0, patchDomain0, patchDomain1, patchDomain2);
    const float3 sourcePatchBary1 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain1, patchDomain0, patchDomain1, patchDomain2);
    const float3 sourcePatchBary2 = ReyesComposeSourceBarycentricsPoint(microTrianglePatchDomain2, patchDomain0, patchDomain1, patchDomain2);

    patchPos0 = tri.p0 * sourcePatchBary0.x + tri.p1 * sourcePatchBary0.y + tri.p2 * sourcePatchBary0.z;
    patchPos1 = tri.p0 * sourcePatchBary1.x + tri.p1 * sourcePatchBary1.y + tri.p2 * sourcePatchBary1.z;
    patchPos2 = tri.p0 * sourcePatchBary2.x + tri.p1 * sourcePatchBary2.y + tri.p2 * sourcePatchBary2.z;

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
            (-mul(float4(tri.p0, 1.0f), objectToView).z
            + -mul(float4(tri.p1, 1.0f), objectToView).z
            + -mul(float4(tri.p2, 1.0f), objectToView).z) / 3.0f,
            max(cam.zNear, 1.0e-3f));
        const float2 heightUv0 = DecodeCompressedUV(triIdx.x, materialInfo.heightUvSetIndex, md);
        const float2 heightUv1 = DecodeCompressedUV(triIdx.y, materialInfo.heightUvSetIndex, md);
        const float2 heightUv2 = DecodeCompressedUV(triIdx.z, materialInfo.heightUvSetIndex, md);
        const float3 patchNormal0 = normalize(tri.n0 * sourcePatchBary0.x + tri.n1 * sourcePatchBary0.y + tri.n2 * sourcePatchBary0.z);
        const float3 patchNormal1 = normalize(tri.n0 * sourcePatchBary1.x + tri.n1 * sourcePatchBary1.y + tri.n2 * sourcePatchBary1.z);
        const float3 patchNormal2 = normalize(tri.n0 * sourcePatchBary2.x + tri.n1 * sourcePatchBary2.y + tri.n2 * sourcePatchBary2.z);
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
            reyesMaterialInfo,
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

}

// The surface at this pixel, before its material is applied.
struct ClodResolvedSurface
{
    float3 posOS;
    float3 previousPosOS;
    float3 dpdxOS;
    float3 dpdyOS;
    float3 worldPosition;
    float3 dpdx;
    float3 dpdy;
    float3 normalOS;
    float3 vertexColor;
    float3x3 normalMatrix;
    float3 interpolatedWorldNormal;
    float3 worldNormal;
    float4 tangentWS;
};

ClodResolvedSurface CLodResolveInterpolateSurface(
    ClodResolvedTriangle tri,
    float3 evalPos0,
    float3 evalPos1,
    float3 evalPos2,
    BarycentricDeriv bary,
    BarycentricDeriv positionBary,
    BarycentricDeriv sourceDerivativeBary,
    bool useSourceDerivativeBary,
    PerObjectBuffer obj,
    MeshletResolveData md,
    uint materialFlags,
    bool isReyesPatch,
    bool useReyesGeometricNormal)
{
    float3 interpPosX = InterpolateWithDeriv(positionBary, evalPos0.x, evalPos1.x, evalPos2.x);
    float3 interpPosY = InterpolateWithDeriv(positionBary, evalPos0.y, evalPos1.y, evalPos2.y);
    float3 interpPosZ = InterpolateWithDeriv(positionBary, evalPos0.z, evalPos1.z, evalPos2.z);

    float3 posOS = float3(interpPosX.x, interpPosY.x, interpPosZ.x);
    // Use the same source-triangle barycentrics at the previous palette. This
    // captures skeletal deformation instead of treating the current skinned
    // object-space position as though it also existed last frame.
    float3 previousPosOS =
        tri.previousP0 * bary.m_lambda.x +
        tri.previousP1 * bary.m_lambda.y +
        tri.previousP2 * bary.m_lambda.z;
#if !defined(PSO_CLOD_SKINNING)
    // Static Reyes patches resolve the current position from the displaced
    // micro-triangle surface. Reprojecting against the undisplaced source
    // triangle gives DLSS/TAA a bogus per-frame velocity equal to the static
    // displacement offset, which shows up as jitter/swimming when camera jitter
    // is enabled. For non-skinned objects, the same displaced object-space
    // surface existed last frame; obj.prevModel still accounts for object motion.
    if (isReyesPatch)
    {
        previousPosOS = posOS;
    }
#endif
    float3 dpdxOS = float3(interpPosX.y, interpPosY.y, interpPosZ.y);
    float3 dpdyOS = float3(interpPosX.z, interpPosY.z, interpPosZ.z);
    if (useSourceDerivativeBary)
    {
        float3 sourceInterpPosX = InterpolateWithDeriv(sourceDerivativeBary, tri.p0.x, tri.p1.x, tri.p2.x);
        float3 sourceInterpPosY = InterpolateWithDeriv(sourceDerivativeBary, tri.p0.y, tri.p1.y, tri.p2.y);
        float3 sourceInterpPosZ = InterpolateWithDeriv(sourceDerivativeBary, tri.p0.z, tri.p1.z, tri.p2.z);
        dpdxOS = float3(sourceInterpPosX.y, sourceInterpPosY.y, sourceInterpPosZ.y);
        dpdyOS = float3(sourceInterpPosX.z, sourceInterpPosY.z, sourceInterpPosZ.z);
    }

    float3 worldPosition = mul(float4(posOS, 1.0f), obj.model).xyz;

    float3x3 model3x3 = (float3x3)obj.model;
    float3 dpdx = 0.0f.xxx;
    float3 dpdy = 0.0f.xxx;
    uint derivativeMaterialFlags = MATERIAL_NORMAL_MAP | MATERIAL_PARALLAX;
#if defined(PSO_TERRAIN) || defined(CLOD_RESOLVE_PATH_HIT)
    derivativeMaterialFlags |= MATERIAL_TERRAIN;
#endif
    if ((materialFlags & derivativeMaterialFlags) != 0u)
    {
        dpdx = mul(dpdxOS, model3x3);
        dpdy = mul(dpdyOS, model3x3);
    }

    float interpNX = InterpolateWithDeriv(bary, tri.n0.x, tri.n1.x, tri.n2.x).x;
    float interpNY = InterpolateWithDeriv(bary, tri.n0.y, tri.n1.y, tri.n2.y).x;
    float interpNZ = InterpolateWithDeriv(bary, tri.n0.z, tri.n1.z, tri.n2.z).x;
    float3 normalOS = normalize(float3(interpNX, interpNY, interpNZ));
    float3 vertexColor = 1.0f.xxx;
    if (tri.hasVertexColor)
    {
        vertexColor = float3(
            InterpolateWithDeriv(bary, tri.c0.x, tri.c1.x, tri.c2.x).x,
            InterpolateWithDeriv(bary, tri.c0.y, tri.c1.y, tri.c2.y).x,
            InterpolateWithDeriv(bary, tri.c0.z, tri.c1.z, tri.c2.z).x);
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
        float interpTX = InterpolateWithDeriv(bary, tri.t0.x, tri.t1.x, tri.t2.x).x;
        float interpTY = InterpolateWithDeriv(bary, tri.t0.y, tri.t1.y, tri.t2.y).x;
        float interpTZ = InterpolateWithDeriv(bary, tri.t0.z, tri.t1.z, tri.t2.z).x;
        float interpTW = InterpolateWithDeriv(bary, tri.t0.w, tri.t1.w, tri.t2.w).x;
        tangentWS = float4(normalize(mul(float3(interpTX, interpTY, interpTZ), normalMatrix)), interpTW < 0.0f ? -1.0f : 1.0f);
    }

    ClodResolvedSurface surface;
    surface.posOS = posOS;
    surface.previousPosOS = previousPosOS;
    surface.dpdxOS = dpdxOS;
    surface.dpdyOS = dpdyOS;
    surface.worldPosition = worldPosition;
    surface.dpdx = dpdx;
    surface.dpdy = dpdy;
    surface.normalOS = normalOS;
    surface.vertexColor = vertexColor;
    surface.normalMatrix = normalMatrix;
    surface.interpolatedWorldNormal = interpolatedWorldNormal;
    surface.worldNormal = worldNormal;
    surface.tangentWS = tangentWS;
    return surface;
}

// Samples the material. Terrain replaces the surface's derivatives with ones from its tangent plane.
MaterialInputs CLodResolveEvaluateMaterial(
    ClodResolveMaterialInfo materialInfo,
    uint materialFlags,
    MeshletResolveData md,
    uint3 triIdx,
    BarycentricDeriv bary,
    Camera cam,
    uint2 pixel,
    float2 winSize,
    bool isReyesPatch,
    bool useReyesGeometricNormal,
    float reyesObjectNormalMapBlend,
#if defined(CLOD_RESOLVE_PATH_HIT)
    bool isTerrain, // A literal at each closest-hit entry, so after inlining the other branch is gone.
#endif
    inout ClodResolvedSurface surface,
    out float2 materialDebugUv,
    out float materialDebugUvValid,
    out float2 materialDebugUvDerivative)
{
    materialDebugUv = 0.0f.xx;
    materialDebugUvValid = 0.0f;
    materialDebugUvDerivative = 0.0f.xx;

    const float3 posOS = surface.posOS;
    const float3 dpdxOS = surface.dpdxOS;
    const float3 dpdyOS = surface.dpdyOS;
    const float3 worldPosition = surface.worldPosition;
    float3 dpdx = surface.dpdx;
    float3 dpdy = surface.dpdy;
    const float3 normalOS = surface.normalOS;
    const float3 vertexColor = surface.vertexColor;
    const float3x3 normalMatrix = surface.normalMatrix;
    const float3 interpolatedWorldNormal = surface.interpolatedWorldNormal;
    const float3 worldNormal = surface.worldNormal;
    const float4 tangentWS = surface.tangentWS;

    MaterialInputs materialInputs;
#if defined(CLOD_RESOLVE_PATH_HIT)
    if (isTerrain)
    {
#endif
#if defined(PSO_TERRAIN) || defined(CLOD_RESOLVE_PATH_HIT)
    if (!isReyesPatch)
    {
        float3 terrainDpdxWS;
        float3 terrainDpdyWS;
#if !defined(CLOD_RESOLVE_RAY_HIT) // This reprojects the camera ray through the pixel, which isn't this ray.
        if (CLodResolveEstimateTerrainTangentPlaneDerivatives(cam, pixel, winSize, worldPosition, worldNormal, terrainDpdxWS, terrainDpdyWS))
        {
            dpdx = terrainDpdxWS;
            dpdy = terrainDpdyWS;
        }
#endif
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
#endif
#if defined(CLOD_RESOLVE_PATH_HIT)
    }
    else
    {
#endif
#if !defined(PSO_TERRAIN) || defined(CLOD_RESOLVE_PATH_HIT)
    const bool isObjectTriplanarMaterial = (materialFlags & MATERIAL_OBJECT_TRIPLANAR_STOCHASTIC) != 0u;
    const bool needsObjectSpaceNormalUv =
        (materialFlags & (MATERIAL_NORMAL_MAP | MATERIAL_OBJECT_SPACE_NORMAL_MAP)) ==
        (MATERIAL_NORMAL_MAP | MATERIAL_OBJECT_SPACE_NORMAL_MAP);
#if defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    const bool needsMaterialDebugUv = false;
#else
    const bool needsMaterialDebugUv = true;
#endif
#if MATERIAL_EVAL_COMPILED_UV_REQUIREMENTS
#if defined(PSO_BASE_COLOR_TEXTURE) || defined(PSO_OPACITY_TEXTURE) || defined(PSO_METALLIC_TEXTURE) || \
    defined(PSO_ROUGHNESS_TEXTURE) || defined(PSO_NORMAL_MAP) || defined(PSO_AO_TEXTURE) || \
    defined(PSO_EMISSIVE_TEXTURE) || defined(PSO_PARALLAX) || defined(PSO_OPENPBR_COAT_TEXTURES) || \
    defined(PSO_OPENPBR_FUZZ_TEXTURES)
    const bool hasCompiledMaterialUvConsumer = true;
#else
    const bool hasCompiledMaterialUvConsumer = false;
#endif
    const bool needsMaterialUvData =
        needsMaterialDebugUv ||
        needsObjectSpaceNormalUv ||
        (!isObjectTriplanarMaterial && hasCompiledMaterialUvConsumer);
#else
    const bool needsMaterialUvData = !isObjectTriplanarMaterial || needsObjectSpaceNormalUv || needsMaterialDebugUv;
#endif
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

    if (needsObjectSpaceNormalUv)
    {
#if defined(PSO_NORMAL_MAP)
        const MaterialUvSample objectNormalUv = GetBoundUvSample(uvCache, uvBindings, MATERIAL_TEXTURE_SLOT_NORMAL);
        Texture2D<float4> objectNormalTexture = ResourceDescriptorHeap[NonUniformResourceIndex(ResolveMaterialTextureDescriptor(materialInfo.normalTextureIndex, materialInfo.normalStreamingTextureID))];
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
#if defined(CLOD_RESOLVE_PATH_HIT)
    }
#endif

    surface.dpdx = dpdx;
    surface.dpdy = dpdy;
    return materialInputs;
}

// Path hits resolve in pathTracingHit.hlsli: they have no visibility key, and this path reaches wave ops and Reyes.
#if !defined(CLOD_RESOLVE_PATH_HIT)
bool ResolveClodCommonSampleFromVisKeyWithFace(uint64_t vis, uint2 pixel, bool isBackface, out ClodResolvedCommonSample sample)
{
    sample = (ClodResolvedCommonSample)0;

    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        return false;
    }

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
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
#if defined(PSO_CLOD_REYES_PATCH)
    if (!CLodResolvePatchKey(
            clusterIndex,
            meshletTriangleIndex,
            patchDomain0,
            patchDomain1,
            patchDomain2,
            microTrianglePatchDomain0,
            microTrianglePatchDomain1,
            microTrianglePatchDomain2))
    {
        return false;
    }
#elif !defined(CLOD_RESOLVE_RAY_HIT) // A ray hit has no index into the visible cluster buffer to range check.
    if (clusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE)
    {
        return false;
    }
#endif

#if defined(CLOD_RESOLVE_RAY_HIT)
    const uint4 packedVisibleCluster = g_clodRtPackedCluster;
#else
    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedVisibleCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
#endif
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
#if !defined(CLOD_RESOLVE_RAY_HIT)
    if (VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint2> visibleClusterTransformIndices =
            ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
        assemblyTransformIndex = visibleClusterTransformIndices[clusterIndex].x;
    }
#else
    assemblyTransformIndex = g_clodRtHitTransformIndex;
#endif

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

    const ClodResolvedTriangle tri = CLodResolveLoadTriangle(triIdx, md, assemblyTransformIndex);

    PerObjectBuffer obj = LoadInstanceTransformForDrawWithAssemblyTransform(md.objAndMesh.x, assemblyTransformIndex);
#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
    StructuredBuffer<ClodResolveMaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialEvalDataBuffer)];
#else
    StructuredBuffer<ClodResolveMaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
#endif
    ClodResolveMaterialInfo materialInfo = materialDataBuffer[md.materialDataIndex];
    MaterialEvalInfo reyesMaterialInfo = (MaterialEvalInfo)0;
#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
    reyesMaterialInfo = materialInfo;
#else
    reyesMaterialInfo.materialFlags = materialInfo.materialFlags;
    reyesMaterialInfo.heightMapIndex = materialInfo.heightMapIndex;
    reyesMaterialInfo.heightSamplerIndex = materialInfo.heightSamplerIndex;
    reyesMaterialInfo.heightMapScale = materialInfo.heightMapScale;
    reyesMaterialInfo.geometricDisplacementMin = materialInfo.geometricDisplacementMin;
    reyesMaterialInfo.geometricDisplacementMax = materialInfo.geometricDisplacementMax;
    reyesMaterialInfo.geometricDisplacementEnabled = materialInfo.geometricDisplacementEnabled;
    reyesMaterialInfo.terrainSetIndex = materialInfo.terrainSetIndex;
    reyesMaterialInfo.heightChannel = materialInfo.heightChannel;
    reyesMaterialInfo.heightUvSetIndex = materialInfo.heightUvSetIndex;
    reyesMaterialInfo.heightStreamingTextureID = materialInfo.heightStreamingTextureID;
    reyesMaterialInfo.reyesUvDensity = materialInfo.reyesUvDensity;
    reyesMaterialInfo.objectSurfaceTexelDensity = materialInfo.objectSurfaceTexelDensity;
    reyesMaterialInfo.objectSurfaceSamplingMode = materialInfo.objectSurfaceSamplingMode;
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
    float4x4 objectToClip = mul(obj.model, cam.viewProjection);
    float4 clip0 = mul(float4(tri.p0, 1.0f), objectToClip);
    float4 clip1 = mul(float4(tri.p1, 1.0f), objectToClip);
    float4 clip2 = mul(float4(tri.p2, 1.0f), objectToClip);
    float3 evalPos0 = tri.p0;
    float3 evalPos1 = tri.p1;
    float3 evalPos2 = tri.p2;
    BarycentricDeriv sourceDerivativeBary = (BarycentricDeriv)0;
    bool useSourceDerivativeBary = false;

    if (isReyesPatch)
    {
        float3 patchPos0;
        float3 patchPos1;
        float3 patchPos2;
        CLodResolveBuildReyesPatchPositions(
            tri,
            triIdx,
            md,
            obj,
            cam,
            perFrame,
            materialInfo,
            reyesMaterialInfo,
            patchDomain0,
            patchDomain1,
            patchDomain2,
            microTrianglePatchDomain0,
            microTrianglePatchDomain1,
            microTrianglePatchDomain2,
            patchPos0,
            patchPos1,
            patchPos2);

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

#if defined(CLOD_RESOLVE_RAY_HIT)
    // Barycentrics come from the hit. Gradients come from the width of the ray cone against the triangle edges, so
    // textures pick a mip for what the ray can actually resolve.
    const float3 rayHitEdge1 = mul(float4(evalPos1 - evalPos0, 0.0f), obj.model).xyz;
    const float3 rayHitEdge2 = mul(float4(evalPos2 - evalPos0, 0.0f), obj.model).xyz;
    const float rayHitDx = g_clodRtConeWidth / max(length(rayHitEdge1), 1e-5f);
    const float rayHitDy = g_clodRtConeWidth / max(length(rayHitEdge2), 1e-5f);
    BarycentricDeriv bary;
    bary.m_lambda = float3(1.0f - g_clodRtBarycentrics.x - g_clodRtBarycentrics.y, g_clodRtBarycentrics.x, g_clodRtBarycentrics.y);
    bary.m_ddx = float3(-rayHitDx, rayHitDx, 0.0f);
    bary.m_ddy = float3(-rayHitDy, 0.0f, rayHitDy);
#else
    BarycentricDeriv bary = CalcFullBary(clip0, clip1, clip2, pixelNdc, winSize);
#endif
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
    const bool clodSurfaceResolveBackface = ClodBarycentricDerivativesAreBackFacing(bary);
#endif

    ClodResolvedSurface surface = CLodResolveInterpolateSurface(
        tri,
        evalPos0,
        evalPos1,
        evalPos2,
        bary,
        positionBary,
        sourceDerivativeBary,
        useSourceDerivativeBary,
        obj,
        md,
        materialFlags,
        isReyesPatch,
        useReyesGeometricNormal);

    float2 materialDebugUv;
    float materialDebugUvValid;
    float2 materialDebugUvDerivative;
    MaterialInputs materialInputs = CLodResolveEvaluateMaterial(
        materialInfo,
        materialFlags,
        md,
        triIdx,
        bary,
        cam,
        pixel,
        winSize,
        isReyesPatch,
        useReyesGeometricNormal,
        reyesObjectNormalMapBlend,
        surface,
        materialDebugUv,
        materialDebugUvValid,
        materialDebugUvDerivative);
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    const float3 reyesDebugSourceBarycentrics = bary.m_lambda;
#endif

    float3 positionVS = mul(float4(surface.worldPosition, 1.0f), cam.view).xyz;

    sample.linearDepth = depth;
    sample.clusterIndex = clusterIndex;
    sample.meshletTriangleIndex = meshletTriangleIndex;
    sample.meshletIndex = md.drawcallAndMeshlet.y;
    sample.geometryGroupIndex = CLodVisibleClusterGroupID(packedVisibleCluster);
    sample.positionWS = surface.worldPosition;
    sample.positionVS = positionVS;
    sample.normalWSBase = surface.worldNormal;
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    sample.normalOS = surface.normalOS;
#endif
    sample.vertexColor = surface.vertexColor;
#if !defined(VISUTIL_COLOR_ONLY_GBUFFER_EVAL)
    sample.materialDebugUv = materialDebugUv;
    sample.materialDebugUvValid = materialDebugUvValid;
    sample.materialDebugUvDerivative = materialDebugUvDerivative;
    sample.reyesDebugSourceBarycentrics = reyesDebugSourceBarycentrics;
#endif
    sample.dpdxWS = surface.dpdx;
    sample.dpdyWS = surface.dpdy;
    sample.motionVector = ComputeClodMotionVector(
        surface.previousPosOS,
        surface.worldPosition,
        obj.prevModel,
        mul(cam.view, cam.unjitteredProjection),
        mul(cam.prevView, cam.prevUnjitteredProjection));
    materialInputs.sourceObjectId = uint2(obj.stableSceneIdLo, obj.stableSceneIdHi);
    materialInputs.sourceMaterialId = materialInfo.sourceMaterialId;
    materialInputs.materialTableIndex = md.materialDataIndex;
    materialInputs.semanticFamily = materialInfo.semanticFamily;
    materialInputs.surfaceFlags = materialInfo.surfaceFlags;
    materialInputs.diagnosticReason = materialInfo.diagnosticReason;
#if defined(VISUTIL_USE_COMPACT_MATERIAL_EVAL)
    sample.materialInfo = (MaterialInfo)0;
#else
    sample.materialInfo = materialInfo;
#endif
    sample.materialFlags = materialFlags;
    sample.materialInputs = materialInputs;
#if defined(VISUTIL_DOUBLE_SIDED_GBUFFER_RESOLVE)
    if (clodSurfaceResolveBackface)
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
    sample.voxelDebugUvDensity = resolvedSample.voxelDebugUvDensity;
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
    sample.voxelDebugUvDensity = resolvedSample.voxelDebugUvDensity;
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
#endif // !CLOD_RESOLVE_PATH_HIT

#endif // __CLOD_RESOLVE_COMMON_HLSLI__
