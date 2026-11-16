# Stochastic lighting

Stochastic direct lighting shadows every analytic light, the sun included, by ray tracing instead of shadow maps, in the
way Unreal's MegaLights does: each pixel picks a few lights at random in proportion to their importance, traces one shadow ray per
pick, and a denoiser turns the result into one diffuse and one specular signal. Its cost follows the number of samples,
not the number of lights.

It is `ShadowingMode::StochasticRayTraced` ([lighting.md](lighting.md) "Shadowing modes"). The plan it is being built to,
milestone by milestone, is SARP's `~/.claude/plans/megalights-stochastic-di.md`.

## Status

| Milestone | State |
|---|---|
| M0: mode, settings, test scene, reference | Done; runs in ZorahLights. |
| M1: stochastic estimator without history | Done; its accumulated mean matches the reference (below). No denoiser yet, so the live image is noisy. |
| M2: guiding (visible-light lists, exploration) | Done; unbiased against the reference, less noise with more lights (below). Wave-intrinsic list build and importance-kept full lists (2026-11-15, "Guiding at scale"). |
| M3: denoiser (temporal plus spatial) and blue-noise sampling | Built; validation below. Retuned for what survives DLSS (2026-11-15, "Noise after DLSS"). |
| M4: scalability (reduced-resolution sampling, light grid) | Bounded candidates per pixel done (2026-11-15, "Bounded candidates"); light grid done (2026-11-15, "After the light grid"); reduced-resolution sampling done and on by default (2026-11-15, "Reduced resolution"); the estimator split into selection, trace and shading kernels with one ray per light (2026-11-15, "Kernel split"). |
| M5: screen-space traces, geometry the BVH lacks | Not started. |
| M6: the sun, forward passes, ray hits | The sun: done (below, "Directional lights"). Forward passes and ray hits: not started. |

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
  the sun included, until M6.
- Ray traced reflection hits are unaffected: they shade every light unshadowed except the sun, which gets its own ray.
- The sun's clipmap views still exist (`LightManager` makes them for shadow-casting directional lights), but nothing
  culls or renders them.

## Settings

| Setting | Environment | Default | Effect |
|---|---|---|---|
| `enableStochasticLighting` | `SARP_STOCHASTIC_LIGHTING` | off | The mode (rebuilds the pipeline). |
| `stochasticLightingSamplesPerPixel` | `SARP_STOCHASTIC_SAMPLES` | 2 | Lights drawn per pixel, 1 to 4, one ray each (a light drawn twice shares one, "Kernel split"). Per frame with reduced resolution, where each sampled pixel draws twice as many (at most 4); at full resolution it rebuilds the graph (the kernels' buffers hold the setting's samples). |
| `stochasticLightingDownsample` | `SARP_STOCHASTIC_DOWNSAMPLE` | 2 | 2: one pixel of each 2×2 sampled a frame, and the denoiser reconstructs the rest ("Reduced resolution"); 1: every pixel (rebuilds the graph). The reference is always full resolution. |
| `stochasticLightingReference` | `SARP_STOCHASTIC_REFERENCE` | off | The reference in place of the estimator (rebuilds the graph). |
| `stochasticLightingAccumulate` | `SARP_STOCHASTIC_ACCUMULATE` | off | The estimator keeps a running average while nothing changes, to compare with the reference (rebuilds the graph). |
| `stochasticLightingGuiding` | `SARP_STOCHASTIC_GUIDING` | on | Guiding by last frame's visible lights (below). Per frame. |
| `stochasticLightingDenoise` | `SARP_STOCHASTIC_DENOISE` | on | The denoiser (rebuilds the graph). |

The menu has them under Post Processing and Ray Tracing (Stochastic Lighting, Reference, Accumulate, Stochastic
Samples per Pixel, Stochastic Half-Resolution Sampling). The debug output **Stochastic Lighting** (`OUTPUT_STOCHASTIC_LIGHTING`, 77;
`SARP_DEBUG_OUTPUT_TYPE=77`) shows the stochastic lights' radiance alone (every analytic light, the sun included), as
x / (1 + x), without sky, image-based lighting or emission. Before the sun joined (2026-11-14) it showed spot and point
lights only, so measurements in the sections below that use it exclude the sun.

## Estimator

Since 2026-11-15 three kernels, each its own pass ("Kernel split", below): selection (`shaders/Stochastic/stochasticSelect.hlsl`,
`StochasticSelectPass`), the shadow rays (`stochasticTrace.hlsl`, `StochasticTracePass`) and shading with the visible-list
build (`stochasticShade.hlsl`, `StochasticShadePass`). Before that, selection, tracing and shading were one kernel,
`stochasticSample.hlsl` (`StochasticSamplePass`); the steps below are the same in both.

1. **Candidates.** The lights of the pixel's light cluster, which since 2026-11-15 is its light grid froxel's opaque run
   ([lighting.md](lighting.md) "The light grid"; directional lights are in every froxel): with guiding, the
   reprojected tile's listed lights plus at most `STOCHASTIC_CANDIDATE_COUNT` (24) drawn entries of the cluster ("Bounded
   candidates", below). For each, the unshadowed contribution `f_i` is evaluated with the real BRDF and its importance is
   `w_i = log2(1 + luminance(f_i))` (MegaLights' perceptual weight: one bright light doesn't take every sample after tone
   mapping).
2. **Selection.** One weighted reservoir per sample, all fed the same candidate stream. Each reservoir holds a single
   uniform number, kept uniform after every accept/reject by sample warping, and the N numbers are stratified,
   `u_k = (k + u) / N`, so the N picks fall in different strata of the light distribution. A reservoir keeps its light's
   `f_i` and `w_i`, so nothing is evaluated twice.
3. **Visibility and shading.** One shadow ray per pick; the pixel's value is `Σ_k f_k V_k W / (N w_k)` with `W = Σ w_i`,
   that is `f / p` with `p = w / W` exactly.

Until 2026-11-15 the candidate loop evaluated the BRDF for every light in the cluster, as the deferred loop does, and that
was the cost that grew with the light count. "Bounded candidates" (below) caps it.

### Validation (2026-11-12)

ZorahLights, overhead camera, lights frozen, 2560×1600, debug output 77, about 1000 accumulated frames each. Accumulated
estimator (2 samples per pixel) against the reference, over the upper half of the frame (the menus cover the rest):

| Measure | Value |
|---|---|
| Mean luminance ratio | 0.997 |
| Per-pixel absolute difference, mean / p95 | 0.0036 / 0.015 |
| 16×16 block mean difference, mean / p95 / max | 0.0011 / 0.0046 / 0.028 |

