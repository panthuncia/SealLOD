#ifndef HAIR_SHADING_HLSLI
#define HAIR_SHADING_HLSLI

// What the layered path's rasterizer, shade passes and composite share: how the vertices to light are marked, and how a
// lit colour is stored per strand vertex. The fibre's response to light is in hairBsdf.hlsli.

#include "HairShaders/hairStructs.hlsli"

// The rasterizer marks the pool rows the shade pass has to light: the two vertices of every segment that offered a key
// to a pixel's layers this frame. A mark is the frame's number in the row's free half, where a colour has zero, so a row
// lit in an earlier frame is not taken for marked (but for the one frame 65,536 frames later, which only lights it again).
// The shade-list pass (hairShadeList.hlsl) gathers the marked rows into a list, and the shade pass lights that list.
uint2 HairShadeMark(uint frameNumber)
{
    return uint2(0u, (frameNumber & 0xFFFFu) << 16u);
}

bool HairIsMarkedForShading(uint2 packed, uint frameNumber)
{
    return (packed.y >> 16u) == (frameNumber & 0xFFFFu);
}

// A lit colour in a row of the shading buffer: three half floats. The fourth half is zero.
uint2 HairPackColor(float3 color)
{
    const uint3 halves = f32tof16(min(color, 65000.0f.xxx));
    return uint2(halves.x | (halves.y << 16u), halves.z);
}

float3 HairUnpackColor(uint2 packed)
{
    return f16tof32(uint3(packed.x & 0xFFFFu, packed.x >> 16u, packed.y & 0xFFFFu));
}

#endif // HAIR_SHADING_HLSLI
