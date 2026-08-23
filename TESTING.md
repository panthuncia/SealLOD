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
