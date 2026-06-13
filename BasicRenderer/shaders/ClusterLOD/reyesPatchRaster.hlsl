#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/vertexFlags.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodStructs.hlsli"
#include "include/reyesPatchCommon.hlsli"
#include "include/visibilityPacking.hlsli"
#include "PerPassRootConstants/clodReyesPatchRasterRootConstants.h"

static const uint REYES_PATCH_RASTER_GROUP_SIZE = 64u;
static const float REYES_PATCH_RASTER_TINY_TRIANGLE_AREA_EPSILON = 1e-8f;

#define REYES_PATCH_RASTER_ENABLE_NEAR_PLANE_CLIPPING 1 // TODO: Looks nicer, but might be more expensive. Maybe have a separate dispatch for these?

#ifndef REYES_PATCH_RASTER_ENABLE_NEAR_PLANE_CLIPPING
#define REYES_PATCH_RASTER_ENABLE_NEAR_PLANE_CLIPPING 0
#endif

#ifndef REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
#define REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK 0
#endif

#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
static const float REYES_PATCH_RASTER_TINY_TRIANGLE_MAX_EXTENT = 1.5f;
#endif

uint ReadPackedBits32(ByteAddressBuffer buf, uint startBit, uint bitCount)
{
    if (bitCount == 0u) return 0u;

    uint wordIndex = startBit >> 5;
    uint bitOffset = startBit & 31u;
    uint packed = buf.Load(wordIndex * 4u) >> bitOffset;
    if (bitOffset + bitCount > 32u)
    {
        packed |= buf.Load((wordIndex + 1u) * 4u) << (32u - bitOffset);
    }

    uint mask = (bitCount >= 32u) ? 0xffffffffu : ((1u << bitCount) - 1u);
    return packed & mask;
}

float3 DecodeCompressedPosition(
    uint meshletLocalVertex,
    uint positionBitstreamBase,
    uint positionBitOffset,
    uint bitsX,
    uint bitsY,
    uint bitsZ,
    uint quantExp,
    int3 minQ,
    uint pagePoolSlabDescriptorIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    return CLodLoadPagePosition(slab, quantExp, positionBitstreamBase, positionBitOffset, meshletLocalVertex);
}

uint3 DecodeTriangle(ByteAddressBuffer slab, uint triStreamBase, uint triByteOffset, uint triLocalIndex)
{
    uint triOffset = triStreamBase + triByteOffset + triLocalIndex * 3u;
    uint alignedOffset = (triOffset / 4u) * 4u;
    uint firstWord = slab.Load(alignedOffset);
    uint byteOffset = triOffset % 4u;

    uint b0 = (firstWord >> (byteOffset * 8u)) & 0xFFu;
    uint b1;
    uint b2;

    if (byteOffset <= 1u)
    {
        b1 = (firstWord >> ((byteOffset + 1u) * 8u)) & 0xFFu;
        b2 = (firstWord >> ((byteOffset + 2u) * 8u)) & 0xFFu;
    }
    else if (byteOffset == 2u)
    {
        b1 = (firstWord >> 24u) & 0xFFu;
        uint secondWord = slab.Load(alignedOffset + 4u);
        b2 = secondWord & 0xFFu;
    }
    else
    {
        uint secondWord = slab.Load(alignedOffset + 4u);
        b1 = secondWord & 0xFFu;
        b2 = (secondWord >> 8u) & 0xFFu;
    }

    return uint3(b0, b1, b2);
}

