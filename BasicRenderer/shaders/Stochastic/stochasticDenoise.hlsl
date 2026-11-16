// Stochastic lighting's denoiser (docs/stochastic_lighting.md "Denoiser"), after MegaLights' SVGF-style filter. Two passes:
//
// Temporal: demodulates this frame's noisy diffuse and specular radiance (by the diffuse colour and F0), reprojects last
// frame's history with bilinear taps validated against the surface history's depth, shortens the history where it no
// longer agrees with this frame's 5x5 neighbourhood, and accumulates (an exponential moving average, weight 1 / length).
// Writes the history this pass reads next frame.
//
// Reduced resolution (STOCHASTIC_DOWNSAMPLE_FACTOR 2; docs/stochastic_lighting.md "Reduced resolution"): the noisy inputs
// hold one sample per 2x2 block, at the pixel StochasticDownsampledPixel picks. The temporal pass reconstructs each pixel's
// value for this frame as the mean of the four nearest samples, weighted by bilinear weight times depth and normal
// agreement, and keeps its history alone where none of them lies on its surface.
//
// Spatial: one sparse, per-pixel rotated kernel over the temporal result, 8 pixels wide where its history is short
// (disocclusion, light that just arrived) and 3 pixels everywhere else, edge-stopped by depth, normal and luminance against
// the variance. Remodulates into StochasticLighting::Diffuse and ::Specular. It does not feed back into the history. The
// 3-pixel floor and the mean are for DLSS, which removes what changes from frame to frame and keeps the rest
// (docs/stochastic_lighting.md "Noise after DLSS").
//
// The estimator's radiance is unbiased but heavy-tailed at a few samples per pixel: half of all pixels are black in a
// frame, and rare samples are thousands of times the mean. Everything here that compares against neighbours is built so
// it does not trim that tail, which darkens (see "Denoiser" in the doc for the measurements).
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "PerPassRootConstants/stochasticLightingRootConstants.h"

