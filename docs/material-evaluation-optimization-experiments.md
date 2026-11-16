# Material evaluation optimization experiments

## Object POM distance and debug-height gating (2026-09-04)

- Baseline `EvaluateMaterialGroupsPass`: 5.172462 ms over 70 samples.
- Added an object-POM distance test matching the existing terrain fade range.
  Pixels beyond `heightFadeEndDistance` now skip camera-vector normalization,
  TBN construction, and the POM texture walk. Pixels in the fade interval use
  a faded height scale and proportionally reduced step count.
- POM candidate: 4.851859 ms over 38 samples, 6.20% below baseline.
- Restricted `SampleMaterialGeometricHeightDebug` and the terrain RVT
  geometric-height lookup to `OUTPUT_TERRAIN_GEOMETRIC_HEIGHT`; these values
  have no normal shading consumer.
- Combined candidate: 4.767046 ms over 88 samples, 7.84% below baseline.
- All 50 live material-evaluation PSO specializations were compiled and
  published in the same MO2-launched process. Generation 3 is the retained
  combined candidate for every specialization.
- The first single NVPerf candidate sample was noisy/order-sensitive. An
  immediate baseline/candidate repeat measured 4.804960 ms versus 4.421888 ms,
  a 7.97% reduction consistent with the statistical result. The multi-sample
  render-graph timing remains the reliable estimator.

## Register pressure and the dependent-load chain (2026-11-14)

Zorah (ZorahLights, still overhead camera, lights frozen, `SARP_TEXTURE_FEEDBACK_FORCE_MIP=4`, `clodLodErrorPixels` 3).
`EvaluateMaterialGroupsPass`, render-graph timer, same session with the old and new shaders hot-swapped: **7.87 ms -> 3.74 ms**.
NVPerf (`tpc__average_registers_per_thread_shader_cs`): 244 -> ~128 registers a thread; warp launches stalled on register
allocation 54% -> ~25%. HDR captures of old and new differ by no more than two frames of the same shader do.

What held registers or latency, and the change for each:

| Cause | Change | Effect |
|---|---|---|
| The object triplanar path compiled into every textured variant (18 samples a texture: 82 sample ops for four textures) | `MaterialCompileObjectTriplanar` -> `PSO_OBJECT_TRIPLANAR` | 10.2k -> 5.9k DXIL instructions; registers unchanged on its own |
| The UV cache read and written through run-time indices (50 `[8 x float]` allocas) | `MaterialCompileMultipleUvSets` clear -> `MATERIAL_EVAL_SINGLE_UV_SET`: one entry, constant indices (`MaterialUvCacheAt`), one UV decode | -48 registers (with the next row) |
| `Camera` copied at the top of the resolve: the motion vector's four matrices loaded there and held through the geometry | read at the motion vector; view-projections applied as two factors | 165 -> 130 |
| The material record copied at the top | flags first, the record just before sampling, IDs after it | 130 -> 123 |
| `DynamicSwizzle` indexing a vector (8 small allocas) | selects | no allocas left |
| The object-space normal map path, sampled outside the feedback scope, holding the normal matrix across sampling | `MaterialCompileObjectSpaceNormalMap` -> `PSO_OBJECT_SPACE_NORMAL_MAP` | neutral on Zorah (no such materials) |
| Each texture's streaming row read twice and two sample paths compiled | color-only evaluation samples the resolved descriptor (`SampleResolvedMaterialTexture2DGrad`) | 8 -> 4 sample ops |
| **`LoadMeshletResolveData_Wave` looped over the wave's distinct clusters, each iteration waiting out the whole chain of dependent loads** | per-lane loads | **5.9 -> 4.2 ms**, -21% instructions |
| The assembly-composed model, previous model and inverse composed together at load time | each composed where used; the previous position transformed as a point (`AssemblyPrevWorldPosition`) | small |

Measurement notes:

- Register counts move in cliffs: the driver picks a register budget per variant. On the still camera one stub that
  removed the assembly from the previous position took the average from 129 to 94 without changing the time, which read
  as "no longer occupancy-limited". Under camera motion that is wrong: the same register drop, done properly, saved 11%
  (see "Occupancy under camera motion" below).
