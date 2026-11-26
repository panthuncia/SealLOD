#pragma once

// Stochastic lighting's passes (src/Lighting/Stochastic, shaders/Stochastic). They trace through clodRayTracingHits.hlsli, so
// they keep the secondary rays' TLAS, CLAS info, frame and instance rows slots (clodRayTracingSetupRootConstants.h:
// CLOD_RT_SEC_TLAS_SRV 0, CLOD_RT_SEC_CLAS_INFO_SRV 1, CLOD_RT_SEC_FRAME 4, CLOD_RT_SEC_INSTANCE_ROWS_SRV 13) and use the
// others below 21, where the lighting toggles begin.

#define STOCHASTIC_DIFFUSE_UAV UintRootConstant2
#define STOCHASTIC_SPECULAR_UAV UintRootConstant3
#define STOCHASTIC_FRAME UintRootConstant4 // The frame number (CLOD_RT_SEC_FRAME's slot).
#define STOCHASTIC_FLAGS UintRootConstant5
#define STOCHASTIC_ACCUMULATION_DIFFUSE_UAV UintRootConstant6
#define STOCHASTIC_ACCUMULATION_SPECULAR_UAV UintRootConstant7
#define STOCHASTIC_SAMPLE_COUNT UintRootConstant8 // Lights drawn per pixel, 1 to STOCHASTIC_MAX_SAMPLE_COUNT.
// Guiding (docs/stochastic_lighting.md "Guiding"). Each pair is this frame's (written) and last frame's (read); the pass swaps
// them every frame.
#define STOCHASTIC_VISIBLE_LISTS_UAV UintRootConstant9 // Per 8x8 tile: a count, then up to STOCHASTIC_VISIBLE_LIST_CAPACITY entries.
#define STOCHASTIC_PREVIOUS_VISIBLE_LISTS_UAV UintRootConstant10
#define STOCHASTIC_SURFACE_HISTORY_UAV UintRootConstant11 // R32G32_UInt: linear depth bits, octahedral normal.
#define STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV UintRootConstant12
// Reduced-resolution sampling (docs/stochastic_lighting.md "Reduced resolution"): 1, or 2 for one sampled pixel per 2x2. The
// estimator and both denoiser passes read it.
#define STOCHASTIC_DOWNSAMPLE_FACTOR UintRootConstant15
// The estimator's three kernels (docs/stochastic_lighting.md "Kernel split"): selection writes each sampled pixel's sample
// slots and pixel word and appends its shadow rays; the trace marks the visible ones in the pixel word; shading sums them.
#define STOCHASTIC_SLOTS_UAV UintRootConstant16 // Per sampled pixel, STOCHASTIC_SAMPLE_COUNT uint3 slots (stochasticSplit.hlsli).
#define STOCHASTIC_PIXEL_WORDS_UAV UintRootConstant17 // Per sampled pixel: visibility bits, surface valid, debug.
#define STOCHASTIC_RAYS_UAV UintRootConstant18 // The compacted shadow rays, uint3 each.
#define STOCHASTIC_RAY_COUNTER_UAV UintRootConstant19 // [0] the rays appended this frame, [1] the count the trace reads.
#define STOCHASTIC_RAY_ARGS_UAV UintRootConstant20 // The trace's indirect dispatch arguments.
#define STOCHASTIC_RAY_CAPACITY UintRootConstant14 // Entries the ray list holds.