#define DENOISE_ALBEDO_FLOOR 0.02f
#ifndef DENOISE_MAX_HISTORY
#define DENOISE_MAX_HISTORY 32.0f
#endif
// Histories, moments and variance are fp16.
#define DENOISE_FP16_LIMIT 60000.0f
// Rectification: the history is kept in full while it is within this many standard deviations of the neighbourhood's
// mean, and shortened to nothing at twice that. The #ifndef guards let pso.recompile try others.
#ifndef DENOISE_RECTIFY_SIGMA
#define DENOISE_RECTIFY_SIGMA 1.0f
#endif
#ifndef DENOISE_RECTIFY
#define DENOISE_RECTIFY 1
#endif
// The length rectification shortens a history to at the far end (twice DENOISE_RECTIFY_SIGMA). 1 keeps half of it each
// frame, which is no reset for a history a light left 10^5 times brighter than what is there now (the trail test,
// docs/stochastic_lighting.md "Moving-light trails"): it takes ~17 frames of halving to fade. 0 drops it.
// How much of rectification's per-sample variance is the history's temporal one (the rest the neighbourhood's spatial one).
// The unshadowed bound (docs/stochastic_lighting.md "Moving-light trails"). Selection estimates how much unshadowed light
// reaches each sampled pixel this frame (stochasticSplit.hlsli) and shading passes it in the noisy diffuse input's alpha.
// The history's luminance, remodulated, may not exceed DENOISE_BOUND_MARGIN times the largest bound in the 5x5
// neighbourhood: light that has left can't stay, while light that is still there (shadowed or not) is never held under its
// unshadowed level. The margin covers the cheap importance's approximations (no Fresnel or masking) and the hidden lights'
// estimate: the noisy input reaches 1.1x the bound at its 99th percentile and 1.3x at its 99.9th in the trail test.
// Margin 2 against no bound: stuck energy 0.027 -> 0.003, stuck blocks 0.59% -> 0.17%; still lights unchanged (4: no
// change, 1: -1%). Cutting the history where it binds was tried and dropped (fewer stuck blocks, more arrival lag).
#ifndef DENOISE_BOUND
#define DENOISE_BOUND 1
#endif
#ifndef DENOISE_BOUND_MARGIN
#define DENOISE_BOUND_MARGIN 2.0f
#endif
// Change from the bound: the history keeps the bound it stands for, a running average of the neighbourhood's largest bound
// with the history's own weights (the change history's x; DENOISE_CHANGE_DETECT must be off). This frame's bound against it
// says how the light that can reach the surface has changed since the history was made, lit or shadowed.
//   1: where they differ by a factor r beyond DENOISE_BOUND_CHANGE_TOLERANCE, the history is cut to DENOISE_MAX_HISTORY / r.
//   2: where this frame's is lower, the history is scaled down by the ratio (and stands for this frame's bound after): the
//      light that left takes its share of the history with it, as long as visibility hasn't changed with it.
//   3: both.
// DENOISE_BOUND_CHANGE_FLOOR keeps nearly dark bounds from reading as large ratios.
#ifndef DENOISE_BOUND_CHANGE
#define DENOISE_BOUND_CHANGE 0
#endif
// Mode 2 at 8: trail excess -35% and stuck blocks halved against the bound alone, for 6% darker in motion and 0.7% with
// still lights (the bound's own frame-to-frame noise near hot spots); at 4, more of both. Off by default: a trade.
#ifndef DENOISE_BOUND_CHANGE_TOLERANCE
#define DENOISE_BOUND_CHANGE_TOLERANCE 8.0f
#endif
#ifndef DENOISE_BOUND_CHANGE_FLOOR
#define DENOISE_BOUND_CHANGE_FLOOR 0.05f
#endif
#if DENOISE_BOUND_CHANGE && DENOISE_CHANGE_DETECT
#error DENOISE_BOUND_CHANGE and DENOISE_CHANGE_DETECT share the change history
#endif
#ifndef DENOISE_RECTIFY_TEMPORAL_WEIGHT
#define DENOISE_RECTIFY_TEMPORAL_WEIGHT 0.5f
#endif
#ifndef DENOISE_RECTIFY_MIN_LENGTH
#define DENOISE_RECTIFY_MIN_LENGTH 1.0f
#endif
// Change-aware rectification (docs/stochastic_lighting.md "Change-aware rectification"). 1: the history is shortened where
// the 5x5 neighbourhood's compressed mean has changed, judged by a fast average of it (weight DENOISE_CHANGE_FAST_WEIGHT)
// against a slow one that follows the history's own length: like with like, so spatial structure (an edge, a shadow's
// border) is no evidence, and several frames' evidence, so one noisy frame is none either. The history is kept in full up
// to DENOISE_CHANGE_SIGMA standard deviations of their difference and shortened to one frame at DENOISE_CHANGE_SIGMA_RESET.
// The per-pixel test above (DENOISE_RECTIFY) still applies on top, at DENOISE_RECTIFY_SIGMA. 0: the per-pixel test alone.
#ifndef DENOISE_CHANGE_DETECT
#define DENOISE_CHANGE_DETECT 0
#endif
#ifndef DENOISE_CHANGE_FAST_WEIGHT
#define DENOISE_CHANGE_FAST_WEIGHT 0.25f
#endif
#ifndef DENOISE_CHANGE_SIGMA
#define DENOISE_CHANGE_SIGMA 2.0f
#endif
#ifndef DENOISE_CHANGE_SIGMA_RESET
#define DENOISE_CHANGE_SIGMA_RESET 4.0f
#endif
// The spike limit (below, where it is applied): on or off, how many times the neighbourhood's brightest sample the history
// may reach, and the fraction of lit samples a neighbourhood needs before it applies.
#ifndef DENOISE_SPIKE
#define DENOISE_SPIKE 1
#endif
#ifndef DENOISE_SPIKE_LIMIT
#define DENOISE_SPIKE_LIMIT 4.0f
#endif
#ifndef DENOISE_SPIKE_LIT_FRACTION
#define DENOISE_SPIKE_LIT_FRACTION 0.75f
#endif
#ifndef DENOISE_SPATIAL
#define DENOISE_SPATIAL 1
#endif
// Reprojection holds per tap when last frame's surface there was this close in view depth (relative). There is no normal
// test: the shading normal is normal-mapped, and under the upscaler's jitter it swings past any useful threshold at the
// same pixel from frame to frame (a cosine-0.5 test discarded the history of 13% of pixels every frame).
#ifndef DENOISE_DEPTH_TOLERANCE
#define DENOISE_DEPTH_TOLERANCE 0.05f
#endif
// 1: the history is rounded to fp16 stochastically (unbiased whatever the store's rounding mode).
#ifndef DENOISE_STOCHASTIC_ROUNDING
#define DENOISE_STOCHASTIC_ROUNDING 1
#endif
// Below this history length the spatial filter widens.
#define DENOISE_SHORT_HISTORY 4.0f
// Reduced-resolution reconstruction: a sample's weight for a pixel is its bilinear weight times its depth agreement (falling
// linearly to 0 at this relative view-depth difference) times its normal agreement (the cosine to this power). Below the
// minimum total weight no sample lies on the pixel's surface, and the pixel keeps its history alone.
#ifndef DENOISE_RECONSTRUCT_DEPTH
#define DENOISE_RECONSTRUCT_DEPTH 0.05f
#endif
#ifndef DENOISE_RECONSTRUCT_NORMAL_POWER
#define DENOISE_RECONSTRUCT_NORMAL_POWER 8.0f
#endif
#ifndef DENOISE_RECONSTRUCT_MIN_WEIGHT
#define DENOISE_RECONSTRUCT_MIN_WEIGHT 0.05f
#endif
// 1: the frame's value is the weighted mean of the four samples; 0: one of them chosen in proportion to its weight (before
// 2026-11-15). The moments still take the chosen one, so rectification sees a sample's spread. Downstream of DLSS the mean
// is better: DLSS averages over a long window, so what survives it is the noise at low temporal frequencies, and the random
// choice adds selection noise at every frequency where the mean adds none (docs/stochastic_lighting.md "Noise after DLSS").
#ifndef DENOISE_RECONSTRUCT_MEAN
#define DENOISE_RECONSTRUCT_MEAN 1
#endif
// The spatial filter's radius never falls below this (pixels), so it runs on every pixel, as CORTAO's prefilter does: the
// temporal average leaves the noise at low temporal frequencies (what DLSS keeps) as it found it, and only a spatial filter
// lowers it. 0: the filter runs on short histories alone (before 2026-11-15).
#ifndef DENOISE_SPATIAL_MIN_RADIUS
#define DENOISE_SPATIAL_MIN_RADIUS 3.0f
#endif
// 1: where both signals' radius is at that floor (the history has converged), the filter takes CORTAO's small kernel instead
// of the 12 taps: 4 taps at the floor radius in a plus, rotated per pixel and frame, each tap's depth and normal loaded once
// for both signals. Specular joins only where it is rough enough to take the same radius. 0: the 12 taps everywhere.
#ifndef DENOISE_SPATIAL_FLOOR_TAPS
#define DENOISE_SPATIAL_FLOOR_TAPS 1
#endif

#define DENOISE_TILE 8
#define DENOISE_BORDER 2
#define DENOISE_REGION (DENOISE_TILE + 2 * DENOISE_BORDER)
// Downsampled: the tile's 4x4 samples and a 2-sample border, one per thread.
#define DENOISE_SAMPLE_REGION (DENOISE_TILE / 2 + 2 * DENOISE_BORDER)

static const float2 kDenoisePoisson[12] = {
    float2(-0.326f, -0.406f), float2(-0.840f, -0.074f), float2(-0.696f, 0.457f), float2(-0.203f, 0.621f),
    float2(0.962f, -0.195f), float2(0.473f, -0.480f), float2(0.519f, 0.767f), float2(0.185f, -0.893f),
    float2(0.507f, 0.064f), float2(0.896f, 0.412f), float2(-0.322f, -0.933f), float2(-0.792f, -0.598f),
};

float DenoiseLuminance(float3 value)
{
    return dot(value, float3(0.25f, 0.5f, 0.25f));
}

// What rectification and the moments see: log2(1 + luminance). Bounded where the tail is not, so its moments neither
// overflow fp16 nor are dominated by one sample, and its spatial and temporal means are comparable.
float DenoiseCompress(float luminance)
{
    return log2(1.0f + max(luminance, 0.0f));
}

