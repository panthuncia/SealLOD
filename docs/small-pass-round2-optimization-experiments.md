# Small-pass optimization, round 2

## Baseline and method

The round began with new frame-1800 NVPerf captures of all direct and compute
ranges, followed by 80-160-sample render-graph timestamp experiments on a settled
scene. The broad capture identified these underexamined costs:

| Work | Baseline |
|---|---:|
| Tonemapping plus separate bloom composition | approximately 287 us |
| `LinearDepthHistoryCopyPass` | 135.0 us |
| `ClearVisibilityBufferPass` | 121.7 us |
| Phase-1 depth copy plus downsample | 207.3 us |
| `SkyboxPass` | 60.6 us |
| GTAO filter plus denoise | 157.9 us |

The profiler configuration is `config/small_passes_round2_sampling.json`. The
final timestamp repeat accepted 160 samples. Full-range captures are
`out/full_pass_profile_direct_round2_verified_20260726.csv` and
`out/full_pass_profile_compute_round2_final_20260726.csv`.

## Retained changes

### Fuse bloom composition into tonemapping

Tonemapping now reads the already-filtered bloom mip 1 and accumulated mip 2
while it reads HDR, applies the original 4% blend, and writes the backbuffer.

An initial eight-fetch fused reconstruction measured 218.3 us. Reducing it to
one hardware-bilinear sample from each mip measured 176.5 us. Relative to the
approximately 287 us separate pair, this saves about 110 us.

### Remove the redundant phase-1 projected-depth store

Both depth-copy phases must rebuild linear depth, but only phase 2 must publish
projected depth.

| Pass | Baseline | Final | Change |
|---|---:|---:|---:|
| `CLodOpaque::LinearDepthCopyPass1` | 137.9 us | 95.2 us | -42.7 us |
| `CLodOpaque::LinearDepthDownsamplePass1` | 69.4 us | 41.7 us | -27.8 us |

The downsample improvement is attributed to lower preceding DRAM pressure.

### Render sky in DeferredShading's background branch

DeferredShading already reads final linear depth for every pixel and returned
for the no-geometry sentinel. That branch now samples the environment and writes
background HDR and motion vectors. The standalone 60.6 us `SkyboxPass` is gone.

The settled DeferredShading result was 3.577 ms, versus approximately 3.606 ms
for the prior DeferredShading plus Skybox pair, saving about 29 us.

### Remove overwritten clears

HDR, motion vectors, and linear-depth mips are fully produced before their first
read, so their initialization clears were removed. G-buffer clears are retained
for diagnostic modes but skipped for normal color rendering. The debug-payload
sentinel remains unconditional.

`ClearVisibilityBufferPass` fell from 121.7 to 66.4 us, saving 55.3 us.

### Reuse current linear depth as one-frame history

After phase-2 downsampling, the current depth pyramid remains unchanged until the
next frame's phase-1 cull. Phase 1 now reads that resource before rasterization
overwrites it. `LinearDepthHistoryCopyPass` remains as a dependency and validity
marker but performs no texture copy.

| Pass | Baseline | Final | Change |
|---|---:|---:|---:|
| `LinearDepthHistoryCopyPass` | 135.0 us | 1.63 us | -133.4 us |

The final sampler included both hierarchical culling phases and completed 160
accepted settled samples with stable workload-defining state.

## Rejected and neutral work

### Skip the debug-sentinel clear

Skipping the debug clear left stale payloads. `DebugResolvePass` increased to
169 us from roughly 28-33 us and could produce incorrect color output. The clear
was restored unconditionally; the verified capture measured DebugResolve at
32.7 us.

### Reuse the general HZB as XeGTAO working depth

Investigated but not implemented. The renderer's HZB uses max-depth reduction
for conservative occlusion, while XeGTAO constructs a view-space pyramid with
its own filtering. Substitution would change AO behavior.

### Remove either depth-copy phase

Rejected during mapping. Phase 1 feeds phase-2 occlusion replay; phase 2 provides
final depth for shading, GTAO, and the next frame.

## Aggregate result

The retained round-2 changes remove about 390 us of serialized pass work:

- approximately 110 us from tonemap/bloom fusion;
- approximately 70 us from phase-1 depth output reduction;
- approximately 29 us from skybox fusion;
- approximately 55 us from dead/conditional clears;
- approximately 133 us from in-place depth history;
- approximately 9 us of downstream GTAO/cache regression.

Direct and compute queues overlap, so end-to-end reduction depends on the
critical queue. The verified all-range capture found no displaced
debug-resolve cost after restoring the sentinel clear.
