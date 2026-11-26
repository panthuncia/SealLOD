// Stochastic lighting's denoiser (docs/stochastic_lighting.md "Denoiser"), after MegaLights' SVGF-style filter. Two passes.
//
// By default (stochasticLightingNormalize, "Guide-normalized accumulation" below) every value it filters is the noisy signal
// over a deterministic prediction of the unshadowed light (the guide pass's, stochasticGuide.hlsl), averaged as ratios of
// sums, and only the output is multiplied back by the guide at the output pixel; light-mixture confidence (below) shortens the
// history where the lights the guide's light comes from have changed. With the setting off it filters demodulated radiance (the
// path before overhaul phase 2, with the unshadowed bound).
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
// (docs/stochastic_lighting.md "Running the benchmarks").
//
// The estimator's radiance is unbiased but heavy-tailed at a few samples per pixel: half of all pixels are black in a
// frame, and rare samples are thousands of times the mean. Everything here that compares against neighbours is built so
// it does not trim that tail, which darkens (see "Denoiser" in the doc for the measurements).
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "PerPassRootConstants/stochasticLightingRootConstants.h"

#define DENOISE_ALBEDO_FLOOR STOCHASTIC_DEMODULATION_FLOOR
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
// The unshadowed bound (docs/performance/stochastic-lighting-experiments.md "Moving-light trails"). Selection estimates how much unshadowed light
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
// Guide-normalized accumulation (STOCHASTIC_FLAG_NORMALIZE, the setting stochasticLightingNormalize; docs/stochastic_lighting.md
// "Overhaul phase 2"). The noisy inputs then hold radiance / guide at the pixel each was taken at (stochasticShade.hlsl), so
// after demodulation every value here (samples, reconstruction, history, moments, the 5x5 statistics, the spike limit, the
// variance and the spatial filter) is (radiance / albedo) / guide, a ratio near the visible fraction of the unshadowed light.
// Only the spatial pass's output multiplies by the guide, at the output pixel, with the albedo. The domains, exactly:
//   guide.r = lum_d(sum unshadowed diffuse / max(BaseColorOpacity.rgb, 0.02)), guide.g = lum_d(sum unshadowed specular /
//   max(SpecularAo.rgb, 0.02)), lum_d the weights (0.25, 0.5, 0.25) (StochasticDemodulatedLuminance; DenoiseLuminance here);
//   history.rgb = EMA of noisy.rgb / (max(albedo, 0.02) * guide), albedo and guide both at the sample's own pixel;
//   output.rgb = filtered history.rgb * guide * max(albedo, 0.02), both at the output pixel.
// The unshadowed bound (DENOISE_BOUND) compares remodulated radiance with selection's noisy bound and is skipped in this mode
// unless DENOISE_NORMALIZE_BOUND is 1 (then it compares the history times the output pixel's guide and albedo).
#ifndef DENOISE_NORMALIZE_BOUND
#define DENOISE_NORMALIZE_BOUND 0
#endif
// Normalized and downsampled: the reconstruction weights each of the four samples' ratio by the guide where it was taken as
// well (per lobe), so the frame's value is sum(w S / albedo) / sum(w G): a ratio of sums, not a mean of ratios. Multiplied
// by the output pixel's guide, a mean of ratios moves light between neighbours wherever the ratio varies with the guide at
// pixel scale (a crevice whose shading normal faces the light has a large guide and is shadowed): with still lights and no
// jitter it was 0.6% too bright overall and 25% in the darkest quarter of 8x8 blocks. A ratio of sums blurs radiance
// where the guide is smooth (as the unnormalized reconstruction did, which conserves light) and still follows the guide
// where it is steep (a light close by). 0: the mean of ratios.
#ifndef DENOISE_NORMALIZE_GUIDE_WEIGHTS
#define DENOISE_NORMALIZE_GUIDE_WEIGHTS 1
#endif
// Normalized: the temporal average is a ratio of sums too, sum(a_t S_t) / sum(a_t G_t) over the history's exponential
// weights a_t, so each frame's ratio enters weighted by that frame's guide against the history's average guide (kept as
// log2 in the change history's b and a, diffuse and specular). The upscaler's jitter moves the shading normal and the guide
// with it from frame to frame (specular's 16x16 blocks by 29%, diffuse's by 3%), and a frame whose guide is high is more
// often shadowed: a mean of ratios times this frame's guide came out 1.4% too bright (4% displayed) with still lights.
// Once the light has gone, a frame's guide is small against the history's, so its ratio moves the history little; the
// output still follows this frame's guide. 0: a mean of ratios.
#ifndef DENOISE_NORMALIZE_TEMPORAL_WEIGHTS
#if DENOISE_CHANGE_DETECT || DENOISE_BOUND_CHANGE || DENOISE_RECONSTRUCT_MOMENTS
#define DENOISE_NORMALIZE_TEMPORAL_WEIGHTS 0 // They hold the change history.
#else
#define DENOISE_NORMALIZE_TEMPORAL_WEIGHTS 1
#endif
#endif
// The history's average guide is held to at most this many times this frame's, so a frame's ratio always takes at least
// 1 / DENOISE_NORMALIZE_GUIDE_RANGE of its usual weight. Without it, a pixel the light has left (its guide far below the
// history's) kept its old ratio for tens of frames, and the light that is left (other lights) was shown with it: in the
// sweep the stuck blocks halved but the age of those left rose from 16 to 28 frames (p95). 0: no limit.
#ifndef DENOISE_NORMALIZE_GUIDE_RANGE
#define DENOISE_NORMALIZE_GUIDE_RANGE 4.0f
#endif
// The same limit for the specular lobe alone (docs/performance/stochastic-lighting-experiments.md "Correctness pass (2026-11-17)"): the specular guide
// follows the normal-mapped shading normal under the upscaler's jitter and moves by orders of magnitude from frame to frame at
// one pixel, where the limit then boosts the dim frames' ratios (a mean of ratios: +0.25% at 64 lights). 0: no limit.
#ifndef DENOISE_NORMALIZE_GUIDE_RANGE_SPECULAR
#define DENOISE_NORMALIZE_GUIDE_RANGE_SPECULAR DENOISE_NORMALIZE_GUIDE_RANGE
#endif
#if DENOISE_NORMALIZE_TEMPORAL_WEIGHTS && (DENOISE_CHANGE_DETECT || DENOISE_BOUND_CHANGE || DENOISE_RECONSTRUCT_MOMENTS)
#error DENOISE_NORMALIZE_TEMPORAL_WEIGHTS keeps the history's guide in the change history
#endif
// The paired ratio (STOCHASTIC_FLAG_PAIRED, the setting stochasticLightingPaired on top of stochasticLightingNormalize;
// docs/stochastic_lighting.md "The paired ratio"). Shading also writes, in each noisy input's alpha, the unshadowed estimate
// drawn with the same samples (every used slot, visible or not), demodulated and over the guide: x = lum_d(X / albedo) / G,
// whose expectation is 1. Everything the numerator gets, x gets with identical weights: the guide-weighted reconstruction, the
// bilinear reprojection, the history length (rectification), the guide-weighted temporal blend and the spatial filter's taps
// (whose edge stops look at geometry and the numerator only). Its history H_X = sum(a_t X_t) / sum(a_t G_t) sits in the change
// history's r, g (diffuse, specular) beside log2 of the history's guide in b, a. The output is
//   G * H_Y / D,   D = lerp(1, H_X, c),   c = saturate(H_X / DENOISE_PAIRED_FLOOR) * saturate(length / DENOISE_PAIRED_MIN_LENGTH),
// so H_Y / H_X = sum(a_t Y_t) / sum(a_t X_t): the selection noise (which light, its 1 / p) is common to both sums and cancels,
// leaving the visibility's. Each visible slot adds the same value to Y and X, so lum_d(Y) <= X per sample and the paired ratio
// lies in [0, 1] wherever the two have had the same weights; where H_X is small (the samples have found little of the
// guide's light so far) the denominator slides to 1, the guide-normalized output G * H_Y. Needs the change history, and the
// exact guide (stochasticGuide.hlsl: froxels of at most STOCHASTIC_GUIDE_SMALL_FROXEL lights, or STOCHASTIC_GUIDE_FAR_FIELD 0):
// the output is G * sum(a Y) / sum(a X), which keeps the mean only where E[X] = G.
#define DENOISE_PAIRED_AVAILABLE (!(DENOISE_CHANGE_DETECT || DENOISE_BOUND_CHANGE || DENOISE_RECONSTRUCT_MOMENTS))
#ifndef DENOISE_PAIRED_FLOOR
#define DENOISE_PAIRED_FLOOR 0.05f
#endif
// The history length (frames) from which the paired denominator has its full weight. 1: from the first frame.
#ifndef DENOISE_PAIRED_MIN_LENGTH
#define DENOISE_PAIRED_MIN_LENGTH 1.0f
#endif
// The paired ratio's fallback by effective sample count instead of the centre's length (docs/stochastic_lighting.md "Visibility gradients"): the spatial filter's n_eff = (sum w)^2 / sum(w^2 / n_i), n_i = 2 length_i - 1 the
// tap's history's (an exponential average over length frames holds about that many samples' worth), and the denominator takes
// its full weight from DENOISE_PAIRED_MIN_SAMPLES on (linearly from 0). The ratio estimator's bias is O(1 / n): short histories
// behind fast lights, few taps across edges. 0: by length (DENOISE_PAIRED_MIN_LENGTH).
#ifndef DENOISE_PAIRED_MIN_SAMPLES
#define DENOISE_PAIRED_MIN_SAMPLES 0.0f
#endif
// The spike limit, paired: 0 scales the numerator's history alone (as without pairing, so it lowers the ratio); 1 scales the
// unshadowed history with it (the ratio is kept, and this frame's ratio weighs more in the blend).
#ifndef DENOISE_PAIRED_SPIKE
#define DENOISE_PAIRED_SPIKE 0
#endif
// Light-mixture confidence (STOCHASTIC_FLAG_MIXTURE, the setting stochasticLightingMixtureConfidence on top of
// stochasticLightingNormalize; docs/stochastic_lighting.md "Guide-normalized accumulation"). A ratio to the unshadowed guide cannot see a
// change in which lights the unshadowed light comes from: where a light the surface saw leaves and one it doesn't see takes
// its place at the same unshadowed level, the guide stays put and the history keeps the old light's visibility. The guide
// pass writes each pixel's light-mixture signature (the share of its unshadowed light from each of 8 buckets of lights,
// stochasticLightingRootConstants.h); the history keeps the mixture it was made under, as it keeps its average guide: the
// shares blended with the frame's guide-weighted blend weight, so they describe the history's denominator sum(a_t G_t),
// reprojected with the history's taps (R32G32_UInt, the same byte packing, rounded stochastically). Where this frame's
// mixture differs from the history's by a total variation m (the share of the unshadowed light that sits with other
// lights), both histories' lengths are scaled by
//   c = saturate(1 - DENOISE_MIXTURE_GAIN * max(m - DENOISE_MIXTURE_TOLERANCE, 0))
// before the blend: proportional (a steady mismatch m holds the history near 1 / (1 - c) frames), a reset only once m
// reaches 0.175; the spatial pass widens for the short history as for any other. The tolerance absorbs the signature's own
// wobble (jitter moves the shading normal; the byte rounding). It says "the history may be about other lights", not
// "visibility changed": where both lights are visible it costs noise only. Gain 8 because the measure is diluted: in `swap`
// the light that leaves is 15-20% of the signature where it was a third of the radiance (gain 2: the ghost's frame max 1.22;
// 4: 1.11; 8: 1.06; norm alone 1.32). Still lights, gain 8: 0.5% of pixels below c = 0.9 (gain 4: 0.24%).
#ifndef DENOISE_MIXTURE_TOLERANCE
#define DENOISE_MIXTURE_TOLERANCE 0.05f
#endif
#ifndef DENOISE_MIXTURE_GAIN
#define DENOISE_MIXTURE_GAIN 8.0f
#endif
// Visibility gradients (STOCHASTIC_FLAG_GRADIENT, the setting stochasticLightingVisibilityGradient on top of
// stochasticLightingNormalize; docs/stochastic_lighting.md "Visibility gradients"). Neither the guide
// (unshadowed) nor the mixture (shares unchanged) sees a light that keeps its share but changes visibility: a near light passing
// behind an edge. The estimator re-traces one sample per cell of sampled pixels from last frame (the same origin, light and point
// on the emitter, toward the light where it is now) and holds g = |V_now - V_then| x the light's share, the largest over 3x3
// cells, decayed over 2-3 frames (stochasticShade.hlsl). Both histories' lengths are scaled by
//   c_v = saturate(1 - DENOISE_GRADIENT_GAIN * max(g - DENOISE_GRADIENT_TOLERANCE, 0))
// with the mixture confidence. A matched ray changes only when the light or an occluder moved, so with still lights g is 0
// (unlike any statistic of the noisy input).
#ifndef DENOISE_GRADIENT_TOLERANCE
#define DENOISE_GRADIENT_TOLERANCE 0.05f
#endif
#ifndef DENOISE_GRADIENT_GAIN
#define DENOISE_GRADIENT_GAIN 4.0f
#endif
// 0: c_v is computed (and shown by view 9) but not applied, to see where the gradients would fire on the history without them.
#ifndef DENOISE_GRADIENT_APPLY
#define DENOISE_GRADIENT_APPLY 1
#endif
// STOCHASTIC_SIGNATURE_LOWRES 0 (phase 4's per-pixel mixture) only: the 4x4 blocks replace the tent by default.
// This frame's mixture is the signatures' average under a tent this many pixels wide (1: 3x3, 2: 5x5, weights 1-2-3-2-1),
// over the pixels on this pixel's surface, for the comparison and the history alike. Wherever the signature follows the
// normal map it changes between neighbouring pixels (gravel, grout, carving); the history, reprojected bilinearly under
// jitter, is an average of its neighbours, so comparing it with one pixel's signature reads as a change. Still lights, gain
// 2, the share measured as radiance: 13.5% of pixels below c = 0.9 at radius 0, 13.9% at 1, 7.4% at 2; with the wrapped
// cosine (STOCHASTIC_GUIDE_SIGNATURE_LOBES 2, the default) 4.0%, 0.55%, 0.07%. 0: the pixel's own. Cost (16 lights, the
// temporal pass's 1.91 ms): +0.36 ms at 0, +0.54 at 1, +0.89 at 2 (texture loads; a groupshared tile was slower, and a
// `continue` at the screen's edge inside the unrolled loop cost another 1.1 ms).
#ifndef DENOISE_MIXTURE_RADIUS
#define DENOISE_MIXTURE_RADIUS 2
#endif
// STOCHASTIC_SIGNATURE_LOWRES: the history's block mixture is reprojected from the nearest block (0) or bilinearly from four (1).
#ifndef DENOISE_MIXTURE_BILINEAR
#define DENOISE_MIXTURE_BILINEAR 0
#endif
// How much of rectification's per-sample variance is the history's temporal one (the rest the neighbourhood's spatial one;
// DenoiseRectify says why half).
#ifndef DENOISE_RECTIFY_TEMPORAL_WEIGHT
#define DENOISE_RECTIFY_TEMPORAL_WEIGHT 0.5f
#endif
// The length rectification shortens a history to at the far end (twice DENOISE_RECTIFY_SIGMA). 1 keeps half of it each
// frame, which is no reset for a history a light left 10^5 times brighter than what is there now (the trail test,
// docs/performance/stochastic-lighting-experiments.md "Moving-light trails"): it takes ~17 frames of halving to fade. 0 drops it.
#ifndef DENOISE_RECTIFY_MIN_LENGTH
#define DENOISE_RECTIFY_MIN_LENGTH 1.0f
#endif
// Change-aware rectification (docs/stochastic_lighting.md "Denoiser switches"). 1: the history is shortened where
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
// Overhaul phase 1 (docs/performance/stochastic-lighting-experiments.md "Overhaul phase 1"): surface consistency.
// 1: a reprojection tap also needs last frame's surface there to lie on this pixel's plane: its distance from the plane
// through this point, along the geometric normal (from this frame's depth, so normal maps and their jitter play no part),
// within DENOISE_PLANE_TOLERANCE of the view depth. The 5% depth test alone accepts 50 cm at 10 m, across leaves, railings
// and edges. 0: the depth test alone. Off: it halved the sweep's stuck blocks but added slow noise with still lights (32x32
// +5%) and in the camera flight (paired 32x32 |d| +29%); at 0.03 the gain mostly went too.
#ifndef DENOISE_REPROJECT_PLANE
#define DENOISE_REPROJECT_PLANE 0
#endif
#ifndef DENOISE_PLANE_TOLERANCE
#define DENOISE_PLANE_TOLERANCE 0.01f
#endif
// The 5x5 neighbourhood's statistics weighted by each sample's agreement with this pixel's surface (the reconstruction's
// depth and normal weight), so a neighbourhood that spans leaves and the ground behind them describes the surface the
// history is for. Bits: 1 mean, variance and count (rectification); 2 brightest sample and lit fraction (the spike limit);
// 4 the largest bound (DENOISE_BOUND). Where the samples on this surface weigh less than DENOISE_RECEIVER_MIN_WEIGHT in
// all, the unweighted statistics stand. Maxima take samples of weight DENOISE_RECEIVER_PEAK_WEIGHT and more. 0: every
// valid sample counts the same. 1 by default: with still lights 0.3% brighter and less slow noise (32x32 -4%), the sweep's
// stuck blocks fewer. Bits 2 and 4 darkened still lights by 0.7% and added noise (a narrower peak and bound clamp more).
#ifndef DENOISE_RECEIVER_STATS
#define DENOISE_RECEIVER_STATS 1
#endif
#ifndef DENOISE_RECEIVER_MIN_WEIGHT
#define DENOISE_RECEIVER_MIN_WEIGHT 2.0f
#endif
#ifndef DENOISE_RECEIVER_PEAK_WEIGHT
#define DENOISE_RECEIVER_PEAK_WEIGHT 0.25f
#endif
// Reduced resolution: rectification judges the reconstructed signal, the value the history accumulates (the weighted mean
// of four samples, DENOISE_RECONSTRUCT_MEAN), instead of one sample chosen at random. The history keeps that value's
// compressed moments in the change history (so DENOISE_CHANGE_DETECT and DENOISE_BOUND_CHANGE must be off), and the
// neighbourhood's side is the same kind of value: the 16 2x2 means of its 5x5 samples. The raw-sample moments stay what
// the spatial filter's noise estimate comes from. 0: rectification on the raw-sample moments. Off: alone it was neutral on
// trails and 0.25% brighter with still lights, but with DENOISE_RECEIVER_STATS rectification grew too lax (the `off` scene's
// excess +57%, stuck blocks +64%).
#ifndef DENOISE_RECONSTRUCT_MOMENTS
#define DENOISE_RECONSTRUCT_MOMENTS 0
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
// choice adds selection noise at every frequency where the mean adds none (docs/stochastic_lighting.md "Running the benchmarks").
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
// of the 12 taps: DENOISE_SPATIAL_FLOOR_TAP_COUNT taps at the floor radius in a plus, rotated per pixel and frame, each tap's
// depth and normal loaded once for both signals. Specular joins only where it is rough enough to take the same radius. 0: the
// 12 taps everywhere.
#ifndef DENOISE_SPATIAL_FLOOR_TAPS
#define DENOISE_SPATIAL_FLOOR_TAPS 1
#endif
// Speed options (docs/stochastic_lighting.md "Speed options"): the taps the floor's plus takes (4; 2, the rotated
// axis's two ends: 0.07 ms less, still lights unchanged, about 2% more pixel noise after DLSS) and the taps the wide kernel
// takes (12; 6: every other Poisson tap, the set still rotated per pixel and frame).
#ifndef DENOISE_SPATIAL_FLOOR_TAP_COUNT
#define DENOISE_SPATIAL_FLOOR_TAP_COUNT 4
#endif
#ifndef DENOISE_SPATIAL_TAP_COUNT
#define DENOISE_SPATIAL_TAP_COUNT 12
#endif
// Diagnostic views of the denoiser's internals (docs/stochastic_lighting.md "Denoiser"; scripts/stochastic_scaling.sh trail
// and still read them). The spatial pass writes them in place of its output; 0 (the default) compiles them out.
//   1: like DENOISE_DEBUG_PAIR, StochasticLighting::Diffuse holds the frame's denoised (r) and noisy (g) total luminance, plus
//      the temporal pass's own result before the spatial filter, remodulated (b), and selection's bound (a); ::Specular is 0.
//   2: history length, diffuse (r) and specular (g).
//   3: variance: the per-sample luminance variance the temporal pass hands the spatial filter, diffuse (r) and specular (g),
//      and the same over the history length, the luminance stop's scale (b, a).
//   4: the unshadowed bound's grip (DENOISE_BOUND): the reprojected history's remodulated luminance over DENOISE_BOUND_MARGIN
//      times the 5x5 neighbourhood's largest bound, before the clamp (r; above 1 it was scaled down by 1 / r), and that largest
//      bound (g). Set on both denoiser pipelines: the temporal pass hands it over in the variance texture.
//   5: the composite left as it is, for captures after DLSS: ::Diffuse holds the denoised total (rgb; ::Specular's rgb is 0,
//      and the deferred pass adds the two) with the noisy total luminance in its alpha, and ::Specular's alpha holds the
//      temporal pass's result before the spatial filter (luminance, remodulated).
//   6: cross-surface history (DENOISE_REPROJECT_PLANE): ::Diffuse holds the denoised total luminance (r), the share of the
//      reprojected history's weight that the depth test accepted but that lies off this pixel's plane (g; whether or not
//      DENOISE_REPROJECT_PLANE then rejects it), the temporal pass's result (b) and the noisy total luminance (a); ::Specular
//      is 0. Set on both denoiser pipelines: the temporal pass hands the share over in the variance texture's g, and the
//      spatial filter's specular luminance stop takes diffuse's variance meanwhile. The composite is not valid.
//   7: the paired ratio's denominator ("The paired ratio" above; 0 unless paired): ::Diffuse holds the temporal pass's
//      unshadowed history H_X (r diffuse, g specular) and the same after the spatial filter (b, a); ::Specular this frame's
//      noisy x at the block's sampled pixel (r diffuse, b specular) and the guide there (g, a). Spatial pass only. With
//      capture.hdr's stochastic_guide target, E[x] = 1 and the long history's H_X against 1 check E[X] = G.
//   8: light-mixture confidence ("Light-mixture confidence" above; 1 unless on): ::Diffuse holds the denoised total luminance
//      (r), this frame's confidence c, the factor the history's length was scaled by (g; 1 where no history was compared),
//      the temporal pass's result (b) and the noisy total luminance (a); ::Specular is 0. Set on both denoiser pipelines: the
//      temporal pass hands c over in the variance texture's g, as view 6 does, with the same caveats. With visibility gradients
//      on, g is the combined confidence (the mixture's times the gradients').
//   9: visibility gradients ("Visibility gradients" above; 1 unless on): as view 8 with g the gradients' confidence c_v alone.
//   With the paired ratio, view 1's alpha (selection's bound) holds the noisy diffuse x instead, and the temporal-only values
//   (views 1, 5, 6) are divided by the center pixel's own denominator.
#ifndef DENOISE_DEBUG_VIEW
#define DENOISE_DEBUG_VIEW 0
#endif
#if DENOISE_DEBUG_VIEW && defined(DENOISE_DEBUG_PAIR)
#error DENOISE_DEBUG_VIEW and DENOISE_DEBUG_PAIR both write the spatial pass's output
#endif
#if DENOISE_RECONSTRUCT_MOMENTS && (DENOISE_BOUND_CHANGE || DENOISE_CHANGE_DETECT)
#error DENOISE_RECONSTRUCT_MOMENTS keeps its moments in the change history
#endif

