# Stochastic lighting

Stochastic direct lighting shadows every analytic light, the sun included, by ray tracing instead of shadow maps, in the
way Unreal's MegaLights does: each pixel picks a few lights at random in proportion to their importance, traces one shadow ray per
pick, and a denoiser turns the result into one diffuse and one specular signal. Its cost follows the number of samples,
not the number of lights.

It is `ShadowingMode::StochasticRayTraced` ([lighting.md](lighting.md) "Shadowing modes"). Measurements, validations and the
experiments behind the current design are in
[performance/stochastic-lighting-experiments.md](performance/stochastic-lighting-experiments.md).

## Turning it on

- Setting `enableStochasticLighting` (menu: Post Processing and Ray Tracing, **Stochastic Lighting**; environment
  `SARP_STOCHASTIC_LIGHTING=1` at startup) adds `StochasticDirectLightingTechnique` to the recipe.
- It runs only with shadows on (`enableShadows`), clustered lighting on, raster rendering
  (path tracing replaces it; the recipe it returns to keeps it) and a device that traces clusters. Otherwise the mode stays
  `VirtualShadowMaps`. `Renderer::StochasticLightingRequested` and `StochasticLightingActive` decide.
- The BVH is built whenever it is requested, as for the ray traced sun shadow.

## What changes in the mode

- No virtual shadow map is built. `ClusterLodShadowTechnique` stays in the recipe as the "shadows on" switch, but its
  graph extension (`CLodShadow`) is not registered, so no shadow culling, raster or page pass runs. `LightManager::
  UpdateLocalShadows(false)` gives no spot or point light faces. The ray traced sun mask (`enableRayTracedShadows`) is
  not built either. `Renderer::VirtualShadowMapsActive`, published as the `virtualShadowMapsActive` setting, tells
  passes whether the shadow map's resources exist.
- The deferred pass's light loop skips every analytic light (`DirectLightingOptions::stochasticLights`) and adds
  `Builtin::StochasticLighting::Diffuse` and `::Specular` (RGBA16F, render resolution) instead.
- Forward passes (AVBOIT, deep visibility resolve) have no shadow map to read, so they light every light unshadowed,
  the sun included.
- Ray traced reflection hits are unaffected: they shade every light unshadowed except the sun, which gets its own ray.
- The sun's clipmap views still exist (`LightManager` makes them for shadow-casting directional lights), but nothing
  culls or renders them.
- The BVH leaves out skinned draws, assemblies, displaced and voxel geometry, grass and alpha-blended draws
  ([clod_raytracing.md](../../docs/clod_raytracing.md) in SARP). These cast no shadows in this mode, from the sun or any
  other light.

## Settings

| Setting | Environment | Default | Effect |
|---|---|---|---|
| `enableStochasticLighting` | `SARP_STOCHASTIC_LIGHTING` | off | The mode (rebuilds the pipeline). |
| `stochasticLightingSamplesPerPixel` | `SARP_STOCHASTIC_SAMPLES` | 2 | Lights drawn per pixel, 1 to 4, one ray each (a light drawn twice shares one, "Kernel split"). Per frame with reduced resolution, where each sampled pixel draws twice as many (at most 4); at full resolution it rebuilds the graph (the kernels' buffers hold the setting's samples). |
| `stochasticLightingDownsample` | `SARP_STOCHASTIC_DOWNSAMPLE` | 2 | 2: one pixel of each 2×2 sampled a frame, and the denoiser reconstructs the rest ("Reduced resolution"); 1: every pixel (rebuilds the graph). The reference is always full resolution. |
| `stochasticLightingReference` | `SARP_STOCHASTIC_REFERENCE` | off | The reference in place of the estimator (rebuilds the graph). |
| `stochasticLightingAccumulate` | `SARP_STOCHASTIC_ACCUMULATE` | off | The estimator keeps a running average while nothing changes, to compare with the reference (rebuilds the graph). |
| `stochasticLightingGuiding` | `SARP_STOCHASTIC_GUIDING` | on | Guiding by last frame's visible lights ("Guiding"). Per frame. |
| `stochasticLightingDenoise` | `SARP_STOCHASTIC_DENOISE` | on | The denoiser (rebuilds the graph). |
| `stochasticLightingNormalize` | `SARP_STOCHASTIC_NORMALIZE` (`=0` turns it off) | on | Guide-normalized accumulation in the denoiser ("Denoiser"). Off: the radiance path. Per frame; the denoiser's history restarts when it changes. |
| `stochasticLightingPaired` | `SARP_STOCHASTIC_PAIRED` | off | With `stochasticLightingNormalize`: the paired shadowed / unshadowed ratio ("The paired ratio"). Forces the exact guide. Per frame; the denoiser's history restarts when it changes. Without normalize it does nothing. |
| `stochasticLightingMixtureConfidence` | `SARP_STOCHASTIC_MIXTURE` (`=0` turns it off) | on | With `stochasticLightingNormalize`: light-mixture confidence, the history shortened where the lights the unshadowed light comes from have changed. Per frame; the denoiser's history restarts when it changes. Without normalize it does nothing; with paired as well, both apply. |
| `stochasticLightingVisibilityGradient` | `SARP_STOCHASTIC_VISIBILITY_GRADIENT` (`=0` turns it off) | on | With `stochasticLightingNormalize`: visibility gradients, one matched shadow ray per cell of 8×8 sampled pixels re-traces a sample of last frame's, and the history shortens where the same light's visibility changed ("Visibility gradients"). Per frame; the denoiser's history restarts when it changes. Composes with mixture confidence and paired. |

The menu has them under Post Processing and Ray Tracing (Stochastic Lighting, Reference, Accumulate, Stochastic
Samples per Pixel, Stochastic Half-Resolution Sampling, Stochastic Guiding, Stochastic Denoiser, and on the next line Normalize,
Mixture Confidence, Visibility Gradient and Paired, greyed out while the denoiser, or for the last three normalize, is off). The
debug output **Stochastic Lighting** (`OUTPUT_STOCHASTIC_LIGHTING`, 77; `SARP_DEBUG_OUTPUT_TYPE=77`) shows the stochastic lights'
radiance alone (every analytic light, the sun included), as x / (1 + x), without sky, image-based lighting or emission.

## Estimator

Three kernels, each its own pass ("Kernel split"): selection (`shaders/Stochastic/stochasticSelect.hlsl`,
`StochasticSelectPass`), the shadow rays (`stochasticTrace.hlsl`, `StochasticTracePass`) and shading with the visible-list
build (`stochasticShade.hlsl`, `StochasticShadePass`).

1. **Candidates.** The lights of the pixel's light grid froxel's opaque run ([lighting.md](lighting.md) "The light grid";
   directional lights are in every froxel): with guiding, the reprojected tile's listed lights plus at most
   `STOCHASTIC_CANDIDATE_COUNT` (24) drawn entries of the run ("Bounded candidates"). For each, the unshadowed contribution
   `f_i` is evaluated and its importance is `w_i = log2(1 + luminance(f_i))` (MegaLights' perceptual weight: one bright light
   doesn't take every sample after tone mapping). With `STOCHASTIC_CHEAP_IMPORTANCE` (1) the importance uses a Lambert + GGX D
   stand-in for `f_i`; the chosen sample is still shaded with the full BRDF, and the estimate stays unbiased because the
   importance is positive wherever the contribution is.
2. **Selection.** One weighted reservoir per sample, all fed the same candidate stream. Each reservoir holds a single
   uniform number, kept uniform after every accept/reject by sample warping. The N numbers are `u_k = frac(u + k/N)`, so the
   N picks of a set fall in different strata of its distribution, and each `u_k` is uniform on its own. A reservoir keeps its
   light's `f_i` and `w_i`, so nothing is evaluated twice.
3. **Visibility and shading.** One shadow ray per pick; the pixel's value is `Σ_k f_k V_k W / (N w_k)` with `W = Σ w_i`,
   that is `f / p` with `p = w / W` exactly.

## Guiding

After MegaLights: the previous frame's visibility guides this frame's choice of lights, and the estimator stays unbiased.
Selection is in `stochasticSelect.hlsl`, the list build in `stochasticShade.hlsl`. Each 8×8 thread group is one screen tile, so
the tile's list is built by the group that shaded it, with no separate pass. `stochasticLightingGuiding`
(`SARP_STOCHASTIC_GUIDING=0` turns it off; menu: **Stochastic Guiding**) switches it per frame.

**Visible lists.** For each tile there is a count, then up to `STOCHASTIC_VISIBLE_LIST_CAPACITY` entries (32; the
buffers have room for `STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY`, 64, so `pso.recompile` can try any capacity up to that).
Each entry holds:

- the light buffer index (20 bits);
- the low 8 bits of the light's `generation`, so a slot given to another light doesn't match;
- a 2×2 mask of the quadrants of the emitter's sampling domain that a ray reached.

The list is sorted by index and built from this frame's samples whose ray reached the light. It is a binary "seen
visible", as in MegaLights, not a visible fraction. The build:

1. **Per wave.** Each wave takes its lanes' keys in index order: a round is one `WaveActiveMin` over the lanes' smallest
   remaining keys, a `WaveActiveBitOr` of the quadrants and a `WaveActiveSum` of the weights of the samples that chose that
   light. It appends (light, quadrants, weight) to the wave's sorted run in groupshared memory. One round per unique
   light in the wave, and no group barrier.
2. **Merge.** After the one group barrier, the first wave merges the runs: lane w holds the head of wave w's run, each
   round takes the smallest head with `WaveActiveMin`, combines every run that has it, and advances them. With 64-lane
   waves there is one run, which is already the list.
3. **Capacity.** A tile whose rays reached more lights than the capacity keeps the most important: an entry's weight is
   the sum of the luminance of its samples' estimates (`f·V/p`, the light's estimated contribution to the tile), and an
   entry is kept when fewer than `STOCHASTIC_VISIBLE_LIST_CAPACITY` entries outweigh it (ties by position, so exactly the
   capacity is kept). The kept entries are compacted in index order with `WavePrefixCountBits`, since the estimator
   binary-searches the list.

The build's groupshared arrays hold one packed uint per entry (light 20 bits, quadrants 4, the weight as an 8-bit log2 code
in steps of 2^(1/4)): 2 KB per group. The weight lives only there; the stored entry has no weight. Only which lights are
listed depends on the weights: the list stays unique (each light once, its quadrants ORed) and sorted, which the hidden draw's
skip of listed entries relies on, so the estimate is unbiased whatever the capacity.

The lists and a per-pixel surface history are ping-ponged: two buffers of each, swapped every frame.

- The surface history is R32G32_UInt: linear depth and an octahedral normal. The renderer's own linear-depth history is
  overwritten before shading, so this pass keeps its own.
- On a pass's first frame (after a graph build), or after a frame with guiding off, the history is marked invalid.

**Reprojection.** A pixel moves by `Surface::Motion` into last frame. The reprojection holds if the surface stored
there is within 10% of the depth this point had in last frame's view (`Camera::prevView`) and within about 45° of its
normal. The pixel then picks one of the four nearest tiles at random, with bilinear weights, so a change between lists
doesn't show as a tile edge.

**Two local sets and the mixture.** The local lights are split in two sets:

- the reprojected list's lights (generation checked) feed the *visible* (listed) reservoirs;
- the run's other lights, a bounded draw of them ("Bounded candidates"), feed the *hidden* reservoirs, each set with its own
  stratified random numbers.

A local sample takes the hidden set with probability α:

- α is the hidden lights' share of the weight, capped at 0.2 (`STOCHASTIC_HIDDEN_SHARE`), so an occluded bright light cannot
  take every ray;
- the cap rises toward 1 as the listed lights' share falls below a quarter, since the list is likely stale;
- the cap is at least 0.5 (`STOCHASTIC_HIDDEN_SHARE_UNGUIDED`) where reprojection failed;
- α is 1 with no listed weight and 0 with no hidden weight.

