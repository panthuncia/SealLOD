#ifndef CLOD_CLUSTER_FORMAT_HLSLI
#define CLOD_CLUSTER_FORMAT_HLSLI

// Fetchers for the streams of a cluster page: triangles and UVs. Raster and resolve paths
// call these instead of keeping their own copy of the layout.
// Requires clodStructs.hlsli and clodPageAccess.hlsli.

#include "clodStructs.hlsli"
#include "clodPageAccess.hlsli"

// Reads up to 32 bits from a bitstream, spanning at most two words.
uint CLodReadPackedBits32(ByteAddressBuffer slab, uint startBit, uint bitCount)
{
    if (bitCount == 0u)
    {
        return 0u;
    }

    const uint wordIndex = startBit >> 5u;
    const uint bitOffset = startBit & 31u;
    uint packed = slab.Load(wordIndex * 4u) >> bitOffset;
    if (bitOffset + bitCount > 32u)
    {
        packed |= slab.Load((wordIndex + 1u) * 4u) << (32u - bitOffset);
    }

    const uint mask = bitCount >= 32u ? 0xffffffffu : ((1u << bitCount) - 1u);
    return packed & mask;
}

// Triangle indices are three bytes per triangle with no alignment, so one can straddle two words.
uint3 CLodDecodeTriangle(ByteAddressBuffer slab, uint triangleStreamAddress, uint triangleIndex)
{
    const uint byteAddress = triangleStreamAddress + triangleIndex * 3u;
    const uint alignedAddress = byteAddress & ~3u;
    const uint byteOffset = byteAddress & 3u;

    uint packed = slab.Load(alignedAddress);
    if (byteOffset != 0u)
    {
        packed >>= byteOffset * 8u;
        if (byteOffset > 1u)
        {
            packed |= slab.Load(alignedAddress + 4u) << ((4u - byteOffset) * 8u);
        }
    }

    return uint3(packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu);
}

// Decodes triangleBlockBase + laneIndex for each lane of a full 32-lane wave, loading the block's
// words once between them. Compute only: this pattern stalled the GPU in a mesh shader.
uint3 CLodDecodeTriangleWave32(
    ByteAddressBuffer slab,
    uint triangleStreamAddress,
    uint triangleBlockBase,
    uint laneIndex)
{
    const uint blockAddress = triangleStreamAddress + triangleBlockBase * 3u;
    const uint alignedAddress = blockAddress & ~3u;
    const uint prefixBytes = blockAddress & 3u;
    const uint blockWordCount = (prefixBytes + 32u * 3u + 3u) >> 2u;
    const uint laneWord = laneIndex < blockWordCount
        ? slab.Load(alignedAddress + laneIndex * 4u)
        : 0u;

    const uint relativeByteOffset = prefixBytes + laneIndex * 3u;
    const uint sourceWordLane = relativeByteOffset >> 2u;
    const uint sourceByteOffset = relativeByteOffset & 3u;
    const uint firstWord = WaveReadLaneAt(laneWord, sourceWordLane);
    const uint secondWord = WaveReadLaneAt(laneWord, sourceWordLane + 1u);
    const uint packed = sourceByteOffset == 0u
        ? firstWord
        : (firstWord >> (sourceByteOffset * 8u)) | (secondWord << ((4u - sourceByteOffset) * 8u));
    return uint3(packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu);
}

// One meshlet's view of one UV set. Load it once, then decode as many vertices as needed.
struct CLodUvStream
{
    uint firstBit;
    uint bitsU;
    uint bitsV;
    float2 uvMin;
    float2 uvScale;
};

// A UV set the page doesn't have yields a stream that decodes every vertex to zero.
CLodUvStream CLodLoadUvStream(
    uint slabDescriptorIndex,
    uint pageByteOffset,
    uint uvSetCount,
    uint uvDescriptorOffset,
    uint uvBitstreamDirectoryOffset,
    uint meshletIndex,
    uint uvSetIndex)
{
    CLodUvStream stream = (CLodUvStream)0;
    if (uvSetIndex >= uvSetCount)
    {
        return stream;
    }

    const CLodMeshletUvDescriptor descriptor = LoadMeshletUvDescriptor(
        slabDescriptorIndex, pageByteOffset, uvDescriptorOffset, uvSetCount, meshletIndex, uvSetIndex);
    const uint bitstreamBase = pageByteOffset + LoadPageUvBitstreamOffset(
        slabDescriptorIndex, pageByteOffset, uvBitstreamDirectoryOffset, uvSetIndex);

    stream.firstBit = bitstreamBase * 8u + descriptor.uvBitOffset;
    stream.bitsU = CLodUvDescBitsU(descriptor);
    stream.bitsV = CLodUvDescBitsV(descriptor);
    stream.uvMin = float2(descriptor.uvMinU, descriptor.uvMinV);
    stream.uvScale = float2(descriptor.uvScaleU, descriptor.uvScaleV);
    return stream;
}

float2 CLodDecodeUv(ByteAddressBuffer slab, CLodUvStream stream, uint vertexIndex)
{
    const uint bitCursor = stream.firstBit + vertexIndex * (stream.bitsU + stream.bitsV);
    const uint encodedU = CLodReadPackedBits32(slab, bitCursor, stream.bitsU);
    const uint encodedV = CLodReadPackedBits32(slab, bitCursor + stream.bitsU, stream.bitsV);
    return stream.uvMin + float2(encodedU, encodedV) * stream.uvScale;
}

// Normals are stored octahedral in two snorm16s.
float3 CLodDecodeNormal(uint packed)
{
    const int signedPacked = asint(packed);
    const float2 encoded = max(
        float2((signedPacked << 16) >> 16, signedPacked >> 16) / 32767.0f,
        float2(-1.0f, -1.0f));
    float3 normal = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0.0f)
    {
        normal.xy = (1.0f - abs(normal.yx)) * float2(normal.x >= 0.0f ? 1.0f : -1.0f, normal.y >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(normal);
}

float3 CLodDecodeColor(uint packed)
{
    return float3(packed & 0xffu, (packed >> 8u) & 0xffu, (packed >> 16u) & 0xffu) / 255.0f;
}

#endif // CLOD_CLUSTER_FORMAT_HLSLI
