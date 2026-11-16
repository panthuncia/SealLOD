# Optimizing a render pass: workflow

How to make a GPU pass faster: measuring it, finding what limits it, trying changes in a running demo, and validating
them. It was written up from the material evaluation work (`docs/material-evaluation-optimization-experiments.md`),
but nothing in it is specific to that pass. [TESTING.md](../TESTING.md) is the reference for the sampling harness,
the control pipe and `capture.hdr`; this document is the order to use them in and the traps.

## Rules

- **Measure a moving camera.** A gain that shows up only with a still camera is not kept. A still camera holds the
  LOD cut, culling, streaming and caches still, so it flatters every change. Use a still camera only for image
  comparisons, which need a repeatable frame.
- **Compare within one session.** Two launches of the same build can land in states ~10% apart (streaming, VRAM).
  Run base and variant as A/B/A/B in one process by hot-swapping the shader (below).
- **Cap quality for timing.** At the VRAM limit the scene never settles and texture pull-back moves the numbers.
  `SARP_TEXTURE_FEEDBACK_FORCE_MIP=4` plus `clodLodErrorPixels` 3 settles in ~7 s at ~9 GB, and repeated profiles agree
  within ~0.1%. Don't use `SARP_TEXTURE_STREAMING_BUDGET_MB` as a cap: it is compared against total VRAM use and starves
  textures.
- **Keep clearly positive changes even when the timing is flat** (fewer loads, less live state, removed redundancy).
  **Drop complexity that buys nothing.**
- **Check that a claimed revert landed**: diff the deployed shader against source when you're done. A "reverted"
  experiment that wasn't caused a visual bug only visible under motion.
- **Write it down.** Each pass has, or gets, a `docs/<pass>-optimization-experiments.md` holding the setup, a table of
  base, variant and outcome, and the reverted experiments with the reason. Those reasons are what stop the next agent
  repeating them.

## 1. Build and launch

As SARP's submodule, build the demo explicitly (`build.cmd [jobs] all` skips it):

```
set SARP_SKIP_INSTALL=1 & set SARP_SKIP_CONFIGURE=1 & C:\...\SARP\build.cmd 16 BasicRendererDemo
```

The exe lands in `build/ninja-renderer-host/BasicRenderer/BasicRenderer/RelWithDebInfo/` but can't run there: it needs
the runtime folder's `D3D/`, `shaders/` and `textures/`. Run it from a deployed folder (`scripts/deploy_demo.ps1`,
default `D:\BasicRendererDemo`), copying the new `BasicRenderer.exe` and `.pdb` in after each build. The deployed
`shaders/` is a copy: hot-swaps edit it, and source edits must be copied in.

Launch through a `.cmd` wrapper started with `explorer.exe <wrapper>.cmd`, which is what works from agent shells.
A typical wrapper:

```
cd /d D:\BasicRendererDemo
set PATH=<BasicRenderer>\ThirdParty\NvPerf\bin\x64;%PATH%
set SARP_RT_PATH_TRACING=0
set SARP_STOCHASTIC_LIGHTING=1
set SARP_TEXTURE_FEEDBACK_FORCE_MIP=4
set ZORAH_LOD_ERROR_PIXELS=3
set ZORAH_FREEZE_LIGHTS=1
set ZORAH_CAMERA_MOTION=profile
set BASICRENDERER_LOG_PATH=D:\BasicRendererDemo\logs\<run>.log
BasicRenderer.exe --sampling-control-pipe p_<run> --sampling-config <config.json> --script <BasicRenderer>\Scripting\Scenarios\ZorahLights.cs --exit-after-seconds 900
```

- Path tracing is the default where supported. Raster passes need `SARP_RT_PATH_TRACING=0`.
- `ZORAH_CAMERA_MOTION=profile` flies the camera loop only while a profile settles and samples, restarting it each
  profile, so every profile measures the same stretch. `always` flies it continuously (for Nsight or watching).
