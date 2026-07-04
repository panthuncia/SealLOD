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

#ifndef PSO_SKINNED
#define PSO_SKINNED 0
#endif

#ifndef CLOD_VOXEL_RASTER_TELEMETRY
#define CLOD_VOXEL_RASTER_TELEMETRY 0
#endif

#ifndef CLOD_VOXEL_RASTER_DEBUG_VIS
#define CLOD_VOXEL_RASTER_DEBUG_VIS 0
#endif

#ifndef CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
#define CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE 1
#endif

#ifndef CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
#define CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT 1
#endif

#ifndef CLOD_VOXEL_RASTER_MAX_PROJECTED_FOOTPRINT_PIXELS
#define CLOD_VOXEL_RASTER_MAX_PROJECTED_FOOTPRINT_PIXELS 64.0f
#endif

#ifndef CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE
#define CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE 32u
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
static const uint VOXEL_RASTER_CUBE_BATCH_SIZE = min(CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
static const uint64_t VOXEL_RASTER_VISIBILITY_EMPTY = 0xFFFFFFFFFFFFFFFF;
static const uint WG_COUNTER_VOXEL_RASTER_WORK_GROUPS = 134u;
static const uint WG_COUNTER_VOXEL_RASTER_INVALID_PACKED_CLUSTER = 135u;
static const uint WG_COUNTER_VOXEL_RASTER_SEGMENT_PAGE_MISSES = 136u;
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
static const uint VOXEL_RASTER_TRACE_HIT = 0u;
static const uint VOXEL_RASTER_TRACE_DDA_MISS = 1u;
static const uint VOXEL_RASTER_TRACE_NON_POSITIVE_DEPTH = 2u;
static const uint VOXEL_RASTER_PROJECT_OK = 0u;
static const uint VOXEL_RASTER_PROJECT_REJECTED = 1u;
static const uint VOXEL_RASTER_PROJECT_SCISSOR_REJECTED = 2u;
static const uint VOXEL_RASTER_PROJECT_FOOTPRINT_REJECTED = 3u;

#ifndef CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
#define CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST 1
#endif

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
groupshared uint gs_voxelRasterQueueCount;
groupshared uint gs_voxelRasterPixelQueue[VOXEL_RASTER_PIXEL_QUEUE_CAPACITY];
groupshared uint gs_voxelRasterBatchMaxPixelCount;
#endif

bool VoxelMaskTest(uint2 mask, uint bitIndex)
{
    return bitIndex < 32u ? ((mask.x & (1u << bitIndex)) != 0u) : ((mask.y & (1u << (bitIndex - 32u))) != 0u);
}

void VoxelRasterTelemetryAdd(uint counterIndex, uint value)
{
#if CLOD_VOXEL_RASTER_TELEMETRY
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return;
    }

    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryCounters[counterIndex], value);
#endif
}

