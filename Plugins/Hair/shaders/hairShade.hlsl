// The layered path's shading: one thread per row of the shade list (hairShadeList.hlsl), the pool rows the rasterizer
// marked: the vertices of every segment that offered a key to some pixel's layers this frame. Each is lit, and its
// colour written into the shading buffer, parallel to the pool. The composite (hairComposite.hlsl) interpolates the two
// colours of a segment at each pixel that shows it. Vertices of segments hidden everywhere are not lit.
//
// The shading point is the vertex itself, on the strand's centreline. A strand has one colour across its width: a strand
// many pixels wide is not shaded as round, and a shadow's edge is resolved along the strand only, at the spacing of its
// vertices (HairFrameGPU::segmentPixels on screen).
//
// The light loop is hair's own, built from the renderer's pieces: the light model (EvaluateLightAtPoint,
// LightIncidentRadiance), the occlusion lookup (EvaluateLightOcclusion: the virtual shadow map for the sun and for spot
// and point lights with faces) and, for HAIR_LIGHTS_GRID, the light grid's forward runs. The renderer's own loop
// (EvaluateDirectLighting) is the surface model's and drops light from behind the surface. The response is
// HairEvaluateBsdf (hairBsdf.hlsli), and the environment's is HairBsdfEnvironmentSample's four directions, with the
// environment's radiance along each taken from its spherical harmonics.
//
// Hair shadows itself through the density volumes (hairVolume.hlsl builds them, HairVolumeGPU says what they hold). For
// every light, and for each of the environment's directions, the vertex marches its instance's volume that way and
// counts the fibres in between (HairMarchVolume); HairEvaluateBsdf turns the count into the light that still arrives,
// directly and forward scattered, and adds what the hair around the vertex scatters back (dual scattering,
// Hair/HairScatterShared.h). The shadow map below is still what opaque geometry casts on hair, and multiplies in. An
// instance without a volume (the feature off, out of budget) is lit as before: by every light in full.
//
// Shadows are looked up at the vertex, moved toward the light by the lookup's own receiver offset (the "normal" it is
// given is the direction to the light). The virtual shadow map only has the pages that pixels of the frame's depth
// buffer asked for, and on this path hair is not in the depth buffer: a vertex whose page nothing asked for is looked up
// in up to two coarser clipmaps and is lit when none has it. The counters say how often.
//
// When hair casts shadows (HAIR_SHADOW_FLAG_ENABLED, hairShadow.hlsl) the sun's shadow map holds hair too, and a vertex
// that looked itself up there would be shadowed by its own groom twice: once by the volume, once by the map. So a vertex
// with a volume asks the map two narrower questions instead of one. What is in the way outside its groom's box: the
// same lookup, started where the ray to the sun leaves the box (in a clipmap that moves the receiver along the light
// and nothing else, so everything outside the box shadows as before, another groom's hair included). And what opaque
// thing is in the way inside the box, the head above all: the depth of the shadow map's cached pages, which hold the
// rigid casters and, in the clipmaps hair is redrawn into every frame, no hair (hairShadow.hlsl); an occluder there
// between the vertex and the box's edge shadows the vertex. That second answer is one texel's, without the lookup's
// soft edge, and it does not know casters that are themselves redrawn every frame (skinned meshes): a skinned head
// shadows its own hair only through the volume's scalp shell. In the far clipmaps, where hair is in the cached pages
// too, it is not asked. A vertex without a volume is looked up as before and so is shadowed by hair in the map, its
// own groom's included.

// A vertex can lie anywhere in front of the opaque surface: read the light grid's forward runs.
#define LIGHT_GRID_FORWARD 1
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/lighting.hlsli"
#include "HairShaders/hairCommon.hlsli"
#include "HairShaders/hairShading.hlsli"
#include "HairShaders/hairBsdf.hlsli"
#include "HairShaders/hairVolume.hlsli"