// The denoiser (shaders/Stochastic/stochasticDenoise.hlsl) traces nothing; it keeps the output, frame, flags and surface
// history slots above and uses the rest.
#define STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV UintRootConstant0
#define STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV UintRootConstant1
#define STOCHASTIC_DENOISE_HISTORY_DIFFUSE_UAV UintRootConstant6
#define STOCHASTIC_DENOISE_HISTORY_SPECULAR_UAV UintRootConstant7
#define STOCHASTIC_DENOISE_HISTORY_MOMENTS_UAV UintRootConstant8
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_DIFFUSE_UAV UintRootConstant9
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_SPECULAR_UAV UintRootConstant10
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_MOMENTS_UAV UintRootConstant13
#define STOCHASTIC_DENOISE_VARIANCE_UAV UintRootConstant14
// The change history (RGBA16F, ping-ponged), per pixel: with guide-normalized accumulation the history's average guide as log2
// (b, a: diffuse, specular) and, paired, the unshadowed history H_X (r, g) (docs/stochastic_lighting.md "Denoiser"); with a
// legacy switch instead (docs/stochastic_lighting.md "Denoiser switches"), DENOISE_CHANGE_DETECT's slow and fast averages of the
// neighbourhood's compressed mean, DENOISE_BOUND_CHANGE's bound or DENOISE_RECONSTRUCT_MOMENTS's moments.
#define STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV UintRootConstant11
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV UintRootConstant16
// The guide (docs/stochastic_lighting.md "Guide-normalized accumulation"), read by both denoiser passes.
#define STOCHASTIC_DENOISE_GUIDE_SRV UintRootConstant17
// The spatial pass: this frame's surface history (selection's STOCHASTIC_SURFACE_HISTORY_UAV), its taps' depth and normal
// (DENOISE_SPATIAL_SURFACE_HISTORY; the temporal pass's STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV slot).
#define STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV UintRootConstant12
// Light-mixture confidence (docs/stochastic_lighting.md "Guide-normalized accumulation", "Overhaul phase 5"), temporal pass only: this
// frame's light-mixture signature (the guide pass writes it), and the history's mixture, this frame's (written) and last frame's
// (read); per 4x4 block in their top-left corner with STOCHASTIC_SIGNATURE_LOWRES (below), else per pixel.
#define STOCHASTIC_DENOISE_SIGNATURE_SRV UintRootConstant18
#define STOCHASTIC_DENOISE_MIXTURE_UAV UintRootConstant19
#define STOCHASTIC_DENOISE_PREVIOUS_MIXTURE_UAV UintRootConstant20

// Visibility gradients (STOCHASTIC_FLAG_GRADIENT; docs/stochastic_lighting.md "Visibility gradients"):
// one buffer of uints (layout below), in the slot after the estimator's last (21-23 are free in the estimator's kernels and the
// denoiser: the lighting passes' switches begin at 24). Selection writes this frame's records, the trace's gradient kernel
// re-traces last frame's and scatters the gradient, shading fills in the records' visibility and keeps the held gradient, and
// the temporal pass reads that.
#define STOCHASTIC_GRADIENT_UAV UintRootConstant21
#define STOCHASTIC_DENOISE_GRADIENT_UAV UintRootConstant21

// The guide pass (shaders/Stochastic/stochasticGuide.hlsl) writes it; shading reads it in the accumulation's slot, which
// shading uses only without the denoiser (STOCHASTIC_FLAG_ACCUMULATE and STOCHASTIC_FLAG_DENOISED never meet).
#define STOCHASTIC_GUIDE_UAV UintRootConstant2
#define STOCHASTIC_SHADE_GUIDE_SRV UintRootConstant6
// The guide pass's light-mixture signature (STOCHASTIC_FLAG_MIXTURE).
#define STOCHASTIC_GUIDE_SIGNATURE_UAV UintRootConstant3

#ifndef STOCHASTIC_MAX_SAMPLE_COUNT
#define STOCHASTIC_MAX_SAMPLE_COUNT 4u
#endif
#define STOCHASTIC_TILE_SIZE 8u
// The trace kernel's group: one compacted ray per thread.
#define STOCHASTIC_TRACE_GROUP_SIZE 64u
// A tile's visible list (docs/stochastic_lighting.md "Guiding"): the buffers hold STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY
// entries per tile, and the estimator keeps at most STOCHASTIC_VISIBLE_LIST_CAPACITY of them (the most important), which
// pso.recompile can lower or raise up to the maximum.
#define STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY 64u
#ifndef STOCHASTIC_VISIBLE_LIST_CAPACITY
#define STOCHASTIC_VISIBLE_LIST_CAPACITY 32u
#endif
#define STOCHASTIC_VISIBLE_LIST_STRIDE (STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY + 1u)