bool RaycastVoxelCubeDDA(float3 rayOrigin, float3 rayDir, uint2 occupancyMask, out float tHit)
{
    tHit = 0.0f;

    const float largeT = 3.402823e+38f;
    const float3 rayDirAbs = abs(rayDir);
    if ((rayDirAbs.x <= 1.0e-8f && (rayOrigin.x < 0.0f || rayOrigin.x > 4.0f)) ||
        (rayDirAbs.y <= 1.0e-8f && (rayOrigin.y < 0.0f || rayOrigin.y > 4.0f)) ||
        (rayDirAbs.z <= 1.0e-8f && (rayOrigin.z < 0.0f || rayOrigin.z > 4.0f)))
    {
        return false;
    }

    const float3 invRayDir = float3(
        rayDirAbs.x > 1.0e-8f ? rcp(rayDir.x) : largeT,
        rayDirAbs.y > 1.0e-8f ? rcp(rayDir.y) : largeT,
        rayDirAbs.z > 1.0e-8f ? rcp(rayDir.z) : largeT);
    const float3 t0 = (0.0f.xxx - rayOrigin) * invRayDir;
    const float3 t1 = (4.0f.xxx - rayOrigin) * invRayDir;
    const float3 tNear = min(t0, t1);
    const float3 tFar = max(t0, t1);
    const float tEnter = max(max(tNear.x, tNear.y), max(tNear.z, 0.0f));
    const float tExit = min(tFar.x, min(tFar.y, tFar.z));
    if (tExit < tEnter)
    {
        return false;
    }

    if ((occupancyMask.x & occupancyMask.y) == 0xFFFFFFFFu)
    {
        tHit = tEnter;
        return true;
    }

    float currentT = tEnter + 1e-4f;
    const float startT = currentT;
    float3 p = clamp(rayOrigin + rayDir * startT, float3(0.0f, 0.0f, 0.0f), float3(3.9999f, 3.9999f, 3.9999f));
    int3 cell = int3(floor(p));
    const int3 stepDir = int3(rayDir.x >= 0.0f ? 1 : -1, rayDir.y >= 0.0f ? 1 : -1, rayDir.z >= 0.0f ? 1 : -1);

    float3 nextBoundary = float3(
        stepDir.x > 0 ? float(cell.x + 1) : float(cell.x),
        stepDir.y > 0 ? float(cell.y + 1) : float(cell.y),
        stepDir.z > 0 ? float(cell.z + 1) : float(cell.z));
    float3 tMax = float3(
        rayDirAbs.x > 1.0e-8f ? (nextBoundary.x - rayOrigin.x) * invRayDir.x : largeT,
        rayDirAbs.y > 1.0e-8f ? (nextBoundary.y - rayOrigin.y) * invRayDir.y : largeT,
        rayDirAbs.z > 1.0e-8f ? (nextBoundary.z - rayOrigin.z) * invRayDir.z : largeT);
    const float3 tDelta = abs(invRayDir);

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

struct VoxelRasterProjectedCube
{
    int2 minPx;
    int2 maxPx;
    float minLinearDepth;
};

struct VoxelRasterPreparedCube
{
    uint2 occupancyMask;
    uint pixelCount;
    uint pixelWidth;
    float invVoxelWidth;
    float rayOriginViewZ;
    float3 cameraOriginLocal;
    float3 rayOriginCube;
    float4 localViewZ;
    row_major matrix viewToLocal;
    VoxelRasterProjectedCube projected;
};

struct VoxelRasterVoxelSetup
{
    float3 aabbMin;
    float cubeObjectWidth;
    float invVoxelWidth;
};

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
groupshared VoxelRasterPreparedCube gs_voxelRasterPreparedCubes[CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE];
groupshared uint gs_voxelRasterPreparedCubeOffsets[CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE];
#endif

#if !PSO_SKINNED
struct VoxelRasterRigidSetup
{
    float sphereRadius;
    float rayOriginViewZ;
    float3 cameraOriginLocal;
    float4 localViewZ;
    row_major matrix localToWorld;
    row_major matrix viewToLocal;
};
#endif

uint2 VoxelRasterPixelFromLinear(uint pixelLinear, int2 minPx, uint pixelWidth)
{
    return uint2(
        uint(minPx.x + int(pixelLinear % pixelWidth)),
        uint(minPx.y + int(pixelLinear / pixelWidth)));
}

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
void VoxelRasterQueueReset()
{
    gs_voxelRasterQueueCount = 0u;
}

uint VoxelRasterPackQueuedPixel(uint cubeSlot, uint pixelLinear)
{
    return ((cubeSlot & 0xFFu) << 16u) | (pixelLinear & 0xFFFFu);
}

uint VoxelRasterQueuedPixelCubeSlot(uint queuedPixel)
{
    return (queuedPixel >> 16u) & 0xFFu;
}

uint VoxelRasterQueuedPixelLinear(uint queuedPixel)
{
    return queuedPixel & 0xFFFFu;
}

void VoxelRasterQueuePush(bool enqueuePixel, uint queuedPixel)
{
    const uint waveEnqueueCount = WaveActiveCountBits(enqueuePixel);
    uint waveBaseSlot = 0u;
    if (WaveIsFirstLane() && waveEnqueueCount != 0u)
    {
        InterlockedAdd(gs_voxelRasterQueueCount, waveEnqueueCount, waveBaseSlot);
        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_QUEUED_PIXELS, waveEnqueueCount);
    }

    waveBaseSlot = WaveReadLaneFirst(waveBaseSlot);
    const uint queueSlot = waveBaseSlot + WavePrefixCountBits(enqueuePixel);
    if (!enqueuePixel)
    {
        return;
    }

    if (queueSlot < VOXEL_RASTER_PIXEL_QUEUE_CAPACITY)
    {
        gs_voxelRasterPixelQueue[queueSlot] = queuedPixel;
        return;
    }

    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_QUEUE_OVERFLOW, 1u);
}
#endif

