#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodResolveCommon.hlsli"
#include "include/reyesPatchCommon.hlsli"
#include "include/occlusionCulling.hlsli"
#include "PerPassRootConstants/clodReyesSplitRootConstants.h"

static const uint REYES_SPLIT_GROUP_SIZE = 64u;
static const uint REYES_SPLIT_TELEMETRY_PASS_COUNT = 4u;
static const float REYES_SHADOW_FINE_TARGET_TEXELS_PER_MICRO_TRIANGLE = 1.0f;
static const float REYES_SHADOW_COARSE_TARGET_PAGES_PER_TRIANGLE_DEFAULT = 10.0f;

float ReyesGetShadowCoarseTargetTexelsPerTriangle()
{
    const float targetPagesPerTriangle = max(
        CLOD_REYES_SPLIT_SHADOW_COARSE_TARGET_PAGES_PER_TRIANGLE,
        REYES_SHADOW_COARSE_TARGET_PAGES_PER_TRIANGLE_DEFAULT);
    return targetPagesPerTriangle * float(kCLodVirtualShadowPhysicalPageSize);
}

bool SphereOutsideFrustumViewSpace(float3 viewSpaceCenter, float radius, Camera camera)
{
    [unroll]
    for (uint i = 0u; i < 6u; ++i)
    {
        const float4 plane = camera.clippingPlanes[i].plane;
        const float distanceToPlane = dot(plane.xyz, viewSpaceCenter) + plane.w;
        if (distanceToPlane < -radius)
        {
            return true;
        }
    }

    return false;
}

float ReyesMaxAxisScale_RowVector(row_major matrix m)
{
    const float3 row0 = float3(m._11, m._12, m._13);
    const float3 row1 = float3(m._21, m._22, m._23);
    const float3 row2 = float3(m._31, m._32, m._33);
    return sqrt(max(dot(row0, row0), max(dot(row1, row1), dot(row2, row2))));
}

float ReyesPatchMaxDisplacementMagnitude(MaterialInfo materialInfo)
{
    if (materialInfo.geometricDisplacementEnabled == 0u)
    {
        return 0.0f;
    }

    return max(abs(materialInfo.geometricDisplacementMin), abs(materialInfo.geometricDisplacementMax));
}

void ReyesPatchBuildConservativeSphere(
    float3 worldPosition0,
    float3 worldPosition1,
    float3 worldPosition2,
    float displacementMagnitude,
    out float3 centerWorld,
    out float radiusWorld)
{
    centerWorld = (worldPosition0 + worldPosition1 + worldPosition2) / 3.0f;
    radiusWorld = max(
        distance(centerWorld, worldPosition0),
        max(distance(centerWorld, worldPosition1), distance(centerWorld, worldPosition2))) + displacementMagnitude;
}

void ReyesPatchBuildConservativeObjectSphere(
    float3 objectPosition0,
    float3 objectPosition1,
    float3 objectPosition2,
    float displacementMagnitude,
    out float3 centerObject,
    out float radiusObject)
{
    centerObject = (objectPosition0 + objectPosition1 + objectPosition2) / 3.0f;
    radiusObject = max(
        distance(centerObject, objectPosition0),
        max(distance(centerObject, objectPosition1), distance(centerObject, objectPosition2))) + displacementMagnitude;
}

void ReyesPatchBuildConservativeViewAABB(
    float3 objectPosition0,
    float3 objectPosition1,
    float3 objectPosition2,
    float displacementMagnitude,
    row_major matrix modelMatrix,
    row_major matrix viewMatrix,
    out float3 viewMin,
    out float3 viewMax)
{
    const float3 objectMin = min(objectPosition0, min(objectPosition1, objectPosition2)) - displacementMagnitude.xxx;
    const float3 objectMax = max(objectPosition0, max(objectPosition1, objectPosition2)) + displacementMagnitude.xxx;
    viewMin = 1.0e30f.xxx;
    viewMax = -1.0e30f.xxx;

    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        const float3 objectCorner = float3(
            (cornerIndex & 0x1u) != 0u ? objectMax.x : objectMin.x,
            (cornerIndex & 0x2u) != 0u ? objectMax.y : objectMin.y,
            (cornerIndex & 0x4u) != 0u ? objectMax.z : objectMin.z);
        const float3 viewCorner = mul(mul(float4(objectCorner, 1.0f), modelMatrix), viewMatrix).xyz;
        viewMin = min(viewMin, viewCorner);
        viewMax = max(viewMax, viewCorner);
    }
}

bool ReyesPatchOcclusionEnabled()
{
    return CLOD_REYES_SPLIT_ENABLE_PATCH_OCCLUSION != 0u &&
        CLOD_REYES_SPLIT_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX != 0xFFFFFFFFu;
}