SkinningInfluences DecodePackedJoints(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex)
{
    SkinningInfluences skinning = (SkinningInfluences)0;
    if ((hdr.attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    uint addr = pageByteOffset + hdr.jointArrayOffset + (desc.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.joints0 = LoadUint4(addr, slab);
    skinning.joints1 = LoadUint4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences DecodePackedWeights(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex,
    SkinningInfluences skinning)
{
    if ((hdr.attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    uint addr = pageByteOffset + hdr.weightArrayOffset + (desc.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.weights0 = LoadFloat4(addr, slab);
    skinning.weights1 = LoadFloat4(addr + 16u, slab);
    return skinning;
}

float3 DecodeSkinnedPosition(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex,
    uint vertexFlags,
    uint skinningInstanceSlot)
{
    float3 localPos = DecodeCompressedPosition(
        meshletLocalVertex,
        pageByteOffset + hdr.positionBitstreamOffset,
        desc.positionBitOffset,
        CLodDescBitsX(desc),
        CLodDescBitsY(desc),
        CLodDescBitsZ(desc),
        hdr.compressedPositionQuantExp,
        int3(desc.minQx, desc.minQy, desc.minQz),
        pagePoolSlabDescriptorIndex);

    if ((vertexFlags & VERTEX_SKINNED) != 0u)
    {
        SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, hdr, desc, pageByteOffset, pagePoolSlabDescriptorIndex);
        skinning = DecodePackedWeights(meshletLocalVertex, hdr, desc, pageByteOffset, pagePoolSlabDescriptorIndex, skinning);
        localPos = mul(float4(localPos, 1.0f), BuildSkinMatrix(skinningInstanceSlot, skinning)).xyz;
    }

    return localPos;
}

float2 UnpackSnorm16x2(uint packed)
{
    int signedPacked = asint(packed);
    int x = (signedPacked << 16) >> 16;
    int y = signedPacked >> 16;
    float sx = max(-1.0f, (float)x / 32767.0f);
    float sy = max(-1.0f, (float)y / 32767.0f);
    return float2(sx, sy);
}

float3 OctDecodeNormal(float2 e)
{
    float3 v = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f)
    {
        float2 folded = (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
        v.x = folded.x;
        v.y = folded.y;
    }
    return normalize(v);
}

float3 DecodeCompressedNormal(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    uint addr = pageByteOffset + hdr.normalArrayOffset + (desc.vertexAttributeOffset + meshletLocalVertex) * 4u;
    uint packed = slab.Load(addr);
    return OctDecodeNormal(UnpackSnorm16x2(packed));
}

float3 DecodeSkinnedNormal(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex,
    uint vertexFlags,
    uint skinningInstanceSlot)
{
    float3 localNormal = DecodeCompressedNormal(meshletLocalVertex, hdr, desc, pageByteOffset, pagePoolSlabDescriptorIndex);

    if ((vertexFlags & VERTEX_SKINNED) != 0u)
    {
        SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, hdr, desc, pageByteOffset, pagePoolSlabDescriptorIndex);
        skinning = DecodePackedWeights(meshletLocalVertex, hdr, desc, pageByteOffset, pagePoolSlabDescriptorIndex, skinning);
        localNormal = mul(localNormal, (float3x3)BuildSkinMatrix(skinningInstanceSlot, skinning));
    }

    return normalize(localNormal);
}

float2 DecodeCompressedUV(
    uint meshletLocalVertex,
    uint uvSetIndex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint localMeshletIndex,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex)
{
    CLodMeshletUvDescriptor uvDesc = LoadMeshletUvDescriptor(
        pagePoolSlabDescriptorIndex,
        pageByteOffset,
        hdr.uvDescriptorOffset,
        hdr.uvSetCount,
        localMeshletIndex,
        uvSetIndex);

    uint uvBitstreamBase = pageByteOffset + LoadPageUvBitstreamOffset(
        pagePoolSlabDescriptorIndex,
        pageByteOffset,
        hdr.uvBitstreamDirectoryOffset,
        uvSetIndex);

    uint bitsU = CLodUvDescBitsU(uvDesc);
    uint bitsV = CLodUvDescBitsV(uvDesc);
    uint bitsPerVertex = bitsU + bitsV;
    uint bitCursor = uvBitstreamBase * 8u + uvDesc.uvBitOffset + meshletLocalVertex * bitsPerVertex;
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    uint encodedU = ReadPackedBits32(slab, bitCursor, bitsU);
    bitCursor += bitsU;
    uint encodedV = ReadPackedBits32(slab, bitCursor, bitsV);

    return float2(
        uvDesc.uvMinU + float(encodedU) * uvDesc.uvScaleU,
        uvDesc.uvMinV + float(encodedV) * uvDesc.uvScaleV);
}

#if REYES_PATCH_RASTER_ENABLE_NEAR_PLANE_CLIPPING
struct ReyesRasterVertex
{
    float4 clip;
    float depth;
};
#endif

#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
// Extremely small projected micro-triangles can numerically collapse to zero area or an empty
// pixel bounds even when the patch still covers a real sample. In that case we conservatively
// stamp the tiny screen-space extent, or its centroid as a last resort, so those subpixel hits
// survive rasterization instead of disappearing as view-dependent pinholes.
bool ReyesTryRasterizeTinyProjectedMicroTriangle(
    RWTexture2D<uint64_t> visBuffer,
    uint2 visDims,
    ClodViewRasterInfo viewRasterInfo,
    float2 s0,
    float2 s1,
    float2 s2,
    float depth0,
    float depth1,
    float depth2,
    uint patchVisibilityIndex,
    uint microTriangleIndex)
{
    const float2 bbMinF = min(min(s0, s1), s2);
    const float2 bbMaxF = max(max(s0, s1), s2);
    const float2 bbExtent = bbMaxF - bbMinF;
    if (bbExtent.x > REYES_PATCH_RASTER_TINY_TRIANGLE_MAX_EXTENT ||
        bbExtent.y > REYES_PATCH_RASTER_TINY_TRIANGLE_MAX_EXTENT)
    {
        return false;
    }

    const float fallbackDepth = min(depth0, min(depth1, depth2));
    const uint64_t visKey = PackVisKey(fallbackDepth, patchVisibilityIndex, microTriangleIndex);

    int2 minPx = int2(floor(bbMinF));
    int2 maxPx = int2(floor(bbMaxF));
    minPx = max(minPx, int2(viewRasterInfo.scissorMinX, viewRasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(viewRasterInfo.scissorMaxX) - 1, int(viewRasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(visDims.x) - 1, int(visDims.y) - 1));

    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
        const float2 centroidPosition = (s0 + s1 + s2) * (1.0f / 3.0f);
        const int2 centroidPx = int2(floor(centroidPosition));
        if (centroidPx.x < int(viewRasterInfo.scissorMinX) || centroidPx.y < int(viewRasterInfo.scissorMinY) ||
            centroidPx.x >= int(viewRasterInfo.scissorMaxX) || centroidPx.y >= int(viewRasterInfo.scissorMaxY) ||
            centroidPx.x < 0 || centroidPx.y < 0 ||
            centroidPx.x >= int(visDims.x) || centroidPx.y >= int(visDims.y))
        {
            return false;
        }

        InterlockedMin(visBuffer[uint2(centroidPx)], visKey);
        return true;
    }

    [loop]
    for (int py = minPx.y; py <= maxPx.y; ++py)
    {
        [loop]
        for (int px = minPx.x; px <= maxPx.x; ++px)
        {
            InterlockedMin(visBuffer[uint2(px, py)], visKey);
        }
    }

    return true;
}
#endif

void ReyesRasterizeProjectedMicroTriangle(
    RWTexture2D<uint64_t> visBuffer,
    uint2 visDims,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer,
    ClodViewRasterInfo viewRasterInfo,
    float4 clip0,
    float4 clip1,
    float4 clip2,
    float depth0,
    float depth1,
    float depth2,
    uint patchVisibilityIndex,
    uint microTriangleIndex)
{
    const float clipWEpsilon = 1e-6f;
    if (clip0.w <= clipWEpsilon || clip1.w <= clipWEpsilon || clip2.w <= clipWEpsilon)
    {
        InterlockedAdd(telemetryBuffer[0].rasterClipCullCount, 1u);
        return;
    }

    const float2 ndc0 = clip0.xy / clip0.w;
    const float2 ndc1 = clip1.xy / clip1.w;
    const float2 ndc2 = clip2.xy / clip2.w;

    const float visWidth = float(viewRasterInfo.scissorMaxX - viewRasterInfo.scissorMinX);
    const float visHeight = float(viewRasterInfo.scissorMaxY - viewRasterInfo.scissorMinY);
    const float scissorMinXf = float(viewRasterInfo.scissorMinX);
    const float scissorMinYf = float(viewRasterInfo.scissorMinY);
    float2 s0 = float2((ndc0.x + 1.0f) * 0.5f * visWidth + scissorMinXf, (1.0f - ndc0.y) * 0.5f * visHeight + scissorMinYf);
    float2 s1 = float2((ndc1.x + 1.0f) * 0.5f * visWidth + scissorMinXf, (1.0f - ndc1.y) * 0.5f * visHeight + scissorMinYf);
    float2 s2 = float2((ndc2.x + 1.0f) * 0.5f * visWidth + scissorMinXf, (1.0f - ndc2.y) * 0.5f * visHeight + scissorMinYf);

    float2 e01 = s1 - s0;
    float2 e02 = s2 - s0;
    float twiceArea = e01.x * e02.y - e01.y * e02.x;
    if (abs(twiceArea) <= REYES_PATCH_RASTER_TINY_TRIANGLE_AREA_EPSILON)
    {
#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
        if (ReyesTryRasterizeTinyProjectedMicroTriangle(
            visBuffer,
            visDims,
            viewRasterInfo,
            s0,
            s1,
            s2,
            depth0,
            depth1,
            depth2,
            patchVisibilityIndex,
            microTriangleIndex))
        {
            InterlockedAdd(telemetryBuffer[0].rasterTinyTriangleFallbackCount, 1u);
            return;
        }
#endif

        InterlockedAdd(telemetryBuffer[0].rasterPreAreaCullCount, 1u);
        return;
    }

    if (twiceArea > 0.0f)
    {
        InterlockedAdd(telemetryBuffer[0].rasterWindingSwapCount, 1u);
        float2 tmpPos = s1;
        s1 = s2;
        s2 = tmpPos;

        float tmpDepth = depth1;
        depth1 = depth2;
        depth2 = tmpDepth;

        e01 = s1 - s0;
        e02 = s2 - s0;
        twiceArea = e01.x * e02.y - e01.y * e02.x;
    }
    else if (twiceArea >= 0.0f)
    {
#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
        if (ReyesTryRasterizeTinyProjectedMicroTriangle(
            visBuffer,
            visDims,
            viewRasterInfo,
            s0,
            s1,
            s2,
            depth0,
            depth1,
            depth2,
            patchVisibilityIndex,
            microTriangleIndex))
        {
            InterlockedAdd(telemetryBuffer[0].rasterTinyTriangleFallbackCount, 1u);
            return;
        }
#endif

        InterlockedAdd(telemetryBuffer[0].rasterPostSwapNonNegativeAreaCount, 1u);
        return;
    }

    const float invTwiceArea = -1.0f / twiceArea;
    const float2 bbMinF = min(min(s0, s1), s2);
    const float2 bbMaxF = max(max(s0, s1), s2);
    int2 minPx = int2(floor(bbMinF));
    int2 maxPx = int2(floor(bbMaxF));
    minPx = max(minPx, int2(viewRasterInfo.scissorMinX, viewRasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(viewRasterInfo.scissorMaxX) - 1, int(viewRasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(visDims.x) - 1, int(visDims.y) - 1));
    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
        if (ReyesTryRasterizeTinyProjectedMicroTriangle(
            visBuffer,
            visDims,
            viewRasterInfo,
            s0,
            s1,
            s2,
            depth0,
            depth1,
            depth2,
            patchVisibilityIndex,
            microTriangleIndex))
        {
            InterlockedAdd(telemetryBuffer[0].rasterTinyTriangleFallbackCount, 1u);
            return;
        }
#endif

        InterlockedAdd(telemetryBuffer[0].rasterEmptyBoundsCullCount, 1u);
        return;
    }

    const float2 origin = float2(float(minPx.x) + 0.5f, float(minPx.y) + 0.5f);
    const float2 e12 = s2 - s1;
    const float2 e20 = s0 - s2;
    const float row_b0 = ((origin.x - s1.x) * e12.y - (origin.y - s1.y) * e12.x) * invTwiceArea;
    const float row_b1 = ((origin.x - s2.x) * e20.y - (origin.y - s2.y) * e20.x) * invTwiceArea;
    const float dx_b0 = e12.y * invTwiceArea;
    const float dx_b1 = e20.y * invTwiceArea;
    const float dy_b0 = -e12.x * invTwiceArea;
    const float dy_b1 = -e20.x * invTwiceArea;

    float scanline_b0 = row_b0;
    float scanline_b1 = row_b1;
    [loop]
    for (int py = minPx.y; py <= maxPx.y; ++py)
    {
        float b0 = scanline_b0;
        float b1 = scanline_b1;
        [loop]
        for (int px = minPx.x; px <= maxPx.x; ++px)
        {
            const float b2 = 1.0f - b0 - b1;

            // Incremental float edge stepping does not hit exact shared-edge zeros reliably.
            // Using a strict top-left equality test here can leave tiny uncovered seams between
            // adjacent micro-triangles at certain view angles. Match the existing SW raster path
            // and accept all non-negative edge coordinates instead.
            if (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f)
            {
                const float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                const uint64_t visKey = PackVisKey(depth, patchVisibilityIndex, microTriangleIndex);
                InterlockedMin(visBuffer[uint2(px, py)], visKey);
            }

            b0 += dx_b0;
            b1 += dx_b1;
        }

        scanline_b0 += dy_b0;
        scanline_b1 += dy_b1;
    }
}

void ReyesRasterizeMicroTriangle(
    RWTexture2D<uint64_t> visBuffer,
    uint2 visDims,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer,
    ClodViewRasterInfo viewRasterInfo,
    float4 clip0,
    float4 clip1,
    float4 clip2,
    float depth0,
    float depth1,
    float depth2,
    float nearDepth,
    uint patchVisibilityIndex,
    uint microTriangleIndex)
{
#if REYES_PATCH_RASTER_ENABLE_NEAR_PLANE_CLIPPING
    ReyesRasterVertex inputVertices[3];
    inputVertices[0].clip = clip0;
    inputVertices[0].depth = depth0;
    inputVertices[1].clip = clip1;
    inputVertices[1].depth = depth1;
    inputVertices[2].clip = clip2;
    inputVertices[2].depth = depth2;

    ReyesRasterVertex clippedVertices[4];
    uint clippedCount = 0u;

    [unroll]
    for (uint edgeIndex = 0u; edgeIndex < 3u; ++edgeIndex)
    {
        const ReyesRasterVertex currentVertex = inputVertices[edgeIndex];
        const ReyesRasterVertex nextVertex = inputVertices[(edgeIndex + 1u) % 3u];
        const bool currentInside = currentVertex.depth >= nearDepth;
        const bool nextInside = nextVertex.depth >= nearDepth;

        if (currentInside)
        {
            clippedVertices[clippedCount++] = currentVertex;
        }

        if (currentInside != nextInside)
        {
            const float depthDelta = nextVertex.depth - currentVertex.depth;
            const float t = saturate((nearDepth - currentVertex.depth) / depthDelta);

            ReyesRasterVertex clippedVertex;
            clippedVertex.clip = lerp(currentVertex.clip, nextVertex.clip, t);
            clippedVertex.depth = nearDepth;
            clippedVertices[clippedCount++] = clippedVertex;
        }
    }

    if (clippedCount < 3u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterClipCullCount, 1u);
        return;
    }

    ReyesRasterizeProjectedMicroTriangle(
        visBuffer,
        visDims,
        telemetryBuffer,
        viewRasterInfo,
        clippedVertices[0].clip,
        clippedVertices[1].clip,
        clippedVertices[2].clip,
        clippedVertices[0].depth,
        clippedVertices[1].depth,
        clippedVertices[2].depth,
        patchVisibilityIndex,
        microTriangleIndex);

    if (clippedCount == 4u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterNearPlaneClippedQuadCount, 1u);
        ReyesRasterizeProjectedMicroTriangle(
            visBuffer,
            visDims,
            telemetryBuffer,
            viewRasterInfo,
            clippedVertices[0].clip,
            clippedVertices[2].clip,
            clippedVertices[3].clip,
            clippedVertices[0].depth,
            clippedVertices[2].depth,
            clippedVertices[3].depth,
            patchVisibilityIndex,
            microTriangleIndex);
    }
#else
    if (depth0 < nearDepth || depth1 < nearDepth || depth2 < nearDepth)
    {
        InterlockedAdd(telemetryBuffer[0].rasterClipCullCount, 1u);
        return;
    }

    ReyesRasterizeProjectedMicroTriangle(
        visBuffer,
        visDims,
        telemetryBuffer,
        viewRasterInfo,
        clip0,
        clip1,
        clip2,
        depth0,
        depth1,
        depth2,
        patchVisibilityIndex,
        microTriangleIndex);
#endif
}

[shader("compute")]
[numthreads(REYES_PATCH_RASTER_GROUP_SIZE, 1, 1)]
void ReyesPatchRasterCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint rasterWorkIndex = dispatchThreadId.x;
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> diceQueueCounter = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesRasterWorkEntry> rasterWorkBuffer = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> rasterWorkCounter = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_WORK_COUNTER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableVertices = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableTriangles = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_VIEW_RASTER_INFO_DESCRIPTOR_INDEX];
    ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];

    const uint rasterWorkCount = rasterWorkCounter[0];
    if (rasterWorkIndex >= rasterWorkCount)
    {
        return;
    }

    const CLodReyesRasterWorkEntry rasterWorkEntry = rasterWorkBuffer[rasterWorkIndex];
    const uint diceIndex = rasterWorkEntry.diceQueueIndex;
    const uint diceCount = diceQueueCounter[0];
    if (diceIndex >= diceCount)
    {
        return;
    }

    const CLodReyesDiceQueueEntry diceEntry = diceQueue[diceIndex];
    uint viewRasterInfoCount = 0u;
    uint viewRasterInfoStride = 0u;
    viewRasterInfoBuffer.GetDimensions(viewRasterInfoCount, viewRasterInfoStride);
    if (diceEntry.viewID >= viewRasterInfoCount)
    {
        return;
    }

    const ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[diceEntry.viewID];
    if (viewRasterInfo.visibilityUAVDescriptorIndex == 0xFFFFFFFFu)
    {
        return;
    }

    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, diceEntry.visibleClusterIndex);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, localMeshletIndex);

    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(diceEntry.instanceID);
    const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(diceEntry.instanceID);
    const CullingCameraInfo camera = cameras[diceEntry.viewID];
    const MaterialInfo materialInfo = materials[perMesh.materialDataIndex];

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
    const uint sourceTriangleIndex = diceEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
    if (sourceTriangleIndex >= CLodDescTriangleCount(meshletDesc))
    {
        return;
    }

    const uint3 sourceTriangle = DecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, meshletDesc.triangleByteOffset, sourceTriangleIndex);
    const float3 sourcePosition0 = DecodeSkinnedPosition(sourceTriangle.x, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const float3 sourcePosition1 = DecodeSkinnedPosition(sourceTriangle.y, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const float3 sourcePosition2 = DecodeSkinnedPosition(sourceTriangle.z, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const bool displacementEnabled = materialInfo.geometricDisplacementEnabled != 0u;
    float3 sourceNormal0 = float3(0.0f, 0.0f, 1.0f);
    float3 sourceNormal1 = float3(0.0f, 0.0f, 1.0f);
    float3 sourceNormal2 = float3(0.0f, 0.0f, 1.0f);
    float2 sourceUv0 = float2(0.0f, 0.0f);
    float2 sourceUv1 = float2(0.0f, 0.0f);
    float2 sourceUv2 = float2(0.0f, 0.0f);
    if (displacementEnabled)
    {
        sourceNormal0 = DecodeSkinnedNormal(sourceTriangle.x, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceNormal1 = DecodeSkinnedNormal(sourceTriangle.y, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceNormal2 = DecodeSkinnedNormal(sourceTriangle.z, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceUv0 = DecodeCompressedUV(sourceTriangle.x, materialInfo.heightUvSetIndex, hdr, meshletDesc, localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
        sourceUv1 = DecodeCompressedUV(sourceTriangle.y, materialInfo.heightUvSetIndex, hdr, meshletDesc, localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
        sourceUv2 = DecodeCompressedUV(sourceTriangle.z, materialInfo.heightUvSetIndex, hdr, meshletDesc, localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
    }

    const float3 domain0 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex0UV);
    const float3 domain1 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex1UV);
    const float3 domain2 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex2UV);
    const uint microTriangleCount = ReyesGetDicePatchMicroTriangleCount(tessTableConfigs, diceEntry);
    if (microTriangleCount == 0u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterZeroMicroTriangleCount, 1u);
        return;
    }

    if (microTriangleCount > 128u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterMicroTriangleOverflowCount, 1u);
        return;
    }

    row_major matrix modelViewProjection = mul(objectData.model, camera.viewProjection);
    float4 modelViewZ = mul(objectData.model, camera.viewZ);
    const float patchDepth = max(
        ( -dot(float4(sourcePosition0, 1.0f), modelViewZ)
        + -dot(float4(sourcePosition1, 1.0f), modelViewZ)
        + -dot(float4(sourcePosition2, 1.0f), modelViewZ)) / 3.0f,
        max(camera.zNear, 1.0e-3f));

    const uint patchVisibilityIndex = CLOD_REYES_PATCH_RASTER_PATCH_INDEX_BASE + diceIndex;
    const uint visibilityDescriptorIndex = viewRasterInfo.visibilityUAVDescriptorIndex;
    RWTexture2D<uint64_t> visBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(visibilityDescriptorIndex)];
    uint2 visDims;
    visBuffer.GetDimensions(visDims.x, visDims.y);

    const uint rasterMicroTriangleEnd = min(rasterWorkEntry.microTriangleOffset + rasterWorkEntry.microTriangleCount, microTriangleCount);
    [loop]
    for (uint microTriangleIndex = rasterWorkEntry.microTriangleOffset; microTriangleIndex < rasterMicroTriangleEnd; ++microTriangleIndex)
    {
        float3 patchBary0;
        float3 patchBary1;
        float3 patchBary2;
        ReyesDecodeMicroTrianglePatchDomain(tessTableConfigs, tessTableVertices, tessTableTriangles, microTriangleIndex, diceEntry, patchBary0, patchBary1, patchBary2);

        float3 sourceBary0;
        float3 sourceBary1;
        float3 sourceBary2;
        float3 patchPosition0;
        float3 patchPosition1;
        float3 patchPosition2;
        ReyesEvaluateDisplacedPatchTriangle(
            materialInfo,
            displacementEnabled,
            camera,
            patchDepth,
            sourcePosition0,
            sourcePosition1,
            sourcePosition2,
            sourceNormal0,
            sourceNormal1,
            sourceNormal2,
            sourceUv0,
            sourceUv1,
            sourceUv2,
            domain0,
            domain1,
            domain2,
            patchBary0,
            patchBary1,
            patchBary2,
            sourceBary0,
            sourceBary1,
            sourceBary2,
            patchPosition0,
            patchPosition1,
            patchPosition2);

        const float4 clip0 = mul(float4(patchPosition0, 1.0f), modelViewProjection);
        const float4 clip1 = mul(float4(patchPosition1, 1.0f), modelViewProjection);
        const float4 clip2 = mul(float4(patchPosition2, 1.0f), modelViewProjection);
        const float depth0 = -dot(float4(patchPosition0, 1.0f), modelViewZ);
        const float depth1 = -dot(float4(patchPosition1, 1.0f), modelViewZ);
        const float depth2 = -dot(float4(patchPosition2, 1.0f), modelViewZ);

        ReyesRasterizeMicroTriangle(
            visBuffer,
            visDims,
            telemetryBuffer,
            viewRasterInfo,
            clip0,
            clip1,
            clip2,
            depth0,
            depth1,
            depth2,
            camera.zNear,
            patchVisibilityIndex,
            microTriangleIndex);
    }
}
