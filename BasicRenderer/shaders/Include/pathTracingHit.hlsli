#ifndef PATH_TRACING_HIT_HLSLI
#define PATH_TRACING_HIT_HLSLI

// The CLod material resolve as DXR hit shaders use it: no wave ops, no derivatives, and streaming requests from primary hits
// only. Include this before anything that pulls in utilities.hlsli, since these defines shape what it compiles.
#if defined(__UTILITY_HLSL__)
#error pathTracingHit.hlsli must be included before utilities.hlsli
#endif
#if defined(PSO_TERRAIN) || defined(PSO_CLOD_SKINNING) || defined(PSO_CLOD_VOXEL) || defined(PSO_CLOD_REYES_PATCH)
#error Path hits pick terrain per closest-hit entry and only trace static triangles
#endif

#define CLOD_RESOLVE_PATH_HIT 1
// Diagnostic cost breakdowns, set only through a live PSO recompile: 1 constant material, 2 no shadow rays, 3 no alpha tests,
// 4 primary rays only, 5 no alpha tests on hierarchy roots' instances. 0 is the real path tracer.
#ifndef PATH_TRACE_EXPERIMENT
#define PATH_TRACE_EXPERIMENT 0
#endif
#define CLOD_RESOLVE_RAY_HIT 1
#define VISUTIL_COLOR_ONLY_GBUFFER_EVAL 1
#define PSO_NORMAL_MAP 1
#define PSO_OPENPBR_COAT_COLOR_TEXTURE 1
#define PSO_OPENPBR_COAT_WEIGHT_TEXTURE 1
#define PSO_OPENPBR_COAT_ROUGHNESS_TEXTURE 1
#define PSO_OPENPBR_FUZZ_COLOR_TEXTURE 1
#define PSO_OPENPBR_FUZZ_WEIGHT_TEXTURE 1
#define PSO_OPENPBR_FUZZ_ROUGHNESS_TEXTURE 1

static uint4 g_clodRtPackedCluster;
// The hit's assembly transform (a part portal occurrence), from its TLAS row; sentinel for a root's own clusters.
static uint g_clodRtHitTransformIndex = 0xFFFFFFFFu;
// Why CLodResolvePathHit last returned false, for the pixel watch: 1 primitive past the meshlet's triangles (count and
// material in g_clodPathHitFailureDetail), 2 terrain kind mismatch (the material there).
static uint g_clodPathHitFailure = 0u;
static uint g_clodPathHitFailureDetail = 0u;

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/visUtilCommon.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"
#include "include/clodResolveCommon.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include "include/clodRayTracingScene.hlsli"

// The pass's descriptor index for the CLAS info buffer, by default in the same root constant the compute RT passes use.
#ifndef CLOD_PATH_HIT_CLAS_INFO_SRV
#define CLOD_PATH_HIT_CLAS_INFO_SRV CLOD_RT_SEC_CLAS_INFO_SRV
#endif

struct CLodPathHitSurface
{
    MaterialInputs materialInputs;
    uint materialFlags;
    float3 geometricNormalWS; // On the same side as interpolatedNormalWS, not necessarily towards the ray.
    float3 interpolatedNormalWS; // Before normal mapping.
    bool frontFace;
    float2 motionVector; // Primary hits only: NDC, current minus previous. Zero past that.
};