uint VoxelRasterClampProjectedPixels(
    float2 screenMin,
    float2 screenMax,
    float cubeMinLinearDepth,
    in const ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
    out VoxelRasterProjectedCube projected)
{
    const float2 projectedFootprintPixels = max(screenMax - screenMin, float2(0.0f, 0.0f));
    if (max(projectedFootprintPixels.x, projectedFootprintPixels.y) > CLOD_VOXEL_RASTER_MAX_PROJECTED_FOOTPRINT_PIXELS)
    {
        return VOXEL_RASTER_PROJECT_FOOTPRINT_REJECTED;
    }

    int2 minPx = int2(floor(screenMin));
    int2 maxPx = int2(floor(screenMax));
    minPx = max(minPx, int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(targetDims.x) - 1, int(targetDims.y) - 1));
    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
        return VOXEL_RASTER_PROJECT_SCISSOR_REJECTED;
    }

    projected.minPx = minPx;
    projected.maxPx = maxPx;
    projected.minLinearDepth = cubeMinLinearDepth;
    return VOXEL_RASTER_PROJECT_OK;
}

uint VoxelRasterProjectCubeCornersToPixels(
    float3 cubeMinObject,
    float3 cubeMaxObject,
    row_major matrix localToClip,
    float4 localViewZ,
    in const ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
    out VoxelRasterProjectedCube projected)
{
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
        return VOXEL_RASTER_PROJECT_REJECTED;
    }

    if (hasBehindNearCorner)
    {
        cubeMinLinearDepth = 0.0f;
    }

    return VoxelRasterClampProjectedPixels(screenMin, screenMax, cubeMinLinearDepth, rasterInfo, targetDims, projected);
}

#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
float VoxelRasterComputeCubeSphereRadius(float halfCubeWidth, row_major matrix localToWorld)
{
    const float3 halfAxisX = mul(float4(halfCubeWidth, 0.0f, 0.0f, 0.0f), localToWorld).xyz;
    const float3 halfAxisY = mul(float4(0.0f, halfCubeWidth, 0.0f, 0.0f), localToWorld).xyz;
    const float3 halfAxisZ = mul(float4(0.0f, 0.0f, halfCubeWidth, 0.0f), localToWorld).xyz;
    return sqrt(dot(halfAxisX, halfAxisX) + dot(halfAxisY, halfAxisY) + dot(halfAxisZ, halfAxisZ));
}

uint VoxelRasterProjectCubeSphereToPixels(
    float3 cubeCenterObject,
    row_major matrix localToWorld,
    float radius,
    in const CullingCameraInfo camera,
    in const ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
    out VoxelRasterProjectedCube projected)
{
    const float3 cubeCenterWorld = mul(float4(cubeCenterObject, 1.0f), localToWorld).xyz;
    const float3 cameraToCenter = cubeCenterWorld - camera.positionWorldSpace.xyz;
    const float centerLinearDepth = dot(cameraToCenter, camera.viewForwardWorld.xyz);

    if (centerLinearDepth + radius <= 0.0f)
    {
        return VOXEL_RASTER_PROJECT_REJECTED;
    }

    const float viewX = dot(cameraToCenter, camera.viewRightWorld.xyz);
    const float viewY = dot(cameraToCenter, camera.viewUpWorld.xyz);
    float2 ndcCenter = float2(0.0f, 0.0f);
    float2 ndcRadius = float2(0.0f, 0.0f);
    if (camera.isOrtho != 0u)
    {
        ndcCenter = float2(viewX * camera.projX, viewY * camera.projY);
        ndcRadius = radius * abs(float2(camera.projX, camera.projY));
    }
    else
    {
        const float invDepth = rcp(max(centerLinearDepth, 1.0e-4f));
        const float invNearSphereDepth = rcp(max(centerLinearDepth - radius, 1.0e-4f));
        ndcCenter = float2(viewX * camera.projX, viewY * camera.projY) * invDepth;
        ndcRadius = radius * abs(float2(camera.projX, camera.projY)) * invNearSphereDepth;
    }

    const float2 viewportSize = float2(
        float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX),
        float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY));
    const float2 viewportMin = float2(float(rasterInfo.scissorMinX), float(rasterInfo.scissorMinY));
    const float2 screenCenter = float2(
        (ndcCenter.x + 1.0f) * 0.5f * viewportSize.x + viewportMin.x,
        (1.0f - ndcCenter.y) * 0.5f * viewportSize.y + viewportMin.y);
    const float2 screenRadius = ndcRadius * (0.5f * viewportSize);
    const float cubeMinLinearDepth = max(centerLinearDepth - radius, 0.0f);

    return VoxelRasterClampProjectedPixels(
        screenCenter - screenRadius,
        screenCenter + screenRadius,
        cubeMinLinearDepth,
        rasterInfo,
        targetDims,
        projected);
}
#endif