// HairShadePass::Record. The camera, the lights, the environment and the shadow map are bound by name; root constants
// 24 and 25 are the lighting switches (MiscShadowingMode, MiscEnablePunctualLights).
#define HAIR_FRAME_STATE UintRootConstant0       // StructuredBuffer<HairFrameGPU>
#define HAIR_COUNTERS UintRootConstant1          // RWStructuredBuffer<uint>
#define HAIR_VISIBLE_CLUSTERS UintRootConstant2  // StructuredBuffer<HairVisibleCluster>
#define HAIR_POOL_VERTICES UintRootConstant3     // StructuredBuffer<HairPoolVertex>
#define HAIR_POOL_SEGMENT_IDS UintRootConstant4  // StructuredBuffer<uint>
#define HAIR_INSTANCES UintRootConstant5         // StructuredBuffer<HairInstanceGPU>
#define HAIR_POOL_COLORS UintRootConstant6       // RWStructuredBuffer<uint2>
#define HAIR_VISIBILITY_WIDTH UintRootConstant7
#define HAIR_VISIBILITY_HEIGHT UintRootConstant8
#define HAIR_GROUPS_PER_ROW UintRootConstant9    // the dispatch is this many groups wide
#define HAIR_SHADE_FLAGS UintRootConstant10
#define HAIR_SHADE_FLAG_ENVIRONMENT 1u           // the frame has image-based lighting: add the environment term
#define HAIR_SHADE_LIST UintRootConstant11       // StructuredBuffer<uint>: the pool rows to light
#define HAIR_VOLUMES UintRootConstant12          // StructuredBuffer<HairVolumeGPU>: the density volumes, one row per instance
#define HAIR_VOXELS UintRootConstant13           // StructuredBuffer<uint>: their voxels
#define HAIR_SCATTER_TABLES UintRootConstant14   // StructuredBuffer<HairScatterRow>: HAIR_SCATTER_TABLE_ROWS rows per instance
#define HAIR_SHADOW_STATIC_PAGES UintRootConstant15 // Texture2D<uint>: the shadow map's cached pages, or HAIR_INVALID_INDEX

struct HairShadePoint
{
    FragmentInfo fragment; // what the occlusion lookup reads: the position, its pixel and, as the normal, the light's direction
    float3 tangent;
    float3 view;
    HairFibreMaterial material;
    // The density volume of the vertex's instance, and how this frame marches it.
    bool hasVolume;
    HairVolumeGPU volume;
    uint instanceIndex;
    float marchOffset;   // the vertex's jitter, in [0, 1)
    uint marchCount;     // marches made from this vertex so far
    uint marchSteps;     // and the voxels they read
    float3 debugValue;   // HAIR_SHADE_DEBUG_TRANSMITTANCE and _FIBRE_COUNT: of the first light
};

// The vertex's march toward a light or an environment direction, and what HairEvaluateBsdf makes of it.
HairVolumeMarch HairShadeMarch(inout HairShadePoint shadePoint, HairFrameGPU frame, float3 direction, float maxDistance, uint steps)
{
    StructuredBuffer<uint> voxels = ResourceDescriptorHeap[HAIR_VOXELS];
    // Each march of a vertex takes its own offset: the golden ratio's multiples of the vertex's.
    const float offset = frac(shadePoint.marchOffset + 0.618034f * (float)shadePoint.marchCount);
    const HairVolumeMarch march = HairMarchVolume(shadePoint.volume, voxels, shadePoint.fragment.fragPosWorldSpace, direction, maxDistance,
        steps, frame.volumeBias, offset);
    ++shadePoint.marchCount;
    shadePoint.marchSteps += march.steps;
    return march;
}

HairLightPath HairShadeLightPath(HairShadePoint shadePoint, HairFrameGPU frame, float fibreCount, float sinTheta, uint parts)
{
    HairLightPath path;
    path.hasVolume = shadePoint.hasVolume;
    path.dualScattering = (frame.volumeFlags & HAIR_VOLUME_FLAG_DUAL_SCATTERING) != 0u;
    path.fibreCount = fibreCount;
    path.densityFactor = frame.volumeDensityFactor;
    path.parts = parts;
    path.scatter = (HairScatterRow)0;
    if (path.hasVolume && path.dualScattering)
    {
        StructuredBuffer<HairScatterRow> tables = ResourceDescriptorHeap[HAIR_SCATTER_TABLES];
        path.scatter = HairLoadScatterRow(tables, shadePoint.instanceIndex, sinTheta);
    }
    return path;
}

// Black at no fibres, through red and yellow to white at 16.
float3 HairFibreCountColor(float fibreCount)
{
    const float level = saturate(fibreCount / 16.0f) * 3.0f;
    return saturate(float3(level, level - 1.0f, level - 2.0f));
}