// Interleaved gradient noise (Jimenez), shifted by the golden ratio each frame: low-discrepancy in space and time.
float DenoiseIgn(float2 pixel, uint frame)
{
    pixel += 5.588238f * float(frame & 63u);
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

uint DenoiseHash(uint x)
{
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    x *= 0x846CA68Bu;
    x ^= x >> 16u;
    return x;
}

// Rounds non-negative values to a neighbouring fp16 value, up with probability equal to the fraction of the way there, so
// the store is exact and its expected value is the input's. An exponential moving average stored at fp16 otherwise loses
// up to an ulp a frame (the conversion may truncate), which over a 32-frame window adds up to a percent of darkening.
float3 DenoiseRoundToHalf(float3 value, float u)
{
#if DENOISE_STOCHASTIC_ROUNDING
    const uint3 nearest = f32tof16(value);
    const float3 rounded = f16tof32(nearest);
    const uint3 other = select(rounded < value, nearest + 1u, select(and(rounded > value, nearest > 0u), nearest - 1u, nearest));
    const float3 otherValue = f16tof32(other);
    const float3 span = otherValue - rounded;
    const float3 t = select(span != 0.0f, (value - rounded) / span, 0.0f);
    return select(u < t, otherValue, rounded);
#else
    return value;
#endif
}

struct DenoiseSurface
{
    bool valid;
    float depth; // Linear view depth.
    float3 normal;
    float roughness; // Perceptual.
    float3 position;
    // Floored: what demodulation divides by and remodulation multiplies by. Specular is demodulated by F0 alone: the
    // split-sum albedo (F0 * A + B) depends on N.V, which jitters with the normal map, and dividing by one frame's value
    // and multiplying by another's biased specular by +13%.
    float3 diffuseAlbedo;
    float3 specularAlbedo;
};

DenoiseSurface DenoiseLoadSurface(int2 pixel, PerFrameBuffer perFrame, Camera camera)
{
    DenoiseSurface surface = (DenoiseSurface)0;
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    const float linearZ = depthTexture[pixel];
    surface.valid = asuint(linearZ) != 0x7F7FFFFFu;
    if (!surface.valid)
    {
        return surface;
    }
    Texture2D<float4> baseColor = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::BaseColorOpacity)];
    Texture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    Texture2D<float4> specularAo = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::SpecularAo)];
    const float4 nr = normalRoughness[pixel];
    float2 uv = (float2(pixel) + 0.5f) / float2(perFrame.screenResX, perFrame.screenResY);
    uv.y = 1.0f - uv.y;
    const float2 ndc = uv * 2.0f - 1.0f;
    const float3 positionVS = float3(ndc.x * camera.projectionInverse[0][0], ndc.y * camera.projectionInverse[1][1], -1.0f) * linearZ;
    surface.position = mul(float4(positionVS, 1.0f), camera.viewInverse).xyz;
    surface.depth = linearZ;
    surface.normal = normalize(nr.xyz);
    surface.roughness = saturate(nr.w);
    surface.diffuseAlbedo = max(baseColor[pixel].rgb, DENOISE_ALBEDO_FLOOR);
    surface.specularAlbedo = max(specularAo[pixel].rgb, DENOISE_ALBEDO_FLOOR);
    return surface;
}

// ---------------------------------------------------------------------------------------------------------------------
// Temporal.

groupshared float3 gs_diffuse[DENOISE_REGION * DENOISE_REGION]; // Demodulated.
groupshared float3 gs_specular[DENOISE_REGION * DENOISE_REGION];
groupshared float3 gs_compressed[DENOISE_REGION * DENOISE_REGION]; // Diffuse, specular, valid.
groupshared float gs_bound[DENOISE_REGION * DENOISE_REGION]; // Selection's unshadowed bound (0 where invalid).
groupshared float4 gs_sampleSurface[DENOISE_SAMPLE_REGION * DENOISE_SAMPLE_REGION]; // Downsampled: normal, view depth (0: none).

struct DenoiseHistory
{
    float3 value; // Demodulated.
    float length;
    float2 moments; // Of the compressed luminance.
    float2 area; // The neighbourhood's compressed mean: slow (over the history's length) and fast averages.
};

// Change-aware rectification: how long the history may stay given how far the neighbourhood's fast average (this frame's
// mean included) has moved from its slow one. The noise of each is the variance of one frame's neighbourhood mean (the
// per-sample variance, as in DenoiseRectify, over the sample count) times an exponential average's factor a / (2 - a).
float DenoiseChangeLimit(DenoiseHistory history, float neighbourhoodMean, float neighbourhoodVariance, float count)
{
#if DENOISE_CHANGE_DETECT
    if (history.length > 0.0f)
    {
        const float fastWeight = max(DENOISE_CHANGE_FAST_WEIGHT, 1.0f / (history.length + 1.0f));
        const float slowWeight = 1.0f / history.length;
        const float fast = lerp(history.area.y, neighbourhoodMean, fastWeight);
        const float temporalVariance = max(history.moments.y - history.moments.x * history.moments.x, 0.0f);
        const float frameVariance = 0.5f * (neighbourhoodVariance + temporalVariance) / max(count, 1.0f);
        const float deviation = sqrt(frameVariance * (fastWeight / (2.0f - fastWeight) + slowWeight / (2.0f - slowWeight))) + 1e-3f;
        const float distance = abs(fast - history.area.x) / deviation;
        return lerp(1.0f, DENOISE_MAX_HISTORY, saturate((DENOISE_CHANGE_SIGMA_RESET - distance) / (DENOISE_CHANGE_SIGMA_RESET - DENOISE_CHANGE_SIGMA)));
    }
#endif
    return DENOISE_MAX_HISTORY;
}

// The averages after this frame, with the history's new length.
float2 DenoiseChangeBlend(float2 area, float neighbourhoodMean, float length)
{
    const float slowWeight = 1.0f / length;
    return float2(lerp(area.x, neighbourhoodMean, slowWeight), lerp(area.y, neighbourhoodMean, max(DENOISE_CHANGE_FAST_WEIGHT, slowWeight)));
}