uint VoxelRasterProjectCubeToPixels(
#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
    float3 cubeCenterObject,
    row_major matrix localToWorld,
    float sphereRadius,
    in const CullingCameraInfo camera,
#else
    float3 cubeMinObject,
    float3 cubeMaxObject,
    row_major matrix localToClip,
    float4 localViewZ,
#endif
    in const ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
    out VoxelRasterProjectedCube projected)
{
#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
    return VoxelRasterProjectCubeSphereToPixels(
        cubeCenterObject,
        localToWorld,
        sphereRadius,
        camera,
        rasterInfo,
        targetDims,
        projected);
#else
    return VoxelRasterProjectCubeCornersToPixels(
        cubeMinObject,
        cubeMaxObject,
        localToClip,
        localViewZ,
        rasterInfo,
        targetDims,
        projected);
#endif
}

uint VoxelRasterPrepareCube(
    uint cubeOffset,
    in const GroupPageMapEntry pageEntry,
    in const CLodVoxelPageHeader pageHeader,
    in const CLodVoxelClusterRecord voxelCluster,
    in const VoxelRasterVoxelSetup voxelSetup,
#if PSO_SKINNED
    in const PerMeshInstanceBuffer meshInstance,
    in const PerObjectBuffer objectData,
#endif
    in const CullingCameraInfo camera,
    in const ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
#if !PSO_SKINNED
    in const VoxelRasterRigidSetup rigidSetup,
#endif
    out VoxelRasterPreparedCube preparedCube)
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

    preparedCube.occupancyMask = occupancyMask;
    if ((occupancyMask.x | occupancyMask.y) == 0u)
    {
        return VOXEL_RASTER_PROJECT_REJECTED;
    }

    const uint3 cubeCoord = CLodVoxelDecodeCubeCoord(cubeCoordPacked);
    const float3 cubeMinObject = voxelSetup.aabbMin + float3(cubeCoord) * voxelSetup.cubeObjectWidth;

#if PSO_SKINNED
    row_major matrix localToWorld = objectData.model;
    row_major matrix worldToLocal = objectData.modelInverse;
    if (dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
    {
        const float4x4 skinMatrix = LoadBoneSkinMatrix(meshInstance.skinningInstanceSlot, dominantBoneIndex);
        const float4x4 inverseSkinMatrix = LoadBoneInverseSkinMatrix(meshInstance.skinningInstanceSlot, dominantBoneIndex);
        localToWorld = mul(skinMatrix, objectData.model);
        worldToLocal = mul(objectData.modelInverse, inverseSkinMatrix);
    }
#else
    const row_major matrix localToWorld = rigidSetup.localToWorld;
#endif

    VoxelRasterProjectedCube projected;
#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
#if PSO_SKINNED
    const float halfCubeWidth = voxelSetup.cubeObjectWidth * 0.5f;
    const float sphereRadius = VoxelRasterComputeCubeSphereRadius(halfCubeWidth, localToWorld);
#else
    const float sphereRadius = rigidSetup.sphereRadius;
#endif
    const float3 cubeCenterObject = cubeMinObject + voxelSetup.cubeObjectWidth * 0.5f;
    const uint projectResult = VoxelRasterProjectCubeToPixels(
        cubeCenterObject,
        localToWorld,
        sphereRadius,
        camera,
        rasterInfo,
        targetDims,
        projected);
#else
    const float3 cubeMaxObject = cubeMinObject + voxelSetup.cubeObjectWidth;
    const row_major matrix localToClip = mul(localToWorld, camera.viewProjection);
#if PSO_SKINNED
    const float4 localViewZ = mul(localToWorld, camera.viewZ);
#else
    const float4 localViewZ = rigidSetup.localViewZ;
#endif
    const uint projectResult = VoxelRasterProjectCubeToPixels(
        cubeMinObject,
        cubeMaxObject,
        localToClip,
        localViewZ,
        rasterInfo,
        targetDims,
        projected);
#endif
    if (projectResult != VOXEL_RASTER_PROJECT_OK)
    {
        return projectResult;
    }

#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
#if PSO_SKINNED
    const float4 localViewZ = mul(localToWorld, camera.viewZ);
#else
    const float4 localViewZ = rigidSetup.localViewZ;
#endif
#endif
#if PSO_SKINNED
    const float3 cameraOriginLocal = mul(float4(camera.positionWorldSpace.xyz, 1.0f), worldToLocal).xyz;
    const row_major matrix viewToLocal = mul(camera.viewInverse, worldToLocal);
    const float rayOriginViewZ = dot(float4(cameraOriginLocal, 1.0f), localViewZ);
#else
    const float3 cameraOriginLocal = rigidSetup.cameraOriginLocal;
    const row_major matrix viewToLocal = rigidSetup.viewToLocal;
    const float rayOriginViewZ = rigidSetup.rayOriginViewZ;
#endif
    const uint pixelWidth = uint(projected.maxPx.x - projected.minPx.x + 1);
    const uint pixelHeight = uint(projected.maxPx.y - projected.minPx.y + 1);

    preparedCube.pixelWidth = pixelWidth;
    preparedCube.pixelCount = pixelWidth * pixelHeight;
    preparedCube.invVoxelWidth = voxelSetup.invVoxelWidth;
    preparedCube.rayOriginViewZ = rayOriginViewZ;
    preparedCube.cameraOriginLocal = cameraOriginLocal;
    preparedCube.rayOriginCube = (cameraOriginLocal - cubeMinObject) * voxelSetup.invVoxelWidth;
    preparedCube.localViewZ = localViewZ;
    preparedCube.viewToLocal = viewToLocal;
    preparedCube.projected = projected;
    return VOXEL_RASTER_PROJECT_OK;
}

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
bool VoxelRasterPassesDepthPretest(uint2 pixel, float cubeMinLinearDepth, RWTexture2D<uint64_t> visibilityBuffer)
{
    const uint64_t currentVisKey = visibilityBuffer[pixel];
    return currentVisKey == VOXEL_RASTER_VISIBILITY_EMPTY ||
        UnpackVisKeyDepth(currentVisKey) > cubeMinLinearDepth;
}
#endif

