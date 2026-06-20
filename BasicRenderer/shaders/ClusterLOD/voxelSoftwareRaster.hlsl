#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/debugPayload.hlsli"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"
#include "include/clodStructs.hlsli"

#ifndef CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#define CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW 0
#endif

#ifndef CLOD_MAX_DDA_STEPS
#define CLOD_MAX_DDA_STEPS 16u
#endif

#define CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE 1

#ifndef CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
#define CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE 0
#endif

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
#ifndef CLOD_VOXEL_RASTER_THREADS_PER_GROUP
#define CLOD_VOXEL_RASTER_THREADS_PER_GROUP 64u
#endif

#ifndef CLOD_VOXEL_RASTER_PIXEL_QUEUE_CAPACITY
#define CLOD_VOXEL_RASTER_PIXEL_QUEUE_CAPACITY 256u
#endif
#else
#ifndef CLOD_VOXEL_RASTER_THREADS_PER_GROUP
#define CLOD_VOXEL_RASTER_THREADS_PER_GROUP 128u
#endif

#ifndef CLOD_VOXEL_RASTER_PIXEL_QUEUE_CAPACITY
#define CLOD_VOXEL_RASTER_PIXEL_QUEUE_CAPACITY 128u
#endif
#endif

static const uint VOXEL_RASTER_THREADS_PER_GROUP = CLOD_VOXEL_RASTER_THREADS_PER_GROUP;
static const uint VOXEL_RASTER_PIXEL_QUEUE_CAPACITY = CLOD_VOXEL_RASTER_PIXEL_QUEUE_CAPACITY;
static const uint64_t VOXEL_RASTER_VISIBILITY_EMPTY = 0xFFFFFFFFFFFFFFFF;
static const uint WG_COUNTER_VOXEL_RASTER_WORK_GROUPS = 134u;
static const uint WG_COUNTER_VOXEL_RASTER_INVALID_PACKED_CLUSTER = 135u;
static const uint WG_COUNTER_VOXEL_RASTER_DESCRIPTOR_MISSES = 136u;
static const uint WG_COUNTER_VOXEL_RASTER_INVALID_CLUSTER = 137u;
static const uint WG_COUNTER_VOXEL_RASTER_INVALID_VOXEL_WIDTH = 138u;
static const uint WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED = 139u;
static const uint WG_COUNTER_VOXEL_RASTER_SCISSOR_REJECTED = 140u;
static const uint WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED = 141u;
static const uint WG_COUNTER_VOXEL_RASTER_DDA_MISSES = 142u;
static const uint WG_COUNTER_VOXEL_RASTER_VISIBILITY_WRITES = 143u;
static const uint WG_COUNTER_VOXEL_RASTER_PROJECTED_PIXELS = 144u;
static const uint WG_COUNTER_VOXEL_RASTER_QUEUED_PIXELS = 145u;
static const uint WG_COUNTER_VOXEL_RASTER_QUEUE_OVERFLOW = 146u;
static const uint WG_COUNTER_VOXEL_RASTER_NON_POSITIVE_DEPTH = 147u;
static const uint WG_COUNTER_VOXEL_RASTER_VISIBILITY_WINS = 148u;
static const uint WG_COUNTER_VOXEL_RASTER_VISIBILITY_LOSSES = 149u;

// Voxel cubes are coarse coverage proxies. Their projected AABB depth bounds are
// often too conservative for grass sitting directly on terrain, so use the
// visibility buffer only for the final ordered write rather than as a pre-DDA
// rejection source.
static const bool CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST = false;

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
groupshared uint gs_voxelRasterQueueCount;
groupshared uint gs_voxelRasterQueueReadCursor;
groupshared uint2 gs_voxelRasterPixelQueue[VOXEL_RASTER_PIXEL_QUEUE_CAPACITY];
#endif

bool VoxelMaskTest(uint2 mask, uint bitIndex)
{
    return bitIndex < 32u ? ((mask.x & (1u << bitIndex)) != 0u) : ((mask.y & (1u << (bitIndex - 32u))) != 0u);
}

void VoxelRasterTelemetryAdd(uint counterIndex, uint value)
{
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return;
    }

    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryCounters[counterIndex], value);
}

bool RayBoxIntersect(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tEnter, out float tExit)
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

