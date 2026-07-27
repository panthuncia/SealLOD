#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "fullscreenVS.hlsli"
#include "include/gammaCorrection.hlsli"
#include "PerPassRootConstants/bloomSampleRootConstants.h"

// UintRootConstant0 is the source SRV descriptor index.
// UintRootConstant1/2 are the source dimensions.

// UintRootConstant3/4 carry texel size as float bit patterns
float4 downsample(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    float x = SRC_TEXEL_SIZE_X;
    float y = SRC_TEXEL_SIZE_Y;
    
    Texture2D<float4> source = ResourceDescriptorHeap[SOURCE_TEXTURE_DESCRIPTOR_INDEX];
    float2 texCoord = input.uv;
    texCoord.y = 1.0f - texCoord.y;
    
    // Four bilinear taps cover the 4x4 source footprint used by this 2:1
    // downsample. The sampler performs the four sub-texel reads per tap.
    float3 downsample = source.SampleLevel(g_linearClamp, texCoord + float2(-x, -y), 0).rgb;
    downsample += source.SampleLevel(g_linearClamp, texCoord + float2(x, -y), 0).rgb;
    downsample += source.SampleLevel(g_linearClamp, texCoord + float2(-x, y), 0).rgb;
    downsample += source.SampleLevel(g_linearClamp, texCoord + float2(x, y), 0).rgb;
    downsample *= 0.25f;
    
    downsample = max(downsample, 0.00001f);
    
    if (length(downsample) < 1.0)
        return (float4(0, 0, 0, 1));
    
    return float4(downsample, 1.0);
}