#define DENOISE_TILE 8
#define DENOISE_BORDER 2
#define DENOISE_REGION (DENOISE_TILE + 2 * DENOISE_BORDER)
// Downsampled: the tile's 4x4 samples and a 2-sample border, one per thread.
#define DENOISE_SAMPLE_REGION (DENOISE_TILE / 2 + 2 * DENOISE_BORDER)
// Whether the temporal pass is compiled for reduced resolution (docs/performance/stochastic-lighting-experiments.md "Performance pass (2026-11-17)").
// -1: read at run time (STOCHASTIC_DOWNSAMPLE_FACTOR), the groupshared region sized for full resolution's 12x12. 0 or 1:
// compiled for full or reduced resolution, the region sized for what it uses (reduced: 8x8 samples, 4,608 bytes instead of
// 9,728). StochasticDenoisePass sets it from the graph's downsample, which a change of the setting rebuilds.
#ifndef DENOISE_DOWNSAMPLED
#define DENOISE_DOWNSAMPLED -1
#endif
#ifndef DENOISE_SKIP_UNUSED_BOUND
#define DENOISE_SKIP_UNUSED_BOUND 1
#endif
#ifndef DENOISE_STATS_ROW_LOOP
#define DENOISE_STATS_ROW_LOOP 1
#endif
#if DENOISE_DOWNSAMPLED == 1
#define DENOISE_SHARED_SLOTS (DENOISE_SAMPLE_REGION * DENOISE_SAMPLE_REGION)
#else
#define DENOISE_SHARED_SLOTS (DENOISE_REGION * DENOISE_REGION)
#endif