bool RaycastVoxelCubeDDA(float3 rayOrigin, float3 rayDir, uint2 occupancyMask, out float tHit)
{
    tHit = 0.0f;

    float tEnter = 0.0f;
    float tExit = 0.0f;
    if (!RayBoxIntersect(rayOrigin, rayDir, float3(0.0f, 0.0f, 0.0f), float3(4.0f, 4.0f, 4.0f), tEnter, tExit))
    {
        return false;
    }

    float currentT = max(tEnter, 0.0f) + 1e-4f;
    const float startT = currentT;
    float3 p = clamp(rayOrigin + rayDir * startT, float3(0.0f, 0.0f, 0.0f), float3(3.9999f, 3.9999f, 3.9999f));
    int3 cell = int3(floor(p));
    const int3 stepDir = int3(rayDir.x >= 0.0f ? 1 : -1, rayDir.y >= 0.0f ? 1 : -1, rayDir.z >= 0.0f ? 1 : -1);

    float3 nextBoundary = float3(
        stepDir.x > 0 ? float(cell.x + 1) : float(cell.x),
        stepDir.y > 0 ? float(cell.y + 1) : float(cell.y),
        stepDir.z > 0 ? float(cell.z + 1) : float(cell.z));
    const float largeT = 3.402823e+38f;
    float3 tMax = float3(
        abs(rayDir.x) > 1.0e-8f ? (nextBoundary.x - rayOrigin.x) / rayDir.x : largeT,
        abs(rayDir.y) > 1.0e-8f ? (nextBoundary.y - rayOrigin.y) / rayDir.y : largeT,
        abs(rayDir.z) > 1.0e-8f ? (nextBoundary.z - rayOrigin.z) / rayDir.z : largeT);
    float3 tDelta = float3(
        abs(rayDir.x) > 1.0e-8f ? abs(1.0f / rayDir.x) : largeT,
        abs(rayDir.y) > 1.0e-8f ? abs(1.0f / rayDir.y) : largeT,
        abs(rayDir.z) > 1.0e-8f ? abs(1.0f / rayDir.z) : largeT);

    [loop]
    for (uint iter = 0u; iter < CLOD_MAX_DDA_STEPS; ++iter)
    {
        if (any(cell < 0) || any(cell >= 4))
        {
            break;
        }

        const uint cellIndex = (uint)cell.x | ((uint)cell.y << 2u) | ((uint)cell.z << 4u);
        if (VoxelMaskTest(occupancyMask, cellIndex))
        {
            tHit = currentT;
            return true;
        }

        if (tMax.x <= tMax.y && tMax.x <= tMax.z)
        {
            if (tMax.x > tExit) break;
            currentT = tMax.x + 1e-4f;
            cell.x += stepDir.x;
            tMax.x += tDelta.x;
        }
        else if (tMax.y <= tMax.z)
        {
            if (tMax.y > tExit) break;
            currentT = tMax.y + 1e-4f;
            cell.y += stepDir.y;
            tMax.y += tDelta.y;
        }
        else
        {
            if (tMax.z > tExit) break;
            currentT = tMax.z + 1e-4f;
            cell.z += stepDir.z;
            tMax.z += tDelta.z;
        }
    }

    return false;
}

void VoxelRasterLoadCubeHeader(
    uint slabDescriptorIndex,
    uint pageByteOffset,
    uint cubeRecordsOffset,
    uint pageLocalCubeIndex,
    out uint cubeCoord,
    out uint dominantBoneIndex,
    out uint2 occupancyMask)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    const uint addr = pageByteOffset + cubeRecordsOffset + pageLocalCubeIndex * CLOD_VOXEL_CUBE_RECORD_STRIDE;
    const uint4 d0 = slab.Load4(addr + 0u);
    cubeCoord = d0.x;
    dominantBoneIndex = d0.y;
    occupancyMask = d0.zw;
}

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
bool VoxelRasterWriteVirtualShadow(
    uint2 pixel,
    float linearDepth,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable,
    RWTexture2D<uint> physicalPages)
{
    const float2 shadowUv = saturate((float2(pixel) + 0.5f) / max((float)clipmapInfo.virtualResolution, 1.0f));
    const uint2 virtualPageCoords = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(virtualPageCoords, clipmapInfo);

    const uint3 pageCoords = uint3(wrappedPageCoords, clipmapInfo.pageTableLayer);
    const uint pageEntry = pageTable[pageCoords];
    if (!CLodVirtualShadowPageEntryCanRaster(pageEntry))
    {
        return false;
    }

    const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
    const uint2 virtualTexelCoords = CLodVirtualShadowVirtualTexelCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 atlasPixel = CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapInfo);

    InterlockedMin(physicalPages[atlasPixel], asuint(linearDepth));
    uint ignored = 0u;
    InterlockedOr(pageTable[pageCoords], kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask, ignored);
    return true;
}
#endif

