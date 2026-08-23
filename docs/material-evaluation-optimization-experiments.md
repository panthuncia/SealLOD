# Material-evaluation optimization experiments

Measurements use `EvaluateMaterialGroupsPass` in the persistent sampling
process, 95% confidence intervals, and 80--160 accepted frames. Shader
experiments were switched through live PSO generations without restarting the
process unless the buffer layout itself had to change.

## Baseline

- Original 64-thread pass: 1.4106 ms, with an A2 rollback at 1.4140 ms.
- Hardware trace: 3.83 active warps/cycle, 132.3 million instructions,
  22.7% SM throughput, 25.3% L1/TEX, 30.0% L2, and 49.5% DRAM throughput.
- Long-scoreboard stalls were 0.759 cycles per active warp. Register-allocation
  launch stalls were also substantial (113 million cycles), although this
  driver's registers-per-thread metric returned zero and is not usable.

## Rejected or neutral experiments

### 32-thread material groups

- Result: 1.4253 ms versus 1.4106/1.4140 ms A--B--A controls.
- Decision: rejected. Halving the group size regressed the pass by about 0.9%.

### 128-thread material groups

- Result: 1.4105 ms.
- Decision: neutral. It did not improve on the first baseline and was only
  about 0.25% below the later control, below the retention threshold.

### Sink G-buffer UAV handles below material resolution

- Change: moved creation of the seven output UAV handles after the expensive
  material-resolution call.
- Result: 1.4132 ms.
- Decision: neutral and reverted. DXC already schedules the handles/lifetimes
  effectively.

### Suppress unused color-only resolved-sample outputs

- Change: explicitly skipped position/debug/common-sample stores that are not
  consumed by the color-only material-evaluation variant.
- Result: 1.4125 ms.
- Decision: neutral and reverted. Inlining and dead-store elimination already
  remove these values.

## Retained: cache the visibility key in the pixel list

`BuildPixelListCS` already loads the 64-bit visibility key. `PixelRef` now
carries that key with the packed pixel coordinate so material evaluation can
avoid a later random visibility-texture load.

- Wider-record control (still reloading the texture): 1.4574 ms.
- Cached-key candidate: 1.3837 ms.
- A2 wider-record control: 1.4616 ms.
- Cached-key confirmation: 1.3900 ms.
- Within-process improvement: 4.9--5.1%.
- Compared with the original narrow-record pass, the retained result improves
  material evaluation by approximately 1.5--2.2%.

The wider record's cost to pixel-list construction and total material pipeline
still needs a dedicated full-pipeline measurement; retain only while that
combined result remains positive.

Subsequent combined measurement put the retained 12-byte pixel record at
0.2180 ms for `BuildPixelListPass` and 1.3838--1.3899 ms for evaluation.

### Compute the motion vector before material sampling

- Change: computed the final two-component motion vector immediately after
  world-position reconstruction, intending to release previous-position,
  transform, and camera state before texture sampling.
- Result: 1.5967 ms versus 1.3838/1.3899 ms A--B--A controls.
- Decision: rejected and reverted. Keeping the result live across sampling
  increased pressure more than releasing its inputs helped.

### Explicit rigid previous-position alias

- Change: for non-skinned variants, replaced the three previous triangle
  positions and their interpolation with `previousPosOS = posOS`.
- Result: 1.3862 ms versus a 1.3899 ms adjacent control and 1.3838 ms initial
  control.
- Decision: neutral and reverted. DXC already folds the rigid path.

### Cache the full visible-cluster record

- Change: appended the 16-byte packed visible-cluster record to `PixelRef` and
  consumed it directly in color evaluation.
- Wider-record control: 0.3979 ms pixel-list construction plus 1.4626 ms
  evaluation.
- Cached-record candidate: 0.3978 ms plus 1.4346 ms.
- Decision: rejected and reverted. Avoiding the random 16-byte load saved only
  1.9% in evaluation, while the larger sequential record nearly doubled
  producer time and increased the combined measured work by about 16%.

### Explicit wave32 and 256-thread groups

- Forced wave32: 1.3808 ms versus 1.3693 ms before the change and 1.3817 ms
  after rollback; no improvement after accounting for clock drift.
- 256-thread groups: 1.3715 ms. The adjacent controls ranged from 1.3693 to
  1.3872 ms as clocks moved, while the evaluation/pixel-list ratio was
  unchanged.
- Decision: rejected. Native wave selection and the existing 64-thread group
  remain the best tested choices.

### Compact color-result structure

- Change: flattened only the 25 color-output scalars into a compact result
  instead of returning the full `MaterialInputs` aggregate.
- Result: 1.3818 ms, matching the adjacent 1.3817 ms control.
- Decision: neutral and reverted. DXC successfully eliminates the unused
  aggregate fields.

### Reduce the UV-cache capacity

- Change: diagnostic builds reduced the maximum unique UV sets from eight to
  four and then two, to test whether the large UV sample/binding arrays set the
  register peak.
