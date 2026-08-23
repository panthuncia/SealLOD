#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "fullscreenVS.hlsli"
#include "include/gammaCorrection.hlsli"

#define A_GPU 1
#define A_HLSL 1
#include "FidelityFX/ffx_a.h"

#include "PerPassRootConstants/tonemapRootConstants.h"
uint4 LpmFilterCtl(uint i)
{
    StructuredBuffer<LPMConstants> lpmConstants = ResourceDescriptorHeap[LPM_CONSTANTS_BUFFER_SRV_DESCRIPTOR_INDEX];
    LPMConstants constants = lpmConstants[0];
    return constants.u_ctl[i];
}

#define LPM_NO_SETUP 1
#include "FidelityFX/ffx_lpm.h"

#include "FidelityFX/transferFunction.h"

#define TONEMAP_REINHARD_JODIE 0
#define TONEMAP_KRONOS_PBR_NEUTRAL 1
#define TONEMAP_ACES_HILL 2
#define TONEMAP_AMD_LPM 3

float luminanceFromColor(float3 color)
{
    //standard luminance coefficients
	return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

//https://64.github.io/tonemapping/
//Interpolates between per-channel reinhard and luninance-based reinhard
float3 reinhardJodie(float3 color)
{
	float luminance = luminanceFromColor(color);
	float3 reinhardPerChannel = color / (1.0f + color);
	float3 reinhardLuminance = color / (1.0f + luminance);
	return lerp(reinhardLuminance, reinhardPerChannel, reinhardPerChannel);
}

float3 toneMap_KhronosPbrNeutral(float3 color)
{
	const float startCompression = 0.8 - 0.04;
	const float desaturation = 0.15;

	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;

	float peak = max(color.r, max(color.g, color.b));
	if (peak < startCompression)
		return color;

	const float d = 1. - startCompression;
	float newPeak = 1. - d * d / (peak + d - startCompression);
	color *= newPeak / peak;

	float g = 1. - 1. / (desaturation * (peak - newPeak) + 1.);
	return lerp(color, newPeak * float3(1, 1, 1), g);
}

// sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
static const float3x3 ACESInputMat = float3x3
(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777
);


// ODT_SAT => XYZ => D60_2_D65 => sRGB
static const float3x3 ACESOutputMat = float3x3
(
    1.60475, -0.10208, -0.00327,
    -0.53108, 1.10813, -0.07276,
    -0.07367, -0.00605, 1.07602
);

// ACES filmic tone map approximation
// see https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
float3 RRTAndODTFit(float3 color)
{
	float3 a = color * (color + 0.0245786) - 0.000090537;
	float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
	return a / b;
}


// tone mapping
float3 toneMapACES_Hill(float3 color)
{
	color = mul(color, ACESInputMat);

    // Apply RRT and ODT
	color = RRTAndODTFit(color);

	color = mul(color, ACESOutputMat);

    // Clamp to [0, 1]
	color = clamp(color, 0.0, 1.0);

	return color;
}

float3 SampleBloomForTonemap(float2 uv)
{
    Texture2D<float4> bloom = ResourceDescriptorHeap[TONEMAP_BLOOM_MIP1_SRV_DESCRIPTOR_INDEX];
    Texture2D<float4> lowBloom = ResourceDescriptorHeap[TONEMAP_BLOOM_MIP2_SRV_DESCRIPTOR_INDEX];
    // Every pyramid level has already been filtered during downsample and
    // accumulation. One hardware-bilinear reconstruction per remaining level
    // retains the broad bloom profile without another eight-tap output filter.
    return bloom.SampleLevel(g_linearClamp, uv, 0).rgb +
        lowBloom.SampleLevel(g_linearClamp, uv, 0).rgb;
}

// UintRootConstant0 is HDR source SRV
float4 PSMain(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    Texture2D<float4> hdrSource = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PostProcessing::UpscaledHDR)];
    float2 uv = input.uv;
    uv.y = 1.0f - uv.y; // Why is this necessary only here?
    float4 color = float4(hdrSource.SampleLevel(g_pointClamp, uv, 0).rgb, 1.0);
    if (TONEMAP_BLOOM_ENABLED != 0u)
        color.rgb = lerp(color.rgb, SampleBloomForTonemap(uv), 0.04f);
	ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
	// Apply tone mapping based on the selected method
    switch (TONEMAP_TYPE)
    {
		case TONEMAP_REINHARD_JODIE:
			color.rgb = reinhardJodie(color.rgb);
			break;
		case TONEMAP_KRONOS_PBR_NEUTRAL:
			color.rgb = toneMap_KhronosPbrNeutral(color.rgb);
			break;
		case TONEMAP_ACES_HILL:
			color.rgb = toneMapACES_Hill(color.rgb);
			break;
        case TONEMAP_AMD_LPM:{
            StructuredBuffer<LPMConstants> lpmConstants = ResourceDescriptorHeap[LPM_CONSTANTS_BUFFER_SRV_DESCRIPTOR_INDEX];
                LPMConstants constants = lpmConstants[0];
                LpmFilter(color.r, color.g, color.b, constants.shoulder, constants.con, constants.soft, constants.con2, constants.clip, constants.scaleOnly);

            }
		default:
			// No tone mapping
			break;
	}
	
	color.rgb = LinearToSRGB(color.rgb);
	
	return color;
}
