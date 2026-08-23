#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "fullscreenVS.hlsli"
#include "include/gammaCorrection.hlsli"
#include "PerPassRootConstants/bloomSampleRootConstants.h"

float3 sample_for_upsample(Texture2D<float4> source, float2 texCoord, float x, float y)
{
    const float2 halfOffset = 0.5f * float2(x, y);
    float3 result = source.SampleLevel(g_linearClamp, texCoord + float2(-halfOffset.x, -halfOffset.y), 0).rgb;
    result += source.SampleLevel(g_linearClamp, texCoord + float2(halfOffset.x, -halfOffset.y), 0).rgb;
    result += source.SampleLevel(g_linearClamp, texCoord + float2(-halfOffset.x, halfOffset.y), 0).rgb;
    result += source.SampleLevel(g_linearClamp, texCoord + float2(halfOffset.x, halfOffset.y), 0).rgb;
    return result * 0.25f;
}

// UintRootConstant2 is src res x
// UintRootConstant3 is src res y

// UintRootConstant3/4 carry filter radius and aspect ratio as float bit patterns

float4 upsample(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    float filterRadius = BLOOM_SAMPLE_FILTER_RADIUS;
    float x = filterRadius;
    float y = filterRadius * BLOOM_SAMPLE_ASPECT_RATIO;
    
    Texture2D<float4> source = ResourceDescriptorHeap[SOURCE_TEXTURE_DESCRIPTOR_INDEX];
    float2 texCoord = input.uv;
    texCoord.y = 1.0f - texCoord.y;
    
    float3 upsample = sample_for_upsample(source, texCoord, x, y);
    
    return float4(upsample, 1.0);
}