// The unshadowed bound (DENOISE_BOUND above): scales both histories so that their remodulated luminance stays under the
// margin times the neighbourhood's largest bound.
void DenoiseApplyBound(inout DenoiseHistory diffuse, inout DenoiseHistory specular, DenoiseSurface surface, float boundMax)
{
#if DENOISE_BOUND
    const float modulated = DenoiseLuminance(diffuse.value * surface.diffuseAlbedo + specular.value * surface.specularAlbedo);
    const float limit = DENOISE_BOUND_MARGIN * boundMax + 1e-4f;
    if (modulated > limit)
    {
        const float scale = limit / modulated;
        diffuse.value *= scale;
        specular.value *= scale;
    }
#endif
}

// MegaLights' rectification, for a heavy-tailed signal: shortens the history by how far its compressed mean is from the
// neighbourhood's, in standard deviations of that difference (the neighbourhood mean's and the history's own); it never
// moves the value. Clamping linear values into a box of the neighbourhood's spread darkened by 2-11%: most 5x5
// neighbourhoods hold none of the rare bright samples, so their box sits below a converged history.
//
// The per-sample variance is the mean of the history's temporal one and the neighbourhood's spatial one, because either
// alone is lopsided under moving lights. Where a light has just left, the history's variance is large (it saw the light)
// and the neighbourhood's small; where one has just arrived, the reverse. The temporal one alone kept departed light and
// cut arriving light (1.4-1.7x too bright); the spatial one alone did the opposite (5% too dark). Choosing by direction
// (the neighbourhood's when the history is brighter, the history's when darker) was 6% too dark in motion.
//
// The compressed test is blind to a single bright sample, which barely moves a mean of log2(1 + Y); the spike limit
// after it catches those.
DenoiseHistory DenoiseRectify(DenoiseHistory history, float neighbourhoodMean, float neighbourhoodVariance, float count)
{
#if DENOISE_RECTIFY
    if (history.length > 0.0f)
    {
        const float temporalVariance = max(history.moments.y - history.moments.x * history.moments.x, 0.0f);
        const float variance = lerp(neighbourhoodVariance, temporalVariance, DENOISE_RECTIFY_TEMPORAL_WEIGHT);
        const float deviation = sqrt(variance / max(count, 1.0f) + variance / history.length) + 1e-3f;
        const float distance = abs(history.moments.x - neighbourhoodMean) / (DENOISE_RECTIFY_SIGMA * deviation);
        history.length = min(history.length, lerp(DENOISE_RECTIFY_MIN_LENGTH, DENOISE_MAX_HISTORY, saturate(2.0f - distance)));
    }
#endif
    return history;
}

