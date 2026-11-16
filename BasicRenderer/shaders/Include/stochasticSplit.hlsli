#ifndef __STOCHASTIC_SPLIT_HLSLI__
#define __STOCHASTIC_SPLIT_HLSLI__

// What the estimator's three kernels pass between them (docs/stochastic_lighting.md "Kernel split"):
//
// - A sample slot per sample of each sampled pixel (STOCHASTIC_SLOTS_UAV, uint3, index pixel * sampleCount + k): the meta
//   word below, and the sample's unshadowed estimate f * scale for diffuse and specular, each as one shared-exponent word.
//   Selection writes every slot of every sampled pixel each frame; shading reads them.
// - A pixel word per sampled pixel (STOCHASTIC_PIXEL_WORDS_UAV): selection writes the surface-valid and debug bits and
//   clears the visibility bits; the trace ORs in bit k when slot k's ray reached its light.
// - A compacted ray list (STOCHASTIC_RAYS_UAV, uint3): the sampled pixel and slot, the direction (octahedral, 16 bits a
//   component) and the ray's length. The trace rebuilds the origin from the surface history (depth and normal), as
//   selection built it.

// Slot meta: the light's buffer index (20 bits), the emitter quadrant its ray takes (2 bits), and the flags.
#define STOCHASTIC_SLOT_QUADRANT_SHIFT 20u
#define STOCHASTIC_SLOT_USED 0x400000u // The slot holds a sample (otherwise it adds nothing).
#define STOCHASTIC_SLOT_RAY 0x800000u // Its visibility is the trace's bit; without it the sample is visible (no shadowCaster,
                                     // or the surface is inside or against the emitter).

// Pixel word.
#define STOCHASTIC_PIXEL_VISIBLE_MASK 0xFu // Bit k: slot k's ray reached its light.
// Bits 4-6 (STOCHASTIC_PIXEL_GRADIENT_SHIFT, stochasticLightingRootConstants.h): with visibility gradients, the slot selection
// recorded for its cell at this pixel + 1, so shading can fill in the record's visibility.
#define STOCHASTIC_PIXEL_LIST_SHIFT 8u // STOCHASTIC_DEBUG_PROBE 5: the reprojected list's length + 1 (0: reprojection failed).
#define STOCHASTIC_PIXEL_VALID 0x40000000u // The sampled pixel has a surface.
#define STOCHASTIC_PIXEL_DEBUG 0x80000000u // Selection wrote a debug probe's output itself; shading leaves the pixel alone.
// Bits 16-29: the unshadowed bound (docs/performance/stochastic-lighting-experiments.md "Moving-light trails"), log2(1 + B) in 1/512 steps,
// rounded up. B is selection's estimate of the pixel's unshadowed light (luminance, albedo included): the linear sum of its
// candidates' importances, the hidden ones scaled by M / drawCount. Shading writes it to the noisy diffuse input's alpha,
// and the denoiser keeps the history under it, so light that has left a pixel can't linger there.
#define STOCHASTIC_PIXEL_BOUND_SHIFT 16u
#define STOCHASTIC_PIXEL_BOUND_MASK 0x3FFFu

uint StochasticEncodeBound(float bound)
{
    return min(uint(ceil(log2(1.0f + max(bound, 0.0f)) * 512.0f)), STOCHASTIC_PIXEL_BOUND_MASK) << STOCHASTIC_PIXEL_BOUND_SHIFT;
}

float StochasticDecodeBound(uint pixelWord)
{
    return exp2(float((pixelWord >> STOCHASTIC_PIXEL_BOUND_SHIFT) & STOCHASTIC_PIXEL_BOUND_MASK) * (1.0f / 512.0f)) - 1.0f;
}

// A ray entry's first word: the full-resolution pixel (x 16 bits, y 14 bits) and the slot (2 bits).
uint StochasticPackRayPixel(uint2 pixel, uint slot)
{
    return (pixel.x & 0xFFFFu) | ((pixel.y & 0x3FFFu) << 16u) | (slot << 30u);
}

uint2 StochasticRayPixel(uint packed)
{
    return uint2(packed & 0xFFFFu, (packed >> 16u) & 0x3FFFu);
}

uint StochasticRaySlot(uint packed)
{
    return packed >> 30u;
}

// Radiance as 9-bit mantissas with a shared 5-bit exponent, value = m * 2^(e - STOCHASTIC_RGB9E5_BIAS). The bias puts the
// largest value near 2^20 (a firefly's f / p can be thousands of times the mean) and the smallest step at 2^-20. The
// exponent depends on the value alone and the mantissas are rounded stochastically with one number, so the decoded
// value's expectation is the input's, small values included: the estimate stays unbiased through the packing.
#define STOCHASTIC_RGB9E5_BIAS 20
uint StochasticPackRgb9e5(float3 rgb, float rounding)
{
    const float largest = 511.0f * exp2(31.0f - STOCHASTIC_RGB9E5_BIAS);
    rgb = clamp(rgb, 0.0f, largest);
    const float brightest = max(rgb.r, max(rgb.g, rgb.b));
    // The exponent that puts the brightest channel's mantissa in [255.5, 511), so rounding up stays within 9 bits.
    int exponent = clamp(int(floor(log2(max(brightest, 1e-30f)))) - 8 + STOCHASTIC_RGB9E5_BIAS, 0, 31);
    float step = exp2(float(exponent - STOCHASTIC_RGB9E5_BIAS));
    if (brightest >= 511.0f * step && exponent < 31)
    {
        exponent += 1;
        step *= 2.0f;
    }
    const uint3 mantissa = (uint3)min(floor(rgb / step + rounding), 511.0f);
    return mantissa.r | (mantissa.g << 9u) | (mantissa.b << 18u) | (uint(exponent) << 27u);
}

float3 StochasticUnpackRgb9e5(uint packed)
{
    const float step = exp2(float(int(packed >> 27u) - STOCHASTIC_RGB9E5_BIAS));
    return float3(packed & 0x1FFu, (packed >> 9u) & 0x1FFu, (packed >> 18u) & 0x1FFu) * step;
}

// A debug probe's value for a sampled pixel, written where the shading kernel writes its output without accumulation: the
// pixel, the noisy input's texel for its block (denoised, downsampled), or every pixel of its block (downsampled, no
// denoiser). For the kernels that compute a probe's value themselves.
void StochasticWriteDebugOutput(uint2 block, uint2 pixel, uint downsample, uint2 screen, float3 diffuse, float3 specular)
{
    RWTexture2D<float4> outputDiffuse = ResourceDescriptorHeap[STOCHASTIC_DIFFUSE_UAV];
    RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[STOCHASTIC_SPECULAR_UAV];
    if (downsample <= 1u || (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_DENOISED) != 0u)
    {
        const uint2 target = downsample > 1u ? block : pixel;
        outputDiffuse[target] = float4(diffuse, 1.0f);
        outputSpecular[target] = float4(specular, 1.0f);
        return;
    }
    [unroll]
    for (uint q = 0u; q < 4u; ++q)
    {
        const uint2 other = block * 2u + uint2(q & 1u, q >> 1u);
        if (all(other < screen))
        {
            outputDiffuse[other] = float4(diffuse, 1.0f);
            outputSpecular[other] = float4(specular, 1.0f);
        }
    }
}

#endif // __STOCHASTIC_SPLIT_HLSLI__
