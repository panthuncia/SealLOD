// The light grid (docs/lighting.md "The light grid"; layout and readers in include/lightGrid.hlsli). Three passes a frame:
//
//   LightGridPrepareCS      one view-space culling record per active light (Builtin::Light::CullingRecords), and every
//                           froxel's depth bounds reset.
//   LightGridDepthBoundsCS  each opaque pixel widens its froxel's depth bounds (nearest and farthest pixel) and its tile's
//                           farthest depth (Builtin::Light::GridDepthBounds).
//   LightCullingCS          one thread group per block of LIGHT_GRID_COARSE_* froxels, one thread per froxel. Each batch of
//                           lights is tested against the block's bounds, then each froxel tests the survivors. A first
//                           sweep counts each froxel's lights, a wave-aggregated atomic gives it a contiguous run of
//                           Builtin::Light::GridIndices, and a second sweep fills it. Nothing is dropped unless GridIndices
//                           is full; then CullingStats says how many entries were needed, and the renderer grows it.

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/lightModel.hlsli"
#include "include/lightGrid.hlsli"
#include "PerPassRootConstants/lightCullingRootConstants.h"

// Bounds padding, relative to depth: the forward and opaque readers compute their slice with the same function from the
// same depth, so this only absorbs rounding between the slice boundaries and the pixels.
#ifndef LIGHT_GRID_DEPTH_EPSILON
#define LIGHT_GRID_DEPTH_EPSILON 1.0e-3f
#endif
#define LIGHT_GRID_EMPTY_DEPTH 0x7F7FFFFFu // The linear depth of a background pixel.
// A pixel within rounding of a slice boundary also widens the neighbouring slice's bounds. 0: only its own slice.
#ifndef LIGHT_GRID_BOUNDS_NEIGHBOURS
#define LIGHT_GRID_BOUNDS_NEIGHBOURS 1
#endif
// Diagnostic: the depth bounds pass reads depth and writes the tile's farthest depth only (for timing its floor).
#ifndef LIGHT_GRID_BOUNDS_STUB
#define LIGHT_GRID_BOUNDS_STUB 0
#endif

// True if a culling record's sphere touches an axis-aligned box in view space. A negative radius is a directional light,
// which reaches everything.
bool LightGridSphereTouchesBox(float4 sphere, float3 boxMin, float3 boxMax)
{
    const float3 closest = clamp(sphere.xyz, boxMin, boxMax);
    const float3 offset = closest - sphere.xyz;
    return sphere.w < 0.0f || dot(offset, offset) <= sphere.w * sphere.w;
}

// The view-space box of a tile's frustum between view depths near and far. kx and ky are the tile's edge slopes (view
// x / depth and y / depth); the frustum is bilinear in slope and depth, so its corners bound it exactly.
void LightGridFrustumBox(float2 kx, float2 ky, float nearDepth, float farDepth, out float3 boxMin, out float3 boxMax)
{
    boxMin = float3(min(kx.x * nearDepth, kx.x * farDepth), min(ky.x * nearDepth, ky.x * farDepth), -farDepth);
    boxMax = float3(max(kx.y * nearDepth, kx.y * farDepth), max(ky.y * nearDepth, ky.y * farDepth), -nearDepth);
}

// ---------------------------------------------------------------------------------------------------------------------
[numthreads(LIGHT_GRID_PREPARE_THREADS, 1, 1)]
void LightGridPrepareCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const uint i = dispatchThreadId.x;
    const uint boundsCount = LightGridCellCount(perFrame) + perFrame.lightClusterGridSizeX * perFrame.lightClusterGridSizeY;
    if (i < boundsCount)
    {
        RWStructuredBuffer<uint2> bounds = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridDepthBounds)];
        bounds[i] = uint2(0xFFFFFFFFu, 0u);
    }
    const uint recordCount = min(perFrame.numLights, LIGHT_GRID_RECORD_CAPACITY);
    if (i < recordCount)
    {
        StructuredBuffer<uint> activeLights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
        StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        RWStructuredBuffer<LightCullingRecord> records = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::CullingRecords)];
        const uint lightIndex = activeLights[i];
        const LightInfo light = lights[lightIndex];
        LightCullingRecord record;
        // Point and spot lights: their bounding sphere (a spot's encloses its cone). Directional: everywhere.
        record.sphere = light.type == LIGHT_TYPE_DIRECTIONAL
            ? float4(0.0f, 0.0f, 0.0f, -1.0f)
            : float4(mul(float4(light.boundingSphere.sphere.xyz, 1.0f), cameras[perFrame.mainCameraIndex].view).xyz, light.boundingSphere.sphere.w);
        record.lightIndex = lightIndex;
        record.pad = uint3(0u, 0u, 0u);
        records[i] = record;
    }
    if (i == 0u && perFrame.numLights > recordCount)
    {
        // The record buffer grows with the light count (graph rebuild); until then the lights past it are left out.
        RWStructuredBuffer<uint> cullingStats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::CullingStats)];
        InterlockedAdd(cullingStats[LIGHT_CULLING_STATS_DROPPED_LIGHTS], perFrame.numLights - recordCount);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// A group covers LIGHT_GRID_DEPTH_BOUNDS_TILE pixels square, inside one grid tile (the tile size is a multiple of it). Its
