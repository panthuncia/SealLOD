# DeferredShadingPass optimization experiments

This log records shader experiments for `DeferredShadingPass` in the settled
BasicRenderer demo scene. Measurements use the Clang RelWithDebInfo build,
target the compute-queue `DeferredShadingPass`, and switch shader generations
through the persistent profiler's PSO hot-reload path. Unless noted otherwise,
each timing is the converged mean of at least 80 accepted frames.

## Initial profile

The original targeted NVPerf capture measured:

- 4.10624 ms settled render-graph time.
- 453.83 million instructions: 185.92 million FMA, 146.83 million ALU,
  and 23.01 million TEX instructions.
- 248.12 registers per thread.
- Long scoreboard was the dominant issue stall at 0.5069 cycles per active
  warp, followed by wait at 0.1811 and short scoreboard at 0.0809.
- SM throughput was 26.4%, L1/TEX 19.5%, and DRAM 15.6%. The pass was
  instruction/latency and occupancy limited rather than bandwidth saturated.

## Retained experiments

### Specialized perspective reconstruction

- Change: reconstruct the infinite reverse-Z perspective view ray directly
  from the two inverse-projection diagonal elements, and transform the
  normalized view-space ray to world space for the view direction.
- Result: 4.10624 -> 4.09102 ms (-0.37%).
- Decision: retained. This removes a general 4x4 inverse-projection multiply
  and redundant world-space subtraction without changing the renderer's
  projection contract.

### Zero-presence OpenPBR coat and fuzz paths

- Change: return identity/zero states before fuzz basis construction, coat
  darkening, dielectric-energy lookup, and coat passage evaluation.
- Result: 4.09102 -> 4.06162 -> 3.82023 ms as the construction and downstream
  identity paths were added.
- Decision: retained. Zero coat/fuzz presence is common in the scene and the
  bypasses are algebraic identity cases.

### Zero-emissive OpenPBR setup

- Change: do not construct a second base/coat state when emissive is exactly
  zero.
- Result: 4.06162 -> 3.98807 ms.
- Decision: retained.

### Zero-fuzz IBL cubemap lookup

- Change: skip the additional prefiltered-environment sample when fuzz weight
  is zero.
- Result in the continuation process: 3.83705 -> 3.82487 ms (-0.32%).
- Decision: retained.

### Fully shadowed direct lights

- Change: after shadow evaluation, skip light-vector setup and the direct
  OpenPBR BRDF when shadow is exactly 1.0.
- Result: 3.82487 -> 3.56501 ms (-6.79% incremental).
- Confirmation: 3.56533 ms, 95% CI [3.56482, 3.56584].
- Decision: retained. Full occlusion makes the complete direct-light term
  exactly zero.

## Rejected or neutral experiments

### Remove deferred debug-output work

- Change: compile a color-only variant without the debug payload path while
  preserving the descriptor interface.
- Result: 4.10594 -> 4.11447 ms.
- Decision: rejected and reverted. DXC already isolates the unused debug path;
  the explicit variant slightly regressed timing.

### Reuse the passed main camera for cluster selection

- Change: use the `mainCamera` argument rather than loading the same camera
  again from the descriptor buffer in `ComputeClusterID`.
- Result: 4.10594 -> 4.10540 ms.
- Decision: neutral and reverted.

### Branch around zero-weight diffuse/dielectric/metal lobes

- Change: dynamically skip individual OpenPBR base lobes when their color or
  weight was exactly zero.
- Result: 3.56501 -> 3.61423 ms (+1.38%).
- Decision: rejected and reverted. Divergence and branch overhead outweighed
  the arithmetic saved in the mixed deferred screen.

### Hoist prepared OpenPBR state across light traversal

- Change: build base, coat, and fuzz states once per fragment and retain them
  across VSM shadow traversal for reuse by visible lights.
- Result: 3.56527 -> 3.58798 ms (+0.64%).
- Decision: rejected and reverted. Longer live ranges hurt the already
  register-heavy shader; DXC was already simplifying enough invariant setup.

### Range-cull punctual lights before shadow evaluation

- Change: calculate light distance and reject out-of-range point/spot lights
  before sampling shadows.
- Result: 3.56527 -> 3.61302 ms (+1.34%).
- Decision: rejected and reverted. Cluster membership already made range
  rejection uncommon, while the reordered light-vector work ran for fully
  shadowed lights that the retained path rejects first.

## Continuation round: no retained changes

The next persistent-process run established a fresh A-side baseline of
3.54465 ms, 95% CI [3.54418, 3.54512]. After all candidates below were
reverted, the final reconfirmation was 3.54564 ms, 95% CI
[3.54514, 3.54613]. The small difference is inter-experiment process drift;
no candidate from this round was retained.

### Build `LightingParameters` only after shadow visibility

- Change: move construction of the large direct-light parameter aggregate to
  the visible-light call site so fully shadowed pixels do not keep it live
  across VSM traversal.