uint VoxelRasterTracePixel(
    uint2 pixel,
    float2 targetDimsInv,
    row_major matrix projectionInverse,
    row_major matrix viewToLocal,
    float3 cameraOriginLocal,
    float3 rayOriginCube,
    float invVoxelWidth,
    uint2 occupancyMask,
    float rayOriginViewZ,
    float4 localViewZ,
    out float linearDepth)
{
    const float2 uv = (float2(pixel) + 0.5f) * targetDimsInv;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 viewNear = mul(float4(ndc, 0.0f, 1.0f), projectionInverse);
    viewNear.xyz /= max(viewNear.w, 1.0e-6f);
    const float3 localPoint = mul(float4(viewNear.xyz, 1.0f), viewToLocal).xyz;
    const float3 rayDirObject = normalize(localPoint - cameraOriginLocal);
    const float3 rayDirCube = rayDirObject * invVoxelWidth;

    float tHitCube = 0.0f;
    if (!RaycastVoxelCubeDDA(rayOriginCube, rayDirCube, occupancyMask, tHitCube))
    {
        linearDepth = 0.0f;
        return VOXEL_RASTER_TRACE_DDA_MISS;
    }

    linearDepth = -(rayOriginViewZ + tHitCube * dot(rayDirObject, localViewZ.xyz));
    return linearDepth > 0.0f ? VOXEL_RASTER_TRACE_HIT : VOXEL_RASTER_TRACE_NON_POSITIVE_DEPTH;
}

#if CLOD_VOXEL_RASTER_DEBUG_VIS
void VoxelRasterWriteDebug(uint2 pixel, bool debugMode, RWTexture2D<uint2> debugVisTex)
{
    if (debugMode)
    {
        WriteDebugPixel(debugVisTex, pixel, PackDebugUint(2u));
    }
}
#endif

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
void VoxelRasterWriteHit(
    uint2 pixel,
    float linearDepth,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable,
    RWTexture2D<uint> physicalPages)
{
    VoxelRasterWriteVirtualShadow(pixel, linearDepth, clipmapInfo, pageTable, physicalPages);
}
#else
void VoxelRasterWriteHit(
    uint2 pixel,
    float linearDepth,
    uint visibleClusterIndex,
    uint cubeOffset,
    RWTexture2D<uint64_t> visibilityBuffer)
{
    const uint64_t visKey = PackVisKey(linearDepth, visibleClusterIndex, cubeOffset);
    uint64_t previousVisKey = 0u;
    InterlockedMin(visibilityBuffer[pixel], visKey, previousVisKey);
    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_VISIBILITY_WRITES, 1u);
    VoxelRasterTelemetryAdd(
        previousVisKey > visKey
            ? WG_COUNTER_VOXEL_RASTER_VISIBILITY_WINS
            : WG_COUNTER_VOXEL_RASTER_VISIBILITY_LOSSES,
        1u);
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
    if (GI == 0u)
    {
        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_WORK_GROUPS, 1u);
    }

    StructuredBuffer<CLodMeshMetadata> metadataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];

    const uint instanceIndex = work.instanceIndex;
    const uint viewId = work.viewId;
    const uint localGroupId = work.localGroupId;
    const uint localPageIndex = work.localPageIndex;
    const uint pageLocalClusterIndex = work.pageLocalClusterIndex;