[numthreads(DENOISE_TILE, DENOISE_TILE, 1)]
void StochasticDenoiseTemporalCS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    Texture2D<float4> noisyDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV];
    Texture2D<float4> noisySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV];
    const int2 screen = int2(perFrame.screenResX, perFrame.screenResY);
    const bool downsampled = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u;
    // The noisy inputs' grid: pixels, or downsampled the 2x2 blocks, each sampled at one pixel.
    const int2 grid = downsampled ? (screen + 1) / 2 : screen;
    const int regionSize = downsampled ? DENOISE_SAMPLE_REGION : DENOISE_REGION;

    // The tile's samples and a 2-sample border, demodulated by the albedo where each was taken, for the 5x5 neighbourhoods
    // (and, downsampled, for the reconstruction).
    const int2 regionOrigin = int2(groupId.xy) * (downsampled ? DENOISE_TILE / 2 : DENOISE_TILE) - DENOISE_BORDER;
    for (uint slot = groupIndex; slot < uint(regionSize * regionSize); slot += DENOISE_TILE * DENOISE_TILE)
    {
        const int2 cell = clamp(regionOrigin + int2(slot % uint(regionSize), slot / uint(regionSize)), int2(0, 0), grid - 1);
        const int2 coordinate = downsampled
            ? int2(StochasticDownsampledPixel(uint2(cell), STOCHASTIC_FRAME, 2u, uint2(screen)))
            : cell;
        const DenoiseSurface neighbour = DenoiseLoadSurface(coordinate, perFrame, camera);
        const float3 d = neighbour.valid ? max(noisyDiffuse[cell].rgb / neighbour.diffuseAlbedo, 0.0f) : 0.0f;
        const float3 s = neighbour.valid ? max(noisySpecular[cell].rgb / neighbour.specularAlbedo, 0.0f) : 0.0f;
        gs_diffuse[slot] = d;
        gs_specular[slot] = s;
        gs_compressed[slot] = float3(DenoiseCompress(DenoiseLuminance(d)), DenoiseCompress(DenoiseLuminance(s)), neighbour.valid ? 1.0f : 0.0f);
        gs_bound[slot] = neighbour.valid ? noisyDiffuse[cell].a : 0.0f;
        if (downsampled)
        {
            gs_sampleSurface[slot] = float4(neighbour.normal, neighbour.valid ? neighbour.depth : 0.0f);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 pixel = int2(id.xy);
    if (any(pixel >= screen))
    {
        return;
    }
    RWTexture2D<float4> historyDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_HISTORY_DIFFUSE_UAV];
    RWTexture2D<float4> historySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_HISTORY_SPECULAR_UAV];
    RWTexture2D<float4> historyMoments = ResourceDescriptorHeap[STOCHASTIC_DENOISE_HISTORY_MOMENTS_UAV];
    RWTexture2D<float2> varianceOutput = ResourceDescriptorHeap[STOCHASTIC_DENOISE_VARIANCE_UAV];
    RWTexture2D<float4> historyChange = ResourceDescriptorHeap[STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV];
    const DenoiseSurface surface = DenoiseLoadSurface(pixel, perFrame, camera);
    if (!surface.valid)
    {
        historyDiffuse[pixel] = 0.0f;
        historySpecular[pixel] = 0.0f;
        historyMoments[pixel] = 0.0f;
        historyChange[pixel] = 0.0f;
        varianceOutput[pixel] = 0.0f;
        return;
    }

    // ---- This frame's value, and the neighbourhood's mean and variance of the compressed luminance (over the 5x5 samples
    // around the pixel: pixels, or downsampled the samples of the 5x5 blocks around its own).
    const int2 centerCell = downsampled ? int2(threadId.xy) / 2 : int2(threadId.xy);
    const uint centerSlot = (centerCell.y + DENOISE_BORDER) * regionSize + centerCell.x + DENOISE_BORDER;
    uint sourceSlot = centerSlot;
    bool sourceValid = true;
    bool reconstructed = false;
    float3 reconstructedDiffuse = 0.0f, reconstructedSpecular = 0.0f;
    if (downsampled)
    {
        // Reconstruction: one of the four nearest samples, at random with probability proportional to its bilinear weight
        // times its agreement with this pixel's depth and normal. A choice rather than a weighted mean keeps the frame's
        // value a single sample, which the temporal average then averages, as at full resolution.
        const float2 cellPosition = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
        const float2 cellBase = floor(cellPosition);
        const float2 fraction = cellPosition - cellBase;
        const int2 baseSlot = int2(cellBase) - regionOrigin;
        float weights[4];
        float total = 0.0f;
        [unroll]
        for (uint tap = 0; tap < 4u; ++tap)
        {
            const int2 offset = int2(tap & 1u, tap >> 1u);
            const uint slot = uint(baseSlot.y + offset.y) * uint(regionSize) + uint(baseSlot.x + offset.x);
            const float4 sampleSurface = gs_sampleSurface[slot];
            const float bilinear = (offset.x ? fraction.x : 1.0f - fraction.x) * (offset.y ? fraction.y : 1.0f - fraction.y);
            const float depthWeight = saturate(1.0f - abs(sampleSurface.w - surface.depth) / (DENOISE_RECONSTRUCT_DEPTH * surface.depth));
            const float normalWeight = pow(saturate(dot(sampleSurface.xyz, surface.normal)), DENOISE_RECONSTRUCT_NORMAL_POWER);
            weights[tap] = sampleSurface.w > 0.0f ? bilinear * depthWeight * normalWeight : 0.0f;
            total += weights[tap];
        }
        sourceValid = total >= DENOISE_RECONSTRUCT_MIN_WEIGHT;
        const float u = float(DenoiseHash((uint(pixel.x) | (uint(pixel.y) << 16u)) ^ DenoiseHash(STOCHASTIC_FRAME * 0x9E3779B9u + 1u)) >> 8u) * (1.0f / 16777216.0f);
        float target = u * total;
        uint chosen = 3u;
        [unroll]
        for (uint tap = 0; tap < 3u; ++tap)
        {
            if (chosen == 3u && target < weights[tap])
            {
                chosen = tap;
            }
            target -= chosen == 3u ? weights[tap] : 0.0f;
        }
        sourceSlot = uint(baseSlot.y + int(chosen >> 1u)) * uint(regionSize) + uint(baseSlot.x + int(chosen & 1u));
#if DENOISE_RECONSTRUCT_MEAN
        if (sourceValid)
        {
            float3 meanDiffuse = 0.0f, meanSpecular = 0.0f;
            [unroll]
            for (uint tap = 0; tap < 4u; ++tap)
            {
                const uint slot = uint(baseSlot.y + int(tap >> 1u)) * uint(regionSize) + uint(baseSlot.x + int(tap & 1u));
                meanDiffuse += gs_diffuse[slot] * weights[tap];
                meanSpecular += gs_specular[slot] * weights[tap];
            }
            reconstructedDiffuse = meanDiffuse / total;
            reconstructedSpecular = meanSpecular / total;
            reconstructed = true;
        }
#endif
    }
    const float3 currentDiffuse = reconstructed ? reconstructedDiffuse : gs_diffuse[sourceSlot];
    const float3 currentSpecular = reconstructed ? reconstructedSpecular : gs_specular[sourceSlot];
    const float2 currentCompressed = gs_compressed[sourceSlot].xy;
    float2 sum = 0.0f, sum2 = 0.0f, peak = 0.0f, lit = 0.0f;
    float count = 0.0f, boundMax = 0.0f;
    [unroll]
    for (int y = -DENOISE_BORDER; y <= DENOISE_BORDER; ++y)
    {
        [unroll]
        for (int x = -DENOISE_BORDER; x <= DENOISE_BORDER; ++x)
        {
            const int neighbourSlot = (centerCell.y + DENOISE_BORDER + y) * regionSize + centerCell.x + DENOISE_BORDER + x;
            const float3 c = gs_compressed[neighbourSlot];
            boundMax = max(boundMax, gs_bound[neighbourSlot]);
            sum += c.xy * c.z;
            peak = max(peak, c.xy * c.z);
            lit += select(c.xy > 0.0f, c.z, 0.0f);
            sum2 += c.xy * c.xy * c.z;
            count += c.z;
        }
    }
    const float2 neighbourhoodMean = sum / max(count, 1.0f);
    const float2 neighbourhoodVariance = max(sum2 / max(count, 1.0f) - neighbourhoodMean * neighbourhoodMean, 0.0f);

    // ---- Last frame's history, by four bilinear taps each validated against the surface it was made for.
    DenoiseHistory previousDiffuse = (DenoiseHistory)0;
    DenoiseHistory previousSpecular = (DenoiseHistory)0;
    if ((STOCHASTIC_FLAGS & STOCHASTIC_FLAG_HISTORY_VALID) != 0u)
    {
        Texture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
        RWTexture2D<uint2> previousSurface = ResourceDescriptorHeap[STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV];
        RWTexture2D<float4> previousHistoryDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_DIFFUSE_UAV];
        RWTexture2D<float4> previousHistorySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_SPECULAR_UAV];
        RWTexture2D<float4> previousHistoryMoments = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_MOMENTS_UAV];
        RWTexture2D<float4> previousHistoryChange = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV];
        const float2 resolution = float2(screen);
        const float2 uv = (float2(pixel) + 0.5f) / resolution;
        const float2 previousNdc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f) - motion[pixel];
        const float2 previousPixel = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f) * resolution - 0.5f;
        const float2 base = floor(previousPixel);
        const float2 fraction = previousPixel - base;
        const float expectedDepth = -mul(float4(surface.position, 1.0f), camera.prevView).z;
        float weightSum = 0.0f;
        float4 diffuseSum = 0.0f, specularSum = 0.0f, momentsSum = 0.0f, changeSum = 0.0f;
        [unroll]
        for (uint tap = 0; tap < 4u; ++tap)
        {
            const int2 offset = int2(tap & 1u, tap >> 1u);
            const int2 coordinate = int2(base) + offset;
            if (any(coordinate < 0) || any(coordinate >= screen))
            {
                continue;
            }
            const uint2 stored = previousSurface[coordinate];
            if (stored.x == 0x7F7FFFFFu || abs(asfloat(stored.x) - expectedDepth) > DENOISE_DEPTH_TOLERANCE * max(expectedDepth, 1e-3f))
            {
                continue;
            }
            const float weight = (offset.x ? fraction.x : 1.0f - fraction.x) * (offset.y ? fraction.y : 1.0f - fraction.y);
            weightSum += weight;
            diffuseSum += previousHistoryDiffuse[coordinate] * weight;
            specularSum += previousHistorySpecular[coordinate] * weight;
            momentsSum += previousHistoryMoments[coordinate] * weight;
#if DENOISE_CHANGE_DETECT || DENOISE_BOUND_CHANGE
            changeSum += previousHistoryChange[coordinate] * weight;
#endif
        }
        if (weightSum > 0.05f)
        {
            diffuseSum /= weightSum;
            specularSum /= weightSum;
            momentsSum /= weightSum;
            changeSum /= weightSum;
            previousDiffuse.value = diffuseSum.rgb;
            previousDiffuse.length = diffuseSum.a;
            previousDiffuse.moments = momentsSum.xy;
            previousDiffuse.area = changeSum.xy;
            previousSpecular.value = specularSum.rgb;
            previousSpecular.length = specularSum.a;
            previousSpecular.moments = momentsSum.zw;
            previousSpecular.area = changeSum.zw;
        }
    }
    const float u = float(DenoiseHash((uint(pixel.x) | (uint(pixel.y) << 16u)) ^ DenoiseHash(STOCHASTIC_FRAME)) >> 8u) * (1.0f / 16777216.0f);
    float historyBound = boundMax; // The bound the history stands for (DENOISE_BOUND_CHANGE).