Pass timer (menu's Pass Timings, not the statistical profiler; RTX 5070 Laptop): estimator 4.0 ms at 2 samples per pixel,
reference 4.7 ms, both at full resolution with 16 lights.

## Guiding

M2, after MegaLights: the previous frame's visibility guides this frame's choice of lights, and the estimator stays
unbiased. Selection is in `stochasticSelect.hlsl`, the list build in `stochasticShade.hlsl` (before the kernel split, both in `stochasticSample.hlsl`). Each 8×8 thread group is one screen tile, so the tile's list is
built by the group that shaded it, with no separate pass. `stochasticLightingGuiding` (`SARP_STOCHASTIC_GUIDING=0` turns
it off; menu: **Stochastic Guiding**) switches it per frame.

**Visible lists.** For each tile there is a count, then up to `STOCHASTIC_VISIBLE_LIST_CAPACITY` entries (32; the
buffers have room for `STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY`, 64, so `pso.recompile` can try any capacity up to that).
Each entry holds:

- the light buffer index (20 bits);
- the low 8 bits of the light's `generation`, so a slot given to another light doesn't match;
- a 2×2 mask of the quadrants of the emitter's sampling domain that a ray reached.

The list is sorted by index and built from this frame's samples whose ray reached the light. It is a binary "seen
visible", as in MegaLights, not a visible fraction. Since 2026-11-15 ("Guiding at scale", below) the build is:

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
   binary-searches the list. The weight lives only in groupshared memory during the build; the entry format is unchanged.

Until then each round took the group's smallest remaining key with `InterlockedMin` in groupshared memory, four group
barriers per round, and a full list kept the 32 lowest light indices, whatever their contribution.

The lists and a per-pixel surface history are ping-ponged: two buffers of each, swapped every frame.

- The surface history is R32G32_UInt: linear depth and an octahedral normal. The renderer's own linear-depth history is
  overwritten before shading, so this pass keeps its own.
- On a pass's first frame (after a graph build), or after a frame with guiding off, the history is marked invalid.

**Reprojection.** A pixel moves by `Surface::Motion` into last frame. The reprojection holds if the surface stored
there is within 10% of the depth this point had in last frame's view (`Camera::prevView`) and within about 45° of its
normal. The pixel then picks one of the four nearest tiles at random, with bilinear weights, so a change between lists
doesn't show as a tile edge.

**Two reservoirs and the mixture.** The local lights are split in two sets:

- the reprojected list's lights (generation checked) feed the *visible* (listed) reservoirs;
- the cluster's other lights feed the *hidden* reservoirs, each set with its own stratified random numbers. Since
  2026-11-15 only a bounded draw of them is evaluated ("Bounded candidates").

A sample takes the hidden set with probability α:

- α is the hidden lights' share of the weight, capped at 0.2, so an occluded bright light cannot take every ray;
- the cap rises toward 1 as the listed lights' share falls below a quarter, since the list is likely stale;
- the cap is at least 0.5 where reprojection failed;
- α is 1 with no listed weight and 0 with no hidden weight.

A listed light's probability is exactly `(1 − α)·w/W_visible`, and a hidden light's `α·w/W_hidden`, because the two sets
are disjoint (with the bounded draw, `W_hidden` is an estimate and the hidden probability a RIS contribution weight). The light's
contribution is evaluated again for the chosen sample, so the reservoirs hold only (u, light, weight, mask).

**Quadrant masks.** A listed light's emitter point is drawn by first choosing a quadrant of its sampling domain:

- weight 1 for each quadrant the list marks as reached, 0.15 for each that it doesn't;
- the point is drawn uniformly within the chosen quadrant;
- the estimate is scaled by `1 / (4 p(quadrant))`, so it stays unbiased;
- unreached quadrants keep their floor, so one that becomes visible is still found.

Unlisted lights have all four quadrants, which is uniform sampling.

The domain is the light's cone around the direction from the surface to the light. Pixels in a tile see nearly the same
cone, which is what makes a tile's mask useful at its other pixels.

A 64-bit filter is built from the reprojected list once per pixel. A drawn cluster light whose bit is clear skips the
binary search that would find it in the list.

### Validation (2026-11-13)

Measured as in M1: debug output 77, lights frozen, upper half of the frame, against the reference. "Live" is a single
frame, with no accumulation and no denoiser.

| Run | Mean ratio | Per-pixel abs. diff, mean | 16×16 block diff, mean / p95 |
|---|---|---|---|
| 16 lights, guided, accumulated | 0.9975 | 0.0040 | 0.0012 / 0.0040 |
| 16 lights, live, guided / unguided | | 0.0239 / 0.0254 | 0.016 / 0.016 |
| 64 lights (`ZORAH_LIGHT_SCALE=4`), accumulated, guided / unguided | 0.990 / 0.986 | 0.0117 / 0.0132 | 0.0041 / 0.0056 |
| 64 lights, live, guided / unguided | | 0.048 / 0.059 | 0.035 / 0.046 |

- With 64 lights, guiding cuts single-frame error by about a fifth. With 16 lights the gain is small: two samples
  already cover most of a pixel's lights.
- The accumulated runs at 64 lights read slightly dark against the reference, the unguided one more so. This is
  residual noise passed through the debug view's concave x / (1 + x), which pulls a noisy mean down, not a bias in
  guiding: guided against unguided reads 1.004.
- Lights moving: no tile-edge blocks are visible.

Cost by pass timer, 2 samples per pixel:

| Lights | Unguided | Guided |
|---|---|---|
| 16 | 4.2 ms | 5.2 ms |
| 64 | 5.9 ms | 7.6 ms |

An experiment at 64 lights (lists read but ignored) put about 0.7 ms of that in fixed work:

- the surface history;
- the list build (up to 32 rounds of four group barriers);
- the second set of reservoirs;
- evaluating the chosen lights again.

The remaining ~0.9 ms comes with actually using the lists. The filter saved only 0.15 ms, so the search is not what
costs. A guided sample more often picks a light it can see, and that ray runs all the way to the light instead of
stopping at an early hit, which likely accounts for the rest. M4 (sampling at reduced resolution, evaluating the visible
list plus a subset of the cluster) is where the per-pixel cost comes down.

### Bounded candidates (2026-11-15)

Before this, the candidate loop walked every light of the pixel's cluster, M of them, so the sample pass cost about
0.12 ms per light walked (39 ms at 1024 lights). Now a pixel evaluates at most `|list| + K` local candidates, K =
`STOCHASTIC_CANDIDATE_COUNT` (24), plus its directional lights. All of it is in selection (`stochasticSelect.hlsl`).

**The three sets.**

- *Listed*: every entry of the reprojected tile's visible list (at most 32), read from the list itself rather than
  found in the cluster. Generation and quadrant masks as before. A listed light that doesn't reach this pixel gets
  weight 0 from the range check. Directional entries are skipped (they are the directional set's).
- *Directional*: unchanged ("Directional lights"), taken from `sunLightIndex` instead of the cluster.
- *Hidden*: K of the cluster's M entries, drawn by a stratified stride: `s = M/K` (real), one offset `o` uniform in
  `[0, s)`, entries `floor(o + j·s)` for `j = 0 … K−1`. Drawn entries that are listed (filter, binary search,
  generation) or directional are skipped. With `M ≤ K` the draw takes every entry, and the cluster is walked once and split
  into listed and hidden lights exactly as before; the list loop is then skipped.

**Why each entry is drawn with probability K/M exactly.** With `s ≥ 1`, entry `i` is drawn when some
`o + j·s ∈ [i, i+1)`. Those intervals of `o` are spaced `s` apart and 1 long, so exactly a length-1 part of `[0, s)` draws
`i`, and the `j` it needs lies in `0 … K−1` for every `i < M`. So `P(i drawn) = 1/s = K/M`, whatever `i`.

**The estimator.** Let `D` be the drawn hidden lights. The reservoirs pick within `D` by `w` alone; every drawn light
carries the same factor `M/K`, so weighting each by `w·M/K` would pick the same way. The hidden total in every probability
is the estimate

    Ŵ_hidden = (M/K) · Σ_{i∈D} w_i

and a hidden pick's probability in the estimate is `(1 − δ)·α·w_i / Ŵ_hidden`. Its actual probability, given `D`, is
`(1 − δ)·α·(w_i·M/K) / Ŵ_hidden`, so the estimate `f_i / p` has expectation, given `D`, of `(M/K) · Σ_{i∈D} f_i`: the
draw's estimate of the hidden lights' sum (`Ŵ_hidden / w_i` is the pick's RIS contribution weight). Over draws its expectation
is `Σ_hidden f_i · P(i drawn) · M/K = Σ_hidden f_i`, because each hidden light is drawn with probability K/M. The
listed and directional parts are exact as before, so the whole estimate is unbiased, provided every set with positive
weight can be chosen:

- `α > 0` whenever `Ŵ_hidden > 0` (α is `min(raw share, cap)` with a positive raw share, or 1 without listed weight);
- `α < 1` whenever `W_listed > 0` (the raw share and the cap are both below 1);
- `δ > 0` whenever `W_dir > 0`, and `δ < 1` whenever local weight is positive (unchanged).

α and δ use `Ŵ_hidden` in place of the true hidden total. That only changes how samples are shared between the sets
(the shares are still fixed before the light is chosen), not the expectation.

**Σp = 1 for the listed part.** Given the draw, the listed lights' probabilities sum to
`(1 − δ)(1 − α) · Σ_listed w_i / W_listed = (1 − δ)(1 − α)`, the drawn hidden lights' actual probabilities to
`(1 − δ) · α · (M/K) Σ_D w_i / Ŵ_hidden = (1 − δ) α`, and the directional lights' to `δ`. Total 1. A listed light outside
the pixel's range has `w = 0` and probability 0, which is right since its `f` is 0. (A listed light in range but missing
from the cluster, possible only when culling overflows, is now lit where the reference, which reads the cluster, has none.)

**One offset per tile.** `o` comes from a hash of the thread group and the frame, not the pixel. Pixels of a wave that
share a cluster then draw the same entries, and the wave's page and `LightInfo` loads coincide; per-pixel offsets cost
3 ms more at 1024 lights (16.2 against 13.2 ms). Each pixel's `o` is still uniform and independent of its other random numbers, which is all
the estimate needs; neighbouring pixels' errors are correlated within a tile.

**Reading the cluster.** Until 2026-11-15 clusters were linked pages of 12 lights, and the draw walked them once, in order.
Contiguous pages inside that culling scheme saved 0.65 ms of sampling at 1024 lights but cost 0.45 ms of culling in every
mode, so they were dropped then. The light grid now stores each froxel as one contiguous run, so entry `i` of the draw is
read directly: `GridIndices[first + floor(o + j·s)]`. The estimator is otherwise unchanged.

**Retired lights.** The list is now read without the cluster, so a slot whose light was removed must stop matching it:
`LightManager::RemoveLight` writes the slot dark (zero intensity, range 0) under a new generation before freeing it.
Without that, a removed light still in a list would keep being chosen, its ray would keep reaching it, and it would keep
itself listed.

#### Validation (2026-11-15)

ZorahLights, range 18, 2 samples per pixel, frozen lights; `scripts/stochastic_scaling.sh`. "Before" is the pre-Phase-1
walk (every cluster light, full-contribution importance), run in the same session as a `--shader` variant.

**Unbiased** (`bias`, about 4,200 frames each unless noted). 1024 lights was not checked: the debug view saturates there,
so the comparison means nothing.

| Lights | Variant | Mean ratio | Pixel abs. diff | 16×16 block diff, mean / p95 | Reference drift, block mean / p95 |
|---|---|---|---|---|---|
| 16 | default | 1.0001 | 0.0069 | 0.0008 / 0.0029 | 0.0006 / 0.0014 |
| 64 | default | 0.9999 | 0.0187 | 0.0018 / 0.0055 | 0.0007 / 0.0007 |
| 256 | default | 1.0007 | 0.0252 | 0.0033 / 0.0068 | 0.0005 / 0.0005 |
| 256 | default, 12,300 frames | 1.0008 | 0.0246 | 0.0032 / 0.0072 | 0.0010 / 0.0003 |
| 256 | listed lights limited to the cluster (test only) | 1.0001 | 0.0253 | 0.0027 / 0.0071 | 0.0007 / 0.0004 |
| 256 | `STOCHASTIC_CANDIDATE_COUNT=0` (unbounded) | 0.9998 | 0.0228 | 0.0022 / 0.0064 | 0.0006 / 0.0005 |
| 256 | unbounded, 12,300 frames | 0.9995 | 0.0225 | 0.0024 / 0.0064 | 0.0001 / 0.0001 |

- The mean is within 0.1% everywhere. The +0.07% at 256 lights is listed lights that reach the pixel but that culling
  didn't put in its cluster (the reference reads the cluster): restricting the listed set to the cluster removes it.
- At 64 and 256 lights the block difference stays above the reference's drift, and **it doesn't shrink with 3× the frames,
  for the unbounded estimator too** (0.0022 at 4,200 frames, 0.0024 at 12,300). So a per-pixel residual that doesn't
  average away predates bounding; bounding raises it from 0.0024 to 0.0032. Its cause is open. One candidate is the
  R1 sequence (`StochasticTemporalSequence`): its dimensions advance by the same step every frame, so at a pixel the
  reservoir numbers and the set choice keep a fixed offset from each other and never fill the square jointly. Taking the set
  numbers in sorted order (`STOCHASTIC_SORTED_SETS`, "Reduced resolution") halved it at 64 lights (0.0018 to 0.0010).

**Noise** (`noise`, 16 live undenoised frames per variant against the accumulation, one session per row):

| Lights | Variant | Pixel error | 16×16 block error |
|---|---|---|---|
| 256 | before (unbounded, full importance) | 0.406 | 0.042 |
| 256 | unbounded, cheap importance | 0.417 | 0.043 |
| 256 | K = 32 | 0.432 | 0.057 |
| 256 | **K = 24 (default)** | **0.436 (+7.5%)** | **0.059 (+40%)** |
| 256 | K = 24, per-pixel offset | 0.438 | 0.047 |
| 256 | K = 16 | 0.443 | 0.068 |
| 1024 | before (unbounded, full importance) | 0.498 | 0.043 |
| 1024 | unbounded, cheap importance | 0.513 | 0.045 |
| 1024 | K = 32 | 0.532 | 0.073 |
| 1024 | **K = 24 (default)** | **0.534 (+7.2%)** | **0.077 (+78%)** |
| 1024 | K = 24, per-pixel offset | 0.559 | 0.053 |
| 1024 | K = 16 | 0.541 | 0.086 |

The pixel error rises by 7% against the unbounded estimator (3% of it from cheap importance). The block error rises much
more, because one offset per tile makes the tile's pixels draw the same candidates: their errors are correlated, and a
block no longer averages them away. A per-pixel offset keeps the block error near the unbounded one but costs 3 ms at 1024
lights (timings below). How much of the correlated error the denoiser removes hasn't been measured.

**Cheap importance.** Noise against the full-contribution importance, before bounding: at 16 lights +1.4% (5 samples of
12 or 24 frames each: 0.309 against 0.313; the 16-light number moves ±4% between samples), at 256 lights +2.9% (three
samples of 24 frames, 0.404 against 0.416, each within 0.5%). Both are within the 5% allowed, so it is on by default. With
bounded candidates it saves 9% of the sample pass at 1024 lights (13.7 against 15.2 ms, three A/B pairs). At 16 lights
the sample pass reads 4% slower (6.33 against 6.08 ms), but the denoiser and deferred passes that overlap it read faster by
as much, and their sum is the same (8.44 against 8.43 ms). The stand-in wasn't changed. A variant that used it for the hidden draw only, with the full contribution for listed and
directional lights, was slower at 16 lights (6.93 ms) and in between at 1024.

**Time** (`profile`, moving camera, one session per row, all variants in that session; ms):

| Range | Lights | Before | K = 24 (default) | K = 16 | K = 32 | K = 24, per-pixel offset | K = 24, full importance |
|---|---|---|---|---|---|---|---|
| 18 | 16 | 6.33 | 6.37 | 6.38 | 6.61 | 6.38 | 6.08 |
| 18 | 256 | 14.99 | 11.19 | 10.17 | 12.01 | 12.35 | 11.82 |
| 18 | 1024 | 42.87 | 13.53 | 12.34 | 14.72 | 16.06 | 13.30 |
| 18 | 4096 | 93.74 | 16.04 | 14.84 | 17.91 | 20.23 | 17.75 |
| 6 | 16 | 4.89 | 5.03 | | | | |
| 6 | 1024 | 9.72 | 8.23 | | | | |
| 6 | 4096 | 26.83 | 10.13 | | | | |

- Single profiles move by a few percent within a session, and at 16 lights time shifts between the sample pass and the
  passes overlapping it (see "Cheap importance"). The 1024-light default measured 13.1–13.8 ms across sessions.
- **The 12 ms target at 1024 lights is not met** with K = 24 (13.5); K = 16 gives 12.3. The pass is latency-bound
  (NVPerf: SM throughput 23%, 168 registers, 24% occupancy as before). What remains at 1024 lights, by stubbing: the list
  loop about 2 ms (32 listed lights), the draw about 6 ms (of which the page walk 0.65 and the listed-light search 0.85),
  the rest the fixed work measured at 16 lights.
- At 4096 lights and range 18 culling still drops (cluster, light) pairs (Dropped column of the runner); the times are
  for the lights the clusters kept.
- K = 24 is the default as asked: K = 16 is 1.2 ms faster at 1024 lights for +1–1.5% pixel error and +15% block error; K
  = 32 is 1.2 ms slower for −1% pixel error.

**What didn't work.**

- One loop over listed and drawn candidates together, branching on the phase per lane, cost 32 ms at 256 lights against
  11 ms for two loops: lanes of a wave in different phases ran both bodies.
- Per-pixel draw offsets: 16.2 ms at 1024 lights; per 2×2 quad 15.4, per 4×4 block 14.2, per tile 13.2.
- Contiguous cluster pages (entry i read directly, no walk): -0.65 ms of sampling, +0.45 ms of light culling in every mode.
- `[unroll(2)]` on the draw loop: 7.9 ms, but the image was 18% dark (mean ratio 0.815 in `noise`). DXC leaves the loop
  rolled with an `llvm.loop.unroll.count` hint, and the driver's unrolled loop is wrong. Don't use partial unroll hints on
  these loops without checking the image.
- Gating reservoir updates past the sample count, and leaving listed lights' records unloaded in the draw: within noise.

### Guiding at scale (2026-11-15)

The list build ("Visible lists", above) now uses wave intrinsics, and a full list keeps its most important lights instead
of its lowest light indices. All of it is in the list build (`stochasticShade.hlsl` since the kernel split); the entry format is unchanged.

**Shared memory.** The build's groupshared arrays hold one packed uint per entry (light 20 bits, quadrants 4, the weight as
an 8-bit log2 code in steps of 2^(1/4)): 2 KB per group. A first version kept the weight as a float beside each key, 5 KB
per group, and the pass was 0.2–1.2 ms *slower* than the barrier build it replaced: NVPerf showed the same 168 registers
but an L1 hit rate of 73% against 80%, since the driver gives a kernel's shared memory its share of the L1. Packed, the hit
rate is 78% and the pass is faster than before (below).

**Unbiased by construction.** Only which lights are listed changes. The list stays unique (each light once, its
quadrants ORed) and sorted by index, so the listed set is still found exactly by the binary search that the hidden draw's
skip of listed entries relies on ("Bounded candidates"): a drawn light is skipped exactly when the listed loop evaluated
it. The α and δ conditions don't depend on which lights are listed.

**Measured.** ZorahLights, range 18, 2 samples per pixel. "Before" is the Phase 2 shader (barrier build, 32 lowest
indices), run as a `--shader` variant in the same sessions.

*Time* (`profile`, moving camera, A B A B in one session per row, mean of the two; ms):

| Lights | Before | Capacity 24 | **32 (default)** | 48 |
|---|---|---|---|---|
| 16 | 6.85 | 6.24 | **6.23** | 6.49 |
| 256 | 10.93 | 9.61 | **9.62** | 9.63 |
| 1024 | 12.58 | 11.76 | **11.28** | 12.02 |
| 4096 | 13.43 | 11.75 | **12.31** | 12.71 |

The build is 0.6 ms cheaper at 16 lights, where lists are short (4 lights on average), and 1.1–1.3 ms at 256 and more. The
1024-light capacity-24 and -48 means include one slow profile each (12.39, 12.67; their other profiles were 11.12 and
11.36). With the unpacked build, capacities 48 and 64 cost 0.3–0.5 ms more than 32 at 1024 and 4096 lights (the list
loop runs per pixel over more entries).

*List lengths* (probe 5, overhead still camera, frozen lights): the tile's unique visible lights average 4.2 / 9 / 20 / 30
at 64 / 256 / 1024 / 4096 lights (p95 8 / 16 / 35 / 45, max 13 / 24 / 49 / 65). A 32-entry list overflowed in no tile at
64 or 256 lights, in 7–9% of tiles at 1024 and 32–41% at 4096. Tiles see few unique lights because a tile has only 128
samples, half of them on the sun, and many lights it draws are occluded.

*Noise* (`noise`, 16 live undenoised frames per variant against the accumulation, one session per row; the point lights'
intensity scaled with `ZORAH_LIGHT_INTENSITY` so the point lights' total stays that of 64 lights):

| Lights | Unguided (`STOCHASTIC_GUIDING=0`) | Capacity 24 | **32** | 48 | 64 |
|---|---|---|---|---|---|
| 64 | 0.353 (block 0.033) | | **0.332** (0.034) | 0.330 | |
| 1024 (intensity 1/16) | 0.711 (block 0.326) | 0.455 | **0.455** (0.060) | 0.455 | |
| 4096 (intensity 1/64) | | 0.600 | **0.587** (0.096) | 0.584 | 0.585 |

- Guided against unguided pixel error: 0.94 at 64 lights (−6%) and 0.64 at 1024 lights (−36%). At 64 lights the gain is
  smaller than M2's −19% ("Validation (2026-11-13)"): the sun now takes half the samples, and a pixel's cluster holds
  fewer than K = 24 lights, so unguided pixels already evaluate their whole cluster. At 1024 lights unguided pixels only
  see K = 24 drawn entries with one offset per tile, hence its large block error.
- Capacity barely matters for noise: 24 is 2.3% worse at 4096 lights, and 48 or 64 at most 0.6% better. 32 stays the
  default: 48 costs 0.4 ms at 4096 lights for 0.6%, and 24 saves 0.55 ms there for +2.3%.

*Unbiased* (`bias`, 4,195 frames each): 16 lights mean ratio 0.9999, block |d| 0.0009 / p95 0.0031 against a drift of
0.0007 / 0.0014; 64 lights 0.9999, 0.0018 / 0.0058 against 0.0005 / 0.0007. Both match Phase 2 (the residual at 64 lights
predates Phase 1, "Bounded candidates").

*Tiling.* The tiling seen at 64 lights on 2026-11-13 ("Validation (2026-11-13)" of the denoiser) is not there now, with
the old build or the new: denoised moving-light frames at 64 lights (`look --vars ZORAH_FREEZE_LIGHTS=0`), and undenoised
ones, show no 8×8 or 32×32 structure, and the mean luminance step across tile edges is that inside tiles (ratio 0.97–1.03,
before and after alike). It was not list overflow: at 64 lights no tile's list holds more than 13 lights. It was most likely
the culling bug found the same day (clusters of exactly 12 or 24 lights went black in rectangles), fixed then.

## Reduced resolution

Since 2026-11-15 the estimator samples a quarter of the pixels by default, as in MegaLights (section 7.1 of the study), and the
denoiser reconstructs every pixel. `stochasticLightingDownsample` (`SARP_STOCHASTIC_DOWNSAMPLE`; menu: **Stochastic
Half-Resolution Sampling**) is 2 by default; 1 samples every pixel, as before, and is the quality option. It rebuilds the
graph. The factor reaches the shaders as the root constant `STOCHASTIC_DOWNSAMPLE_FACTOR`, not a define, so the pipelines
keep their ids (`Stochastic.Select.PSO` and the others) and `pso.recompile` keeps working across the switch.

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

**Guiding at quarter resolution.** A thread group is still 8×8 threads, so its tile, and its visible list, now covers 16×16
pixels, and there are a quarter as many lists. A group's 64 threads take 4 samples each, the 256 keys the build's
groupshared arrays were already sized for (`STOCHASTIC_MAX_SAMPLE_COUNT` 4). Reprojection is unchanged except that the
previous pixel is divided by the tile's 16 pixels. The surface history stays per pixel: a thread writes its whole block,
the pixels it doesn't sample from `LinearDepthMap` and `NormalRoughness` (three extra loads of each). The hidden draw's offset
is still per tile, now 16×16 pixels.

**Sorted set numbers.** `STOCHASTIC_SORTED_SETS` (1): a pixel's N set numbers are `(j + t)/N` with `t = frac(N v)`, the same
strata as `frac(v + j/N)` in increasing order. Sample j of every lane then takes the directional set first, the listed set
next and the unlisted last, so a wave's lanes trace their sun rays together and their local rays together. The reservoirs'
own strata are still `frac(u + j/N)`, each uniform, so the estimate stays unbiased ("Directional lights" explains why that
matters). It saved 0.24 ms at 16 lights and 0.37 at 1024 (downsample 2, moving camera, A B A B: 3.43 against 3.66 ms, 5.64 against 6.01), and it lowered the residual at 64 lights (Validation, below).

**Reconstruction** (temporal pass, `stochasticDenoise.hlsl`). The noisy inputs are a quarter the size, one texel per block.
The pass loads the tile's 4×4 samples plus a 2-sample border (8×8, one per thread), each demodulated by the albedo at the pixel
it was taken, with that pixel's depth and normal. For each pixel:

1. The four nearest samples, by bilinear position in the block grid, are weighted by bilinear weight × depth agreement
   (falling to 0 at 5% relative view depth, `DENOISE_RECONSTRUCT_DEPTH`) × `saturate(n·n')^8`
   (`DENOISE_RECONSTRUCT_NORMAL_POWER`).
2. Their weighted mean is the frame's value (`DENOISE_RECONSTRUCT_MEAN`, since 2026-11-15). The moments (and so
   rectification) still take one of them chosen at random in proportion to its weight, so they see a single sample's
   spread. Until 2026-11-15 the chosen sample was the value too; the mean leaves less noise after DLSS ("Noise after DLSS").
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
every pixel's own estimate with the reference pixel by pixel, like with like; it checks the estimator, not the
reconstruction, which is a filter. With the denoiser off and no accumulation, a block's sample is written to all four
pixels (nearest), a raw view.

### Validation (2026-11-15)

ZorahLights, range 18, `stochasticLightingSamplesPerPixel` 2 (4 per sampled pixel at downsample 2); `scripts/stochastic_scaling.sh`.

**Unbiased** (`bias`; downsample 2 held 8,095 frames, about 2,000 per pixel, and downsample 1 4,195):

| Lights | Downsample | Mean ratio | Pixel abs. diff | 16×16 block diff, mean / p95 | Reference drift, block mean / p95 |
|---|---|---|---|---|---|
| 16 | 2 | 1.0000 | 0.0072 | 0.0009 / 0.0026 | 0.0006 / 0.0011 |
| 64 | 2 | 1.0000 | 0.0073 | 0.0008 / 0.0026 | 0.0010 / 0.0005 |
| 16 | 1 | 1.0000 | 0.0043 | 0.0007 / 0.0017 | 0.0006 / 0.0013 |
| 64 | 1 | 0.9999 | 0.0090 | 0.0010 / 0.0029 | 0.0006 / 0.0007 |
| 64 | 1, `STOCHASTIC_SORTED_SETS=0` (cyclic, as before) | 0.9998 | 0.0193 | 0.0018 / 0.0058 | 0.0004 / 0.0007 |

The 64-light residual at full resolution ("Bounded candidates": block 0.0018 / p95 0.0055 to 0.0059 in Phases 1 to 3) is
0.0010 / 0.0029 with sorted set numbers, still above the drift. The same session with cyclic set numbers gives the old 0.0018 / 0.0058, so the sorted order is what removed about half of it, which supports the suspicion in "Bounded candidates" that the residual comes from the fixed offsets between a pixel's R1 dimensions. What is left is still open. The 64-light drift at downsample 2 has one
outlier block (max 7.3) that lifts its mean; its p95 is 0.0005.

**Time** (`profile`, moving camera, one session per row, A B A B; ms, mean of the two):

| Lights | Sample, downsample 2 | Sample, downsample 1 | Temporal + spatial, 2 | Temporal + spatial, 1 |
|---|---|---|---|---|
| 16 | 3.50 | 6.48 (6.06, 6.90) | 1.02 + 0.73 | 1.07 + 0.64 |
| 256 | 4.86 | 10.10 (10.91, 9.28) | 1.13 + 0.82 | 1.19 + 0.78 |
| 1024 | 5.78 | 12.07 (13.10, 11.03) | 1.15 + 0.85 | 1.19 + 0.83 |
| 4096 | 6.26 | 12.29 | 1.14 + 0.93 | 1.20 + 0.88 |

- Downsample 2 halves the sample pass (−46% at 16 lights, −49 to −52% from 256). The downsample-1 profiles move by up to
  2 ms between repeats in these sessions; downsample 2 repeats agree within 0.1 ms.
- **The 3 ms target at 16 lights is not met (3.5 ms).** The 6 ms target at 1024 lights is (5.78). With rays stubbed (V = 1,
  the trace kept but never taken), the pass costs 0.97 ms at downsample 2 and 2.06 at 1 (16 lights): rays are 2.6 of the 3.5
  ms, for half as many rays as at full resolution (4.1 ms). A ray costs about 25% more at reduced resolution: a wave's
  sampled pixels are spread over twice the area, and each thread traces 4 rays in a row.
- NVPerf (16 lights): 168 registers a thread at both factors, the same as with 2 samples (the reservoirs were already
  unrolled for 4); launch stalled on register allocation 30%; SM throughput 13% at downsample 2 against 16% at 1; stalls
  mostly long scoreboard. Latency-bound at 24% occupancy, which is Phase 5's subject.
- The denoiser costs about the same at both factors: the temporal pass is 0.05 ms cheaper (a quarter of the samples to load),
  the spatial pass 0.02–0.09 ms dearer (more short histories, so wider kernels).

**Image** (`look`, `pair`, `noise --denoise`):

- *Still camera* (`noise --denoise`, denoised frames against the accumulation): 16 lights, pixel error 0.157 at downsample 2
  against 0.108 at 1 (+45%; half the samples per pixel), 16×16 block error 0.030 against 0.031. 1024 lights (intensity
  1/16): pixel 0.127 against 0.117 (+8%), block 0.029 against 0.027.
- *Moving camera* (`pair`, `ZORAH_CAMERA_MOTION=always`, 8 frames per variant): each frame's denoised luminance against its
  own noisy input, 32×32 blocks. 16 lights: mean ratio 0.989 / 0.986 (two runs) at downsample 2, 0.982 at 1; block error
  0.055 / 0.052 against 0.112. 1024 lights: 0.991 / 0.992 against 0.998, block error 0.053 / 0.056 against 0.092. The noise
  floor of the noisy block means is about 0.04 at 16 lights for both and 0.038 / 0.030 at 1024. That downsample 2 reads
  *lower* in motion is partly because the camera flies by time and downsample 2 renders faster, so it moves less per frame;
  the comparison isn't at equal motion per frame. Either way, reduced resolution adds no lag in motion that these
  measurements can see.
- *Thin geometry* (held camera, `ZORAH_CAMERA_T=0.3`, 16 lights, crops in `out/stochastic_scaling/look/p4_crop_*`): the
  balustrades, wrought-iron gates, statues and lamp posts look the same at both factors; flat stone shows slightly more
  low-frequency mottling at downsample 2. No black or stale pixels on thin geometry.
- *Disocclusions* (moving camera, 16 and 1024 lights): both factors show the same ghosting of roofs and walls dragged across
  the courtyard during the fast loop, so it is the temporal pass's existing behaviour, not the reconstruction. At newly
  revealed surfaces downsample 1 shows sparkly noise and downsample 2 smoother blotches (the spatial pass's 8-pixel kernel
  over a quarter of the samples).

### Per-tile against per-pixel draw offsets, denoised (2026-11-15)

Phase 1 left open whether the hidden draw's offset should be per tile (the default; 3 ms cheaper at 1024 lights at full
resolution) or per pixel (+40% / +78% undenoised 16×16 block error at 256 / 1024 lights with per tile). With the denoiser,
1024 lights at intensity 1/16:

| | Downsample 2, per tile | Downsample 2, per pixel | Downsample 1, per tile | Downsample 1, per pixel |
|---|---|---|---|---|
| Still, pixel error | 0.127 | 0.126 | 0.117 | 0.112 |
| Still, 16×16 block error | 0.029 | 0.026 (−12%) | 0.027 | 0.024 (−10%) |
| Moving, 32×32 block error (`pair`) | 0.053 / 0.056 | 0.047 | 0.092 | 0.093 |
| Sample pass, moving camera | 5.77 ms | 6.35 ms (+0.58) | | |
| Sample pass, 16 lights | 3.49 ms | 3.54 ms (+0.05) | | |

The denoiser removes most of the per-tile penalty: what is left is 10–12% of still-camera block error, a visibly slightly
blotchier flat surface in a held view (`p4_crop_offset_*_tile_vs_pixel.png`), and within noise in motion. Per pixel would
put the 1024-light sample pass over 6 ms. The default stays per tile; `STOCHASTIC_DRAW_OFFSET_PER_PIXEL=1` switches it.

## Kernel split

Since 2026-11-15 the estimator is three kernels, each its own render-graph pass, instead of one (`stochasticSample.hlsl`,
which held selection, the rays, shading and the list build). The estimator itself is unchanged: selection is the single
kernel's code up to the choice of each sample's light. What changed is where the rays are traced and what is stored between.