[numthreads(1, 1, 1)]
void VoxelRasterBuildDispatchArgsCS()
{
    StructuredBuffer<uint> counter = ResourceDescriptorHeap[CLOD_RASTER_VOXEL_WORK_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodVoxelRasterDispatchCommand> args = ResourceDescriptorHeap[CLOD_RASTER_VOXEL_INDIRECT_ARGS_DESCRIPTOR_INDEX];
    const uint count = min(counter[0], CLOD_RASTER_VOXEL_WORK_CAPACITY);
    args[0].dispatchX = count;
    args[0].dispatchY = 1u;
    args[0].dispatchZ = 1u;
}

[numthreads(VOXEL_RASTER_THREADS_PER_GROUP, 1, 1)]
void VoxelRasterCS(uint3 groupId : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    const uint workIndex = groupId.x;
    const uint GI = groupThreadID.x;
    StructuredBuffer<uint> counter = ResourceDescriptorHeap[CLOD_RASTER_VOXEL_WORK_COUNTER_DESCRIPTOR_INDEX];
    const uint workCount = min(counter[0], CLOD_RASTER_VOXEL_WORK_CAPACITY);
    if (workIndex >= workCount)
    {
        return;
    }

    StructuredBuffer<CLodVoxelRasterWorkRecord> workRecords = ResourceDescriptorHeap[CLOD_RASTER_VOXEL_WORK_RECORDS_DESCRIPTOR_INDEX];
    const CLodVoxelRasterWorkRecord work = workRecords[workIndex];
    ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_RASTER_VOXEL_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, work.visibleClusterIndex);
    if (!CLodVisibleClusterIsVoxel(packedCluster))
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_PACKED_CLUSTER, 1u);
        }
        return;
    }
    if (GI == 0u)
    {
        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_WORK_GROUPS, 1u);
    }

    StructuredBuffer<CLodMeshMetadata> metadataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];

    const uint instanceIndex = CLodVisibleClusterInstanceID(packedCluster);
    const uint viewId = CLodVisibleClusterViewID(packedCluster);
    const uint localGroupId = CLodVisibleClusterGroupID(packedCluster);
    const uint localVoxelClusterIndex = CLodVisibleClusterVoxelClusterIndex(packedCluster);

    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(instanceIndex);
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(instanceIndex);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceIndex);
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    const CullingCameraInfo camera = cameras[viewId];
    const ClodViewRasterInfo rasterInfo = viewRasterInfoBuffer[viewId];

    CLodVoxelGroupDescriptor descriptor;
    if (!CLodTryLoadVoxelDescriptorByClusterIndex(metadata, localGroupId, localVoxelClusterIndex, descriptor))
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DESCRIPTOR_MISSES, 1u);
        }
        return;
    }

    GroupPageMapEntry pageEntry;
    CLodVoxelPageHeader pageHeader;
    const CLodVoxelClusterRecord voxelCluster = CLodLoadVoxelCluster(metadata, descriptor, localGroupId, localVoxelClusterIndex, pageEntry, pageHeader);
    if (voxelCluster.cubeCount == 0u || voxelCluster.cubeCount > CLOD_VOXEL_MAX_CUBES_PER_CLUSTER)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_CLUSTER, 1u);
        }
        return;
    }

    const float voxelWidth = descriptor.aabbMinAndVoxelWidth.w;
    if (voxelWidth <= 0.0f)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_VOXEL_WIDTH, 1u);
        }
        return;
    }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    uint2 targetDims = uint2(rasterInfo.scissorMaxX, rasterInfo.scissorMaxY);
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    if (!CLodVirtualShadowTryGetClipmapInfoForView(viewId, clipmapInfos, clipmapInfo))
    {
        return;
    }
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWTexture2D<uint> physicalPages = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX];
#else
    if (rasterInfo.visibilityUAVDescriptorIndex == 0xFFFFFFFFu)
    {
        return;
    }
    RWTexture2D<uint64_t> visibilityBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(rasterInfo.visibilityUAVDescriptorIndex)];
    uint2 targetDims;
    visibilityBuffer.GetDimensions(targetDims.x, targetDims.y);