static const float2 kDenoisePoisson[12] = {
    float2(-0.326f, -0.406f), float2(-0.840f, -0.074f), float2(-0.696f, 0.457f), float2(-0.203f, 0.621f),
    float2(0.962f, -0.195f), float2(0.473f, -0.480f), float2(0.519f, 0.767f), float2(0.185f, -0.893f),
    float2(0.507f, 0.064f), float2(0.896f, 0.412f), float2(-0.322f, -0.933f), float2(-0.792f, -0.598f),
};

float DenoiseLuminance(float3 value)
{
    return StochasticDemodulatedLuminance(value);
}

// The guide at a pixel (diffuse, specular), or 1 when the inputs are not normalized: what the history multiplies by to
// become demodulated radiance again.
float2 DenoiseGuide(int2 pixel, bool normalized)
{
    if (!normalized)
    {
        return 1.0f;
    }
    Texture2D<float2> guide = ResourceDescriptorHeap[STOCHASTIC_DENOISE_GUIDE_SRV];
    const float2 g = guide[pixel];
    return select(g > STOCHASTIC_GUIDE_EPSILON, g, 0.0f);
}

// The guide at the pixel a noisy input's texel was taken at (downsampled, the one StochasticDownsampledPixel picks this frame),
// for the diagnostics that show the noisy input as radiance.
float2 DenoiseNoisyGuide(int2 cell, int2 screen, bool normalized)
{
    const int2 source = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? int2(StochasticDownsampledPixel(uint2(cell), STOCHASTIC_FRAME, 2u, uint2(screen))) : cell;
    return DenoiseGuide(source, normalized);
}

// The paired ratio's denominator (DENOISE_PAIRED_FLOOR above), per lobe: the unshadowed history H_X where it has weight, sliding
// to 1 (the guide-normalized output) where it is small or the history short. Never below about DENOISE_PAIRED_FLOOR.
float2 DenoisePairedDenominator(float2 unshadowed, float2 historyLength, float2 samples)
{
    const float2 confidence = saturate(unshadowed / DENOISE_PAIRED_FLOOR) * (DENOISE_PAIRED_MIN_SAMPLES > 0.0f
        ? saturate(samples / DENOISE_PAIRED_MIN_SAMPLES) : saturate(historyLength / DENOISE_PAIRED_MIN_LENGTH));
    return lerp(1.0f, max(unshadowed, 0.0f), confidence);
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

// A pixel's view-space position at a linear depth, as DenoiseLoadSurface rebuilds it (without the jitter, which moves it by
// a fraction of a pixel).
float3 DenoiseViewPosition(float2 pixel, float linearZ, float2 resolution, Camera camera)
{
    float2 uv = (pixel + 0.5f) / resolution;
    uv.y = 1.0f - uv.y;
    const float2 ndc = uv * 2.0f - 1.0f;
    return float3(ndc.x * camera.projectionInverse[0][0], ndc.y * camera.projectionInverse[1][1], -1.0f) * linearZ;
}

// The geometric normal in view space, from this frame's depth: along each axis the neighbour nearer in depth, so an edge
// takes the side this pixel belongs to. Facing the camera. Zero where an axis has no valid neighbour.
float3 DenoiseGeometricNormal(int2 pixel, float linearZ, int2 screen, Camera camera)
{
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    const float2 resolution = float2(screen);
    const float3 center = DenoiseViewPosition(float2(pixel), linearZ, resolution, camera);
    float3 axes[2];
    [unroll]
    for (uint axis = 0; axis < 2u; ++axis)
    {
        const int2 step = axis == 0u ? int2(1, 0) : int2(0, 1);
        const int2 plus = min(pixel + step, screen - 1);
        const int2 minus = max(pixel - step, int2(0, 0));
        const float depthPlus = depthTexture[plus];
        const float depthMinus = depthTexture[minus];
        const bool validPlus = asuint(depthPlus) != 0x7F7FFFFFu && any(plus != pixel);
        const bool validMinus = asuint(depthMinus) != 0x7F7FFFFFu && any(minus != pixel);
        const bool usePlus = validPlus && (!validMinus || abs(depthPlus - linearZ) <= abs(depthMinus - linearZ));
        axes[axis] = usePlus ? DenoiseViewPosition(float2(plus), depthPlus, resolution, camera) - center
            : validMinus ? center - DenoiseViewPosition(float2(minus), depthMinus, resolution, camera) : 0.0f;
    }
    const float3 normal = cross(axes[0], axes[1]);
    const float lengthSquared = dot(normal, normal);
    if (lengthSquared <= 1e-20f)
    {
        return 0.0f;
    }
    const float3 n = normal * rsqrt(lengthSquared);
    return dot(n, center) > 0.0f ? -n : n;
}

// A sample's weight for a pixel's surface: depth agreement (falling linearly to 0 at DENOISE_RECONSTRUCT_DEPTH relative)
// times normal agreement (the cosine to DENOISE_RECONSTRUCT_NORMAL_POWER). sampleSurface: normal, view depth (0: none).
float DenoiseSurfaceWeight(float4 sampleSurface, DenoiseSurface surface)
{
    const float depthWeight = saturate(1.0f - abs(sampleSurface.w - surface.depth) / (DENOISE_RECONSTRUCT_DEPTH * surface.depth));
    const float normalWeight = pow(saturate(dot(sampleSurface.xyz, surface.normal)), DENOISE_RECONSTRUCT_NORMAL_POWER);
    return sampleSurface.w > 0.0f ? depthWeight * normalWeight : 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------------
// Temporal.

groupshared float3 gs_diffuse[DENOISE_SHARED_SLOTS]; // Demodulated.
groupshared float3 gs_specular[DENOISE_SHARED_SLOTS];
groupshared float3 gs_compressed[DENOISE_SHARED_SLOTS]; // Diffuse, specular, valid.
groupshared float gs_bound[DENOISE_SHARED_SLOTS]; // Selection's unshadowed bound (0 where invalid, or paired).
#if DENOISE_PAIRED_AVAILABLE
groupshared float2 gs_unshadowed[DENOISE_SHARED_SLOTS]; // Paired: x, diffuse and specular (0 where invalid).
#endif
#if DENOISE_NORMALIZE_GUIDE_WEIGHTS
groupshared float2 gs_sampleGuide[DENOISE_SAMPLE_REGION * DENOISE_SAMPLE_REGION]; // Downsampled: the guide where each sample was taken.
#endif
// Downsampled (or with DENOISE_RECEIVER_STATS): normal, view depth (0: none).
#if DENOISE_RECEIVER_STATS
groupshared float4 gs_sampleSurface[DENOISE_SHARED_SLOTS];
#else
groupshared float4 gs_sampleSurface[DENOISE_SAMPLE_REGION * DENOISE_SAMPLE_REGION];
#endif

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
// guide: the output pixel's (DenoiseGuide), 1 when not normalized.
void DenoiseApplyBound(inout DenoiseHistory diffuse, inout DenoiseHistory specular, DenoiseSurface surface, float boundMax, float2 guide)
{
#if DENOISE_BOUND
    const float modulated = DenoiseLuminance(diffuse.value * surface.diffuseAlbedo * guide.x + specular.value * surface.specularAlbedo * guide.y);
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

#if STOCHASTIC_SIGNATURE_LOWRES
// Light-mixture confidence per 4x4 block (STOCHASTIC_SIGNATURE_LOWRES). The history's mixture for the block whose top-left pixel
// is blockPixel, reprojected bilinearly in the low-resolution grid through the motion at the block's centre, and renormalized
// (so a tap that holds no light, sky or an unlit texel, dilutes nothing); false where there is none. Also this frame's block
// signature. With DENOISE_MIXTURE_KEEP 0 the thread that writes the block's mixture after its blend recomputes it (when the pass
// sat at 168 registers and within 48 bytes of a groupshared occupancy step, holding it cost 0.15 ms; docs/stochastic_lighting.md
// "Overhaul phase 5"); since the performance pass (88 registers, 4,608 bytes) it keeps the packed texel instead (below).
// The nearest low-resolution texel of the history's block mixture to where the block's centre was last frame (through the
// motion at the block's centre), packed.
uint2 DenoiseBlockMixtureTexel(int2 blockPixel, int2 screen)
{
    Texture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
    RWTexture2D<uint2> previousMixture = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_MIXTURE_UAV];
    const int2 lowSize = (screen + STOCHASTIC_SIGNATURE_BLOCK - 1) / STOCHASTIC_SIGNATURE_BLOCK;
    const int2 centerPixel = min(blockPixel + 2, screen - 1);
    const float2 resolution = float2(screen);
    const float2 uv = (float2(centerPixel) + 0.5f) / resolution;
    const float2 previousNdc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f) - motion[centerPixel];
    const float2 previousCenter = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f) * resolution
        + (float2(blockPixel) + 0.5f * STOCHASTIC_SIGNATURE_BLOCK - (float2(centerPixel) + 0.5f));
    const float2 lowPosition = previousCenter / STOCHASTIC_SIGNATURE_BLOCK - 0.5f;
    const int2 coordinate = clamp(int2(floor(lowPosition + 0.5f)), int2(0, 0), lowSize - 1);
    return previousMixture[coordinate];
}

// The history's block mixture from its packed texel, renormalized; false where it holds no light (or there is no history).
bool DenoiseBlockMixtureFinish(bool reprojected, uint2 packed, out float4 historyLow, out float4 historyHigh)
{
    historyLow = reprojected ? StochasticUnpackShares(packed.x) : 0.0f;
    historyHigh = reprojected ? StochasticUnpackShares(packed.y) : 0.0f;
    const float total = dot(historyLow + historyHigh, 1.0f);
    const bool known = reprojected && total > 1e-3f;
    historyLow = known ? historyLow / total : 0.0f;
    historyHigh = known ? historyHigh / total : 0.0f;
    return known;
}

