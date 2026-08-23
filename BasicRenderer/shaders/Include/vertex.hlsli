#ifndef __VERTEX_HLSL__
#define __VERTEX_HLSL__

#include "include/vertexFlags.hlsli"
#include "include/loadingUtils.hlsli"
#include "include/vertexLayout.hlsli"

// Manually assembled from ByteAddressBuffer
struct SkinningInfluences
{
    uint4 joints0;
    uint4 joints1;
    float4 weights0;
    float4 weights1;
};

struct Vertex {
    float3 position;
    float3 normal;
    float4 tangent;
    float2 texcoord;
    SkinningInfluences skinning;
    float3 color;
};

// Per-object flags (mirrors OBJECT_FLAG_* in ShaderBuffers.h)
#define OBJECT_FLAG_REVERSE_WINDING (1u << 0)

Vertex LoadVertex(uint byteOffset, ByteAddressBuffer buffer, uint flags) {
    Vertex vertex;

    // Load position (float3, 12 bytes)
    vertex.position = LoadFloat3(byteOffset, buffer);
    byteOffset += VERTEX_LAYOUT_POSITION_SIZE;

    // Load normal (float3, 12 bytes)
    vertex.normal = LoadFloat3(byteOffset, buffer);
    byteOffset += VERTEX_LAYOUT_NORMAL_SIZE;

    vertex.skinning.joints0 = uint4(0, 0, 0, 0);
    vertex.skinning.joints1 = uint4(0, 0, 0, 0);
    vertex.skinning.weights0 = float4(0.0, 0.0, 0.0, 0.0);
    vertex.skinning.weights1 = float4(0.0, 0.0, 0.0, 0.0);

    vertex.tangent = float4(1.0, 0.0, 0.0, 0.0);
    if (flags & VERTEX_TANGENTS) {
        vertex.tangent = LoadFloat4(byteOffset, buffer);
        byteOffset += VERTEX_LAYOUT_TANGENT_SIZE;
    }

    if (flags & VERTEX_TEXCOORDS) {
        vertex.texcoord = LoadFloat2(byteOffset, buffer);
        byteOffset += VERTEX_LAYOUT_TEXCOORD_SIZE;
    }
    else
    {
        vertex.texcoord = float2(0.0, 0.0);
    }

    vertex.color = float3(1.0, 1.0, 1.0);
    if (flags & VERTEX_COLORS) {
        vertex.color = LoadFloat3(byteOffset, buffer);
    }

    return vertex;
}

Vertex LoadSkinningVertex(uint byteOffset, ByteAddressBuffer buffer, uint flags)
{
    Vertex vertex;

    // Load position (float3, 12 bytes)
    vertex.position = LoadFloat3(byteOffset, buffer);
    byteOffset += 12;

    // Load normal (float3, 12 bytes)
    vertex.normal = LoadFloat3(byteOffset, buffer);
    byteOffset += 12;

    vertex.texcoord = float2(0.0, 0.0);
    vertex.tangent = float4(1.0, 0.0, 0.0, 0.0);
    vertex.skinning.joints0 = uint4(0, 0, 0, 0);
    vertex.skinning.joints1 = uint4(0, 0, 0, 0);
    vertex.skinning.weights0 = float4(0.0, 0.0, 0.0, 0.0);
    vertex.skinning.weights1 = float4(0.0, 0.0, 0.0, 0.0);

    if (flags & VERTEX_SKINNED) {
        // Load joints (uint4x2, 32 bytes)
        vertex.skinning.joints0 = LoadUint4(byteOffset, buffer);
        byteOffset += 16;
        vertex.skinning.joints1 = LoadUint4(byteOffset, buffer);
        byteOffset += 16;

        // Load weights (float4x2, 32 bytes)
        vertex.skinning.weights0 = LoadFloat4(byteOffset, buffer);
        byteOffset += 16;
        vertex.skinning.weights1 = LoadFloat4(byteOffset, buffer);
        byteOffset += 16;
    }
    
    vertex.color = float3(1.0, 1.0, 1.0);

    return vertex;
}

#endif // __VERTEX_HLSL__
