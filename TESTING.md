# BasicRenderer Testing and Benchmark Harness

Applications linked to `BasicRenderer::BasicRenderer` can use
`Telemetry/StatisticalSampler.h` to load a sampling configuration, select
measurements from `br::telemetry::nvperf::CaptureResult`, test convergence,
and persist an experiment. Application-specific scene setup and readiness
policy intentionally remain in the application.

Metrics use the NvPerf source by default. A metric with
`"source": "render_graph_gpu_time"` samples the render graph's raw GPU
timestamp for each configured pass instead, so applications can gather pass
timing distributions without requiring an exclusive hardware-counter session.

## Demo sampling control

The demo app accepts `--sampling-config <path>`. For example,
`--sampling-config config/hierarchical_culling_sampling.json` profiles both
opaque hierarchical CLOD culling phases and exits after convergence.

Add `--sampling-control-pipe <name>` to keep the demo alive and control
multiple experiments through `StatisticalSamplingControl`. The shared
`Telemetry/SamplingControlServer.h` provides the same named-pipe transport to
other BasicRenderer applications. Supported core commands are
`session.status`, `profile.run`, `profile.status`, `profile.cancel`, and
`shutdown`.

The demo also exposes `clod.mode.get` and `clod.mode.set` for paired CLOD
experiments without restarting the process. `clod.mode.set` accepts
`culling` (`pure_compute` or `work_graph`) and `software_raster`
(`disabled`, `compute`, or `work_graph`). It also accepts the optional
`rigid_only` boolean. Enable that specialization only when the loaded workload
contains no skinned geometry; the default general variant remains
skinned-capable.

`pathtracing.get` and `pathtracing.set` switch path tracing at runtime.
`pathtracing.set` takes optional `enabled` and `accumulate` booleans
(`accumulate` is the `pathTracingAccumulate` setting, the reference-image mode,
and rebuilds the graph). Both report `enabled`, `supported`, `accumulate`,
`accumulating` (the current graph has the accumulation buffer),
`accumulated_frames` (frames since the last accumulation restart) and
`accumulation_restarts`.

Accumulation restarts when the scene's ray-tracing state, the path-tracing
settings, the camera's view or unjittered projection (FOV, aspect, near plane),
or the `pathTracingCameraModelHash` setting changes. Jitter does not restart it.
Code that changes how primary rays are generated (a lens model) sets
`pathTracingCameraModelHash` to a hash of that state. The lens's focus is not in
it: focus is per frame, and the path tracer restarts accumulation on the GPU the
frame the focus moves (manual focus changes also restart it on the CPU side;
in autofocus the CPU's frame count follows the focus epoch it reads back a few
frames later, so `accumulated_frames` never counts more samples than the
buffer holds).

### HDR capture

`capture.hdr` writes one frame's linear radiance as an RGBA32F DDS
(`DXGI_FORMAT_R32G32B32A32_FLOAT`, one mip, no tonemapping):

```powershell
StatisticalSamplingControl.exe --pipe <name> --json pathtracing.set '{"enabled":true,"accumulate":true}'
StatisticalSamplingControl.exe --pipe <name> --json capture.hdr '{"path":"D:/captures/ref.dds","spp":1024}'
```

| Argument | Default | Meaning |
|---|---|---|
| `path` | required | Output file. Relative paths resolve against the demo's working directory; parent directories are created. The file is written as `<path>.partial` and renamed, so a file at `path` is always complete. |
| `target` | `accumulation` when path tracing is accumulating, else `hdr` | `accumulation`: the path tracer's accumulation buffer at render resolution. `hdr`: `Builtin::PostProcessing::UpscaledHDR` as `TonemappingPass` reads it, at output resolution, after the upscaler and before bloom and tonemapping. |
| `spp` | `0` | Samples per pixel to wait for. `0` captures the next frame. Anything above 0 needs accumulation on. |
| `timeout_seconds` | `300` | The job fails after this long. |

For `accumulation`, rgb is already the average radiance (the shader keeps a
running mean), so nothing needs dividing; alpha is the per-pixel sample count.
For `hdr`, alpha is whatever the target holds.

The reply carries a `job_id` (at or above 2^48, apart from pipeline jobs).
`StatisticalSamplingControl` polls `job.status` until it ends unless `--async`
is passed. `job.status` on a capture job returns `job` with `state`
(`waiting`, `reading`, `published` or `failed`), `error`, `path`, `target`,
`spp_requested`, `width`, `height`, `samples_min` and `samples_max` (the
alpha range read back), `reads` and `frames_since_restart`.