- Wait for the pipe (`profile.status` answers) and for the scene (the log's "zorah placed").
- `BasicRenderer.launch.json` beside the exe gives a bare launch (Nsight) the same environment and arguments. See
  "Nsight" below. **It also fills in every variable your wrapper leaves unset**: with one in place, a "still camera"
  run without `ZORAH_CAMERA_MOTION` flew the camera (`always`) and an image check's noise floor went from 0.4% to 33%.
  Set every variable the file names, or move the file aside, for scripted runs.

Drive everything with `StatisticalSamplingControl.exe --pipe p_<run> --json <command> '<json args>'` (built beside the
demo). Useful commands: `profile.run {"label":..}`, `settings.set {"name":..,"value":..}`, `pso.list`, `pso.recompile`,
`capture.hdr`, `shutdown`. `profile.run`'s reply has no summary: read the config's `last_run_summary` file.

## 2. Time the pass

A config with the `render_graph_gpu_time` source, 120+ samples, and the pass by name and queue:

```json
{
  "name": "my pass timing",
  "readiness": { "stable_frames": 60, "settling_frames": 30, "timeout_ms": 120000 },
  "statistics": { "minimum_samples": 120, "maximum_samples": 400, "confidence_level": 0.95, "relative_half_width": 0.01 },
  "output": { "database": "<scratch>/perf.sqlite", "last_run_summary": "<scratch>/last.md" },
  "metrics": [ { "id": "gpu_time", "name": "render_graph_gpu_time", "output_name": "GPU time", "unit": "ms",
                 "source": "render_graph_gpu_time", "relative_half_width": 0.02 } ],
  "passes": [ { "id": "p", "name": "MyPass", "queue": "Graphics", "occurrence": 0, "metrics": [ "gpu_time" ] } ]
}
```

- Pass names come from `GetStatisticsService()->GetPassNames()`. Passes that a feature renames (for example
  `CLodOpaque::Scene*Pass`) need the new name, and the sampling config can't change in a live session: relaunch.
- In-situ render-graph timers move with what overlaps the pass on other queues. Compare A/B/A/B, not across sessions.
- NVPerf's `gpu__time_duration` swings ±15% between profiles in one session. **Never judge time from NVPerf**; use it
  for the hardware counters below.

## 3. Hot-swap A/B

1. Save the deployed shader file, then put the variant in its place, in the deployed `shaders/`, not the source tree.
2. `pso.list`, and take the `id` of every pipeline built from that shader (match on `name`).
3. `pso.recompile {"pipeline_id": id, "defines": {}}` for each. It returns a job. Wait until `pso.list` shows the
   pipeline not `compiling`, with `active_generation` advanced. Recompiling is refused while a profile runs.
4. `profile.run`, then restore, recompile and profile again: A1 B1 A2 B2.
5. At the end, `cmp` the deployed file against the one you saved (or against source).

- `defines` overrides `#ifndef` knobs without editing files. Overrides that change the shader's set of Builtin
  resources are refused ("use structural shader reload"): edit the deployed file and relaunch instead.
- Script it. If several variant files target different shaders, restore **all** targets before applying the next
  variant. A restore loop that failed silently once stacked every variant on top of the previous one.

## 4. Find the limiter (NVPerf)

Add `--sampling-config` with an NVPerf config (examples: `config/*_nvperf.json`). Metrics are `{id, name, output_name,
unit, type, rollup, submetric, required}`. Mark them `"required": false`: unavailable metrics are logged ("metric 'x' is
unavailable on chip 'GB206'") and skipped. **Don't use `"rollup": "max"`**: such metrics came back non-finite and every
sample was rejected. The pass's `queue` must be the queue its work runs on. Ranges are per pass; there are none per
dispatch.

Read these first:

| Question | Metrics |
|---|---|
| Latency- or throughput-bound? | `smsp__issue_active` (% active), `sm__throughput`, `l1tex__throughput`, `lts__throughput`, `dram__throughput` |
| Waiting on what? | `smsp__average_warps_issue_stalled_<reason>_per_issue_active` for long_scoreboard (memory), tex_throttle, lg_throttle, mio_throttle, short_scoreboard, wait, barrier... |
| Occupancy, and what caps it | `sm__warps_active` (pct of peak active), `tpc__average_registers_per_thread_shader_cs`, `tpc__warp_launch_cycles_stalled_shader_cs_reason_{register,shmem,warp,cta,barrier}_allocation` (pct of peak elapsed) |
| Idle SMs (tail, small dispatches)? | `sm__cycles_active` (pct of peak elapsed) |
| Spills? | `l1tex__t_sectors_pipe_lsu_mem_local_op_ld` / `_st` (should be ~0) |
| Load path | `l1tex__texin_requests` vs `l1tex__lsuin_requests` (buffer loads count on the TEX path), hit rates |

**Register arithmetic (NVIDIA, 64K registers per SM, 4 schedulers).** Each scheduler has 16K 32-bit registers. A warp
lives on one scheduler and is allocated in steps of 8 registers a thread. So:

| Registers a thread | Warps a scheduler | Occupancy (12 warps a scheduler max) |
|---|---|---|
| ≤ 128 | 4 | 33% |
| ≤ 96 | 5 | 42% |
| ≤ 80 | 6 | 50% |
| ≤ 64 | 8 | 67% |

The compiler picks a budget at one of these steps, so counts land exactly on 128 or 96 and move in cliffs. A count of
128 with no local-memory traffic is the compiler's choice, not a spill. Saving 10 registers inside a step buys nothing;
crossing a step does. Under camera motion, going from 4 warps a scheduler to 5 saved 11% of material evaluation.

## 5. Isolate the variant that matters

A pass that dispatches several pipelines (material evaluation runs one per compile-flag bucket) reports a time-weighted
average. Before stubbing, find the variant that holds the time. Temporarily filter the pass's dispatch steps by a
setting (registered with `settingsManager.registerSetting<float>(...)`, read in `Prepare` and changed with
`settings.set`), log each step's shader key once, and profile each step on its own. In material evaluation one variant
was 92% of the time. Remove the filter afterwards.

## 6. Locate register peaks

**Stub probes**: replace a section with a cheap stand-in and read `Registers/thread` for the isolated variant:

- Keep the stand-in's inputs alive (fold them into an output), or dead-code elimination removes the code upstream too and
  the probe lies.
- Read registers, which are stable. Don't read time: it's noisy, and a stub removes work as well as registers.
- Registers are a budget, not a sum. A probe that doesn't move the count doesn't mean that code is free (try
  combinations). Removing code can raise the count: dropping a scope once took a variant from 96 to 128.

**Nsight Graphics Shader Profiler** shows it directly. RelWithDebInfo builds compile shaders optimized with debug info
and source embedded (`WRITE_DEBUG_FILES` in `PSOManager.cpp`), so its source view has live registers and stall samples
per HLSL line.

- Put a `BasicRenderer.launch.json` beside the deployed exe so a bare launch from Nsight gets the profiling setup. The
  demo reads it at startup: environment entries never replace variables already set, and `arguments` is used only for
  an empty command line. Use forward slashes in paths.

  ```json
  { "environment": { "SARP_RT_PATH_TRACING": "0", "SARP_TEXTURE_FEEDBACK_FORCE_MIP": "4", "ZORAH_LOD_ERROR_PIXELS": "3",
                     "ZORAH_FREEZE_LIGHTS": "1", "ZORAH_CAMERA_MOTION": "always" },
    "arguments": "--script C:/.../BasicRenderer/Scripting/Scenarios/ZorahLights.cs" }
  ```

- Export the source view as CSV and run `python scripts/nsight_shader_lines.py <csv> [--threshold 80]`. It prints the
  regions above the threshold, the share of samples by live-register band, and the lines with the most stall samples.
- A line's count is the most live at any instruction mapped to it, after inlining and scheduling. It shows where the
  peak happens, not which values are live.
- **A spike** (one region well above the rest) is one live range held too long: find the value and move it.
- **A plateau** (a whole phase at the peak) is many independent loads in flight at once, the compiler buying
  memory-level parallelism. Getting under it means serializing loads, which costs latency. Don't chase the next step
  there; work on what the stall samples point at instead.

## 7. Patterns that have paid, and ones that haven't

Paid:

- **Load at the use site.** A value loaded early is held in registers until used. An assembly transform loaded beside
  the model and used at the motion vector cost a whole register step. So did copying a `Camera` or a material record
  at the top of a function: copy only the fields, where they're read.
- **Specialize variants** so each compiles one path (compile flags → `PSO_*` defines). One shader with every path pays
  the worst path's registers.
- **Constant indices** into local arrays. Run-time indexing makes allocas.
- **Per-lane loads** over wave loops. A loop over a wave's distinct clusters waited out the whole dependent chain once
  per iteration.
- **Write less.** Skip targets a surface doesn't use, and share identical per-pixel records across a wave.
- **Remove loads, don't move them.** Pre-resolving or precomputing a value removes a level of a dependent chain
  (per-material resolved texture descriptors: -5%). **One load per record, not one per field:** a vertex's packed U and
  V in one `Load3`, not up to four loads behind branches (-4.7%).
- **No range-check branch in front of a hot load.** A per-texture fallback test (`row < capacity`) made a lookup table 3%
  slower than not having it, at identical registers and instructions. Size the table so it can't be out of range, and
  clamp (a `min`, no branch) for safety.

Didn't pay:

- **Issuing loads earlier** to "start the chain sooner" lost every time (3.40 → 3.42 to 3.93 ms): their results are
  held across everything in between.
- **A per-cluster record stage** to collapse the per-pixel chain saved 5% of the pass and cost a 0.6 ms pass and
  ~300 MB.
- **Splitting a pass to cut registers** doesn't help when one half alone sits on the register plateau, and it adds a
  write and read of an intermediate.

## 8. Validate

- **Every variant compiles**, not only those the scene uses: SARP's `scripts/sarp_compile_clod_shaders.py` compiles the
  CLod raster and material evaluation matrix offline with dxc.
- **Images.** Use a still camera and frozen lights in one session: capture N frames each of A, B and A' with
  `capture.hdr` (hot-swapping between them) and compare B against A next to A' against A. The second number is the
  noise floor. `capture.hdr` reads the upscaled HDR target, so debug output modes don't show in it. Across sessions,
  check the log's texture streaming line ("pull-back held", resident MiB) first: pull-back dominates diffs near the
  VRAM limit. SARP's `scripts/sarp_compare_captures.py` compares the canonical surface targets.
- **Look under motion.** Some bugs (stale or dark rectangular blocks from a wrong wave operation) only show with the
  camera moving and look like denoiser or DLSS failures. Avoid `WaveReadLaneAt` with a non-uniform lane index; use a
  `WaveReadLaneFirst` loop.
- **Contract changes.** If a change alters what a pass writes (indexing, packing, skipped targets), find every reader:
  shaders, CPU readbacks, pickers, comparison scripts. Update them in the same change.

## Traps

- **A faster ray tracing pass may mean rays hit nothing.** On 2026-11-14 a BLAS cache change left every BLAS empty, and
  the sampling and trace passes got cheaper. Raster images didn't change, and stochastic lighting stayed unbiased
  against its reference, which traces the same BVH. After any change to the ray tracing scene build, run
  `SARP_RT_PRIMARY_COMPARE=1` and check the log's `CLod RT compare: match=` (millions) and `rayMissed=` (a handful).
- The render graph runs passes **in declaration order**. CLod culling and raster are inserted before
  `MaterialHistogramPass`. A pass declared earlier that reads visible clusters sees last frame's data with reset
  counters, and declaring an extra read doesn't reorder it.
- Readiness needs a still camera. With `ZORAH_CAMERA_MOTION=profile` the loop starts only once the profile settles. A
  "waiting for readiness" log line every 5 s says what blocks it.
- The sampling source is chosen at launch. NVPerf capture needs exclusive profiler ownership, so the demo drops
  Streamline (DLSS) for NVPerf configs; `BASICRENDERER_SAMPLING_KEEP_STREAMLINE=1` keeps it for timer-only configs.
