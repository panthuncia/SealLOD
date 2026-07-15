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
#define CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT 0
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
static const uint VOXEL_RASTER_WAVE_SIZE = 32u;
static const uint VOXEL_RASTER_WAVES_PER_GROUP = (CLOD_VOXEL_RASTER_THREADS_PER_GROUP + 31u) / 32u;
static const uint VOXEL_RASTER_PIXEL_QUEUE_CAPACITY = CLOD_VOXEL_RASTER_PIXEL_QUEUE_CAPACITY;
static const uint VOXEL_RASTER_WAVE_PIXEL_QUEUE_CAPACITY = CLOD_VOXEL_RASTER_PIXEL_QUEUE_CAPACITY / VOXEL_RASTER_WAVES_PER_GROUP;
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
static const uint VOXEL_RASTER_PROJECT_NO_VALID_CLIP = 4u;
static const uint VOXEL_RASTER_PROJECT_NON_POSITIVE_DEPTH = 5u;

#ifndef CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
#define CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST 1
#endif

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
groupshared uint gs_voxelRasterPixelQueue[VOXEL_RASTER_PIXEL_QUEUE_CAPACITY];
groupshared uint gs_voxelRasterBatchMaxPixelCount;
#endif

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW || !CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
#define VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_PARAM
#define VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_ARG
#else
#define VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_PARAM , RWTexture2D<uint64_t> visibilityBuffer
#define VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_ARG , visibilityBuffer
#endif

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#define VOXEL_RASTER_OUTPUT_TARGET_PARAMS , CLodVirtualShadowClipmapInfo clipmapInfo, RWTexture2DArray<uint> pageTable, RWTexture2D<uint> physicalPages
#define VOXEL_RASTER_OUTPUT_TARGET_ARGS , clipmapInfo, pageTable, physicalPages
#else
#define VOXEL_RASTER_OUTPUT_TARGET_PARAMS , RWTexture2D<uint64_t> visibilityBuffer
#define VOXEL_RASTER_OUTPUT_TARGET_ARGS , visibilityBuffer
#endif

bool VoxelMaskTest(uint2 mask, uint bitIndex)
{
    return bitIndex < 32u ? ((mask.x & (1u << bitIndex)) != 0u) : ((mask.y & (1u << (bitIndex - 32u))) != 0u);
}

uint3 VoxelRasterDecodeActiveBoundsMin(uint activeBounds)
{
    return uint3(
        activeBounds & 0x3u,
        (activeBounds >> 2u) & 0x3u,
        (activeBounds >> 4u) & 0x3u);
}

uint3 VoxelRasterDecodeActiveBoundsMax(uint activeBounds)
{
    return uint3(
        (activeBounds >> 6u) & 0x3u,
        (activeBounds >> 8u) & 0x3u,
        (activeBounds >> 10u) & 0x3u);
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
    const float rayDirAbsX = abs(rayDir.x);
    const float rayDirAbsY = abs(rayDir.y);
    const float rayDirAbsZ = abs(rayDir.z);
    if ((rayDirAbsX <= 1.0e-8f && (rayOrigin.x < 0.0f || rayOrigin.x > 4.0f)) ||
        (rayDirAbsY <= 1.0e-8f && (rayOrigin.y < 0.0f || rayOrigin.y > 4.0f)) ||
        (rayDirAbsZ <= 1.0e-8f && (rayOrigin.z < 0.0f || rayOrigin.z > 4.0f)))
    {
        return false;
    }

    const float invRayDirX = rayDirAbsX > 1.0e-8f ? rcp(rayDir.x) : largeT;
    const float invRayDirY = rayDirAbsY > 1.0e-8f ? rcp(rayDir.y) : largeT;
    const float invRayDirZ = rayDirAbsZ > 1.0e-8f ? rcp(rayDir.z) : largeT;

    float tEnter = 0.0f;
    float tExit = largeT;
    {
        const float tx0 = -rayOrigin.x * invRayDirX;
        const float tx1 = (4.0f - rayOrigin.x) * invRayDirX;
        tEnter = max(tEnter, min(tx0, tx1));
        tExit = min(tExit, max(tx0, tx1));
    }
    {
        const float ty0 = -rayOrigin.y * invRayDirY;
        const float ty1 = (4.0f - rayOrigin.y) * invRayDirY;
        tEnter = max(tEnter, min(ty0, ty1));
        tExit = min(tExit, max(ty0, ty1));
    }
    {
        const float tz0 = -rayOrigin.z * invRayDirZ;
        const float tz1 = (4.0f - rayOrigin.z) * invRayDirZ;
        tEnter = max(tEnter, min(tz0, tz1));
        tExit = min(tExit, max(tz0, tz1));
    }
    if (tExit < tEnter)
    {
        return false;
    }

    if ((occupancyMask.x & occupancyMask.y) == 0xFFFFFFFFu)
    {
        tHit = tEnter;
        return true;
    }

    tHit = tEnter + 1e-4f;
    int cellX = int(clamp(rayOrigin.x + rayDir.x * tHit, 0.0f, 3.9999f));
    int cellY = int(clamp(rayOrigin.y + rayDir.y * tHit, 0.0f, 3.9999f));
    int cellZ = int(clamp(rayOrigin.z + rayDir.z * tHit, 0.0f, 3.9999f));
    const int stepDirX = rayDir.x >= 0.0f ? 1 : -1;
    const int stepDirY = rayDir.y >= 0.0f ? 1 : -1;
    const int stepDirZ = rayDir.z >= 0.0f ? 1 : -1;

    float tMaxX = rayDirAbsX > 1.0e-8f ? ((stepDirX > 0 ? float(cellX + 1) : float(cellX)) - rayOrigin.x) * invRayDirX : largeT;
    float tMaxY = rayDirAbsY > 1.0e-8f ? ((stepDirY > 0 ? float(cellY + 1) : float(cellY)) - rayOrigin.y) * invRayDirY : largeT;
    float tMaxZ = rayDirAbsZ > 1.0e-8f ? ((stepDirZ > 0 ? float(cellZ + 1) : float(cellZ)) - rayOrigin.z) * invRayDirZ : largeT;
    const float tDeltaX = abs(invRayDirX);
    const float tDeltaY = abs(invRayDirY);
    const float tDeltaZ = abs(invRayDirZ);

    [loop]
    for (uint iter = 0u; iter < CLOD_MAX_DDA_STEPS; ++iter)
    {
        if ((uint)cellX >= 4u || (uint)cellY >= 4u || (uint)cellZ >= 4u)
        {
            break;
        }

        const uint cellIndex = (uint)cellX | ((uint)cellY << 2u) | ((uint)cellZ << 4u);
        if (VoxelMaskTest(occupancyMask, cellIndex))
        {
            return true;
        }

        if (tMaxX <= tMaxY && tMaxX <= tMaxZ)
        {
            if (tMaxX > tExit) break;
            tHit = tMaxX + 1e-4f;
            cellX += stepDirX;
            tMaxX += tDeltaX;
        }
        else if (tMaxY <= tMaxZ)
        {
            if (tMaxY > tExit) break;
            tHit = tMaxY + 1e-4f;
            cellY += stepDirY;
            tMaxY += tDeltaY;
        }
        else
        {
            if (tMaxZ > tExit) break;
            tHit = tMaxZ + 1e-4f;
            cellZ += stepDirZ;
            tMaxZ += tDeltaZ;
        }
    }

    tHit = 0.0f;
    return false;
}