With `spp`, the job waits until the path tracer has prepared that many frames
since its last restart, then reads the buffer after the whole frame. If the
alpha read back is still below `spp` (a restart it did not see, or frames
counted before the pipeline could trace), it waits and reads again. The image
can therefore hold a few more samples than requested, by up to the frames in
flight; `samples_min` and `samples_max` say how many it holds. Keep the camera
still while waiting: any restart starts the count again.

### Upscaler state

`upscaler.get` reports what the upscaler does: `setting` (the `upscalingMode`
setting), `graph` (the mode the current graph was built with), `evaluated`
(what the last upscaling pass ran), `quality`, `render_resolution`, and the
failure counters: `failed_evaluations`, `out_of_memory_skips` (Streamline
skipped for GPU memory pressure), `consecutive_failures`, `last_failed_frame`,
`fallback` (what failed frames showed: `input_copies`, `dlss_sr`, `cleared`),
and `ray_reconstruction_settings_failed`, `ray_reconstruction_settings_failures`
and `ray_reconstruction_retry_seconds` (Ray Reconstruction's optimal settings
failed, so graphs use DLSS until a retry). `upscaler.set` takes optional
`quality` (0 DLAA to 4 Ultra Performance) and `ray_reconstruction` (bool), as the
menu sets them. `upscaler.simulate_failure` takes `out_of_memory_frames` and
`settings_failures` and makes that many evaluations or settings requests fail;
`BASICRENDERER_SIMULATE_DLSS_FAILURE=oom=N,settings=M` does the same at startup.
See SARP's `docs/path_tracing.md`, "When Ray Reconstruction can't run".

A sampling run turns Streamline off (`BASICRENDERER_DISABLE_STREAMLINE=1`),
because NVPerf replay needs the device-level sampling session to itself, so
DLSS and Ray Reconstruction are unavailable under `--sampling-config` by
default. `BASICRENDERER_SAMPLING_KEEP_STREAMLINE=1` keeps Streamline for a run
whose configuration doesn't use NVPerf (only `render_graph_gpu_time` metrics),
so the upscaler commands above and path tracing with Ray Reconstruction can be
tested through the control pipe.

### Lens, camera and shader store

`lens.get`, `lens.set`, `lens.selftest` and `lens.selftest.status` control the
path tracer's physical camera lens: `lens.set` takes `mode` (`pinhole`,
`explicit` or `polynomial`), `file`, `focus_m`, `focus_mode` (`manual` or
`auto`), `af_point` (`[x, y]` in NDC), `af_region`, `af_speed`, `monochromatic`,
`f_number` (0 is wide open), `field_mode` (`remap` or `physical`) and
`polynomial_degree` (5 or 7). Focus settings apply the next frame without a
rebuild (`published_revision` stays); the others rebuild the lens off the render
thread (a few seconds), so poll `lens.get` until its `state` is `explicit` or
`polynomial`. `lens.get`'s `lens.focus` reports the focus the GPU last read back,
the autofocus target and measurement, and an approximate depth of field. `lens.selftest` starts a GPU-against-CPU parity test
of the published lens and `lens.selftest.status` returns its report (`passed`).
SARP's `docs/lens_simulation.md` describes every field.

`lens.flare.get`, `lens.flare.set`, `lens.flare.selftest` and
`lens.flare.selftest.status` control the lens's ghost flares (SARP's
`docs/lens_simulation.md`, "Ghost flares"): `lens.flare.set` takes `enabled`,
`max_ghosts`, `grid`, `wavelengths`, `max_lights` (each of these rebuilds the
graph) and `intensity` (1 is physical), and `test_source`
(`{"direction":[x,y,z],"distance_m":d,"irradiance":[r,g,b]}` in view space, or
`null`), which replaces the scene's flare sources with one source, for tests.
The ghosts are rebuilt off the render thread whenever the lens is; poll
`lens.flare.get` until its `state` is `ready`. `lens.flare.selftest` compares one
frame's GPU ghost lattice with LensCore and `lens.flare.selftest.status` returns
the report (`passed`). The flares are added to the upscaled HDR colour, so
`capture.hdr` with `target` `hdr` includes them; capturing with `intensity` 0 and
1 and subtracting isolates them.

`camera.set` takes an optional `fov_deg` (the primary camera's vertical field of
view, in (1, 170)) and reports `fov_deg` and `aspect`. A lens covers only its
design field, so lens tests narrow it first.

