#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "fullscreenVS.hlsli"
#include "include/gammaCorrection.hlsli"
#include "PerPassRootConstants/bloomBlendRootConstants.h"

// UintRootConstant1 is bloom source SRV
// UintRootConstant2 is src res x
// UintRootConstant3 is src res y

// UintRootConstant4/5 carry filter radius and aspect ratio as float bit patterns
float4 blend(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    Texture2D<float4> bloom = ResourceDescriptorHeap[BLOOM_SOURCE_SRV_DESCRIPTOR_INDEX];
    Texture2D<float4> lowBloom = ResourceDescriptorHeap[BLOOM_LOW_SOURCE_SRV_DESCRIPTOR_INDEX];
    float2 texCoord = input.uv;
    texCoord.y = 1.0f - texCoord.y;

    const float filterRadius = BLOOM_BLEND_FILTER_RADIUS;
    const float x = filterRadius;
    const float y = filterRadius * BLOOM_BLEND_ASPECT_RATIO;
    const float2 halfOffset = 0.5f * float2(x, y);
    float3 finalBloom = bloom.SampleLevel(g_linearClamp, texCoord + float2(-halfOffset.x, -halfOffset.y), 0).rgb;
    finalBloom += bloom.SampleLevel(g_linearClamp, texCoord + float2(halfOffset.x, -halfOffset.y), 0).rgb;
    finalBloom += bloom.SampleLevel(g_linearClamp, texCoord + float2(-halfOffset.x, halfOffset.y), 0).rgb;
    finalBloom += bloom.SampleLevel(g_linearClamp, texCoord + float2(halfOffset.x, halfOffset.y), 0).rgb;
    finalBloom *= 0.25f;

    // Fold the last pyramid upsample into this pass. Mip 2 already contains
    // the accumulated lower-frequency levels; four bilinear taps reconstruct
    // it directly at output resolution.
    const float2 lowOffset = float2(x, y);
    float3 lowFrequency = lowBloom.SampleLevel(g_linearClamp, texCoord + float2(-lowOffset.x, -lowOffset.y), 0).rgb;
    lowFrequency += lowBloom.SampleLevel(g_linearClamp, texCoord + float2(lowOffset.x, -lowOffset.y), 0).rgb;
    lowFrequency += lowBloom.SampleLevel(g_linearClamp, texCoord + float2(-lowOffset.x, lowOffset.y), 0).rgb;
    lowFrequency += lowBloom.SampleLevel(g_linearClamp, texCoord + float2(lowOffset.x, lowOffset.y), 0).rgb;
    finalBloom += lowFrequency * 0.25f;

    // The PSO uses SrcAlpha / InvSrcAlpha for color and preserves destination
    // alpha, exactly matching lerp(existing, finalBloom, 0.04).
    return float4(finalBloom, 0.04f);
}
