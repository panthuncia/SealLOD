#ifndef __LIGHT_GRID_HLSLI__
#define __LIGHT_GRID_HLSLI__

// The light grid (docs/lighting.md "The light grid"): the primary camera's view split into froxels, each holding a
// contiguous run of light buffer indices in Builtin::Light::GridIndices. lightCulling.hlsl builds it every frame.
//
// A froxel is a tile of perFrame.lightGridTileSize pixels square by one depth slice: lightClusterGridSizeZ slices,
// LIGHT_GRID_NEAR_SLICES uniform ones up to perFrame.clusterZSplitDepth and logarithmic ones beyond to the camera's zFar.
//
// Builtin::Light::GridCells holds one uint2 per froxel: x the first index, y two counts. The opaque count (low 16 bits)
// covers the lights that reach the froxel's occupied depth, the span between its nearest and farthest opaque pixel; only
// the primary surface's pixels may read it (deferred shading, stochastic lighting). The forward count (high 16 bits)
// covers the whole froxel in front of the tile's farthest opaque pixel, and its first entries are the opaque ones; forward
// passes, whose fragments lie anywhere in front of the opaque surface, read it by defining LIGHT_GRID_FORWARD 1 before
// including this file.

#include "include/structs.hlsli"

#ifndef LIGHT_GRID_NEAR_SLICES
#define LIGHT_GRID_NEAR_SLICES 4u
#endif
#ifndef LIGHT_GRID_FORWARD
#define LIGHT_GRID_FORWARD 0
#endif
#define LIGHT_GRID_COUNT_MASK 0xFFFFu
#define LIGHT_GRID_FORWARD_SHIFT 16u
#define LIGHT_GRID_MAX_CELL_LIGHTS 0xFFFFu

uint LightGridCellCount(PerFrameBuffer perFrame)
{
    return perFrame.lightClusterGridSizeX * perFrame.lightClusterGridSizeY * perFrame.lightClusterGridSizeZ;
}

uint2 LightGridTile(uint2 pixel, PerFrameBuffer perFrame)
{
    const uint tileSize = max(perFrame.lightGridTileSize, 1u);
    return min(pixel / tileSize, uint2(perFrame.lightClusterGridSizeX, perFrame.lightClusterGridSizeY) - 1u);
}

// Where view depth `depth` (positive, toward the scene) falls in the slices, as a continuous coordinate: its integer part is
// the slice.
float LightGridSliceCoordinate(float depth, PerFrameBuffer perFrame, float cameraNear, float cameraFar)
{
    const uint totalZ = max(perFrame.lightClusterGridSizeZ, 1u);
    const float zNear = max(cameraNear, 1.0e-5f);
    const float z = max(depth, zNear);
    const uint nearSlices = min(LIGHT_GRID_NEAR_SLICES, totalZ);
    const float zSplit = max(perFrame.clusterZSplitDepth, zNear + 1.0e-4f);
    const float zFar = max(cameraFar, zSplit + 1.0e-4f);
    if (nearSlices > 0u && z < zSplit)
    {
        return saturate((z - zNear) / max(zSplit - zNear, 1.0e-5f)) * float(nearSlices);
    }
    const float logStart = log(zSplit / zNear);
    const float logEnd = log(zFar / zNear);
    const float u = saturate((log(z / zNear) - logStart) / max(logEnd - logStart, 1.0e-5f));
    return float(nearSlices) + u * float(totalZ - nearSlices);
}

uint LightGridSliceFromCoordinate(float coordinate, PerFrameBuffer perFrame)
{
    return min(uint(coordinate), max(perFrame.lightClusterGridSizeZ, 1u) - 1u);
}

// The depth slice holding view depth `depth`.
uint LightGridSlice(float depth, PerFrameBuffer perFrame, float cameraNear, float cameraFar)
{
    return LightGridSliceFromCoordinate(LightGridSliceCoordinate(depth, perFrame, cameraNear, cameraFar), perFrame);
}

// The view depth of slice boundary `boundary` (0 .. lightClusterGridSizeZ), the inverse of LightGridSlice.
float LightGridSliceBoundary(uint boundary, PerFrameBuffer perFrame, float cameraNear, float cameraFar)
{
    const uint totalZ = max(perFrame.lightClusterGridSizeZ, 1u);
    const float zNear = max(cameraNear, 1.0e-5f);
    const uint nearSlices = min(LIGHT_GRID_NEAR_SLICES, totalZ);
    const float zSplit = max(perFrame.clusterZSplitDepth, zNear + 1.0e-4f);
    const float zFar = max(cameraFar, zSplit + 1.0e-4f);
    if (boundary <= nearSlices && nearSlices > 0u)
    {
        return zNear + (zSplit - zNear) * float(boundary) / float(nearSlices);
    }
    const float logStart = log(zSplit / zNear);
    const float logEnd = log(zFar / zNear);
    const float t = float(boundary - nearSlices) / float(max(totalZ - nearSlices, 1u));
    return zNear * exp(logStart + t * (logEnd - logStart));
}

uint LightGridCellIndex(uint3 cell, PerFrameBuffer perFrame)
{
    return cell.x + perFrame.lightClusterGridSizeX * (cell.y + perFrame.lightClusterGridSizeY * cell.z);
}

// The froxel of a pixel at view-space z `viewZ` (negative in front of the camera, as fragPosViewSpace.z).
uint3 LightGridCell(float2 pixelCoords, float viewZ, PerFrameBuffer perFrame, Camera camera)
{
    const uint2 tile = LightGridTile(uint2(pixelCoords), perFrame);
    return uint3(tile, LightGridSlice(abs(viewZ), perFrame, camera.zNear, camera.zFar));
}

// A froxel's lights: indices first .. first + count - 1 of Builtin::Light::GridIndices, read with LightGridLight.
struct LightGridRange
{
    uint first;
    uint count;
    uint slice; // The froxel's depth slice (the light cluster debug output).
};

LightGridRange LightGridLoadRange(float2 pixelCoords, float viewZ, PerFrameBuffer perFrame, Camera camera)
{
    StructuredBuffer<uint2> cells = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridCells)];
    const uint3 cell = LightGridCell(pixelCoords, viewZ, perFrame, camera);
    const uint2 packed = cells[LightGridCellIndex(cell, perFrame)];
    LightGridRange range;
    range.first = packed.x;
#if LIGHT_GRID_FORWARD
    range.count = packed.y >> LIGHT_GRID_FORWARD_SHIFT;
#else
    range.count = packed.y & LIGHT_GRID_COUNT_MASK;
#endif
    range.slice = cell.z;
    return range;
}

// Entry i of a froxel's run: a light buffer index.
uint LightGridLight(LightGridRange range, uint i)
{
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridIndices)];
    return indices[range.first + i];
}

#endif // __LIGHT_GRID_HLSLI__