#endif

    RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool debugMode = perFrameBuffer.outputType == OUTPUT_SW_RASTER;
    const row_major matrix objectModel = objectData.model;
    const float2 targetDimsInv = rcp(max(float2(targetDims), float2(1.0f, 1.0f)));

    for (uint cubeOffset = 0u; cubeOffset < voxelCluster.cubeCount; ++cubeOffset)
    {
        uint cubeCoordPacked = 0u;
        uint dominantBoneIndex = 0u;
        uint2 occupancyMask = uint2(0u, 0u);
        VoxelRasterLoadCubeHeader(
            pageEntry.slabDescriptorIndex,
            pageEntry.slabByteOffset,
            pageHeader.cubeRecordsOffset,
            voxelCluster.firstCube + cubeOffset,
            cubeCoordPacked,
            dominantBoneIndex,
            occupancyMask);
        if (occupancyMask.x == 0u && occupancyMask.y == 0u)
        {
            continue;
        }

        const uint3 cubeCoord = CLodVoxelDecodeCubeCoord(cubeCoordPacked);
        const float cubeObjectWidth = voxelWidth * 4.0f;
        const float3 cubeMinObject = descriptor.aabbMinAndVoxelWidth.xyz + float3(cubeCoord) * cubeObjectWidth;
        const float3 cubeMaxObject = cubeMinObject + cubeObjectWidth;
        const float invVoxelWidth = rcp(voxelWidth);

        row_major matrix localToWorld = objectModel;
        row_major matrix worldToLocal = objectData.modelInverse;
        if (dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
        {
            const float4x4 skinMatrix = LoadBoneSkinMatrix(meshInstance.skinningInstanceSlot, dominantBoneIndex);
            const float4x4 inverseSkinMatrix = LoadBoneInverseSkinMatrix(meshInstance.skinningInstanceSlot, dominantBoneIndex);
            localToWorld = mul(skinMatrix, objectModel);
            worldToLocal = mul(objectData.modelInverse, inverseSkinMatrix);
        }
        const row_major matrix localToClip = mul(localToWorld, camera.viewProjection);
        const float4 localViewZ = mul(localToWorld, camera.viewZ);

        float2 screenMin = float2(3.402823e+38f, 3.402823e+38f);
        float2 screenMax = float2(-3.402823e+38f, -3.402823e+38f);
        float cubeMinLinearDepth = 3.402823e+38f;
        float cubeMaxLinearDepth = 0.0f;
        bool validProjection = false;
        bool hasBehindNearCorner = false;
        [unroll]
        for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
        {
            const float3 corner = float3(
                (cornerIndex & 1u) ? cubeMaxObject.x : cubeMinObject.x,
                (cornerIndex & 2u) ? cubeMaxObject.y : cubeMinObject.y,
                (cornerIndex & 4u) ? cubeMaxObject.z : cubeMinObject.z);
            const float cornerLinearDepth = -dot(float4(corner, 1.0f), localViewZ);
            if (cornerLinearDepth > 0.0f)
            {
                cubeMinLinearDepth = min(cubeMinLinearDepth, cornerLinearDepth);
                cubeMaxLinearDepth = max(cubeMaxLinearDepth, cornerLinearDepth);
            }
            else
            {
                hasBehindNearCorner = true;
            }
            const float4 clip = mul(float4(corner, 1.0f), localToClip);
            if (clip.w <= 0.0f)
            {
                continue;
            }
            const float2 ndc = clip.xy / clip.w;
            const float2 screen = float2(
                (ndc.x + 1.0f) * 0.5f * float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX) + float(rasterInfo.scissorMinX),
                (1.0f - ndc.y) * 0.5f * float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY) + float(rasterInfo.scissorMinY));
            screenMin = min(screenMin, screen);
            screenMax = max(screenMax, screen);
            validProjection = true;
        }
        if (!validProjection || cubeMaxLinearDepth <= 0.0f)
        {
            if (GI == 0u)
            {
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED, 1u);
            }
            continue;
        }
        if (hasBehindNearCorner)
        {
            cubeMinLinearDepth = 0.0f;
        }

        int2 minPx = int2(floor(screenMin));
        int2 maxPx = int2(floor(screenMax));
        minPx = max(minPx, int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY));
        maxPx = min(maxPx, int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1));
        minPx = max(minPx, int2(0, 0));
        maxPx = min(maxPx, int2(int(targetDims.x) - 1, int(targetDims.y) - 1));
        if (minPx.x > maxPx.x || minPx.y > maxPx.y)
        {
            if (GI == 0u)
            {
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SCISSOR_REJECTED, 1u);
            }
            continue;
        }

        const float3 cameraOriginLocal = mul(float4(camera.positionWorldSpace.xyz, 1.0f), worldToLocal).xyz;
        const float rayOriginViewZ = dot(float4(cameraOriginLocal, 1.0f), localViewZ);
        const float3 rayOriginCube = (cameraOriginLocal - cubeMinObject) * invVoxelWidth;
        const uint pixelWidth = uint(maxPx.x - minPx.x + 1);
        const uint pixelHeight = uint(maxPx.y - minPx.y + 1);
        const uint pixelCount = pixelWidth * pixelHeight;
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTED_PIXELS, pixelCount);
        }

