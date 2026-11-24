# CLod compute culling: optimization experiments

`CLodOpaque::HierarchicalCullingPass1` and `Pass2` (pure compute backend: `shaders/ClusterLOD/computeCulling.hlsl`, with
`ClusterCullBody` in `workGraphCulling.hlsl`) on the Zorah scene: raster with stochastic direct lighting, capped quality
(`SARP_TEXTURE_FEEDBACK_FORCE_MIP=4`, `clodLodErrorPixels` 3), frozen lights, moving camera (`ZORAH_CAMERA_MOTION=profile`),
2560×1600, RTX 5070 Laptop (GB206). Times are the in-situ render graph timer, A/B/A/B in one session; NVPerf for counters.

## Reading NVPerf on this pass

`ComputeCullingRecorder::Mark` opens a sub-range per indirect dispatch. Under NVPerf each one is its own range,
`<pass>/<stage> L<level>` (for example `CLodOpaque::HierarchicalCullingPass1/cull clusters L3`), and the pass's own range keeps
only what is outside them (clears, argument builds, object culling). Profiled by the pass name alone it reported 0.10 ms against
the timer's 2.9 ms; name the sub-ranges in the sampling config instead (`"required": false` for levels that may not run). The
render graph timer has no sub-ranges outside kept recordings (`BeginSubRangeQuery` needs an owned query set), so per-stage time
comes from NVPerf only.

Per stage and level, NVPerf (ms, 2026-11-21): 7 traversal levels; `cull clusters` at levels 2-4 held 2.2 of 3.4 ms (0.55, 0.84,
0.85), node and leaf traversal ~0.6 ms together, object culling and the rest ~0.13 ms. It is work, not dependent-dispatch latency.

## Cluster culling registers (kept)

`PureComputeDenseClusterWorkCS`: 152 registers, 23.5% occupancy, latency-bound (issue active 23%, long scoreboard the largest
stall). Stub probes, edited into the deployed `workGraphCulling.hlsl` and recompiled live (registers):

| Variant | Registers |
|---|---:|
| Baseline | 152 |
| Previous model composed at the phase 1 occlusion test instead of with the model (kept) | 128 |
| Camera infos loaded at their use sites (on its own) | 136 |
| Both | 120 |
| No occlusion test | 120 |
| No LOD child suppression | 136 |
| No skinned meshlet bounds | 112 |
| No previous-pose skinned bounds | 144 |
| No skinned bounds of either kind | 96 |
| Benchmark-only "software raster disabled" path removed | 152 |

`ClusterCullBody` composed the whole `PerObjectBuffer` (model, previous model and inverse, each through the assembly transform)
in its setup, and the previous model, used only by phase 1's occlusion test, stayed live through the meshlet's bounds. It now
composes the model alone (`ComposeAssemblyModel`) and the previous model at the test (`ComposeAssemblyPrevModel`,
`instanceDrawRecordHelpers.hlsli`).

| | Pass 1 | Pass 2 |
|---|---:|---:|
| Baseline | 2.81 ms | 0.925 ms |
| Previous model at its use | 2.53 ms (-10%) | 0.83 ms (-10%) |
| Also the camera infos at their uses (120 registers) | 2.55 ms | 0.84 ms |

The camera variant stays in the same register step and adds loads, so it was not kept. Visible clusters were identical with the
old and new shaders swapped in one session at a still camera (188,931 of 3.64M).

Zorah has no skinned meshlets (`live_evaluations=0`), so the skinned bounds code is dead here and costs 128 → 96 registers. A
rigid-only culling variant chosen per scene was left out on purpose (user, 2026-11-21): other scenes have skinning.

## Traversal registers (kept)

`PureComputeTraverseFrontierCS` had the same pattern (the whole `PerObjectBuffer` at the top, the previous model used by the
streaming and occlusion tests). Composing it at those uses: node traversal 152 → 128 registers (occupancy 21.5 → 28%, ~10% faster
by NVPerf), leaf traversal 160 → 144 (no occupancy step, ~3%).

## Expansion factor (not changed)

`clod.phase2_expansion.set` (`CLodPureComputePhase2ExpansionFactorDefault`, 16): lanes per cluster run record in the dense cull.

| Lanes | 4 | 8 | 16 | 32 |
|---|---:|---:|---:|---:|
| Pass 1, before the register change | 2.77 ms | 2.75 ms | 2.84 ms | 3.07 ms |
| Pass 1, after | 2.44-2.45 ms | 2.39-2.40 ms | 2.46-2.47 ms | |

8 is ~2.7% faster for the primary view. The setting is shared with the virtual shadow views, which were not measured, so the
default stays 16.

## Traps

- A stub that changes what culling keeps also changes streaming, and readiness then never settles (`timed out waiting for stable
  readiness`). For register probes use `stable_frames` 1.