void VoxelRasterLoadCubeHeader(
    uint slabDescriptorIndex,
    uint pageByteOffset,
    uint cubeRecordsOffset,
    uint pageLocalCubeIndex,
    out uint cubeCoord,
    out uint dominantBoneIndex,
    out uint2 occupancyMask,
    out uint activeBounds)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    const uint addr = pageByteOffset + cubeRecordsOffset + pageLocalCubeIndex * CLOD_VOXEL_CUBE_RECORD_STRIDE;
    const uint4 d0 = slab.Load4(addr + 0u);
    cubeCoord = d0.x;
    dominantBoneIndex = d0.y;
    occupancyMask = d0.zw;
    activeBounds = slab.Load(addr + 28u);
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

#if PSO_SKINNED
struct VoxelRasterSkinnedTraceBasis
{
    float4 viewToLocalX;
    float4 viewToLocalY;
    float4 viewToLocalZ;
    float4 localViewZAndRayOriginViewZ;
};
#endif

struct VoxelRasterPreparedCube
{
    uint2 occupancyMask;
    uint packedMinPx;
    uint packedPixelDims;
    uint minDepthBits;
    uint cubeCoordPacked;
#if PSO_SKINNED
    VoxelRasterSkinnedTraceBasis traceBasis;
#endif
};

uint VoxelRasterPackMinPx(int2 minPx)
{
    return (uint(minPx.x) & 0xFFFFu) | ((uint(minPx.y) & 0xFFFFu) << 16u);
}

int2 VoxelRasterUnpackMinPx(uint packedMinPx)
{
    return int2(int(packedMinPx & 0xFFFFu), int((packedMinPx >> 16u) & 0xFFFFu));
}

uint VoxelRasterPackPixelDims(uint pixelWidth, uint pixelCount)
{
    return (pixelWidth & 0xFFFFu) | ((pixelCount & 0xFFFFu) << 16u);
}

uint VoxelRasterUnpackPixelWidth(uint packedPixelDims)
{
    return packedPixelDims & 0xFFFFu;
}

uint VoxelRasterUnpackPixelCount(uint packedPixelDims)
{
    return (packedPixelDims >> 16u) & 0xFFFFu;
}

struct VoxelRasterVoxelSetup
{
    float3 aabbMin;
    float cubeObjectWidth;
    float invVoxelWidth;
};

struct VoxelRasterRayProjectionSetup
{
    float4 ndcX;
    float4 ndcY;
    float4 ndcZ;
    float4 origin;
};

float3 VoxelRasterRayOriginCubeFromCoord(
    uint cubeCoordPacked,
    float3 cameraOriginLocal,
    VoxelRasterVoxelSetup voxelSetup)
{
    const uint3 cubeCoord = CLodVoxelDecodeCubeCoord(cubeCoordPacked);
    const float3 cubeMinObject = voxelSetup.aabbMin + float3(cubeCoord) * voxelSetup.cubeObjectWidth;
    return (cameraOriginLocal - cubeMinObject) * voxelSetup.invVoxelWidth;
}

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
groupshared VoxelRasterPreparedCube gs_voxelRasterPreparedCubes[CLOD_VOXEL_RASTER_CUBE_BATCH_SIZE];
#endif

#if !PSO_SKINNED
struct VoxelRasterRigidSetup
{
    float sphereRadius;
    float rayOriginViewZ;
    float3 cameraOriginLocal;
    float4 localViewZ;
    row_major matrix localToWorld;
    float4 viewToLocalX;
    float4 viewToLocalY;
    float4 viewToLocalZ;
};
#endif

uint2 VoxelRasterPixelFromLinear(uint pixelLinear, int2 minPx, uint pixelWidth)
{
    return uint2(
        uint(minPx.x + int(pixelLinear % pixelWidth)),
        uint(minPx.y + int(pixelLinear / pixelWidth)));
}

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
uint VoxelRasterPackQueuedPixelCoords(uint cubeSlot, uint2 localPixel)
{
    return ((cubeSlot & 0xFFu) << 24u) | ((localPixel.y & 0xFFFu) << 12u) | (localPixel.x & 0xFFFu);
}

uint VoxelRasterQueuedPixelCubeSlot(uint queuedPixel)
{
    return (queuedPixel >> 24u) & 0xFFu;
}

uint2 VoxelRasterQueuedPixelLocalCoords(uint queuedPixel)
{
    return uint2(queuedPixel & 0xFFFu, (queuedPixel >> 12u) & 0xFFFu);
}

void VoxelRasterQueuePushPixelCoords(
    uint queueBase,
    inout uint queueCount,
    bool enqueuePixel,
    uint cubeSlotForPixel,
    uint2 localPixel)
{
    const uint waveEnqueueCount = WaveActiveCountBits(enqueuePixel);
    if (waveEnqueueCount == 0u)
    {
        return;
    }

    uint waveBaseSlot = 0u;
    if (WaveIsFirstLane())
    {
        waveBaseSlot = queueCount;
        queueCount += waveEnqueueCount;
        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_QUEUED_PIXELS, waveEnqueueCount);
    }

    waveBaseSlot = WaveReadLaneFirst(waveBaseSlot);
    queueCount = WaveReadLaneFirst(queueCount);
    if (waveEnqueueCount == WaveGetLaneCount())
    {
        gs_voxelRasterPixelQueue[queueBase + waveBaseSlot + WaveGetLaneIndex()] = VoxelRasterPackQueuedPixelCoords(cubeSlotForPixel, localPixel);
        return;
    }

    const uint queueSlot = queueBase + waveBaseSlot + WavePrefixCountBits(enqueuePixel);
    if (!enqueuePixel)
    {
        return;
    }

    gs_voxelRasterPixelQueue[queueSlot] = VoxelRasterPackQueuedPixelCoords(cubeSlotForPixel, localPixel);
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

    if (!validProjection)
    {
        return VOXEL_RASTER_PROJECT_NO_VALID_CLIP;
    }

    if (cubeMaxLinearDepth <= 0.0f)
    {
        return VOXEL_RASTER_PROJECT_NON_POSITIVE_DEPTH;
    }

    if (hasBehindNearCorner)
    {
        cubeMinLinearDepth = 0.0f;
    }

    return VoxelRasterClampProjectedPixels(screenMin, screenMax, cubeMinLinearDepth, rasterInfo, targetDims, projected);
}

#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
float VoxelRasterComputeBoxSphereRadius(float3 halfExtents, row_major matrix localToWorld)
{
    const float3 halfAxisX = mul(float4(halfExtents.x, 0.0f, 0.0f, 0.0f), localToWorld).xyz;
    const float3 halfAxisY = mul(float4(0.0f, halfExtents.y, 0.0f, 0.0f), localToWorld).xyz;
    const float3 halfAxisZ = mul(float4(0.0f, 0.0f, halfExtents.z, 0.0f), localToWorld).xyz;
    return sqrt(dot(halfAxisX, halfAxisX) + dot(halfAxisY, halfAxisY) + dot(halfAxisZ, halfAxisZ));
}