`shadercache.stats` returns `shader_store`: the shader store's batch summary
line, its folder, hit, compile and miss counts (misses by reason, and the first
changed file), and its index and artifact sizes. See SARP's
`docs/preprocessing.md`.

## C# scenarios

`--script <file.cs|dir> --script-exit` runs a C# scenario and exits with 0 (passed), 1 (failed or timed out), 2
(compile error) or 3 (the scripting runtime could not start). `--script-result <path>` writes its result document,
`--script-fixed-dt <s>` makes the run frame-rate independent and `--no-scene-overlap` makes writes reach the next
frame. With `--sampling-control-pipe` the pipe also takes `script.run {path, scenario?, timeout_seconds?}`,
`script.reload`, `script.stop {reason?}` and `script.status`; F5 in the window reruns the last scenario. Scenarios
reach every command in this file through `ctx.Commands`. See [docs/scripting.md](docs/scripting.md).
`ScriptRuntimeTests` and `ScriptHostTests` cover the runtime and host without a device.

The control pipe no longer needs `--sampling-config`: `--sampling-control-pipe <name>` alone serves every command
except `profile.run`. Commands are registered once in a `ControlCommandRegistry`
(`Diagnostics/ControlCommandRegistry.h`) that the pipe and scripts share.

## CLOD virtual-shadow CPU benchmark

For repeatable CLOD virtual-shadow CPU measurements, run:

```powershell
.\scripts\Benchmark-CLodVsmCpu.ps1 -Runs 3 -DirectHashIngest On
```

The harness builds the RelWithDebInfo demo, runs a smooth 480-frame camera
arc using the normal renderer settings, and writes raw dependency-batch
samples plus per-run and aggregate JSON/CSV reports under
`out/clod-vsm-cpu-benchmarks`. Use `-DirectHashIngest Both -ParallelSort On`
to compare direct hash ingestion against the sorted fallback, or
`-ParallelSort On/Off` to compare sorting policies on that fallback. The
benchmark deliberately avoids the teleporting camera and graph rebuilds used
by `--clod-streaming-stress-test`.

### Request tracing

Add `-RequestTrace` to capture the lifetime of every CLOD upload request from
decoded readback through CPU scheduling, disk I/O, upload queueing, residency
commit, and final promotion. Each run writes `request-trace.json` with stage
percentiles, full bounded request traces, the 100 worst completed requests,
and the oldest requests still active at shutdown.

The request trace separates I/O admission, task-queue, active-read, and
result-service latency, and reports the subset that was still live when
admitted. Use `-IoAdmissionDepth` to sweep the bounded I/O window. The retained
large-scene default is derived as 48 jobs per I/O worker (1,536 with the
demo's 32 workers). `-SchedulerAging On` and `-LiveBackgroundLanes On` enable
the experimental fairness policies; they remain off by default because the
reference workload showed lower throughput and worse p99 latency.

Use `-IoWorkerCount` to sweep the process-wide I/O pool and
`-IoTaskBatchSize` to sweep CLOD request grouping independently. CLOD mapped
reads default to batches of eight requests per scheduler task; results from a
task are published under one lock and generate one streaming-owner wake.
The renderer uses 32 I/O workers by default. Late-allocation mapped-view
requests warm cache ranges on those workers, so the retained large-scene
configuration benefits from the additional concurrency.
`SARP_CLOD_IO_WORKER_COUNT` and `SARP_CLOD_IO_TASK_BATCH_SIZE` expose the same
controls for normal launches.

Ordinary CLOD requests use late physical-page allocation by default. Their
payloads are represented by stable shared mapped-container views, so up to
1,536 admitted/staged groups retain only compact metadata rather than copied
page blobs. Each shared mesh acquires its mapped-container lease once during
registration. I/O workers lock-free deduplicate warming by exact mesh page
and submit all newly cold ranges in one OS prefetch call per request batch.
`SARP_CLOD_LATE_CPU_PAGE_ALLOCATION`,
`SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT`, and
`SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET` control this path. Pinned groups and GPU
DirectStorage destinations continue to reserve their required physical pages
before submission.

The same mode can be enabled for a normal demo launch by setting
`SARP_CLOD_REQUEST_TRACE_OUTPUT` to the desired JSON path. Tracing is disabled
when the variable is unset. Completed traces are capped at 100,000 records;
the report includes a dropped-record count if that bound is reached.

[Back to the BasicRenderer overview](README.md)
