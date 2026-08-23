# CLOD pure-compute versus work-graph performance

## Test setup

- GPU: NVIDIA GeForce RTX 5070 Laptop GPU (GB206)
- Build: Clang RelWithDebInfo
- Process: one persistent `BasicRenderer.exe` process
- Scene: unchanged across captures; no skinned geometry
- Samples: 160 accepted frames per capture, no rejected frames
- Sequence: pure compute/compute SW raster, work graph/work-graph SW raster,
  pure compute/compute SW raster
- Mode changes used the `clod.mode.set` sampling-control-pipe command. The
  process was not restarted.
- Reported pure-compute values below are the mean of the two bracketing
  captures to reduce sensitivity to thermal and scene-state drift.

The core total includes hierarchical culling, hardware bucket
histogram/scan/offset/compaction and mesh raster, plus the compute SW
bucket/dispatch/raster passes when present. The extended total additionally
includes the CLOD linear-depth downsample and copy passes.

## Pipeline totals

| Scope | Pure compute | Work graph | Difference |
|---|---:|---:|---:|
| Phase 1 core | 6.5760 ms | 8.9482 ms | +36.1% |
| Phase 2 core | 0.6831 ms | 0.9047 ms | +32.4% |
| Both phases, core | 7.2591 ms | 9.8530 ms | +35.7% |
| Both phases, including depth passes | 7.5760 ms | 10.2531 ms | +35.3% |

The two pure-compute core totals were 7.2330 ms
(95% CI [7.2289, 7.2372]) and 7.2852 ms
(95% CI [7.2754, 7.2951]). The work-graph result was 9.8530 ms
(95% CI [9.8386, 9.8673]), well outside run-to-run drift.

## Per-pass breakdown

Values are milliseconds. A dash means that the work is integrated into the
work graph and no separate render-graph pass exists.

| Pass | Pure compute | Work graph | Difference |
|---|---:|---:|---:|
| Hierarchical culling 1 | 1.0774 | 5.0357 | +367.4% |
| HW bucket histogram 1 | 0.0760 | 0.0825 | +8.7% |
| HW bucket prefix scan 1 | 0.0060 | 0.0060 | +1.5% |
| HW bucket prefix offsets 1 | 0.0062 | 0.0064 | +3.0% |
| HW compact/args 1 | 0.0834 | 0.0957 | +14.8% |
| Hardware mesh raster 1 | 3.3875 | 3.7218 | +9.9% |
| SW create command 1 | 0.0001 | — | integrated |
| SW bucket histogram 1 | 0.0701 | — | integrated |
| SW bucket prefix scan 1 | 0.0061 | — | integrated |
| SW bucket prefix offsets 1 | 0.0062 | — | integrated |
| SW compact/args 1 | 0.0799 | — | integrated |
| Software raster 1 | 1.7770 | — | integrated |
| Linear-depth downsample 1 | 0.0599 | 0.0728 | +21.6% |
| Linear-depth copy 1 | 0.1173 | 0.1551 | +32.3% |
| Hierarchical culling 2 | 0.5269 | 0.8326 | +58.0% |
| HW bucket histogram 2 | 0.0268 | 0.0271 | +1.2% |
| HW bucket prefix scan 2 | 0.0059 | 0.0061 | +3.0% |
| HW bucket prefix offsets 2 | 0.0062 | 0.0064 | +3.0% |
| HW compact/args 2 | 0.0265 | 0.0267 | +0.9% |
| Hardware mesh raster 2 | 0.0057 | 0.0059 | +2.5% |
| SW routing and raster 2 | 0.0851 | — | integrated |
| Linear-depth downsample 2 | 0.0513 | 0.0604 | +17.8% |
| Linear-depth copy 2 | 0.0883 | 0.1118 | +26.5% |

## Notable observations

- Phase 1 dominates both configurations.
- In pure-compute mode, phase-1 culling plus the complete SW routing/raster
  chain costs about 3.017 ms. The corresponding integrated work-graph culling
  pass costs 5.036 ms, about 66.9% more.
- The work-graph configuration also increases the separate phase-1 hardware
  sorting and mesh-raster route from 3.559 ms to 3.912 ms, about 9.9%. The
  difference is therefore not only render-graph dispatch overhead: work
  distribution or overlap between HW and SW classifications changes.
- Phase-2 rasterization is effectively empty in this scene. Its measurable
  cost is culling and fixed bookkeeping rather than triangle raster work.
- The A-B-A check reproduced phase-1 pure-compute culling at 1.0788 and
  1.0760 ms. The large work-graph result is not explained by thermal drift.
- Earlier hardware-counter captures of the simpler work-graph path identified
  long-scoreboard stalls as the dominant issue. Those captures predate the
  current graph-integrated SW raster topology, so they are directional
  evidence rather than current quantitative results.

Raw per-pass experiments are stored in
`out/clod_compute_vs_workgraph_pipeline.sqlite` under the application build
directory. The reusable configuration is
`config/clod_compute_vs_workgraph_pipeline_sampling.json`.
