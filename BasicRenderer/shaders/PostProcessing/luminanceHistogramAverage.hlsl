#include "include/cbuffers.hlsli"
#include "PerPassRootConstants/luminanceHistogramAverageRootConstants.h"

#define A_GPU 1
#define A_HLSL 1
#include "FidelityFX/ffx_a.h"

A_STATIC AF1 fs2S;
A_STATIC AF1 hdr10S;

A_STATIC void LpmSetupOut(AU1 i, inAU4 v)
{
    RWStructuredBuffer<LPMConstants> lpmConstants = ResourceDescriptorHeap[ResourceDescriptorIndex(FFX::LPMConstants)];
    for (int j = 0; j < 4; ++j)
    {
        lpmConstants[0].u_ctl[i * 4 + j] = v[j];
    }
}

uint4 LpmFilterCtl(uint i)
{
    StructuredBuffer<LPMConstants> lpmConstants = ResourceDescriptorHeap[ResourceDescriptorIndex(FFX::LPMConstants)];
    LPMConstants constants = lpmConstants[0];
    return constants.u_ctl[i];
}

#include "FidelityFX/ffx_lpm.h"

struct ColorSpace
{
    float2 r;
    float2 g;
    float2 b;
    float2 D65;
};

static const ColorSpace Rec709 = {
	{ 0.64f, 0.33f }, // Red
	{ 0.30f, 0.60f }, // Green
	{ 0.15f, 0.06f }, // Blue
	{ 0.3127f, 0.3290f } // D65 white point
};

// https://bruop.github.io/exposure/
static const uint GROUP_SIZE = 256;
static const uint THREADS_X = 256;
static const uint THREADS_Y = 1;

groupshared uint waveWeightedSums[8];

[WaveSize(32)]
[numthreads(THREADS_X, THREADS_Y, 1)]
void CSMain(
    uint3 dispatchThreadID : SV_DispatchThreadID, // not used here
    uint3 groupThreadID : SV_GroupThreadID, // 2D coords in group
    uint groupIndex : SV_GroupIndex // linear index in [0..255]
)
{
    uint localIndex = groupIndex;

    RWStructuredBuffer<uint> histogram = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PostProcessing::LuminanceHistogram)];
    uint countForThisBin = histogram[localIndex];
    histogram[localIndex] = 0;

    // Reduce within each 32-lane wave, then combine the eight wave totals with
    // one final wave reduction. This replaces eight group-wide synchronization
    // points in the original shared-memory tree with one.
    const uint weightedWaveSum = WaveActiveSum(countForThisBin * localIndex);
    if (WaveIsFirstLane())
        waveWeightedSums[localIndex >> 5u] = weightedWaveSum;
    GroupMemoryBarrierWithGroupSync();

    const uint waveTotal = localIndex < 8u ? waveWeightedSums[localIndex] : 0u;
    const uint weightedHistogramSum = WaveActiveSum(waveTotal);

    // Only one thread needs to compute and store the final exposure
    if (localIndex == 0)
    {
        // weighted log-average, normalized to [0..1], then offset by -1
        float weightedLogAverage = (weightedHistogramSum / max(NUM_PIXELS - countForThisBin, 1.0)) - 1.0;

        // map back to luminance domain
        float weightedAvgLum = exp2((weightedLogAverage / 254.0) * LOG_LUMINANCE_RANGE + MIN_LOG_LUMINANCE);

        RWStructuredBuffer<float> s_target = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PostProcessing::AdaptedLuminance)];

        // fetch last frame's adapted luminance
        float lumLastFrame = s_target[0];

        // lerp toward new value
        float adaptedLum = lumLastFrame + (weightedAvgLum - lumLastFrame) * TIME_COEFFICIENT;

        // store updated adapted luminance
        s_target[0] = adaptedLum;
        
        float scaleC = 1.0f;
        float softGap = 0.001f;
        float hdrMax = 10.0f; // TODO : replace with actual HDR max luminance
        
        float Lk = adaptedLum;
        float exposure = log2(hdrMax * 0.18f / max(Lk, 1e-5f));
        float contrast = 0.0f;
        float shoulderContrast = 1.0f;

        float3 saturation = { 0.0f, 0.0f, 0.0f };
        float3 crosstalk = { 1.0f, 1.0f, 1.0f };
        
        RWStructuredBuffer<LPMConstants> lpmConstantsBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(FFX::LPMConstants)];
        LPMConstants lpmConstants = lpmConstantsBuffer[0];
        
        LpmSetup(
        lpmConstants.shoulder, lpmConstants.con, lpmConstants.soft, lpmConstants.con2, lpmConstants.clip, lpmConstants.scaleOnly,
            Rec709.r, Rec709.g, Rec709.b, Rec709.D65,
            Rec709.r, Rec709.g, Rec709.b, Rec709.D65,
            Rec709.r, Rec709.g, Rec709.b, Rec709.D65,
            scaleC, softGap, hdrMax, exposure, contrast, shoulderContrast, saturation, crosstalk
        );
        
    }
}