#if DENOISE_BOUND_CHANGE
    if (previousDiffuse.length > 0.0f || previousSpecular.length > 0.0f)
    {
        historyBound = previousDiffuse.area.x;
#if (DENOISE_BOUND_CHANGE & 2)
        // Drops within DENOISE_BOUND_CHANGE_TOLERANCE are the bound's own noise (the sampled pixel of each 2x2 moves every
        // frame, and the hidden lights are estimated): not scaled, or each dip would darken the history for good.
        const float fall = min(DENOISE_BOUND_CHANGE_TOLERANCE * (boundMax + DENOISE_BOUND_CHANGE_FLOOR) / (historyBound + DENOISE_BOUND_CHANGE_FLOOR), 1.0f);
        previousDiffuse.value *= fall;
        previousSpecular.value *= fall;
        historyBound *= fall;
#endif
#if (DENOISE_BOUND_CHANGE & 1)
        const float ratio = (max(boundMax, historyBound) + DENOISE_BOUND_CHANGE_FLOOR) / (min(boundMax, historyBound) + DENOISE_BOUND_CHANGE_FLOOR);
        const float cap = DENOISE_MAX_HISTORY / max(ratio / DENOISE_BOUND_CHANGE_TOLERANCE, 1.0f);
        previousDiffuse.length = min(previousDiffuse.length, cap);
        previousSpecular.length = min(previousSpecular.length, cap);
#endif
    }
#endif
    DenoiseApplyBound(previousDiffuse, previousSpecular, surface, boundMax);
    if (!sourceValid)
    {
        // Downsampled, and no sample this frame lies on this pixel's surface (thin geometry, an edge): the history alone,
        // unchanged, or nothing (length 0, which the spatial pass skips) until a sample lands on it.
        historyDiffuse[pixel] = float4(DenoiseRoundToHalf(min(previousDiffuse.value, DENOISE_FP16_LIMIT), u), previousDiffuse.length);
        historySpecular[pixel] = float4(DenoiseRoundToHalf(min(previousSpecular.value, DENOISE_FP16_LIMIT), u), previousSpecular.length);
        historyMoments[pixel] = float4(previousDiffuse.moments, previousSpecular.moments);
#if DENOISE_BOUND_CHANGE
        historyChange[pixel] = float4(historyBound, 0.0f, 0.0f, 0.0f);
#else
        historyChange[pixel] = float4(previousDiffuse.area, previousSpecular.area);
#endif
        const float2 historyVariance = max(float2(previousDiffuse.moments.y - previousDiffuse.moments.x * previousDiffuse.moments.x,
            previousSpecular.moments.y - previousSpecular.moments.x * previousSpecular.moments.x), 0.0f);
        const float2 historySlope = 0.6931472f * (1.0f + float2(DenoiseLuminance(previousDiffuse.value), DenoiseLuminance(previousSpecular.value)));
        varianceOutput[pixel] = min(historyVariance * historySlope * historySlope, DENOISE_FP16_LIMIT);
        return;
    }
    // The change test reads the history as reprojected, before the per-pixel test shortens it.
    const float changeLimitDiffuse = DenoiseChangeLimit(previousDiffuse, neighbourhoodMean.x, neighbourhoodVariance.x, count);
    const float changeLimitSpecular = DenoiseChangeLimit(previousSpecular, neighbourhoodMean.y, neighbourhoodVariance.y, count);
    previousDiffuse = DenoiseRectify(previousDiffuse, neighbourhoodMean.x, neighbourhoodVariance.x, count);
    previousSpecular = DenoiseRectify(previousSpecular, neighbourhoodMean.y, neighbourhoodVariance.y, count);
    previousDiffuse.length = min(previousDiffuse.length, changeLimitDiffuse);
    previousSpecular.length = min(previousSpecular.length, changeLimitSpecular);