// Barycentric derivatives for a footprint on the triangle's plane: coneWidth across the ray and stretched along it where the
// ray comes in at a grazing angle. Raster's screen derivatives have this shape, so a pixel-sized cone picks the mips it would.
BarycentricDeriv CLodPathHitBarycentrics(float2 barycentrics, float3 edge1WS, float3 edge2WS, float3 rayDirection, float coneWidth, out float3 windingNormalWS)
{
    const float3 areaVector = cross(edge1WS, edge2WS);
    const float areaLengthSq = max(dot(areaVector, areaVector), 1e-20f);
    windingNormalWS = areaVector * rsqrt(areaLengthSq);

    float3 acrossRay = cross(windingNormalWS, rayDirection);
    const float sinIncidenceSq = dot(acrossRay, acrossRay);
    // Head on, every direction on the plane is across the ray, and the cross product above has no length to normalize.
    acrossRay = sinIncidenceSq > 1e-6f ? acrossRay * rsqrt(sinIncidenceSq) : normalize(edge1WS);
    const float incidence = dot(windingNormalWS, rayDirection);
    // Raster's x and y derivatives cross to point away from the viewer, and cotangent_frame_from_derivs takes its sign from that.
    const float3 alongRay = cross(incidence < 0.0f ? -windingNormalWS : windingNormalWS, acrossRay);
    const float3 footprintX = acrossRay * coneWidth;
    const float3 footprintY = alongRay * coneWidth / max(abs(incidence), 0.05f);

    BarycentricDeriv bary;
    bary.m_lambda = float3(1.0f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    const float x1 = dot(cross(footprintX, edge2WS), areaVector) / areaLengthSq;
    const float x2 = dot(cross(edge1WS, footprintX), areaVector) / areaLengthSq;
    const float y1 = dot(cross(footprintY, edge2WS), areaVector) / areaLengthSq;
    const float y2 = dot(cross(edge1WS, footprintY), areaVector) / areaLengthSq;
    bary.m_ddx = float3(-x1 - x2, x1, x2);
    bary.m_ddy = float3(-y1 - y2, y1, y2);
    return bary;
}

// instanceID and instanceIndex are InstanceID() and InstanceIndex(), and clasSlot is ClusterID(). coneWidth is the footprint's width at this hit, in world units.
// isPrimaryHit decides whether the hit may feed streaming and use parallax. isTerrain must be a literal at each closest-hit
// entry, so the other material path folds away. False if the hit doesn't resolve to geometry of that kind.
bool CLodResolvePathHit(
    uint instanceID,
    uint instanceIndex,
    uint clasSlot,
    uint primitive,
    float2 barycentrics,
    bool frontFace,
    float3 worldPosition,
    float3 rayDirection,
    float coneWidth,
    bool isPrimaryHit,
    bool isTerrain,
    out CLodPathHitSurface hit)
{
    hit = (CLodPathHitSurface)0;
    hit.frontFace = frontFace;
    g_clodPathHitIsPrimary = isPrimaryHit;

    const uint drawRecord = CLodRtInstanceDraw(instanceID);
    g_clodRtHitTransformIndex = CLodRtInstanceTransform(instanceID, instanceIndex);
    StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_PATH_HIT_CLAS_INFO_SRV];
    const CLodRtClasInfo info = clasInfo[clasSlot];
    g_clodRtPackedCluster = CLodPackVisibleCluster(0u, drawRecord, info.localMeshlet, CLodRtClasGroup(info), info.slabDescriptor,
        info.pageOffsetUnits << CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT, CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX);

    const MeshletResolveData md = LoadMeshletResolveData(0u);
    if (primitive >= md.triangleCount)
    {
        g_clodPathHitFailure = 1u;
        g_clodPathHitFailureDetail = md.triangleCount | (md.materialDataIndex << 8u);
        return false;
    }

    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materials[md.materialDataIndex];
    if (isTerrain != ((materialInfo.materialFlags & MATERIAL_TERRAIN) != 0u))
    {
        g_clodPathHitFailure = 2u;
        g_clodPathHitFailureDetail = md.materialDataIndex;
        return false;
    }
    if (!isPrimaryHit)
    {
        materialInfo.materialFlags &= ~MATERIAL_PARALLAX; // It takes its view direction from the main camera.
    }
    const uint materialFlags = materialInfo.materialFlags;

    const uint3 triIdx = DecodeTriangleCompact(primitive, md);
    const ClodResolvedTriangle tri = CLodResolveLoadTriangle(triIdx, md, g_clodRtHitTransformIndex);
    const PerObjectBuffer obj = LoadInstanceTransformForDrawWithAssemblyTransform(md.objAndMesh.x, g_clodRtHitTransformIndex);
    const float3 edge1WS = mul(float4(tri.p1 - tri.p0, 0.0f), obj.model).xyz;
    const float3 edge2WS = mul(float4(tri.p2 - tri.p0, 0.0f), obj.model).xyz;
    float3 windingNormalWS;
    const BarycentricDeriv bary = CLodPathHitBarycentrics(barycentrics, edge1WS, edge2WS, rayDirection, coneWidth, windingNormalWS);

    ClodResolvedSurface surface = CLodResolveInterpolateSurface(
        tri,
        tri.p0,
        tri.p1,
        tri.p2,
        bary,
        bary,
        (BarycentricDeriv)0,
        false,
        obj,
        md,
        materialFlags,
        false,
        false);
    hit.interpolatedNormalWS = surface.worldNormal;
    // Winding alone says nothing on mirrored instances, whose normal matrix keeps normals outward while their edges flip.
    hit.geometricNormalWS = dot(windingNormalWS, surface.worldNormal) < 0.0f ? -windingNormalWS : windingNormalWS;

    if (isPrimaryHit)
    {
        ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const Camera camera = cameras[perFrame.mainCameraIndex];
        // From the interpolated position, not the ray's: this and last frame's position then differ by object motion alone, where
        // the ray's position would add its own rounding at distance.
        hit.motionVector = ComputeClodMotionVector(
            surface.previousPosOS,
            surface.worldPosition,
            obj.prevModel,
            mul(camera.view, camera.unjitteredProjection),
            mul(camera.prevView, camera.prevUnjitteredProjection));
    }

    surface.worldPosition = worldPosition;

    float2 unusedDebugUv;
    float unusedDebugUvValid;
    float2 unusedDebugUvDerivative;
#if PATH_TRACE_EXPERIMENT == 1 // Cost breakdown only: a constant material instead of evaluating it.
    hit.materialInputs = (MaterialInputs)0;
    hit.materialInputs.albedo = 0.5f.xxx;
    hit.materialInputs.normalWS = surface.worldNormal;
    hit.materialInputs.roughness = 0.8f;
    hit.materialInputs.opacity = 1.0f;
    hit.materialInputs.ambientOcclusion = 1.0f;
    if (false)
#endif
    hit.materialInputs = CLodResolveEvaluateMaterial(
        materialInfo,
        materialFlags,
        md,
        triIdx,
        bary,
        (Camera)0,
        0u.xx,
        0.0f.xx,
        false,
        false,
        1.0f,
        isTerrain,
        surface,
        unusedDebugUv,
        unusedDebugUvValid,
        unusedDebugUvDerivative);
    hit.materialInputs.glintEnabled = 0u;
    hit.materialFlags = materialFlags;
    return true;
}

// What lightFragmentColor and the BSDF read, for a surface CLodResolvePathHit produced. No GTAO: it's a screen-space input.
void CLodPathHitFillFragment(CLodPathHitSurface hit, float3 worldPosition, float3 rayDirection, uint2 pixel, out FragmentInfo fragment)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];

    fragment = (FragmentInfo)0;
    fragment.pixelCoords = pixel;
    fragment.fragPosWorldSpace = worldPosition;
    fragment.fragPosViewSpace = mul(float4(worldPosition, 1.0f), cameras[perFrame.mainCameraIndex].view).xyz;
    FillFragmentInfoDirect(fragment, hit.materialInputs, -rayDirection, float2(pixel), false, false, hit.frontFace, hit.materialFlags);
}

#endif // PATH_TRACING_HIT_HLSLI