// The environment's radiance along a direction, as its second-order spherical harmonics have it: the renderer keeps
// them convolved with the cosine lobe (irradianceSH, IBL.hlsli), so this is the irradiance of a surface facing
// `direction` over pi, the environment blurred by a cosine lobe.
struct HairEnvironment
{
    float3 coefficients[9];
};

HairEnvironment HairLoadEnvironment(uint environmentIndex)
{
    StructuredBuffer<EnvironmentInfo> environments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Environment::InfoBuffer)];
    const EnvironmentInfo info = environments[environmentIndex];
    HairEnvironment environment;
    for (uint i = 0u; i < 9u; ++i)
    {
        environment.coefficients[i] = float3(info.sphericalHarmonics[3u * i], info.sphericalHarmonics[3u * i + 1u],
            info.sphericalHarmonics[3u * i + 2u]) * (info.sphericalHarmonicsScale * SH_FLOAT_SCALE_INVERSE);
    }
    return environment;
}

float3 HairEnvironmentRadiance(HairEnvironment environment, float3 n)
{
    // irradianceSH's basis.
    const float3 irradiance = environment.coefficients[0] + environment.coefficients[1] * n.y + environment.coefficients[2] * n.z +
        environment.coefficients[3] * n.x + environment.coefficients[4] * (n.y * n.x) + environment.coefficients[5] * (n.y * n.z) +
        environment.coefficients[6] * (3.0f * n.z * n.z - 1.0f) + environment.coefficients[7] * (n.z * n.x) +
        environment.coefficients[8] * (n.x * n.x - n.y * n.y);
    return max(irradiance, 0.0f.xxx) * (1.0f / HAIR_BSDF_PI);
}

// Whether a rigid opaque caster shadows `position` from inside its groom's box: an occluder between the vertex and the
// point `exitDistance` along `toLight`, in the shadow map's cached pages. False where those do not answer: no page, no
// cached content, or a clipmap in which hair is drawn into the cached pages itself.
bool HairShadeOpaqueInsideBox(float3 position, float3 toLight, float exitDistance, HairFrameGPU frame, uint clipmapCount)
{
#if defined(LIGHTING_NO_VIRTUAL_SHADOW_MAPS)
    return false;
#else
    const uint activeClipmapCount = min(clipmapCount, kCLodVirtualShadowClipmapCount);
    if (activeClipmapCount == 0u || HAIR_SHADOW_STATIC_PAGES == HAIR_INVALID_INDEX)
    {
        return false;
    }
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodClipmapInfo)];
    StructuredBuffer<CLodVirtualShadowMainCameraInfo> compactMainCameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactMainCamera)];
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> compactShadowCameraBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactShadowCameras)];
    StructuredBuffer<float4> directionalPageViewInfo = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodDirectionalPageViewInfo)];
    Texture2DArray<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodPageTable)];
    Texture2D<uint> staticPages = ResourceDescriptorHeap[HAIR_SHADOW_STATIC_PAGES];
    // The renderer's own single-texel lookup, given the cached pages in place of the frame's.
    CLodVirtualShadowDebugInfo info;
    const CLodVirtualShadowLookupResult lookup = CLodVirtualShadowLookupDirectionalOcclusion(position, toLight, activeClipmapCount,
        clipmapInfos, compactMainCameraBuffer, compactShadowCameraBuffer, directionalPageViewInfo, pageTable, staticPages,
        float2(0.0f, 0.0f), info);
    if (lookup.valid == 0u || lookup.depthAvailable == 0u || lookup.sampledClipmapIndex >= activeClipmapCount ||
        (lookup.clipmapInfo.flags & kCLodVirtualShadowClipmapDynamicSkinnedFlag) == 0u ||
        (info.sampledPageEntry & kCLodVirtualShadowContentValidMask) == 0u)
    {
        return false;
    }
    // A cached page's depths are along the light from where the clipmap's view was when the page was drawn.
    const CLodVirtualShadowCompactShadowCameraInfo lightCamera = compactShadowCameraBuffer[lookup.sampledClipmapIndex];
    const float occluderDepth = lookup.closestDepth + directionalPageViewInfo[lookup.sampledPhysicalPageIndex].z - lightCamera.view[3][2];
    const float occluderDistance = -mul(float4(position, 1.0f), lightCamera.view).z - occluderDepth;
    // The texel's depth is that of a surface that may slope across it: an occluder has to be clear of the vertex.
    return occluderDistance > frame.shadowReceiverBias + 2.0f * lookup.clipmapInfo.texelWorldSize && occluderDistance <= exitDistance;