#if !CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
        for (uint pixelLinear = GI; pixelLinear < pixelCount; pixelLinear += VOXEL_RASTER_THREADS_PER_GROUP)
        {
            const uint px = uint(minPx.x + int(pixelLinear % pixelWidth));
            const uint py = uint(minPx.y + int(pixelLinear / pixelWidth));

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
            const uint64_t currentVisKey = visibilityBuffer[uint2(px, py)];
            if (CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST)
            {
                if (currentVisKey != VOXEL_RASTER_VISIBILITY_EMPTY)
                {
                    float currentDepth = 0.0f;
                    uint currentClusterId = 0u;
                    uint currentPrimId = 0u;
                    UnpackVisKey(currentVisKey, currentDepth, currentClusterId, currentPrimId);
                    if (currentDepth <= cubeMinLinearDepth)
                    {
                        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED, 1u);
                        continue;
                    }
                }
            }
#endif

            const float2 uv = (float2(px, py) + 0.5f) * targetDimsInv;
            const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
            float4 viewNear = mul(float4(ndc, 0.0f, 1.0f), camera.projectionInverse);
            viewNear.xyz /= max(viewNear.w, 1.0e-6f);
            const float4 worldNear = mul(float4(viewNear.xyz, 1.0f), camera.viewInverse);
            const float3 worldPoint = worldNear.xyz / max(worldNear.w, 1.0e-6f);
            const float3 localPoint = mul(float4(worldPoint, 1.0f), worldToLocal).xyz;
            const float3 rayDirObject = normalize(localPoint - cameraOriginLocal);
            const float3 rayDirCube = rayDirObject * invVoxelWidth;

            float tHitCube = 0.0f;
            if (!RaycastVoxelCubeDDA(rayOriginCube, rayDirCube, occupancyMask, tHitCube))
            {
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DDA_MISSES, 1u);
                continue;
            }

            const float linearDepth = -(rayOriginViewZ + tHitCube * dot(rayDirObject, localViewZ.xyz));
            if (linearDepth <= 0.0f)
            {
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_NON_POSITIVE_DEPTH, 1u);
                continue;
            }

            if (debugMode)
            {
                WriteDebugPixel(debugVisTex, uint2(px, py), PackDebugUint(2u));
            }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
            VoxelRasterWriteVirtualShadow(uint2(px, py), linearDepth, clipmapInfo, pageTable, physicalPages);
#else
            const uint64_t visKey = PackVisKey(linearDepth, work.visibleClusterIndex, cubeOffset);
            uint64_t previousVisKey = 0u;
            InterlockedMin(visibilityBuffer[uint2(px, py)], visKey, previousVisKey);
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_VISIBILITY_WRITES, 1u);
            VoxelRasterTelemetryAdd(
                previousVisKey > visKey
                    ? WG_COUNTER_VOXEL_RASTER_VISIBILITY_WINS
                    : WG_COUNTER_VOXEL_RASTER_VISIBILITY_LOSSES,
                1u);
#endif
        }
