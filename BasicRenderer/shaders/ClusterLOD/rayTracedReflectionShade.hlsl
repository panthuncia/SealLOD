// Reflections, second stage: shade each hit with the same material resolve and light loop as primary surfaces. The resolve
// is compiled with CLOD_RESOLVE_RAY_HIT, which takes barycentrics from the ray and gradients from a ray cone instead of
// projecting a pixel. Built twice: without PSO_TERRAIN for everything else, and with it for terrain, each skipping the other.
#define CLOD_RESOLVE_RAY_HIT 1
#define CLOD_RT_HIT_LIGHTING 1

static uint4 g_clodRtPackedCluster;
static float2 g_clodRtBarycentrics;
static float g_clodRtConeWidth;
static float g_clodRtSunShadow;

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/visUtilCommon.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"
#include "include/clodResolveCommon.hlsli"
#include "include/lighting.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include "include/clodRayTracingScene.hlsli"
#include "include/clodRayTracingReflectionHit.hlsli"

// RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY. See clodRayTracingHits.hlsli.
#define CLOD_RT_RAYQUERY_FLAGS 2

[numthreads(8, 8, 1)]
void CLodRtReflectionShadeCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (id.x >= perFrame.screenResX || id.y >= perFrame.screenResY) {
        return;
    }
    RWStructuredBuffer<CLodRtReflectionHit> hits = ResourceDescriptorHeap[CLOD_RT_SEC_HITS_UAV];
    const CLodRtReflectionHit hit = hits[id.y * perFrame.screenResX + id.x];
    if (hit.drawRecord == CLOD_RT_REFLECTION_NO_HIT) {
        return;
    }

    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const PerMeshBuffer perMesh = perMeshBuffer[LoadMeshTemplateForDraw(hit.drawRecord).perMeshBufferIndex];
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const uint materialFlags = materials[perMesh.materialDataIndex].materialFlags;
#if defined(PSO_TERRAIN)
    if ((materialFlags & (MATERIAL_TERRAIN)) == 0u) {
        return;
    }
#else
    if ((materialFlags & (MATERIAL_TERRAIN)) != 0u) {
        return;
    }
#endif

    StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_SEC_CLAS_INFO_SRV];
    const CLodRtClasInfo info = clasInfo[hit.clasSlot];
    g_clodRtPackedCluster = CLodPackVisibleCluster(perFrame.mainCameraIndex, hit.drawRecord, info.localMeshlet, info.groupLocalIndex,
        info.slabDescriptor, info.pageOffsetUnits << CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT, CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX);
    g_clodRtBarycentrics = hit.barycentrics;
    g_clodRtConeWidth = hit.coneWidth;

    ClodGBufferColorSample sample;
    const uint64_t key = PackVisKey(hit.t, 0u, hit.primitive);
    if (!ResolveClodGBufferColorSampleFromVisKeyWithFace(key, id.xy, false, sample)) {
        return;
    }

    const float3 position = hit.origin + hit.direction * hit.t;
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];

    FragmentInfo fragment = (FragmentInfo)0;
    fragment.pixelCoords = id.xy;
    fragment.fragPosWorldSpace = position;
    fragment.fragPosViewSpace = mul(float4(position, 1.0f), camera.view).xyz;
    FillFragmentInfoDirect(fragment, sample.materialInputs, -hit.direction, float2(id.xy), false, false, hit.frontFace != 0u, materialFlags);

    // Sun visibility by ray: shadow maps only have pages for what the camera sees.
    g_clodRtSunShadow = 0.0f;
    StructuredBuffer<unsigned int> activeLights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    for (uint i = 0u; i < perFrame.numLights; ++i) {
        const LightInfo light = lights[activeLights[i]];
        if (light.type == 2u && light.shadowCaster) {
            RaytracingAccelerationStructure scene = ResourceDescriptorHeap[CLOD_RT_SEC_TLAS_SRV];
            RayDesc ray;
            ray.Origin = position + fragment.normalWS * max(0.05f, hit.coneWidth);
            ray.Direction = normalize(-light.dirWorldSpace.xyz);
            ray.TMin = 0.0f;
            ray.TMax = 100000.0f;
            // Alpha-tested casters count as solid here: this is a shadow seen in a reflection, and it keeps this pass simple.
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, CLOD_RT_RAYQUERY_FLAGS> query;
            query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFFu, ray);
            query.Proceed();
            g_clodRtSunShadow = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1.0f : 0.0f;
            break;
        }
    }

    const float3 lit = lightFragmentColor(fragment, camera, perFrame.activeEnvironmentIndex,
        ResourceDescriptorIndex(Builtin::Environment::InfoBuffer)) + fragment.emissive;
    RWTexture2D<float4> output = ResourceDescriptorHeap[CLOD_RT_SEC_OUTPUT_UAV];
    output[id.xy] = float4(lit, 1.0f);
}