float VoxelRasterComputeCubeSphereRadius(float halfCubeWidth, row_major matrix localToWorld)
{
    return VoxelRasterComputeBoxSphereRadius(float3(halfCubeWidth, halfCubeWidth, halfCubeWidth), localToWorld);
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

float4 VoxelRasterViewToLocalX(row_major matrix viewToLocal)
{
    return float4(viewToLocal._m00, viewToLocal._m10, viewToLocal._m20, viewToLocal._m30);
}

float4 VoxelRasterViewToLocalY(row_major matrix viewToLocal)
{
    return float4(viewToLocal._m01, viewToLocal._m11, viewToLocal._m21, viewToLocal._m31);
}

float4 VoxelRasterViewToLocalZ(row_major matrix viewToLocal)
{
    return float4(viewToLocal._m02, viewToLocal._m12, viewToLocal._m22, viewToLocal._m32);
}

float3 VoxelRasterTransformViewVectorToLocal(float3 viewVector, float4 viewToLocalX, float4 viewToLocalY, float4 viewToLocalZ)
{
    return float3(dot(viewVector, viewToLocalX.xyz), dot(viewVector, viewToLocalY.xyz), dot(viewVector, viewToLocalZ.xyz));
}

float3 VoxelRasterViewToLocalOrigin(float4 viewToLocalX, float4 viewToLocalY, float4 viewToLocalZ)
{
    return float3(viewToLocalX.w, viewToLocalY.w, viewToLocalZ.w);
}

VoxelRasterRayProjectionSetup VoxelRasterBuildRayProjectionSetup(row_major matrix projectionInverse)
{
    VoxelRasterRayProjectionSetup projectionSetup;
    projectionSetup.ndcX = float4(
        projectionInverse._m00,
        projectionInverse._m01,
        projectionInverse._m02,
        projectionInverse._m03);
    projectionSetup.ndcY = float4(
        projectionInverse._m10,
        projectionInverse._m11,
        projectionInverse._m12,
        projectionInverse._m13);
    projectionSetup.ndcZ = float4(
        projectionInverse._m20,
        projectionInverse._m21,
        projectionInverse._m22,
        projectionInverse._m23);
    projectionSetup.origin = float4(
        projectionInverse._m30,
        projectionInverse._m31,
        projectionInverse._m32,
        projectionInverse._m33);
    return projectionSetup;
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
    in const CLodMeshMetadata metadata,
    uint assemblyTransformIndex,
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
    uint activeBounds = 0u;
    VoxelRasterLoadCubeHeader(
        pageEntry.slabDescriptorIndex,
        pageEntry.slabByteOffset,
        pageHeader.cubeRecordsOffset,
        voxelCluster.firstCube + cubeOffset,
        cubeCoordPacked,
        dominantBoneIndex,
        occupancyMask,
        activeBounds);

    preparedCube.occupancyMask = occupancyMask;
    if ((occupancyMask.x | occupancyMask.y) == 0u)
    {
        return VOXEL_RASTER_PROJECT_REJECTED;
    }

    const uint3 cubeCoord = CLodVoxelDecodeCubeCoord(cubeCoordPacked);
    const float3 cubeMinObject = voxelSetup.aabbMin + float3(cubeCoord) * voxelSetup.cubeObjectWidth;
    const uint3 activeMinCell = VoxelRasterDecodeActiveBoundsMin(activeBounds);
    const uint3 activeMaxCell = VoxelRasterDecodeActiveBoundsMax(activeBounds);
    const float voxelObjectWidth = voxelSetup.cubeObjectWidth * 0.25f;
    const float3 activeMinObject = cubeMinObject + float3(activeMinCell) * voxelObjectWidth;
    const float3 activeMaxObject = cubeMinObject + float3(activeMaxCell + 1u) * voxelObjectWidth;

#if PSO_SKINNED
    row_major matrix localToWorld = objectData.model;
    row_major matrix worldToLocal = objectData.modelInverse;
    if (dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
    {
        const uint expandedBoneIndex = ResolveAssemblyBoneIndex(dominantBoneIndex, metadata, assemblyTransformIndex);
        const float4x4 skinMatrix = LoadAssemblyLocalBoneSkinMatrix(
            meshInstance.skinningInstanceSlot, expandedBoneIndex, assemblyTransformIndex);
        const float4x4 inverseSkinMatrix = LoadAssemblyLocalBoneInverseSkinMatrix(
            meshInstance.skinningInstanceSlot, expandedBoneIndex, assemblyTransformIndex);
        localToWorld = mul(skinMatrix, objectData.model);
        worldToLocal = mul(objectData.modelInverse, inverseSkinMatrix);
    }
#else
    const row_major matrix localToWorld = rigidSetup.localToWorld;
#endif

    VoxelRasterProjectedCube projected;
#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
    const float3 cubeCenterObject = (activeMinObject + activeMaxObject) * 0.5f;
    const float3 cubeHalfExtentObject = (activeMaxObject - activeMinObject) * 0.5f;
    const float sphereRadius = VoxelRasterComputeBoxSphereRadius(cubeHalfExtentObject, localToWorld);
    const uint projectResult = VoxelRasterProjectCubeToPixels(
        cubeCenterObject,
        localToWorld,
        sphereRadius,
        camera,
        rasterInfo,
        targetDims,
        projected);
#else
    const float3 cubeMaxObject = activeMaxObject;
    const row_major matrix localToClip = mul(localToWorld, camera.viewProjection);
#if PSO_SKINNED
    const float4 localViewZ = mul(localToWorld, camera.viewZ);
#else
    const float4 localViewZ = rigidSetup.localViewZ;
#endif
    const uint projectResult = VoxelRasterProjectCubeToPixels(
        activeMinObject,
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
    const float rayOriginViewZ = rigidSetup.rayOriginViewZ;
#endif
    const uint pixelWidth = uint(projected.maxPx.x - projected.minPx.x + 1);
    const uint pixelHeight = uint(projected.maxPx.y - projected.minPx.y + 1);

    preparedCube.packedMinPx = VoxelRasterPackMinPx(projected.minPx);
    preparedCube.packedPixelDims = VoxelRasterPackPixelDims(pixelWidth, pixelWidth * pixelHeight);
    preparedCube.minDepthBits = asuint(projected.minLinearDepth) >> 1u;
    preparedCube.cubeCoordPacked = cubeCoordPacked;
#if PSO_SKINNED
    preparedCube.traceBasis.localViewZAndRayOriginViewZ = float4(localViewZ.xyz, rayOriginViewZ);
    preparedCube.traceBasis.viewToLocalX = VoxelRasterViewToLocalX(viewToLocal);
    preparedCube.traceBasis.viewToLocalY = VoxelRasterViewToLocalY(viewToLocal);
    preparedCube.traceBasis.viewToLocalZ = VoxelRasterViewToLocalZ(viewToLocal);
#endif
    return VOXEL_RASTER_PROJECT_OK;
}

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
bool VoxelRasterPassesDepthPretestBits(uint2 pixel, uint cubeMinDepthBits, RWTexture2D<uint64_t> visibilityBuffer)
{
    const uint currentDepthBits = (uint)(visibilityBuffer[pixel] >> META_BITS);
    return currentDepthBits > cubeMinDepthBits;
}
#endif

uint VoxelRasterTracePixel(
    uint2 pixel,
    float2 targetDimsInv,
    VoxelRasterRayProjectionSetup projectionSetup,
    float4 viewToLocalX,
    float4 viewToLocalY,
    float4 viewToLocalZ,
    float3 rayOriginCube,
    float voxelWidth,
    uint2 occupancyMask,
    float rayOriginViewZ,
    float3 localViewZ,
    out float linearDepth)
{
    const float ndcX = (float(pixel.x) + 0.5f) * targetDimsInv.x * 2.0f - 1.0f;
    const float ndcY = 1.0f - (float(pixel.y) + 0.5f) * targetDimsInv.y * 2.0f;
    const float viewNearW = ndcX * projectionSetup.ndcX.w + ndcY * projectionSetup.ndcY.w + projectionSetup.ndcZ.w + projectionSetup.origin.w;
    const float viewNearInvW = rcp(max(viewNearW, 1.0e-6f));
    const float3 viewNear = (ndcX * projectionSetup.ndcX.xyz + ndcY * projectionSetup.ndcY.xyz + projectionSetup.ndcZ.xyz + projectionSetup.origin.xyz) * viewNearInvW;
    const float3 rayDirObject = float3(
        dot(viewNear, viewToLocalX.xyz),
        dot(viewNear, viewToLocalY.xyz),
        dot(viewNear, viewToLocalZ.xyz));
    const float rayLinearDepthStep = dot(rayDirObject, localViewZ);

    float tHitCube = 0.0f;
    if (!RaycastVoxelCubeDDA(rayOriginCube, rayDirObject, occupancyMask, tHitCube))
    {
        linearDepth = 0.0f;
        return VOXEL_RASTER_TRACE_DDA_MISS;
    }

    linearDepth = -(rayOriginViewZ + tHitCube * voxelWidth * rayLinearDepthStep);
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

bool VoxelRasterTryLoadWorkInputs(
    uint workIndex,
    uint GI,
    out CLodVoxelRasterWorkRecord work,
#if PSO_SKINNED
    out PerMeshInstanceBuffer meshInstance,
#endif
    out PerObjectBuffer objectData,
    out CLodMeshMetadata metadata,
    out uint visibleClusterAssemblyTransformIndex,
    out CullingCameraInfo camera,
    out ClodViewRasterInfo rasterInfo,
    out GroupPageMapEntry pageEntry,
    out CLodVoxelPageHeader pageHeader,
    out CLodVoxelClusterRecord voxelCluster)
{
    work = (CLodVoxelRasterWorkRecord)0;
#if PSO_SKINNED
    meshInstance = (PerMeshInstanceBuffer)0;
#endif
    metadata = (CLodMeshMetadata)0;
    visibleClusterAssemblyTransformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
    objectData = (PerObjectBuffer)0;
    camera = (CullingCameraInfo)0;
    rasterInfo = (ClodViewRasterInfo)0;
    pageEntry = (GroupPageMapEntry)0;
    pageHeader = (CLodVoxelPageHeader)0;
    voxelCluster = (CLodVoxelClusterRecord)0;

    StructuredBuffer<CLodVoxelRasterWorkRecord> workRecords = ResourceDescriptorHeap[CLOD_RASTER_VOXEL_WORK_RECORDS_DESCRIPTOR_INDEX];
    work = workRecords[workIndex];
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
    meshInstance = LoadMeshTemplateForDraw(instanceIndex);
    // Procedural wind palettes are frame-transient and indexed by the draw
    // record's instance transform. The mesh template retains the source slot,
    // which is bind pose for these assembly instances.
    meshInstance.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(
        instanceIndex, meshInstance.skinningInstanceSlot);
#endif
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceIndex);
    metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    camera = cameras[viewId];
    rasterInfo = viewRasterInfoBuffer[viewId];

    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const ClusterLODGroup group = groups[metadata.groupsBase + localGroupId];
    if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
        localPageIndex < group.pageMapBase ||
        localPageIndex >= group.pageMapBase + group.pageCount)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_PACKED_CLUSTER, 1u);
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SEGMENT_PAGE_MISSES, 1u);
        }
        return false;
    }

    StructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_RASTER_VOXEL_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    visibleClusterAssemblyTransformIndex = visibleClusterTransformIndices[work.visibleClusterIndex];
    objectData = LoadInstanceTransformForDrawWithAssemblyTransform(
        instanceIndex,
        visibleClusterAssemblyTransformIndex);

    pageEntry = CLodLoadVoxelPageMapEntry(metadata, group, localPageIndex);
    pageHeader = CLodLoadVoxelPageHeader(pageEntry.slabDescriptorIndex, pageEntry.slabByteOffset);
    if (pageHeader.formatAndKind != CLOD_VOXEL_PAGE_MAGIC ||
        pageLocalClusterIndex >= pageHeader.clusterCount)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_CLUSTER, 1u);
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SEGMENT_PAGE_MISSES, 1u);
        }
        return false;
    }

    voxelCluster = CLodLoadVoxelClusterFromPage(
        pageEntry.slabDescriptorIndex,
        pageEntry.slabByteOffset,
        pageHeader.descriptorOffset,
        pageLocalClusterIndex);
    if (voxelCluster.cubeCount == 0u || voxelCluster.cubeCount > CLOD_VOXEL_MAX_CUBES_PER_CLUSTER)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_CLUSTER, 1u);
        }
        return false;
    }

    const float voxelWidth = voxelCluster.aabbMinAndVoxelWidth.w;
    if (voxelWidth <= 0.0f)
    {
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_INVALID_VOXEL_WIDTH, 1u);
        }
        return false;
    }

    return true;
}