- Now: long-scoreboard stalls are ~70% of stall cycles, L2 ~43%, DRAM ~35%. Without material sampling the pass takes
  1.9 ms, half of it the G-buffer writes (84 bytes a pixel, ~336 MB a frame at 2560x1600, the 32-byte surface record the
  largest). The rest is the per-pixel chain pixel list -> cluster -> draw record and page header -> mesh and meshlet
  descriptor -> palette and material -> triangle -> vertices -> material record -> texture.
- Tried and reverted: a per-visible-cluster record stage (below).
- NVPerf `gpu__time_duration` over 8 samples swings ±0.2 ms; use `render_graph_gpu_time` with 120+ samples for timing.

### Per-visible-cluster resolve records (2026-11-14, reverted)

A compute pass after both culling phases wrote one 128-byte record per live visible cluster: everything
`LoadMeshletResolveData` produces, plus the instance transform, the assembly transform and the group. Material evaluation
read its eight `uint4`s instead of the chain cluster -> draw record and page header -> mesh and meshlet descriptor -> palette
and material. The live entries were hardware clusters at `[0, hw1 + hw2)` and software clusters at
`[capacity - sw1 - sw2, capacity)`, so no stamps were needed; the pass read the four counters.

Zorah, moving camera, `render_graph_gpu_time`, same build, records on and off (off in the deployed shader):

| | Evaluation | Record pass | Sum |
|---|---|---|---|
| Records off | 3.42 ms | - | 3.42 ms |
| Records on | 3.27 ms | 0.61-0.66 ms | 3.9 ms |

- Evaluation lost 99 DXIL instructions and 16 buffer loads but gained only 0.15 ms (~5%). Since the records removed the whole
  cluster-to-material chain, that is roughly the most any cheaper way of collapsing it (fields carried in the pixel list,
  smaller records) can save. What evaluation waits on is the triangle -> vertex -> attribute fetch from the page and the
  texture samples, not the cluster lookups.
- The record pass costs 0.6 ms for ~420k live clusters (200k hardware), with 65k or 524k threads alike: the work per cluster,
  not serialization.
- Memory: the records are indexed like the visible-cluster buffer (4M entries, 512 MB). Aliased, the persistent alias plan's
  requirement went from 1006 MB to 1290-1380 MB.

Trap found on the way: the graph runs passes in declaration order, and CLod culling and raster are inserted before
`MaterialHistogramPass`. A pass declared ahead of that point which reads the visible-cluster buffer and counters runs
before culling and sees the previous frame's clusters with this frame's reset counters. Declaring a read of a resource
the later passes write does not reorder it.

## Under camera motion: chain, writes and occupancy (2026-11-14)

Zorah with `ZORAH_CAMERA_MOTION=profile` (still-camera-only gains are not kept). Timings are `render_graph_gpu_time`
A/B/A/B in one session, with the deployed shader hot-swapped and the 15 evaluation PSOs recompiled.

### Where the time goes

Stubs, measurement only:

| Stub | Evaluation |
|---|---|
| Base | 3.40 ms |
| Isotropic sampling (no 16x anisotropy) | 3.31 ms |
| No material sampling | 2.65 ms |
| One 4-byte write instead of the G-buffer | 2.81 ms |
| No surface record write | 3.12 ms |
| Payload targets written only when used | 3.30 ms |

Sampling is ~0.77 ms, the G-buffer writes ~0.6 ms (the record 0.28 ms, the payloads 0.11 ms), and the geometry, the
dependent chain and the ALU the remaining ~2 ms.

### Dependent load chain (all reverted)

| Change | Base | Variant |
|---|---|---|
| The material record loaded by row | 3.40 | 3.42 ms |
| The material record loaded early | 3.40 | 3.93 ms |
| Early load plus early descriptor resolve | 3.40 | 3.75 ms |
| UVs decoded early | 3.42 | 3.47 ms |

Starting a load sooner holds its result in registers across everything in between. Shortening the chain without saving
registers bought at most 5% (the per-cluster records above). Moving loads earlier costs occupancy and loses.

### G-buffer writes (kept)