// The running average starts over this frame: the camera, a light or the BVH changed.
#define STOCHASTIC_FLAG_RESTART_ACCUMULATION 1u
// The estimator keeps a running average too (the reference always does), to compare with the reference.
#define STOCHASTIC_FLAG_ACCUMULATE 2u
// Guide selection with last frame's visible lights, and write this frame's.
#define STOCHASTIC_FLAG_GUIDING 4u
// Last frame's visible lists and surface history were written by this pass (not on its first frame after a graph build).
#define STOCHASTIC_FLAG_HISTORY_VALID 8u
// The estimator writes the denoiser's noisy inputs, which are at the sampling resolution (one texel per sampled pixel).
#define STOCHASTIC_FLAG_DENOISED 16u
// Guide-normalized accumulation (stochasticLightingNormalize; docs/stochastic_lighting.md "Guide-normalized accumulation"): the guide pass
// runs, shading divides the noisy inputs by the guide at the sampled pixel, and the denoiser multiplies by it at the output
// pixel. Only with STOCHASTIC_FLAG_DENOISED.
#define STOCHASTIC_FLAG_NORMALIZE 32u
// The paired ratio (stochasticLightingPaired; docs/stochastic_lighting.md "The paired ratio"), only with
// STOCHASTIC_FLAG_NORMALIZE: shading also writes each lobe's unshadowed estimate from the same samples (every used slot,
// visible or not) over the guide into the noisy inputs' alphas, and the denoiser divides the filtered ratio by that
// estimate's filtered history before multiplying by the guide.
#define STOCHASTIC_FLAG_PAIRED 64u
// Light-mixture confidence (stochasticLightingMixtureConfidence; docs/stochastic_lighting.md "Guide-normalized accumulation"), only with
// STOCHASTIC_FLAG_NORMALIZE: the guide pass also writes which lights its unshadowed light comes from (a signature of shares in
// STOCHASTIC_SIGNATURE_BUCKETS buckets by light), and the temporal pass shortens the history by how far that mixture has
// moved from the one the history was made under.
#define STOCHASTIC_FLAG_MIXTURE 128u
// Visibility gradients (stochasticLightingVisibilityGradient; docs/stochastic_lighting.md "Visibility gradients"), only with STOCHASTIC_FLAG_NORMALIZE: per cell of STOCHASTIC_GRADIENT_CELL x STOCHASTIC_GRADIENT_CELL sampled
// pixels, selection records one traced sample (light and generation, its point in the emitter's [0,1)^2 domain, the ray's origin,
// the light's share of the pixel's unshadowed light), shading adds whether its ray reached the light, and next frame the trace
// re-traces that sample from the same origin toward the light where it is now. g = |V_now - V_then| x share goes to the cell the
// origin projects to; shading takes the 3x3 cells' maximum and holds it with decay; the temporal pass shortens the history by it.
// The extra rays only read the estimator's state: no slot, pixel word bit or ray list entry changes.
#define STOCHASTIC_FLAG_GRADIENT 256u
// Last frame's records were written (the estimator ran with gradients the frame before), so the trace may re-trace them and
// shading may hold last frame's gradient.
#define STOCHASTIC_FLAG_GRADIENT_VALID 512u

// The gradient buffer's cells (sampled pixels per side): 8, a selection tile (16x16 pixels at reduced resolution), or 4. Set
// it on Stochastic.Select.PSO, Stochastic.TraceGradient.PSO, Stochastic.Shade.PSO and Stochastic.DenoiseTemporal.PSO together.
// The buffer is sized for STOCHASTIC_GRADIENT_MIN_CELL.
#ifndef STOCHASTIC_GRADIENT_CELL
#define STOCHASTIC_GRADIENT_CELL 8u
#endif
#define STOCHASTIC_GRADIENT_MIN_CELL 4u
#define STOCHASTIC_GRADIENT_RECORD_WORDS 8u
// The raw gradient per cell, as the trace scatters it: the largest |V_now - V_then| x share (float bits), the signed sum of
// (V_now - V_then) x share in units of 1 / STOCHASTIC_GRADIENT_FIXED, and the records re-traced into the cell.
#define STOCHASTIC_GRADIENT_RAW_WORDS 3u
#define STOCHASTIC_GRADIENT_FIXED 4096.0f
// Words per cell of capacity: two records' sections (by frame parity), the raw gradient, two held gradients (by parity).
#define STOCHASTIC_GRADIENT_WORDS_PER_CELL (2u * STOCHASTIC_GRADIENT_RECORD_WORDS + STOCHASTIC_GRADIENT_RAW_WORDS + 2u)
// Record word 0: the light's index (20 bits), its generation's low 8 bits, the slot (2 bits), then shading's two bits.
#define STOCHASTIC_GRADIENT_COMPLETE 0x40000000u // Shading has filled in the visibility.
#define STOCHASTIC_GRADIENT_VISIBLE 0x80000000u // The ray reached the light.
// Pixel word bits 4-6 (stochasticSplit.hlsli): the slot selection recorded at this pixel + 1 (0: none).
#define STOCHASTIC_PIXEL_GRADIENT_SHIFT 4u