- Results: 1.3767 ms at four and 1.3725 ms at two. Normalizing to the
  simultaneously measured pixel-list pass produced no improvement over the
  eight-entry controls.
- Decision: rejected and reverted. The compiler appears to scalarize or
  eliminate unused UV-cache entries; a hard capacity reduction would also be
  unsafe for general materials.

### Immediately consume texture-sample results

- Change: directly accumulated base color and opacity samples and nested
  metallic, roughness, normal, and AO sample consumption into their final
  calculations, intending to shorten the lifetime of intermediate `float4`
  sample results.
- A1 control: 0.21829 ms pixel-list construction and 1.38829 ms material
  evaluation over 80 accepted frames.
- Candidate: 0.21818 ms pixel-list construction and 1.39241 ms material
  evaluation over 84 accepted frames.
- A2 control: 0.22280 ms pixel-list construction and 1.39537 ms material
  evaluation over 160 accepted frames. This interval encountered a noisy
  clock/load excursion; pixel-list timing had a 2.58% relative half-width.
- Decision: neutral/rejected and reverted. The candidate regressed 0.30%
  against the clean A1 control and did not establish a reproducible
  improvement after accounting for the noisy A2 interval. DXC already keeps
  the named sample-result lifetimes sufficiently short.

### Skip geometric-height debug sampling in color-only evaluation

- Change: returned zero instead of calling
  `SampleMaterialGeometricHeightDebug` in color-only material PSOs.
- Clean A1 control: 0.21674 ms pixel-list construction and 1.37745 ms material
  evaluation over 80 accepted frames.
- Candidate confirmation: 0.21664 ms pixel-list construction and 1.38147 ms
  material evaluation over 80 accepted frames.
- Decision: neutral/rejected and reverted. The active workload's material
  shader keys contain no height-texture feature, so the guarded sampling path
  is not hot. An initially favorable normalized result occurred during a noisy
  clock interval and did not reproduce.

### Compile selected-mip debug tracking out of color-only texture samples

- Change: made `ShouldTrackMaterialSelectedMipDebug` return compile-time false
  for color-only material PSOs.
- Candidate: 0.22045 ms pixel-list construction and 1.39301 ms material
  evaluation over 160 accepted frames.
- A2 control: 0.22039 ms pixel-list construction and 1.38996 ms material
  evaluation over 160 accepted frames.
- Decision: neutral/rejected and reverted. The normalized timings matched or
  slightly favored the control. The active workload has no texture-feature
  material bins, so the per-texture branch is not exercised.

## Retained: compile-time gate for material UV reconstruction

Color-only, non-triplanar materials previously built `MaterialUvCache` and
`MaterialUvBindings` even when the specialized PSO contained no feature that
could consume UVs. The retained path derives a compile-time
`hasCompiledMaterialUvConsumer` value from the texture, normal, parallax, and
OpenPBR feature macros. UV decode/cache/binding work now runs only for debug
output, object-space normal-map handling, or a compiled UV-consuming feature.

- Clean A1 control: 0.21674 ms pixel-list construction and 1.37745 ms material
  evaluation over 80 accepted frames.
- Candidate B1: 0.22247 ms pixel-list construction and 1.36997 ms material
  evaluation over 160 accepted frames. The pixel-list interval was noisy, but
  its evaluation/pixel-list ratio improved about 2.4%.
- A2 control: 0.22463 ms pixel-list construction and 1.39401 ms material
  evaluation over 160 accepted frames; pixel-list timing was again noisy.
- Candidate B2 confirmation: 0.21670 ms pixel-list construction and 1.35556 ms
  material evaluation over 102 accepted frames.
- Decision: retained. Against the clean, clock-matched A1 control, confirmation
  improved material evaluation by about 1.6% with unchanged pixel-list cost.

NvPerf A--B--A captures used three accepted samples and 46 replay passes per
sample. The most stable differentiator was instruction count:

- Retained A1: 122.05 million instructions.
- Fallback B: 135.39 million instructions.
- Retained A2: 122.02 million instructions.
- The retained gate therefore removes about 9.9% of executed instructions.

NvPerf replay duration was 1.591/1.414 ms for the two retained captures versus
1.512 ms for the fallback and was too variable to use as the primary timing
result. Register-allocation launch stalls were likewise noisy at 101.5/89.4
million cycles retained versus 91.4 million fallback. Active warps were
3.82/3.69 retained versus 4.01 fallback. Long-scoreboard stalls were
0.777/0.800 retained versus 0.686 fallback. The throughput percentages moved
with replay duration: retained captures spanned 22.0--23.7% SM,
27.2--29.4% L1/TEX, 26.2--27.9% L2, and 49.3--56.9% DRAM; the fallback reported
27.3%, 30.4%, 28.3%, and 44.8%, respectively. Registers/thread remained zero
and unusable on this driver.

The current scene's active material-evaluation shader keys have no texture
feature bits. UV0-only, packed-binding, and channel-layout specializations
therefore cannot be meaningfully evaluated on this workload; they remain
future candidates for a texture-heavy benchmark rather than being recorded as
rejected.