A listed light's probability is `(1 − α)·w/W_visible`, and a hidden light's `α·w/W_hidden`, because the two sets
are disjoint (with the bounded draw, `W_hidden` is an estimate and the hidden probability a RIS contribution weight). The light's
contribution is evaluated again for the chosen sample, so the reservoirs hold only (u, light, weight, mask).

**Quadrant masks.** A listed light's emitter point is drawn by first choosing a quadrant of its sampling domain:

- weight 1 for each quadrant the list marks as reached, 0.15 (`STOCHASTIC_QUADRANT_FLOOR`) for each that it doesn't;
- the point is drawn uniformly within the chosen quadrant;
- the estimate is scaled by `1 / (4 p(quadrant))`, so it stays unbiased;
- unreached quadrants keep their floor, so one that becomes visible is still found.

Unlisted lights have all four quadrants, which is uniform sampling.

The domain is the light's cone around the direction from the surface to the light. Pixels in a tile see nearly the same
cone, which is what makes a tile's mask useful at its other pixels.

A 64-bit filter is built from the reprojected list once per pixel. A drawn light whose bit is clear skips the binary search
that would find it in the list.

### Bounded candidates

A pixel evaluates at most `|list| + K` local candidates, K = `STOCHASTIC_CANDIDATE_COUNT` (24; 0 takes every entry of the
run), plus its directional lights, so the candidate cost stops growing with the lights in the froxel. All of it is in
selection.

**The three sets.**