- Result: 3.54465 -> 3.54476 ms.
- Decision: neutral and reverted. DXC already scalarizes the aggregate and
  treats its assignments as aliases.

### Split VSM debug and normal output paths

- Change: call the non-debug directional VSM wrapper for normal output modes
  and the detailed function only for VSM visualization modes.
- Result: 3.54465 -> 3.54790 ms (+0.09%).
- Decision: rejected and reverted. Unused debug writes are already eliminated
  effectively; the uniform mode branch and duplicated call path add overhead.

### Reuse the normalized directional-light vector

- Change: use the CPU-normalized light direction directly when constructing
  the SMRT basis instead of normalizing it again per pixel.
- Result: 3.54465 -> 3.55081 ms (+0.17%).
- Decision: rejected and reverted. The explicit normalize produces better
  generated scheduling or numerical behavior on this shader.

### Reuse receiver lookup clipmap data

- Change: use `receiverLookup.clipmapInfo` instead of reloading the sampled
  clipmap record from its structured buffer.
- Result against the A2 baseline: 3.54511 -> 3.55085 ms (+0.16%).
- Decision: rejected and reverted. Keeping the larger returned record live is
  more expensive than the cached structured-buffer load.

### Pair SMRT sine and cosine

- Change: replace separate `sin` and `cos` expressions with HLSL `sincos`.
- Result: 3.54473 ms versus the 3.54511 ms A-side confirmation, with
  overlapping confidence intervals.
- Decision: neutral and reverted. DXC/NVIDIA already combines or schedules the
  two forms equivalently.

### Keep the cluster ID integer

- Change: retain the `uint3` returned by `ComputeClusterID` instead of
  converting it to `float3` and back while flattening the index.
- Result: 3.54543 ms versus 3.54511 ms.
- Decision: neutral/slightly negative and reverted. The conversions are
  already optimized away or hidden.

### Algebraically reduce logarithmic cluster slicing

- Change: replace three logarithms and subtracts with
  `log(z / zSplit) / log(zFar / zSplit)`.
- Result: 3.54511 -> 3.55572 ms (+0.30%).
- Decision: rejected and reverted. Small floating-point boundary changes alter
  cluster assignment and increase downstream light work despite fewer
  arithmetic instructions.

### Wave-broadcast cluster log constants

- Change: preserve the original cluster formula but compute `logStart` and
  `logEnd` once in the first active lane and broadcast them.
- Result: 3.54511 -> 3.56933 ms (+0.68%).
- Decision: rejected and reverted. Wave control/shuffle overhead and
  partial-wave behavior outweigh two uniform logarithms.

### Skip metal-average Fresnel preprocessing for zero metal weight

- Change: branch around `OpenPBRMetalAverageFresnelWithF82Tint` when the exact
  metal specular weight is zero.
- Result: 3.54511 -> 3.55889 ms (+0.39%).
- Decision: rejected and reverted. Material divergence costs more than the
  saved vector arithmetic.

### Skip the fuzz G-buffer load for zero fuzz weight

- Change: load metallic/roughness first and conditionally fetch the separate
  fuzz texture only when its stored fuzz weight is nonzero.
- Result: 3.54511 -> 3.60119 ms (+1.58%).
- Decision: rejected and reverted. Divergent descriptor access and reduced
  memory-latency hiding are substantially worse than the saved texture fetch.

### Evaluate IBL after direct lighting

- Change: move additive IBL evaluation after VSM/direct lighting to shorten
  the apparent lifetime of IBL debug results across the shadow loop.
- Result: 3.54511 -> 3.58800 ms (+1.21%).
- Decision: rejected and reverted. The original ordering provides materially
  better texture-latency hiding.

### Use the receiver lookup's sampled clipmap index

- Change: use `receiverLookup.sampledClipmapIndex` rather than the equivalent
  scalar already copied into VSM debug info.
- Result: 3.54511 -> 3.55460 ms (+0.27%).
- Decision: rejected and reverted. Reaching back into the larger lookup result
  extends an unfavorable live range.

### Wave-broadcast blue-noise dimensions

- Change: issue `Texture2D::GetDimensions` only in the first active lane and
  broadcast the two dimensions.
- Result: 3.54511 -> 3.56563 ms (+0.58%).
- Decision: rejected and reverted. The uniform texture query is already
  efficient; wave branching and shuffles add overhead.

## Current counter result

After the retained changes above, the targeted replay measured:

- 3.64651 ms replay duration and 369.65 million instructions.
- 124.51 million ALU, 138.52 million FMA, and 20.92 million TEX instructions.
- 240.16 registers per thread.
- Long scoreboard remained dominant at 0.4766 cycles per active warp, followed
  by wait at 0.1589 and short scoreboard at 0.0718.

Relative to the original counter capture, this is an 18.5% instruction
reduction, including 25.5% fewer FMA and 9.1% fewer TEX instructions. The
settled render-graph timing improved from 4.10624 to 3.56533 ms (-13.2%).

## Saved-camera MO2 run: receiver camera-depth trace

