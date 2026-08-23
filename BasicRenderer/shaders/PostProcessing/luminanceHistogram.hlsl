#include "include/cbuffers.hlsli"
#include "PerPassRootConstants/luminanceHistogramRootConstants.h"
#include "include/waveIntrinsicsHelpers.hlsli"

// https://bruop.github.io/exposure/
static const uint GROUP_SIZE = 256;
static const float EPSILON = 0.005f;
static const float3 RGB_TO_LUM = float3(0.2125f, 0.7154f, 0.0721f);

// Convert an HDR RGB -> luminance bin index [0..255]
uint colorToBin(float3 hdrColor, float minLogLum, float inverseLogLumRange)
{
    float lum = dot(hdrColor, RGB_TO_LUM);

    // bin 0 for almost black
    if (lum < EPSILON)
        return 0u;

    // map log2(lum) into [0,1]
    float logLum = saturate((log2(lum) - minLogLum) * inverseLogLumRange);

    // bin [1..255]
    return (uint) (logLum * 254.0f + 1.0f);
}

[numthreads(16, 16, 1)]
void CSMain(
    uint3 dispatchThreadID : SV_DispatchThreadID
)
{
    Texture2D<float4> s_texColor = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Color::HDRColorTarget)];
    RWStructuredBuffer<uint> histogram = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PostProcessing::LuminanceHistogram)];

    uint width, height;
    s_texColor.GetDimensions(width, height);

    // Inactive lanes bail out early; only active lanes participate in wave ops.
    const uint2 sourcePixel = dispatchThreadID.xy * 4u;
    if (sourcePixel.x >= width || sourcePixel.y >= height)
        return;

    // One representative sample per 4x4 footprint. Weighting by the exact
    // footprint size preserves the histogram population for odd resolutions.
    const uint sampleWeight =
        min(4u, width - sourcePixel.x) *
        min(4u, height - sourcePixel.y);
    const uint2 representativePixel = min(sourcePixel + 2u, uint2(width - 1u, height - 1u));
    float3 hdr = s_texColor.Load(int3(representativePixel, 0)).xyz;
    uint bin = colorToBin(hdr, MIN_LOG_LUMINANCE, INVERSE_LOG_LUM_RANGE);

    // Include footprint weight in the key so partial edge footprints are
    // accumulated exactly without penalizing the common four-pixel groups.
    uint waveKey = bin | (sampleWeight << 8u);
    uint4 mask       = WaveMatch(waveKey);
    uint  groupCount = CountBits128(mask);     // how many lanes have this bin
    uint  lane       = WaveGetLaneIndex();
    uint  leaderLane = GetWaveGroupLeaderLane(mask);

    // One atomic per (wave, bin) group
    if (lane == leaderLane)
    {
        uint groupWeight = groupCount * sampleWeight;
        InterlockedAdd(histogram[bin], groupWeight);
    }
}