bool DenoiseBlockMixture(int2 blockPixel, int2 screen, out float4 historyLow, out float4 historyHigh, out uint2 blockSignature)
{
    historyLow = 0.0f;
    historyHigh = 0.0f;
    Texture2D<uint2> signature = ResourceDescriptorHeap[STOCHASTIC_DENOISE_SIGNATURE_SRV];
    blockSignature = signature[blockPixel / STOCHASTIC_SIGNATURE_BLOCK];
    if ((STOCHASTIC_FLAGS & STOCHASTIC_FLAG_HISTORY_VALID) == 0u)
    {
        return false;
    }
    Texture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
    RWTexture2D<uint2> previousMixture = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_MIXTURE_UAV];
    const int2 lowSize = (screen + STOCHASTIC_SIGNATURE_BLOCK - 1) / STOCHASTIC_SIGNATURE_BLOCK;
    const int2 centerPixel = min(blockPixel + 2, screen - 1);
    const float2 resolution = float2(screen);
    const float2 uv = (float2(centerPixel) + 0.5f) / resolution;
    const float2 previousNdc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f) - motion[centerPixel];
    // The block centre's position last frame, in pixels (continuous), then in the low-resolution grid's texels.
    const float2 previousCenter = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f) * resolution
        + (float2(blockPixel) + 0.5f * STOCHASTIC_SIGNATURE_BLOCK - (float2(centerPixel) + 0.5f));
    const float2 lowPosition = previousCenter / STOCHASTIC_SIGNATURE_BLOCK - 0.5f;
#if !DENOISE_MIXTURE_BILINEAR
    {
        // The nearest block's: one load (a block is 4 pixels; the confidence is per block anyway).
        const uint2 packed = DenoiseBlockMixtureTexel(blockPixel, screen);
        historyLow = StochasticUnpackShares(packed.x);
        historyHigh = StochasticUnpackShares(packed.y);
    }
#else
    const float2 lowBase = floor(lowPosition);
    const float2 lowFraction = lowPosition - lowBase;
    [unroll]
    for (uint tap = 0; tap < 4u; ++tap)
    {
        // Clamped, with no weight outside the grid (a `continue` here costs far more than the loads).
        const int2 offset = int2(tap & 1u, tap >> 1u);
        const int2 unclamped = int2(lowBase) + offset;
        const int2 coordinate = clamp(unclamped, int2(0, 0), lowSize - 1);
        const float weight = all(coordinate == unclamped)
            ? (offset.x ? lowFraction.x : 1.0f - lowFraction.x) * (offset.y ? lowFraction.y : 1.0f - lowFraction.y) : 0.0f;
        const uint2 packed = previousMixture[coordinate];
        historyLow += StochasticUnpackShares(packed.x) * weight;
        historyHigh += StochasticUnpackShares(packed.y) * weight;
    }
#endif
    const float total = dot(historyLow + historyHigh, 1.0f);
    const bool known = total > 1e-3f;
    historyLow = known ? historyLow / total : 0.0f;
    historyHigh = known ? historyHigh / total : 0.0f;
    return known;
}

// Writes the block's mixture history: the reprojected one blended toward this frame's block signature by weight (this frame's
// signature where there was none), rounded stochastically (dither: one number per bucket, spread from u by the golden ratio;
// derived here rather than kept from earlier in the pass, which held 8 more registers through it).
// DENOISE_MIXTURE_KEEP (docs/performance/stochastic-lighting-experiments.md "Performance pass (2026-11-17)"): the writer keeps the history's packed
// texel and the block's signature from the start of the pass (four registers, in the block's top-left thread) instead of
// reprojecting again; the same values. Nearest-block mode only.
#ifndef DENOISE_MIXTURE_KEEP
#define DENOISE_MIXTURE_KEEP 1
#endif
#if DENOISE_MIXTURE_BILINEAR
#undef DENOISE_MIXTURE_KEEP
#define DENOISE_MIXTURE_KEEP 0
#endif

void DenoiseWriteBlockMixtureKept(int2 pixel, float weight, float u, bool reprojected, uint2 packed, uint2 signature)
{
    const float4 ditherLow = frac(u + float4(0.0f, 1.0f, 2.0f, 3.0f) * 0.618034f);
    const float4 ditherHigh = frac(u + float4(4.0f, 5.0f, 6.0f, 7.0f) * 0.618034f);
    float4 historyLow, historyHigh;
    const bool known = DenoiseBlockMixtureFinish(reprojected, packed, historyLow, historyHigh);
    const float w = known ? weight : 1.0f;
    RWTexture2D<uint2> mixtureOutput = ResourceDescriptorHeap[STOCHASTIC_DENOISE_MIXTURE_UAV];
    mixtureOutput[pixel / STOCHASTIC_SIGNATURE_BLOCK] = StochasticPackSignature(lerp(historyLow, StochasticUnpackShares(signature.x), w),
        lerp(historyHigh, StochasticUnpackShares(signature.y), w), ditherLow, ditherHigh);
}

void DenoiseWriteBlockMixture(int2 pixel, int2 screen, float weight, float u)
{
    const float4 ditherLow = frac(u + float4(0.0f, 1.0f, 2.0f, 3.0f) * 0.618034f);
    const float4 ditherHigh = frac(u + float4(4.0f, 5.0f, 6.0f, 7.0f) * 0.618034f);
    float4 historyLow, historyHigh;
    uint2 signature;
    const bool known = DenoiseBlockMixture(pixel, screen, historyLow, historyHigh, signature);
    const float w = known ? weight : 1.0f;
    RWTexture2D<uint2> mixtureOutput = ResourceDescriptorHeap[STOCHASTIC_DENOISE_MIXTURE_UAV];
    mixtureOutput[pixel / STOCHASTIC_SIGNATURE_BLOCK] = StochasticPackSignature(lerp(historyLow, StochasticUnpackShares(signature.x), w),
        lerp(historyHigh, StochasticUnpackShares(signature.y), w), ditherLow, ditherHigh);
}
#endif

#if STOCHASTIC_SIGNATURE_LOWRES
#if DENOISE_MIXTURE_KEEP
#define DENOISE_WRITE_BLOCK_MIXTURE(weight, dither) DenoiseWriteBlockMixtureKept(pixel, weight, dither, keptReprojected, keptHistory, keptSignature)
#else
#define DENOISE_WRITE_BLOCK_MIXTURE(weight, dither) DenoiseWriteBlockMixture(pixel, screen, weight, dither)
#endif
#endif

// Performance pass 2 (docs/performance/stochastic-lighting-experiments.md "Performance pass 2 (2026-11-17)"):
//   DENOISE_MIXTURE_SHARED 1: the block confidence goes from each block's top-left thread to its 16 pixels through groupshared
//     memory (four words, read after the barrier the region load needs anyway) instead of four WaveActiveMin, two of which find
//     no writer in a wave of 32. The same values on waves of 32 or more (smaller waves now get the confidence too).
//   DENOISE_MIXTURE_EARLY 1: the writer's dependent loads (the motion, then the history's packed mixture) are issued before the
//     region load instead of after it, so their latency hides behind the region's loads rather than stalling the barrier.
#ifndef DENOISE_MIXTURE_SHARED
#define DENOISE_MIXTURE_SHARED 1
#endif
#ifndef DENOISE_MIXTURE_EARLY
#define DENOISE_MIXTURE_EARLY 1
#endif
#if STOCHASTIC_SIGNATURE_LOWRES && DENOISE_MIXTURE_SHARED
groupshared float gs_blockConfidence[4];
#endif

#if STOCHASTIC_SIGNATURE_LOWRES
// The block's light-mixture confidence, in its top-left thread (1 elsewhere, and where the block has no known history), with
// the history's packed texel and the signature kept for the writer (DENOISE_MIXTURE_KEEP).
float DenoiseBlockConfidence(bool mixture, uint3 id, uint3 threadId, int2 screen, inout uint2 keptHistory, inout uint2 keptSignature,
    inout bool keptReprojected)
{
    float blockConfidence = 1.0f;
    if (mixture && all((threadId.xy & 3u) == 0u) && all(int2(id.xy) < screen))
    {
        float4 historyLow, historyHigh;
        uint2 blockSignature;
#if DENOISE_MIXTURE_KEEP
        Texture2D<uint2> signature = ResourceDescriptorHeap[STOCHASTIC_DENOISE_SIGNATURE_SRV];
        keptSignature = signature[int2(id.xy) / STOCHASTIC_SIGNATURE_BLOCK];
        blockSignature = keptSignature;
        keptReprojected = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_HISTORY_VALID) != 0u;
        keptHistory = keptReprojected ? DenoiseBlockMixtureTexel(int2(id.xy), screen) : 0u;
        if (DenoiseBlockMixtureFinish(keptReprojected, keptHistory, historyLow, historyHigh))
#else
        if (DenoiseBlockMixture(int2(id.xy), screen, historyLow, historyHigh, blockSignature))
#endif
        {
            const float moved = 1.0f - StochasticMixtureOverlap(historyLow, historyHigh,
                StochasticUnpackShares(blockSignature.x), StochasticUnpackShares(blockSignature.y));
            blockConfidence = saturate(1.0f - DENOISE_MIXTURE_GAIN * max(moved - DENOISE_MIXTURE_TOLERANCE, 0.0f));
        }
    }
    return blockConfidence;
}
#endif

[numthreads(DENOISE_TILE, DENOISE_TILE, 1)]
void StochasticDenoiseTemporalCS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    Texture2D<float4> noisyDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV];
    Texture2D<float4> noisySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV];
    const int2 screen = int2(perFrame.screenResX, perFrame.screenResY);
    const bool downsampled = DENOISE_DOWNSAMPLED >= 0 ? DENOISE_DOWNSAMPLED == 1 : STOCHASTIC_DOWNSAMPLE_FACTOR > 1u;
    // The inputs are radiance / guide (above, "Guide-normalized accumulation"); nothing below but the bound needs to know.
    const bool normalized = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_NORMALIZE) != 0u;
    // The noisy alphas hold the unshadowed estimate x (above, "The paired ratio"), not the bound.
    const bool paired = DENOISE_PAIRED_AVAILABLE && normalized && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_PAIRED) != 0u;
    // Light-mixture confidence (above): only with the guide, which writes the signature.
    const bool mixture = normalized && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_MIXTURE) != 0u;
    // Visibility gradients (above): the estimator's held gradient, with the guide too.
    const bool gradient = normalized && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_GRADIENT) != 0u;
    // The noisy inputs' grid: pixels, or downsampled the 2x2 blocks, each sampled at one pixel.
    const int2 grid = downsampled ? (screen + 1) / 2 : screen;
    const int regionSize = downsampled ? DENOISE_SAMPLE_REGION : DENOISE_REGION;

    // The tile's samples and a 2-sample border, demodulated by the albedo where each was taken, for the 5x5 neighbourhoods
    // (and, downsampled, for the reconstruction).
    const int2 regionOrigin = int2(groupId.xy) * (downsampled ? DENOISE_TILE / 2 : DENOISE_TILE) - DENOISE_BORDER;