VoxelRasterVoxelSetup VoxelRasterBuildVoxelSetup(CLodVoxelClusterRecord voxelCluster)
{
    const float voxelWidth = voxelCluster.aabbMinAndVoxelWidth.w;
    VoxelRasterVoxelSetup voxelSetup;
    voxelSetup.aabbMin = voxelCluster.aabbMinAndVoxelWidth.xyz;
    voxelSetup.cubeObjectWidth = voxelWidth * 4.0f;
    voxelSetup.invVoxelWidth = rcp(voxelWidth);
    return voxelSetup;
}

#if !PSO_SKINNED
VoxelRasterRigidSetup VoxelRasterBuildRigidSetup(
    float voxelWidth,
    PerObjectBuffer objectData,
    CullingCameraInfo camera)
{
    VoxelRasterRigidSetup rigidSetup;
    rigidSetup.sphereRadius = 0.0f;
    rigidSetup.localToWorld = objectData.model;
    rigidSetup.localViewZ = mul(objectData.model, camera.viewZ);
    const row_major matrix viewToLocal = mul(camera.viewInverse, objectData.modelInverse);
    rigidSetup.viewToLocalX = VoxelRasterViewToLocalX(viewToLocal);
    rigidSetup.viewToLocalY = VoxelRasterViewToLocalY(viewToLocal);
    rigidSetup.viewToLocalZ = VoxelRasterViewToLocalZ(viewToLocal);
    rigidSetup.cameraOriginLocal = mul(float4(camera.positionWorldSpace.xyz, 1.0f), objectData.modelInverse).xyz;
    rigidSetup.rayOriginViewZ = dot(float4(rigidSetup.cameraOriginLocal, 1.0f), rigidSetup.localViewZ);
#if CLOD_VOXEL_RASTER_FAST_SPHERE_PROJECT
    rigidSetup.sphereRadius = VoxelRasterComputeCubeSphereRadius(voxelWidth * 2.0f, objectData.model);
#endif
    return rigidSetup;
}
#endif