#endif
}

void HairAccumulateLight(uint lightIndex, inout HairShadePoint shadePoint, Camera camera, DirectLightingOptions options,
    HairFrameGPU frame, uint lobeMask, uint parts, inout DirectLightingResult lookups, inout float3 color)
{
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    const LightInfo light = lights[lightIndex];
    const LightFragmentData lightData = EvaluateLightAtPoint(light, shadePoint.fragment.fragPosWorldSpace);
    if (!LightInRange(light, lightData))
    {
        return;
    }
    const float3 radiance = LightIncidentRadiance(lightData);
    if (!any(radiance > 0.0f))
    {
        return;
    }
    shadePoint.fragment.normalWS = lightData.lightToFrag;
    // With hair in the sun's shadow map, a vertex with a volume leaves its own groom's hair to the volume.
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    FragmentInfo receiver = shadePoint.fragment;
    bool opaqueInsideBox = false;
    if ((frame.shadowFlags & HAIR_SHADOW_FLAG_ENABLED) != 0u && shadePoint.hasVolume && options.virtualShadowMaps &&
        lightIndex == perFrame.sunLightIndex)
    {
        float enter;
        float exit;
        HairVolumeRayRange(shadePoint.volume, HairVolumeGridPosition(shadePoint.volume, receiver.fragPosWorldSpace),
            HairVolumeGridDirection(shadePoint.volume, lightData.lightToFrag), enter, exit);
        const float exitDistance = max(exit, 0.0f);
        opaqueInsideBox = HairShadeOpaqueInsideBox(receiver.fragPosWorldSpace, lightData.lightToFrag, exitDistance, frame,
            perFrame.numDirectionalClipmaps);
        receiver.fragPosWorldSpace += lightData.lightToFrag * exitDistance;
        receiver.fragPosViewSpace = mul(float4(receiver.fragPosWorldSpace, 1.0f), camera.view).xyz;
    }
    float occlusion = opaqueInsideBox ? 1.0f : EvaluateLightOcclusion(light, lightIndex, receiver, camera, options, lookups);
    if (occlusion >= 1.0f)
    {
        return;
    }
    // The hair between the vertex and the light.
    float fibreCount = 0.0f;
    const bool firstMarch = shadePoint.marchCount == 0u;
    if (shadePoint.hasVolume)
    {
        const HairVolumeMarch march = HairShadeMarch(shadePoint, frame, lightData.lightToFrag,
            light.type == LIGHT_TYPE_DIRECTIONAL ? 1.0e30f : lightData.distance, frame.volumeSteps);
        if (march.open <= 0.0f)
        {
            return;
        }
        // The scalp shell is opaque like the shadow map's occluders.
        occlusion = 1.0f - (1.0f - occlusion) * march.open;
        fibreCount = march.fibreCount;
    }
    const HairLightPath path = HairShadeLightPath(shadePoint, frame, fibreCount, dot(shadePoint.tangent, lightData.lightToFrag), parts);
    if (shadePoint.hasVolume && firstMarch)
    {
        const uint debugView = (frame.shadeMask >> HAIR_SHADE_DEBUG_SHIFT) & HAIR_SHADE_DEBUG_MASK;
        if (debugView == HAIR_SHADE_DEBUG_FIBRE_COUNT)
        {
            shadePoint.debugValue = HairFibreCountColor(fibreCount);
        }
        else if (debugView == HAIR_SHADE_DEBUG_TRANSMITTANCE)
        {
            shadePoint.debugValue = exp(-fibreCount).xxx;
            if (path.dualScattering)
            {
                shadePoint.debugValue = float3(
                    exp(-fibreCount * (1.0f - min(path.scatter.forward.x, 0.99f))),
                    exp(-fibreCount * (1.0f - min(path.scatter.forward.y, 0.99f))),
                    exp(-fibreCount * (1.0f - min(path.scatter.forward.z, 0.99f))));
            }
        }
    }
    color += (1.0f - occlusion) * radiance *
        HairEvaluateBsdf(shadePoint.tangent, shadePoint.view, lightData.lightToFrag, shadePoint.material, path, lobeMask);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairShadeCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint listIndex = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    if (listIndex >= min(counters[HAIR_COUNTER_SHADE_LIST], frame.poolVertexCapacity))
    {
        return;
    }
    StructuredBuffer<uint> shadeList = ResourceDescriptorHeap[HAIR_SHADE_LIST];
    const uint row = shadeList[listIndex];
    RWStructuredBuffer<uint2> poolColors = ResourceDescriptorHeap[HAIR_POOL_COLORS];
    // The vertex's cluster, from the id of the segment it starts or, for a strand's last vertex, ends. As in the
    // rasterizer, a row is this frame's when the visible cluster its id names owns it.
    StructuredBuffer<uint> poolSegmentIds = ResourceDescriptorHeap[HAIR_POOL_SEGMENT_IDS];
    uint segmentId = poolSegmentIds[row];
    if (segmentId == HAIR_INVALID_INDEX && row != 0u)
    {
        segmentId = poolSegmentIds[row - 1u];
    }
    if (segmentId == HAIR_INVALID_INDEX)
    {
        return;
    }
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    const uint visibleIndex = segmentId >> HAIR_VIS_CLUSTER_SHIFT;
    if (visibleIndex >= min(counters[HAIR_COUNTER_CLUSTERS_VISIBLE], frame.visibleClusterCapacity))
    {
        return;
    }
    const HairVisibleCluster visible = visibleClusters[visibleIndex];
    const uint strandVertexCount = (1u << visible.vertexLevel) + 1u;
    if (row < visible.poolVertexBase || row >= visible.poolVertexBase + visible.strandCount * strandVertexCount)
    {
        return;
    }

    StructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_POOL_VERTICES];
    StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
    const float3 position = poolVertices[row].position;
    // The tangent: between the vertex's neighbours along its strand, toward the tip.
    const uint clusterStrand = (row - visible.poolVertexBase) / strandVertexCount;
    const uint strandVertex = (row - visible.poolVertexBase) - clusterStrand * strandVertexCount;
    const float3 before = strandVertex != 0u ? poolVertices[row - 1u].position : position;
    const float3 after = strandVertex + 1u != strandVertexCount ? poolVertices[row + 1u].position : position;

    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const Camera camera = HairLoadMainCamera();
    HairShadePoint shadePoint;
    shadePoint.tangent = HairSafeNormalize(after - before, float3(0.0f, 1.0f, 0.0f));
    shadePoint.view = HairSafeNormalize(camera.positionWorldSpace.xyz - position, float3(0.0f, 0.0f, 1.0f));
    shadePoint.material = HairMakeFibreMaterial(instances[visible.instanceIndex],
        visible.instanceIndex * 0x9E3779B9u + visible.clusterIndex * HAIR_MAX_STRANDS_PER_CLUSTER + clusterStrand,
        shadePoint.tangent, shadePoint.view);
    shadePoint.fragment = (FragmentInfo)0;
    shadePoint.fragment.fragPosWorldSpace = position;
    shadePoint.fragment.fragPosViewSpace = mul(float4(position, 1.0f), camera.view).xyz;
    // The vertex's pixel, for the light grid and the shadow lookup's noise: the nearest one on screen for a vertex that
    // is off it or behind the camera.
    const float2 size = float2((float)HAIR_VISIBILITY_WIDTH, (float)HAIR_VISIBILITY_HEIGHT);
    const float4 clip = mul(float4(position, 1.0f), camera.viewProjection);
    const float2 ndc = clip.xy / max(clip.w, camera.zNear);
    shadePoint.fragment.pixelCoords = clamp(float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * size, 0.0f.xx, size - 1.0f);

    // The instance's density volume, when the frame built one for it.
    shadePoint.hasVolume = false;
    shadePoint.volume = (HairVolumeGPU)0;
    shadePoint.instanceIndex = visible.instanceIndex;
    shadePoint.marchCount = 0u;
    shadePoint.marchSteps = 0u;
    shadePoint.debugValue = 0.0f.xxx;
    // The march's jitter: the vertex's own, the same in every frame and at every level of detail that draws it, unless
    // the frame asks for a new one each frame.
    const uint vertexKey = (visible.instanceIndex * 0x9E3779B9u + visible.clusterIndex * HAIR_MAX_STRANDS_PER_CLUSTER + clusterStrand) * 1031u +
        (strandVertex << (HAIR_MAX_VERTEX_LEVEL - visible.vertexLevel));
    shadePoint.marchOffset = HairVolumeUnit(HairVolumeHash(
        vertexKey + ((frame.volumeFlags & HAIR_VOLUME_FLAG_JITTER_FRAME) != 0u ? frame.frameNumber * 0x85EBCA6Bu : 0u)));
    if ((frame.volumeFlags & HAIR_VOLUME_FLAG_ENABLED) != 0u && visible.instanceIndex < frame.volumeInstanceCount)
    {
        StructuredBuffer<HairVolumeGPU> volumes = ResourceDescriptorHeap[HAIR_VOLUMES];
        shadePoint.volume = volumes[visible.instanceIndex];
        shadePoint.hasVolume = HairVolumeExists(shadePoint.volume);
    }
    // What of the scattering a vertex's colour holds: all of it, but in the debug views that show one part alone.
    const uint debugView = (frame.shadeMask >> HAIR_SHADE_DEBUG_SHIFT) & HAIR_SHADE_DEBUG_MASK;
    const uint parts = debugView == HAIR_SHADE_DEBUG_SCATTER_GLOBAL ? HAIR_SCATTER_PART_GLOBAL
        : debugView == HAIR_SHADE_DEBUG_SCATTER_LOCAL ? HAIR_SCATTER_PART_LOCAL : HAIR_SCATTER_PART_ALL;

    // The lighting switches as a lighting pass's root constants give them. Stochastic lighting has no entry for a
    // receiver that is not a surface, and no shadow map: hair is then lit by every light, unshadowed.
    DirectLightingOptions options = DirectLightingOptionsFromRoot();
    DirectLightingResult lookups = (DirectLightingResult)0;
    lookups.hasShadowDebug = false;
    lookups.shadowDebug = CLodVirtualShadowInitDebugInfo(0xFFFFFFFFu);
    float3 color = 0.0f.xxx;
    const uint lobeMask = frame.shadeMask & HAIR_LOBE_ALL;
    if (options.punctualLights && lobeMask != 0u)
    {
        if (frame.lightLookup == HAIR_LIGHTS_GRID)
        {
            const LightGridRange range =
                LightGridLoadRange(shadePoint.fragment.pixelCoords, shadePoint.fragment.fragPosViewSpace.z, perFrame, camera);
            for (uint i = 0u; i < range.count; ++i)
            {
                HairAccumulateLight(LightGridLight(range, i), shadePoint, camera, options, frame, lobeMask, parts, lookups, color);
            }
        }
        else
        {
            StructuredBuffer<uint> activeLightIndices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
            for (uint i = 0u; i < perFrame.numLights; ++i)
            {
                HairAccumulateLight(activeLightIndices[i], shadePoint, camera, options, frame, lobeMask, parts, lookups, color);
            }
        }
    }
    if ((HAIR_SHADE_FLAGS & HAIR_SHADE_FLAG_ENVIRONMENT) != 0u && (frame.shadeMask & HAIR_SHADE_ENVIRONMENT) != 0u)
    {
        const HairEnvironment environment = HairLoadEnvironment(perFrame.activeEnvironmentIndex);
        float3 ambient = 0.0f.xxx;
        if (shadePoint.hasVolume)
        {
            // Each direction is occluded by the hair that way, and takes the weight of a fibre in hair
            // (HairBsdfEnvironmentWeightDual): the stand-ins of the unoccluded form are not needed.
            const float sinThetaO = clamp(dot(shadePoint.tangent, shadePoint.view), -1.0f, 1.0f);
            const float cosThetaO = sqrt(1.0f - sinThetaO * sinThetaO);
            const HairLightPath path = HairShadeLightPath(shadePoint, frame, 0.0f, sinThetaO, parts);
            for (uint direction = 0u; direction < HAIR_BSDF_ENVIRONMENT_SAMPLES; ++direction)
            {
                const float3 toEnvironment = HairBsdfEnvironmentDirection(shadePoint.tangent, shadePoint.view, direction);
                const HairVolumeMarch march = HairShadeMarch(shadePoint, frame, toEnvironment, 1.0e30f, frame.volumeEnvironmentSteps);
                if (march.open <= 0.0f)
                {
                    continue;
                }
                ambient += HairBsdfEnvironmentWeightDual(shadePoint.material.lobes, path.scatter, shadePoint.view, cosThetaO, direction,
                    toEnvironment, march.fibreCount, path.densityFactor, path.dualScattering, parts) *
                    (march.open * HairEnvironmentRadiance(environment, toEnvironment));
            }
        }
        else
        {
            // Nothing occludes the environment (HairBsdfEnvironmentSample says what that leaves out).
            for (uint direction = 0u; direction < HAIR_BSDF_ENVIRONMENT_SAMPLES; ++direction)
            {
                float3 toEnvironment;
                const float3 weight =
                    HairBsdfEnvironmentSample(shadePoint.material.lobes, shadePoint.tangent, shadePoint.view, direction, toEnvironment);
                ambient += weight * HairEnvironmentRadiance(environment, toEnvironment);
            }
        }
        color += frame.ambientScale * ambient;
    }

    if (debugView == HAIR_SHADE_DEBUG_TRANSMITTANCE || debugView == HAIR_SHADE_DEBUG_FIBRE_COUNT)
    {
        color = shadePoint.debugValue;
    }
    else if (debugView == HAIR_SHADE_DEBUG_DENSITY)
    {
        // The fibres a ray crosses in one voxel's length at the vertex; blue in a voxel of the scalp shell.
        color = 0.0f.xxx;
        if (shadePoint.hasVolume)
        {
            StructuredBuffer<uint> voxels = ResourceDescriptorHeap[HAIR_VOXELS];
            const int3 voxel = (int3)floor(HairVolumeGridPosition(shadePoint.volume, position));
            if (HairVolumeContains(shadePoint.volume, voxel))
            {
                const uint value = voxels[HairVolumeVoxelIndex(shadePoint.volume, (uint3)voxel)];
                color = (value & HAIR_VOLUME_SCALP_BIT) != 0u ? float3(0.0f, 0.0f, 1.0f)
                    : (0.5f * HairVolumeFibres(value, shadePoint.volume.voxelSize)).xxx;
            }
        }
    }
    poolColors[row] = HairPackColor(color);

    // The frame's counts, one atomic per wave each. The shadow lookup the renderer reports is that of the light its
    // shadow map debug views show: the sun, unless the user picked another.
    const bool lookedUp = lookups.hasShadowDebug;
    const bool missing = lookedUp && lookups.shadowDebug.sampledClipmapIndex == 0xFFFFFFFFu;
    const bool coarser = lookedUp && !missing && lookups.shadowDebug.sampledClipmapIndex != lookups.shadowDebug.preferredClipmapIndex;
    const bool cleared = lookedUp && (lookups.shadowDebug.flags & kCLodVirtualShadowDebugFlagSampledTexelCleared) != 0u;
    const uint shadedCount = WaveActiveCountBits(true);
    const uint volumeCount = WaveActiveCountBits(shadePoint.hasVolume);
    const uint marchCount = WaveActiveSum(shadePoint.marchCount);
    const uint stepCount = WaveActiveSum(shadePoint.marchSteps);
    const uint lookupCount = WaveActiveCountBits(lookedUp);
    const uint coarserCount = WaveActiveCountBits(coarser);
    const uint missingCount = WaveActiveCountBits(missing);
    const uint clearedCount = WaveActiveCountBits(cleared);
    if (WaveIsFirstLane())
    {
        InterlockedAdd(counters[HAIR_COUNTER_SHADED_VERTICES], shadedCount);
        if (volumeCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_VOLUME_VERTICES], volumeCount);
        if (marchCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_VOLUME_MARCHES], marchCount);
        if (stepCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_VOLUME_STEPS], stepCount);
        if (lookupCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_SHADOW_LOOKUPS], lookupCount);
        if (coarserCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_SHADOW_COARSER], coarserCount);
        if (missingCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_SHADOW_MISSING], missingCount);
        if (clearedCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_SHADOW_CLEARED], clearedCount);
    }
}