bool ReyesPatchHZBOccluded(
    float3 objectPosition0,
    float3 objectPosition1,
    float3 objectPosition2,
    float displacementMagnitude,
    PerObjectBuffer objectData,
    Camera camera,
    uint viewID,
    uint phaseIndex,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer)
{
    if (!ReyesPatchOcclusionEnabled())
    {
        return false;
    }

    StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
        ResourceDescriptorHeap[CLOD_REYES_SPLIT_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
    const uint depthMapDescriptorIndex = viewDepthSRVIndices[viewID].linearDepthSRVIndex;
    if (depthMapDescriptorIndex == 0u)
    {
        return false;
    }

    float3 objectCenter = 0.0f.xxx;
    float objectRadius = 0.0f;
    ReyesPatchBuildConservativeObjectSphere(
        objectPosition0,
        objectPosition1,
        objectPosition2,
        displacementMagnitude,
        objectCenter,
        objectRadius);
    if (!all(isfinite(objectCenter)) || !isfinite(objectRadius) || objectRadius <= 0.0f)
    {
        return false;
    }

    const bool phaseOne = phaseIndex == 1u;
    row_major matrix modelMatrix = objectData.model;
    row_major matrix viewMatrix = camera.view;
    row_major matrix projectionMatrix = camera.projection;
    if (phaseOne)
    {
        modelMatrix = objectData.prevModel;
        viewMatrix = camera.prevView;
        projectionMatrix = camera.prevUnjitteredProjection;
    }
    const float scaledRadius = objectRadius * ReyesMaxAxisScale_RowVector(modelMatrix);
    const float3 viewSpaceCenter = mul(mul(float4(objectCenter, 1.0f), modelMatrix), viewMatrix).xyz;
    const float boundingSphereDepth = -viewSpaceCenter.z;
    if (!all(isfinite(viewSpaceCenter)) || !isfinite(scaledRadius) || boundingSphereDepth <= scaledRadius)
    {
        return false;
    }

    bool occluded = false;
    InterlockedAdd(telemetryBuffer[0].splitOcclusionTestCount, 1u);
    if (CLOD_REYES_SPLIT_USE_AABB_OCCLUSION != 0u)
    {
        float3 viewMin = 0.0f.xxx;
        float3 viewMax = 0.0f.xxx;
        ReyesPatchBuildConservativeViewAABB(
            objectPosition0,
            objectPosition1,
            objectPosition2,
            displacementMagnitude,
            modelMatrix,
            viewMatrix,
            viewMin,
            viewMax);
        OcclusionCullingPerspectiveViewAABBTexture2D(
            occluded,
            uint2(camera.depthResX, camera.depthResY),
            camera.numDepthMips,
            projectionMatrix,
            viewMin,
            viewMax,
            depthMapDescriptorIndex);
    }
    else
    {
        OcclusionCullingPerspectiveTexture2D(
            occluded,
            uint2(camera.depthResX, camera.depthResY),
            camera.numDepthMips,
            camera.UVScaleToNextPowerOf2,
            projectionMatrix,
            viewSpaceCenter,
            boundingSphereDepth,
            scaledRadius,
            depthMapDescriptorIndex);
    }
    return occluded;
}

void ReyesReplayOrDropOccludedSplitPatch(
    CLodReyesSplitQueueEntry entry,
    uint phaseIndex,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer)
{
    if (phaseIndex == 1u &&
        CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu &&
        CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu &&
        CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<CLodReyesSplitQueueEntry> replayQueue =
            ResourceDescriptorHeap[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_DESCRIPTOR_INDEX];
        RWStructuredBuffer<uint> replayCounter =
            ResourceDescriptorHeap[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX];
        RWStructuredBuffer<uint> replayOverflow =
            ResourceDescriptorHeap[CLOD_REYES_SPLIT_REPLAY_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];

        uint replayIndex = 0u;
        InterlockedAdd(replayCounter[0], 1u, replayIndex);
        if (replayIndex < CLOD_REYES_SPLIT_QUEUE_CAPACITY)
        {
            replayQueue[replayIndex] = entry;
            InterlockedAdd(telemetryBuffer[0].splitOcclusionDeferCount, 1u);
        }
        else
        {
            InterlockedAdd(replayOverflow[0], 1u);
            InterlockedAdd(telemetryBuffer[0].replaySplitQueueOverflowCount, 1u);
        }
        return;
    }

    InterlockedAdd(telemetryBuffer[0].splitOcclusionDropCount, 1u);
}

bool CLodVirtualShadowComputeSphereAabbUvBounds(
    float3 worldCenter,
    float radiusWorld,
    CLodVirtualShadowCompactShadowCameraInfo shadowCamera,
    out float2 uvMin,
    out float2 uvMax,
    out bool queryClipped)
{
    float2 ndcMin = float2(1.0e30f, 1.0e30f);
    float2 ndcMax = float2(-1.0e30f, -1.0e30f);
    bool failOpen = false;

    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        const float3 cornerOffset = float3(
            (cornerIndex & 0x1u) != 0u ? radiusWorld : -radiusWorld,
            (cornerIndex & 0x2u) != 0u ? radiusWorld : -radiusWorld,
            (cornerIndex & 0x4u) != 0u ? radiusWorld : -radiusWorld);
        const float4 clipCorner = mul(float4(worldCenter + cornerOffset, 1.0f), shadowCamera.viewProjection);

        float3 ndcCorner = 0.0f.xxx;
        if (CLodVirtualShadowCompactCameraIsOrtho(shadowCamera))
        {
            ndcCorner = clipCorner.xyz;
        }
        else
        {
            const float safeW = abs(clipCorner.w);
            if (safeW <= 1.0e-6f || clipCorner.w <= 0.0f || clipCorner.z < 0.0f || clipCorner.z > clipCorner.w)
            {
                failOpen = true;
                break;
            }

            ndcCorner = clipCorner.xyz / clipCorner.w;
        }

        ndcMin = min(ndcMin, ndcCorner.xy);
        ndcMax = max(ndcMax, ndcCorner.xy);
    }

    if (failOpen)
    {
        uvMin = 0.0f.xx;
        uvMax = 1.0f.xx;
        queryClipped = true;
        return true;
    }

    if (ndcMax.x < -1.0f || ndcMin.x > 1.0f ||
        ndcMax.y < -1.0f || ndcMin.y > 1.0f)
    {
        uvMin = 0.0f.xx;
        uvMax = 0.0f.xx;
        queryClipped = false;
        return false;
    }

    queryClipped =
        ndcMin.x < -1.0f || ndcMax.x > 1.0f ||
        ndcMin.y < -1.0f || ndcMax.y > 1.0f;

    ndcMin = clamp(ndcMin, -1.0f.xx, 1.0f.xx);
    ndcMax = clamp(ndcMax, -1.0f.xx, 1.0f.xx);

    uvMin = float2(ndcMin.x * 0.5f + 0.5f, 1.0f - (ndcMax.y * 0.5f + 0.5f));
    uvMax = float2(ndcMax.x * 0.5f + 0.5f, 1.0f - (ndcMin.y * 0.5f + 0.5f));
    return true;
}