#if PSO_SKINNED
    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(instanceIndex);
#endif
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(instanceIndex);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceIndex);
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    const CullingCameraInfo camera = cameras[viewId];
    const ClodViewRasterInfo rasterInfo = viewRasterInfoBuffer[viewId];

    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup group = groups[metadata.groupsBase + localGroupId];
    if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
        localPageIndex < group.pageMapBase ||
        localPageIndex >= group.pageMapBase + group.pageCount)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SEGMENT_PAGE_MISSES, 1u);
        }
        return;
    }

    GroupPageMapEntry pageEntry = CLodLoadVoxelPageMapEntry(metadata, group, localPageIndex);
    CLodVoxelPageHeader pageHeader = CLodLoadVoxelPageHeader(pageEntry.slabDescriptorIndex, pageEntry.slabByteOffset);
    if (pageHeader.magic != CLOD_VOXEL_PAGE_MAGIC ||
        pageHeader.version != CLOD_VOXEL_PAGE_VERSION ||
        pageLocalClusterIndex >= pageHeader.clusterCount)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SEGMENT_PAGE_MISSES, 1u);
        }
        return;
    }

    const CLodVoxelClusterRecord voxelCluster = CLodLoadVoxelClusterFromPage(
        pageEntry.slabDescriptorIndex,
        pageEntry.slabByteOffset,
        pageHeader.clusterRecordsOffset,
        pageLocalClusterIndex);
    if (voxelCluster.cubeCount == 0u || voxelCluster.cubeCount > CLOD_VOXEL_MAX_CUBES_PER_CLUSTER)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_CLUSTER, 1u);
        }
        return;
    }

    const float voxelWidth = voxelCluster.aabbMinAndVoxelWidth.w;
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

#if CLOD_VOXEL_RASTER_DEBUG_VIS
    RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool debugMode = perFrameBuffer.outputType == OUTPUT_SW_RASTER;
#endif
    const float2 targetDimsInv = rcp(max(float2(targetDims), float2(1.0f, 1.0f)));

    VoxelRasterVoxelSetup voxelSetup;
    voxelSetup.aabbMin = voxelCluster.aabbMinAndVoxelWidth.xyz;
    voxelSetup.cubeObjectWidth = voxelWidth * 4.0f;
    voxelSetup.invVoxelWidth = rcp(voxelWidth);

#if !PSO_SKINNED
    VoxelRasterRigidSetup rigidSetup;
    rigidSetup.sphereRadius = 0.0f;
    rigidSetup.localToWorld = objectData.model;
    rigidSetup.localViewZ = mul(objectData.model, camera.viewZ);
    rigidSetup.viewToLocal = mul(camera.viewInverse, objectData.modelInverse);
    rigidSetup.cameraOriginLocal = mul(float4(camera.positionWorldSpace.xyz, 1.0f), objectData.modelInverse).xyz;
    rigidSetup.rayOriginViewZ = dot(float4(rigidSetup.cameraOriginLocal, 1.0f), rigidSetup.localViewZ);
#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT && !PSO_SKINNED
    rigidSetup.sphereRadius = VoxelRasterComputeCubeSphereRadius(voxelWidth * 2.0f, objectData.model);
#endif
#endif

    const uint workFirstCubeOffset = min(work.firstCubeOffset, voxelCluster.cubeCount);
    const uint workCubeCount = min(work.cubeCount, voxelCluster.cubeCount - workFirstCubeOffset);
    const uint workCubeEnd = workFirstCubeOffset + workCubeCount;

