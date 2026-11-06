// The preprocessor's opacity micromap bake (src/RendererHost/Preprocessing/OpacityMicromapBake.cpp): one mesh's
// alpha-tested clusters classified against the test mip of their material's alpha textures, as the path tracer's any-hit
// tests them (clodRayTracingAlpha.hlsli). It runs on tesworld's GPU context (tesworld/GpuCompute.h), not in the renderer:
// push constants at b0 and everything else through the descriptor heaps, at the indices the caller passes.
//
//   CLodOmmUvsOfflineCS:    one thread group per cluster, a thread per triangle. Writes each triangle's corner UVs, as
//                           any-hit decodes them, for the CPU to choose subdivision levels by (meshopt_opacityMapMeasure).
//   CLodOmmBakeOfflineCS:   one thread group per micromap entry (its UVs and level). Writes the entry's 4-state micromap,
//                           in bird-curve order, at its word offset: compaction is the CPU's (meshopt_opacityMapCompact).
//   CLodOmmVerifyOfflineCS: one thread group per entry. Classifies it as the bake does, then checks every micro-triangle
//                           called opaque or transparent against the sampler at many points, as any-hit samples: the
//                           classification must never disagree with it. Writes mismatches and points checked per entry.
//
// Compiled by plain DXC, without the renderer's shader preprocessor, so the renderer's named resources
// (ResourceDescriptorIndex(Builtin::...)) aren't bound here: the macro below lets the shared headers parse, and nothing
// these kernels call uses one. The DXIL's only constant buffer is b0.
#define ResourceDescriptorIndex(name) 0xFFFFFFFFu
#define OptionalResourceDescriptorIndex(name) 0xFFFFFFFFu
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodClusterFormat.hlsli"
#include "include/clodOpacityMicromap.hlsli"

struct CLodOmmOfflineConstants
{
    uint input;     // ByteAddressBuffer SRV: the UV pass's pages; the bake's and verification's entries.
    uint records;   // ByteAddressBuffer SRV: the UV pass's clusters; the bake's and verification's material.
    uint output;    // RWByteAddressBuffer UAV: UVs, micromap words, or verification counts.
    uint count;     // Clusters or entries in this dispatch.
    uint first;     // This dispatch's first cluster or entry.
    uint padding2;
    uint padding0;
    uint padding1;
};

cbuffer CLodOmmOfflinePush : register(b0)
{
    CLodOmmOfflineConstants g_bake;
};

// The UV pass's cluster: 16 bytes.
struct CLodOmmOfflineCluster
{
    uint pageByteOffset; // In the pages buffer.
    uint localMeshlet;
    uint triangleCount;  // The cluster's UV slots: lanes past it belong to the next cluster's.
    uint firstTriangle;  // UV slot of the cluster's first triangle.
};

// A micromap to classify: 32 bytes.
struct CLodOmmOfflineEntry
{
    float2 uv0, uv1, uv2;
    uint level;      // 0 to CLOD_OMM_MAX_LEVEL.
    uint wordOffset; // In the output: the entry's micromap words.
};

#define CLOD_OMM_OFFLINE_HAS_BASE 1u
#define CLOD_OMM_OFFLINE_HAS_OPACITY 2u

// The material: 32 bytes.
struct CLodOmmOfflineMaterial
{
    uint baseTexture;    // Texture2D SRV holding only the test mip.
    uint baseSampler;    // SamplerDescriptorHeap index, the material's address modes.
    uint opacityTexture;
    uint opacitySampler;
    float factor;        // baseColorFactor.a
    float cutoff;
    uint flags;          // CLOD_OMM_OFFLINE_HAS_*
    uint padding;
};

// A triangle the UV pass couldn't decode: its micromap is left to any-hit.
#define CLOD_OMM_OFFLINE_INVALID_UV asfloat(0x7FC00000u)

CLodRtOmmTexture CLodOmmOfflineTexture(bool present, uint textureIndex, uint samplerIndex)
{
    CLodRtOmmTexture texture = (CLodRtOmmTexture)0;
    texture.present = present;
    texture.atTestMip = present;
    if (!present) {
        return texture;
    }
    texture.descriptor = textureIndex;
    texture.samplerIndex = samplerIndex;
    texture.mip = 0u;
    Texture2D<float4> bound = ResourceDescriptorHeap[NonUniformResourceIndex(textureIndex)];
    uint levels = 1u;
    bound.GetDimensions(0u, texture.width, texture.height, levels);
    return texture;
}