#if STOCHASTIC_SIGNATURE_LOWRES
    // Light-mixture confidence per 4x4 block (STOCHASTIC_SIGNATURE_LOWRES): the thread at each block's top-left pixel compares the
    // block's signature with the history's block mixture (DenoiseBlockMixture), and the block's 16 pixels take its confidence (a
    // block's four rows are 32 consecutive threads, so with waves of 32 or more that thread is in their wave; smaller waves keep
    // confidence 1 unless DENOISE_MIXTURE_SHARED). The same thread writes the block's mixture after its own blend (below), with
    // its own blend weight. DENOISE_MIXTURE_EARLY: computed here, before the region load; else after it.
    uint2 keptHistory = 0u, keptSignature = 0u;
    bool keptReprojected = false;
#if DENOISE_MIXTURE_EARLY
    float blockConfidence = DenoiseBlockConfidence(mixture, id, threadId, screen, keptHistory, keptSignature, keptReprojected);
#endif
#endif
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
        gs_bound[slot] = neighbour.valid && !paired ? noisyDiffuse[cell].a : 0.0f;
#if DENOISE_PAIRED_AVAILABLE
        gs_unshadowed[slot] = neighbour.valid && paired ? max(float2(noisyDiffuse[cell].a, noisySpecular[cell].a), 0.0f) : 0.0f;
#endif
#if DENOISE_NORMALIZE_GUIDE_WEIGHTS
        if (downsampled)
        {
            gs_sampleGuide[slot] = neighbour.valid ? DenoiseGuide(coordinate, normalized) : 0.0f;
        }
#endif
        if (downsampled || DENOISE_RECEIVER_STATS)
        {
            gs_sampleSurface[slot] = float4(neighbour.normal, neighbour.valid ? neighbour.depth : 0.0f);
        }
    }
#if STOCHASTIC_SIGNATURE_LOWRES
#if !DENOISE_MIXTURE_EARLY
    float blockConfidence = DenoiseBlockConfidence(mixture, id, threadId, screen, keptHistory, keptSignature, keptReprojected);
#endif
#if DENOISE_MIXTURE_SHARED
    // The broadcast through groupshared memory (DENOISE_MIXTURE_SHARED): each block's top-left thread writes its word, and every
    // thread reads its block's after the barrier below.
    if (all((threadId.xy & 3u) == 0u))
    {
        gs_blockConfidence[(threadId.y >> 2u) * 2u + (threadId.x >> 2u)] = blockConfidence;
    }
#else
    // The broadcast: per block, the minimum over the wave of the confidence its top-left thread found (1 elsewhere). A lane index
    // that differs between lanes (WaveReadLaneAt) was read as uniform here: half the blocks took another block's value.
    {
        const bool writer = all((threadId.xy & 3u) == 0u);
        const uint block = (threadId.y >> 2u) * 2u + (threadId.x >> 2u);
        float mine = 1.0f;
        [unroll]
        for (uint b = 0; b < 4u; ++b)
        {
            const float value = WaveActiveMin(writer && block == b ? blockConfidence : 1.0f);
            mine = block == b ? value : mine;
        }
        blockConfidence = WaveGetLaneCount() >= 32u ? mine : 1.0f;
    }
#endif
#endif
    GroupMemoryBarrierWithGroupSync();
#if STOCHASTIC_SIGNATURE_LOWRES && DENOISE_MIXTURE_SHARED
    blockConfidence = gs_blockConfidence[(threadId.y >> 2u) * 2u + (threadId.x >> 2u)];
#endif
    // Light-mixture confidence per block (above): this pixel's block, and whether this thread writes the block's mixture.
#if STOCHASTIC_SIGNATURE_LOWRES
    const bool mixtureWriter = mixture && all((threadId.xy & 3u) == 0u);
    const bool fullResolutionMixture = false;
#else
    const bool fullResolutionMixture = mixture;
#endif

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
#if STOCHASTIC_SIGNATURE_LOWRES
        if (mixtureWriter)
        {
            // The block's history starts from this frame's signature (its other pixels may have a surface).
            DENOISE_WRITE_BLOCK_MIXTURE(1.0f, 0.0f);
        }
#endif
        if (fullResolutionMixture)
        {
            RWTexture2D<uint2> mixtureOutput = ResourceDescriptorHeap[STOCHASTIC_DENOISE_MIXTURE_UAV];
            mixtureOutput[pixel] = 0u;
        }
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
    float2 reconstructedUnshadowed = 0.0f; // Paired: x with the same weights.
    // Normalized: the guide this frame's value stands for (DENOISE_NORMALIZE_TEMPORAL_WEIGHTS): the reconstruction's weighted
    // guide, or the guide where the one sample was taken; the output pixel's otherwise.
    float2 frameGuide = DenoiseGuide(pixel, normalized);
    if (downsampled)
    {
        // Reconstruction: each of the four nearest samples is weighted by its bilinear weight times its agreement with this
        // pixel's depth and normal (and, normalized, by the guide where it was taken: a ratio of sums). The frame's value is
        // their weighted mean (DENOISE_RECONSTRUCT_MEAN 1); one of them is also chosen at random in proportion to the same
        // weights, and its compressed luminance feeds the moments (with DENOISE_RECONSTRUCT_MEAN 0 it is the frame's value,
        // the path before 2026-11-15).
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
            const float bilinear = (offset.x ? fraction.x : 1.0f - fraction.x) * (offset.y ? fraction.y : 1.0f - fraction.y);
            weights[tap] = bilinear * DenoiseSurfaceWeight(gs_sampleSurface[slot], surface);
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
            float2 guideTotal = 0.0f, meanUnshadowed = 0.0f;
            [unroll]
            for (uint tap = 0; tap < 4u; ++tap)
            {
                const uint slot = uint(baseSlot.y + int(tap >> 1u)) * uint(regionSize) + uint(baseSlot.x + int(tap & 1u));
#if DENOISE_NORMALIZE_GUIDE_WEIGHTS
                // Not normalized, the guide reads 1 and these are the weights alone.
                const float2 w = weights[tap] * gs_sampleGuide[slot];
#else
                const float2 w = weights[tap];
#endif
                meanDiffuse += gs_diffuse[slot] * w.x;
                meanSpecular += gs_specular[slot] * w.y;
#if DENOISE_PAIRED_AVAILABLE
                meanUnshadowed += gs_unshadowed[slot] * w;
#endif
                guideTotal += w;
            }
            // A lobe whose four guides are all 0 has no ratio to give (and its output is 0).
            reconstructedDiffuse = guideTotal.x > 0.0f ? meanDiffuse / guideTotal.x : 0.0f;
            reconstructedSpecular = guideTotal.y > 0.0f ? meanSpecular / guideTotal.y : 0.0f;
            reconstructedUnshadowed = select(guideTotal > 0.0f, meanUnshadowed / max(guideTotal, 1e-30f), 0.0f);
            reconstructed = true;
#if DENOISE_NORMALIZE_GUIDE_WEIGHTS
            frameGuide = guideTotal / total;
#endif
        }
#endif
#if DENOISE_NORMALIZE_GUIDE_WEIGHTS
        if (!reconstructed)
        {
            frameGuide = gs_sampleGuide[sourceSlot];
        }
#endif
    }
    const float3 currentDiffuse = reconstructed ? reconstructedDiffuse : gs_diffuse[sourceSlot];
    const float3 currentSpecular = reconstructed ? reconstructedSpecular : gs_specular[sourceSlot];
#if DENOISE_PAIRED_AVAILABLE
    const float2 currentUnshadowed = reconstructed ? reconstructedUnshadowed : gs_unshadowed[sourceSlot];
#else
    const float2 currentUnshadowed = 0.0f;
#endif
    const float2 currentCompressed = gs_compressed[sourceSlot].xy;
    float2 sum = 0.0f, sum2 = 0.0f, peak = 0.0f, lit = 0.0f;
    float count = 0.0f, boundMax = 0.0f;
    // Only the radiance path, DENOISE_NORMALIZE_BOUND, DENOISE_BOUND_CHANGE and view 4 read boundMax: normalized, the 25 taps'
    // bounds are not loaded (DENOISE_SKIP_UNUSED_BOUND, docs/performance/stochastic-lighting-experiments.md "Performance pass (2026-11-17)").
    const bool usesBound = !DENOISE_SKIP_UNUSED_BOUND || !normalized || DENOISE_NORMALIZE_BOUND || DENOISE_BOUND_CHANGE || DENOISE_DEBUG_VIEW == 4;
#if DENOISE_RECEIVER_STATS
    // The same, weighted by each sample's agreement with this pixel's surface (DENOISE_RECEIVER_STATS).
    float2 receiverSum = 0.0f, receiverSum2 = 0.0f, receiverPeak = 0.0f, receiverLit = 0.0f;
    float receiverWeight = 0.0f, receiverWeight2 = 0.0f, receiverBound = 0.0f, receiverPeakCount = 0.0f;
#endif
#if DENOISE_RECONSTRUCT_MOMENTS
    float2 cellLuminance[25];
    float cellWeight[25];
#endif
    // DENOISE_STATS_ROW_LOOP (docs/performance/stochastic-lighting-experiments.md "Performance pass (2026-11-17)"): the rows as a loop, so at most a
    // row's groupshared loads are in flight (unrolled, all 25 taps' are: the pass's register peak).
#if DENOISE_STATS_ROW_LOOP && !DENOISE_RECONSTRUCT_MOMENTS
    [loop]
#else
    [unroll]
#endif
    for (int y = -DENOISE_BORDER; y <= DENOISE_BORDER; ++y)
    {
        [unroll]
        for (int x = -DENOISE_BORDER; x <= DENOISE_BORDER; ++x)
        {
            const int neighbourSlot = (centerCell.y + DENOISE_BORDER + y) * regionSize + centerCell.x + DENOISE_BORDER + x;
            const float3 c = gs_compressed[neighbourSlot];
            const float bound = usesBound ? gs_bound[neighbourSlot] : 0.0f;
            boundMax = max(boundMax, bound);
            sum += c.xy * c.z;
            peak = max(peak, c.xy * c.z);
            lit += select(c.xy > 0.0f, c.z, 0.0f);
            sum2 += c.xy * c.xy * c.z;
            count += c.z;
            float w = c.z;
#if DENOISE_RECEIVER_STATS
            w = c.z * DenoiseSurfaceWeight(gs_sampleSurface[neighbourSlot], surface);
            receiverSum += c.xy * w;
            receiverSum2 += c.xy * c.xy * w;
            receiverLit += select(c.xy > 0.0f, w, 0.0f);
            receiverWeight += w;
            receiverWeight2 += w * w;
            if (w >= DENOISE_RECEIVER_PEAK_WEIGHT)
            {
                receiverPeak = max(receiverPeak, c.xy);
                receiverBound = max(receiverBound, bound);
                receiverPeakCount += 1.0f;
            }
#endif
#if DENOISE_RECONSTRUCT_MOMENTS
            const int cell = (y + DENOISE_BORDER) * 5 + x + DENOISE_BORDER;
            cellLuminance[cell] = float2(DenoiseLuminance(gs_diffuse[neighbourSlot]), DenoiseLuminance(gs_specular[neighbourSlot]));
            cellWeight[cell] = w;
#endif
        }
    }
    float2 neighbourhoodMean = sum / max(count, 1.0f);
    float2 neighbourhoodVariance = max(sum2 / max(count, 1.0f) - neighbourhoodMean * neighbourhoodMean, 0.0f);
#if DENOISE_RECEIVER_STATS
    if (receiverWeight >= DENOISE_RECEIVER_MIN_WEIGHT)
    {
#if (DENOISE_RECEIVER_STATS & 1)
        neighbourhoodMean = receiverSum / receiverWeight;
        neighbourhoodVariance = max(receiverSum2 / receiverWeight - neighbourhoodMean * neighbourhoodMean, 0.0f);
#endif
        // The spike limit compares lit against a fraction of count; the effective count below keeps the weighted fraction.
        const float effectiveCount = receiverWeight * receiverWeight / max(receiverWeight2, 1e-6f);
#if (DENOISE_RECEIVER_STATS & 2)
        if (receiverPeakCount > 0.0f)
        {
            peak = receiverPeak;
        }
        lit = receiverLit / receiverWeight * count;
#endif
#if (DENOISE_RECEIVER_STATS & 4)
        if (receiverPeakCount > 0.0f)
        {
            boundMax = receiverBound;
        }
#endif
#if (DENOISE_RECEIVER_STATS & 1)
        lit *= effectiveCount / max(count, 1.0f);
        count = effectiveCount;
#endif
    }