#if !CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
void VoxelRasterRasterizeClusterDirect(
    uint GI,
    uint visibleClusterIndex,
    uint workFirstCubeOffset,
    uint workCubeEnd,
    GroupPageMapEntry pageEntry,
    CLodVoxelPageHeader pageHeader,
    CLodVoxelClusterRecord voxelCluster,
    VoxelRasterVoxelSetup voxelSetup,
#if PSO_SKINNED
    PerMeshInstanceBuffer meshInstance,
    PerObjectBuffer objectData,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex,
#endif
    CullingCameraInfo camera,
    ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
    float2 targetDimsInv,
    VoxelRasterRayProjectionSetup projectionSetup,
#if !PSO_SKINNED
    VoxelRasterRigidSetup rigidSetup,
#endif
#if CLOD_VOXEL_RASTER_DEBUG_VIS
    bool debugMode,
    RWTexture2D<uint2> debugVisTex,
#endif
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable,
    RWTexture2D<uint> physicalPages
#else
    RWTexture2D<uint64_t> visibilityBuffer
#endif
    )
{
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
            metadata,
            assemblyTransformIndex,
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
                if (projectResult == VOXEL_RASTER_PROJECT_SCISSOR_REJECTED)
                {
                    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SCISSOR_REJECTED, 1u);
                }
                else
                {
                    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED, 1u);
                }
            }
            continue;
        }

        const uint pixelCount = VoxelRasterUnpackPixelCount(preparedCube.packedPixelDims);
        const uint pixelWidth = VoxelRasterUnpackPixelWidth(preparedCube.packedPixelDims);
        const int2 minPx = VoxelRasterUnpackMinPx(preparedCube.packedMinPx);
        const uint minDepthBits = preparedCube.minDepthBits;
        if (GI == 0u)
        {
            VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTED_PIXELS, pixelCount);
        }

#if PSO_SKINNED
        const float4 viewToLocalX = preparedCube.traceBasis.viewToLocalX;
        const float4 viewToLocalY = preparedCube.traceBasis.viewToLocalY;
        const float4 viewToLocalZ = preparedCube.traceBasis.viewToLocalZ;
        const float3 cameraOriginLocal = VoxelRasterViewToLocalOrigin(viewToLocalX, viewToLocalY, viewToLocalZ);
        const float rayOriginViewZ = preparedCube.traceBasis.localViewZAndRayOriginViewZ.w;
        const float3 localViewZ = preparedCube.traceBasis.localViewZAndRayOriginViewZ.xyz;
#else
        const float4 viewToLocalX = rigidSetup.viewToLocalX;
        const float4 viewToLocalY = rigidSetup.viewToLocalY;
        const float4 viewToLocalZ = rigidSetup.viewToLocalZ;
        const float3 cameraOriginLocal = rigidSetup.cameraOriginLocal;
        const float rayOriginViewZ = rigidSetup.rayOriginViewZ;
        const float3 localViewZ = rigidSetup.localViewZ.xyz;
#endif
        const float3 rayOriginCube = VoxelRasterRayOriginCubeFromCoord(preparedCube.cubeCoordPacked, cameraOriginLocal, voxelSetup);
        const float voxelWidth = voxelSetup.cubeObjectWidth * 0.25f;
        for (uint pixelLinear = GI; pixelLinear < pixelCount; pixelLinear += VOXEL_RASTER_THREADS_PER_GROUP)
        {
            const uint2 pixel = VoxelRasterPixelFromLinear(pixelLinear, minPx, pixelWidth);

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
            if (!VoxelRasterPassesDepthPretestBits(pixel, minDepthBits, visibilityBuffer))
            {
                VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED, 1u);
                continue;
            }
#endif

            float linearDepth = 0.0f;
            const uint traceResult = VoxelRasterTracePixel(
                    pixel,
                    targetDimsInv,
                    projectionSetup,
                    viewToLocalX,
                    viewToLocalY,
                    viewToLocalZ,
                    rayOriginCube,
                    voxelWidth,
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
            VoxelRasterWriteHit(pixel, linearDepth, visibleClusterIndex, cubeOffset, visibilityBuffer);
#endif
        }
    }
}
#else
uint VoxelRasterPrepareQueuedCubeCandidate(
    uint cubeOffset,
    GroupPageMapEntry pageEntry,
    CLodVoxelPageHeader pageHeader,
    CLodVoxelClusterRecord voxelCluster,
    VoxelRasterVoxelSetup voxelSetup,
#if PSO_SKINNED
    PerMeshInstanceBuffer meshInstance,
    PerObjectBuffer objectData,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex,
#endif
    CullingCameraInfo camera,
    ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
#if !PSO_SKINNED
    VoxelRasterRigidSetup rigidSetup,
#endif
    out VoxelRasterPreparedCube preparedCube)
{
    preparedCube = (VoxelRasterPreparedCube)0;

    const uint projectResult = VoxelRasterPrepareCube(
        cubeOffset,
        pageEntry,
        pageHeader,
        voxelCluster,
        voxelSetup,
#if PSO_SKINNED
        meshInstance,
        objectData,
        metadata,
        assemblyTransformIndex,
#endif
        camera,
        rasterInfo,
        targetDims,
#if !PSO_SKINNED
        rigidSetup,
#endif
        preparedCube);

    const uint2 occupancyMask = preparedCube.occupancyMask;
    if ((occupancyMask.x | occupancyMask.y) == 0u ||
        projectResult != VOXEL_RASTER_PROJECT_OK)
    {
        preparedCube.packedPixelDims = 0u;
        return projectResult;
    }

    return VOXEL_RASTER_PROJECT_OK;
}

uint VoxelRasterAccumulateQueuedCubeBatchStats(
    uint GI,
    uint pixelCount,
    uint projectResult,
    uint2 occupancyMask)
{
    const bool hasOccupancy = (occupancyMask.x | occupancyMask.y) != 0u;
    const bool scissorRejected = hasOccupancy && projectResult == VOXEL_RASTER_PROJECT_SCISSOR_REJECTED;
    const bool projectionRejected = hasOccupancy &&
        projectResult != VOXEL_RASTER_PROJECT_OK &&
        projectResult != VOXEL_RASTER_PROJECT_SCISSOR_REJECTED;

    const uint waveMaxPixelCount = WaveActiveMax(pixelCount);
    const uint waveProjectedPixels = WaveActiveSum(pixelCount);
    const uint waveScissorRejected = WaveActiveCountBits(scissorRejected);
    const uint waveProjectionRejected = WaveActiveCountBits(projectionRejected);
    if (WaveIsFirstLane() && waveProjectedPixels != 0u)
    {
        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTED_PIXELS, waveProjectedPixels);
    }
    if (WaveIsFirstLane() && waveScissorRejected != 0u)
    {
        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_SCISSOR_REJECTED, waveScissorRejected);
    }
    if (WaveIsFirstLane() && waveProjectionRejected != 0u)
    {
        VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_PROJECTION_REJECTED, waveProjectionRejected);
    }

    return waveMaxPixelCount;
}

void VoxelRasterStoreQueuedPreparedCubeSlot(
    uint cubeSlot,
    VoxelRasterPreparedCube preparedCube)
{
    gs_voxelRasterPreparedCubes[cubeSlot] = preparedCube;
}