// pixels reduce into group-shared bounds per slice, and one thread per slice then widens the global bounds.
#define LIGHT_GRID_MAX_SLICES 256u
groupshared uint gs_sliceNear[LIGHT_GRID_MAX_SLICES];
groupshared uint gs_sliceFar[LIGHT_GRID_MAX_SLICES];
groupshared uint gs_tileFar;

void LightGridWidenSlice(uint slice, uint depthBits, bool active)
{
    // A wave's pixels nearly always share a slice: one shared atomic for the wave then.
    if (WaveActiveAllTrue(active) && WaveActiveAllEqual(slice))
    {
        const uint nearest = WaveActiveMin(depthBits);
        const uint farthest = WaveActiveMax(depthBits);
        if (WaveIsFirstLane())
        {
            InterlockedMin(gs_sliceNear[slice], nearest);
            InterlockedMax(gs_sliceFar[slice], farthest);
        }
    }
    else if (active)
    {
        InterlockedMin(gs_sliceNear[slice], depthBits);
        InterlockedMax(gs_sliceFar[slice], depthBits);
    }
}

[numthreads(LIGHT_GRID_DEPTH_BOUNDS_TILE, LIGHT_GRID_DEPTH_BOUNDS_TILE, 1)]
void LightGridDepthBoundsCS(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    RWStructuredBuffer<uint2> bounds = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridDepthBounds)];

    const uint slices = min(perFrame.lightClusterGridSizeZ, LIGHT_GRID_MAX_SLICES);
    for (uint i = groupIndex; i < slices; i += LIGHT_GRID_DEPTH_BOUNDS_TILE * LIGHT_GRID_DEPTH_BOUNDS_TILE)
    {
        gs_sliceNear[i] = 0xFFFFFFFFu;
        gs_sliceFar[i] = 0u;
    }
    if (groupIndex == 0u)
    {
        gs_tileFar = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = dispatchThreadId.xy;
    const bool inBounds = pixel.x < perFrame.screenResX && pixel.y < perFrame.screenResY;
    const float depth = inBounds ? depthTexture[pixel] : 0.0f;
    const bool background = inBounds && asuint(depth) == LIGHT_GRID_EMPTY_DEPTH;
    const bool surface = inBounds && !background && depth > 0.0f;

    // The tile's farthest depth bounds its froxels for forward passes; a background pixel leaves it unbounded (a
    // transparent surface in front of the sky can be at any depth).
    const uint farBits = WaveActiveMax(surface ? asuint(depth) : (background ? LIGHT_GRID_EMPTY_DEPTH : 0u));
    if (WaveIsFirstLane())
    {
        InterlockedMax(gs_tileFar, farBits);
    }

    // The pixel's froxel, as its readers compute it from the same depth. A depth within rounding of a slice boundary
    // also widens the neighbouring slice.
    const Camera camera = cameras[perFrame.mainCameraIndex];
#if !LIGHT_GRID_BOUNDS_STUB
    const float sliceCoordinate = LightGridSliceCoordinate(depth, perFrame, camera.zNear, camera.zFar);
    const uint slice = min(LightGridSliceFromCoordinate(sliceCoordinate, perFrame), slices - 1u);
    LightGridWidenSlice(slice, asuint(depth), surface);
#if LIGHT_GRID_BOUNDS_NEIGHBOURS
    // Readers compute the same coordinate from the same depth, but another shader's arithmetic may round differently.
    const float sliceFraction = sliceCoordinate - float(slice);
    const uint neighbour = sliceFraction < 1.0e-3f ? slice - 1u : (sliceFraction > 1.0f - 1.0e-3f ? slice + 1u : slice);
    if (surface && neighbour != slice && neighbour < slices)
    {
        InterlockedMin(gs_sliceNear[neighbour], asuint(depth));
        InterlockedMax(gs_sliceFar[neighbour], asuint(depth));
    }
#endif
#endif
    GroupMemoryBarrierWithGroupSync();

    const uint2 tile = LightGridTile(groupId.xy * LIGHT_GRID_DEPTH_BOUNDS_TILE, perFrame);
    for (uint z = groupIndex; z < slices; z += LIGHT_GRID_DEPTH_BOUNDS_TILE * LIGHT_GRID_DEPTH_BOUNDS_TILE)
    {
        const uint nearest = gs_sliceNear[z];
        const uint farthest = gs_sliceFar[z];
        if (nearest <= farthest)
        {
            const uint cellIndex = LightGridCellIndex(uint3(tile, z), perFrame);
            InterlockedMin(bounds[cellIndex].x, nearest);
            InterlockedMax(bounds[cellIndex].y, farthest);
        }
    }
    if (groupIndex == 0u && gs_tileFar != 0u)
    {
        InterlockedMax(bounds[LightGridCellCount(perFrame) + tile.x + perFrame.lightClusterGridSizeX * tile.y].y, gs_tileFar);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
#define LIGHT_GRID_MAX_WAVES 4u // LIGHT_GRID_COARSE_THREADS / the smallest wave (16 lanes)

groupshared float4 gs_survivorSphere[LIGHT_GRID_COARSE_THREADS];
groupshared uint gs_survivorRecord[LIGHT_GRID_COARSE_THREADS];
groupshared uint gs_waveCount[LIGHT_GRID_MAX_WAVES];
groupshared float3 gs_waveBoxMin[LIGHT_GRID_MAX_WAVES];
groupshared float3 gs_waveBoxMax[LIGHT_GRID_MAX_WAVES];

[numthreads(LIGHT_GRID_COARSE_THREADS, 1, 1)]
void LightCullingCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    StructuredBuffer<LightCullingRecord> records = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::CullingRecords)];
    StructuredBuffer<uint2> bounds = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridDepthBounds)];
    RWStructuredBuffer<uint2> cells = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridCells)];
    RWStructuredBuffer<uint> indices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridIndices)];
    RWStructuredBuffer<uint> cullingStats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::CullingStats)];

    const uint3 grid = uint3(perFrame.lightClusterGridSizeX, perFrame.lightClusterGridSizeY, perFrame.lightClusterGridSizeZ);
    const uint3 local = uint3(groupIndex % LIGHT_GRID_COARSE_X, (groupIndex / LIGHT_GRID_COARSE_X) % LIGHT_GRID_COARSE_Y,
        groupIndex / (LIGHT_GRID_COARSE_X * LIGHT_GRID_COARSE_Y));
    const uint3 cell = groupId * uint3(LIGHT_GRID_COARSE_X, LIGHT_GRID_COARSE_Y, LIGHT_GRID_COARSE_Z) + local;
    const bool inGrid = all(cell < grid);
    const uint cellIndex = LightGridCellIndex(cell, perFrame);
    const bool forwardLists = (LIGHT_GRID_FLAGS & LIGHT_GRID_FLAG_FORWARD_LISTS) != 0u;

    // ---- The froxel's two boxes: the opaque one around its occupied depth, the forward one around the whole froxel in
    // front of the tile's farthest opaque pixel (with a pixel of slack for jittered forward fragments).
    const Camera camera = cameras[perFrame.mainCameraIndex];
    const float2 resolution = float2(perFrame.screenResX, perFrame.screenResY);
    const float tileSize = float(max(perFrame.lightGridTileSize, 1u));
    const float2 pixelMin = float2(cell.xy) * tileSize;
    const float2 pixelMax = min(pixelMin + tileSize, resolution);
    // Slopes as deferred.hlsl rebuilds a pixel's view position: x = ndc.x * projectionInverse[0][0] * depth, ndc.y up.
    const float slopeX = camera.projectionInverse[0][0];
    const float slopeY = camera.projectionInverse[1][1];
    const float2 opaqueKx = float2(pixelMin.x, pixelMax.x) / resolution.x * 2.0f - 1.0f;
    const float2 opaqueKy = 1.0f - float2(pixelMax.y, pixelMin.y) / resolution.y * 2.0f;
    const float2 forwardKx = float2(pixelMin.x - 1.0f, pixelMax.x + 1.0f) / resolution.x * 2.0f - 1.0f;
    const float2 forwardKy = 1.0f - float2(pixelMax.y + 1.0f, pixelMin.y - 1.0f) / resolution.y * 2.0f;

    bool opaqueValid = false;
    bool forwardValid = false;
    float3 opaqueMin = 0.0f, opaqueMax = 0.0f, forwardMin = 0.0f, forwardMax = 0.0f;
    if (inGrid)
    {
        const uint2 occupied = bounds[cellIndex];
        if (occupied.x <= occupied.y)
        {
            opaqueValid = true;
            LightGridFrustumBox(opaqueKx * slopeX, opaqueKy * slopeY, asfloat(occupied.x) * (1.0f - LIGHT_GRID_DEPTH_EPSILON),
                asfloat(occupied.y) * (1.0f + LIGHT_GRID_DEPTH_EPSILON), opaqueMin, opaqueMax);
        }
        if (forwardLists)
        {
            const uint tileFarBits = bounds[LightGridCellCount(perFrame) + cell.x + grid.x * cell.y].y;
            const float sliceNear = LightGridSliceBoundary(cell.z, perFrame, camera.zNear, camera.zFar) * (1.0f - LIGHT_GRID_DEPTH_EPSILON);
            float sliceFar = LightGridSliceBoundary(cell.z + 1u, perFrame, camera.zNear, camera.zFar) * (1.0f + LIGHT_GRID_DEPTH_EPSILON);
            if (tileFarBits != LIGHT_GRID_EMPTY_DEPTH)
            {
                sliceFar = min(sliceFar, asfloat(tileFarBits) * (1.0f + LIGHT_GRID_DEPTH_EPSILON));
            }
            if (sliceNear < sliceFar)
            {
                forwardValid = true;
                LightGridFrustumBox(forwardKx * slopeX, forwardKy * slopeY, sliceNear, sliceFar, forwardMin, forwardMax);
            }
        }
    }

    // ---- The block's box: the union of its froxels' boxes (the forward box contains the opaque one).
    const uint laneCount = WaveGetLaneCount();
    const uint waveIndex = groupIndex / laneCount;
    const uint waveCount = (LIGHT_GRID_COARSE_THREADS + laneCount - 1u) / laneCount;
    {
        const float3 ownMin = forwardValid ? forwardMin : (opaqueValid ? opaqueMin : float3(1.0e30f, 1.0e30f, 1.0e30f));
        const float3 ownMax = forwardValid ? forwardMax : (opaqueValid ? opaqueMax : float3(-1.0e30f, -1.0e30f, -1.0e30f));
        const float3 waveMin = WaveActiveMin(ownMin);
        const float3 waveMax = WaveActiveMax(ownMax);
        if (WaveIsFirstLane())
        {
            gs_waveBoxMin[waveIndex] = waveMin;
            gs_waveBoxMax[waveIndex] = waveMax;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    float3 blockMin = gs_waveBoxMin[0];
    float3 blockMax = gs_waveBoxMax[0];
    for (uint w = 1u; w < waveCount; ++w)
    {
        blockMin = min(blockMin, gs_waveBoxMin[w]);
        blockMax = max(blockMax, gs_waveBoxMax[w]);
    }
    const bool blockEmpty = any(blockMin > blockMax); // Uniform across the group.
    const uint lightCount = blockEmpty ? 0u : min(perFrame.numLights, LIGHT_GRID_RECORD_CAPACITY);

    // ---- Two sweeps over the lights: count, then fill. Each froxel's run is its opaque lights, then the lights that reach
    // only its forward box; both in active-list order.
    uint opaqueCount = 0u;
    uint extraCount = 0u;
    uint first = 0u;
    uint opaqueStored = 0u;
    uint extraStored = 0u;
    uint dropped = 0u;
    [loop]
    for (uint sweep = 0u; sweep < 2u; ++sweep)
    {
        uint opaqueCursor = 0u;
        uint extraCursor = 0u;
        for (uint batch = 0u; batch < lightCount; batch += LIGHT_GRID_COARSE_THREADS)
        {
            // Compact the batch's lights that touch the block's box, in order.
            const uint recordIndex = batch + groupIndex;
            float4 sphere = 0.0f;
            bool survivor = false;
            if (recordIndex < lightCount)
            {
                sphere = records[recordIndex].sphere;
                survivor = LightGridSphereTouchesBox(sphere, blockMin, blockMax);
            }
            const uint wavePrefix = WavePrefixCountBits(survivor);
            const uint waveSurvivors = WaveActiveCountBits(survivor);
            if (WaveIsFirstLane())
            {
                gs_waveCount[waveIndex] = waveSurvivors;
            }
            GroupMemoryBarrierWithGroupSync();
            uint slot = wavePrefix;
            uint survivors = 0u;
            for (uint v = 0u; v < waveCount; ++v)
            {
                const uint count = gs_waveCount[v];
                slot += v < waveIndex ? count : 0u;
                survivors += count;
            }
            if (survivor)
            {
                gs_survivorSphere[slot] = sphere;
                gs_survivorRecord[slot] = recordIndex;
            }
            GroupMemoryBarrierWithGroupSync();

            if (opaqueValid || forwardValid)
            {
                for (uint s = 0u; s < survivors; ++s)
                {
                    const float4 candidate = gs_survivorSphere[s];
                    const bool opaqueHit = opaqueValid && LightGridSphereTouchesBox(candidate, opaqueMin, opaqueMax);
                    const bool extraHit = !opaqueHit && forwardValid && LightGridSphereTouchesBox(candidate, forwardMin, forwardMax);
                    if (sweep == 0u)
                    {
                        opaqueCount += opaqueHit ? 1u : 0u;
                        extraCount += extraHit ? 1u : 0u;
                    }
                    else if (opaqueHit || extraHit)
                    {
                        const uint slotInRun = opaqueHit ? opaqueCursor : opaqueStored + extraCursor;
                        const bool fits = opaqueHit ? opaqueCursor < opaqueStored : extraCursor < extraStored;
                        if (fits)
                        {
                            indices[first + slotInRun] = records[gs_survivorRecord[s]].lightIndex;
                        }
                        opaqueCursor += opaqueHit ? 1u : 0u;
                        extraCursor += extraHit ? 1u : 0u;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }

        if (sweep == 0u)
        {
            // ---- The froxel's run: one atomic per wave on the allocator. Opaque lights are kept first when the buffer or
            // the 16-bit counts run out.
            const uint wanted = opaqueCount + extraCount;
            const uint waveWanted = WaveActiveSum(wanted);
            const uint waveOffset = WavePrefixSum(wanted);
            uint waveBase = 0u;
            if (WaveIsFirstLane() && waveWanted > 0u)
            {
                InterlockedAdd(cullingStats[LIGHT_CULLING_STATS_NEEDED_LIGHTS], waveWanted, waveBase);
            }
            first = WaveReadLaneFirst(waveBase) + waveOffset;
            const uint room = first < LIGHT_GRID_INDEX_CAPACITY ? LIGHT_GRID_INDEX_CAPACITY - first : 0u;
            const uint stored = min(min(wanted, room), LIGHT_GRID_MAX_CELL_LIGHTS);
            opaqueStored = min(opaqueCount, stored);
            extraStored = stored - opaqueStored;
            dropped = wanted - stored;
            if (inGrid)
            {
                cells[cellIndex] = uint2(first, opaqueStored | (stored << LIGHT_GRID_FORWARD_SHIFT));
            }
        }
    }

    const uint waveDropped = WaveActiveSum(dropped);
    const uint waveOverflowed = WaveActiveCountBits(dropped > 0u);
    const uint waveStored = WaveActiveSum(opaqueStored + extraStored);
    if (WaveIsFirstLane())
    {
        if (waveDropped > 0u)
        {
            InterlockedAdd(cullingStats[LIGHT_CULLING_STATS_DROPPED_LIGHTS], waveDropped);
            InterlockedAdd(cullingStats[LIGHT_CULLING_STATS_OVERFLOWED_CLUSTERS], waveOverflowed);
        }
        if (waveStored > 0u)
        {
            InterlockedAdd(cullingStats[LIGHT_CULLING_STATS_STORED_LIGHTS], waveStored);
        }
    }
}