#if !CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
    for (uint cubeOffset = workFirstCubeOffset; cubeOffset < workCubeEnd; ++cubeOffset)
    {
        VoxelRasterPreparedCube preparedCube = (VoxelRasterPreparedCube)0;
        const uint projectResult = VoxelRasterPrepareCube(
            cubeOffset,
            pageEntry,
            pageHeader,
            voxelCluster,
            voxelSetup,
#if PSO_SKINNED
            meshInstance,
            objectData,
#endif
            camera,
            rasterInfo,
            targetDims,
#if !PSO_SKINNED
            rigidSetup,
#endif
            preparedCube);

        const uint2 occupancyMask = preparedCube.occupancyMask;
        if ((occupancyMask.x | occupancyMask.y) == 0u)
        {
            continue;
        }

        if (projectResult != VOXEL_RASTER_PROJECT_OK)
        {
            if (GI == 0u)
            {
                VoxelRasterTelemetryAdd(
                    projectResult == VOXEL_RASTER_PROJECT_SCISSOR_REJECTED
                        ? WG_COUNTER_VOXEL_RASTER_SCISSOR_REJECTED
                        : WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED,
                    1u);
            }
            continue;
        }

        const uint pixelCount = preparedCube.pixelCount;
        const uint pixelWidth = preparedCube.pixelWidth;
        const int2 minPx = preparedCube.projected.minPx;
        const float minLinearDepth = preparedCube.projected.minLinearDepth;
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTED_PIXELS, pixelCount);
        }

        const row_major matrix viewToLocal = preparedCube.viewToLocal;
        const float3 cameraOriginLocal = preparedCube.cameraOriginLocal;
        const float3 rayOriginCube = preparedCube.rayOriginCube;
        const float invVoxelWidth = preparedCube.invVoxelWidth;
        const float rayOriginViewZ = preparedCube.rayOriginViewZ;
        const float4 localViewZ = preparedCube.localViewZ;
        for (uint pixelLinear = GI; pixelLinear < pixelCount; pixelLinear += VOXEL_RASTER_THREADS_PER_GROUP)
        {
            const uint2 pixel = VoxelRasterPixelFromLinear(pixelLinear, minPx, pixelWidth);

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
            if (!VoxelRasterPassesDepthPretest(pixel, minLinearDepth, visibilityBuffer))
            {
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED, 1u);
                continue;
            }
#endif

            float linearDepth = 0.0f;
            const uint traceResult = VoxelRasterTracePixel(
                    pixel,
                    targetDimsInv,
                    camera.projectionInverse,
                    viewToLocal,
                    cameraOriginLocal,
                    rayOriginCube,
                    invVoxelWidth,
                    occupancyMask,
                    rayOriginViewZ,
                    localViewZ,
                    linearDepth);
            if (traceResult != VOXEL_RASTER_TRACE_HIT)
            {
                VoxelRasterTelemetryAdd(
                    traceResult == VOXEL_RASTER_TRACE_DDA_MISS
                        ? WG_COUNTER_VOXEL_RASTER_DDA_MISSES
                        : WG_COUNTER_VOXEL_RASTER_NON_POSITIVE_DEPTH,
                    1u);
                continue;
            }

#if CLOD_VOXEL_RASTER_DEBUG_VIS
            VoxelRasterWriteDebug(pixel, debugMode, debugVisTex);
#endif

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
            VoxelRasterWriteHit(pixel, linearDepth, clipmapInfo, pageTable, physicalPages);
#else
            VoxelRasterWriteHit(pixel, linearDepth, work.visibleClusterIndex, cubeOffset, visibilityBuffer);
#endif
        }
    }