#endif
    // Rectification's statistics. With DENOISE_RECONSTRUCT_MOMENTS and reduced resolution, of the reconstructed signal: the
    // compressed luminance of each 2x2 mean of the 5x5 samples (the 16 of them overlap, so they count as a quarter as many).
    float2 rectifyMean = neighbourhoodMean;
    float2 rectifyVariance = neighbourhoodVariance;
    float rectifyCount = count;
#if DENOISE_RECONSTRUCT_MOMENTS
    if (downsampled)
    {
        float2 boxSum = 0.0f, boxSum2 = 0.0f;
        float boxWeight = 0.0f;
        [unroll]
        for (int by = 0; by < 4; ++by)
        {
            [unroll]
            for (int bx = 0; bx < 4; ++bx)
            {
                const int cell = by * 5 + bx;
                const float w0 = cellWeight[cell], w1 = cellWeight[cell + 1], w2 = cellWeight[cell + 5], w3 = cellWeight[cell + 6];
                const float total = w0 + w1 + w2 + w3;
                if (total > 0.0f)
                {
                    const float2 mean = (cellLuminance[cell] * w0 + cellLuminance[cell + 1] * w1 + cellLuminance[cell + 5] * w2 + cellLuminance[cell + 6] * w3) / total;
                    const float2 c = float2(DenoiseCompress(mean.x), DenoiseCompress(mean.y));
                    const float w = 0.25f * total;
                    boxSum += c * w;
                    boxSum2 += c * c * w;
                    boxWeight += w;
                }
            }
        }
        if (boxWeight > 0.0f)
        {
            rectifyMean = boxSum / boxWeight;
            rectifyVariance = max(boxSum2 / boxWeight - rectifyMean * rectifyMean, 0.0f);
            rectifyCount = max(count * 0.25f, 1.0f);
        }
    }
#endif

    // ---- Last frame's history, by four bilinear taps each validated against the surface it was made for.
    DenoiseHistory previousDiffuse = (DenoiseHistory)0;
    DenoiseHistory previousSpecular = (DenoiseHistory)0;
#if DENOISE_DEBUG_VIEW == 6
    float debugCross = 0.0f;
#endif
    // Light-mixture confidence: the history's mixture (shares, buckets 0-3 and 4-7), reprojected with its taps.
    float4 mixtureLow = 0.0f, mixtureHigh = 0.0f;
    bool mixtureKnown = false;
    if ((STOCHASTIC_FLAGS & STOCHASTIC_FLAG_HISTORY_VALID) != 0u)
    {
        Texture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
        RWTexture2D<uint2> previousSurface = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_SURFACE_HISTORY_UAV];
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
        const float3 previousPosition = mul(float4(surface.position, 1.0f), camera.prevView).xyz;
        const float expectedDepth = -previousPosition.z;
        float weightSum = 0.0f;
        float4 diffuseSum = 0.0f, specularSum = 0.0f, momentsSum = 0.0f, changeSum = 0.0f;
#if DENOISE_REPROJECT_PLANE || DENOISE_DEBUG_VIEW == 6
        // This point's plane in last frame's view space: the geometric normal, from this frame's depth, rotated there.
        const float3 normalVS = DenoiseGeometricNormal(pixel, surface.depth, screen, camera);
        const float3 previousNormal = mul(mul(normalVS, (float3x3)camera.viewInverse), (float3x3)camera.prevView);
        const bool planeKnown = dot(normalVS, normalVS) > 0.0f;
        float depthAcceptedWeight = 0.0f, crossWeight = 0.0f;
#endif
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
#if DENOISE_REPROJECT_PLANE || DENOISE_DEBUG_VIEW == 6
            {
                // Last frame's surface at the tap, against this point's plane (both in last frame's view space).
                const float3 tapPosition = DenoiseViewPosition(float2(coordinate), asfloat(stored.x), resolution, camera);
                const bool offPlane = planeKnown
                    && abs(dot(previousNormal, tapPosition - previousPosition)) > DENOISE_PLANE_TOLERANCE * max(expectedDepth, 1e-3f);
                depthAcceptedWeight += weight;
                crossWeight += offPlane ? weight : 0.0f;
#if DENOISE_REPROJECT_PLANE
                if (offPlane)
                {
                    continue;
                }
#endif
            }
#endif
            weightSum += weight;
            diffuseSum += previousHistoryDiffuse[coordinate] * weight;
            specularSum += previousHistorySpecular[coordinate] * weight;
            momentsSum += previousHistoryMoments[coordinate] * weight;
#if DENOISE_CHANGE_DETECT || DENOISE_BOUND_CHANGE || DENOISE_RECONSTRUCT_MOMENTS || DENOISE_NORMALIZE_TEMPORAL_WEIGHTS
            changeSum += previousHistoryChange[coordinate] * weight;
#endif
            if (fullResolutionMixture)
            {
                RWTexture2D<uint2> previousMixture = ResourceDescriptorHeap[STOCHASTIC_DENOISE_PREVIOUS_MIXTURE_UAV];
                const uint2 packed = previousMixture[coordinate];
                mixtureLow += StochasticUnpackShares(packed.x) * weight;
                mixtureHigh += StochasticUnpackShares(packed.y) * weight;
            }
        }
#if DENOISE_DEBUG_VIEW == 6
        debugCross = depthAcceptedWeight > 0.0f ? crossWeight / depthAcceptedWeight : 0.0f;
#endif
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
            mixtureLow /= weightSum;
            mixtureHigh /= weightSum;
            mixtureKnown = fullResolutionMixture;
        }
    }
    // This frame's mixture around this pixel: the guide pass's signatures under a tent of DENOISE_MIXTURE_RADIUS pixels, over
    // the pixels on this surface (depth within DENOISE_RECONSTRUCT_DEPTH).
    float4 signatureLow = 0.0f, signatureHigh = 0.0f;
    if (fullResolutionMixture)
    {
        Texture2D<uint2> signature = ResourceDescriptorHeap[STOCHASTIC_DENOISE_SIGNATURE_SRV];
        Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
        float signatureWeight = 0.0f;
        [unroll]
        for (int sy = -DENOISE_MIXTURE_RADIUS; sy <= DENOISE_MIXTURE_RADIUS; ++sy)
        {
            [unroll]
            for (int sx = -DENOISE_MIXTURE_RADIUS; sx <= DENOISE_MIXTURE_RADIUS; ++sx)
            {
                const int2 coordinate = clamp(pixel + int2(sx, sy), int2(0, 0), screen - 1);
                const float tapDepth = depthTexture[coordinate];
                float w = float(DENOISE_MIXTURE_RADIUS + 1 - abs(sx)) * float(DENOISE_MIXTURE_RADIUS + 1 - abs(sy));
                w *= (sx == 0 && sy == 0) || (asuint(tapDepth) != 0x7F7FFFFFu
                    && abs(tapDepth - surface.depth) <= DENOISE_RECONSTRUCT_DEPTH * surface.depth) ? 1.0f : 0.0f;
                if (w > 0.0f)
                {
                    const uint2 packed = signature[coordinate];
                    signatureLow += StochasticUnpackShares(packed.x) * w;
                    signatureHigh += StochasticUnpackShares(packed.y) * w;
                    signatureWeight += w;
                }
            }
        }
        signatureLow /= signatureWeight;
        signatureHigh /= signatureWeight;
    }
    const float u = float(DenoiseHash((uint(pixel.x) | (uint(pixel.y) << 16u)) ^ DenoiseHash(STOCHASTIC_FRAME)) >> 8u) * (1.0f / 16777216.0f);
    // The mixture history's stochastic rounding: one number per bucket, spread from u by the golden ratio.
    const float4 mixtureDitherLow = frac(u + float4(0.0f, 1.0f, 2.0f, 3.0f) * 0.618034f);
    const float4 mixtureDitherHigh = frac(u + float4(4.0f, 5.0f, 6.0f, 7.0f) * 0.618034f);
    // Light-mixture confidence (above): the share of the unshadowed light that has changed lights since the history was made.
    // Both mixtures are kept packed from here on (they would otherwise hold 16 registers through the rest of the pass).
    float mixtureConfidence = 1.0f;
    uint2 signaturePacked = 0u, mixturePacked = 0u;
#if STOCHASTIC_SIGNATURE_LOWRES
    if (mixture)
    {
        mixtureConfidence = blockConfidence;
    }
#endif
    if (fullResolutionMixture)
    {
        if (mixtureKnown)
        {
            const float moved = 1.0f - StochasticMixtureOverlap(mixtureLow, mixtureHigh, signatureLow, signatureHigh);
            mixtureConfidence = saturate(1.0f - DENOISE_MIXTURE_GAIN * max(moved - DENOISE_MIXTURE_TOLERANCE, 0.0f));
            mixturePacked = StochasticPackSignature(mixtureLow, mixtureHigh, mixtureDitherLow, mixtureDitherHigh);
        }
        signaturePacked = StochasticPackSignature(signatureLow, signatureHigh, 0.5f, 0.5f);
    }
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
#if DENOISE_DEBUG_VIEW == 4
    // Diagnostic: the bound's grip on the reprojected history, before the clamp (the spatial pass shows it).
    const float debugBoundRatio = DenoiseLuminance(previousDiffuse.value * surface.diffuseAlbedo + previousSpecular.value * surface.specularAlbedo)
        / (DENOISE_BOUND_MARGIN * boundMax + 1e-4f);
    const float2 debugBound = min(float2(debugBoundRatio, boundMax), DENOISE_FP16_LIMIT);
#endif
    if (!normalized || (DENOISE_NORMALIZE_BOUND && !paired)) // Paired, the alphas hold no bound.
    {
        DenoiseApplyBound(previousDiffuse, previousSpecular, surface, boundMax, DenoiseGuide(pixel, normalized));
    }
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
        // (DENOISE_NORMALIZE_TEMPORAL_WEIGHTS: the history's log2 guide, in zw, unchanged with it.)
        historyChange[pixel] = float4(previousDiffuse.area, previousSpecular.area);
#endif
        const float2 historyVariance = max(float2(previousDiffuse.moments.y - previousDiffuse.moments.x * previousDiffuse.moments.x,
            previousSpecular.moments.y - previousSpecular.moments.x * previousSpecular.moments.x), 0.0f);
        const float2 historySlope = 0.6931472f * (1.0f + float2(DenoiseLuminance(previousDiffuse.value), DenoiseLuminance(previousSpecular.value)));
        varianceOutput[pixel] = min(historyVariance * historySlope * historySlope, DENOISE_FP16_LIMIT);
#if DENOISE_DEBUG_VIEW == 4
        varianceOutput[pixel] = debugBound;
#elif DENOISE_DEBUG_VIEW == 6
        varianceOutput[pixel] = float2(min(historyVariance.x * historySlope.x * historySlope.x, DENOISE_FP16_LIMIT), debugCross);
#elif DENOISE_DEBUG_VIEW == 8 || DENOISE_DEBUG_VIEW == 9
        varianceOutput[pixel] = float2(min(historyVariance.x * historySlope.x * historySlope.x, DENOISE_FP16_LIMIT), 1.0f);
#endif
#if STOCHASTIC_SIGNATURE_LOWRES
        if (mixtureWriter)
        {
            DENOISE_WRITE_BLOCK_MIXTURE(0.0f, u);
        }