#ifndef __cplusplus
// The gradient buffer's capacity in cells (at STOCHASTIC_GRADIENT_MIN_CELL) and its sections' word offsets. sampled: the
// sampled pixels' grid. The C++ sizes it the same way (StochasticLightingResources).
uint StochasticGradientCapacity(uint2 sampled)
{
    const uint2 cells = (sampled + STOCHASTIC_GRADIENT_MIN_CELL - 1u) / STOCHASTIC_GRADIENT_MIN_CELL;
    return cells.x * cells.y;
}
uint2 StochasticGradientCells(uint2 sampled)
{
    return (sampled + STOCHASTIC_GRADIENT_CELL - 1u) / STOCHASTIC_GRADIENT_CELL;
}
uint StochasticGradientRecordBase(uint capacity, uint parity, uint cell)
{
    return (parity * capacity + cell) * STOCHASTIC_GRADIENT_RECORD_WORDS;
}
uint StochasticGradientRawIndex(uint capacity, uint cell)
{
    return 2u * STOCHASTIC_GRADIENT_RECORD_WORDS * capacity + cell * STOCHASTIC_GRADIENT_RAW_WORDS;
}
uint StochasticGradientHeldIndex(uint capacity, uint parity, uint cell)
{
    return (2u * STOCHASTIC_GRADIENT_RECORD_WORDS + STOCHASTIC_GRADIENT_RAW_WORDS + parity) * capacity + cell;
}
#endif

#ifndef __cplusplus
// Reduced-resolution sampling (docs/stochastic_lighting.md "Reduced resolution"). With factor 2, each 2x2 block of pixels is
// sampled at one pixel a frame. Over each 2x2 of blocks the four chosen pixels form a four-rooks pattern (one in each row and
// column of the 4x4 pixels), and the whole pattern is flipped per frame within the blocks by one of four offsets, so each
// pixel is sampled exactly once in each cycle of four frames; the order of the four changes per cycle. The estimator and
// the denoiser both call this, so they agree on where each sample lies.
uint2 StochasticDownsampledPixel(uint2 block, uint frame, uint downsample, uint2 screen)
{
    if (downsample <= 1u)
    {
        return block;
    }
    // Rooks offsets for the blocks (0,0), (1,0), (0,1), (1,1) of each 2x2: rows 0..3 of the 4x4 take columns 1, 3, 0, 2.
    const uint rooks = (block.x & 1u) | ((block.y & 1u) << 1u);
    const uint2 base = uint2((0x3u >> rooks) & 1u, (0xAu >> rooks) & 1u); // (1,0) (1,1) (0,0) (0,1)
    // Consecutive frames take diagonally opposite offsets (0, 3, 1, 2), xored and optionally transposed per cycle.
    uint cycle = (frame >> 2u) * 0x9E3779B1u;
    cycle ^= cycle >> 15u;
    cycle *= 0x85EBCA77u;
    cycle ^= cycle >> 13u;
    uint flip = ((0x9Cu >> ((frame & 3u) * 2u)) & 3u) ^ (cycle & 3u); // 0x9C: 0, 3, 1, 2 as 2-bit fields.
    if ((cycle & 4u) != 0u)
    {
        flip = ((flip & 1u) << 1u) | (flip >> 1u);
    }
    const uint2 offset = base ^ uint2(flip & 1u, flip >> 1u);
    return min(block * 2u + offset, screen - 1u);
}

// The index of this frame among the visits of the pixel that StochasticDownsampledPixel picks: the per-pixel random sequences
// advance once per visit, so each pixel's own sequence stays consecutive.
uint StochasticDownsampledVisit(uint frame, uint downsample)
{
    return downsample <= 1u ? frame : frame >> 2u;
}

// The denoiser's domain, which the guide (stochasticGuide.hlsl) is computed in: diffuse radiance divided by
// max(Surface::BaseColorOpacity.rgb, floor), specular by max(Surface::SpecularAo.rgb, floor), and luminance with weights
// (0.25, 0.5, 0.25).
#define STOCHASTIC_DEMODULATION_FLOOR 0.02f
float StochasticDemodulatedLuminance(float3 value)
{
    return dot(value, float3(0.25f, 0.5f, 0.25f));
}
// A guide at or below this (no light reaches the pixel, or the lobe is black) normalizes to 0 and remodulates to 0.
#define STOCHASTIC_GUIDE_EPSILON 1e-7f