`WriteCanonicalSurfaceSample` writes the coat and fuzz payload targets only for a payload profile that carries them, and
`GetFragmentInfoCanonicalSurface` reads them only then (otherwise zero, as the profile's contract says). Lanes of a wave
with equal surface records share one record, kept at the lowest of their pixel indices. Consumers reach it through
`identity.x`, which the public contract (`SARPSurfaceRecordV1`) has always used. **3.40 -> 3.19 ms.** Images match within
same-shader noise (0.42% vs 0.43%). The CLod inspection readback, pixel pick, host object picker and capture comparison
now go through `identity.x`, not the pixel index.

### Occupancy under camera motion (kept)

NVPerf launch-stall reasons for the pass, as % of elapsed cycles: register allocation ~31, barrier 1.6, shared memory 1.4,
warp slots 1.4, CTA slots 0.25. SMs ~100% active. So registers were the only occupancy limiter. Groups are 64 threads, and
each scheduler holds 16K registers, so a variant at 128 registers gets 4 warps a scheduler (33%), at 96 gets 5 (42%) and at
80 gets 6 (50%).

To get per-variant numbers, a temporary filter dispatched one evaluation step at a time. One variant (flags `0x78`: base
color, normal, metallic and roughness textures) took 2.62 of 2.86 ms, at 128 registers. Local-memory traffic was ~0, so
there were no spills: the compiler chose the 128 step.

Stubs on that variant alone (registers): no motion vector 96, no material sampling 96, no streaming-feedback scope 128
(worse). No change from removing the record dedupe or the writer's OpenPBR load. The motion-vector cost was the assembly
transform: `LoadAssemblyTransformOrIdentity` ran beside the model, and its 12 floats stayed live through the triangle fetch,
the barycentrics and the surface until the motion vector. **Loaded at the motion vector: 128 -> 96 registers for that
variant (and for `0x7c`), achieved occupancy 26% -> 34%, evaluation 3.08 -> 2.75 ms (-11%)** at the same instruction
count.

The next step down (80) was not found. With the fix in, none of these moves the variant off 96: no normal map, no metallic
and roughness, no streaming descriptor resolve, the motion vector written straight away, or no samples at all. The remaining
pressure is spread across the resolve, not one live range.

### From the Nsight register export (2026-11-14, kept)

An Nsight Graphics Shader Profiler export of the dominant variant (`scripts/nsight_shader_lines.py`) showed a plateau, not
a spike: 24% of samples at 88-95 live registers, spread over the whole geometry phase (UV decode, normal interpolation,
the assembly inverse, the motion vector, the cluster -> material chain), and sampling at 45-65. So the next step (80) means
serializing loads, and it was dropped. The hottest lines pointed at three changes instead. Moving camera,
`render_graph_gpu_time`, A/B/A/B in one session:

| Change | Base | With it |
|---|---|---|
| A vertex's U and V read with one `Load3` (`ReadPackedBitPair32_BA`), and the triangle's index bytes with one `Load2`. They took up to four loads behind branches; `ReadPackedBits32_BA`'s two loads were the most sampled geometry lines (13.5%). | 2.73, 2.76 ms | 2.61, 2.63 ms |
| The normal matrix from `obj.model`'s cofactors over its determinant (`CLodNormalMatrixFromModel`), not `ComposeAssemblyModelInverse`: no load of the instance's inverse, no second assembly load, no inversion, no matrix multiply. Kept as structurally better. | 2.61, 2.63 ms | 2.63, 2.63 ms |
| Streamed texture descriptors resolved per material before evaluation (`ResolveMaterialTextureDescriptorsPass`, ~6 us). Evaluation reads one `uint4` row by material index, beside the material record, instead of chaining record -> streaming row -> descriptor per texture: that chain was the most sampled line (9%). | 2.62, 2.65 ms | 2.49, 2.50 ms |

- The resolved table first had a range check in front of each texture's read (`row < capacity`, else the chain). That
  was **3% slower than no table**; the same table without the check was 7% faster. Registers, occupancy and instruction
  counts were identical, so the branches stopped the loads and samples from being scheduled together. The table now
  covers every material row (it grows with the material tables, rebuilding the graph), and the read is clamped by the
  table's length, which costs no branch.
- Images (still camera, one session): new vs old 0.38% mean, the same as new vs new (0.41%). One small spot on a
  normal-mapped ornament under a bright light differed in both new captures. With stochastic lighting off it was within
  noise too, so it was the light sampling decorrelating, not the material.
- The table's binding is kept in both states of `MATERIAL_EVAL_RESOLVED_TEXTURE_TABLE` (1 by default in
  `VisUtilEvaluate.hlsl`), so `pso.recompile` can A/B it in a running demo.