#endif
        if (fullResolutionMixture)
        {
            // The history's mixture goes with the history, unchanged (or this frame's where there was none).
            RWTexture2D<uint2> mixtureOutput = ResourceDescriptorHeap[STOCHASTIC_DENOISE_MIXTURE_UAV];
            mixtureOutput[pixel] = mixtureKnown ? mixturePacked : signaturePacked;
        }
        return;
    }
    // The change test reads the history as reprojected, before the per-pixel test shortens it.
    const float changeLimitDiffuse = DenoiseChangeLimit(previousDiffuse, neighbourhoodMean.x, neighbourhoodVariance.x, count);
    const float changeLimitSpecular = DenoiseChangeLimit(previousSpecular, neighbourhoodMean.y, neighbourhoodVariance.y, count);
#if DENOISE_RECONSTRUCT_MOMENTS
    // On the reconstructed signal's moments (kept in area); the raw-sample moments carry on for the noise estimate.
    {
        DenoiseHistory reconstructedDiffuse = previousDiffuse;
        DenoiseHistory reconstructedSpecular = previousSpecular;
        reconstructedDiffuse.moments = previousDiffuse.area;
        reconstructedSpecular.moments = previousSpecular.area;
        previousDiffuse.length = DenoiseRectify(reconstructedDiffuse, rectifyMean.x, rectifyVariance.x, rectifyCount).length;
        previousSpecular.length = DenoiseRectify(reconstructedSpecular, rectifyMean.y, rectifyVariance.y, rectifyCount).length;
    }
#else
    previousDiffuse = DenoiseRectify(previousDiffuse, rectifyMean.x, rectifyVariance.x, rectifyCount);
    previousSpecular = DenoiseRectify(previousSpecular, rectifyMean.y, rectifyVariance.y, rectifyCount);
#endif
    previousDiffuse.length = min(previousDiffuse.length, changeLimitDiffuse);
    previousSpecular.length = min(previousSpecular.length, changeLimitSpecular);
    // Visibility gradients (above): the held gradient of this pixel's cell; 1 unless on.
    float visibilityConfidence = 1.0f;
    if (gradient)
    {
        RWStructuredBuffer<uint> gradientBuffer = ResourceDescriptorHeap[STOCHASTIC_DENOISE_GRADIENT_UAV];
        const uint2 cells = StochasticGradientCells(uint2(grid));
        const uint2 cell = min((uint2(pixel) / (downsampled ? 2u : 1u)) / STOCHASTIC_GRADIENT_CELL, cells - 1u);
        const float held = asfloat(gradientBuffer[StochasticGradientHeldIndex(StochasticGradientCapacity(uint2(grid)), STOCHASTIC_FRAME & 1u,
            cell.y * cells.x + cell.x)]);
        visibilityConfidence = saturate(1.0f - DENOISE_GRADIENT_GAIN * max(held - DENOISE_GRADIENT_TOLERANCE, 0.0f));
    }
    // Light-mixture confidence (above) and the visibility gradients': 1 unless on and compared.
    const float appliedVisibility = DENOISE_GRADIENT_APPLY ? visibilityConfidence : 1.0f;
    previousDiffuse.length *= mixtureConfidence * appliedVisibility;
    previousSpecular.length *= mixtureConfidence * appliedVisibility;
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
        const float2 spikeScale = float2(luminanceDiffuse > limit.x ? limit.x / luminanceDiffuse : 1.0f,
            luminanceSpecular > limit.y ? limit.y / luminanceSpecular : 1.0f);
        previousDiffuse.value *= spikeScale.x;
        previousSpecular.value *= spikeScale.y;
#if DENOISE_PAIRED_SPIKE
        if (paired)
        {
            previousDiffuse.area *= spikeScale; // Paired: the unshadowed history (diffuse, specular) with it.
        }
#endif
    }
#endif

    // ---- Blend: an exponential moving average with weight 1 / length, length capped by rectification.
    const float lengthDiffuse = min(previousDiffuse.length + 1.0f, DENOISE_MAX_HISTORY);
    const float lengthSpecular = min(previousSpecular.length + 1.0f, DENOISE_MAX_HISTORY);
    float3 diffuse = lerp(previousDiffuse.value, currentDiffuse, 1.0f / lengthDiffuse);
    float3 specular = lerp(previousSpecular.value, currentSpecular, 1.0f / lengthSpecular);
    float2 blendWeight = 1.0f / float2(lengthDiffuse, lengthSpecular); // This frame's weight in the blend, per lobe.
#if DENOISE_NORMALIZE_TEMPORAL_WEIGHTS
    float2 historyGuideLog = 0.0f;
    if (normalized)
    {
        // A ratio of sums over time (above): this frame's ratio weighted by its guide against the history's average guide,
        // kappa = frameGuide / historyGuide; the average guide then blends with the same 1 / length. Without a history the
        // frame's own guide stands for it (kappa 1).
        // The change history's z and w (diffuse, specular) arrive as specular's area.
        float2 historyGuide = float2(previousDiffuse.length > 0.0f ? exp2(previousSpecular.area.x) : frameGuide.x,
            previousSpecular.length > 0.0f ? exp2(previousSpecular.area.y) : frameGuide.y);
        if (DENOISE_NORMALIZE_GUIDE_RANGE > 0.0f)
        {
            historyGuide.x = min(historyGuide.x, DENOISE_NORMALIZE_GUIDE_RANGE * frameGuide.x);
        }
        if (DENOISE_NORMALIZE_GUIDE_RANGE_SPECULAR > 0.0f)
        {
            historyGuide.y = min(historyGuide.y, DENOISE_NORMALIZE_GUIDE_RANGE_SPECULAR * frameGuide.y);
        }
        const float2 alpha = 1.0f / float2(lengthDiffuse, lengthSpecular);
        const float2 kept = (1.0f - alpha) * historyGuide;
        const float2 added = alpha * frameGuide;
        const float2 total = kept + added;
        const float2 currentWeight = select(total > 0.0f, added / max(total, 1e-30f), alpha);
        diffuse = lerp(previousDiffuse.value, currentDiffuse, currentWeight.x);
        specular = lerp(previousSpecular.value, currentSpecular, currentWeight.y);
        historyGuideLog = log2(max(total, 1e-30f));
        blendWeight = currentWeight;
    }
#endif
    // Paired: the unshadowed history with exactly the numerator's weights (the change history's r, g arrive as diffuse's area).
    const float2 unshadowed = paired ? lerp(previousDiffuse.area, currentUnshadowed, blendWeight) : 0.0f;
#if STOCHASTIC_SIGNATURE_LOWRES
    if (mixtureWriter)
    {
        // The block's mixture after this frame, with this (top-left) pixel's blend weight, the faster lobe's.
        DENOISE_WRITE_BLOCK_MIXTURE(max(blendWeight.x, blendWeight.y), frac(u + 0.5f));
    }
#endif
    if (fullResolutionMixture)
    {
        // The history's mixture after this frame: blended with the frame's weight, the faster lobe's (one mixture serves both).
        RWTexture2D<uint2> mixtureOutput = ResourceDescriptorHeap[STOCHASTIC_DENOISE_MIXTURE_UAV];
        const float mixtureWeight = mixtureKnown ? max(blendWeight.x, blendWeight.y) : 1.0f;
        mixtureOutput[pixel] = StochasticPackSignature(
            lerp(StochasticUnpackShares(mixturePacked.x), StochasticUnpackShares(signaturePacked.x), mixtureWeight),
            lerp(StochasticUnpackShares(mixturePacked.y), StochasticUnpackShares(signaturePacked.y), mixtureWeight),
            frac(mixtureDitherLow + 0.5f), frac(mixtureDitherHigh + 0.5f));
    }
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
#elif DENOISE_NORMALIZE_TEMPORAL_WEIGHTS
    // r, g: paired, the unshadowed history (rounded as the numerator is, so its expectation survives fp16); else 0.
    historyChange[pixel] = float4(DenoiseRoundToHalf(float3(min(unshadowed, DENOISE_FP16_LIMIT), 0.0f), u).xy, historyGuideLog);
#elif DENOISE_PAIRED_AVAILABLE
    historyChange[pixel] = float4(DenoiseRoundToHalf(float3(min(unshadowed, DENOISE_FP16_LIMIT), 0.0f), u).xy, 0.0f, 0.0f);
#elif DENOISE_RECONSTRUCT_MOMENTS
    {
        // The reconstructed value's compressed moments (the raw sample's at full resolution, where the two are the same).
        const float2 reconstructedCompressed = reconstructed
            ? float2(DenoiseCompress(DenoiseLuminance(currentDiffuse)), DenoiseCompress(DenoiseLuminance(currentSpecular))) : currentCompressed;
        historyChange[pixel] = float4(
            lerp(previousDiffuse.area, float2(reconstructedCompressed.x, reconstructedCompressed.x * reconstructedCompressed.x), 1.0f / lengthDiffuse),
            lerp(previousSpecular.area, float2(reconstructedCompressed.y, reconstructedCompressed.y * reconstructedCompressed.y), 1.0f / lengthSpecular));
    }
#endif
    varianceOutput[pixel] = min(compressedVariance * slope * slope, DENOISE_FP16_LIMIT);
#if DENOISE_DEBUG_VIEW == 4
    varianceOutput[pixel] = debugBound;
#elif DENOISE_DEBUG_VIEW == 6
    varianceOutput[pixel] = float2(min(compressedVariance.x * slope.x * slope.x, DENOISE_FP16_LIMIT), debugCross);
#elif DENOISE_DEBUG_VIEW == 8
    varianceOutput[pixel] = float2(min(compressedVariance.x * slope.x * slope.x, DENOISE_FP16_LIMIT), mixtureConfidence * visibilityConfidence);
#elif DENOISE_DEBUG_VIEW == 9
    varianceOutput[pixel] = float2(min(compressedVariance.x * slope.x * slope.x, DENOISE_FP16_LIMIT), visibilityConfidence);
#endif
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