bool CLodVirtualShadowDirtyHierarchyAnyHit(
    Texture2DArray<uint> queryTexture,
    uint arrayLayer,
    uint2 baseResolution,
    float2 uvMin,
    float2 uvMax)
{
    const float2 clampedUvMin = saturate(uvMin);
    const float2 clampedUvMax = saturate(uvMax);
    const float2 baseResolutionF = float2(baseResolution);
    const float2 minTexel = clamp(baseResolutionF * clampedUvMin, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float2 maxTexel = clamp(baseResolutionF * clampedUvMax, 0.0f.xx, baseResolutionF - 1.0f.xx);
    const float pixelWidth = max(maxTexel.x - minTexel.x, maxTexel.y - minTexel.y);
    const uint sampleWidth = 2u;
    const uint maxMipLevel = firstbithigh(max(baseResolution.x, baseResolution.y));
    const uint sampledMipLevel = min(
        (uint)clamp(ceil(log2(max(pixelWidth, 1.0f))) - log2((float)sampleWidth), 0.0f, (float)maxMipLevel),
        maxMipLevel);

    const int2 quadCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 minCornerTexel = int2(minTexel) >> sampledMipLevel;
    const int2 maxCornerTexel = int2(maxTexel) >> sampledMipLevel;
    const int2 atMipPixelWidth = maxCornerTexel - minCornerTexel + 1;
    const int2 texelBounds = max(int2(0, 0), (int2(baseResolution) >> sampledMipLevel) - 1);

    [loop]
    for (uint x = 0u; x <= sampleWidth; ++x)
    {
        [loop]
        for (uint y = 0u; y <= sampleWidth; ++y)
        {
            if ((int)x >= atMipPixelWidth.x || (int)y >= atMipPixelWidth.y)
            {
                continue;
            }

            const int2 sampleTexel = clamp(quadCornerTexel + int2(x, y), int2(0, 0), texelBounds);
            if (queryTexture.Load(int4(sampleTexel, arrayLayer, sampledMipLevel)) != 0u)
            {
                return true;
            }
        }
    }

    return false;
}

bool ReyesPatchBuildShadowHierarchyQuery(
    float3 centerWorld,
    float radiusWorld,
    uint clipmapIndex,
    uint viewID,
    out uint arrayLayer,
    out uint2 baseResolution,
    out float2 uvMin,
    out float2 uvMax)
{
    arrayLayer = 0u;
    baseResolution = 0u.xx;
    uvMin = 0.0f.xx;
    uvMax = 0.0f.xx;

    if (CLOD_REYES_SPLIT_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX == 0xFFFFFFFFu ||
        clipmapIndex >= kCLodVirtualShadowClipmapCount)
    {
        return false;
    }

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_REYES_SPLIT_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
    if (!CLodVirtualShadowClipmapIsValid(clipmapInfo) || clipmapInfo.shadowCameraBufferIndex != viewID)
    {
        return false;
    }

    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> shadowCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodCompactShadowCameras)];

    bool queryClipped = false;
    const bool queryValid = CLodVirtualShadowComputeSphereAabbUvBounds(
        centerWorld,
        radiusWorld,
        shadowCameras[clipmapIndex],
        uvMin,
        uvMax,
        queryClipped);
    if (!queryValid)
    {
        return false;
    }

    arrayLayer = clipmapInfo.pageTableLayer;
    baseResolution = uint2(clipmapInfo.pageTableResolution, clipmapInfo.pageTableResolution);
    return true;
}

bool ReyesPatchTouchesShadowDirtyPages(
    float3 centerWorld,
    float radiusWorld,
    uint clipmapIndex,
    uint viewID)
{
    if (CLOD_REYES_SPLIT_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return true;
    }

    uint arrayLayer = 0u;
    uint2 baseResolution = 0u.xx;
    float2 uvMin = 0.0f.xx;
    float2 uvMax = 0.0f.xx;
    if (!ReyesPatchBuildShadowHierarchyQuery(centerWorld, radiusWorld, clipmapIndex, viewID, arrayLayer, baseResolution, uvMin, uvMax))
    {
        return true;
    }

    Texture2DArray<uint> dirtyHierarchy = ResourceDescriptorHeap[CLOD_REYES_SPLIT_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX];

    return CLodVirtualShadowDirtyHierarchyAnyHit(
        dirtyHierarchy,
        arrayLayer,
        baseResolution,
        uvMin,
        uvMax);
}

bool ReyesPatchTouchesOnlyShadowDirtyPages(
    float3 centerWorld,
    float radiusWorld,
    uint clipmapIndex,
    uint viewID)
{
    if (CLOD_REYES_SPLIT_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX == 0xFFFFFFFFu ||
        CLOD_REYES_SPLIT_SHADOW_NON_RASTERABLE_HIERARCHY_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return false;
    }

    uint arrayLayer = 0u;
    uint2 baseResolution = 0u.xx;
    float2 uvMin = 0.0f.xx;
    float2 uvMax = 0.0f.xx;
    if (!ReyesPatchBuildShadowHierarchyQuery(
        centerWorld,
        radiusWorld,
        clipmapIndex,
        viewID,
        arrayLayer,
        baseResolution,
        uvMin,
        uvMax))
    {
        return false;
    }

    Texture2DArray<uint> dirtyHierarchy = ResourceDescriptorHeap[CLOD_REYES_SPLIT_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX];
    if (!CLodVirtualShadowDirtyHierarchyAnyHit(dirtyHierarchy, arrayLayer, baseResolution, uvMin, uvMax))
    {
        return false;
    }

    Texture2DArray<uint> nonRasterableHierarchy = ResourceDescriptorHeap[CLOD_REYES_SPLIT_SHADOW_NON_RASTERABLE_HIERARCHY_DESCRIPTOR_INDEX];
    return !CLodVirtualShadowDirtyHierarchyAnyHit(nonRasterableHierarchy, arrayLayer, baseResolution, uvMin, uvMax);
}