#if DENOISE_SPIKE
    // A light that has just moved on leaves its spot in the history at 1/length of a sample that may be a hundred times
    // its surroundings: a visible dot per frame along its path. In the compressed luminance rectification compares,
    // that one sample barely moves the mean, so it is limited here in linear terms: the history may not exceed
    // DENOISE_SPIKE_LIMIT times the brightest of this frame's 5x5 samples.
    {
        // Only where most samples are lit: at a few samples per pixel about half of all pixels are black in a frame, and a
        // neighbourhood of black samples is no evidence that the history is too bright (limiting there punched black holes).
        // At 4x it costs about 2% in motion; at 2x, 3-6%.
        const float2 limit = select(lit >= DENOISE_SPIKE_LIT_FRACTION * count, DENOISE_SPIKE_LIMIT * (exp2(peak) - 1.0f) + 1e-4f, 1e30f);
        const float luminanceDiffuse = DenoiseLuminance(previousDiffuse.value);
        const float luminanceSpecular = DenoiseLuminance(previousSpecular.value);
        if (luminanceDiffuse > limit.x) previousDiffuse.value *= limit.x / luminanceDiffuse;
        if (luminanceSpecular > limit.y) previousSpecular.value *= limit.y / luminanceSpecular;
    }
#endif

    // ---- Blend: an exponential moving average with weight 1 / length, length capped by rectification.
    const float lengthDiffuse = min(previousDiffuse.length + 1.0f, DENOISE_MAX_HISTORY);
    const float lengthSpecular = min(previousSpecular.length + 1.0f, DENOISE_MAX_HISTORY);
    const float3 diffuse = lerp(previousDiffuse.value, currentDiffuse, 1.0f / lengthDiffuse);
    const float3 specular = lerp(previousSpecular.value, currentSpecular, 1.0f / lengthSpecular);
    const float2 momentsDiffuse = lerp(previousDiffuse.moments, float2(currentCompressed.x, currentCompressed.x * currentCompressed.x), 1.0f / lengthDiffuse);
    const float2 momentsSpecular = lerp(previousSpecular.moments, float2(currentCompressed.y, currentCompressed.y * currentCompressed.y), 1.0f / lengthSpecular);

    // The spatial filter's per-sample luminance variance, from the compressed one by the derivative of log2(1 + y) at the
    // history's luminance; the neighbourhood's while the history is short.
    const float2 compressedVariance = float2(
        lengthDiffuse < DENOISE_SHORT_HISTORY ? neighbourhoodVariance.x : max(momentsDiffuse.y - momentsDiffuse.x * momentsDiffuse.x, 0.0f),
        lengthSpecular < DENOISE_SHORT_HISTORY ? neighbourhoodVariance.y : max(momentsSpecular.y - momentsSpecular.x * momentsSpecular.x, 0.0f));
    const float2 slope = 0.6931472f * (1.0f + float2(DenoiseLuminance(diffuse), DenoiseLuminance(specular)));

    // Stored as fp16: a demodulated firefly would otherwise overflow to infinity.
    historyDiffuse[pixel] = float4(DenoiseRoundToHalf(min(diffuse, DENOISE_FP16_LIMIT), u), lengthDiffuse);
    historySpecular[pixel] = float4(DenoiseRoundToHalf(min(specular, DENOISE_FP16_LIMIT), u), lengthSpecular);
    historyMoments[pixel] = float4(momentsDiffuse, momentsSpecular);
#if DENOISE_CHANGE_DETECT
    historyChange[pixel] = float4(DenoiseChangeBlend(previousDiffuse.area, neighbourhoodMean.x, lengthDiffuse),
        DenoiseChangeBlend(previousSpecular.area, neighbourhoodMean.y, lengthSpecular));
#elif DENOISE_BOUND_CHANGE
    historyChange[pixel] = float4(min(lerp(historyBound, boundMax, 1.0f / lengthDiffuse), DENOISE_FP16_LIMIT), 0.0f, 0.0f, 0.0f);
#endif
    varianceOutput[pixel] = min(compressedVariance * slope * slope, DENOISE_FP16_LIMIT);
}

// ---------------------------------------------------------------------------------------------------------------------
// Spatial.

// Filter radius in pixels for a signal: wide where its relative noise is high, narrow where it has converged (so the
// upscaler keeps detail); doubled while history is short.
#define DENOISE_SPATIAL_RADIUS 8.0f
#define DENOISE_SPATIAL_FADE 8.0f
// By history length alone: 8 pixels for a new history, none from 8 frames on. A radius that follows the pixel's own
// (noisy) value or variance biases whichever way it leans (dim pixels widened and brightened by 1.3%; with a luminance
// stop, bright neighbours were rejected and specular darkened by 8%), and at convergence the filter only blurs detail.
float DenoiseRadius(float historyLength)
{
#if !DENOISE_SPATIAL
    return 0.0f;
#endif
    return max(DENOISE_SPATIAL_RADIUS * saturate((DENOISE_SPATIAL_FADE - historyLength) / (DENOISE_SPATIAL_FADE - 1.0f)), DENOISE_SPATIAL_MIN_RADIUS);
}

[numthreads(DENOISE_TILE, DENOISE_TILE, 1)]
void StochasticDenoiseSpatialCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const int2 screen = int2(perFrame.screenResX, perFrame.screenResY);
    const int2 pixel = int2(id.xy);
    if (any(pixel >= screen))
    {
        return;
    }
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    RWTexture2D<float4> outputDiffuse = ResourceDescriptorHeap[STOCHASTIC_DIFFUSE_UAV];
    RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[STOCHASTIC_SPECULAR_UAV];
    const DenoiseSurface surface = DenoiseLoadSurface(pixel, perFrame, camera);
    if (!surface.valid)
    {
        outputDiffuse[pixel] = 0.0f;
        outputSpecular[pixel] = 0.0f;
        return;
    }
    RWTexture2D<float4> historyDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_HISTORY_DIFFUSE_UAV];
    RWTexture2D<float4> historySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_HISTORY_SPECULAR_UAV];
    RWTexture2D<float2> varianceInput = ResourceDescriptorHeap[STOCHASTIC_DENOISE_VARIANCE_UAV];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    Texture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];

    const float4 centerDiffuse = historyDiffuse[pixel];
    const float4 centerSpecular = historySpecular[pixel];
    // The luminance stop's scale: the variance of one frame's input over the history length, the variance of the temporal
    // average itself.
    const float2 inputVariance = varianceInput[pixel];
    const float2 variance = inputVariance / max(float2(centerDiffuse.a, centerSpecular.a), 1.0f);
    const float centerLuminanceDiffuse = DenoiseLuminance(centerDiffuse.rgb);
    const float centerLuminanceSpecular = DenoiseLuminance(centerSpecular.rgb);
    const float radiusDiffuse = DenoiseRadius(centerDiffuse.a);
    // Glossy specular keeps its detail: its radius shrinks with roughness.
    const float radiusSpecular = DenoiseRadius(centerSpecular.a) * saturate(surface.roughness * 2.0f);
    const float sigmaDiffuse = 4.0f * sqrt(variance.x) + 1e-4f;
    const float sigmaSpecular = 4.0f * sqrt(variance.y) + 1e-4f;

    // One kernel, rotated per pixel and frame, instead of several a-trous iterations.
    const float angle = DenoiseIgn(float2(pixel), STOCHASTIC_FRAME) * 6.2831853f;
    const float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
    // A history of length 0 holds nothing yet (downsampled, a pixel no sample has reached): it takes no weight, here or as
    // a neighbour.
    float weightDiffuse = centerDiffuse.a > 0.0f ? 1.0f : 0.0f;
    float weightSpecular = centerSpecular.a > 0.0f ? 1.0f : 0.0f;
    float3 diffuse = centerDiffuse.rgb * weightDiffuse;
    float3 specular = centerSpecular.rgb * weightSpecular;