uint VoxelRasterFinalizeQueuedCubeCandidate(
    uint GI,
    bool batchCubeLane,
    uint cubeSlot,
    uint projectResult,
    VoxelRasterPreparedCube preparedCube)
{
    const uint pixelCount = VoxelRasterUnpackPixelCount(preparedCube.packedPixelDims);
    const uint batchMaxPixelCount = VoxelRasterAccumulateQueuedCubeBatchStats(GI, pixelCount, projectResult, preparedCube.occupancyMask);
    if (batchCubeLane)
    {
        VoxelRasterStoreQueuedPreparedCubeSlot(cubeSlot, preparedCube);
    }
    return batchMaxPixelCount;
}

void VoxelRasterPublishPreparedQueuedCubeBatch(
    uint GI,
    uint batchMaxPixelCountLocal,
    uint batchCubeCount,
    out uint batchQueuedCubeCount,
    out uint batchMaxPixelCount)
{
    if (GI == 0u)
    {
        gs_voxelRasterBatchMaxPixelCount = batchMaxPixelCountLocal;
    }
    GroupMemoryBarrierWithGroupSync();

    batchQueuedCubeCount = batchCubeCount;
    batchMaxPixelCount = gs_voxelRasterBatchMaxPixelCount;
}

void VoxelRasterPrepareQueuedCubeBatch(
    uint GI,
    uint batchFirstCubeOffset,
    uint batchCubeCount,
    GroupPageMapEntry pageEntry,
    CLodVoxelPageHeader pageHeader,
    CLodVoxelClusterRecord voxelCluster,
    VoxelRasterVoxelSetup voxelSetup,
#if PSO_SKINNED
    PerMeshInstanceBuffer meshInstance,
    PerObjectBuffer objectData,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex,
#endif
    CullingCameraInfo camera,
    ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
#if !PSO_SKINNED
    VoxelRasterRigidSetup rigidSetup,
#endif
    out uint batchQueuedCubeCount,
    out uint batchMaxPixelCount)
{
    uint cubeOffset = batchFirstCubeOffset + GI;
    uint projectResult = VOXEL_RASTER_PROJECT_OK;
    VoxelRasterPreparedCube preparedCube = (VoxelRasterPreparedCube)0;
    const bool batchCubeLane = GI < batchCubeCount;
    if (batchCubeLane)
    {
        projectResult = VoxelRasterPrepareQueuedCubeCandidate(
            cubeOffset,
            pageEntry,
            pageHeader,
            voxelCluster,
            voxelSetup,
#if PSO_SKINNED
            meshInstance,
            objectData,
            metadata,
            assemblyTransformIndex,
#endif
            camera,
            rasterInfo,
            targetDims,
#if !PSO_SKINNED
            rigidSetup,
#endif
            preparedCube);
    }

    const uint batchMaxPixelCountLocal = VoxelRasterFinalizeQueuedCubeCandidate(
        GI,
        batchCubeLane,
        GI,
        projectResult,
        preparedCube);
    VoxelRasterPublishPreparedQueuedCubeBatch(GI, batchMaxPixelCountLocal, batchCubeCount, batchQueuedCubeCount, batchMaxPixelCount);
}

uint2 VoxelRasterQueueLocalPixelFromLinear(uint pixelLinear, uint pixelWidth, float pixelWidthInv)
{
    uint y = uint(float(pixelLinear) * pixelWidthInv);
    uint rowBase = y * pixelWidth;
    if (pixelLinear < rowBase)
    {
        --y;
        rowBase -= pixelWidth;
    }
    else if (pixelLinear - rowBase >= pixelWidth)
    {
        ++y;
        rowBase += pixelWidth;
    }

    return uint2(pixelLinear - rowBase, y);
}

void VoxelRasterQueueDecodeTaskPixel(
    uint taskLinear,
    uint batchMaxPixelCount,
    float batchMaxPixelCountInv,
    out uint cubeSlot,
    out uint pixelLinear)
{
    cubeSlot = uint(float(taskLinear) * batchMaxPixelCountInv);
    uint cubeBase = cubeSlot * batchMaxPixelCount;
    if (taskLinear < cubeBase)
    {
        --cubeSlot;
        cubeBase -= batchMaxPixelCount;
    }
    else if (taskLinear - cubeBase >= batchMaxPixelCount)
    {
        ++cubeSlot;
        cubeBase += batchMaxPixelCount;
    }

    pixelLinear = taskLinear - cubeBase;
}

void VoxelRasterQueueAdvanceTaskPixel(
    uint cubeStep,
    uint pixelStepRemainder,
    uint batchMaxPixelCount,
    inout uint cubeSlot,
    inout uint pixelLinear)
{
    cubeSlot += cubeStep;
    pixelLinear += pixelStepRemainder;
    if (pixelLinear >= batchMaxPixelCount)
    {
        pixelLinear -= batchMaxPixelCount;
        ++cubeSlot;
    }
}

bool VoxelRasterQueueCandidateFromTask(
    uint cubeSlot,
    uint pixelLinear,
    out uint2 localPixel
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
    ,
    out uint2 pixel,
    out uint minDepthBits
#endif
    )
{
    const uint packedPixelDims = gs_voxelRasterPreparedCubes[cubeSlot].packedPixelDims;
    const uint pixelCount = VoxelRasterUnpackPixelCount(packedPixelDims);
    if (pixelLinear >= pixelCount)
    {
        localPixel = uint2(0u, 0u);
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
        pixel = uint2(0u, 0u);
        minDepthBits = 0u;
#endif
        return false;
    }

    const uint pixelWidth = VoxelRasterUnpackPixelWidth(packedPixelDims);
    localPixel = VoxelRasterQueueLocalPixelFromLinear(pixelLinear, pixelWidth, rcp(float(pixelWidth)));
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
    pixel = uint2(VoxelRasterUnpackMinPx(gs_voxelRasterPreparedCubes[cubeSlot].packedMinPx)) + localPixel;
    minDepthBits = gs_voxelRasterPreparedCubes[cubeSlot].minDepthBits;
#endif
    return true;
}

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
bool VoxelRasterQueuePassesDepthPretest(
    bool enqueuePixel,
    uint2 pixel,
    uint minDepthBits,
    RWTexture2D<uint64_t> visibilityBuffer)
{
    if (!enqueuePixel)
    {
        return false;
    }

    if (VoxelRasterPassesDepthPretestBits(pixel, minDepthBits, visibilityBuffer))
    {
        return true;
    }

    VoxelRasterTelemetryAdd(WG_COUNTER_VOXEL_RASTER_DEPTH_REJECTED, 1u);
    return false;
}
#endif

void VoxelRasterQueuePushDecodedTaskCandidate(
    uint queueBase,
    inout uint queuedPixelCount,
    uint cubeSlot,
    uint pixelLinear
    VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_PARAM)
{
    uint2 localPixel = uint2(0u, 0u);
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
    uint2 pixel = uint2(0u, 0u);
    uint minDepthBits = 0u;
#endif

    bool enqueuePixel = VoxelRasterQueueCandidateFromTask(
        cubeSlot,
        pixelLinear,
        localPixel
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
        ,
        pixel,
        minDepthBits
#endif
        );

#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && CLOD_VOXEL_RASTER_ENABLE_DEPTH_PRETEST
    enqueuePixel = VoxelRasterQueuePassesDepthPretest(enqueuePixel, pixel, minDepthBits, visibilityBuffer);
#endif

    VoxelRasterQueuePushPixelCoords(queueBase, queuedPixelCount, enqueuePixel, cubeSlot, localPixel);
}