bool ReyesPatchShouldCull(
    float3 worldPosition0,
    float3 worldPosition1,
    float3 worldPosition2,
    float displacementMagnitude,
    Camera sceneCamera,
    uint routeKind,
    uint clipmapIndex,
    uint viewID,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer,
    bool isChildPatch)
{
    float3 centerWorld = 0.0f.xxx;
    float radiusWorld = 0.0f;
    ReyesPatchBuildConservativeSphere(
        worldPosition0,
        worldPosition1,
        worldPosition2,
        displacementMagnitude,
        centerWorld,
        radiusWorld);

    const float3 centerViewSpace = mul(float4(centerWorld, 1.0f), sceneCamera.view).xyz;
    if (SphereOutsideFrustumViewSpace(centerViewSpace, radiusWorld, sceneCamera))
    {
        InterlockedAdd(telemetryBuffer[0].splitFrustumCullCount, 1u);
        if (isChildPatch)
        {
            InterlockedAdd(telemetryBuffer[0].splitChildCullCount, 1u);
        }
        return true;
    }

    if ((routeKind == CLOD_REYES_ROUTE_FINE_MICROPOLY_VSM || routeKind == CLOD_REYES_ROUTE_COARSE_HARDWARE_VSM) &&
        !ReyesPatchTouchesShadowDirtyPages(centerWorld, radiusWorld, clipmapIndex, viewID))
    {
        InterlockedAdd(telemetryBuffer[0].splitShadowDirtyCullCount, 1u);
        if (isChildPatch)
        {
            InterlockedAdd(telemetryBuffer[0].splitChildCullCount, 1u);
        }
        return true;
    }

    return false;
}

float3 ReyesInterpolateTriangle(float3 p0, float3 p1, float3 p2, float3 barycentrics)
{
    precise float3 result = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
    return result;
}

float3 ComputeReyesEdgeTessFactors(float3 worldPosition0, float3 worldPosition1, float3 worldPosition2, CullingCameraInfo camera)
{
    const float distance01 = max(camera.zNear, min(length(worldPosition0 - camera.positionWorldSpace.xyz), length(worldPosition1 - camera.positionWorldSpace.xyz)));
    const float distance12 = max(camera.zNear, min(length(worldPosition1 - camera.positionWorldSpace.xyz), length(worldPosition2 - camera.positionWorldSpace.xyz)));
    const float distance20 = max(camera.zNear, min(length(worldPosition2 - camera.positionWorldSpace.xyz), length(worldPosition0 - camera.positionWorldSpace.xyz)));

    const float edge01 = length(worldPosition0 - worldPosition1);
    const float edge12 = length(worldPosition1 - worldPosition2);
    const float edge20 = length(worldPosition2 - worldPosition0);

    const float scale = camera.projY * REYES_SCREEN_SCALE_REFERENCE * REYES_PROJECTED_PIXEL_TO_TESS_FACTOR_SCALE;
    return max(float3(1.0f, 1.0f, 1.0f), float3(edge01 / distance01, edge12 / distance12, edge20 / distance20) * scale);
}

float3 ComputeReyesShadowEdgeTessFactors(
    float3 worldPosition0,
    float3 worldPosition1,
    float3 worldPosition2,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    float targetTexelsPerTriangle)
{
    const float edge01 = length(worldPosition0 - worldPosition1);
    const float edge12 = length(worldPosition1 - worldPosition2);
    const float edge20 = length(worldPosition2 - worldPosition0);
    const float targetWorldLength = max(clipmapInfo.texelWorldSize * max(targetTexelsPerTriangle, 1.0f), 1e-5f);
    return max(float3(1.0f, 1.0f, 1.0f), float3(edge01, edge12, edge20) / targetWorldLength);
}

// Compute per-edge split factors: how many segments to split each edge into,
// such that after splitting, each child's edge factors fit within REYES_TESS_TABLE_MAX_SEGMENTS.
uint3 ComputeReyesSplitFactors(float3 edgeFactors)
{
    return clamp(
        uint3(ceil(edgeFactors / float(REYES_TESS_TABLE_MAX_SEGMENTS))),
        uint3(1u, 1u, 1u),
        uint3(REYES_TESS_TABLE_MAX_SEGMENTS, REYES_TESS_TABLE_MAX_SEGMENTS, REYES_TESS_TABLE_MAX_SEGMENTS));
}