#else
        for (uint queueBase = 0u; queueBase < pixelCount; queueBase += VOXEL_RASTER_PIXEL_QUEUE_CAPACITY)
        {
            if (GI == 0u)
            {
                gs_voxelRasterQueueCount = 0u;
            }
            GroupMemoryBarrierWithGroupSync();

            const uint queueEnd = min(queueBase + VOXEL_RASTER_PIXEL_QUEUE_CAPACITY, pixelCount);
            for (uint pixelLinear = queueBase + GI; pixelLinear < queueEnd; pixelLinear += VOXEL_RASTER_THREADS_PER_GROUP)
            {
                const int px = minPx.x + int(pixelLinear % pixelWidth);
                const int py = minPx.y + int(pixelLinear / pixelWidth);
                bool enqueuePixel = true;

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                const uint64_t currentVisKey = visibilityBuffer[uint2(px, py)];
                if (CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST &&
                    currentVisKey != VOXEL_RASTER_VISIBILITY_EMPTY)
                {
                    float currentDepth = 0.0f;
                    uint currentClusterId = 0u;
                    uint currentPrimId = 0u;
                    UnpackVisKey(currentVisKey, currentDepth, currentClusterId, currentPrimId);
                    enqueuePixel = currentDepth > cubeMinLinearDepth;
                    if (!enqueuePixel)
                    {
                        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED, 1u);
                    }
                }
#endif

                if (enqueuePixel)
                {
                    uint queueSlot = 0u;
                    InterlockedAdd(gs_voxelRasterQueueCount, 1u, queueSlot);
                    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_QUEUED_PIXELS, 1u);
                    if (queueSlot < VOXEL_RASTER_PIXEL_QUEUE_CAPACITY)
                    {
                        gs_voxelRasterPixelQueue[queueSlot] = uint2(px, py);
                    }
                    else
                    {
                        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_QUEUE_OVERFLOW, 1u);
                    }
                }
            }

            GroupMemoryBarrierWithGroupSync();

            if (GI == 0u)
            {
                gs_voxelRasterQueueReadCursor = 0u;
            }
            GroupMemoryBarrierWithGroupSync();

            [loop]
            while (true)
            {
                uint queueSlot = 0u;
                InterlockedAdd(gs_voxelRasterQueueReadCursor, 1u, queueSlot);
                if (queueSlot >= gs_voxelRasterQueueCount)
                {
                    break;
                }
                if (queueSlot >= VOXEL_RASTER_PIXEL_QUEUE_CAPACITY)
                {
                    break;
                }

                const uint2 pixel = gs_voxelRasterPixelQueue[queueSlot];
                const uint px = pixel.x;
                const uint py = pixel.y;
                const float2 uv = (float2(px, py) + 0.5f) * targetDimsInv;
                const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
                float4 viewNear = mul(float4(ndc, 0.0f, 1.0f), camera.projectionInverse);
                viewNear.xyz /= max(viewNear.w, 1.0e-6f);
                const float4 worldNear = mul(float4(viewNear.xyz, 1.0f), camera.viewInverse);
                const float3 worldPoint = worldNear.xyz / max(worldNear.w, 1.0e-6f);
                const float3 localPoint = mul(float4(worldPoint, 1.0f), worldToLocal).xyz;
                const float3 rayDirObject = normalize(localPoint - cameraOriginLocal);
                const float3 rayDirCube = rayDirObject * invVoxelWidth;

                float tHitCube = 0.0f;
                if (!RaycastVoxelCubeDDA(rayOriginCube, rayDirCube, occupancyMask, tHitCube))
                {
                    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DDA_MISSES, 1u);
                    continue;
                }

                const float linearDepth = -(rayOriginViewZ + tHitCube * dot(rayDirObject, localViewZ.xyz));
                if (linearDepth <= 0.0f)
                {
                    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_NON_POSITIVE_DEPTH, 1u);
                    continue;
                }

                if (debugMode)
                {
                    WriteDebugPixel(debugVisTex, uint2(px, py), PackDebugUint(2u));
                }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                VoxelRasterWriteVirtualShadow(uint2(px, py), linearDepth, clipmapInfo, pageTable, physicalPages);
#else
                const uint64_t visKey = PackVisKey(linearDepth, work.visibleClusterIndex, cubeOffset);
                uint64_t previousVisKey = 0u;
                InterlockedMin(visibilityBuffer[uint2(px, py)], visKey, previousVisKey);
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_VISIBILITY_WRITES, 1u);
                VoxelRasterTelemetryAdd(
                    previousVisKey > visKey
                        ? WG_COUNTER_VOXEL_RASTER_VISIBILITY_WINS
                        : WG_COUNTER_VOXEL_RASTER_VISIBILITY_LOSSES,
                    1u);
#endif
            }

            GroupMemoryBarrierWithGroupSync();
        }
#endif
    }
}