CLodOmmOfflineMaterial CLodOmmLoadOfflineMaterial()
{
    ByteAddressBuffer materials = ResourceDescriptorHeap[g_bake.records];
    const uint4 words0 = materials.Load4(0u);
    const uint4 words1 = materials.Load4(16u);
    CLodOmmOfflineMaterial material;
    material.baseTexture = words0.x;
    material.baseSampler = words0.y;
    material.opacityTexture = words0.z;
    material.opacitySampler = words0.w;
    material.factor = asfloat(words1.x);
    material.cutoff = asfloat(words1.y);
    material.flags = words1.z;
    material.padding = 0u;
    return material;
}

CLodOmmOfflineEntry CLodOmmLoadOfflineEntry(uint index)
{
    ByteAddressBuffer entries = ResourceDescriptorHeap[g_bake.input];
    const uint4 words0 = entries.Load4(index * 32u);
    const uint4 words1 = entries.Load4(index * 32u + 16u);
    CLodOmmOfflineEntry entry;
    entry.uv0 = asfloat(words0.xy);
    entry.uv1 = asfloat(words0.zw);
    entry.uv2 = asfloat(words1.xy);
    entry.level = min(words1.z, CLOD_OMM_MAX_LEVEL);
    entry.wordOffset = words1.w;
    return entry;
}

[numthreads(128, 1, 1)]
void CLodOmmUvsOfflineCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (group.x >= g_bake.count) {
        return;
    }
    ByteAddressBuffer clusters = ResourceDescriptorHeap[g_bake.records];
    const uint4 clusterWords = clusters.Load4((g_bake.first + group.x) * 16u);
    CLodOmmOfflineCluster cluster;
    cluster.pageByteOffset = clusterWords.x;
    cluster.localMeshlet = clusterWords.y;
    cluster.triangleCount = clusterWords.z;
    cluster.firstTriangle = clusterWords.w;
    if (lane >= cluster.triangleCount) {
        return;
    }

    RWByteAddressBuffer output = ResourceDescriptorHeap[g_bake.output];
    const CLodPageHeader header = LoadPageHeader(g_bake.input, cluster.pageByteOffset);
    const bool pageOk = header.formatAndKind == CLOD_TRIANGLE_PAGE_MAGIC && cluster.localMeshlet < header.meshletCount && header.uvSetCount != 0u;
    // The CPU asks for exactly the cluster's triangles; one the page doesn't have is marked so it never gets a micromap.
    float2 uv0 = CLOD_OMM_OFFLINE_INVALID_UV.xx, uv1 = uv0, uv2 = uv0;
    if (pageOk) {
        const CLodMeshletDescriptor descriptor = LoadMeshletDescriptor(g_bake.input, cluster.pageByteOffset, header.descriptorOffset, cluster.localMeshlet);
        if (lane < CLodDescTriangleCount(descriptor)) {
            CLodOmmLoadTriangleUvs(g_bake.input, cluster.pageByteOffset, header, cluster.localMeshlet, descriptor, lane, uv0, uv1, uv2);
        }
    }
    const uint address = (cluster.firstTriangle + lane) * 24u;
    output.Store2(address, asuint(uv0));
    output.Store2(address + 8u, asuint(uv1));
    output.Store2(address + 16u, asuint(uv2));
}

[numthreads(64, 1, 1)]
void CLodOmmBakeOfflineCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (group.x >= g_bake.count) {
        return;
    }
    const CLodOmmOfflineEntry entry = CLodOmmLoadOfflineEntry(g_bake.first + group.x);
    const CLodOmmOfflineMaterial material = CLodOmmLoadOfflineMaterial();
    const CLodRtOmmTexture baseTexture = CLodOmmOfflineTexture((material.flags & CLOD_OMM_OFFLINE_HAS_BASE) != 0u, material.baseTexture, material.baseSampler);
    const CLodRtOmmTexture opacityTexture = CLodOmmOfflineTexture((material.flags & CLOD_OMM_OFFLINE_HAS_OPACITY) != 0u, material.opacityTexture, material.opacitySampler);
    CLodOmmClassifyTriangle(lane, entry.uv0, entry.uv1, entry.uv2, entry.level, baseTexture, opacityTexture, material.factor, material.cutoff);

    RWByteAddressBuffer output = ResourceDescriptorHeap[g_bake.output];
    const uint stateWords = max((1u << (2u * entry.level)) / 16u, 1u);
    for (uint i = lane; i < stateWords; i += 64u) {
        output.Store((entry.wordOffset + i) * 4u, g_ommStates[i]);
    }
}