[shader("compute")]
[numthreads(REYES_SPLIT_GROUP_SIZE, 1, 1)]
void ReyesSplitCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> splitQueueCounter = ResourceDescriptorHeap[CLOD_REYES_SPLIT_INPUT_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    const uint splitCount = splitQueueCounter[0];
    const uint splitIndex = dispatchThreadId.x;
    if (splitIndex >= splitCount)
    {
        return;
    }

    const uint queueCapacity = CLOD_REYES_SPLIT_QUEUE_CAPACITY;
    const uint maxSplitPassCount = CLOD_REYES_SPLIT_MAX_PASS_COUNT;
    ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_REYES_SPLIT_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesSplitQueueEntry> splitQueue = ResourceDescriptorHeap[CLOD_REYES_SPLIT_INPUT_QUEUE_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    StructuredBuffer<Camera> sceneCameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    RWStructuredBuffer<uint> outputSplitQueueCounter = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> outputSplitQueueOverflowCounter = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesSplitQueueEntry> outputSplitQueue = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_SPLIT_QUEUE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> diceQueueCounter = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> diceQueueOverflowCounter = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[CLOD_REYES_SPLIT_OUTPUT_DICE_QUEUE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_REYES_SPLIT_TELEMETRY_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_REYES_SPLIT_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableVertices = ResourceDescriptorHeap[CLOD_REYES_SPLIT_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableTriangles = ResourceDescriptorHeap[CLOD_REYES_SPLIT_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX];
    const CLodReyesSplitQueueEntry splitEntry = splitQueue[splitIndex];
    const uint splitPassTelemetryIndex = min(splitEntry.splitLevel, REYES_SPLIT_TELEMETRY_PASS_COUNT - 1u);
    InterlockedAdd(telemetryBuffer[0].splitInputCounts[splitPassTelemetryIndex], 1u);
    InterlockedMax(telemetryBuffer[0].deepestSplitLevelReached, splitEntry.splitLevel);

    const uint sourceTriangleIndex = splitEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, splitEntry.visibleClusterIndex);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, splitEntry.localMeshletIndex);

    MeshletResolveData md = (MeshletResolveData)0;
    md.drawcallAndMeshlet = uint2(splitEntry.instanceID, splitEntry.localMeshletIndex);
    md.vertexCount = CLodDescVertexCount(meshletDesc);
    md.triangleCount = CLodDescTriangleCount(meshletDesc);
    md.bitsX = CLodDescBitsX(meshletDesc);
    md.bitsY = CLodDescBitsY(meshletDesc);
    md.bitsZ = CLodDescBitsZ(meshletDesc);
    md.minQ = int3(meshletDesc.minQx, meshletDesc.minQy, meshletDesc.minQz);
    md.positionBitOffset = meshletDesc.positionBitOffset;
    md.vertexAttributeOffset = meshletDesc.vertexAttributeOffset;
    md.triangleByteOffset = meshletDesc.triangleByteOffset;
    md.pageAttributeMask = hdr.attributeMask;
    md.uvSetCount = hdr.uvSetCount;
    md.pageByteOffset = pageSlabByteOffset;
    md.uvDescriptorBase = pageSlabByteOffset + hdr.uvDescriptorOffset;
    md.uvBitstreamDirectoryBase = pageSlabByteOffset + hdr.uvBitstreamDirectoryOffset;
    md.positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
    md.normalArrayBase = pageSlabByteOffset + hdr.normalArrayOffset;
    md.tangentFrameArrayBase = pageSlabByteOffset + hdr.tangentFrameArrayOffset;
    md.colorArrayBase = pageSlabByteOffset + hdr.colorArrayOffset;
    md.jointArrayBase = pageSlabByteOffset + hdr.jointArrayOffset;
    md.weightArrayBase = pageSlabByteOffset + hdr.weightArrayOffset;
    md.triangleStreamBase = pageSlabByteOffset + hdr.triangleStreamOffset;
    md.compressedPositionQuantExp = hdr.compressedPositionQuantExp;
    md.pagePoolSlabDescriptorIndex = pageSlabDescriptorIndex;

    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(splitEntry.instanceID);
    const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(splitEntry.instanceID);
    const CullingCameraInfo camera = cameras[splitEntry.viewID];
    const Camera sceneCamera = sceneCameras[splitEntry.viewID];
    const MaterialInfo materialInfo = materials[perMesh.materialDataIndex];
    const float displacementMagnitude = ReyesPatchMaxDisplacementMagnitude(materialInfo);
    const uint clipmapIndex = CLodVisibleClusterShadowClipmapIndex(packedCluster);

    const uint3 sourceTriangle = DecodeTriangleCompact(min(sourceTriangleIndex, max(0u, md.triangleCount - 1u)), md);
    const float3 sourcePosition0OS = DecodeCompressedPosition(sourceTriangle.x, md);
    const float3 sourcePosition1OS = DecodeCompressedPosition(sourceTriangle.y, md);
    const float3 sourcePosition2OS = DecodeCompressedPosition(sourceTriangle.z, md);

    const float3 bary0 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex0UV);
    const float3 bary1 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex1UV);
    const float3 bary2 = ReyesPatchDomainUVToBarycentrics(splitEntry.domainVertex2UV);
    if (!ReyesPatchDomainHasValidSimplex(bary0, bary1, bary2))
    {
        InterlockedAdd(telemetryBuffer[0].invalidSplitPatchDomainCount, 1u);
        return;
    }

    const float3 currentPosition0OS = ReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary0);
    const float3 currentPosition1OS = ReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary1);
    const float3 currentPosition2OS = ReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, bary2);

    const float3 currentPosition0WS = mul(float4(currentPosition0OS, 1.0f), objectData.model).xyz;
    const float3 currentPosition1WS = mul(float4(currentPosition1OS, 1.0f), objectData.model).xyz;
    const float3 currentPosition2WS = mul(float4(currentPosition2OS, 1.0f), objectData.model).xyz;

    const uint routeKind = CLodReyesDecodeRouteKind(splitEntry.flags);
    bool emitCoarseDirtyOnlyLeaf = false;

    if (routeKind == CLOD_REYES_ROUTE_COARSE_HARDWARE_VSM)
    {
        float3 coarseCenterWorld = 0.0f.xxx;
        float coarseRadiusWorld = 0.0f;
        ReyesPatchBuildConservativeSphere(
            currentPosition0WS,
            currentPosition1WS,
            currentPosition2WS,
            displacementMagnitude,
            coarseCenterWorld,
            coarseRadiusWorld);

        if (ReyesPatchTouchesOnlyShadowDirtyPages(
                coarseCenterWorld,
                coarseRadiusWorld,
                clipmapIndex,
                splitEntry.viewID))
        {
            InterlockedAdd(telemetryBuffer[0].splitCoarseOnlyDirtyEligibleCount, 1u);
            emitCoarseDirtyOnlyLeaf = true;
        }
        else if (CLOD_REYES_SPLIT_SHADOW_DIRTY_HIERARCHY_DESCRIPTOR_INDEX != 0xFFFFFFFFu &&
                 CLOD_REYES_SPLIT_SHADOW_NON_RASTERABLE_HIERARCHY_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
        {
            InterlockedAdd(telemetryBuffer[0].splitCoarseOnlyDirtyRejectedCount, 1u);
        }
    }

    if (ReyesPatchShouldCull(
            currentPosition0WS,
            currentPosition1WS,
            currentPosition2WS,
            displacementMagnitude,
            sceneCamera,
            routeKind,
            clipmapIndex,
            splitEntry.viewID,
            telemetryBuffer,
            false))
    {
        return;
    }

    if (ReyesPatchHZBOccluded(
            currentPosition0OS,
            currentPosition1OS,
            currentPosition2OS,
            displacementMagnitude,
            objectData,
            sceneCamera,
            splitEntry.viewID,
            CLOD_REYES_SPLIT_PHASE_INDEX,
            telemetryBuffer))
    {
        ReyesReplayOrDropOccludedSplitPatch(splitEntry, CLOD_REYES_SPLIT_PHASE_INDEX, telemetryBuffer);
        return;
    }

    const uint nextSplitLevel = splitEntry.splitLevel + 1u;
    InterlockedMax(telemetryBuffer[0].deepestSplitLevelReached, nextSplitLevel);

    if (emitCoarseDirtyOnlyLeaf)
    {
        uint diceIndex = 0u;
        InterlockedAdd(diceQueueCounter[0], 1u, diceIndex);
        if (diceIndex >= queueCapacity)
        {
            InterlockedAdd(diceQueueOverflowCounter[0], 1u);
            InterlockedAdd(telemetryBuffer[0].diceQueueOverflowCounts[splitPassTelemetryIndex], 1u);
            return;
        }

        CLodReyesDiceQueueEntry diceEntry;
        diceEntry.visibleClusterIndex = splitEntry.visibleClusterIndex;
        diceEntry.instanceID = splitEntry.instanceID;
        diceEntry.localMeshletIndex = splitEntry.localMeshletIndex;
        diceEntry.materialIndex = splitEntry.materialIndex;
        diceEntry.viewID = splitEntry.viewID;
        diceEntry.splitLevel = nextSplitLevel;
        diceEntry.quantizedTessFactor = splitEntry.quantizedTessFactor;
        diceEntry.flags = splitEntry.flags | CLOD_REYES_FLAG_COARSE_DIRTY_ONLY_LEAF;
        diceEntry.sourcePrimitiveAndSplitConfig = (sourceTriangleIndex & 0xFFFFu);
        diceEntry.domainVertex0UV = splitEntry.domainVertex0UV;
        diceEntry.domainVertex1UV = splitEntry.domainVertex1UV;
        diceEntry.domainVertex2UV = splitEntry.domainVertex2UV;
        diceEntry.tessTableConfigIndex = 0u;
        diceEntry.reserved = 0u;
        diceQueue[diceIndex] = diceEntry;
        InterlockedAdd(telemetryBuffer[0].splitCoarseOnlyDirtyLeafOutputCount, 1u);
        InterlockedAdd(telemetryBuffer[0].splitDiceOutputCounts[splitPassTelemetryIndex], 1u);
        InterlockedAdd(telemetryBuffer[0].finalDiceQueueEntryCount, 1u);
        return;
    }

    float3 edgeFactors = ComputeReyesEdgeTessFactors(currentPosition0WS, currentPosition1WS, currentPosition2WS, camera);
    if (routeKind == CLOD_REYES_ROUTE_FINE_MICROPOLY_VSM || routeKind == CLOD_REYES_ROUTE_COARSE_HARDWARE_VSM)
    {
        if (CLOD_REYES_SPLIT_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX != 0xFFFFFFFFu &&
            clipmapIndex < kCLodVirtualShadowClipmapCount)
        {
            StructuredBuffer<CLodVirtualShadowClipmapInfo> shadowClipmapInfos =
                ResourceDescriptorHeap[CLOD_REYES_SPLIT_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
            const CLodVirtualShadowClipmapInfo clipmapInfo = shadowClipmapInfos[clipmapIndex];
            if (CLodVirtualShadowClipmapIsValid(clipmapInfo))
            {
                const float targetTexelsPerTriangle =
                    routeKind == CLOD_REYES_ROUTE_FINE_MICROPOLY_VSM
                        ? REYES_SHADOW_FINE_TARGET_TEXELS_PER_MICRO_TRIANGLE
                        : ReyesGetShadowCoarseTargetTexelsPerTriangle();
                edgeFactors = ComputeReyesShadowEdgeTessFactors(
                    currentPosition0WS,
                    currentPosition1WS,
                    currentPosition2WS,
                    clipmapInfo,
                    targetTexelsPerTriangle);
            }
        }
    }
    const float maxEdgeFactor = max(edgeFactors.x, max(edgeFactors.y, edgeFactors.z));

    const uint nextQuantizedTessFactor = (uint)min(65535.0f, ceil(maxEdgeFactor * 256.0f));

    const float2 originalDomainVertex0UV = splitEntry.domainVertex0UV;
    const float2 originalDomainVertex1UV = splitEntry.domainVertex1UV;
    const float2 originalDomainVertex2UV = splitEntry.domainVertex2UV;

    // Route to dice if edge factors fit within the tess table, or we've exhausted split passes.
    bool routeToDice = (nextSplitLevel >= maxSplitPassCount) || (maxEdgeFactor <= float(REYES_TESS_TABLE_MAX_SEGMENTS));
    bool collapseFallbackToDice = false;

    if (routeToDice)
    {
        uint diceIndex = 0u;
        InterlockedAdd(diceQueueCounter[0], 1u, diceIndex);
        if (diceIndex >= queueCapacity)
        {
            InterlockedAdd(diceQueueOverflowCounter[0], 1u);
            InterlockedAdd(telemetryBuffer[0].diceQueueOverflowCounts[splitPassTelemetryIndex], 1u);
            return;
        }

        float2 domainVertex0UV = originalDomainVertex0UV;
        float2 domainVertex1UV = originalDomainVertex1UV;
        float2 domainVertex2UV = originalDomainVertex2UV;
        const uint tessTableConfigIndex = ReyesEncodeCanonicalTessTableConfig(edgeFactors, domainVertex0UV, domainVertex1UV, domainVertex2UV);

        CLodReyesDiceQueueEntry diceEntry;
        diceEntry.visibleClusterIndex = splitEntry.visibleClusterIndex;
        diceEntry.instanceID = splitEntry.instanceID;
        diceEntry.localMeshletIndex = splitEntry.localMeshletIndex;
        diceEntry.materialIndex = splitEntry.materialIndex;
        diceEntry.viewID = splitEntry.viewID;
        diceEntry.splitLevel = nextSplitLevel;
        diceEntry.quantizedTessFactor = nextQuantizedTessFactor;
        diceEntry.flags = splitEntry.flags;
        diceEntry.sourcePrimitiveAndSplitConfig = (sourceTriangleIndex & 0xFFFFu) | ((tessTableConfigIndex & 0xFFFFu) << 16u);
        diceEntry.domainVertex0UV = domainVertex0UV;
        diceEntry.domainVertex1UV = domainVertex1UV;
        diceEntry.domainVertex2UV = domainVertex2UV;
        diceEntry.tessTableConfigIndex = tessTableConfigIndex;
        diceEntry.reserved = 0u;
        diceQueue[diceIndex] = diceEntry;
        InterlockedAdd(telemetryBuffer[0].splitDiceOutputCounts[splitPassTelemetryIndex], 1u);
        InterlockedAdd(telemetryBuffer[0].finalDiceQueueEntryCount, 1u);
        return;
    }

    // --- Tessellation-table-based splitting ---
    // Compute per-edge split factors: ceil(edgeFactor / TESS_TABLE_MAX_SEGMENTS), clamped to [1, MAX_SEGMENTS].
    // This ensures each child sub-triangle's edge factors will fit within the tess table.
    // Because split factors are computed per-edge from edge-local data, two triangles sharing
    // an edge will always agree on the split factor for that edge, producing identical boundary vertices.
    const uint3 splitFactors = ComputeReyesSplitFactors(edgeFactors);

    // Look up the tess table config for these split factors against the current domain.
    // This canonicalizes the factors (rotates so largest is first) and applies the flip bit.
    float2 domainVertex0UV = splitEntry.domainVertex0UV;
    float2 domainVertex1UV = splitEntry.domainVertex1UV;
    float2 domainVertex2UV = splitEntry.domainVertex2UV;
    const uint splitConfigIndex = ReyesEncodeCanonicalTessTableConfig(
        float3(splitFactors), domainVertex0UV, domainVertex1UV, domainVertex2UV);

    const CLodReyesTessTableConfigEntry splitConfig = ReyesGetTessTableConfigEntry(tessTableConfigs, splitConfigIndex);
    const uint childCount = splitConfig.numTriangles;
    InterlockedAdd(telemetryBuffer[0].splitChildOutputCounts[splitPassTelemetryIndex], childCount);
        uint childSurvivesBitset[(CLodReyesMaxVisibilityMicroTrianglesPerPatch + 31u) / 32u];
        [unroll]
        for (uint childWordIndex = 0u; childWordIndex < ((CLodReyesMaxVisibilityMicroTrianglesPerPatch + 31u) / 32u); ++childWordIndex)
        {
            childSurvivesBitset[childWordIndex] = 0u;
        }
    uint survivingChildCount = 0u;

    // Decode the parent's (potentially rotated/flipped) domain vertices for rebasing.
    const float3 parentDomain0 = ReyesPatchDomainUVToBarycentrics(domainVertex0UV);
    const float3 parentDomain1 = ReyesPatchDomainUVToBarycentrics(domainVertex1UV);
    const float3 parentDomain2 = ReyesPatchDomainUVToBarycentrics(domainVertex2UV);

    [loop]
    for (uint childIndex = 0u; childIndex < childCount; ++childIndex)
    {
        const uint3 triIndices = ReyesGetTessTableConfigTriangleVertexIndices(tessTableConfigs, tessTableTriangles, splitConfigIndex, childIndex);

        const float3 microBary0 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.x);
        const float3 microBary1 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.y);
        const float3 microBary2 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.z);

        precise float3 childDomain0 = parentDomain0 * microBary0.x + parentDomain1 * microBary0.y + parentDomain2 * microBary0.z;
        precise float3 childDomain1 = parentDomain0 * microBary1.x + parentDomain1 * microBary1.y + parentDomain2 * microBary1.z;
        precise float3 childDomain2 = parentDomain0 * microBary2.x + parentDomain1 * microBary2.y + parentDomain2 * microBary2.z;

        if (!ReyesPatchDomainHasValidSimplex(childDomain0, childDomain1, childDomain2))
        {
            collapseFallbackToDice = true;
            routeToDice = true;
            break;
        }

        const float3 childPosition0OS = ReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, childDomain0);
        const float3 childPosition1OS = ReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, childDomain1);
        const float3 childPosition2OS = ReyesInterpolateTriangle(sourcePosition0OS, sourcePosition1OS, sourcePosition2OS, childDomain2);
        const float3 childPosition0WS = mul(float4(childPosition0OS, 1.0f), objectData.model).xyz;
        const float3 childPosition1WS = mul(float4(childPosition1OS, 1.0f), objectData.model).xyz;
        const float3 childPosition2WS = mul(float4(childPosition2OS, 1.0f), objectData.model).xyz;

        if (ReyesPatchShouldCull(
            childPosition0WS,
            childPosition1WS,
            childPosition2WS,
            displacementMagnitude,
            sceneCamera,
            routeKind,
            clipmapIndex,
            splitEntry.viewID,
            telemetryBuffer,
            true))
        {
            continue;
        }

        CLodReyesSplitQueueEntry childEntryForOcclusion;
        childEntryForOcclusion.visibleClusterIndex = splitEntry.visibleClusterIndex;
        childEntryForOcclusion.instanceID = splitEntry.instanceID;
        childEntryForOcclusion.localMeshletIndex = splitEntry.localMeshletIndex;
        childEntryForOcclusion.materialIndex = splitEntry.materialIndex;
        childEntryForOcclusion.viewID = splitEntry.viewID;
        childEntryForOcclusion.splitLevel = nextSplitLevel;
        childEntryForOcclusion.quantizedTessFactor = nextQuantizedTessFactor;
        childEntryForOcclusion.flags = splitEntry.flags;
        childEntryForOcclusion.sourcePrimitiveAndSplitConfig = (sourceTriangleIndex & 0xFFFFu);
        childEntryForOcclusion.domainVertex0UV = ReyesPatchBarycentricsToUV(childDomain0);
        childEntryForOcclusion.domainVertex1UV = ReyesPatchBarycentricsToUV(childDomain1);
        childEntryForOcclusion.domainVertex2UV = ReyesPatchBarycentricsToUV(childDomain2);

        if (ReyesPatchHZBOccluded(
                childPosition0OS,
                childPosition1OS,
                childPosition2OS,
                displacementMagnitude,
                objectData,
                sceneCamera,
                splitEntry.viewID,
                CLOD_REYES_SPLIT_PHASE_INDEX,
                telemetryBuffer))
        {
            ReyesReplayOrDropOccludedSplitPatch(childEntryForOcclusion, CLOD_REYES_SPLIT_PHASE_INDEX, telemetryBuffer);
            continue;
        }

        childSurvivesBitset[childIndex >> 5u] |= 1u << (childIndex & 31u);
        survivingChildCount += 1u;
    }

    if (routeToDice)
    {
        if (collapseFallbackToDice)
        {
            InterlockedAdd(telemetryBuffer[0].splitCollapseFallbackDiceCount, 1u);
        }

        uint diceIndex = 0u;
        InterlockedAdd(diceQueueCounter[0], 1u, diceIndex);
        if (diceIndex >= queueCapacity)
        {
            InterlockedAdd(diceQueueOverflowCounter[0], 1u);
            InterlockedAdd(telemetryBuffer[0].diceQueueOverflowCounts[splitPassTelemetryIndex], 1u);
            return;
        }

        domainVertex0UV = originalDomainVertex0UV;
        domainVertex1UV = originalDomainVertex1UV;
        domainVertex2UV = originalDomainVertex2UV;
        const uint tessTableConfigIndex = ReyesEncodeCanonicalTessTableConfig(edgeFactors, domainVertex0UV, domainVertex1UV, domainVertex2UV);

        CLodReyesDiceQueueEntry diceEntry;
        diceEntry.visibleClusterIndex = splitEntry.visibleClusterIndex;
        diceEntry.instanceID = splitEntry.instanceID;
        diceEntry.localMeshletIndex = splitEntry.localMeshletIndex;
        diceEntry.materialIndex = splitEntry.materialIndex;
        diceEntry.viewID = splitEntry.viewID;
        diceEntry.splitLevel = nextSplitLevel;
        diceEntry.quantizedTessFactor = nextQuantizedTessFactor;
        diceEntry.flags = splitEntry.flags;
        diceEntry.sourcePrimitiveAndSplitConfig = (sourceTriangleIndex & 0xFFFFu) | ((tessTableConfigIndex & 0xFFFFu) << 16u);
        diceEntry.domainVertex0UV = domainVertex0UV;
        diceEntry.domainVertex1UV = domainVertex1UV;
        diceEntry.domainVertex2UV = domainVertex2UV;
        diceEntry.tessTableConfigIndex = tessTableConfigIndex;
        diceEntry.reserved = 0u;
        diceQueue[diceIndex] = diceEntry;
        InterlockedAdd(telemetryBuffer[0].splitDiceOutputCounts[splitPassTelemetryIndex], 1u);
        InterlockedAdd(telemetryBuffer[0].finalDiceQueueEntryCount, 1u);
        return;
    }

    uint outputSplitBaseIndex = 0u;
    InterlockedAdd(outputSplitQueueCounter[0], survivingChildCount, outputSplitBaseIndex);
    if (outputSplitBaseIndex >= queueCapacity)
    {
        InterlockedAdd(outputSplitQueueOverflowCounter[0], survivingChildCount);
        InterlockedAdd(telemetryBuffer[0].splitQueueOverflowCounts[splitPassTelemetryIndex], survivingChildCount);
        return;
    }

    const uint maxWritableChildren = min(survivingChildCount, queueCapacity - outputSplitBaseIndex);
    uint emittedChildCount = 0u;

    for (uint childIndex = 0u; childIndex < childCount && emittedChildCount < maxWritableChildren; ++childIndex)
    {
        if ((childSurvivesBitset[childIndex >> 5u] & (1u << (childIndex & 31u))) == 0u)
        {
            continue;
        }

        // Get the micro-triangle's vertex indices from the tess table.
        const uint3 triIndices = ReyesGetTessTableConfigTriangleVertexIndices(tessTableConfigs, tessTableTriangles, splitConfigIndex, childIndex);

        // Get each micro-triangle vertex's barycentrics within the split pattern.
        const float3 microBary0 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.x);
        const float3 microBary1 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.y);
        const float3 microBary2 = ReyesGetTessTableConfigVertexBarycentrics(tessTableConfigs, tessTableVertices, splitConfigIndex, triIndices.z);

        // Rebase: convert micro-triangle barycentrics through the parent domain
        // to get new domain vertices in the original source triangle's barycentric space.
        precise float3 childDomain0 = parentDomain0 * microBary0.x + parentDomain1 * microBary0.y + parentDomain2 * microBary0.z;
        precise float3 childDomain1 = parentDomain0 * microBary1.x + parentDomain1 * microBary1.y + parentDomain2 * microBary1.z;
        precise float3 childDomain2 = parentDomain0 * microBary2.x + parentDomain1 * microBary2.y + parentDomain2 * microBary2.z;

        CLodReyesSplitQueueEntry childEntry;
        childEntry.visibleClusterIndex = splitEntry.visibleClusterIndex;
        childEntry.instanceID = splitEntry.instanceID;
        childEntry.localMeshletIndex = splitEntry.localMeshletIndex;
        childEntry.materialIndex = splitEntry.materialIndex;
        childEntry.viewID = splitEntry.viewID;
        childEntry.splitLevel = nextSplitLevel;
        childEntry.quantizedTessFactor = nextQuantizedTessFactor;
        childEntry.flags = splitEntry.flags;
        childEntry.sourcePrimitiveAndSplitConfig = (sourceTriangleIndex & 0xFFFFu);
        childEntry.domainVertex0UV = ReyesPatchBarycentricsToUV(childDomain0);
        childEntry.domainVertex1UV = ReyesPatchBarycentricsToUV(childDomain1);
        childEntry.domainVertex2UV = ReyesPatchBarycentricsToUV(childDomain2);

        outputSplitQueue[outputSplitBaseIndex + emittedChildCount] = childEntry;
        emittedChildCount += 1u;
    }

    if (maxWritableChildren < survivingChildCount)
    {
        const uint overflowCount = survivingChildCount - maxWritableChildren;
        InterlockedAdd(outputSplitQueueOverflowCounter[0], overflowCount);
        InterlockedAdd(telemetryBuffer[0].splitQueueOverflowCounts[splitPassTelemetryIndex], overflowCount);
    }
}
