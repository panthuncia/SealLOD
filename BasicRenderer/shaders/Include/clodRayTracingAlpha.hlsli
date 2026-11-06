#ifndef CLOD_RAY_TRACING_ALPHA_HLSLI
#define CLOD_RAY_TRACING_ALPHA_HLSLI

// The alpha test path tracing applies to rays, and the opacity micromap bake that must agree with it.
//
// Rays test one fixed mip of each alpha texture: the first no larger than CLOD_RT_ALPHA_TEST_RESOLUTION. A mip chosen
// by distance would make the test view dependent, and coarse foliage mips disagree with fine ones nearly everywhere, so
// no micro-triangle could be classified. With a fixed mip, a micro-triangle whose bilinear footprint is all above or
// all below the cutoff gets the answer any-hit would give, and any-hit only runs where they differ.

#include "include/structs.hlsli"
#include "include/utilities.hlsli"
#include "include/materialFlags.hlsli"
#include "include/clodAlphaCutoff.hlsli"

#ifndef CLOD_RT_ALPHA_TEST_RESOLUTION
#define CLOD_RT_ALPHA_TEST_RESOLUTION 512u
#endif

struct CLodRtAlphaImage
{
    uint descriptor;
    uint mip;          // Within the bound image, which starts at the resident top mip.
    uint absoluteMip;  // Within the full texture.
    uint fullMaxDimension;
    bool atTestMip;    // False when only coarser mips are resident: the test then uses the finest it has.
};

uint CLodRtAlphaTestMip(uint fullMaxDimension, uint mipCount)
{
    uint mip = 0u;
    while (mip + 1u < mipCount && (fullMaxDimension >> mip) > CLOD_RT_ALPHA_TEST_RESOLUTION) {
        ++mip;
    }
    return mip;
}

CLodRtAlphaImage CLodRtResolveAlphaImage(uint textureIndex, uint streamingTextureID)
{
    CLodRtAlphaImage image;
    image.descriptor = textureIndex;
    image.atTestMip = true;
    uint topMip = 0u;
    uint fullWidth = 0u;
    uint fullHeight = 0u;
    uint mipCount = 0u;
    if (streamingTextureID != 0u) {
        const TextureStreamingGPUInfo streaming = LoadTextureStreamingInfo(streamingTextureID);
        if (streaming.imageDescriptorIndex != 0xFFFFFFFFu) {
            image.descriptor = streaming.imageDescriptorIndex;
            topMip = streaming.residentTopMip;
            if (TextureStreamingHasFullMip0Dimensions(streaming)) {
                fullWidth = streaming.fullWidth;
                fullHeight = streaming.fullHeight;
            }
            mipCount = streaming.totalMipCount;
        }
        else {
            image.atTestMip = false; // Whatever the material points at instead is not the texture.
        }
    }
    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(image.descriptor)];
    uint width = 1u;
    uint height = 1u;
    uint levels = 1u;
    texture.GetDimensions(0u, width, height, levels);
    if (fullWidth == 0u || fullHeight == 0u) {
        fullWidth = width << topMip;
        fullHeight = height << topMip;
    }
    if (mipCount == 0u) {
        mipCount = levels + topMip;
    }
    image.fullMaxDimension = max(max(fullWidth, fullHeight), 1u);
    image.absoluteMip = CLodRtAlphaTestMip(image.fullMaxDimension, mipCount);
    image.atTestMip = image.atTestMip && topMip <= image.absoluteMip;
    image.mip = image.absoluteMip >= topMip ? min(image.absoluteMip - topMip, levels - 1u) : 0u;
    return image;
}

#endif // CLOD_RAY_TRACING_ALPHA_HLSLI