// A spatial tap's surface: its linear depth and shading normal (normalize(NormalRoughness.xyz)); false for background.
// DENOISE_SPATIAL_SURFACE_HISTORY 1 (docs/performance/stochastic-lighting-experiments.md "Performance pass (2026-11-17)"): from this frame's surface
// history, which selection writes for every pixel (R32G32_UInt: the depth's bits, the same normal octahedral-packed at 16 bits
// a component, within ~3e-5): one load instead of two. 0: the depth and normal textures.
#ifndef DENOISE_SPATIAL_SURFACE_HISTORY
#define DENOISE_SPATIAL_SURFACE_HISTORY 1
#endif
bool DenoiseTapSurface(int2 coordinate, out float depth, out float3 normal)
{
#if DENOISE_SPATIAL_SURFACE_HISTORY
    RWTexture2D<uint2> surfaceHistory = ResourceDescriptorHeap[STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV];
    const uint2 stored = surfaceHistory[coordinate];
    depth = asfloat(stored.x);
    normal = StochasticUnpackNormal(stored.y);
    return stored.x != 0x7F7FFFFFu;
#else
    Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
    Texture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
    depth = depthTexture[coordinate];
    normal = 0.0f;
    if (asuint(depth) == 0x7F7FFFFFu)
    {
        return false;
    }
    normal = normalize(normalRoughness[coordinate].xyz);
    return true;
#endif
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

    const float4 centerDiffuse = historyDiffuse[pixel];
    const float4 centerSpecular = historySpecular[pixel];
    // The luminance stop's scale: the variance of one frame's input over the history length, the variance of the temporal
    // average itself.
#if DENOISE_DEBUG_VIEW == 6 || DENOISE_DEBUG_VIEW == 8 || DENOISE_DEBUG_VIEW == 9
    // The temporal pass wrote the cross-surface share (view 6) or a confidence (views 8, 9) over specular's variance;
    // specular's stop takes diffuse's meanwhile.
    const float debugCross = varianceInput[pixel].y;
    const float2 inputVariance = varianceInput[pixel].xx;
#else
    const float2 inputVariance = varianceInput[pixel];
#endif
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
    // Paired (docs/stochastic_lighting.md "The paired ratio"): the unshadowed history H_X (the change history's r, g), filtered
    // with each lobe's own tap weights, which look at geometry and the numerator only, so the two keep their covariance.
    const bool normalized = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_NORMALIZE) != 0u;
    const bool paired = DENOISE_PAIRED_AVAILABLE && normalized && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_PAIRED) != 0u;
    RWTexture2D<float4> historyChange = ResourceDescriptorHeap[STOCHASTIC_DENOISE_HISTORY_CHANGE_UAV];
    const float2 centerUnshadowed = paired ? historyChange[pixel].xy : 0.0f;
    float2 unshadowed = centerUnshadowed * float2(weightDiffuse, weightSpecular);
    // Paired with DENOISE_PAIRED_MIN_SAMPLES: sum(w^2 / n_i), each tap's weight squared over its history's sample count.
    float2 inverseSamples = float2(weightDiffuse, weightSpecular) / max(2.0f * float2(centerDiffuse.a, centerSpecular.a) - 1.0f, 1.0f);
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
        for (uint tap = 0; tap < DENOISE_SPATIAL_FLOOR_TAP_COUNT; ++tap)
        {
            const float2 direction = tap == 0u ? axis : tap == 1u ? -axis : tap == 2u ? float2(-axis.y, axis.x) : float2(axis.y, -axis.x);
            const int2 coordinate = pixel + int2(round(direction));
            if (DENOISE_SPATIAL_MIN_RADIUS < 0.5f || any(coordinate < 0) || any(coordinate >= screen) || all(coordinate == pixel))
            {
                continue;
            }
            float depth;
            float3 normal;
            if (!DenoiseTapSurface(coordinate, depth, normal))
            {
                continue;
            }
            const float geometry = exp(-abs(depth - surface.depth) / (0.02f * surface.depth + 1e-3f)) *
                pow(saturate(dot(normal, surface.normal)), 32.0f);
            const float4 valueDiffuse = historyDiffuse[coordinate];
            const float wd = valueDiffuse.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(valueDiffuse.rgb) - centerLuminanceDiffuse) / sigmaDiffuse) : 0.0f;
            diffuse += valueDiffuse.rgb * wd;
            weightDiffuse += wd;
            float ws = 0.0f;
            if (filterSpecular)
            {
                const float4 valueSpecular = historySpecular[coordinate];
                ws = valueSpecular.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(valueSpecular.rgb) - centerLuminanceSpecular) / sigmaSpecular) : 0.0f;
                specular += valueSpecular.rgb * ws;
                weightSpecular += ws;
            }
            if (paired)
            {
                unshadowed += historyChange[coordinate].xy * float2(wd, ws);
                if (DENOISE_PAIRED_MIN_SAMPLES > 0.0f)
                {
                    inverseSamples += float2(wd * wd / max(2.0f * valueDiffuse.a - 1.0f, 1.0f),
                        ws > 0.0f ? ws * ws / max(2.0f * historySpecular[coordinate].a - 1.0f, 1.0f) : 0.0f);
                }
            }
        }
    }
    else
    {
        [unroll]
        for (uint tap = 0; tap < 12u; tap += 12u / DENOISE_SPATIAL_TAP_COUNT)
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
                float depth;
                float3 normal;
                if (!DenoiseTapSurface(coordinate, depth, normal))
                {
                    continue;
                }
                const float geometry = exp(-abs(depth - surface.depth) / (0.02f * surface.depth + 1e-3f)) *
                    pow(saturate(dot(normal, surface.normal)), 32.0f);
                if (signal == 0u)
                {
                    const float4 value = historyDiffuse[coordinate];
                    const float weight = value.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(value.rgb) - centerLuminanceDiffuse) / sigmaDiffuse) : 0.0f;
                    diffuse += value.rgb * weight;
                    weightDiffuse += weight;
                    if (paired)
                    {
                        unshadowed.x += historyChange[coordinate].x * weight;
                        inverseSamples.x += DENOISE_PAIRED_MIN_SAMPLES > 0.0f ? weight * weight / max(2.0f * value.a - 1.0f, 1.0f) : 0.0f;
                    }
                }
                else
                {
                    const float4 value = historySpecular[coordinate];
                    const float weight = value.a > 0.0f ? geometry * exp(-abs(DenoiseLuminance(value.rgb) - centerLuminanceSpecular) / sigmaSpecular) : 0.0f;
                    specular += value.rgb * weight;
                    weightSpecular += weight;
                    if (paired)
                    {
                        unshadowed.y += historyChange[coordinate].y * weight;
                        inverseSamples.y += DENOISE_PAIRED_MIN_SAMPLES > 0.0f ? weight * weight / max(2.0f * value.a - 1.0f, 1.0f) : 0.0f;
                    }
                }
            }
        }
    }
    diffuse /= max(weightDiffuse, 1e-6f);
    specular /= max(weightSpecular, 1e-6f);
    unshadowed /= max(float2(weightDiffuse, weightSpecular), 1e-6f);
    // Paired: the numerator over the unshadowed history (or 1 where that has no weight yet), both filtered alike. The center
    // pixel's own (the temporal pass's result) for the diagnostics.
    const float2 tapWeights = float2(weightDiffuse, weightSpecular);
    const float2 effectiveSamples = tapWeights * tapWeights / max(inverseSamples, 1e-12f);
    const float2 pairedDenominator = paired ? DenoisePairedDenominator(unshadowed, float2(centerDiffuse.a, centerSpecular.a), effectiveSamples) : 1.0f;
    const float2 centerDenominator = paired ? DenoisePairedDenominator(centerUnshadowed, float2(centerDiffuse.a, centerSpecular.a),
        max(2.0f * float2(centerDiffuse.a, centerSpecular.a) - 1.0f, 0.0f)) : 1.0f;
    diffuse /= pairedDenominator.x;
    specular /= pairedDenominator.y;
    // Normalized, the filtered ratio times this pixel's guide (the only place it is remodulated); else 1. The guide can be far
    // past fp16's range beside a light; the output is fp16.
    const float2 guide = DenoiseGuide(pixel, normalized);
    const float3 diffuseRemodulation = surface.diffuseAlbedo * guide.x;
    const float3 specularRemodulation = surface.specularAlbedo * guide.y;
    outputDiffuse[pixel] = float4(min(diffuse * diffuseRemodulation, 65504.0f), 1.0f);
    outputSpecular[pixel] = float4(min(specular * specularRemodulation, 65504.0f), 1.0f);
#ifdef DENOISE_DEBUG_PAIR
    // Diagnostic: this frame's denoised and noisy total luminance side by side, so one capture pairs them (with moving
    // lights, separate captures are separate frames).
    {
        Texture2D<float4> noisyDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV];
        Texture2D<float4> noisySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV];
        const float3 denoised = diffuse * diffuseRemodulation + specular * specularRemodulation;
        // Downsampled, the block's one sample stands in for each of its pixels.
        const int2 cell = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? pixel / 2 : pixel;
        const float2 noisyGuide = DenoiseNoisyGuide(cell, screen, normalized);
        const float3 noisy = noisyDiffuse[cell].rgb * noisyGuide.x + noisySpecular[cell].rgb * noisyGuide.y;
        // b: selection's unshadowed bound for this sample (stochasticSplit.hlsli), to check it against the noisy value.
        outputDiffuse[pixel] = float4(dot(denoised, float3(0.2126f, 0.7152f, 0.0722f)), dot(noisy, float3(0.2126f, 0.7152f, 0.0722f)), noisyDiffuse[cell].a, 1.0f);
        outputSpecular[pixel] = 0.0f;
    }
#endif
#if DENOISE_DEBUG_VIEW
    // Diagnostic views (DENOISE_DEBUG_VIEW above).
    {
        Texture2D<float4> noisyDiffuse = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_DIFFUSE_SRV];
        Texture2D<float4> noisySpecular = ResourceDescriptorHeap[STOCHASTIC_DENOISE_NOISY_SPECULAR_SRV];
        const float3 luma = float3(0.2126f, 0.7152f, 0.0722f);
        const float3 denoised = diffuse * diffuseRemodulation + specular * specularRemodulation;
        // The temporal pass's result at this pixel, before the spatial filter.
        const float temporal = min(dot(centerDiffuse.rgb / centerDenominator.x * diffuseRemodulation
            + centerSpecular.rgb / centerDenominator.y * specularRemodulation, luma), 65504.0f);
        // Downsampled, the block's one sample stands in for each of its pixels.
        const int2 cell = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? pixel / 2 : pixel;
        // Normalized, back to radiance by the guide where the sample was taken.
        const float2 noisyGuide = DenoiseNoisyGuide(cell, screen, normalized);
        // The targets are fp16: a near-field sample's luminance can pass its largest value.
        const float noisy = min(dot(noisyDiffuse[cell].rgb * noisyGuide.x + noisySpecular[cell].rgb * noisyGuide.y, luma), 65504.0f);
#if DENOISE_DEBUG_VIEW == 1
        outputDiffuse[pixel] = float4(dot(denoised, luma), noisy, temporal, noisyDiffuse[cell].a);
        outputSpecular[pixel] = 0.0f;
#elif DENOISE_DEBUG_VIEW == 2
        outputDiffuse[pixel] = float4(centerDiffuse.a, centerSpecular.a, 0.0f, 1.0f);
        outputSpecular[pixel] = 0.0f;
#elif DENOISE_DEBUG_VIEW == 3
        outputDiffuse[pixel] = float4(inputVariance, variance);
        outputSpecular[pixel] = 0.0f;
#elif DENOISE_DEBUG_VIEW == 4
        // The temporal pass wrote the bound's ratio and the neighbourhood's largest bound over the variance.
        outputDiffuse[pixel] = float4(inputVariance, 0.0f, 1.0f);
        outputSpecular[pixel] = 0.0f;
#elif DENOISE_DEBUG_VIEW == 5
        outputDiffuse[pixel] = float4(denoised, noisy);
        outputSpecular[pixel] = float4(0.0f, 0.0f, 0.0f, temporal);
#elif DENOISE_DEBUG_VIEW == 6 || DENOISE_DEBUG_VIEW == 8 || DENOISE_DEBUG_VIEW == 9
        // g: view 6 the cross-surface share, view 8 the combined confidence, view 9 the visibility gradients' alone.
        outputDiffuse[pixel] = float4(dot(denoised, luma), debugCross, temporal, noisy);
        outputSpecular[pixel] = 0.0f;
#elif DENOISE_DEBUG_VIEW == 7
        // The paired ratio's denominator. ::Diffuse: the temporal pass's unshadowed history H_X at this pixel, diffuse (r) and
        // specular (g), and the same filtered (b, a). ::Specular: this frame's noisy x where the block's sample was taken
        // (diffuse r, specular b; the expectation of each is 1) and the guide there (g, a; capped at fp16's largest).
        const float2 noisyGuideCapped = min(noisyGuide, 65504.0f);
        outputDiffuse[pixel] = float4(min(centerUnshadowed, 65504.0f), min(unshadowed, 65504.0f));
        outputSpecular[pixel] = float4(paired ? noisyDiffuse[cell].a : 0.0f, noisyGuideCapped.x, paired ? noisySpecular[cell].a : 0.0f, noisyGuideCapped.y);
#endif
    }
#endif
}
