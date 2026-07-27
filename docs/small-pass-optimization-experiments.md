# Small-pass optimization experiments

## Scope and methodology

This investigation targeted material bookkeeping, exposure, and bloom passes.
Measurements use the persistent profiler on the settled BasicRenderer scene,
with 80-160 timestamp samples per experiment.

The narrow initial baseline was:

| Pass | Initial GPU time |
|---|---:|
| `MaterialHistogramPass` | 130.2 us |
| `BuildPixelListPass` | 264.2 us |
| `luminanceHistogramPass` | 184.8 us |
| `BloomUpsampleAndBlendPass` | 150.5 us |

Material reset, scans, offsets, and indirect-command construction together cost
only about 14 us, so optimization effort remained on the full-resolution passes.

## Retained changes

### Luminance histogram

- Sample one representative pixel per 4x4 footprint.
- Weight each contribution by its exact footprint population, including partial
  footprints at image edges.
- Preserve wave-level bin coalescing and include footprint weight in the key.
- Dispatch only the sampled extent.
- Replace the average pass's eight-barrier shared-memory reduction with a
  32-lane wave reduction and one group barrier.

| Pass | Final GPU time | Change |
|---|---:|---:|
| `luminanceHistogramPass` | 50.7 us | -134.1 us (-72.6%) |
| `LuminanceAveragePass` | 6.9 us | approximately -1.3 us |

The histogram population and exposure normalization remain correct. The spatial
distribution is intentionally estimated rather than exhaustively enumerated.

### Bloom pyramid and composition

- Replace the 13-fetch downsample kernel with four bilinear taps.
- Replace the nine-fetch pyramid upsample kernel with four bilinear taps.
- Fold `BloomUpsamplePass1` and the final HDR blend into tonemapping.
- Sample the already-filtered mip 1 and accumulated mip 2 once each while
  tonemapping already owns the HDR read and backbuffer write.
- Preserve the original equation, `0.96 * HDR + 0.04 * bloom`.

Approximate settled timestamp measurements:

| Work | Baseline | Final/effective | Change |
|---|---:|---:|---:|
| Bloom downsample/upsample pyramid | 201.8 us | approximately 139.2 us | -62.6 us |
| Separate final bloom blend | 150.5 us | removed | -150.5 us |
| Added work in fused tonemapping | 0 us | approximately 27.7 us | +27.7 us |
| Effective bloom cost | 352.4 us | approximately 166.9 us | -185.5 us |

The earlier reported 9.9 us fixed-function blend was invalid: the executable had
not been relinked after the C++ PSO change, so the shader ran against the old
zero-render-target PSO and did not write bloom. A correctly linked
fixed-function pass measured 138.4 us and was superseded by tonemapping fusion.

### Material bookkeeping findings

The retained shaders remain essentially unchanged:

| Pass | Initial | Final repeat |
|---|---:|---:|
| `MaterialHistogramPass` | 130.2 us | 129.8 us |
| `BuildPixelListPass` | 264.2 us | 265.2 us |

Both are dominated by a full-resolution visibility read followed by
cluster/instance/mesh indirection. `BuildPixelListPass` also performs grouped
cursor atomics and writes a 12-byte `PixelRef`.

## Rejected or neutral experiments

### Luminance group-shared histogram

A 256-entry group-shared histogram measured 278.0 us versus 184.8 us. Shared
initialization, barriers, atomics, and the final flush outweighed fewer global
atomics. Reverted.

### BuildPixelList wave specialization

Forcing 32-lane waves and explicit mask/leader/rank logic measured 264.25 us
versus 264.19 us. Neutral; reverted.

### Leader-only material-offset load

Loading `materialOffset[matId]` only in each material group's leader measured
264.24 us. Neutral; reverted.

### Cross-pass material-ID cache

| Pass | Baseline | Cached ID |
|---|---:|---:|
| `MaterialHistogramPass` | 130.2 us | 145.6 us |
| `BuildPixelListPass` | 264.2 us | 293.7 us |

The pair regressed by 44.9 us because the cache traffic cost more than the
repeated indirection. Reverted.

### Final bloom single fetch on the UAV path

Reducing four bloom fetches to one increased the UAV blend from about 105.7 to
109.9 us, establishing that the HDR UAV read/modify/write was the bottleneck.
Reverted.

### Standalone fixed-function bloom blend

The first 9.9 us result was invalid because the executable predated the C++ PSO.
The correctly linked pass measured 138.4 us. It was valid but retained a
redundant full-resolution pass, so tonemapping fusion replaced it.

## Aggregate result

Exposure saves approximately 135 us and the final bloom implementation
approximately 186 us, for about 321 us less serialized GPU work in the targeted
passes. End-to-end frame-time benefit can be smaller when compute and direct
queues overlap. Functionality is retained; exposure uses spatially weighted
sampling and bloom uses bilinear approximations of the wider original kernels.
