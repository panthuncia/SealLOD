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
// Change-aware rectification (docs/stochastic_lighting.md "Change-aware rectification"): per pixel, the slow and fast
// averages of the neighbourhood's compressed mean, diffuse and specular.
#define STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV UintRootConstant11
#define STOCHASTIC_DENOISE_PREVIOUS_HISTORY_CHANGE_UAV UintRootConstant16

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
#endif