This run used world `0000003C`, radius 100, the saved camera, and one persistent
MO2-launched renderer host (PID 20132). A one-sample NVPerf validation measured
7.01248 ms before switching the same control workflow to render-graph timestamp
statistics for fast iteration.

### Reuse projection W as camera-space depth

- Change: in `CLodVirtualShadowReceiverTraceEscapedAt`, reuse `sampleClip.w` as
  `-view.z` instead of applying a second world-to-view matrix multiply. The
  camera projection contract guarantees those values are equal. Also remove an
  unreachable explicit `0xFFFFFFFF` depth-bit comparison; the NaN already fails
  `depth > 0`.
- Initial baseline: 8.16729 ms (120 samples; noisy maximum-sample stop).
- Candidate: 7.69049 ms (56 samples; converged).
- Candidate replication: 7.58238 ms (46 samples; converged).
- Control replication after reactivating generation 1: 7.95151 ms (87 samples;
  converged).
- Same-process one-sample NVPerf check: 8.19184 ms control versus 7.38240 ms
  candidate (-0.80944 ms / -9.88%).
- Decision: retained. Candidate replication is 0.36913 ms / 4.64% faster than
  the interleaved control replication; the first comparison was 5.84% faster.

### Reuse projected light depth

- Change: consume the already-interpolated preferred light depth and the depth
  returned by fallback projection instead of explicitly transforming to light
  view space again.
- Result: 7.69315 ms (42 samples; converged), effectively unchanged from the
  preceding 7.69049 ms result.
- Decision: rejected and reverted; the shader compiler already eliminated the
  redundant work.

### Increment camera receiver tracing in clip space

- Change: project the receiver and trace direction once, then form each trace
  and binary-refinement point with a clip-space multiply-add.
- Result: 7.71678 ms (84 samples; converged).
- Decision: rejected and reverted. The extra live clip vectors likely offset
  the saved matrix work through register pressure.

## Projected lookup follow-up

All results below used the same persistent MO2 host (PID 20132), with generation
switches through the live PSO controller.

### Reuse validated receiver-page state

- Change: pass the receiver lookup's sampled clipmap and physical-page indices
  into projected SMRT lookups. Samples resolving to that exact page skip
  duplicate ownership/tag validation already completed earlier in the same
  shader invocation.
- Result: 7.58048 -> 7.50159 ms (-1.04%).
- Decision: retained.

### Remove duplicate projected-path metadata reads

- Change: projected lookups rely on the sampleable page-table state plus the
  cached absolute-page tag, rather than rereading the physical ownership
  metadata for every ray sample. The authoritative receiver lookup retains the
  complete resident/owner/layer validation.
- Result: 7.50159 -> 7.43495 ms (-0.89%).
- Decision: retained.

### Hoist preferred clipmap and camera descriptors

- Change: pass the already-loaded preferred clipmap info and compact shadow
  camera into `CLodVirtualShadowLookupDirectionalOcclusionProjected`. Attempt
  zero no longer reloads two large structured-buffer records for every SMRT
  sample; fallback attempts preserve the original loads and behavior.
- Statistical result: 7.41960 ms pre-hoist control replication versus 4.77577
  ms final candidate (84 samples, converged), -2.64383 ms / -35.63%.
- Same-process one-sample NVPerf result: 7.765248 ms control versus 5.181056 ms
  final candidate, -2.584192 ms / -33.28%.
- Decision: retained.

### Rejected coverage/address experiments

- Returning immediately for a valid cleared receiver texel measured 7.71226 ms
  and was reverted. The expensive lit pixels generally contain finite
  receiver/self depth, so this did not identify the actual no-coverage case.
- A 128-page-wide atlas bit-address specialization measured 7.49932 ms versus
  7.50159 ms and was removed because it was neutral and affected unrelated
  shader paths.

## SMRT projected-page coherence (2026-09-04)

- Hoisted the adaptive receiver screen-trace's ray-invariant camera-depth
  validation and trace-spacing setup out of the per-ray loop. Ray directions
  produced by SMRT are already normalized, so the redundant normalization was
  also removed.
- Added a per-ray projected-page cache. Consecutive SMRT samples in the same
  virtual page now reuse the validated page entry, physical-page index, and
  cached page-view row. This bypasses toroidal wrapping, the page-table load,
  absolute-page-tag packing/validation, and the page-view-info load.
- Same-process render-graph timing, generation 1 versus generation 3:
  4.880869 ms (30 samples) to 4.707767 ms (97 samples), a 3.55% reduction.
- Same-process single-sample NVPerf validation:
  5.142560 ms to 4.506496 ms, a 12.37% reduction. The statistical timing is
  the reliable estimator; the NVPerf result confirms the candidate remains a
  strong improvement under counter collection.
- Receiver-hoist-only generation 2 measured 4.910955 ms, so it was not a win
  in isolation. The retained combined form is justified by the page-cache
  result and should be reconsidered independently if the cache layout changes.