void VoxelRasterQueueBatchPixels(
    uint GI,
    uint queueBase,
    uint batchMaxPixelCount,
    float batchMaxPixelCountInv,
    uint taskBase,
    uint taskEnd,
    out uint queuedPixelCount
    VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_PARAM
    )
{
    queuedPixelCount = 0u;

    if (batchMaxPixelCount == VOXEL_RASTER_THREADS_PER_GROUP)
    {
        uint cubeSlot = taskBase / VOXEL_RASTER_THREADS_PER_GROUP;
        const uint pixelLinear = GI;
        for (uint taskLinear = taskBase + GI; taskLinear < taskEnd; taskLinear += VOXEL_RASTER_THREADS_PER_GROUP * 2u)
        {
            VoxelRasterQueuePushDecodedTaskCandidate(
                queueBase,
                queuedPixelCount,
                cubeSlot,
                pixelLinear
                VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_ARG
                );
            ++cubeSlot;
            if (taskLinear + VOXEL_RASTER_THREADS_PER_GROUP < taskEnd)
            {
                VoxelRasterQueuePushDecodedTaskCandidate(
                    queueBase,
                    queuedPixelCount,
                    cubeSlot,
                    pixelLinear
                    VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_ARG
                    );
                ++cubeSlot;
            }
        }
        return;
    }

    const uint firstTaskLinear = taskBase + GI;
    uint cubeSlot = 0u;
    uint pixelLinear = 0u;
    VoxelRasterQueueDecodeTaskPixel(firstTaskLinear, batchMaxPixelCount, batchMaxPixelCountInv, cubeSlot, pixelLinear);
    const uint taskPixelStepCube = VOXEL_RASTER_THREADS_PER_GROUP / batchMaxPixelCount;
    const uint taskPixelStepRemainder = VOXEL_RASTER_THREADS_PER_GROUP - taskPixelStepCube * batchMaxPixelCount;

    for (uint taskLinear = firstTaskLinear; taskLinear < taskEnd; taskLinear += VOXEL_RASTER_THREADS_PER_GROUP * 2u)
    {
        VoxelRasterQueuePushDecodedTaskCandidate(
            queueBase,
            queuedPixelCount,
            cubeSlot,
            pixelLinear
            VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_ARG
            );

        VoxelRasterQueueAdvanceTaskPixel(taskPixelStepCube, taskPixelStepRemainder, batchMaxPixelCount, cubeSlot, pixelLinear);
        if (taskLinear + VOXEL_RASTER_THREADS_PER_GROUP < taskEnd)
        {
            VoxelRasterQueuePushDecodedTaskCandidate(
                queueBase,
                queuedPixelCount,
                cubeSlot,
                pixelLinear
                VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_ARG
                );
            VoxelRasterQueueAdvanceTaskPixel(taskPixelStepCube, taskPixelStepRemainder, batchMaxPixelCount, cubeSlot, pixelLinear);
        }
    }
}