- *Listed*: every entry of the reprojected tile's visible list (at most 32), read from the list itself rather than
  found in the run. A listed light that doesn't reach this pixel gets weight 0 from the range check. Directional entries are
  skipped (they are the directional set's).
- *Directional*: taken from `sunLightIndex` ("Directional lights").
- *Hidden*: K of the run's M entries, drawn by a stratified stride: `s = M/K` (real), one offset `o` uniform in
  `[0, s)`, entries `floor(o + j·s)` for `j = 0 … K−1`, read directly as `GridIndices[first + floor(o + j·s)]`. Drawn
  entries that are listed (filter, binary search, generation) or directional are skipped. With `M ≤ K` the draw takes every
  entry, the run is walked once and split into listed and hidden lights, and the list loop is skipped.

**Each entry is drawn with probability K/M exactly.** With `s ≥ 1`, entry `i` is drawn when some `o + j·s ∈ [i, i+1)`.
Those intervals of `o` are spaced `s` apart and 1 long, so exactly a length-1 part of `[0, s)` draws `i`, and the `j` it
needs lies in `0 … K−1` for every `i < M`. So `P(i drawn) = 1/s = K/M`, whatever `i`.

**The estimator.** Let `D` be the drawn hidden lights. The reservoirs pick within `D` by `w` alone; every drawn light
carries the same factor `M/K`, so weighting each by `w·M/K` would pick the same way. The hidden total in every probability
is the estimate

    Ŵ_hidden = (M/K) · Σ_{i∈D} w_i

and a hidden pick's probability in the estimate is `(1 − δ)·α·w_i / Ŵ_hidden`. Its actual probability, given `D`, is
`(1 − δ)·α·(w_i·M/K) / Ŵ_hidden`, so the estimate `f_i / p` has expectation, given `D`, of `(M/K) · Σ_{i∈D} f_i`: the
draw's estimate of the hidden lights' sum (`Ŵ_hidden / w_i` is the pick's RIS contribution weight). Over draws its expectation
is `Σ_hidden f_i · P(i drawn) · M/K = Σ_hidden f_i`, because each hidden light is drawn with probability K/M. The
listed and directional parts are exact, so the whole estimate is unbiased, provided every set with positive weight can be
chosen:

- `α > 0` whenever `Ŵ_hidden > 0` (α is `min(raw share, cap)` with a positive raw share, or 1 without listed weight);
- `α < 1` whenever `W_listed > 0` (the raw share and the cap are both below 1);
- `δ > 0` whenever `W_dir > 0`, and `δ < 1` whenever local weight is positive.

α and δ use `Ŵ_hidden` in place of the true hidden total. That only changes how samples are shared between the sets
(the shares are still fixed before the light is chosen), not the expectation.

**Σp = 1.** Given the draw, the listed lights' probabilities sum to `(1 − δ)(1 − α) · Σ_listed w_i / W_listed = (1 − δ)(1 − α)`,
the drawn hidden lights' actual probabilities to `(1 − δ) · α · (M/K) Σ_D w_i / Ŵ_hidden = (1 − δ) α`, and the directional
lights' to `δ`. Total 1. A listed light outside the pixel's range has `w = 0` and probability 0, which is right since its `f` is
0. A listed light in range but missing from the froxel (possible only if culling missed it) is lit where the reference, which
reads the froxel, has none.

**One offset per tile.** `o` comes from a hash of the thread group and the frame, not the pixel. Pixels of a wave that
share a froxel then draw the same entries, and the wave's index and `LightInfo` loads coincide. Each pixel's `o` is still
uniform and independent of its other random numbers, which is all the estimate needs; neighbouring pixels' errors are
correlated within a tile, and the denoiser removes most of that. `STOCHASTIC_DRAW_OFFSET_PER_PIXEL` (0) switches to a
per-pixel offset (less correlated error, more expensive at high light counts).

**Retired lights.** The list is read without the froxel, so a slot whose light was removed must stop matching it:
`LightManager::RemoveLight` writes the slot dark (zero intensity, range 0) under a new generation before freeing it.
Otherwise a removed light still in a list would keep being chosen, its ray would keep reaching it, and it would keep
itself listed.

## Directional lights

The sun, and any other directional light, is sampled with the local lights, as in MegaLights (section 5.5 of the study). All of
it is in selection.

**A third set.** Directional lights are in every froxel. Their contribution and weight are computed as for local lights,
and they are looked up in the reprojected visible list, but they feed a *directional* set instead of the listed or
unlisted one. They are taken from `PerFrameCB::sunLightIndex` when `numDirectionalLights` is 1, and found by a scan of the
active lights when there are several (one load per active light, paid only in scenes with more than one directional light).
Directional entries met in the list or in the froxel draw are skipped there. The set has a single reservoir that every sample
choosing it shares. Scenes have one sun almost always, so per-sample reservoirs would only cost registers; each sample's
marginal is still exact, so the estimate stays unbiased.

**The directional share δ** of all samples:

- its share of the weight, `W_dir / W_total`, capped;
- the cap is `STOCHASTIC_DIRECTIONAL_SHARE` (0.5) when the list has the light, or where reprojection failed, so the sun
  leaves local lights half the samples even though it outweighs them;
- the cap is `STOCHASTIC_HIDDEN_SHARE` (0.2) where reprojection held but the tile's list doesn't have it: the tile's rays
  did not reach the sun last frame (indoors, in shadow), and it gets only the exploration budget;
- the cap relaxes, as `max(cap, 1 − 4·W_local / W_total)`, when local lights carry little of the weight;
- with no local weight the sun takes every sample, and with no directional weight δ is 0.

The listed and unlisted local sets share the remaining `1 − δ` (α over the local weight only). A light's probability is
`δ·w/W_dir` (directional), `(1−δ)(1−α)·w/W_vis` (listed) or `(1−δ)·α·w/W_hid` (unlisted).

**Stratified set choice.** Each sample picks its set with one stratified number from `StochasticTemporalSequence` dimension 2:
`[0, δ)` directional, then listed, then unlisted. The number of samples a set gets therefore varies by at most one. With
δ = 0.5 and two samples, exactly one sample goes to the sun, so a fully sunlit pixel has no noise from the choice of light;
an independent choice per sample would flip it between 0 and 2× its value. With `STOCHASTIC_SORTED_SETS` (1) the set numbers
are taken in increasing order ("Reduced resolution"). The reservoirs' own strata are `frac(u + k/N)` rather than `(k + u)/N`:
with the set choice stratified, the sample that takes a set would otherwise always read the same strata of it (sample 1 of 2,
the upper half of the set's distribution), which is biased.

**The ray.** A direction in the light's disc (`sourceHalfAngleDegrees`, as the shadow map and path tracer use), drawn
from the same `[0,1)²` domain as a sphere's cone, so the visible list's quadrant masks apply. The disc is the same for
every pixel, which makes a tile's mask a good guide to where its penumbrae are. The ray leaves along the normal by the
usual offset and runs 100 km (`STOCHASTIC_DIRECTIONAL_RAY_LENGTH`), as the ray traced sun shadow's does. A directional light
without `shadowCaster` takes V = 1.

## Reduced resolution

The estimator samples a quarter of the pixels by default, as in MegaLights (section 7.1 of the study), and the denoiser
reconstructs every pixel. `stochasticLightingDownsample` (`SARP_STOCHASTIC_DOWNSAMPLE`; menu: **Stochastic Half-Resolution
Sampling**) is 2 by default; 1 samples every pixel and is the quality option. It rebuilds the graph. The factor reaches the
shaders as the root constant `STOCHASTIC_DOWNSAMPLE_FACTOR`, not a define, so the pipelines keep their ids
(`Stochastic.Select.PSO` and the others) and `pso.recompile` keeps working across the switch.

**Sampling pattern.** One estimator thread per 2×2 block samples one of its pixels a frame
(`StochasticDownsampledPixel`, `stochasticLightingRootConstants.h`, shared with the denoiser):

- Over each 2×2 of blocks the four chosen pixels form a four-rooks pattern, one in each row and column of the 4×4 pixels:
  blocks (0,0), (1,0), (0,1), (1,1) take offsets (1,0), (1,1), (0,0), (0,1).
- Every frame the whole pattern is flipped within the blocks by one of four offsets, in the order 0, 3, 1, 2 (consecutive
  frames diagonally opposite), xored and optionally transposed by a hash of the 4-frame cycle. Each pixel is therefore sampled
  exactly once in every cycle of four frames, and the pattern doesn't repeat from cycle to cycle.
- The per-pixel R1 sequences (`StochasticTemporalSequence`) advance by the pixel's visit (`frame >> 2`), not the frame, so
  each pixel's own sequence stays consecutive.
- Each sampled pixel draws `2 × stochasticLightingSamplesPerPixel` lights (capped at 4): 4 at the default 2, so one sample per
  full-resolution pixel. Rays drop by half and candidate work by four.

**Guiding at quarter resolution.** A thread group is still 8×8 threads, so its tile, and its visible list, covers 16×16
pixels, and there are a quarter as many lists. A group's 64 threads take 4 samples each, the 256 keys the build's
groupshared arrays are sized for (`STOCHASTIC_MAX_SAMPLE_COUNT` 4). Reprojection divides the previous pixel by the tile's 16
pixels. The surface history stays per pixel: a thread writes its whole block, the pixels it doesn't sample from
`LinearDepthMap` and `NormalRoughness`. The hidden draw's offset is per tile, 16×16 pixels.

**Sorted set numbers.** `STOCHASTIC_SORTED_SETS` (1): a pixel's N set numbers are `(j + t)/N` with `t = frac(N v)`, the same
strata as `frac(v + j/N)` in increasing order. Sample j of every lane then takes the directional set first, the listed set
next and the unlisted last, so a wave's lanes trace their sun rays together and their local rays together. The reservoirs'
own strata are still `frac(u + j/N)`, each uniform, so the estimate stays unbiased. It also removes part of a per-pixel
residual that cyclic set numbers leave, where a pixel's R1 dimensions keep a fixed offset from each other.

**Reconstruction** (temporal pass, `stochasticDenoise.hlsl`). The noisy inputs are a quarter the size, one texel per block.
The pass loads the tile's 4×4 samples plus a 2-sample border (8×8, one per thread), each demodulated by the albedo at the pixel
it was taken, with that pixel's depth and normal. For each pixel:

1. The four nearest samples, by bilinear position in the block grid, are weighted by bilinear weight × depth agreement
   (falling to 0 at 5% relative view depth, `DENOISE_RECONSTRUCT_DEPTH`) × `saturate(n·n')^8`
   (`DENOISE_RECONSTRUCT_NORMAL_POWER`).
2. Their weighted mean is the frame's value (`DENOISE_RECONSTRUCT_MEAN`). The moments (and so rectification) take one of them
   chosen at random in proportion to its weight, so they see a single sample's spread.
3. If the weights sum below 0.05 (`DENOISE_RECONSTRUCT_MIN_WEIGHT`), no sample this frame lies on the pixel's surface (thin
   geometry, an edge): the pixel keeps its history unchanged, or, without one, length 0. The spatial pass gives length-0
   histories no weight, as a pixel or as a neighbour, so such a pixel is filled from its surface's neighbours until a sample
   reaches it (at most four frames).
4. Rectification and the spike limit use the 5×5 samples around the pixel's own block, which cover 10×10 pixels.

Everything after that (blend, moments, variance, spatial filter) is as at full resolution. `DENOISE_DEBUG_PAIR` reads the
block's sample as each of its pixels' noisy value.

**The accumulation and the reference.** The reference always runs at full resolution: it is the ground truth. The estimator's
running average (`stochasticLightingAccumulate`) keeps the sampling resolution and is per pixel: each pixel averages the frames
that sampled it (a quarter of them), and pixels not sampled this frame show their own average. So the bias check compares
every pixel's own estimate with the reference pixel by pixel; it checks the estimator, not the reconstruction, which is a
filter. With the denoiser off and no accumulation, a block's sample is written to all four pixels (nearest), a raw view.

## Kernel split

The estimator is three kernels, each its own render-graph pass.

1. **Selection** (`StochasticSelectPass`, `stochasticSelect.hlsl`, `Stochastic.Select.PSO`). One thread per sampled pixel in
   8×8 groups: the surface history, reprojection, the three sets and their reservoirs, the shares, and each sample's set and
   light. Each chosen light's unshadowed contribution is evaluated once, and each sample's estimate `f · scale` (scale = the
   emitter quadrant's `1/(4 p(q))` over `N p`) is written to a *slot*. A sample that needs a ray (its light casts shadows, it
   adds something, and the surface isn't inside or against the emitter) appends one to a compacted *ray list*: per wave, slot
   by slot (`WavePrefixCountBits`, one `InterlockedAdd` per wave), so a trace wave holds the same sample of 32 neighbouring
   pixels, which with sorted set numbers is mostly one set.
2. **Trace** (`StochasticTracePass`, `stochasticTrace.hlsl`). A one-thread kernel turns the ray count into indirect dispatch
   arguments (`Stochastic.TraceArgs.PSO`), then `Stochastic.Trace.PSO` runs one thread per ray, 64 to a group. The ray's
   origin is rebuilt from the surface history (linear depth and octahedral normal) exactly as selection built it; a ray
   that reaches its light sets its slot's bit in the sampled pixel's word (`InterlockedOr`). With visibility gradients a third
   dispatch traces the matched rays (`StochasticTraceGradientCS`, `Stochastic.TraceGradient.PSO`; "Visibility gradients").
3. **Shading** (`StochasticShadePass`, `stochasticShade.hlsl`, `Stochastic.Shade.PSO`). One thread per sampled pixel in the
   same 8×8 tiles: the sum of the slots that are visible (their bit is set, or they took no ray), the running average, the
   outputs at either resolution, and the tile's visible list for next frame, built from the visible slots.

**What passes between them** (`shaders/Include/stochasticSplit.hlsli`; buffers in `StochasticLightingResources`):

| Buffer | Per | Contents | Written / read |
|---|---|---|---|
| Sample slots | sample of a sampled pixel (`pixel · N + k`) | 12 bytes: the light's index (20 bits), the quadrant its ray takes (2), used and ray flags; diffuse and specular `f · scale`, each one 9-9-9-5 shared-exponent word | selection / shading |
| Pixel words | sampled pixel | visibility bits (one per slot), the recorded gradient slot (bits 4-6), surface valid, the unshadowed bound (bits 16-29), debug probe flags | selection clears, the trace ORs, shading reads |
| Ray list | ray | 12 bytes: the full-resolution pixel and slot, the direction (octahedral, 16 bits a component), the length | selection / the trace |
| Ray counter, arguments | frame | the rays appended (cleared by an upload each frame), the count, the indirect dispatch | selection, the trace |
| Visibility gradients (denoised graph only) | cell of sampled pixels | records, raw and held gradients ("Visibility gradients"); kept across frames | selection, the trace's gradient kernel, shading, the temporal pass |

The slots, pixel words, ray list and arguments live within a frame, so the graph aliases them with other passes' memory. At
2560×1600 with reduced resolution they are about 100 MB (4 slots and up to 4 rays per sampled pixel), and in ZorahLights the
alias plan's required memory is the same with and without them. At full resolution the slots follow
`stochasticLightingSamplesPerPixel` (so a change of it rebuilds the graph, at full resolution only): about 210 MB at the default 2.

**Store, not recompute.** The slots store the estimate rather than recomputing it in shading, which would need the surface
rebuilt (canonical surface, material, OpenPBR state) and the BRDF evaluated again. The packing rounds the mantissas
stochastically with one number per slot, so the stored value's expectation is the estimate's, and its range (2^-20 to 2^20)
covers the estimator's fireflies. The rays store direction and length and rebuild the origin, which keeps an entry at 12 bytes
instead of 24.

**Duplicate rays** (`STOCHASTIC_DEDUPLICATE_RAYS`, 1). A pixel's samples can choose the same light: the directional set has one
reservoir that all its samples share, so with 4 samples and the sun at half the weight two samples always take the sun, and
with few lights the listed set repeats too. Samples of the same light belong to the same set, so they have the same
probability and the same emitter distribution: one ray serves all of them, its slot taking the sum of their `1/(N p)`. Each
sample's expectation is unchanged, so the estimate stays unbiased; the samples just aren't independent. The largest saving is
the sun's second ray, the longest ray in the frame (100 km through the whole TLAS). 0 traces one ray per sample.

**Registers.** Selection uses 128 registers (4 warps a scheduler), the trace kernel 96 (5 warps; latency-bound on the BVH),
shading 40 (limited by groupshared memory and the tile's barrier).

### Estimator switches

The `#ifndef`-guarded defines can be changed with `pso.recompile` on `Stochastic.Select.PSO` (and, where noted, on
`Stochastic.Shade.PSO`, which builds the lists):

| Define | Default | Effect |
|---|---|---|
| `STOCHASTIC_DIRECTIONAL_SHARE` | 0.5 | The directional set's cap ("Directional lights"). |
| `STOCHASTIC_HIDDEN_SHARE` | 0.2 | The hidden set's cap, and the sun's where the list lacks it. |
| `STOCHASTIC_HIDDEN_SHARE_UNGUIDED` | 0.5 | The hidden cap's floor where reprojection failed. |
| `STOCHASTIC_SHADOW_RAY_FLAGS` | first hit, back faces culled | In `stochasticLighting.hlsli`. |
| `STOCHASTIC_CANDIDATE_COUNT` | 24 | K, the hidden set's drawn entries; 0 takes every entry of the run. |
| `STOCHASTIC_CHEAP_IMPORTANCE` | 1 | 0 weighs candidates by the full contribution. |
| `STOCHASTIC_VISIBLE_LIST_CAPACITY` | 32 | At most `STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY` (64); `stochasticLightingRootConstants.h`. Also on `Stochastic.Shade.PSO`. |
| `STOCHASTIC_GUIDING` | 1 | 0 compiles guiding out, as `stochasticLightingGuiding` off does, for A/B within a session. Also on `Stochastic.Shade.PSO`. |
| `STOCHASTIC_SORTED_SETS` | 1 | 0 takes the set numbers in cyclic order. |
| `STOCHASTIC_DRAW_OFFSET_PER_PIXEL` | 0 | 1 draws the hidden candidates with a per-pixel offset instead of a per-tile one. |
| `STOCHASTIC_DEDUPLICATE_RAYS` | 1 | 0 traces one ray per sample. |

Don't put partial unroll hints (`[unroll(n)]`) on the candidate loops without checking the image: DXC leaves the loop rolled
with an `llvm.loop.unroll.count` hint, and the driver's unrolled loop computed a wrong (darker) result.

### Diagnostics

`STOCHASTIC_DEBUG_PROBE` (pso.recompile, with `stochasticLightingDenoise` off; read back with `capture.hdr` targets
`stochastic_diffuse` / `stochastic_specular`). Each probe runs in the kernel that has its values, and the kernel that writes it
marks the pixel so shading leaves it alone: 2 and 4 on `Stochastic.Select.PSO`; 5 on `Stochastic.Select.PSO` and
`Stochastic.Shade.PSO` (selection passes the reprojected list's length in the pixel word); 1 and 3 on `Stochastic.Select.PSO`
and `Stochastic.Trace.PSO` (selection gives each sampled pixel with a surface one ray entry and no shadow rays, and the trace
kernel traces the probe's ray and writes its value). The runner's `--pso` takes the list. The probes write at the sampled
pixels, so with reduced resolution each value covers its 2×2 block.

| Value | Diffuse | Specular |
|---|---|---|
| 1 | A camera ray with every candidate accepted: hit, candidates seen, distance | The same ray through `CLodRtTrace`: hit, distance, the TLAS descriptor |
| 2 | The pixel's directional share, unlisted share, 1 where reprojection held | 0 |
| 3 | A ray toward the sun's centre, no culling: hit, distance, the hit material's flags | N·L |
| 4 | The froxel's opaque light count, the local candidates evaluated (listed and drawn), and (brute force over every active light) the lights in range; in range / count is the culling's tightness, and a count below the lights in range means a dropped light. Works with `pso.recompile` (the pass already binds `Light::ActiveLightIndices`); a graph rebuild (any setting that rebuilds it) drops the override, so recompile after one | 0 |
| 5 | Guiding's lists: the reprojected list's length (-1 where reprojection failed), this tile's unique visible lights this frame before the capacity cut (on the first wave's pixels, -1 on the others; take the maximum over each tile: 8×8 pixels, 16×16 with reduced resolution), and the capacity | 0 |

After any change to the ray tracing scene build, run the ray traced primary visibility comparison (`SARP_RT_PRIMARY_COMPARE=1`;
a healthy BVH logs `match=` for nearly every pixel and `rayMissed=` in single digits). An empty BVH leaves the estimator
"unbiased" against a reference that traces the same BVH, and raster images don't show it.

## Denoiser

`shaders/Stochastic/stochasticDenoise.hlsl` (`StochasticDenoisePass`). It runs after the estimator unless
`stochasticLightingDenoise` is off (`SARP_STOCHASTIC_DENOISE=0`; menu: **Stochastic Denoiser**; it rebuilds the graph). It
never runs with the reference or the accumulation. The estimator then writes its raw output to two noisy textures, and
the denoiser writes `StochasticLighting::Diffuse` and `::Specular`.

**Pass order** (`StochasticLightingExtension.cpp`, every pass before `DeferredShadingPass`): `StochasticGuidePass` (with the
denoiser only; it dispatches only while `stochasticLightingNormalize` is on), `StochasticSelectPass`, `StochasticTracePass`,
`StochasticShadePass`, `StochasticDenoiseTemporalPass`, `StochasticDenoiseSpatialPass`. The reference replaces the three
estimator passes with `StochasticReferencePass` and has no guide or denoiser.

The design follows from one property of the input. At 2 samples per pixel, the estimator's radiance is unbiased but
heavy-tailed: about half of all pixels are black in any one frame, and rare samples are thousands of times the mean. Anything
that compares a pixel with its neighbours, clamps it, or picks a filter width from its own value trims that tail, and trimming
the tail darkens. Each step below avoids that.

What the user sees is DLSS's output. DLSS is a long temporal average on a still view: it removes fast (frame-to-frame) noise
from any input, and what reaches the screen is the input's slow noise, the part that holds for tens of frames. A temporal
average ahead of DLSS does not lower slow noise (an exponential average has a gain of 1 at frequency 0), so the denoiser avoids
creating it: restarts of patches of history, and random choices whose error persists, are slow noise; an always-on small
spatial filter lowers it.

### Guide-normalized accumulation

The estimator's raw radiance carries near-singular samples (a light skimming a surface, `1/d²` with a 1 cm floor), and a
history of raw radiance keeps a departed light's spot for tens of frames. So by default (`stochasticLightingNormalize`) the
denoiser accumulates *what the unshadowed light does not predict*:

1. **The guide** (`StochasticGuidePass`, `shaders/Stochastic/stochasticGuide.hlsl`, `Stochastic.Guide.PSO`): per pixel, full
   resolution, 8×8 groups, a deterministic prediction G of the unshadowed light in the denoiser's demodulated domain, diffuse and
   specular luminance (`R32G32_Float`, `Builtin::StochasticLighting::Guide`): r = lum_d(Σ unshadowed diffuse /
   max(BaseColorOpacity.rgb, 0.02)), g likewise for specular with `SpecularAo`, lum_d the denoiser's weights (0.25, 0.5, 0.25)
   (`StochasticDemodulatedLuminance`, `STOCHASTIC_DEMODULATION_FLOOR`). Where the pixel's froxel holds at most
   `STOCHASTIC_GUIDE_SMALL_FROXEL` (32) lights it is exact: every light of the froxel through the estimator's own contribution
   function, no rays. In larger froxels the far-field guide (below) keeps its cost from growing with the light count.
2. **Shading** divides each lobe by G at the pixel the sample was taken at (`STOCHASTIC_FLAG_NORMALIZE`), so the noisy inputs hold
   (radiance / albedo) / G: near the visible fraction of the unshadowed light, and nowhere near fp16's limit. A lobe with
   G ≤ 1e-7 is 0.
3. **Ratios of sums.** The temporal pass's 2×2 reconstruction weights each sample by its own pixel's G
   (`DENOISE_NORMALIZE_GUIDE_WEIGHTS`), and the temporal blend weights each frame by its guide Ĝ against the history's average
   guide Γ (`DENOISE_NORMALIZE_TEMPORAL_WEIGHTS`): the frame's blend weight is αĜ / ((1 − α)Γ + αĜ) with α = 1/length. Γ is kept
   as log2 in the change history's b, a and held to at most `DENOISE_NORMALIZE_GUIDE_RANGE` × Ĝ (4). So the history is
   Σ a S / Σ a Ĝ. A mean of ratios multiplied by one pixel's guide would be biased wherever the ratio varies with the guide at
   pixel scale (normal maps: a crevice whose shading normal faces the light has a large guide and is shadowed).
4. **Output** = the spatially filtered ratio × G(output pixel) × albedo: the only place light is multiplied back, so the output
   follows this frame's unshadowed light (a light that moves takes its spot with it) and the history only holds visibility.
5. **Light-mixture confidence** (`stochasticLightingMixtureConfidence`, below): the history is shortened where the lights the
   unshadowed light comes from have changed.
6. **Visibility gradients** (`stochasticLightingVisibilityGradient`, below): the history is shortened where the same light's
   visibility changed.

On the `!sourceValid` path the history (and Γ) is carried unchanged and the output still follows the current guide. The debug
views and `DENOISE_DEBUG_PAIR` turn the noisy input back into radiance with the guide at its sampled pixel and remodulate the
temporal-only value with the output pixel's guide, so they read radiance in both paths. The reference, the accumulation and the
undenoised path build no guide pass and bind no guide.

The rest of this section (demodulation, rectification, the spike limit, the blend, the spatial filter) applies to the ratio in
the normalized path and to demodulated radiance in the radiance path.

**The guide's per-light term** (`STOCHASTIC_GUIDE_CHEAP_BRDF` 2): GGX D and correlated Smith shared by both specular lobes, each
with its Schlick Fresnel and the dielectric's energy compensation, and Lambert diffuse with both halves of its energy
compensation. It leaves out coat, fuzz, glint, EON diffuse roughness and the per-light metal table, so with
`STOCHASTIC_GUIDE_MID_FALLBACK` 1 a pixel whose material has any of those takes the full BRDF instead (the choice made per pixel,
outside the light loop). The guide is therefore exact everywhere. The BRDF's view terms are built once per pixel
(`OpenPBRBaseLayerViewTerms`, `OpenPBRSurfaceBrdfFrame`).

**The far-field guide** (`STOCHASTIC_GUIDE_FAR_FIELD`, 1). In froxels of more than `STOCHASTIC_GUIDE_SMALL_FROXEL` lights:

- Each 8×8 group collects its pixels' **slots**, a froxel and a depth bin (`STOCHASTIC_GUIDE_DEPTH_BINS`, 5 per octave, so a group
  spanning a leaf and the ground behind it, one froxel, gets a box per layer), with each slot's box of surface points (centre c,
  half-diagonal R) and mean shading normal N0, reduced per wave before the groupshared atomics. With more than 8 distinct slots in
  a group every pixel of it takes the exact walk (which keys win a slot is a race; joining one would make the result depend on
  it). A group with no pixel in a large froxel skips the far field's clearing, walks and barriers.
- The group walks each slot's lights, 64 at a time, once for the share test's total (slots of at most
  `STOCHASTIC_GUIDE_SHARE_MAX_LIGHTS`, 64, lights only) and once to split them. A light is **exact** with weight
  n = max(near, share): near is 1 within `STOCHASTIC_GUIDE_NEAR_RATIO` (2) × R of c, fading to 0 at `STOCHASTIC_GUIDE_FAR_RATIO`
  (4) × R (its 1/d² varies across the box); share is 1 where its incident radiance at c is at least `STOCHASTIC_GUIDE_SHARE` (1/8)
  of the slot's point and spot lights' total, fading to 0 at half that. Directional lights are always exact. Exact lights go into
  a bitmask over the run and every pixel evaluates them through the full BRDF, in the run's order.
- The rest, weighted 1 − n, are summed into one **virtual light**, G = Σ h_i e_i L_i (e_i the incident radiance, L_i the
  direction, h_i = smoothstep(−0.1, 0.1, N0 · L_i) the box's horizon), expanded to first order in position: G and its Jacobian
  (the falloff, window included, differentiated). E(p, N) = Σ e_i max(0, N · L_i) ≈ max(0, N · G(p)) is exact while N and N0 see
  the same lights. Each pixel shades it through the full BRDF with its own normal (diffuse; plus a 0.05 |G| / π floor independent
  of the normal, so a pixel whose normal faces away from G but not from every far light keeps a guide). Its specular is smooth:
  max(N0 · G, 0.05 |G|) / π, no structure of its own, so the ratio of sums blurs far lights' highlights rather than imprinting
  one false highlight.
- The signature in this mode measures each light by its incident radiance (no normal), per pixel for exact lights and per slot
  for the far ones.

The far-field guide is not E[X]: with `stochasticLightingPaired` on, the guide pass gets `STOCHASTIC_FLAG_PAIRED` and every group
takes the exact walk.

**Light-mixture confidence** (`stochasticLightingMixtureConfidence`, flag `STOCHASTIC_FLAG_MIXTURE`). A ratio to the unshadowed
light cannot see a change in *which* lights it comes from: where a visible light gives way to a hidden one at the same unshadowed
level, G doesn't move and the history keeps the first light's visibility.

- *Signature* (the guide pass, texture `signature`, R32G32_UInt): each light's share of its incident radiance ×
  (0.5 + 0.5 N·L) (`STOCHASTIC_GUIDE_SIGNATURE_LOBES` 2 with the exact guide; the far-field guide uses the incident radiance alone),
  summed into 8 buckets of a byte each (share × 255; buckets 0-3 in x, 4-7 in y). The bucket is `(lightIndex + 5 · generation) & 7`
  (`StochasticSignatureBucket`): fixed for a light, so a light whose share moves moves its bucket's share continuously; there is no
  ranking that could flip between two near-equal lights, and nothing random. A reused light slot (new generation) moves to another
  bucket. The wrapped cosine keeps shares from swinging with the normal map near grazing light. With `STOCHASTIC_SIGNATURE_LOWRES`
  (1) the guide pass averages its pixels' shares into each 4×4 block (integer sums in groupshared memory, deterministic) and the
  signature and mixture-history textures are quarter size (the C++ sizes them from this macro, so 0 needs the header edited and
  the demo rebuilt).
- *Mixture history* (`mixtureHistory[2]`, ping-ponged): the mixture the history was made under, kept like Γ: reprojected and
  blended with the frame's guide-weighted blend weight, stored as bytes with stochastic rounding. Comparing with the history's
  mixture rather than last frame's signature catches slow fades, which move the mixture a little each frame.
- *Per block*: the thread at each block's top-left pixel reprojects the history's block mixture from the nearest block (through the
  motion at the block's centre; renormalized, so an empty block dilutes nothing), compares, and broadcasts the block's confidence
  to its 16 pixels through four groupshared words (`DENOISE_MIXTURE_SHARED`); its dependent loads are issued before the region load
  (`DENOISE_MIXTURE_EARLY`), and it writes the block's mixture after its blend (`DENOISE_MIXTURE_KEEP`: keeping its reprojected
  texel).
- *Confidence*: m = 1 − Σ_b min(h_b, s_b) / max(Σh, Σs) (total variation, `StochasticMixtureOverlap`; 1 where either holds no
  light), c = saturate(1 − 8 (m − 0.05)) (`DENOISE_MIXTURE_GAIN`, `DENOISE_MIXTURE_TOLERANCE`). Both lobes' history lengths are
  multiplied by c after rectification, before the blend. A steady mismatch m holds the history near 1 / (1 − c) frames, and the
  spatial pass widens for the short history as for any other.

### Visibility gradients

`stochasticLightingVisibilityGradient`. A ratio to the guide cannot see a light whose share stays put while its visibility changes,
and mixture confidence cannot either. The gradients look at visibility itself: a sample of last frame's is traced again, matched
(the same light, the same point of its emitter's sampling domain, the same origin), so its visibility differs between the frames
only if the light or an occluder moved. Noise in the estimate cannot make it fire.

1. **Records** (selection). Per *cell* of `STOCHASTIC_GRADIENT_CELL`² sampled pixels (8: a selection tile, 16×16 pixels at reduced
   resolution), one pixel, chosen by a hash of the cell and the frame so that every pixel takes its turn, records its traced
   sample whose light has the largest share of its unshadowed light (share = luminance of the light's unshadowed contribution over
   the candidates' summed cheap importances; it only ranks and weights). The record is 8 words: light index (20 bits), generation
   (8), slot (2), then shading's *complete* and *visible* bits; the emitter point u (two floats, after the quadrant remap, exactly
   what the ray used); the ray's origin (three floats, already offset off the surface); the share. The pixel word's bits 4-6 carry
   the recorded slot + 1. Only traced slots are eligible, so with deduplication the slot is the first of its light.
2. **Visibility then** (shading). The recorder's thread reads its pixel word's bit for that slot and sets the record's complete
   and visible bits.
3. **Visibility now** (the trace pass's third dispatch, `StochasticTraceGradientCS`). One thread per cell reads last frame's
   record; if the light still has the recorded generation and casts shadows, it builds the ray from the stored origin toward the
   light *where it is now* with the stored u (`StochasticLightShadowRayFrom`). The change (V_now − V_then) × share goes to the cell
   where the origin projects this frame: the largest |change| (`InterlockedMax` on the float's bits), the signed sum (fixed point,
   1/4096) and the count of records re-traced there. It runs only with `STOCHASTIC_FLAG_GRADIENT_VALID` (the records are last
   frame's) and writes nothing the estimate reads.
4. **Held gradient** (shading). Each cell's first pixel combines the 3×3 cells' raw values. `STOCHASTIC_GRADIENT_SIGNED` 1:
   g = |Σ signed| / Σ count, the share-weighted fraction of re-traced samples whose visibility changed *in one direction* (a shadow
   edge sweeping over adds up, churn cancels). 0: the largest |change| in the 3×3. Then held = max(g, last frame's held ×
   `STOCHASTIC_GRADIENT_DECAY` (0.5)), so a detection lasts 2-3 frames.
5. **Confidence** (temporal pass). c_v = saturate(1 − `DENOISE_GRADIENT_GAIN` (4) × max(held − `DENOISE_GRADIENT_TOLERANCE` (0.05), 0))
   for the pixel's cell; both lengths are multiplied by c × c_v.

**Buffer** (`StochasticLightingResources::gradient`, uints, denoised graph only, not aliased): per cell of capacity (cells of 4
sampled pixels, the finest supported, so a `pso.recompile` of the cell size needs no rebuild), two record sections by frame parity
(8 words), the raw gradient (3 words) and two held gradients by parity: 21 words a cell, 5.4 MB at 2560×1600
(`STOCHASTIC_GRADIENT_WORDS_PER_CELL`, `StochasticGradientCapacity` and the section offsets in `stochasticLightingRootConstants.h`).
Root slot 21 in the estimator's kernels and the temporal pass (`STOCHASTIC_GRADIENT_UAV`, `STOCHASTIC_DENOISE_GRADIENT_UAV`).
Selection clears each cell's raw words; the trace scatters into them; shading reads them. Flags `STOCHASTIC_FLAG_GRADIENT` (256)
and `STOCHASTIC_FLAG_GRADIENT_VALID` (512). The buffer is reached through its root slot, never by a Builtin name, and the shaders
touch it only under the flag, which only the denoised graph sets. It costs one extra ray per 64 sampled pixels.

### The paired ratio

`stochasticLightingPaired` (with normalize; off by default). Selection stores each slot's *unshadowed* estimate, and shading sums
only the visible slots: Y. The sum over every used slot, X, is an estimate of the unshadowed light drawn with the same samples, at
no cost in rays or storage. With G = E[X], G · D[Y] / D[X] (D the same reconstruction, reprojection, length, blend and spatial
weights for both) cancels the selection noise that Y and X share and leaves the visibility's. It is a ratio estimator: consistent,
not unbiased for a finite number of samples, so it reads dark where histories are short (behind fast lights).

- Shading (`STOCHASTIC_FLAG_PAIRED`) writes x_d = lum_d(X_d / albedo) / G_d into the noisy diffuse alpha and x_s likewise into the
  noisy specular alpha (E[x] = 1). It reads `BaseColorOpacity` and `SpecularAo` for this only in the denoiser's graph, where
  `Stochastic.Shade.PSO` is compiled with `STOCHASTIC_SHADE_PAIRED` 1; elsewhere the define is 0 and the shader names neither.
- The temporal pass carries H_X = Σ a X / Σ a Ĝ in the change history's r, g, with exactly the numerator's weights, so
  H_Y / H_X = Σ a Y / Σ a X. Rectification, the spike limit, moments and the variance still look at the numerator.
- The spatial pass reads H_X at the same taps with each lobe's own tap weights, and outputs G · albedo · H_Y / D with
  D = lerp(1, H_X, saturate(H_X / `DENOISE_PAIRED_FLOOR`) · saturate(length / `DENOISE_PAIRED_MIN_LENGTH`)): where the samples have
  found little of the guide's light, it falls back to the normalized output. `DENOISE_PAIRED_MIN_SAMPLES` > 0 takes the
  denominator's weight from the spatial filter's effective sample count instead.
- It needs the change history free: `DENOISE_CHANGE_DETECT`, `DENOISE_BOUND_CHANGE` or `DENOISE_RECONSTRUCT_MOMENTS` compile pairing
  out (`DENOISE_PAIRED_AVAILABLE`). It needs the exact guide, which it forces.

Channels per path:

| Texture | Radiance path | Normalized | Paired |
|---|---|---|---|
| noisy diffuse rgb / a | radiance / bound | radiance / G / bound | radiance / G / x_d |
| noisy specular rgb / a | radiance / 1 | radiance / G / 1 | radiance / G / x_s |
| history diffuse, specular rgb / a | demodulated radiance / length | Σ a S / Σ a Ĝ / length | same |
| moments | compressed luminance | of the ratio | same (numerator) |
| change history r, g / b, a | 0 / 0 (unless a change-history switch) | 0 / log2 Γ | H_X diffuse, specular / log2 Γ |

### The radiance path

With `stochasticLightingNormalize` off the denoiser filters demodulated radiance, and the **unshadowed bound** (`DENOISE_BOUND`)
limits the history. Selection sums its candidates' unshadowed contributions in linear terms (listed and directional lights in full,
drawn hidden lights scaled by M / drawCount): the pixel's unshadowed light this frame, an upper bound on what can reach it. It
travels in bits 16-29 of the pixel word (log2(1 + B) in 1/512 steps, rounded up), and shading writes it into the noisy diffuse
alpha. The temporal pass scales the reprojected history down wherever its remodulated luminance exceeds `DENOISE_BOUND_MARGIN` (2)
times the largest bound in the 5×5 neighbourhood, so light that has left a pixel can't stay there, and light that is still there is
never held below its unshadowed level. The normalized path doesn't need it (`DENOISE_NORMALIZE_BOUND` 1 applies it there anyway;
`DENOISE_SKIP_UNUSED_BOUND` skips its loads otherwise).

### Common steps

With reduced resolution (the default) the noisy inputs hold one sample per 2×2 block, and the temporal pass first reconstructs
each pixel's value for the frame from the nearest samples ("Reduced resolution"); the rest is as below.

**Demodulation.** Both signals are divided by what the material contributes, so the filter blurs lighting, not texture:
diffuse by the diffuse colour (`Surface::BaseColorOpacity`), specular by F0 (`Surface::SpecularAo`). Both are floored at
0.02, and the output multiplies by the same floored values of the current frame. Specular is not divided by the split-sum
albedo `F0·A + B`: that depends on N·V, which under normal maps and the upscaler's jitter changes at a pixel from frame to
frame, so dividing by one frame's value and multiplying by another's brightens.

**Temporal pass** (8×8 groups; `DENOISE_DOWNSAMPLED`, set by the pass from the downsample, compiles it for the graph's
resolution and sizes its groupshared region for it):

1. Loads the tile plus a 2-pixel border, demodulated, into groupshared memory. For rectification it also keeps each
   pixel's compressed luminance `log2(1 + Y)`. Each pixel computes the 5×5 mean and variance of that compressed value,
   each sample weighted by its depth and normal agreement with the pixel's own surface (`DENOISE_RECEIVER_STATS` 1), so a
   neighbourhood spanning leaves and the ground behind them describes the pixel's surface.
2. Reprojects last frame's history through `Surface::Motion` with four bilinear taps. A tap counts only if the
   estimator's surface history there is within 5% of this point's depth in last frame's view. Under 5% total weight is a
   disocclusion. There is no normal test: the shading normal is normal-mapped, and under jitter it swings past any useful
   threshold at the same pixel between frames.
3. Rectifies, as in MegaLights, by shortening the history, never by moving its value. The distance is between the
   history's mean compressed luminance (kept as a moment) and the neighbourhood's, measured in standard deviations of
   that difference. The history is kept in full up to 1 σ (`DENOISE_RECTIFY_SIGMA`), and shrinks to 1 frame at 2 σ. The
   variance is the mean of the history's temporal variance and the neighbourhood's spatial variance
   (`DENOISE_RECTIFY_TEMPORAL_WEIGHT` 0.5), because each alone is lopsided with moving lights: where a light has just left, the
   history's variance is large and the neighbourhood's small, and where one has just arrived, the reverse. Clamping linear
   values into a neighbourhood box, the usual approach, darkens: most 5×5 neighbourhoods contain none of the rare bright
   samples, so their box sits below a converged history.
4. Limits spikes. The compressed test is blind to a single bright sample, because one sample barely moves a mean of
   `log2(1 + Y)`, and a fast light would leave a row of dots along its path. So the history's linear luminance may not exceed
   4× (`DENOISE_SPIKE_LIMIT`) the brightest of this frame's 5×5 samples. The limit applies only where at least 75%
   (`DENOISE_SPIKE_LIT_FRACTION`) of those samples are lit: about half of all pixels are black in any frame, and a fully black
   neighbourhood would set the limit to 0.
5. Applies the confidences (mixture c, gradients c_v) to both lengths.
6. Blends with weight 1/length (length up to 32, `DENOISE_MAX_HISTORY`, kept separately for diffuse and specular in the
   history's alpha), and blends the compressed moments the same way.
7. Writes the history in fp16 with stochastic rounding (`DENOISE_STOCHASTIC_ROUNDING`): up to the next fp16 value with
   probability equal to the fraction of the way there, since truncation in an exponential average over 32 frames loses up to
   one ulp per frame. Values are capped at 60,000 so that a demodulated firefly cannot overflow.

History: RGBA16F diffuse (rgb, length), RGBA16F specular (rgb, length), RGBA16F moments of the compressed luminance and the
RGBA16F change history, ping-ponged by frame parity. The per-sample luminance variance (from the compressed one, through the
derivative of `log2(1 + Y)`) goes to an RG16F texture for the spatial pass's luminance stop.

**Spatial pass:** one sparse kernel, 12 Poisson taps rotated per pixel and frame, run after the history is written so it
doesn't feed back. It is wide where the history is short, and never off:

- The radius is 8 px for a new history and fades to 3 px (`DENOISE_SPATIAL_MIN_RADIUS`) at about 5 frames. It depends on
  nothing else: a radius taken from the pixel's own noisy value biases (a dim pixel's relative variance looks high, which widens
  the filter and pulls in brighter neighbours). The 3 px floor lowers the slow noise DLSS would otherwise keep.
- At the floor (both signals converged) it takes 4 taps in a plus instead of the 12, rotated per pixel and frame, each tap's
  depth and normal loaded once for both signals (`DENOISE_SPATIAL_FLOOR_TAPS`; specular joins where it is rough enough to take
  the same radius).
- Specular's radius is also scaled by roughness.
- The taps' depth and normal come from this frame's surface history (`DENOISE_SPATIAL_SURFACE_HISTORY`; the spatial pass binds it
  at `STOCHASTIC_DENOISE_SURFACE_HISTORY_UAV`).
- Edge-stopping weights: relative depth (2%); normal, as `dot^32`; luminance against 4 σ of the history's own standard deviation
  (variance over length).
- It remodulates by the floored albedos (and, normalized, by the output pixel's guide).

**Random numbers.** The light selection's stratified numbers come from an R1 sequence per pixel: a hashed per-pixel offset plus
`frame · 0.618…` (`StochasticTemporalSequence`), so consecutive frames stratify each pixel's choices. It stands in for
spatiotemporal blue noise textures, which aren't vendored. The emitter point stays on PCG, because driving it from the same
sequence as the selection would correlate them and bias the estimate. The passes seed their noise with `frameNumber`, not the
frame slot `UpdateContext::frameIndex`, which repeats every few frames.

### Denoiser switches

Most constants have `#ifndef` guards, so `pso.recompile` can try other values in a running demo. Pipeline ids are
`Stochastic.DenoiseTemporal.PSO` and `Stochastic.DenoiseSpatial.PSO`.

| Define | Default | Effect |
|---|---|---|
| `DENOISE_RECTIFY` | 1 | Rectification on or off. |
| `DENOISE_RECTIFY_SIGMA` | 1.0 | Where rectification starts shortening the history, in σ. |
| `DENOISE_RECTIFY_TEMPORAL_WEIGHT` | 0.5 | The temporal variance's weight in rectification's variance (the rest is spatial). |
| `DENOISE_RECTIFY_MIN_LENGTH` | 1.0 | The shortest history rectification leaves. |
| `DENOISE_SPIKE` | 1 | Spike limit on or off. |
| `DENOISE_SPIKE_LIMIT` | 4.0 | The limit, in multiples of the 5×5 neighbourhood's brightest sample. |
| `DENOISE_SPIKE_LIT_FRACTION` | 0.75 | The fraction of lit samples a neighbourhood needs before the limit applies. |
| `DENOISE_SPATIAL` | 1 | Spatial pass on or off. |
| `DENOISE_STOCHASTIC_ROUNDING` | 1 | Stochastic rounding of the fp16 store on or off. |
| `DENOISE_DEPTH_TOLERANCE` | 0.05 | The reprojection depth test. |
| `DENOISE_MAX_HISTORY` | 32 | The history's length cap. |
| `DENOISE_RECEIVER_STATS` | 1 | Bits: the 5×5 statistics weighted by agreement with the pixel's surface; 1 mean, variance and count (rectification), 2 brightest sample and lit fraction (spike limit), 4 largest bound. Falls back to unweighted below `DENOISE_RECEIVER_MIN_WEIGHT` (2) in total; maxima take samples of weight `DENOISE_RECEIVER_PEAK_WEIGHT` (0.25) and more. 0: unweighted. |
| `DENOISE_RECONSTRUCT_DEPTH` | 0.05 | Reduced resolution: the relative depth difference at which a sample's weight for a pixel reaches 0. |
| `DENOISE_RECONSTRUCT_NORMAL_POWER` | 8 | Reduced resolution: the normal agreement's exponent. |
| `DENOISE_RECONSTRUCT_MIN_WEIGHT` | 0.05 | Reduced resolution: below this total weight no sample lies on the pixel's surface, and it keeps its history alone. |
| `DENOISE_RECONSTRUCT_MEAN` | 1 | Reduced resolution: the frame's value is the four samples' weighted mean (0: one chosen at random). |
| `DENOISE_SPATIAL_MIN_RADIUS` | 3.0 | The spatial filter's smallest radius in pixels (0: it runs on short histories only). |
| `DENOISE_SPATIAL_FLOOR_TAPS` | 1 | At that floor, a few taps in a rotated plus with shared geometry loads instead of the 12 Poisson taps. |
| `DENOISE_SPATIAL_FLOOR_TAP_COUNT` | 4 | The floor's taps: 4, the full plus, or 2, the rotated axis's two ends (a speed option). |
| `DENOISE_SPATIAL_TAP_COUNT` | 12 | The wide kernel's taps: 12, or 6 (every other Poisson tap; a speed option). |
| `DENOISE_NORMALIZE_GUIDE_WEIGHTS` | 1 | Normalized, reduced resolution: the reconstruction weights each sample's ratio by the guide where it was taken (a ratio of sums; 0: a mean of ratios, biased). |
| `DENOISE_NORMALIZE_TEMPORAL_WEIGHTS` | 1 | Normalized: the temporal average weights each frame's ratio by its guide against the history's average guide; 0 when the change history is taken by another switch. |
| `DENOISE_NORMALIZE_GUIDE_RANGE` | 4.0 | The history's average guide is held to at most this many times the frame's (0: no limit). |
| `DENOISE_NORMALIZE_GUIDE_RANGE_SPECULAR` | = the above | The same for the specular lobe alone. 0 (unlimited) is nearer the reference and less noisy with still lights, but leaves more trail behind fast lights. |
| `DENOISE_NORMALIZE_BOUND` | 0 | 1: the unshadowed bound also applies in the normalized path (against the history times the output pixel's guide). |
| `DENOISE_BOUND`, `DENOISE_BOUND_MARGIN` | 1, 2.0 | The radiance path's unshadowed bound and its margin. |
| `DENOISE_SKIP_UNUSED_BOUND` | 1 | Normalized: the 5×5 loop doesn't load the taps' bounds. |
| `DENOISE_MIXTURE_TOLERANCE`, `DENOISE_MIXTURE_GAIN` | 0.05, 8 | Light-mixture confidence: c = saturate(1 − gain · max(m − tolerance, 0)). |
| `DENOISE_MIXTURE_RADIUS` | 2 | `STOCHASTIC_SIGNATURE_LOWRES` 0 only: the tent this frame's per-pixel signatures are averaged under. |
| `DENOISE_MIXTURE_BILINEAR` | 0 | `STOCHASTIC_SIGNATURE_LOWRES`: the history's block mixture from the nearest block (0) or four bilinearly (1). |
| `DENOISE_MIXTURE_SHARED`, `DENOISE_MIXTURE_EARLY`, `DENOISE_MIXTURE_KEEP` | 1, 1, 1 | The block confidence broadcast through groupshared words (0: four `WaveActiveMin`), its writer's dependent loads issued before the region load, and the writer keeping its reprojection. All exact. |
| `DENOISE_STATS_ROW_LOOP` | 1 | The 5×5 statistics' rows as a loop, inner taps unrolled (88 registers instead of 168). Exact. |
| `DENOISE_SPATIAL_SURFACE_HISTORY` | 1 | The spatial taps' depth and normal from the surface history instead of two textures. Exact. |
| `DENOISE_DOWNSAMPLED` | set by the pass | The temporal pass compiled for reduced (1) or full (0) resolution; -1 (unset): read at run time, sized for full resolution. |
| `DENOISE_GRADIENT_TOLERANCE`, `DENOISE_GRADIENT_GAIN`, `DENOISE_GRADIENT_APPLY` | 0.05, 4, 1 | Visibility gradients: c_v = saturate(1 − gain · max(g − tolerance, 0)) multiplies both lengths with the mixture's c; APPLY 0 computes c_v (view 9) without applying it. |
| `STOCHASTIC_GRADIENT_CELL` | 8u | In `stochasticLightingRootConstants.h`: the gradients' cell in sampled pixels (8 or 4); set it on `Stochastic.Select.PSO`, `Stochastic.TraceGradient.PSO`, `Stochastic.Shade.PSO` and `Stochastic.DenoiseTemporal.PSO` together. |
| `STOCHASTIC_GRADIENT_SIGNED`, `STOCHASTIC_GRADIENT_DECAY` | 1, 0.5 | On `Stochastic.Shade.PSO`: the 3×3 cells' combination (1 the magnitude of their signed mean, 0 their largest change) and the held gradient's decay per frame. |
| `STOCHASTIC_SIGNATURE_LOWRES` | 1 | In `stochasticLightingRootConstants.h`, set on both `Stochastic.Guide.PSO` and `Stochastic.DenoiseTemporal.PSO`: the signature and mixture per 4×4 block. 0: per pixel with the tent; needs the header edited and the demo rebuilt (the C++ sizes the textures from it). |
| `DENOISE_PAIRED_FLOOR` | 0.05 | Paired: below this unshadowed history H_X the denominator slides to 1. |
| `DENOISE_PAIRED_MIN_LENGTH` | 1.0 | Paired: the history length from which the denominator has its full weight. |
| `DENOISE_PAIRED_MIN_SAMPLES` | 0 | Paired: above 0, the denominator's weight from the spatial filter's effective sample count, n_eff = (Σw)² / Σ(w² / (2 length − 1)), full from this many. |
| `DENOISE_PAIRED_SPIKE` | 0 | Paired: 1 scales the unshadowed history with the numerator's when the spike limit binds; 0 the numerator alone. |
| `DENOISE_DEBUG_PAIR` | unset | On the spatial pass: `StochasticLighting::Diffuse` holds the frame's denoised (r) and noisy (g) total luminance, so one `capture.hdr` pairs them. |
| `DENOISE_DEBUG_VIEW` | 0 | Views of the denoiser's internals, written by the spatial pass in place of its output (0 compiles them out). 1: as `DENOISE_DEBUG_PAIR`, plus the temporal pass's result before the spatial filter, remodulated (b), and the bound (a). 2: history length, diffuse (r) and specular (g). 3: the per-sample luminance variance the spatial filter gets (r, g) and the same over the history length (b, a). 4: the bound's grip, the reprojected history's remodulated luminance over `DENOISE_BOUND_MARGIN` × the 5×5 largest bound before the clamp (r; above 1 it was scaled by 1 / r) and that bound (g); set it on both denoiser pipelines. 5: the composite unchanged (::Diffuse rgb the denoised total, ::Specular rgb 0) with the noisy luminance in ::Diffuse's alpha and the temporal result in ::Specular's, for captures that include DLSS's output. 6: cross-surface history: ::Diffuse holds the denoised total luminance (r), the share of the reprojected history's weight that passed the depth test but lies off the pixel's plane (g), the temporal result (b) and the noisy luminance (a); set it on both pipelines; the composite is not valid. 7: the paired ratio's denominator: ::Diffuse the unshadowed history H_X before (r, g) and after (b, a) the spatial filter, ::Specular this frame's noisy x and the guide where it was taken (diffuse r, g; specular b, a). 8: as 1 with g the combined confidence (c × c_v). 9: as 8 with g the visibility gradients' c_v alone. |

Other switches, all off by default:

- **Change-aware rectification** (`DENOISE_CHANGE_DETECT`, with `DENOISE_CHANGE_FAST_WEIGHT`, `DENOISE_CHANGE_SIGMA`,
  `DENOISE_CHANGE_SIGMA_RESET`): the history is shortened where a fast average of the 5×5 neighbourhood's compressed mean has
  moved from a slow one that follows the history's length, judged in standard deviations of their difference. Its averages live
  in the change history, so it compiles normalization's temporal weights and pairing out.
- **`DENOISE_BOUND_CHANGE`** (with `DENOISE_BOUND_CHANGE_TOLERANCE`, 8): the radiance path keeps, in the change history, the bound
  the history stands for (a running average with the history's own weights). Mode 2 scales the history down by the bound's fall
  beyond the tolerance; mode 1 cuts the history to `DENOISE_MAX_HISTORY / r`. Takes the change history.
- **`DENOISE_RECONSTRUCT_MOMENTS`**: reduced resolution, rectification on the reconstructed (4-sample mean) signal's compressed
  moments, kept in the change history, against the 16 2×2 means of the 5×5 samples; the raw-sample moments stay the noise
  estimate. Takes the change history.
- **`DENOISE_REPROJECT_PLANE`** (with `DENOISE_PLANE_TOLERANCE`, 0.01): a reprojection tap also needs last frame's surface on this
  pixel's plane (geometric normal from this frame's depth, `DenoiseGeometricNormal`), within the tolerance of the view depth.
  View 6 shows what it would reject.

The guide's switches (`Stochastic.Guide.PSO`):

| Define | Default | Effect |
|---|---|---|
| `STOCHASTIC_GUIDE_FAR_FIELD` | 1 | The far-field guide in froxels of more than `STOCHASTIC_GUIDE_SMALL_FROXEL` lights; never while `stochasticLightingPaired` is on. 0: every light at every pixel. |
| `STOCHASTIC_GUIDE_SMALL_FROXEL` | 32 | Froxels of at most this many lights take the exact walk (below about 30 lights it is cheaper than the group's machinery). |
| `STOCHASTIC_GUIDE_NEAR_RATIO`, `STOCHASTIC_GUIDE_FAR_RATIO` | 2, 4 | A light within NEAR × the box's half-diagonal of its centre is exact, beyond FAR it is in the far field, linearly between. |
| `STOCHASTIC_GUIDE_SHARE`, `STOCHASTIC_GUIDE_SHARE_MAX_LIGHTS` | 0.125, 64 | A light carrying at least SHARE of the box's point and spot light (at its centre) is exact, fading to 0 at half that; the test (one more walk) only runs in slots of at most MAX lights. 0: off. |
| `STOCHASTIC_GUIDE_DEPTH_BINS` | 5 | Depth bins per octave that split a froxel's pixels of one group into separate boxes. |
| `STOCHASTIC_GUIDE_FAR_SPECULAR` | 0 | The far light's specular: 0 smooth (max(N0 · G, 0.05 \|G\|) / π); 1 the virtual light's through the BRDF. |
| `STOCHASTIC_GUIDE_CHEAP_BRDF` | 2 | The per-light term. 0: the full BRDF. 1: Lambert + GGX D (selection's cheap importance; biased, since Fresnel and masking vary with the normal map). 2: the middle term, exact with the fallback. |
| `STOCHASTIC_GUIDE_MID_FALLBACK` | 1 | With 2: the per-pixel fallback to the full BRDF. 0: the middle term everywhere: faster, exact only on materials without coat, fuzz, glint, metal or diffuse roughness. |
| `STOCHASTIC_GUIDE_MID_DIFFUSE`, `STOCHASTIC_GUIDE_MID_LAYERS`, `STOCHASTIC_GUIDE_MID_FAR` | 1, 0, 0 | With 2: the diffuse (0 Lambert × the view's compensation alone; 1 with the light's table; 2 full EON); the coat and fuzz per light inside the middle term; the far field's exact lights through the middle term too. |
| `STOCHASTIC_GUIDE_SIGNATURE_LOBES` | 2 | The signature's per-light measure with the exact guide. The far-field guide always uses 3, the light's incident radiance. |
| `STOCHASTIC_GUIDE_NORMAL`, `STOCHASTIC_GUIDE_SHADING_FLOOR` | 0, 0.25 | `STOCHASTIC_GUIDE_FAR_FIELD` 0 only. 1: the guide from the geometric normal (biased: depth normals are noisy at pixel scale). |
| `STOCHASTIC_GUIDE_PIXEL_BRDF`, `STOCHASTIC_GUIDE_FRAGMENT_ALBEDO`, `STOCHASTIC_GUIDE_SKIP_EMPTY_FAR` | 1, 1, 1 | Exact speed-ups: the BRDF's view terms once per pixel, the albedos from the loaded surface, no far-field machinery in groups without a large froxel. |

### Speed options

Options that trade quality for time, all off by default:

| Option | How to enable | Saves | Cost |
|---|---|---|---|
| Guide middle term without the fallback | `STOCHASTIC_GUIDE_MID_FALLBACK` 0 on `Stochastic.Guide.PSO` | 0.14-0.42 ms of the guide (16-1024 lights) | None on content without coat, fuzz, glint, metal or diffuse roughness (all of Zorah); elsewhere an inexact guide, which biases. Never with `stochasticLightingPaired`. |
| Floor's spatial taps 4 → 2 | `DENOISE_SPATIAL_FLOOR_TAP_COUNT` 2 on `Stochastic.DenoiseSpatial.PSO` | about 0.07 ms | About 2% more pixel noise after DLSS. |
| Wide spatial kernel 12 → 6 taps | `DENOISE_SPATIAL_TAP_COUNT` 6 on `Stochastic.DenoiseSpatial.PSO` | 0.13-0.17 ms | Converged pixels don't use it; more lag and error behind moving lights. |
| Share threshold 1/8 → 1/4 | `STOCHASTIC_GUIDE_SHARE` 0.25f on `Stochastic.Guide.PSO` | up to 0.55 ms at 256 lights | More slow noise where the far field is used. |
| 16 candidates | `STOCHASTIC_CANDIDATE_COUNT` 16u on `Stochastic.Select.PSO` | 0.10-0.15 ms at 256-1024 lights; nothing at 64 or fewer | None measurable at 64 lights; not measured above. |
| 1 sample per pixel | `stochasticLightingSamplesPerPixel` 1 (`SARP_STOCHASTIC_SAMPLES=1`) | 0.2-0.3 ms at 16-64 lights | A few percent more noise after DLSS (more with more lights); still unbiased. |

The guide's registers (2026-11-21, NVPerf, 2560×1600): 161 a thread at the defaults, 24% occupancy. `STOCHASTIC_GUIDE_CHEAP_BRDF`
0 (the full BRDF, as exact as 2 with its fallback) or `STOCHASTIC_GUIDE_FAR_FIELD` 0 bring it to 128 (31%). Neither is a speed
option: with the full BRDF the guide was 9% slower in spite of the occupancy (16 lights 0.93 → 1.03 ms, 256 lights 2.34 → 2.50
ms, A/B/A/B in one session), so the middle term's cheaper per-light work outweighs the register step, and the far field is a
feature. Getting the default under 128 would need the peak located (Nsight's per-line live registers): with both the middle
term's walk and the far field compiled in it is 161, with either alone 128.

## Shading convention

`shaders/Include/stochasticLighting.hlsli`. A sample of light *i* contributes

    f_i · V / p(i)

where `f_i` is the light's unshadowed contribution exactly as the deferred loop computes it
(`EvaluateDirectLightContribution`), and `V` is whether one ray reaches a point drawn uniformly from the solid angle the
light's sphere subtends (radius `sourceRadius`, at least 1 cm, the light model's falloff floor). The expected value is
`f_i` times the visible fraction of the emitter. Lit, unoccluded surfaces therefore match the virtual-shadow-map mode
exactly, and only shadows differ. As in MegaLights, emission and the BRDF are not integrated over the emitter's area; a
large sphere near a glossy surface is shaded as a point light with a soft shadow.

The shadow ray leaves the surface along its normal by `max(5 cm, 0.2% of the camera distance)`, as the ray traced sun
shadow does, because the BVH can be coarser than the rasterized surface. It stops short of the emitter by
`max(1 cm, 0.1%)` so that a light's own fixture does not hide it. It accepts the first hit, culls back faces and runs the
alpha test (`CLodRtTrace`). Lights without `shadowCaster` take `V = 1`.

## Reference

`stochasticLightingReference` (menu: **Reference**, beside the checkbox; `SARP_STOCHASTIC_REFERENCE=1`) replaces the
estimator with the reference (`StochasticSamplePass` in `StochasticLightingPassMode::Reference`, registered as
`StochasticReferencePass`; `shaders/Stochastic/stochasticReference.hlsl`). For every light in the pixel's froxel, the sun
included, it traces one ray to a fresh point on the light's sphere (or direction in its disc), and it keeps a running average per
pixel (RGBA32F, rgb the mean and a the count) while nothing changes. It restarts when the unjittered camera, the bytes
of the published light tables, or the BVH's instance count, cluster count or CLAS builds change. The light tables'
revision and the resident texture bytes are not used: both change every frame with nothing visible changing. A texture that
streams in sharper therefore does not restart it. It is the target the estimator is checked against; it converges only while
the camera and lights are still (`ZORAH_FREEZE_LIGHTS=1`). Every 300 frames the log has a line `Stochastic lighting
accumulation: N frames held, R restarts (causes 0x..)`, the causes' bits being 1 camera, 2 lights, 4 BVH instances, 8 BVH
clusters, 16 CLAS builds.

`STOCHASTIC_REFERENCE_PER_FRAME=1` outputs each frame's estimate instead of the running average (which keeps running), and
`STOCHASTIC_REFERENCE_RAY_GRID=g` traces a g × g stratified grid of rays per light: the jitter-consistent truth after DLSS
that `stochastic_scaling.sh dlss` uses. `STOCHASTIC_REFERENCE_UNSHADOWED` 2 writes the unshadowed sum as the guide is written
(no rays), the truth `stochastic_scaling.sh guide` compares the guide with.

## Test scene

`Scripting/Scenarios/ZorahLights.cs`: Zorah with 16 moving shadowed point lights of range 18, viewed from overhead.

    BasicRenderer.exe --script <path>\ZorahLights.cs

with `SARP_RT_PATH_TRACING=0` (path tracing is the default where supported) and `SARP_STOCHASTIC_LIGHTING=1`.

| Variable | Effect |
|---|---|
| `ZORAH_FREEZE_LIGHTS=1` | Lights stay at their starting points, for references and A/B. |
| `ZORAH_LIGHT_SCALE=n` | n times as many lights (16n). `0`: no point lights, the sun alone. |
| `ZORAH_LIGHT_RANGE=r` | Every point light's range, default 18. A shorter range keeps many lights from all reaching every pixel. |
| `ZORAH_LIGHT_INTENSITY=x` | Every point light's intensity times x (default 1: 250). From 256 lights the image saturates; x = 16 / lights keeps the point lights' total at 16 lights' (1/64 at 1024 lights). |
| `ZORAH_SOURCE_RADIUS=r` | Every light's source radius, for soft shadows. Default 0 (1 cm). |
| `ZORAH_KEEP_CAMERA=1` | Keep the demo's remembered camera instead of the overhead view. |
| `ZORAH_CAMERA_T=t` | The camera held at fraction t of the fly loop (`ZORAH_CAMERA_MOTION`'s path). |
| `ZORAH_LIGHT_LAYOUT=...` | Test layouts (below); their variables are in the file's header. |
| `ZORAH_EVENT_FILE=path` | `off` and `swap` start their event when the file appears, so a capture can be lined up with it. |
| `ZORAH_VIEW=ex,ey,ez,tx,ty,tz` | The camera held at that eye and target, for finding spots. |

Layouts (all but `sweep` and `off` in the paved garden court west of the central courtyard, between hedges about 2.5 m tall,
under overhead cameras):

- `sweep`: a few lights (`ZORAH_SWEEP_LIGHTS`, 4; `ZORAH_LIGHT_SCALE` has no effect) skim the ground under a held overhead camera;
  `ZORAH_LIGHT_SPEED` scales their speed. The trail test.
- `off`: the sweep, with every light cut to zero intensity at once.
- `single`: one light 1 m over open paving, out and back; nothing in view shadows it.
- `foliage`: one light moving through a hedge's face, the paving lit through the leaves.
- `swap`: two still lights, one over open paving and one hidden behind a hedge; the first fades out as the second fades in, so
  the unshadowed light stays the same while the light that arrives is invisible.
- `flight`: two still lights and a camera flying out and back at eye height along a hedge.

## Running the benchmarks

`scripts/stochastic_scaling.sh` (Git Bash) launches the deployed demo (`DEMO_DIR`, default `D:/BasicRendererDemo`) on
ZorahLights with raster rendering, stochastic lighting, frozen lights, texture mip 4 and `clodLodErrorPixels` 3, and
drives it with `StatisticalSamplingControl.exe` (`CTL`). It moves `BasicRenderer.launch.json` aside while it runs and
puts it back on exit. Deploy first: it runs what is in the demo folder. Its analysis is
`scripts/stochastic_scaling_analysis.py`, and the timing config is `config/stochastic_scaling.json`. Run files and the
profile table's rows go to `out/stochastic_scaling/`; captures to `CAP_DIR` (default `D:/stoch_cap`), deleted after the
analysis unless `--keep`. `scripts/stochastic_scaling.sh` with no arguments prints every option.

    # (a) Timings, moving camera, one session per (range, scale), a table at the end:
    scripts/stochastic_scaling.sh profile --scales "1 4 16 64 256" --ranges "18 6"
    # A/B of define overrides within each session, A B A B (fewer samples where a frame is slow):
    scripts/stochastic_scaling.sh profile --scales "16 64" --repeat 2 \
        --variant 'base={}' --variant 'full={"STOCHASTIC_CHEAP_IMPORTANCE":"0"}'
    # A/B against a shader file that defines can't express, swapped into the deployed shaders/:
    scripts/stochastic_scaling.sh profile --scales "64" --variant 'new={}' --variant 'old={}' --shader 'old=/path/old.hlsl'
    # (b) Unbiasedness, still overhead camera: reference, accumulation, reference again (each held for --converge
    #     frames, default 4000), then mean ratio, pixel |d| and 16x16 block |d| against the reference's own drift:
    scripts/stochastic_scaling.sh bias --scale 16 --range 18
    # (c) Noise: the accumulation, then --frames live undenoised frames per variant, each against the accumulation:
    scripts/stochastic_scaling.sh noise --scale 16 --variant 'base={}' --variant 'full={"STOCHASTIC_CHEAP_IMPORTANCE":"0"}'
    # (d) Look: --frames live denoised frames per variant as PNGs (rgb x / (1 + x), sRGB) in out/stochastic_scaling/look/,
    #     here with moving lights:
    scripts/stochastic_scaling.sh look --scale 4 --frames 4 --vars "ZORAH_FREEZE_LIGHTS=0"
    # (e) Noise after DLSS against a reference, held camera, DLSS on; a table per scale and a crop PNG in
    #     out/stochastic_scaling/dlss/:
    scripts/stochastic_scaling.sh dlss --scale 1 --variant 'base={}' --variant 'other={"DENOISE_SPATIAL_MIN_RADIUS":"0.0f"}'
    # (f) Noise after DLSS within one 60 s session, no reference:
    scripts/stochastic_scaling.sh dlssnoise --scale 1 --settle 3 --exit-after 60 --variant 'a={...}' --variant 'b={...}'

- Variants recompile `Stochastic.Select.PSO` and `Stochastic.Shade.PSO` (`--pso` takes another comma-separated list) with
  `pso.recompile`. The profile table's Sample column is the three split passes' sum (Select, Trace and Shade are shown too). A
  variant whose defines change the shader's Builtin resources fails to publish; the runner stops with the demo log's reason.
- `--shader 'name=PATH'` runs variant `name` with the file PATH copied over the deployed `--shader-target` (default
  `shaders/Stochastic/stochasticSelect.hlsl`); the deployed file is restored after the run. The file must reference the
  same Builtin resources. Each profile's summary is kept as `out/stochastic_scaling/<run>_<variant>.md`, so a run with
  `CONFIG` pointing at an NVPerf config (registers, instructions) keeps one per variant.
- A variant key `"setting:NAME"` sets a setting instead of a define (`settings.set`), for example
  `--variant 'd2={"setting:stochasticLightingDownsample":2}' --variant 'd1={"setting:stochasticLightingDownsample":1}'`. A
  setting that rebuilds the graph drops every `pso.recompile` override, so the runner applies the variant's defines again
  after it. A setting keeps its value until a later variant sets it: name it in every variant. The runner treats
  `stochasticLightingNormalize`, `...Paired`, `...MixtureConfidence`, `...VisibilityGradient` and `enableJitter` as per
  frame (no rebuild, overrides kept); any other setting as a rebuild.
- `pair` (moving camera, `ZORAH_CAMERA_MOTION=always`) recompiles the spatial pass with `DENOISE_DEBUG_PAIR` and captures
  `--frames` frames per variant: each frame's denoised luminance against its own noisy input. The mean ratio is the
  denoiser's bias in motion, the 32×32 block |d| its lag and blur plus the noisy block means' own noise, which it estimates
  per block (the noise floor). `noise --denoise` keeps the denoiser on: still-camera denoised error against the accumulation.
- `trail` captures `--frames` (48) consecutive frames per variant of a moving-light layout (`--layout`, default `sweep`). The noisy
  input is unbiased in every frame, so for each 8×8 block the input's truth is its mean over ±4 frames, and the analysis reports,
  relative to the input's mean: *excess* (output above that truth), *deficit* (below it), *stuck energy* (the output's share in
  blocks whose input is dark, under 2% of the mean), *stuck blocks* (dark blocks showing output above 25% of the mean), *age*
  (frames since a stuck block's input was last lit), the runs of consecutive stuck frames, the *lag* (the delay that best lines up
  the denoised series with the input's), linear, displayed (x / (1 + x)) and windowed ratios, and the *paired* per-frame
  output/input ratio and 32×32 |d|, the only reading valid with a moving camera. `--stages 3` (the default) measures the trail
  after the temporal pass, the spatial pass and DLSS from one capture (`DENOISE_DEBUG_VIEW` 5; the rest of the frame is estimated
  per block as the median of HDR minus the denoised output, valid only under a held camera); `--debug-view n` captures a debug view
  instead (6, 8, 9 have analyses). It writes an overlay PNG to `out/stochastic_scaling/trail/`, stuck blocks in red.
- `--fixed-dt 0.0166667` advances the scenario 1/60 s a frame whatever the frame time (`--script-fixed-dt`). Captures slow the demo
  to 5-15 frames a second, and the scenario's clock follows wall time, so without it lights move several times as far per captured
  frame as they do live. Compare like with like. `--exit-after` caps a session.
- `still`: frozen lights, `DENOISE_DEBUG_VIEW` 1 frames per variant, the converged reference, then the frames again in reverse
  order (their difference is the scene's drift); denoised, temporal-only and noisy means over the reference's, linear and
  displayed, and a noise proxy (block |d| against the reference per frame, and of each 16-frame mean: slow noise and bias, at 8×8
  and 32×32).
- `dlss` captures with `capture.hdr`'s `targets`, `frames` and `luminance` (TESTING.md "HDR capture"): R1 (the converged
  reference, `--ref-frames`, default 8), P1 and P2 (the per-frame reference with a `--ref-grid` × `--ref-grid` ray grid,
  default 4, `--per-frame-frames` each, default 32; DLSS's averaged output of it is the truth after DLSS, since the converged
  reference is already antialiased and DLSS reconstructs it differently), the undenoised input (`--no-noisy` skips it) and
  `--sets` (4) sets of `--frames` (8) consecutive frames per variant, `--set-gap` (1) seconds apart. Errors are weighted by the
  slope of `y / (y + Y)` at the truth and given in percent of white (`SS_DLSS_LINEAR=1` for linear), at pixels, 8×8 and 32×32,
  split into *fast* (flicker within a set), *slow* (spread of the sets' means, less fast's share) and *bias*; noise after DLSS
  is `sqrt(slow² + fast²)` after DLSS. About 1.5 GB of R32F files a variant at 2560×1600. It does not fit a 60 s session.
- `dlssnoise` measures slow and fast noise before and after DLSS from each variant's own frames, without a reference: held camera
  (`ZORAH_CAMERA_T=0` unless `--vars` says otherwise), frozen lights, DLSS on, `--sets` (4) sets of `--frames` (8) frames
  `--set-gap` (1) s apart, `--history` (3) s after the variant is applied. The display weight is taken at the mean of all variants'
  frames. It cannot measure bias (judge that with `still`), noise slower than a variant's span of sets, or moving lights.
  `--noisy` adds the undenoised input, `--floor` the per-frame reference; each is a graph rebuild. Two variants take about 45 s.
- `guide`: frozen lights and camera, frames with and without jitter, then the unshadowed reference: the guide's frame-to-frame
  stability and its ratio to the truth.
- `paired`: frozen lights and camera, view 7 and the guide: E[x] and H_X against 1.
- Guided against unguided: `--variant 'unguided={"STOCHASTIC_GUIDING":"0"}'` (put it after the guided variants: the
  first guided frames after it read stale lists).
- `CONFIG=config/stochastic_sample_nvperf.json` profiles the sample passes' hardware counters (registers, L1 hit rate,
  stalls, launch limits) instead of their time.
- `--vars "A=1 B=2"` adds environment variables (for example `SARP_STOCHASTIC_SAMPLES=1`); `--samples n` lowers the
  profile's sample count for slow scenes.
- Pass timers move ~10% between sessions. Compare within a session (`--variant`), and rerun the base row of a table
  rather than comparing against an old one. Passes on the compute queue overlap, so a pass's time includes contention with
  its neighbours.

## Declaring resources conditionally

`StochasticSamplePass::Declare` declares different resources per graph (denoised, accumulation, undenoised, reference). A view
token is valid only against the layout of the `Declare` call that made it:

1. Decide from state fixed for the graph's life (constructor arguments, `m_resources`, the mode), never from a per-frame
   setting: `Declare` runs once per graph build.
2. Every token `Record` uses unconditionally must be set on every path of `Declare`; give each branch its own `if` / `else`
   pair. `Declare` throws a `logic_error` naming the pass if either output token is empty.
3. A token used only sometimes is tested in `Record` (`if (bindings.guide.layout)`) before `recording.Index`.
4. A Builtin resource the shader names through `ResourceDescriptorIndex` must be declared in every graph that pipeline runs in.
   If a graph doesn't declare it, compile the shader for that graph without the name (a define pushed onto the pipeline
   description in `Declare`, decided as in 1, as `STOCHASTIC_SHADE_PAIRED` is), so the declaration and the define come from the
   same condition.
5. A pipeline override (`pso.recompile`) cannot change a shader's Builtin resource set, so such a define is not switchable at run
   time.

An empty token reaches `RecordingContext::View` as "Foreign or stale declared view token".

## Files

| Path | Contents |
|---|---|
| `src/Lighting/Stochastic/StochasticLightingExtension.*` | The graph extension that places the passes before `DeferredShadingPass`. |
| `src/Lighting/Stochastic/StochasticLightingResources.h` | What the passes share across frames: visible lists, surface history, the denoiser's textures, the guide, the signature and the mixture history, the visibility gradients' buffer. |
| `src/Lighting/Stochastic/RenderPasses/StochasticSamplePass.h` | The pass for the estimator's three kernels (`StochasticSampleKernel`) and the reference (`StochasticLightingPassMode`), with the running average. |
| `src/Lighting/Stochastic/RenderPasses/StochasticDenoisePass.h` | The denoiser's temporal and spatial passes (`StochasticDenoiseStage`). |
| `src/Lighting/Stochastic/RenderPasses/StochasticGuidePass.h` | The guide for guide-normalized accumulation (`shaders/Stochastic/stochasticGuide.hlsl`: exact in small froxels, the far-field guide in large ones) and the light-mixture signature. |
| `shaders/Include/stochasticLighting.hlsli` | Surface reconstruction, unshadowed light evaluation, the shadow ray (`StochasticLightShadowRay`, `StochasticLightShadowRayFrom`). |
| `shaders/PerPassRootConstants/stochasticLightingRootConstants.h` | Root constant slots (shares the secondary rays' TLAS slots), the flags, the reduced-resolution sampling pattern the estimator and denoiser share, the demodulated domain the guide and denoiser share, the surface history's octahedral normal (`StochasticPackNormal`, `StochasticUnpackNormal`), the light-mixture signature's packing, overlap and block size, and the visibility gradients' slot, flags, cell and buffer layout. |
| `shaders/Stochastic/` | The passes' shaders: `stochasticSelect.hlsl`, `stochasticTrace.hlsl` and `stochasticShade.hlsl` (the estimator's three kernels; with visibility gradients, the records, the matched rays and the held gradient), `stochasticReference.hlsl`, `stochasticDenoise.hlsl`, `stochasticGuide.hlsl`. |
| `shaders/Include/stochasticSplit.hlsli` | What the estimator's kernels pass between them: sample slots, pixel words, the ray list's entries, the shared-exponent radiance packing. |
| `scripts/stochastic_scaling.sh`, `scripts/stochastic_scaling_analysis.py` | The benchmark runner and its analysis ("Running the benchmarks"). |
| `config/stochastic_scaling.json` | The runner's sampling config: the stochastic passes, deferred shading, the light grid's three passes. |
| `config/stochastic_sample_nvperf.json` | NVPerf counters for the estimator's passes (`CONFIG=` for the runner). |
