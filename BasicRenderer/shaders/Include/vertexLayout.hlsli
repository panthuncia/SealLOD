#ifndef VERTEX_LAYOUT_HLSLI
#define VERTEX_LAYOUT_HLSLI

#include "include/vertexFlags.hlsli"

static const uint VERTEX_LAYOUT_POSITION_OFFSET = 0u;
static const uint VERTEX_LAYOUT_POSITION_SIZE = 12u;
static const uint VERTEX_LAYOUT_NORMAL_OFFSET = VERTEX_LAYOUT_POSITION_OFFSET + VERTEX_LAYOUT_POSITION_SIZE;
static const uint VERTEX_LAYOUT_NORMAL_SIZE = 12u;
static const uint VERTEX_LAYOUT_BASE_VERTEX_SIZE = VERTEX_LAYOUT_NORMAL_OFFSET + VERTEX_LAYOUT_NORMAL_SIZE;
static const uint VERTEX_LAYOUT_TANGENT_SIZE = 16u;
static const uint VERTEX_LAYOUT_TEXCOORD_SIZE = 8u;
static const uint VERTEX_LAYOUT_COLOR_SIZE = 12u;

bool VertexLayoutHasTexcoords(uint flags)
{
    return (flags & VERTEX_TEXCOORDS) != 0u;
}

bool VertexLayoutHasColors(uint flags)
{
    return (flags & VERTEX_COLORS) != 0u;
}

bool VertexLayoutHasTangents(uint flags)
{
    return (flags & VERTEX_TANGENTS) != 0u;
}

uint VertexLayoutTangentOffset(uint flags)
{
    (void)flags;
    return VERTEX_LAYOUT_BASE_VERTEX_SIZE;
}

uint VertexLayoutTexcoordOffset(uint flags)
{
    return VERTEX_LAYOUT_BASE_VERTEX_SIZE + (VertexLayoutHasTangents(flags) ? VERTEX_LAYOUT_TANGENT_SIZE : 0u);
}

uint VertexLayoutColorOffset(uint flags)
{
    return VERTEX_LAYOUT_BASE_VERTEX_SIZE + (VertexLayoutHasTexcoords(flags) ? VERTEX_LAYOUT_TEXCOORD_SIZE : 0u);
}

uint VertexLayoutVertexSize(uint flags)
{
    return VERTEX_LAYOUT_BASE_VERTEX_SIZE
        + (VertexLayoutHasTangents(flags) ? VERTEX_LAYOUT_TANGENT_SIZE : 0u)
        + (VertexLayoutHasTexcoords(flags) ? VERTEX_LAYOUT_TEXCOORD_SIZE : 0u)
        + (VertexLayoutHasColors(flags) ? VERTEX_LAYOUT_COLOR_SIZE : 0u);
}

#endif // VERTEX_LAYOUT_HLSLI