1. **Selection** (`StochasticSelectPass`, `stochasticSelect.hlsl`, `Stochastic.Select.PSO`). One thread per sampled pixel in
   8×8 groups, as before: the surface history, reprojection, the three sets and their reservoirs, the shares, and each
   sample's set and light. Each chosen light's unshadowed contribution is evaluated once, and each sample's estimate
   `f · scale` (scale = the emitter quadrant's `1/(4 p(q))` over `N p`) is written to a *slot*. A sample that needs a ray
   (its light casts shadows, it adds something, and the surface isn't inside or against the emitter) appends one to a
   compacted *ray list*: per wave, slot by slot (`WavePrefixCountBits`, one `InterlockedAdd` per wave), so a trace wave
   holds the same sample of 32 neighbouring pixels, which with sorted set numbers is mostly one set.
2. **Trace** (`StochasticTracePass`, `stochasticTrace.hlsl`). A one-thread kernel turns the ray count into indirect dispatch
   arguments (`Stochastic.TraceArgs.PSO`), then `Stochastic.Trace.PSO` runs one thread per ray, 64 to a group. The ray's
   origin is rebuilt from the surface history (linear depth and octahedral normal) exactly as selection built it; a ray
   that reaches its light sets its slot's bit in the sampled pixel's word (`InterlockedOr`).
3. **Shading** (`StochasticShadePass`, `stochasticShade.hlsl`, `Stochastic.Shade.PSO`). One thread per sampled pixel in the
   same 8×8 tiles: the sum of the slots that are visible (their bit is set, or they took no ray), the running average, the
   outputs at either resolution, and the tile's visible list for next frame, built from the visible slots exactly as the
   single kernel built it from its samples.

**What passes between them** (`shaders/Include/stochasticSplit.hlsli`; buffers in `StochasticLightingResources`):

| Buffer | Per | Contents | Written / read |
|---|---|---|---|
| Sample slots | sample of a sampled pixel (`pixel · N + k`) | 12 bytes: the light's index (20 bits), the quadrant its ray takes (2), used and ray flags; diffuse and specular `f · scale`, each one 9-9-9-5 shared-exponent word | selection / shading |
| Pixel words | sampled pixel | visibility bits (one per slot), surface valid, debug probe flags | selection clears, the trace ORs, shading reads |
| Ray list | ray | 12 bytes: the full-resolution pixel and slot, the direction (octahedral, 16 bits a component), the length | selection / the trace |
| Ray counter, arguments | frame | the rays appended (cleared by an upload each frame), the count, the indirect dispatch | selection, the trace |

The slots, pixel words, ray list and arguments live within a frame, so the graph aliases them with other passes' memory. At
2560×1600 with reduced resolution they are about 100 MB (4 slots and up to 4 rays per sampled pixel), but the alias plan's
required memory is the same with and without them (893 MB in ZorahLights; 98 MB more if they stood alone), so they cost no
VRAM there. At full resolution the slots follow `stochasticLightingSamplesPerPixel` (so a change of it rebuilds the graph,
at full resolution only): about 210 MB at the default 2.

**Store or recompute.** The slots store the estimate rather than recomputing it in shading, which would need the surface
rebuilt (canonical surface, material, OpenPBR state: as much as selection's own surface load) and the BRDF evaluated again.
Storing costs 8 bytes a slot. The packing rounds the mantissas stochastically with one number per slot, so the stored
value's expectation is the estimate's, and its range (2^-20 to 2^20) covers the estimator's fireflies; the single kernel
summed in fp32 and wrote fp16 noisy inputs, so nothing that reaches the denoiser is less precise than before. The rays
store direction and length and rebuild the origin, which keeps an entry at 12 bytes instead of 24.

**Duplicate rays** (`STOCHASTIC_DEDUPLICATE_RAYS`). A pixel's samples can choose the same light: the directional set has one
reservoir that all its samples share, so with 4 samples and the sun at half the weight two samples always take the sun, and
with few lights the listed set repeats too. Samples of the same light belong to the same set, so they have the same
probability and the same emitter distribution: one ray serves all of them, its slot taking the sum of their `1/(N p)`. Each
sample's expectation is unchanged, so the estimate stays unbiased; the samples just aren't independent any more.

Measured (moving camera, A B A B in one session, ms; noise as in "Bounded candidates", 16 frames per variant):

| | Time, 16 lights | Time, 1024 lights | Undenoised pixel error, 16 / 64 lights | Denoised pixel error, 16 / 64 lights |
|---|---|---|---|---|
| One ray per sample (`STOCHASTIC_DEDUPLICATE_RAYS=0`) | 2.95 (trace 1.83) | 4.21 (trace 2.01) | 0.332 / 0.299 | 0.161 / 0.137 |
| **One ray per light (default)** | **1.69 (trace 0.74)** | **3.52 (trace 1.40)** | 0.366 (+10%) / 0.304 (+2%) | 0.168 (+4%) / 0.138 (+1%) |
| Only lights sampled uniformly (all four quadrants) | 1.87 (trace 0.89) | 3.62 | 0.360 / 0.305 | |

Most of the saving is the sun's second ray: it is the longest ray (100 km through the whole TLAS), and every sunlit pixel
had two of them. The noise is where visibility varies over the emitter (the sun's penumbrae, soft local shadows) and where a
listed light's quadrants are weighted (a shared quadrant draw shares its `1/(4 p(q))`, which two draws would average);
deduplicating only uniformly sampled lights kept most of the noise and a sixth of the time, so it isn't an option. With the
denoiser the cost is 4% of pixel error at 16 lights and 1% at 64, for 1.3 ms (43% of the estimator at 16 lights), so
deduplication is on.

**Registers and occupancy** (NVPerf, 16 lights; 1024 lights the same):

| Kernel | Registers | Warps a scheduler | Time, 16 / 1024 lights | SM throughput |
|---|---|---|---|---|
| Single kernel (before) | 168 | 3 (24%) | 2.86 / 5.06 ms | 14% |
| Selection | 128 | 4 (33%) | 0.78 / 1.89 ms | 36% / 48% |
| Trace | 96 | 5 (42%) | 0.74 / 1.40 ms | 5% |
| Shading | 40 | limited by groupshared memory and the tile's barrier | 0.15 / 0.24 ms | 25% / 57% |

Selection misses the 96 target. Its plateau is the candidate loops: rebuilding the surface after them, so the full OpenPBR
state isn't live across the loops, brought it only to 120 (the same step), and moving the slot loop's evaluation and ray
append into one pass per slot (as now) left it at 128. Getting under 96 would mean serializing the candidate loops' loads or
shrinking the eight reservoirs, which the workflow's experience says costs latency. The trace kernel sits on the 96 step and
is latency-bound (SM throughput 5%, long-scoreboard stalls): it waits on the BVH.

**Ray cost.** Phase 4 measured each ray at about 25% more than at full resolution (fewer, less coherent rays per wave). Deduplication removed the largest single cost, the sun's second ray. The rest of the investigation was not written up: the phase was stopped during its final checks. The trace kernel is latency-bound on the BVH (above), so the remaining options are binning or sorting rays by direction and a sun-only trace; neither has been measured.

After the split (main session, moving camera, 2026-11-15): selection 0.85 / 2.07 ms, trace 0.88 / 1.60, shading 0.16 / 0.28 at 16 / 1024 lights, 1.89 / 3.95 ms in all. A short bias check at 16 lights: mean ratio 0.9999, block 0.0015 against drift 0.0006.

## Directional lights

Since 2026-11-14 the sun, and any other directional light, is sampled with the local lights, as in MegaLights (section
5.5 of the study), and no shadow map runs in this mode. All of it is in selection (`stochasticSelect.hlsl`).

**A third set.** Directional lights are in every cluster. Their contribution and weight are computed as for local lights,
and they are looked up in the reprojected visible list, but they feed a *directional* set instead of the listed or
unlisted one. Since the cluster is no longer walked in full ("Bounded candidates"), they are taken from
`PerFrameCB::sunLightIndex` when `numDirectionalLights` is 1, and found by a scan of the active lights when there are
several (a cost of one load per active light, paid only in scenes with more than one directional light). Directional
entries met in the list or in the cluster draw are skipped there. The set has a single reservoir that every sample choosing it shares. Scenes have one sun almost always, so
per-sample reservoirs would only cost registers; each sample's marginal is still exact, so the estimate stays unbiased.

**The directional share δ** of all samples:

- its share of the weight, `W_dir / W_total`, capped;
- the cap is `STOCHASTIC_DIRECTIONAL_SHARE` (0.5) when the list has the light, or where reprojection failed, so the sun
  leaves local lights half the samples even though it outweighs them;
- the cap is `STOCHASTIC_HIDDEN_SHARE` (0.2) where reprojection held but the tile's list doesn't have it: the tile's rays
  did not reach the sun last frame (indoors, in shadow), and it gets only the exploration budget;
- the cap relaxes, as `max(cap, 1 − 4·W_local / W_total)`, when local lights carry little of the weight;
- with no local weight the sun takes every sample, and with no directional weight δ is 0.

The listed and unlisted local sets share the remaining `1 − δ` exactly as before (α over the local weight only). A light's
probability is `δ·w/W_dir` (directional), `(1−δ)(1−α)·w/W_vis` (listed) or `(1−δ)·α·w/W_hid` (unlisted).

**Stratified set choice.** Each sample picks its set with one stratified number, `v_k = frac(v + k/N)` from
`StochasticTemporalSequence` dimension 2: `[0, δ)` directional, then listed, then unlisted. The number of samples a set
gets therefore varies by at most one. With δ = 0.5 and two samples, exactly one sample goes to the sun, so a fully sunlit
pixel has no noise from the choice of light; an independent choice per sample would flip it between 0 and 2× its value.
Before this the listed/unlisted choice was an independent number per sample.

The reservoirs' strata changed with it, from `u_k = (k + u)/N` to `u_k = frac(u + k/N)`. Both put the N picks of a set in
different strata, but with the set choice stratified, the sample that takes a set would otherwise always read the same
strata of it (sample 1 of 2, the upper half of the set's distribution), which is biased. Each `u_k` is now uniform on
its own.

**The ray.** A direction in the light's disc (`sourceHalfAngleDegrees`, as the shadow map and path tracer use), drawn
from the same `[0,1)²` domain as a sphere's cone, so the visible list's quadrant masks apply. The disc is the same for
every pixel, which makes a tile's mask a good guide to where its penumbrae are. The ray leaves along the normal by the
usual offset and runs 100 km, as the ray traced sun shadow's does. A directional light without `shadowCaster` takes V = 1.

**What it can't shadow.** The BVH leaves out skinned draws, assemblies, displaced and voxel geometry, grass and
alpha-blended draws ([clod_raytracing.md](../../docs/clod_raytracing.md) in SARP). With no shadow map, these cast no sun
shadows in this mode, as they already cast none for local lights. M5's screen-space traces are the planned fix.

The `#ifndef`-guarded defines `STOCHASTIC_DIRECTIONAL_SHARE`, `STOCHASTIC_HIDDEN_SHARE` and
`STOCHASTIC_HIDDEN_SHARE_UNGUIDED` can be changed with `pso.recompile` (pipeline `Stochastic.Select.PSO`; `Stochastic.Sample.PSO` before the kernel split). So can `STOCHASTIC_SHADOW_RAY_FLAGS` (`stochasticLighting.hlsli`),
`STOCHASTIC_CANDIDATE_COUNT` (24; 0 takes every cluster entry), `STOCHASTIC_CHEAP_IMPORTANCE` (1; 0 weighs candidates by
the full contribution), `STOCHASTIC_VISIBLE_LIST_CAPACITY` (32, at most `STOCHASTIC_VISIBLE_LIST_MAX_CAPACITY`, 64;
`stochasticLightingRootConstants.h`), `STOCHASTIC_GUIDING` (1; 0 compiles guiding out, as `stochasticLightingGuiding`
off does, for A/B within a session), `STOCHASTIC_SORTED_SETS` (1; 0 takes the set numbers in cyclic order, "Reduced
resolution") and `STOCHASTIC_DRAW_OFFSET_PER_PIXEL` (0; 1 draws the hidden candidates with a per-pixel offset instead of a
per-tile one). Since the kernel split these are on `Stochastic.Select.PSO` (`STOCHASTIC_GUIDING` and
`STOCHASTIC_VISIBLE_LIST_CAPACITY` on `Stochastic.Shade.PSO` too, which builds the lists), with
`STOCHASTIC_DEDUPLICATE_RAYS` (1; 0 traces one ray per sample, "Kernel split").

### Validation (2026-11-14)

ZorahLights, frozen lights, 2560×1600, 2 samples per pixel. Each comparison is within one session.

**Unbiased.** Accumulated estimator against the reference (both read back from `stochastic_diffuse` + `_specular`, about
4,000–5,000 frames each), with the reference captured before and after to measure drift:

| Scene | Mean ratio | Pixel abs. diff, mean | 16×16 block diff, mean / p95 | Reference drift, block mean / p95 |
|---|---|---|---|---|
| Sun only (`ZORAH_LIGHT_SCALE=0`) | 1.0000 | 0.0041 | 0.0006 / 0.0016 | 0.0006 / 0.0022 |
| Sun and 16 point lights | 1.0001 | 0.0059 | 0.0007 / 0.0026 | 0.0006 / 0.0013 |

**Stratified set choice.** Live undenoised frames against the accumulated estimator, 16 lights, 6 frames each:
single-frame pixel error 0.308 with the stratified choice and 0.366 with an independent choice per sample (−16%), 16×16
block error 0.038 against 0.040. Both frame means are within 0.2% of the accumulation.

**The cap.** The share probe (`STOCHASTIC_DEBUG_PROBE=2`), 16 lights: the sun's share averages 0.51; 13% of pixels give
it every sample (no local light reaches them), and about 14% hold it at 0.2 or less while it lights them: tiles whose rays
did not reach it last frame. With the sun alone it takes every sample, or none where the surface faces away (16%).

**Against the shadow map** (sun only, overhead camera, denoised, VSM mode and stochastic mode in one session):

| | Mean, relative to VSM | VSM / unshadowed | Stochastic / unshadowed |
|---|---|---|---|
| Whole frame | 0.994 | 0.840 | 0.835 |
| Pixels both shadow (40% of lit pixels) | | 0.643 | 0.661 |
| Pixels only the rays shadow (18%) | | 0.857 | 0.820 |
| Pixels neither shadows (40%) | | 0.990 | 0.972 |

A ray toward the sun's centre hits in 96% of the pixels the shadow map darkens. The rays add contact shadows the shadow map
doesn't have (ledges, stair rims, ground cover); shadows both find are about 3% lighter with rays, from the sun's disc. On a
loop view (`ZORAH_CAMERA_T=0.75`) the domes and roofs match the shadow map; their dark blotches are geometry (louvred
dormers), present in both.

**Cost, moving camera** (`ZORAH_CAMERA_MOTION=profile`, 16 lights, pass timer, two profiles each):

| Pass | VSM mode | Stochastic mode |
|---|---|---|
| StochasticSamplePass | | 6.19 ms |
| StochasticDenoiseTemporalPass + SpatialPass | | 0.92 + 0.58 ms |
| DeferredShadingPass | 7.57 ms | 0.61 ms |
| `CLodShadow::HierarchicalCullingPass1` | 3.7–4.0 ms | absent |
| Other shadow map passes (raster, pages) | run | absent |

The sampling pass is now the largest of these. Earlier measurements of it on 2026-11-14 (3.08 ms, still camera) were
taken with the BVH broken (below) and are not comparable: a ray that hits nothing is cheap.

**The BVH fault this exposed.** On 2026-11-14 every ray missed: the ray traced primary visibility comparison
(`SARP_RT_PRIMARY_COMPARE=1`) logged `match=0 rayMissed=4090730`, the path tracer drew only sky, and stochastic lighting
found almost no occluders while staying "unbiased" against a reference that traces the same BVH. The BLAS cache's block
in `CLodRtPlaceCS` (`rayTracingSceneBuild.hlsl`) had been inserted between `if (slot == CLOD_RT_CACHE_INVALID)` and its
`else`, which took the `else`: no cluster reference got its CLAS address, so every BLAS was empty. After the fix the
comparison logs `match=4073980 rayMissed=5` (classic TLAS) and `match=4076593 rayMissed=6` (partitioned). After any change to
the ray tracing scene build, run that comparison: raster images, and checks against the reference, don't show it.

**Diagnostics.** `STOCHASTIC_DEBUG_PROBE` (pso.recompile, with `stochasticLightingDenoise` off; read back with `capture.hdr`
targets `stochastic_diffuse` / `stochastic_specular`). Since the kernel split ("Kernel split") each probe runs where its values
are, and the kernel that writes it marks the pixel so shading leaves it alone: 2 and 4 on `Stochastic.Select.PSO`; 5 on
`Stochastic.Select.PSO` and `Stochastic.Shade.PSO` (selection passes the reprojected list's length in the pixel word); 1 and 3
on `Stochastic.Select.PSO` and `Stochastic.Trace.PSO` (selection gives each sampled pixel with a surface one ray entry and no
shadow rays, and the trace kernel traces the probe's ray and writes its value). The runner's `--pso` takes the list. The
probes write at the sampled pixels, so with reduced resolution each value covers its 2×2 block.

| Value | Diffuse | Specular |
|---|---|---|
| 1 | A camera ray with every candidate accepted: hit, candidates seen, distance | The same ray through `CLodRtTrace`: hit, distance, the TLAS descriptor |
| 2 | The pixel's directional share, unlisted share, 1 where reprojection held | 0 |
| 3 | A ray toward the sun's centre, no culling: hit, distance, the hit material's flags | N·L |
| 4 | The froxel's opaque light count, the local candidates evaluated (listed and drawn), and (brute force over every active light) the lights in range; in range / count is the culling's tightness, and a count below the lights in range means a dropped light. Works with `pso.recompile` (the pass already binds `Light::ActiveLightIndices`); a graph rebuild (any setting that rebuilds it) drops the override, so recompile after one | 0 |
| 5 | Guiding's lists: the reprojected list's length (-1 where reprojection failed), this tile's unique visible lights this frame before the capacity cut (on the first wave's pixels, -1 on the others; take the maximum over each tile: 8×8 pixels, 16×16 with reduced resolution), and the capacity | 0 |

## Scaling (measured 2026-11-14)

ZorahLights, moving camera (`ZORAH_CAMERA_MOTION=profile`), frozen lights, 2 samples per pixel, pass timer:

| Lights (`ZORAH_LIGHT_SCALE`) | StochasticSamplePass | Denoiser (temporal + spatial) | LightCullingPass | DeferredShadingPass |
|---|---|---|---|---|
| 16 (1) | 6.17 ms | 0.93 + 0.59 | 0.03 | 0.61 |
| 64 (4) | 8.10 | 0.97 + 0.63 | 0.05 | 0.60 |
| 256 (16) | 13.79 | 1.00 + 0.68 | 0.16 | 0.61 |
| 1024 (64) | 39.44 | 1.02 + 0.89 | 0.58 | 0.64 |

Per pixel, still overhead camera (`STOCHASTIC_DEBUG_PROBE=4`): the cluster holds 5 / 68 / 279 lights (mean, at 16 / 256 /
1024), of which 3.7 / 46 / 185 reach the surface; no light was dropped by the page pool in this view. The sampling pass
costs about 0.12 ms per candidate a pixel walks, on top of about 5.7 ms that doesn't depend on lights (the rays, about
half of it, the surface history and the list build).

NVPerf (sampling pass, still camera): 168 registers a thread, 24% occupancy (3 warps a scheduler), launch stalls on
register allocation 28%; issue active 20% at 16 lights (latency: rays and loads), 43% at 256 (candidate ALU).

Experiments, one session each, moving camera:

| Variant | 256 lights | 1024 lights |
|---|---|---|
| Current | 13.87 ms | 39.5 ms |
| `STOCHASTIC_MAX_SAMPLE_COUNT=2u` (reservoirs unrolled for 2, not 4) | 13.83 | 39.0 |
| `STOCHASTIC_CHEAP_IMPORTANCE=1` (Lambert + GGX D importance; the chosen sample still shaded in full) | 11.37 | 30.3 |
| Both | 11.28 | 29.1 |

`STOCHASTIC_CHEAP_IMPORTANCE` stays unbiased by construction (the importance is positive wherever the contribution is,
and the estimate divides by it). It is on by default since 2026-11-15 ("Bounded candidates").

### Baseline (2026-11-14, `scripts/stochastic_scaling.sh`)

The runner's profile mode, one session per row, moving camera, frozen lights, 2 samples per pixel, 300 samples each. Times
in ms. **Dropped** is the most (cluster, light) pairs the light culling's page pool (12-light pages, 10 a cluster on
average, replaced by the light grid on 2026-11-15) had no room for in a sampled frame.

| Range | Lights (`ZORAH_LIGHT_SCALE`) | StochasticSamplePass | Temporal + spatial | DeferredShadingPass | LightCullingPass | ClusterGenerationPass | Dropped |
|---|---|---|---|---|---|---|---|
| 18 | 16 (1) | 6.21 | 0.94 + 0.59 | 0.60 | 0.03 | 0.02 | 0 |
| 18 | 64 (4) | 8.14 | 0.96 + 0.63 | 0.60 | 0.05 | 0.02 | 0 |
| 18 | 256 (16) | 13.90 | 0.96 + 0.67 | 0.61 | 0.17 | 0.02 | 0 |
| 18 | 1024 (64) | 39.04 | 0.99 + 0.82 | 0.61 | 0.63 | 0.02 | 0 |
| 18 | 4096 (256) | 88.79 | 0.96 + 1.00 | 0.62 | 2.06 | 0.02 | 310,229 |
| 6 | 16 (1) | 4.95 | 0.89 + 0.57 | 0.63 | 0.03 | 0.02 | 0 |
| 6 | 64 (4) | 5.35 | 0.93 + 0.59 | 0.61 | 0.05 | 0.02 | 0 |
| 6 | 256 (16) | 6.21 | 0.97 + 0.62 | 0.61 | 0.16 | 0.02 | 0 |
| 6 | 1024 (64) | 9.53 | 0.94 + 0.65 | 0.63 | 0.58 | 0.02 | 0 |
| 6 | 4096 (256) | 21.28 | 0.95 + 0.73 | 0.60 | 2.03 | 0.02 | 0 |

The range-18 rows reproduce the table above within 1%. At 4096 lights and range 18 the sample pass time is an
underestimate: the clusters that overflow walk only the lights they kept. Range 6 at 4096 lights stores 69,000–87,000
(cluster, light) pairs, well inside the pool. The light culling cost grows with the light count alone (every light
against every cluster), about 0.5 µs per light.

### After bounded candidates (2026-11-15)

Sample pass with the defaults (K = 24, cheap importance), from "Bounded candidates", range 18: 6.37 / 11.19 / 13.53 /
16.04 ms at 16 / 256 / 1024 / 4096 lights (before: 6.33 / 14.99 / 42.87 / 93.74, same sessions); range 6: 5.03 / 8.23 /
10.13 ms at 16 / 1024 / 4096 (before: 4.89 / 9.72 / 26.83).

### After the light grid (2026-11-15)

Phase 2 replaced the linked light pages with the light grid for every consumer ([lighting.md](lighting.md) "The light
grid"): compact culling records, froxels shrunk to their opaque depth, one contiguous run per froxel, a growable index
buffer and no silent drops. The estimator reads its draw straight from the run ("Reading the cluster", above).

Moving camera, range 18, one session per row, the page build (Phase 1) and the grid run back to back on one afternoon;
ms. Culling is the grid's three passes (prepare + depth bounds + culling) against `LightCullingPass` +
`ClusterGenerationPass` (0.02).

| Lights | Sample, pages | Sample, grid | Culling, pages | Culling, grid | Dropped, pages | Dropped, grid |
|---|---|---|---|---|---|---|
| 16 | 6.99 | 6.82 | 0.05 | 0.02 + 0.12 + 0.01 = 0.15 | 0 | 0 |
| 256 | 11.38 | 10.74 | 0.23 | 0.02 + 0.12 + 0.03 = 0.17 | 0 | 0 |
| 1024 | 13.45 | 12.65 | 0.68 | 0.02 + 0.12 + 0.08 = 0.22 | 0 | 0 |
| 4096 | 15.48 | 13.47 | 2.16 | 0.02 + 0.12 + 0.31 = 0.45 | 261,434 | 0 |

- The sample pass is 0.2–0.8 ms faster from the direct read (no page walk) and the tighter runs; at 4096 lights the page
  build's time was for the lights it kept. Phase 1's own numbers (6.37 / 11.19 / 13.53 / 16.04) were from other sessions.
- The grid costs more at 16 lights (0.15 against 0.05 ms), almost all of it the depth bounds pass, whose floor (reading
  `LinearDepthMap` alone) is 0.09 ms.
- Tightness, probe 4, overhead still camera, 1024 lights: the froxel holds 200 lights of which 185 reach the pixel,
  92.5% (pages: 279, 66%). No pixel's froxel holds fewer lights than reach it.
- At 4096 lights the camera loop needs up to 3.2 million index entries (13 MB). The buffer started at 2 million in these
  runs and grew once during loading (log: `light grid index entries 2097152 -> 4194304`, after 70,296 pairs were dropped
  in one frame); it now starts at 4 million.
- Unbiased (`bias`, 4,195 frames each): 16 lights mean ratio 1.0002, block |d| 0.0009 / p95 0.0031 against a drift of
  0.0006 / 0.0014; 64 lights 1.0000, 0.0019 / 0.0059 against 0.0005 / 0.0006. Both match Phase 1 (the residual at 64
  lights predates it, "Bounded candidates").
- VSM mode (`SARP_STOCHASTIC_LIGHTING=0`), 1024 lights, moving camera: deferred shading 35.0 ms with 32-pixel tiles
  against 36.2 with 64 (the opaque runs are its loop); the HDR image at a held camera (`ZORAH_CAMERA_T=0.3`, 64 and 1024
  lights) matches the page build's within the in-session noise (mean ratio 1.0000, block |d| 0.0004 / 0.0002 against a
  floor of 0.0005 / 0.0002).

### After guiding at scale (2026-11-15)

Sample pass with the wave-intrinsic list build ("Guiding at scale"), range 18, moving camera: 6.23 / 9.62 / 11.28 / 12.31 ms
at 16 / 256 / 1024 / 4096 lights, against 6.85 / 10.93 / 12.58 / 13.43 for the Phase 2 shader in the same sessions (Phase 2's
own table read 6.82 / 10.74 / 12.65 / 13.47). The denoiser and the light grid are unchanged (temporal + spatial 1.15 + 0.7,
culling 0.03–0.32).

### After reduced resolution (2026-11-15)

Sample pass with reduced resolution on (the default, "Reduced resolution"), range 18, moving camera: 3.50 / 4.86 / 5.78 /
6.26 ms at 16 / 256 / 1024 / 4096 lights, against 6.48 / 10.10 / 12.07 / 12.29 at downsample 1 in the same sessions.
Denoiser (temporal + spatial) 1.02 + 0.73 to 1.14 + 0.93 ms, as at downsample 1. At 16 lights rays are 2.6 of the 3.5 ms.

### Running the benchmarks

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
    # A/B against a shader file that defines can't express (here the pre-Phase-1 walk), swapped into the deployed shaders/:
    scripts/stochastic_scaling.sh profile --scales "64" --variant 'new={}' --variant 'old={}' --shader 'old=/path/old.hlsl'
    # (b) Unbiasedness, still overhead camera: reference, accumulation, reference again (each held for --converge
    #     frames, default 4000), then mean ratio, pixel |d| and 16x16 block |d| against the reference's own drift:
    scripts/stochastic_scaling.sh bias --scale 16 --range 18
    # (c) Noise: the accumulation, then --frames live undenoised frames per variant, each against the accumulation:
    scripts/stochastic_scaling.sh noise --scale 16 --variant 'base={}' --variant 'full={"STOCHASTIC_CHEAP_IMPORTANCE":"0"}'
    # (d) Look: --frames live denoised frames per variant as PNGs (rgb x / (1 + x), sRGB) in out/stochastic_scaling/look/,
    #     here with moving lights:
    scripts/stochastic_scaling.sh look --scale 4 --frames 4 --vars "ZORAH_FREEZE_LIGHTS=0"
    # (e) Noise after DLSS ("Noise after DLSS"), held at the moving-camera test's start (ZORAH_CAMERA_T=0; another view with
    #     --vars), DLSS on: the truths, the undenoised input, then each denoiser variant (--pso defaults to the denoiser's two
    #     pipelines here), before and after DLSS; a table per scale and a crop PNG in out/stochastic_scaling/dlss/:
    scripts/stochastic_scaling.sh dlss --scale 1 --variant 'base={}' --variant 'old={"DENOISE_RECONSTRUCT_MEAN":"0","DENOISE_SPATIAL_MIN_RADIUS":"0.0f"}'

- Variants recompile `Stochastic.Select.PSO` and `Stochastic.Shade.PSO` (`--pso` takes another comma-separated list) with
  `pso.recompile`. The profile table's Sample column is the three split passes' sum (Select, Trace and Shade are shown too). A variant whose
  defines change the shader's Builtin resources fails to publish; the runner stops with the demo log's reason.
- `--shader 'name=PATH'` runs variant `name` with the file PATH copied over the deployed `--shader-target` (default
  `shaders/Stochastic/stochasticSelect.hlsl`); the deployed file is restored after the run. The file must reference the
  same Builtin resources. Each profile's summary is kept as `out/stochastic_scaling/<run>_<variant>.md`, so a run with
  `CONFIG` pointing at an NVPerf config (registers, instructions) keeps one per variant.
- A variant key `"setting:NAME"` sets a setting instead of a define (`settings.set`), for example
  `--variant 'd2={"setting:stochasticLightingDownsample":2}' --variant 'd1={"setting:stochasticLightingDownsample":1}'`. A
  setting that rebuilds the graph drops every `pso.recompile` override, so the runner applies the variant's defines again
  after it. A setting keeps its value until a later variant sets it: name it in every variant.
- `pair` (moving camera, `ZORAH_CAMERA_MOTION=always`) recompiles the spatial pass with `DENOISE_DEBUG_PAIR` and captures
  `--frames` frames per variant: each frame's denoised luminance against its own noisy input. The mean ratio is the
  denoiser's bias in motion, the 32×32 block |d| its lag and blur plus the noisy block means' own noise, which it estimates
  per block (the noise floor). `noise --denoise` keeps the denoiser on: still-camera denoised error against the accumulation.
- `dlss` captures with `capture.hdr`'s `targets`, `frames` and `luminance` (TESTING.md "HDR capture"): R1 (the converged
  reference, `--ref-frames`, default 8), P1 and P2 (the per-frame reference with a `--ref-grid` × `--ref-grid` ray grid,
  default 4, `--per-frame-frames` each, default 32), the undenoised input (`--no-noisy` skips it) and `--sets` (4) sets of
  `--frames` (8) consecutive frames per variant, `--set-gap` (1) seconds apart. About 1.5 GB of R32F files a variant at
  2560×1600, deleted after the analysis unless `--keep`. A session at 16 lights with four variants takes about 6 minutes.
  The table's cells are total / bias / slow / fast in percent of white (display-weighted; `SS_DLSS_LINEAR=1` for linear),
  and the last lines give noise after DLSS, `sqrt(slow² + fast²)`, per scale. Repeat a variant (`base2` with a no-op define
  such as `{"DENOISE_SPIKE":"1"}`) for the session's spread: about ±3% here.
- Guided against unguided: `--variant 'unguided={"STOCHASTIC_GUIDING":"0"}'` (put it after the guided variants: the
  first guided frames after it read stale lists).
- `CONFIG=config/stochastic_sample_nvperf.json` profiles the sample pass's hardware counters (registers, L1 hit rate,
  stalls, launch limits) instead of its time.
- `--vars "A=1 B=2"` adds environment variables (for example `SARP_STOCHASTIC_SAMPLES=1`); `--samples n` lowers the
  profile's sample count for slow scenes.
- Pass timers move ~10% between sessions. Compare within a session (`--variant`), and rerun the base row of a table
  rather than comparing against an old one.

Checked on 2026-11-14 at 16 lights, range 18: `bias` gave mean ratio 1.0000, block |d| 0.0008 / p95 0.0027 against a
drift of 0.0007 / 0.0014 (4,195 frames each), matching "Validation (2026-11-14)". `noise` gave a single-frame pixel
error of 0.300 (block 0.033) for the default and 0.324 (block 0.040, +7.8%) for `STOCHASTIC_CHEAP_IMPORTANCE=1`.

## Denoiser

M3, `shaders/Stochastic/stochasticDenoise.hlsl` (`StochasticDenoisePass`). It runs after the estimator unless
`stochasticLightingDenoise` is off (`SARP_STOCHASTIC_DENOISE=0`; menu: **Stochastic Denoiser**; it rebuilds the graph). It
never runs with the reference or the accumulation. The estimator then writes its raw output to two noisy textures, and
the denoiser writes `StochasticLighting::Diffuse` and `::Specular`.

The design is shaped by one property of the input. At 2 samples per pixel, the estimator's radiance is unbiased but
heavy-tailed. About half of all pixels are black in any one frame, and rare samples are thousands of times the mean.
Anything that compares a pixel with its neighbours, clamps it, or picks a filter width from its own value trims that
tail, and trimming the tail darkens. Each step below avoids that, and "Validation" gives the measurements that ruled out
the conventional versions.

With reduced resolution (the default) the noisy inputs hold one sample per 2×2 block, and the temporal pass first
reconstructs each pixel's value for the frame from the nearest samples ("Reduced resolution"); the rest is as below.

**Demodulation.** Both signals are divided by what the material contributes, so the filter blurs lighting, not texture:
diffuse by the diffuse colour (`Surface::BaseColorOpacity`), specular by F0 (`Surface::SpecularAo`). Both are floored at
0.02, and the spatial pass multiplies by the same floored values of the current frame.

Specular is not divided by the split-sum albedo `F0·A + B`, the usual choice. That albedo depends on N·V. With normal maps
and the upscaler's jitter, N·V at a pixel changes from frame to frame. Dividing by one frame's value and multiplying by
another's made specular 13% too bright.

**Temporal pass** (8×8 groups):

1. Loads the tile plus a 2-pixel border, demodulated, into groupshared memory. For rectification it also keeps each
   pixel's compressed luminance `log2(1 + Y)`. Each pixel computes the 5×5 mean and variance of that compressed value.
2. Reprojects last frame's history through `Surface::Motion` with four bilinear taps. A tap counts only if the
   estimator's surface history there is within 5% of this point's depth in last frame's view. Under 5% total weight is a
   disocclusion. There is no normal test: the shading normal is normal-mapped, and under jitter it swings past any useful
   threshold at the same pixel between frames. A cosine-0.5 test (0.8 at first) threw away the history of 13% of pixels
   every frame with a still camera.
3. Rectifies, as in MegaLights, by shortening the history, never by moving its value. The distance is between the
   history's mean compressed luminance (kept as a moment) and the neighbourhood's, measured in standard deviations of
   that difference. The history is kept in full up to 1 σ, and shrinks to 1 frame at 2 σ. The variance is the mean of
   the history's temporal variance and the neighbourhood's spatial variance, because each alone is lopsided with moving
   lights:
   - Where a light has just left, the history's variance is large (it saw the light) and the neighbourhood's small.
   - Where one has just arrived, the reverse.
   - The temporal variance alone keeps departed light and cuts arriving light: 1.4–1.7× too bright. The spatial
     variance alone does the opposite: 5% too dark.

   Clamping linear values into the neighbourhood's mean ± 1.5 σ box, the usual approach, darkened by 2–11%. Most 5×5
   neighbourhoods contain none of the rare bright samples, so their box sits below a converged history. Choosing the
   variance by direction (the neighbourhood's when the history is brighter, the history's when darker) was 6% too dark
   in motion.
4. Limits spikes. The compressed test is blind to a single bright sample, because one sample barely moves a mean of
   `log2(1 + Y)`. A fast light leaves its bright spot in the history at 1/32 weight, and the result is a row of dots
   along its path, one per frame. So the history's linear luminance may not exceed 4× the brightest of this frame's 5×5
   samples. The limit applies only where at least 75% of those samples are lit. About half of all pixels are black in
   any frame, and without that condition a fully black neighbourhood set the limit to 0, which punched black holes into
   the history.
5. Blends with weight 1/length (length up to 32, kept separately for diffuse and specular in the history's alpha), and
   blends the compressed moments the same way.
6. Writes the history in fp16 with stochastic rounding: up to the next fp16 value with probability equal to the
   fraction of the way there. The fp16 conversion truncated, and an exponential average over 32 frames that loses up to
   one ulp per frame came out 0.6% dark. Values are capped at 60,000 so that a demodulated firefly cannot overflow.

History: RGBA16F diffuse (rgb, length), RGBA16F specular (rgb, length), and RGBA16F moments of the compressed luminance,
ping-ponged by frame parity. The per-sample luminance variance (from the compressed one, through the derivative of
`log2(1 + Y)`) goes to an RG16F texture for the spatial pass's luminance stop.

**Spatial pass:** one sparse kernel, 12 Poisson taps rotated per pixel and frame, run after the history is written so it
doesn't feed back. It is wide where the history is short:

- The radius is 8 px for a new history and fades to 3 px at about 5 frames (`DENOISE_SPATIAL_MIN_RADIUS`). It depends on
  nothing else. Until 2026-11-15 it faded to 0 at 8 frames, so a converged pixel was not filtered at all; the 3 px floor is
  for DLSS ("Noise after DLSS").
- At the floor (both signals converged) it takes 4 taps in a plus instead of the 12, rotated per pixel and frame, each tap's
  depth and normal loaded once for both signals (`DENOISE_SPATIAL_FLOOR_TAPS`).
- Specular's radius is also scaled by roughness.

The earlier rule took the radius from the pixel's own relative variance. That brightened dim pixels by 1.3%: a dim
pixel's estimate made its relative variance look high, which widened the filter and pulled in brighter neighbours. With a
luminance stop added, the same rule darkened specular by 8%, because bright neighbours were rejected. Both biases came
from deciding the filter from a noisy value. At convergence the filter only blurred detail.

- Edge-stopping weights:
  - relative depth (2%);
  - normal, as `dot^32`;
  - luminance against 4 σ of the history's own standard deviation (variance over length).
- It remodulates by the floored albedos.

**Random numbers.** The light selection's two stratified numbers (listed and unlisted sets) come from an R1 sequence per
pixel: a hashed per-pixel offset plus `frame · 0.618…` (`StochasticTemporalSequence`). Consecutive frames therefore
stratify each pixel's choices. It stands in for spatiotemporal blue noise (STBN) textures, which aren't vendored.
Interleaved gradient noise was tried first and was biased per pixel: its 64-frame cycle doesn't spread each pixel's values
evenly. The emitter point and the choice of set stay on PCG, because driving them from the same sequence as the selection
would correlate them and bias the estimate.

**Frame seeds.** Before M3, the passes seeded their noise with `UpdateContext::frameIndex`. That is the frame *slot*,
which repeats every few frames, so M1 and M2's "accumulated" images averaged only a few distinct sample patterns. The
passes now use `frameNumber`. The ray traced sun shadow pass (`RayTracedShadowPass`) still seeds with `frameIndex`.

**Diagnostics.** Most constants have `#ifndef` guards, so `pso.recompile` can try other values in a running demo. Pipeline
ids are `Stochastic.DenoiseTemporal.PSO` and `Stochastic.DenoiseSpatial.PSO`. The switches:

| Define | Default | Effect |
|---|---|---|
| `DENOISE_RECTIFY` | 1 | Rectification on or off. |
| `DENOISE_RECTIFY_SIGMA` | 1.0 | Where rectification starts shortening the history, in σ. |
| `DENOISE_SPIKE` | 1 | Spike limit on or off. |
| `DENOISE_SPIKE_LIMIT` | 4.0 | The limit, in multiples of the 5×5 neighbourhood's brightest sample. |
| `DENOISE_SPIKE_LIT_FRACTION` | 0.75 | The fraction of lit samples a neighbourhood needs before the limit applies. |
| `DENOISE_SPATIAL` | 1 | Spatial pass on or off. |
| `DENOISE_STOCHASTIC_ROUNDING` | 1 | Stochastic rounding of the fp16 store on or off. |
| `DENOISE_DEPTH_TOLERANCE` | 0.05 | The reprojection depth test. |
| `DENOISE_RECONSTRUCT_DEPTH` | 0.05 | Reduced resolution: the relative depth difference at which a sample's weight for a pixel reaches 0. |
| `DENOISE_RECONSTRUCT_NORMAL_POWER` | 8 | Reduced resolution: the normal agreement's exponent. |
| `DENOISE_RECONSTRUCT_MIN_WEIGHT` | 0.05 | Reduced resolution: below this total weight no sample lies on the pixel's surface, and it keeps its history alone. |
| `DENOISE_RECONSTRUCT_MEAN` | 1 | Reduced resolution: the frame's value is the four samples' weighted mean (0: one chosen at random, before 2026-11-15). |
| `DENOISE_SPATIAL_MIN_RADIUS` | 3.0 | The spatial filter's smallest radius in pixels (0: it runs on short histories only, before 2026-11-15). |
| `DENOISE_SPATIAL_FLOOR_TAPS` | 1 | At that floor, 4 taps in a rotated plus with shared geometry loads instead of the 12 Poisson taps. |
| `DENOISE_MAX_HISTORY` | 32 | The history's length cap. |
| `DENOISE_DEBUG_PAIR` | unset | On the spatial pass: `StochasticLighting::Diffuse` holds the frame's denoised (r) and noisy (g) total luminance, so one `capture.hdr` pairs them. |

### Validation (2026-11-13)

Measured in ZorahLights at 2560×1600 with 2 samples per pixel. The scene keeps changing for minutes after launch
(texture and geometry streaming), so a reference captured earlier in the run is not a valid target. Every comparison
below is either interleaved within one run or paired within one frame.

**Frozen lights, interleaved.** Undenoised captures alternate with each denoiser variant through one run, and each
variant is compared with the undenoised captures around it. 16 lights, 8 captures per variant per round:

| | Mean vs undenoised | Single-frame error vs all undenoised frames |
|---|---|---|
| Undenoised | 1.000 | 0.24–0.25 |
| Temporal only, before rectification (moving average) | 1.005 | 0.08 |
| Full denoiser (rectification and spike limit) | 0.97 | about 0.08 |

The single-frame error is against the average of 24 undenoised frames, which itself carries about 0.05 of noise, so
0.08 is close to that floor. The full denoiser's 3% darkening comes from rectification (frozen, paired, below).

**Moving lights, paired.** `DENOISE_DEBUG_PAIR` writes the denoised and noisy luminance of the same frame side by side.
The noisy frame is an unbiased estimate of that frame, so the ratio of their means measures the denoiser's bias, and the
mean difference of 32×32 block averages, relative to the mean, measures lag. 32 lights, 3 rounds of 6 frames per
variant, interleaved:

| Variant | Frozen: mean | Normal speed: mean | Normal speed: block error | 4× speed: mean | 4× speed: block error |
|---|---|---|---|---|---|
| No rectification | 1.000 | 0.95 (0.84–1.03) | 0.89 | 1.01 (0.85–1.14) | 1.12 |
| Rectification only | 0.974 | 1.056 | 0.31 | 1.097 | 0.55 |
| **Rectification and spike limit 4× (default)** | **0.969** | **0.977** | **0.25** | **0.979** | **0.47** |
| Rectification and spike limit 2× | 0.968 | 0.966 | 0.23 | 0.938 | 0.42 |

Without rectification the mean stays near 1 on average, but light lingers for the whole 32-frame window: the block
error is 4× higher, and the per-round means swing with where the lights happen to be. Rectification alone keeps the
dots that fast lights leave, which is the brightening in its row.

**Light culling.** The moving-light runs exposed a bug in clustered light culling (`shaders/lightCulling.hlsl`).
When a cluster's page of 12 lights filled, the next page was allocated before checking whether any later light reached
the cluster. If none did, the newest page stayed empty, and the readers (this estimator, the reference and the deferred
light loop) stop at an empty page. So every cluster whose light count was exactly 12 or 24 lost all its lights for
that frame. With moving lights, 7–10% of pixels went black in rectangles on the 12×12 cluster grid. Culling now
allocates a page only when it has a light to put in it, and no pixel's cluster misses a light that reaches it.
Frozen-light comparisons with the reference were unaffected, because the reference reads the same clusters.

**Still to measure:** cost of each pass, and 64 lights. At 64 lights there was visible tiling, thought to be the per-tile
visible-light lists overflowing (32 entries per 8×8 tile). By 2026-11-15 it was gone, and lists at 64 lights never hold
more than 13 lights ("Guiding at scale").

## Noise after DLSS (2026-11-15)

The denoiser was tuned against its own output, from the overhead view. What the user sees is DLSS's output, and from the
moving-camera test's start position (`ZORAH_CAMERA_T=0`) noise was visible after DLSS even with 16 lights. This section
measures the noise left after DLSS and what the denoiser does to it.

**Where DLSS runs.** The demo runs DLSS Super Resolution (Streamline `kFeatureDLSS`, preset K, DLAA by default, so the
render and output resolutions are the same), not Ray Reconstruction. Its inputs are the HDR colour, depth, the motion
vectors (dilated, unjittered; `mvecScale` converts the NDC delta), and a 16-phase Halton jitter; auto-exposure is on and
there is no reactive or bias mask. Stochastic lighting reaches it composited: the deferred pass adds
`StochasticLighting::Diffuse + ::Specular` to the HDR colour in place of the analytic light loop. The debug output 77
is resolved after the upscaler, so it never shows DLSS; the metric reads `Builtin::PostProcessing::UpscaledHDR` instead.
Profile runs (`--sampling-config`) turn Streamline off; the other runner modes keep DLSS.

### The metric

`scripts/stochastic_scaling.sh dlss` (below, "Running the benchmarks"), held camera, frozen lights, DLSS on:

1. **R1**, the converged reference (accumulation, `--converge` frames): the truth before DLSS.
2. **P1**, the reference's *per-frame* estimate (`STOCHASTIC_REFERENCE_PER_FRAME=1`) with a 4×4 grid of rays per light
   (`STOCHASTIC_REFERENCE_RAY_GRID=4`), 32 consecutive frames. DLSS's output of it, averaged, is the truth after DLSS.
3. The undenoised input (`stochasticLightingDenoise` off), then each variant: 4 sets of 8 consecutive frames, 1 s apart.
4. **P2**, the per-frame reference again (its drift against P1 is part of the floor).

Every frame is captured twice, before DLSS (stochastic lighting's two signals) and after it (the upscaled HDR image), as
luminance, with `capture.hdr`'s `targets`, `frames` and `luminance` (TESTING.md "HDR capture").

Why the truth after DLSS is not DLSS's output of the converged reference: that reference averages over the jitter, so it
is already antialiased, while the rest of the frame jitters. DLSS reconstructs such an input differently, and with it as
the truth the error map was the scene's edges, not noise. The per-frame reference jitters with the frame. With one ray per
light it is not clean enough: the sun's disc makes penumbrae metres wide, which were as noisy per pixel as the estimator
and which DLSS darkened as it darkens the estimator's noise (below). With a 4×4 ray grid its same-jitter-phase noise is
0.13 of the mean stochastic luminance per pixel (0.97 with one ray), and P2 against P1 differs by 0.003 of it at 32×32.

**Errors.** Each pixel's error is weighted by the slope of `y / (y + Y)` at the truth (Y the truth's mean final luminance) and
given in percent of white, so errors count as they would on a display. Linear errors (`SS_DLSS_LINEAR=1`) were dominated
by highlights near the lights: 84% of the squared error came from the 0.5% of pixels brighter than 1. Before DLSS the rest
of the final image enters the slope, so both stages are errors in the final image. At three scales (pixels, 8×8 and 32×32
block means), each split in three:

- **fast**: the flicker within a set of 8 consecutive frames;
- **slow**: the spread of the sets' means about each other, less fast's share: noise that holds for tens of frames (crawling
  blotches) but not seconds;
- **bias**: what every set shares.

*Noise after DLSS* is `sqrt(slow² + fast²)` after DLSS, the primary metric. The bias after DLSS is mostly not the denoiser's:
DLSS darkens jittering content of high variance (it seems to average in a compressed space), so the undenoised input comes
out 2–10% darker than the truth and the denoised one 1–6% brighter, while before DLSS the undenoised input is unbiased and
the denoised one 1–3.5% dark. The denoiser's bias is judged before DLSS.

### Baseline (the denoiser before this section)

16 lights, range 18, `ZORAH_CAMERA_T=0`, downsample 2; total / bias / slow / fast, percent of white:

| | Stage | Pixel | 8×8 | 32×32 |
|---|---|---|---|---|
| Per-frame reference (floor) | after | 0.18 / 0 / 0.06 / 0.17 | 0.033 / 0 / 0.008 / 0.032 | 0.012 / 0 / 0.004 / 0.012 |
| Undenoised | before | 8.66 / 2.87 / 0 / 7.72 | 1.95 / 0.70 / 0 / 1.72 | 0.66 / 0.47 / 0 / 0.44 |
| Undenoised | after | 2.01 / 1.96 / 0.44 / 0.17 | 0.42 / 0.41 / 0.10 / 0.04 | 0.30 / 0.30 / 0.026 / 0.011 |
| Denoised | before | 2.95 / 1.84 / 1.09 / 1.90 | 0.71 / 0.58 / 0.21 / 0.33 | 0.35 / 0.33 / 0.059 / 0.10 |
| Denoised | after | 1.77 / 1.70 / 0.47 / 0.14 | 0.70 / 0.69 / 0.10 / 0.026 | 0.45 / 0.45 / 0.029 / 0.008 |
| Downsample 1, denoised | after | 0.99 / 0.95 / 0.26 / 0.13 | 0.35 / 0.35 / 0.044 / 0.024 | 0.24 / 0.23 / 0.016 / 0.008 |

Noise after DLSS (pixel / 8×8 / 32×32), each view's own session, `old` the baseline:

| View, lights | Undenoised | Baseline denoiser |
|---|---|---|
| t = 0, 16 | 0.47 / 0.109 / 0.028 | 0.49 / 0.103 / 0.029 |
| t = 0, 64 | 0.50 / 0.117 / 0.031 | 0.63 / 0.124 / 0.035 |
| t = 0.3, 16 | 0.55 / 0.134 / 0.034 | 0.62 / 0.136 / 0.039 |
| t = 0.3, 64 | 0.65 / 0.154 / 0.040 | 0.89 / 0.168 / 0.047 |
| t = 0.6, 16 | 0.48 / 0.112 / 0.030 | 1.28 / 0.241 / 0.063 |
| t = 0.6, 64 | 0.63 / 0.148 / 0.038 | 1.25 / 0.245 / 0.069 |

**The denoiser left as much noise after DLSS as no denoiser at all, and at t = 0.6 more than twice as much.** DLSS removed
the fast part almost entirely, from either input (after DLSS, fast is at the floor). What survived is slow noise.

### What survives DLSS, and why

- **DLSS is a long temporal average on a still view.** It cut the undenoised input's 8×8 noise from 1.72 to 0.11, as an
  average over a few hundred frames would. So what reaches the screen is the input's noise at *low temporal
  frequencies*.
- **A temporal average ahead of it doesn't lower that.** An exponential average has a gain of 1 at frequency 0: it removes the
  high frequencies, which DLSS removes anyway, and passes the slow noise as it found it. A shorter history
  (`DENOISE_MAX_HISTORY=8`) left more noise after DLSS at every scale (0.50 / 0.113 / 0.033 against 0.48 / 0.100 / 0.029),
  because DLSS then has more to do. A longer one alone (64) lowered it by 5–7%.
- **Rectification adds slow noise.** On a still scene the history is kept in full only within 1σ of the neighbourhood
  (`DENOISE_RECTIFY_SIGMA`) and cut to one frame at 2σ. Were the distance normal, noise alone would put a pixel past 1σ a
  third of the time, so histories are cut often, in patches, and a patch's restart is noise that lasts as long as its new
  history: slow noise. 2σ with a 64-frame cap cut noise after DLSS by 19% / 24% / 24% at 16 lights on its own (and the
  darkening before DLSS: mean ratio 0.985 → 0.995), and on top of the two changes below by a further 22% / 29% / 34% at
  64 lights (the same session). At 64 lights neither half helps alone: at 1σ the cap is never reached, and under a
  32-frame cap the looser test has little room.
- **The reconstruction's random choice adds noise at every frequency.** Choosing one of the four nearest samples has the
  weighted mean's expectation plus selection noise; the temporal average removes the selection noise's high frequencies
  but not its low ones.
- **Nothing filtered a converged pixel spatially.** The spatial pass faded to nothing at 8 frames, and only a spatial filter
  (or more samples) lowers noise at low temporal frequencies.
- **Reduced-resolution sampling** doubles the slow noise against downsample 1 (pixel 0.49 against 0.30, 8×8 0.10 against
  0.05): a quarter of the samples a frame.

The per-tile draw offset doesn't matter at 16 lights (every light of a froxel is a candidate) and was not separated at 64.

### CORTAO

CORTAO (github.com/theagentd/CORTAO) is an SSAO whose optional filter prepares its output for a later TAA or upscaler
instead of denoising it. Its residual is DLSS-friendly because:

- **It has no temporal accumulation of its own.** A fresh random seed every frame (a hash of the pixel plus a 32-bit
  per-frame random) makes the error new every frame, white in time, which a temporal upscaler averages away. Nothing in it
  can turn noise into slow noise.
- **A small, sparse, fixed spatial blur** cuts the per-pixel amplitude before the upscaler sees it: 5 taps (the centre and
  ±3 px along each axis) or 13 (offsets out to 6 px), each a gather of a 2×2 quad. The lower amplitude keeps the noise
  inside the upscaler's neighbourhood clamp, so it is averaged rather than clamped (clamping is the darkening above); the
  sparse taps keep the remainder fine-grained, not blotchy.
- **Depth weights are slope compensated**: the 2×2 quad's depth gradient predicts each tap's depth on the centre's plane, so
  a high sensitivity doesn't reject a tilted floor's own taps. Normal weights from a packed low-precision normal.
- `CORTAO_FILTER_SYMMETRY` gives each opposite pair the smaller of its two weights so the kernel's centre doesn't move
  toward one side of an edge (it improves contact shadows and leaves more noise near edges).

What carries over: the principle (an upscaler downstream keeps slow error and removes fast error, so the denoiser should not
create slow error) and the always-on small spatial filter. What doesn't: dropping temporal accumulation (DLSS darkens and
mistreats our heavy-tailed input, the 2–10% above; the history also feeds rectification and the variance), and the gather
layout (our kernel already samples sparsely and rotates per frame, which keeps its residual white in time, as CORTAO's
per-frame seed does). Symmetric weights were tried (`DENOISE_SPATIAL_SYMMETRIC`, our 12 taps as 6 opposite pairs): no gain
over the asymmetric kernel (0.48 / 0.102 / 0.029 against 0.44 / 0.095 / 0.027 at a 3 px floor), so the switch was removed.
Slope compensation was not tried: the 2% relative depth test wasn't what rejected taps here.

### What changed

- **`DENOISE_RECONSTRUCT_MEAN` (1)**: the reconstructed value is the weighted mean of the four samples; the moments keep the
  chosen sample, so rectification's statistics are unchanged.
- **`DENOISE_SPATIAL_MIN_RADIUS` (3 px)**: the spatial filter runs on every pixel, 8 px on a new history fading to 3 px,
  with the same edge stops (depth, normal, luminance at 4σ of the history's own deviation).
- **`DENOISE_SPATIAL_FLOOR_TAPS` (1)**: where both signals are at the 3 px floor, CORTAO's small kernel instead of the 12
  taps: 4 taps in a plus at 3 px, rotated per pixel and frame, each tap's depth and normal loaded once for both signals
  (specular joins where it is rough enough, roughness ≥ 0.375, to take the same radius). It left as little noise as the 12
  taps (same session, 16 lights: 0.40 / 0.097 / 0.029 against 0.39 / 0.096 / 0.028; 64 lights: 0.48 / 0.113 / 0.033
  against 0.52 / 0.122 / 0.037) at a third of the added time.
- `DENOISE_MAX_HISTORY` gained an `#ifndef` guard; rectification is unchanged (see "Open", below).

A/B at t = 0, 16 lights (two sessions, `base` run twice in each; baseline 0.48–0.49 / 0.100–0.102 / 0.029): mean alone
0.41 / 0.098 / 0.029; 3 px alone 0.44 / 0.095 / 0.027; both 0.37 / 0.092 / 0.027; 5 px instead of 3 the same at pixels,
0.088 at 8×8.

**After** (noise after DLSS, pixel / 8×8 / 32×32; each row's session ran old, new, old, new, and the numbers are means of the
two; change against the baseline). The t = 0 rows are the final default (4-tap floor); the t = 0.3 and 0.6 rows were
measured with the 12-tap floor, which the 4-tap one matched at t = 0:

| View, lights | Baseline | New default | Change |
|---|---|---|---|
| t = 0, 16 | 0.48 / 0.100 / 0.029 | 0.40 / 0.097 / 0.029 | −17% / −3% / −1% |
| t = 0, 64 | 0.64 / 0.124 / 0.034 | 0.48 / 0.113 / 0.033 | −25% / −9% / −4% |
| t = 0.3, 16 | 0.62 / 0.136 / 0.039 | 0.45 / 0.126 / 0.037 | −27% / −7% / −5% |
| t = 0.3, 64 | 0.89 / 0.168 / 0.047 | 0.85 / 0.173 / 0.049 | −4% / +3% / +5% |
| t = 0.6, 16 | 1.28 / 0.241 / 0.063 | 0.95 / 0.224 / 0.064 | −26% / −7% / +1% |
| t = 0.6, 64 | 1.25 / 0.245 / 0.069 | 0.94 / 0.225 / 0.063 | −25% / −8% / −8% |

A variant's repeats in one session differ by up to 13% per pixel and 8% at 8×8 (the t = 0, 16-light session: 0.43 and 0.37
for the new default), so the per-pixel cut is clear in every session and view but the 8×8 and 32×32 changes are within
the spread. Before DLSS the mean ratio moved toward 1 at every view: t = 0 0.985 → 0.989 (16 lights) and 0.984 → 0.988 (64); t = 0.3 0.990 → 0.993 and 0.988 → 0.992; t = 0.6 0.965 →
0.972 and 0.979 → 0.986. Pre-DLSS bias at 64 lights, t = 0: pixel 2.52 → 2.17, 8×8 0.74 → 0.71, 32×32 0.40 → 0.41.

**Lag in motion** (`pair`, 16 lights, moving camera, `base` and `new` each run twice, 32×32 block |d| against the frame's own
noisy input): moving lights 0.167 / 0.155 against 0.181 / 0.133; lights at 4× speed 0.231 / 0.170 against 0.223 / 0.168.
Within the repeats' spread: no added lag. For comparison 2σ with a 64 cap gave 0.283 / 0.215 and 0.347 / 0.289, and the
4×-speed mean ratio rose to 1.05–1.07 (light lingering).

**Time** (moving camera, `profile`, A B A B in one session, ms, the two runs agreeing within 0.01):

| | Temporal, 16 lights | Spatial, 16 lights | Temporal, 1024 lights | Spatial, 1024 lights |
|---|---|---|---|---|
| Baseline | 0.92 | 0.61 | 0.97 | 0.72 |
| 12 taps at the floor | 0.90 | 2.23 | 0.95 | 2.21 |
| **New default (4 taps at the floor)** | **0.90** | **1.02** | **0.95** | **1.11** |

The mean reconstruction costs nothing; the always-on filter costs 0.4 ms (the 12-tap kernel on every pixel cost 1.6).

### Open

- **Rectification is the largest remaining lever, and it trades against lag.** Loosening it (2σ, 64 frames) removes about
  a third of the slow noise on still views at both light counts and reduces the denoiser's darkening, but with moving lights
  it raises the 32×32 lag error by about 45% and leaves fast lights' trails 5–7% bright. 1.5σ with 48 frames sat in
  between (+24% lag error). A rectification that tells noise from change (for example a test over several frames, or one that knows the
  camera and lights are still) would get both; it is a design decision, so the default is unchanged.
- **A moving-camera version of the metric** isn't built: the camera flies by time, so frames aren't repeatable and there is
  no converged truth. `pair` covers lag and bias in motion.
- **Reduced resolution** is half of the remaining slow noise (downsample 1 against 2, above).
- **The bias after DLSS** is dominated by DLSS's own treatment of the truth (above). A truth accumulated per jitter phase
  (16 converged references) would remove that doubt but costs 16 full-resolution accumulations.

## Moving-light trails (2026-11-15)

Lights that move fast close to a surface leave lit trails behind them that last for tens of frames. The test scene is
`ZORAH_LIGHT_LAYOUT=sweep` in `ZorahLights.cs`: a few lights skim the ground under a held overhead camera. The detector
is `scripts/stochastic_scaling.sh trail`. It captures `--frames` (48) consecutive `DENOISE_DEBUG_PAIR` frames per
variant: the denoised and the noisy luminance of the same frame.

**What the detector reports.** The noisy input is unbiased in every frame. So a block whose input stays dark while its
denoised output stays lit is history the denoiser kept after the light left. For each 8×8 block, the input's truth is
its mean over ±4 frames. The analysis (`stochastic_scaling_analysis.py trail`) reports four numbers, all relative to the
input's mean:

- *excess*: the output above that truth;
- *stuck energy*: the share of the output that falls in blocks whose input is dark (under 2% of the mean);
- *stuck blocks*: the share of blocks that are dark yet show output above 25% of the mean;
- *age*: frames since a stuck block's input was last lit (above 10% of the mean).

It also writes an overlay PNG to `out/stochastic_scaling/trail/`, with stuck blocks in red.

**Baseline** (16 lights, downsample 2): stuck energy 0.015–0.034, stuck blocks about 0.6%, age p95 22–31, maximum
34–46 frames. Repeats of one variant differ by about ±40% in stuck energy, so compare variants in one session, with
repeats.

**Cause.**

1. *The estimator.* A point light skimming a surface puts near-singular samples into the noisy input. Its irradiance
   is `I / max(d², r²)` with the 1 cm radius floor. The input reaches the fp16 maximum, 65504, while the image mean is
   0.5. With the light moving about 9 cm a frame, a pixel sees such a value for about one frame.
2. *Rectification can't see the light leave.* It works on compressed luminance (log2(1 + Y)), and its per-sample
   variance is half the history's temporal variance. The light's one frame inflates that temporal variance, so once
   the light has gone, the dark neighbourhood falls within the inflated spread and the history is kept. This is the
   failure the comment on `DenoiseRectify` describes: the temporal variance alone kept departed light.
3. *The safety net is off where it's needed.* The spike limit covers this case, but it only applies where 75% of the
   5×5 samples are lit. A neighbourhood the light has just left is dark, so it doesn't apply there.
4. *The tail is long.* Even a 4-frame history (`DENOISE_MAX_HISTORY=4`) kept trails for up to 24 frames. An exponential
   average forgets a value 10^5 times its surroundings at its own rate: about 17 frames even at length 1, which is the
   shortest rectification allows.

Measured (each row is one variant in the same session):

| Variant | Denoised / input | Stuck energy | Stuck blocks | Age p95 / max |
|---|---|---|---|---|
| Default | 1.03–1.07 | 0.015–0.022 | 0.6% | 25–31 / 34–46 |
| Spike limit everywhere (`DENOISE_SPIKE_LIT_FRACTION=0`) | 0.81 | 0.0005 | 0% | 5 / 5 |
| Rectification on spatial variance only (`DENOISE_RECTIFY_TEMPORAL_WEIGHT=0`) | 0.86 | 0.0021 | 0.08% | 16 / 36 |
| The same, and the history may drop to nothing (`DENOISE_RECTIFY_MIN_LENGTH=0`) | 0.90 | 0.0010 | 0.04% | 10 / 12 |
| Maximum history 4 | 0.93 | 0.0054 | 0.22% | 12 / 24 |
| Full resolution | 1.09 | 0.021 | 0.77% | 15 / 34 |

Every denoiser-only variant that removes the trails also lowers the image in motion by 10–19%.

**The fix: an unshadowed bound from selection** (`DENOISE_BOUND`, on by default). Selection already scores every
candidate light by its unshadowed contribution. It also sums those scores in linear terms: listed and directional lights
in full, drawn hidden lights scaled by M / drawCount. The sum is the pixel's unshadowed light this frame, which is an
upper bound on what can reach the pixel. It travels in bits 16–29 of the pixel word (`stochasticSplit.hlsli`,
log2(1 + B) in 1/512 steps, rounded up). Shading writes it into the noisy diffuse input's alpha. The temporal denoiser
then scales the reprojected history down wherever its remodulated luminance exceeds `DENOISE_BOUND_MARGIN` (2) times the
largest bound in the 5×5 neighbourhood. Light that has left a pixel can't stay there. Light that is still there, lit or
shadowed, is never held below its unshadowed level.

- *Is the bound low anywhere?* No. The noisy input reaches 0.53× the bound at its median, 1.1× at p99 and 1.3× at
  p99.9; it exceeds 4× in 0.01% of pixels.
- *Does it darken converged light?* No. With frozen lights, margin 4 leaves the image as it was (0.968 against 0.969
  without the bound) and margin 1 lowers it by 1%.
- *Results in motion* (two repeats each, one session; *deficit* is the output's shortfall below the windowed input):

| Variant | Denoised / input | Excess | Deficit | Stuck energy | Stuck blocks | Age p95 / max |
|---|---|---|---|---|---|---|
| No bound | 1.00–1.06 | 0.26–0.31 | 0.26 | 0.021–0.027 | 0.6% | 21–31 / 33–42 |
| Bound, margin 2 | 0.83–0.86 | 0.15–0.17 | 0.31–0.33 | 0.005 | 0.4% | 21–22 / 37–38 |

  An earlier session measured margin 2 at stuck blocks 0.17% and stuck energy 0.003. Margins 1–1.5 did no better,
  and margin 4 left about twice the excess. Cutting the history where the bound binds was tried and dropped: it left
  fewer stuck blocks but more lag.
- *Cost.* None measurable. At 1024 lights, temporal 1.07 against 1.08 ms over two A/B pairs; selection unchanged.

**Arrival lag is small; the linear shortfall was hot spots.** Measured in linear units, the output in motion averaged
14–17% under the input once the bound removed the light lingering behind each light. Two checks show that this is not
lag:

- *A lag estimate that noise can't move.* For each block whose input changes by more than 2×, the detector finds the
  delay that best lines up the denoised series with the input's. It's about 1 frame (mean 0.8–1.3) in every variant.
- *Where the energy goes.* The shortfall sits in the 1–2% of blocks right beside the lights. There, demodulated values
  run past the fp16 history's 60000 cap, for example 65504 over a 0.2 albedo, and the spike limit doesn't account for
  it (switching it off changes little). Those pixels display white anyway. Making the excess and deficit *as displayed*
  (x / (1 + x) of the block means) makes the metric about what is seen; the linear ratio is still printed beside it.

As displayed:

| Variant | Displayed / input, moving | Excess | Deficit | Stuck blocks | Age p95 | Still lights, displayed |
|---|---|---|---|---|---|---|
| No bound | 1.04 | 0.127–0.132 | 0.088–0.090 | 0.7% | 17–23 | 0.991 |
| Bound (default) | 0.97–0.99 | 0.072–0.086 | 0.094–0.105 | 0.26–0.73% | 17–27 | 0.990 |
| Bound, scaled down at tolerance 8 (`DENOISE_BOUND_CHANGE=2`) | 0.93–0.94 | 0.050–0.052 | 0.114–0.117 | 0.14% | 11–12 | 0.983 |
| Bound, scaled down at tolerance 4 | 0.88–0.92 | 0.030–0.054 | 0.129–0.153 | 0.06–0.13% | 6–8 | 0.974 |

**Trails still left with the bound.** With frozen lights, the converged reference and the live image match, and neither
shows the lit bands that motion leaves along the lights' lanes. So the bands are history: the foliage (Zorah's own
virtualized geometry) a light at ground height passed through. Once the light has moved on, the ground and the other
blades shadow it, but the bound is unshadowed and stays high along the lane, so it holds the history only to 2× that. Comparing against the previous frame's bound doesn't
help either, because per frame the change is small.

`DENOISE_BOUND_CHANGE` therefore keeps, in the change history's x, the bound the history stands for: a running average
with the history's own weights. Comparing against that average catches the slow fall. Two modes, combinable:

- **Mode 2** scales the history down by the bound's fall beyond `DENOISE_BOUND_CHANGE_TOLERANCE`. The light that left
  takes its share with it, as long as visibility hasn't changed.
- **Mode 1** cuts the history to `DENOISE_MAX_HISTORY / r`. Alone, it had little effect.

The tolerance exists because the bound is noisy from frame to frame near hot spots: the sampled pixel of each 2×2 moves,
and a 5×5 maximum near a 1/d² peak swings by more than 2×. Every dip that gets scaled darkens the history for good. At
tolerance 1.25 the image fell to 0.85 with still lights (linear); at 2, to 0.89. Mode 2 at tolerance 8 cuts the excess
by a third and halves the stuck blocks and their age, but lowers the image 6% in motion and 0.7% with still lights. That
is a trade, not a clear gain, so it is off by default.

**Also tried.** Two attempts made no clear difference and were dropped:

- Judging brightening by the history's variance alone (deficit 0.31 → 0.30 linear).
- Letting rectification drop the history entirely (`DENOISE_RECTIFY_MIN_LENGTH=0`): it removed stuck blocks but raised
  both excess and deficit through noise.

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
`StochasticReferencePass`; `shaders/Stochastic/stochasticReference.hlsl`). For every light in the pixel's light
cluster, the sun included, it traces one ray to a fresh point on the light's sphere (or direction in its disc), and it keeps a running average per
pixel (RGBA32F, rgb the mean and a the count) while nothing changes. It restarts when the unjittered camera, the bytes
of the published light tables, or the BVH's instance count, cluster count or CLAS builds change. The light tables'
revision and the resident texture bytes are not used: both change every frame with nothing visible changing, which kept
the average from ever holding more than one frame. A texture that streams in sharper therefore does not restart it. It
is the target the estimator is checked against; it converges only while the camera and lights are still
(`ZORAH_FREEZE_LIGHTS=1`). Every 300 frames the log has a line `Stochastic lighting accumulation: N frames held, R
restarts (causes 0x..)`, the causes' bits being 1 camera, 2 lights, 4 BVH instances, 8 BVH clusters, 16 CLAS builds.

`STOCHASTIC_REFERENCE_PER_FRAME=1` outputs each frame's estimate instead of the running average (which keeps running), and
`STOCHASTIC_REFERENCE_RAY_GRID=g` traces a g × g stratified grid of rays per light: the jitter-consistent truth after DLSS
of "Noise after DLSS".

## Test scene

`Scripting/Scenarios/ZorahLights.cs`: Zorah with 16 moving shadowed point lights of range 18, viewed from overhead.

    BasicRenderer.exe --script <path>\ZorahLights.cs

with `SARP_RT_PATH_TRACING=0` (path tracing is the default where supported) and `SARP_STOCHASTIC_LIGHTING=1`.

| Variable | Effect |
|---|---|
| `ZORAH_FREEZE_LIGHTS=1` | Lights stay at their starting points, for references and A/B. |
| `ZORAH_LIGHT_SCALE=n` | n times as many lights (16n). |
| `ZORAH_LIGHT_RANGE=r` | Every point light's range, default 18. A shorter range keeps many lights from all reaching every pixel. |
| `ZORAH_LIGHT_INTENSITY=x` | Every point light's intensity times x (default 1: 250). From 256 lights the image saturates; x = 16 / lights keeps the point lights' total at 16 lights' (1/64 at 1024 lights). |
| `ZORAH_SOURCE_RADIUS=r` | Every light's source radius, for soft shadows. Default 0 (1 cm). |
| `ZORAH_KEEP_CAMERA=1` | Keep the demo's remembered camera instead of the overhead view. |
| `ZORAH_LIGHT_SCALE=0` | No point lights: the sun alone. |
| `ZORAH_CAMERA_T=t` | The camera held at fraction t of the fly loop (`ZORAH_CAMERA_MOTION`'s path). |

## Files

| Path | Contents |
|---|---|
| `src/Lighting/Stochastic/StochasticLightingExtension.*` | The graph extension that places the passes before `DeferredShadingPass`. |
| `src/Lighting/Stochastic/StochasticLightingResources.h` | What the passes share across frames: visible lists, surface history, the denoiser's textures. |
| `src/Lighting/Stochastic/RenderPasses/StochasticSamplePass.h` | The pass for the estimator's three kernels (`StochasticSampleKernel`) and the reference (`StochasticLightingPassMode`), with the running average. |
| `src/Lighting/Stochastic/RenderPasses/StochasticDenoisePass.h` | The denoiser's temporal and spatial passes (`StochasticDenoiseStage`). |
| `shaders/Include/stochasticLighting.hlsli` | Surface reconstruction, unshadowed light evaluation, the shadow ray. |
| `shaders/PerPassRootConstants/stochasticLightingRootConstants.h` | Root constant slots (shares the secondary rays' TLAS slots), and the reduced-resolution sampling pattern the estimator and denoiser share. |
| `shaders/Stochastic/` | The passes' shaders: `stochasticSelect.hlsl`, `stochasticTrace.hlsl` and `stochasticShade.hlsl` (the estimator's three kernels), `stochasticReference.hlsl`, `stochasticDenoise.hlsl`. |
| `shaders/Include/stochasticSplit.hlsli` | What the estimator's kernels pass between them: sample slots, pixel words, the ray list's entries, the shared-exponent radiance packing. |
| `scripts/stochastic_scaling.sh`, `scripts/stochastic_scaling_analysis.py` | The benchmark runner (timings, bias, noise, noise after DLSS) and its analysis ("Scaling", "Noise after DLSS"). |
| `config/stochastic_scaling.json` | The runner's sampling config: the stochastic passes, deferred shading, the light grid's three passes. |
| `config/stochastic_sample_nvperf.json` | NVPerf counters for the estimator's passes (`CONFIG=` for the runner). |