#else
    for (uint batchFirstCubeOffset = workFirstCubeOffset; batchFirstCubeOffset < workCubeEnd; batchFirstCubeOffset += CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE)
    {
        const uint batchCubeCount = min(CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE, workCubeEnd - batchFirstCubeOffset);
        if (GI == 0u)
        {
            gs_voxelRasterBatchMaxPixelCount = 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        if (GI < batchCubeCount)
        {
            const uint cubeOffset = batchFirstCubeOffset + GI;
            VoxelRasterPreparedCube preparedCube = (VoxelRasterPreparedCube)0;
            const uint projectResult = VoxelRasterPrepareCube(
                cubeOffset,
                pageEntry,
                pageHeader,
                voxelCluster,
                voxelSetup,
#if PSO_SKINNED
                meshInstance,
                objectData,
#endif
                camera,
                rasterInfo,
                targetDims,
#if !PSO_SKINNED
                rigidSetup,
#endif
                preparedCube);

            const uint2 occupancyMask = preparedCube.occupancyMask;
            if ((occupancyMask.x | occupancyMask.y) == 0u)
            {
                preparedCube.pixelCount = 0u;
            }
            else if (projectResult != VOXEL_RASTER_PROJECT_OK)
            {
                VoxelRasterTelemetryAdd(
                    projectResult == VOXEL_RASTER_PROJECT_SCISSOR_REJECTED
                        ? WG_COUNTER_VOXEL_RASTER_SCISSOR_REJECTED
                        : WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED,
                    1u);
                preparedCube.pixelCount = 0u;
            }
            else
            {
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTED_PIXELS, preparedCube.pixelCount);
                InterlockedMax(gs_voxelRasterBatchMaxPixelCount, preparedCube.pixelCount);
            }

            gs_voxelRasterPreparedCubeOffsets[GI] = cubeOffset;
            gs_voxelRasterPreparedCubes[GI] = preparedCube;
        }
        GroupMemoryBarrierWithGroupSync();

        const uint batchMaxPixelCount = gs_voxelRasterBatchMaxPixelCount;
        const uint batchTaskCount = batchCubeCount * batchMaxPixelCount;
        for (uint taskBase = 0u; taskBase < batchTaskCount; taskBase += VOXEL_RASTER_PIXEL_QUEUE_CAPACITY)
        {
            if (GI == 0u)
            {
                VoxelRasterQueueReset();
            }
            GroupMemoryBarrierWithGroupSync();

            const uint taskEnd = min(taskBase + VOXEL_RASTER_PIXEL_QUEUE_CAPACITY, batchTaskCount);
            for (uint taskLinear = taskBase + GI; taskLinear < taskEnd; taskLinear += VOXEL_RASTER_THREADS_PER_GROUP)
            {
                const uint cubeSlot = taskLinear / batchMaxPixelCount;
                const uint pixelLinear = taskLinear - cubeSlot * batchMaxPixelCount;
                const VoxelRasterPreparedCube preparedCube = gs_voxelRasterPreparedCubes[cubeSlot];
                bool enqueuePixel = pixelLinear < preparedCube.pixelCount;
                if (enqueuePixel)
                {
                    const uint2 pixel = VoxelRasterPixelFromLinear(pixelLinear, preparedCube.projected.minPx, preparedCube.pixelWidth);

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
                    enqueuePixel = VoxelRasterPassesDepthPretest(pixel, preparedCube.projected.minLinearDepth, visibilityBuffer);
                    if (!enqueuePixel)
                    {
                        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED, 1u);
                    }
#endif
                }

                VoxelRasterQueuePush(enqueuePixel, VoxelRasterPackQueuedPixel(cubeSlot, pixelLinear));
            }

            GroupMemoryBarrierWithGroupSync();

            const uint queuedPixelCount = min(gs_voxelRasterQueueCount, VOXEL_RASTER_PIXEL_QUEUE_CAPACITY);
            for (uint queueSlot = GI; queueSlot < queuedPixelCount; queueSlot += VOXEL_RASTER_THREADS_PER_GROUP)
            {
                const uint queuedPixel = gs_voxelRasterPixelQueue[queueSlot];
                const uint cubeSlot = VoxelRasterQueuedPixelCubeSlot(queuedPixel);
                const uint pixelLinear = VoxelRasterQueuedPixelLinear(queuedPixel);
                const VoxelRasterPreparedCube preparedCube = gs_voxelRasterPreparedCubes[cubeSlot];
                const uint2 pixel = VoxelRasterPixelFromLinear(pixelLinear, preparedCube.projected.minPx, preparedCube.pixelWidth);
                float linearDepth = 0.0f;
                const uint traceResult = VoxelRasterTracePixel(
                    pixel,
                    targetDimsInv,
                    camera.projectionInverse,
                    preparedCube.viewToLocal,
                    preparedCube.cameraOriginLocal,
                    preparedCube.rayOriginCube,
                    preparedCube.invVoxelWidth,
                    preparedCube.occupancyMask,
                    preparedCube.rayOriginViewZ,
                    preparedCube.localViewZ,
                    linearDepth);
                if (traceResult != VOXEL_RASTER_TRACE_HIT)
                {
                    VoxelRasterTelemetryAdd(
                        traceResult == VOXEL_RASTER_TRACE_DDA_MISS
                            ? WG_COUNTER_VOXEL_RASTER_DDA_MISSES
                            : WG_COUNTER_VOXEL_RASTER_NON_POSITIVE_DEPTH,
                        1u);
                    continue;
                }

#if CLOD_VOXEL_RASTER_DEBUG_VIS
                VoxelRasterWriteDebug(pixel, debugMode, debugVisTex);
#endif

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                VoxelRasterWriteHit(pixel, linearDepth, clipmapInfo, pageTable, physicalPages);
#else
                const uint cubeOffset = gs_voxelRasterPreparedCubeOffsets[cubeSlot];
                VoxelRasterWriteHit(pixel, linearDepth, work.visibleClusterIndex, cubeOffset, visibilityBuffer);
#endif
            }

            GroupMemoryBarrierWithGroupSync();
        }
    }
#endif
}