#if DENOISE_SPATIAL_FLOOR_TAPS
    const bool floorOnly = DENOISE_SPATIAL && radiusDiffuse <= DENOISE_SPATIAL_MIN_RADIUS && radiusSpecular <= DENOISE_SPATIAL_MIN_RADIUS;
#else
    const bool floorOnly = false;
#endif
    if (floorOnly)
    {
        const bool filterSpecular = radiusSpecular >= DENOISE_SPATIAL_MIN_RADIUS && DENOISE_SPATIAL_MIN_RADIUS >= 0.5f;
        const float2 axis = float2(cos(angle), sin(angle)) * DENOISE_SPATIAL_MIN_RADIUS;
        [unroll]
        for (uint tap = 0; tap < 4u; ++tap)
        {
            const float2 direction = tap == 0u ? axis : tap == 1u ? -axis : tap == 2u ? float2(-axis.y, axis.x) : float2(axis.y, -axis.x);
            const int2 coordinate = pixel + int2(round(direction));
            if (DENOISE_SPATIAL_MIN_RADIUS < 0.5f || any(coordinate < 0) || any(coordinate >= screen) || all(coordinate == pixel))
            {
                continue;
            }
            const float depth = depthTexture[coordinate];
            if (asuint(depth) == 0x7F7FFFFFu)
            {
                continue;
            }
            const float3 normal = normalize(normalRoughness[coordinate].xyz);
            const float geometry = exp(-abs(depth - surface.depth) / (0.02f * surface.depth + 1e-3f)) *
                pow(saturate(dot(normal, surface.normal)), 32.0f);
            const float4 valueDiffuse = historyDiffuse[coordinate];
            const float wd = valueDiffuse.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(valueDiffuse.rgb) - centerLuminanceDiffuse) / sigmaDiffuse) : 0.0f;
            diffuse += valueDiffuse.rgb * wd;
            weightDiffuse += wd;
            if (filterSpecular)
            {
                const float4 valueSpecular = historySpecular[coordinate];
                const float ws = valueSpecular.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(valueSpecular.rgb) - centerLuminanceSpecular) / sigmaSpecular) : 0.0f;
                specular += valueSpecular.rgb * ws;
                weightSpecular += ws;
            }
        }
    }
    else
    {
        [unroll]
        for (uint tap = 0; tap < 12u; ++tap)
        {
            const float2 direction = mul(rotation, kDenoisePoisson[tap]);
            [unroll]
            for (uint signal = 0; signal < 2u; ++signal)
            {
                const float radius = signal == 0u ? radiusDiffuse : radiusSpecular;
                if (radius < 0.5f)
                {
                    continue;
                }
                const int2 coordinate = pixel + int2(round(direction * radius));
                if (any(coordinate < 0) || any(coordinate >= screen) || all(coordinate == pixel))
                {
                    continue;
                }
                const float depth = depthTexture[coordinate];
                if (asuint(depth) == 0x7F7FFFFFu)
                {
                    continue;
                }
                const float3 normal = normalize(normalRoughness[coordinate].xyz);
                const float geometry = exp(-abs(depth - surface.depth) / (0.02f * surface.depth + 1e-3f)) *
                    pow(saturate(dot(normal, surface.normal)), 32.0f);
                if (signal == 0u)
                {
                    const float4 value = historyDiffuse[coordinate];
                    const float weight = value.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(value.rgb) - centerLuminanceDiffuse) / sigmaDiffuse) : 0.0f;
                    diffuse += value.rgb * weight;
                    weightDiffuse += weight;
                }
                else
                {
                    const float4 value = historySpecular[coordinate];
                    const float weight = value.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(value.rgb) - centerLuminanceSpecular) / sigmaSpecular) : 0.0f;
                    specular += value.rgb * weight;
                    weightSpecular += weight;
                }
            }
        }
    }
    diffuse /= max(weightDiffuse, 1e-6f);
    specular /= max(weightSpecular, 1e-6f);
    outputDiffuse[pixel] = float4(diffuse * surface.diffuseAlbedo, 1.0f);
    outputSpecular[pixel] = float4(specular * surface.specularAlbedo, 1.0f);
#ifdef DENOISE_DEBUG_PAIR
    // Diagnostic: this frame's denoised and noisy total luminance side by side, so one capture pairs them (with moving
    // lights, separate captures are separate frames).
    {
        Texture2D<float4> noisyDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV];
        Texture2D<float4> noisySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV];
        const float3 denoised = diffuse * surface.diffuseAlbedo + specular * surface.specularAlbedo;
        // Downsampled, the block's one sample stands in for each of its pixels.
        const int2 cell = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? pixel / 2 : pixel;
        const float3 noisy = noisyDiffuse[cell].rgb + noisySpecular[cell].rgb;
        // b: selection's unshadowed bound for this sample (stochasticSplit.hlsli), to check it against the noisy value.
        outputDiffuse[pixel] = float4(dot(denoised, float3(0.2126f, 0.7152f, 0.0722f)), dot(noisy, float3(0.2126f, 0.7152f, 0.0722f)), noisyDiffuse[cell].a, 1.0f);
        outputSpecular[pixel] = 0.0f;
    }
#endif
}