// The light-mixture signature (STOCHASTIC_FLAG_MIXTURE; docs/stochastic_lighting.md "Guide-normalized accumulation"): the shares of a pixel's
// unshadowed light that come from each of 8 buckets of lights, a byte each (share * 255), packed four to a word in a uint2.
// A light always falls in the same bucket (its light buffer index and generation decide it; a reused slot moves to another
// bucket), so a light's share moving between frames moves its bucket's share continuously: no ranking, no dominant-light
// choice that could flip. Lights that share a bucket are one light to the signature (less sensitive, never a false change).
// Consecutive light indices of one generation fall in different buckets, up to 8 of them.
#define STOCHASTIC_SIGNATURE_BUCKETS 8u
// 1: the signature and the history's mixture are kept per block of STOCHASTIC_SIGNATURE_BLOCK x STOCHASTIC_SIGNATURE_BLOCK pixels
// (docs/performance/stochastic-lighting-experiments.md "Overhaul phase 5"), in the top-left (width / 4) x (height / 4) texels of their full-resolution
// textures: the guide pass averages its pixels' shares into the block's, and the temporal pass compares one reprojected block
// mixture per block and gives its confidence to the block's 16 pixels. The block average does what phase 4's 5x5 tent did
// (removes the shares' pixel-scale normal-map detail) for a sixteenth of the loads. 0: per pixel with the tent (phase 4). The
// guide pass and the temporal pass must agree: pso.recompile sets it on Stochastic.Guide.PSO and Stochastic.DenoiseTemporal.PSO.
#ifndef STOCHASTIC_SIGNATURE_LOWRES
#define STOCHASTIC_SIGNATURE_LOWRES 1
#endif
#define STOCHASTIC_SIGNATURE_BLOCK 4
uint StochasticSignatureBucket(uint lightIndex, uint generation)
{
    return (lightIndex + 5u * generation) & (STOCHASTIC_SIGNATURE_BUCKETS - 1u);
}
// Packs shares (buckets 0-3 in low, 4-7 in high; each in [0, 1]) rounded down after adding dither in [0, 1) (0.5 rounds to
// nearest; a uniform random number rounds stochastically, so an average kept in this form keeps its expectation).
uint StochasticPackShares(float4 shares, float4 dither)
{
    const uint4 q = uint4(min(floor(saturate(shares) * 255.0f + dither), 255.0f));
    return q.x | (q.y << 8u) | (q.z << 16u) | (q.w << 24u);
}
uint2 StochasticPackSignature(float4 low, float4 high, float4 ditherLow, float4 ditherHigh)
{
    return uint2(StochasticPackShares(low, ditherLow), StochasticPackShares(high, ditherHigh));
}
uint StochasticPackNormal(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.z >= 0.0f ? n.xy : (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    const uint2 q = (uint2)round(saturate(e * 0.5f + 0.5f) * 65535.0f);
    return q.x | (q.y << 16u);
}

float3 StochasticUnpackNormal(uint packed)
{
    const float2 e = float2(packed & 0xFFFFu, packed >> 16u) / 65535.0f * 2.0f - 1.0f;
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(n);
}
float4 StochasticUnpackShares(uint word)
{
    return float4((uint4(word, word, word, word) >> uint4(0u, 8u, 16u, 24u)) & 0xFFu) * (1.0f / 255.0f);
}
// How much two mixtures (unpacked shares, buckets 0-3 and 4-7) have in common: the sum over buckets of the smaller share, over
// the larger total (1 - the total variation distance between them). 1 for the same mixture; 1 where either holds no light
// (nothing to compare).
float StochasticMixtureOverlap(float4 lowA, float4 highA, float4 lowB, float4 highB)
{
    const float totalA = dot(lowA + highA, 1.0f);
    const float totalB = dot(lowB + highB, 1.0f);
    if (totalA <= 0.0f || totalB <= 0.0f)
    {
        return 1.0f;
    }
    return saturate(dot(min(lowA, lowB) + min(highA, highB), 1.0f) / max(totalA, totalB));
}
#endif

#ifndef __cplusplus
// The run's frame number and history parity (M6 step 9c). The passes are recorded once and replayed (kept recordings), so their
// root constants are the same every run: the frame number comes from the per-frame buffer (PerFrameBuffer::frameIndex is the
// frame number), and each ping-ponged pair's descriptors are recorded as [0] in the "this frame" slot and [1] in the "last
// frame" slot, which the shaders swap on odd frames. The sampling kernels' pairs use the slots above; the denoiser's are below.
uint StochasticRunFrame()
{
    ConstantBuffer<PerFrameBuffer> stochasticPerFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    return stochasticPerFrame.frameIndex;
}
// even on even frames, odd on odd frames.
#define STOCHASTIC_PARITY_PICK(even, odd) (((StochasticRunFrame() & 1u) != 0u) ? (odd) : (even))
#undef STOCHASTIC_FRAME
#define STOCHASTIC_FRAME StochasticRunFrame()
#ifdef CLOD_RT_SEC_FRAME
#undef CLOD_RT_SEC_FRAME
#define CLOD_RT_SEC_FRAME StochasticRunFrame()
#endif
#undef STOCHASTIC_VISIBLE_LISTS_UAV
#undef STOCHASTIC_PREVIOUS_VISIBLE_LISTS_UAV
#undef STOCHASTIC_SURFACE_HISTORY_UAV
#undef STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV
#define STOCHASTIC_VISIBLE_LISTS_UAV STOCHASTIC_PARITY_PICK(UintRootConstant9, UintRootConstant10)
#define STOCHASTIC_PREVIOUS_VISIBLE_LISTS_UAV STOCHASTIC_PARITY_PICK(UintRootConstant10, UintRootConstant9)
#define STOCHASTIC_SURFACE_HISTORY_UAV STOCHASTIC_PARITY_PICK(UintRootConstant11, UintRootConstant12)
#define STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV STOCHASTIC_PARITY_PICK(UintRootConstant12, UintRootConstant11)
// The denoiser's pairs: [0] in the slot named above, [1] in the previous slot (the moments' [1] in 13, the change history's in 16,
// the mixture's in 20), and the surface history's [0] in 12 and [1] in 22. Both stages are recorded with the same slots.
#undef STOCHASTIC_DENOISE_HISTORY_DIFFUSE_UAV
#undef STOCHASTIC_DENOISE_HISTORY_SPECULAR_UAV
#undef STOCHASTIC_DENOISE_HISTORY_MOMENTS_UAV
#undef STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV
#undef STOCHASTIC_DENOISE_MIXTURE_UAV
#undef STOCHASTIC_DENOISE_PREVIOUS_HISTORY_DIFFUSE_UAV
#undef STOCHASTIC_DENOISE_PREVIOUS_HISTORY_SPECULAR_UAV
#undef STOCHASTIC_DENOISE_PREVIOUS_HISTORY_MOMENTS_UAV
#undef STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV
#undef STOCHASTIC_DENOISE_PREVIOUS_MIXTURE_UAV
#undef STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV
#define STOCHASTIC_DENOISE_HISTORY_DIFFUSE_UAV STOCHASTIC_PARITY_PICK(UintRootConstant6, UintRootConstant9)
#define STOCHASTIC_DENOISE_HISTORY_SPECULAR_UAV STOCHASTIC_PARITY_PICK(UintRootConstant7, UintRootConstant10)
#define STOCHASTIC_DENOISE_HISTORY_MOMENTS_UAV STOCHASTIC_PARITY_PICK(UintRootConstant8, UintRootConstant13)
#define STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV STOCHASTIC_PARITY_PICK(UintRootConstant11, UintRootConstant16)
#define STOCHASTIC_DENOISE_MIXTURE_UAV STOCHASTIC_PARITY_PICK(UintRootConstant19, UintRootConstant20)
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_DIFFUSE_UAV STOCHASTIC_PARITY_PICK(UintRootConstant9, UintRootConstant6)
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_SPECULAR_UAV STOCHASTIC_PARITY_PICK(UintRootConstant10, UintRootConstant7)
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_MOMENTS_UAV STOCHASTIC_PARITY_PICK(UintRootConstant13, UintRootConstant8)
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV STOCHASTIC_PARITY_PICK(UintRootConstant16, UintRootConstant11)
#define STOCHASTIC_DENOISE_PREVIOUS_MIXTURE_UAV STOCHASTIC_PARITY_PICK(UintRootConstant20, UintRootConstant19)
#define STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV STOCHASTIC_PARITY_PICK(UintRootConstant12, UintRootConstant22)
#define STOCHASTIC_DENOISE_PREVIOUS_SURFACE_HISTORY_UAV STOCHASTIC_PARITY_PICK(UintRootConstant22, UintRootConstant12)
#endif