void VoxelRasterTraceQueuedPixels(
    uint waveLane,
    uint queueBase,
    uint queuedPixelCount,
    uint visibleClusterIndex,
    uint batchFirstCubeOffset,
    float2 targetDimsInv,
    VoxelRasterRayProjectionSetup projectionSetup,
    float voxelWidth,
    VoxelRasterVoxelSetup voxelSetup
#if !PSO_SKINNED
    ,
    VoxelRasterRigidSetup rigidSetup
#endif
#if CLOD_VOXEL_RASTER_DEBUG_VIS
    ,
    bool debugMode,
    RWTexture2D<uint2> debugVisTex
#endif
    VOXEL_RASTER_OUTPUT_TARGET_PARAMS
    )
{
    for (uint queueSlot = waveLane; queueSlot < min(queuedPixelCount, VOXEL_RASTER_WAVE_PIXEL_QUEUE_CAPACITY); queueSlot += VOXEL_RASTER_WAVE_SIZE)
    {
        const uint queuedPixel = gs_voxelRasterPixelQueue[queueBase + queueSlot];
        const uint cubeSlot = VoxelRasterQueuedPixelCubeSlot(queuedPixel);
        const uint2 localPixel = VoxelRasterQueuedPixelLocalCoords(queuedPixel);
        const uint2 occupancyMask = gs_voxelRasterPreparedCubes[cubeSlot].occupancyMask;
        const uint2 pixel = uint2(VoxelRasterUnpackMinPx(gs_voxelRasterPreparedCubes[cubeSlot].packedMinPx)) + localPixel;
#if PSO_SKINNED
        const float4 viewToLocalX = gs_voxelRasterPreparedCubes[cubeSlot].traceBasis.viewToLocalX;
        const float4 viewToLocalY = gs_voxelRasterPreparedCubes[cubeSlot].traceBasis.viewToLocalY;
        const float4 viewToLocalZ = gs_voxelRasterPreparedCubes[cubeSlot].traceBasis.viewToLocalZ;
        const float3 cameraOriginLocal = VoxelRasterViewToLocalOrigin(viewToLocalX, viewToLocalY, viewToLocalZ);
        const float rayOriginViewZ = gs_voxelRasterPreparedCubes[cubeSlot].traceBasis.localViewZAndRayOriginViewZ.w;
        const float3 localViewZ = gs_voxelRasterPreparedCubes[cubeSlot].traceBasis.localViewZAndRayOriginViewZ.xyz;
#else
        const float4 viewToLocalX = rigidSetup.viewToLocalX;
        const float4 viewToLocalY = rigidSetup.viewToLocalY;
        const float4 viewToLocalZ = rigidSetup.viewToLocalZ;
        const float3 cameraOriginLocal = rigidSetup.cameraOriginLocal;
        const float rayOriginViewZ = rigidSetup.rayOriginViewZ;
        const float3 localViewZ = rigidSetup.localViewZ.xyz;
#endif
        const float3 rayOriginCube = VoxelRasterRayOriginCubeFromCoord(gs_voxelRasterPreparedCubes[cubeSlot].cubeCoordPacked, cameraOriginLocal, voxelSetup);
        float linearDepth = 0.0f;
        const uint traceResult = VoxelRasterTracePixel(
            pixel,
            targetDimsInv,
            projectionSetup,
            viewToLocalX,
            viewToLocalY,
            viewToLocalZ,
            rayOriginCube,
            voxelWidth,
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
        const uint cubeOffset = batchFirstCubeOffset + cubeSlot;
        VoxelRasterWriteHit(pixel, linearDepth, visibleClusterIndex, cubeOffset, visibilityBuffer);
#endif
    }
}

void VoxelRasterFinishQueuedCubeBatch()
{
    GroupMemoryBarrierWithGroupSync();
}

void VoxelRasterRasterizeClusterQueued(
    uint GI,
    uint visibleClusterIndex,
    uint workFirstCubeOffset,
    uint workCubeEnd,
    GroupPageMapEntry pageEntry,
    CLodVoxelPageHeader pageHeader,
    CLodVoxelClusterRecord voxelCluster,
    VoxelRasterVoxelSetup voxelSetup,
#if PSO_SKINNED
    PerMeshInstanceBuffer meshInstance,
    PerObjectBuffer objectData,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex,
#endif
    CullingCameraInfo camera,
    ClodViewRasterInfo rasterInfo,
    uint2 targetDims,
    float2 targetDimsInv,
    VoxelRasterRayProjectionSetup projectionSetup
#if !PSO_SKINNED
    ,
    VoxelRasterRigidSetup rigidSetup
#endif
#if CLOD_VOXEL_RASTER_DEBUG_VIS
    ,
    bool debugMode,
    RWTexture2D<uint2> debugVisTex
#endif
    VOXEL_RASTER_OUTPUT_TARGET_PARAMS
    )
{
    const uint waveLane = WaveGetLaneIndex();
    const uint waveIndex = GI / VOXEL_RASTER_WAVE_SIZE;
    const uint waveQueueBase = waveIndex * VOXEL_RASTER_WAVE_PIXEL_QUEUE_CAPACITY;
    const float voxelWidth = voxelSetup.cubeObjectWidth * 0.25f;

    for (uint batchFirstCubeOffset = workFirstCubeOffset; batchFirstCubeOffset < workCubeEnd; batchFirstCubeOffset += VOXEL_RASTER_CUBE_BATCH_SIZE)
    {
        const uint batchCubeCount = min(VOXEL_RASTER_CUBE_BATCH_SIZE, workCubeEnd - batchFirstCubeOffset);
        uint batchQueuedCubeCount = 0u;
        uint batchMaxPixelCount = 0u;
        VoxelRasterPrepareQueuedCubeBatch(
            GI,
            batchFirstCubeOffset,
            batchCubeCount,
            pageEntry,
            pageHeader,
            voxelCluster,
            voxelSetup,
#if PSO_SKINNED
            meshInstance,
            objectData,
            metadata,
            assemblyTransformIndex,
#endif
            camera,
            rasterInfo,
            targetDims,
#if !PSO_SKINNED
            rigidSetup,
#endif
            batchQueuedCubeCount,
            batchMaxPixelCount);

        const uint batchTaskCount = batchQueuedCubeCount * batchMaxPixelCount;
        const float batchMaxPixelCountInv = rcp(max(float(batchMaxPixelCount), 1.0f));
        for (uint taskBase = 0u; taskBase < batchTaskCount; taskBase += VOXEL_RASTER_PIXEL_QUEUE_CAPACITY)
        {
            const uint taskEnd = min(taskBase + VOXEL_RASTER_PIXEL_QUEUE_CAPACITY, batchTaskCount);
            uint queuedPixelCount = 0u;
            VoxelRasterQueueBatchPixels(
                GI,
                waveQueueBase,
                batchMaxPixelCount,
                batchMaxPixelCountInv,
                taskBase,
                taskEnd,
                queuedPixelCount
                VOXEL_RASTER_DEPTH_PRETEST_VISIBILITY_ARG
                );

            VoxelRasterTraceQueuedPixels(
                waveLane,
                waveQueueBase,
                queuedPixelCount,
                visibleClusterIndex,
                batchFirstCubeOffset,
                targetDimsInv,
                projectionSetup,
                voxelWidth,
                voxelSetup
#if !PSO_SKINNED
                ,
                rigidSetup
#endif
#if CLOD_VOXEL_RASTER_DEBUG_VIS
                ,
                debugMode,
                debugVisTex
#endif
                VOXEL_RASTER_OUTPUT_TARGET_ARGS
                );
        }

        VoxelRasterFinishQueuedCubeBatch();
    }
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

#if CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
[WaveSize(32)]
#endif
[numthreads(VOXEL_RASTER_THREADS_PER_GROUP, 1, 1)]
void VoxelRasterCS(uint3 groupId : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID)
{
    const uint workIndex = groupId.x;
    const uint GI = groupThreadID.x;

    CLodVoxelRasterWorkRecord work = (CLodVoxelRasterWorkRecord)0;
#if PSO_SKINNED
    PerMeshInstanceBuffer meshInstance = (PerMeshInstanceBuffer)0;
#endif
    PerObjectBuffer objectData = (PerObjectBuffer)0;
    CLodMeshMetadata metadata = (CLodMeshMetadata)0;
    uint visibleClusterAssemblyTransformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
    CullingCameraInfo camera = (CullingCameraInfo)0;
    ClodViewRasterInfo rasterInfo = (ClodViewRasterInfo)0;
    GroupPageMapEntry pageEntry = (GroupPageMapEntry)0;
    CLodVoxelPageHeader pageHeader = (CLodVoxelPageHeader)0;
    CLodVoxelClusterRecord voxelCluster = (CLodVoxelClusterRecord)0;
    if (!VoxelRasterTryLoadWorkInputs(
        workIndex,
        GI,
        work,
#if PSO_SKINNED
        meshInstance,
#endif
        objectData,
        metadata,
        visibleClusterAssemblyTransformIndex,
        camera,
        rasterInfo,
        pageEntry,
        pageHeader,
        voxelCluster))
    {
        return;
    }

    const float voxelWidth = voxelCluster.aabbMinAndVoxelWidth.w;

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    uint2 targetDims = uint2(rasterInfo.scissorMaxX, rasterInfo.scissorMaxY);
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    if (!CLodVirtualShadowTryGetClipmapInfoForView(work.viewId, clipmapInfos, clipmapInfo))
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
    const VoxelRasterRayProjectionSetup projectionSetup = VoxelRasterBuildRayProjectionSetup(camera.projectionInverse);

    const VoxelRasterVoxelSetup voxelSetup = VoxelRasterBuildVoxelSetup(voxelCluster);

#if !PSO_SKINNED
    const VoxelRasterRigidSetup rigidSetup = VoxelRasterBuildRigidSetup(voxelWidth, objectData, camera);
#endif

    const uint workFirstCubeOffset = 0u;
    const uint workCubeEnd = voxelCluster.cubeCount;

#if !CLOD_VOXEL_RASTER_USE_PIXEL_QUEUE
    VoxelRasterRasterizeClusterDirect(
        GI,
        work.visibleClusterIndex,
        workFirstCubeOffset,
        workCubeEnd,
        pageEntry,
        pageHeader,
        voxelCluster,
        voxelSetup,
#if PSO_SKINNED
        meshInstance,
        objectData,
        metadata,
        visibleClusterAssemblyTransformIndex,
#endif
        camera,
        rasterInfo,
        targetDims,
        targetDimsInv,
        projectionSetup
#if !PSO_SKINNED
        ,
        rigidSetup
#endif
#if CLOD_VOXEL_RASTER_DEBUG_VIS
        ,
        debugMode,
        debugVisTex
#endif
        VOXEL_RASTER_OUTPUT_TARGET_ARGS
        );
#else
    VoxelRasterRasterizeClusterQueued(
        GI,
        work.visibleClusterIndex,
        workFirstCubeOffset,
        workCubeEnd,
        pageEntry,
        pageHeader,
        voxelCluster,
        voxelSetup,
#if PSO_SKINNED
        meshInstance,
        objectData,
        metadata,
        visibleClusterAssemblyTransformIndex,
#endif
        camera,
        rasterInfo,
        targetDims,
        targetDimsInv,
        projectionSetup,
#if !PSO_SKINNED
        rigidSetup,
#endif
#if CLOD_VOXEL_RASTER_DEBUG_VIS
        debugMode,
        debugVisTex,
#endif
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        clipmapInfo,
        pageTable,
        physicalPages
#else
        visibilityBuffer
#endif
        );
#endif
}
