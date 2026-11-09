#include "Include/cbuffers.hlsli"
#include "Include/bc7Mode6.hlsli"

[numthreads(8, 8, 1)]
void BC7CompressMode6CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Texture2D<float4> srcTexture = ResourceDescriptorHeap[UintRootConstant0];
    RWByteAddressBuffer dstBuffer = ResourceDescriptorHeap[UintRootConstant1];

    const uint blocksX = (UintRootConstant4 + 3u) / 4u;
    const uint blocksY = (UintRootConstant5 + 3u) / 4u;
    if (dispatchThreadId.x >= blocksX || dispatchThreadId.y >= blocksY)
    {
        return;
    }

    const uint2 blockBase = dispatchThreadId.xy * 4u;
    const uint2 textureSize = uint2(UintRootConstant4, UintRootConstant5);
    BC7Mode6Block block = EncodeMode6(srcTexture, blockBase, textureSize);

    const uint byteOffset = UintRootConstant2 + dispatchThreadId.y * UintRootConstant3 + dispatchThreadId.x * 16u;
    dstBuffer.Store4(byteOffset, block.words);
}