groupshared uint g_ommVerifyMismatches;
groupshared uint g_ommVerifyChecked;

// Stratified points of a micro-triangle (its barycentrics a, b, c), corners and edge midpoints first.
float2 CLodOmmVerifyPoint(uint i, float2 a, float2 b, float2 c)
{
    if (i < 3u) {
        return i == 0u ? a : (i == 1u ? b : c);
    }
    if (i < 6u) {
        return i == 3u ? (a + b) * 0.5f : (i == 4u ? (b + c) * 0.5f : (c + a) * 0.5f);
    }
    // A 9x9 grid folded into the triangle, nudged off the grid lines.
    const uint j = i - 6u;
    float s = (float(j % 9u) + 0.37f) / 9.0f;
    float t = (float(j / 9u) + 0.61f) / 9.0f;
    if (s + t > 1.0f) {
        s = 1.0f - s;
        t = 1.0f - t;
    }
    return a + (b - a) * s + (c - a) * t;
}

#define CLOD_OMM_VERIFY_POINTS 87u

[numthreads(64, 1, 1)]
void CLodOmmVerifyOfflineCS(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (group.x >= g_bake.count) {
        return;
    }
    if (lane == 0u) {
        g_ommVerifyMismatches = 0u;
        g_ommVerifyChecked = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    const CLodOmmOfflineEntry entry = CLodOmmLoadOfflineEntry(g_bake.first + group.x);
    const CLodOmmOfflineMaterial material = CLodOmmLoadOfflineMaterial();
    const CLodRtOmmTexture baseTexture = CLodOmmOfflineTexture((material.flags & CLOD_OMM_OFFLINE_HAS_BASE) != 0u, material.baseTexture, material.baseSampler);
    const CLodRtOmmTexture opacityTexture = CLodOmmOfflineTexture((material.flags & CLOD_OMM_OFFLINE_HAS_OPACITY) != 0u, material.opacityTexture, material.opacitySampler);
    // The bake's classification again (it's deterministic), so this needs nothing from the bake's dispatch.
    CLodOmmClassifyTriangle(lane, entry.uv0, entry.uv1, entry.uv2, entry.level, baseTexture, opacityTexture, material.factor, material.cutoff);

    uint mismatches = 0u;
    uint checked = 0u;
    const uint microCount = 1u << (2u * entry.level);
    for (uint k = lane; k < microCount; k += 64u) {
        float2 a, b, c;
        CLodRtOmmMicroTriangle(k, entry.level, a, b, c);
        const float2 microCenter = (a + b + c) / 3.0f;
        const uint index = CLodRtOmmBarycentricsToIndex(microCenter.x, microCenter.y, entry.level);
        const uint state = (g_ommStates[index >> 4u] >> ((index & 15u) * 2u)) & 3u;
        if (state >= OMM_STATE_UNKNOWN_TRANSPARENT) {
            continue;
        }
        for (uint i = 0u; i < CLOD_OMM_VERIFY_POINTS; ++i) {
            const float2 p = CLodOmmVerifyPoint(i, a, b, c);
            const float2 uv = entry.uv0 * (1.0f - p.x - p.y) + entry.uv1 * p.x + entry.uv2 * p.y;
            const float alpha = material.factor * CLodRtOmmSampleAlpha(baseTexture, uv) * CLodRtOmmSampleAlpha(opacityTexture, uv);
            const bool opaque = CLodRtAlphaIsOpaque(alpha, material.cutoff);
            mismatches += (opaque != (state == OMM_STATE_OPAQUE)) ? 1u : 0u;
            ++checked;
        }
    }
    InterlockedAdd(g_ommVerifyMismatches, mismatches);
    InterlockedAdd(g_ommVerifyChecked, checked);
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        RWByteAddressBuffer output = ResourceDescriptorHeap[g_bake.output];
        output.Store2((g_bake.first + group.x) * 8u, uint2(g_ommVerifyMismatches, g_ommVerifyChecked));
    }
}
