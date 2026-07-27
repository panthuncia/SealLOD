
# BasicRenderer

![Zorah shadow example](images/zorah_shadows.png)

An advanced DX12 research renderer, written to experiment with real-time rendering, built entirely around virtualized geometry

Feature development is driven purely by what I'm interested in at the moment.

## Graphical Features

- Nanite-style virtualized geometry using a novel work-graph approach, capable of real-time rendering of scenes with tens of billions of triangles
- Virtual shadow mapping (directional-only for now) with multiple experimental raster modes
- Voxel LOD fallback for virtualized geometry
- Image-based lighting
- Normal mapping & contact-refinement parallax heightmaps
- Support for arbitrary numbers of point, spot, and directional lights using clustered lighting
- Directional shadow mapping
- Skinned meshes
- Order-independent transparency using a per-pixel linked-list OR adaptive voxel-based OIT
- SSAO with XeGTAO
- Downsample/upsample bloom
- Screen-space reflections with FidelityFX SSSR
- TAA/upscaling with DLSS/FSR
  
## Technical Features
- A powerful render graph for automatic resource transitions and queue synchronization. Supports both retained-mode and immediate-mode GPU command execution, and caches itself automatically for better performance.
- Low-level RHI (Only DX12 backend implemented, for now, but built to support Vulkan)
- Shader-instrumentation debugging, using [GPU Reshape](https://github.com/GPUOpen-Tools/GPU-Reshape)'s backend
- GPU-driven rendering with compute culling & ExecuteIndirect
- NVPerf pass capture plus a reusable statistical sampler with confidence intervals, convergence checks, SQLite history, and Markdown reports
- Visibility buffer (UE5-style), Deferred, and forward+ rendering
- DirectStorage integration for low-latency, high-throughput data streaming
- GPU BC7 compressor for rapid asset optimization
- Clustered lighting with a paged linked-list
- Async-compute
- Compute-based skinning
- Meshlets & mesh shaders
- Flecs ECS for scene management
- A basic UI for feature toggles, importing new asset files, debug view selection, and scene graph introspection & modification

## Gallery
![Zorah cluster example](images/zorah_clusters.png)

<img src="images/needles_0.png" width="34.1%"><img src="images/needles_1.png" width="33.1%"><img src="images/needles_2.png" width="32.6%">

![San-Miguel example](images/SanMiguel.png)

![Bistro example](images/Bistro.png)

![Sponza example](images/Sponza.png)

![SSR example](images/SSR.png)

## Supported File Formats
- USD using OpenUSD, https://github.com/PixarAnimationStudios/OpenUSD
- Partial assimp loader implemented, https://github.com/assimp/assimp/blob/master/doc/Fileformats.md

## Notable Third-Party Dependancies

- [nlohmann-json](https://github.com/nlohmann/json)
- [meshoptimizer](https://github.com/zeux/meshoptimizer)
- [spdlog](https://github.com/gabime/spdlog)
- [ImGui](https://github.com/ocornut/imgui)
- [assimp](https://github.com/assimp/assimp)
- [flecs](https://www.flecs.dev/flecs/)
- [XeGTAO](https://github.com/GameTechDev/XeGTAO)
- [FidelityFX SPD](https://gpuopen.com/fidelityfx-spd/)
- [FidelityFX SSSR](https://gpuopen.com/fidelityfx-sssr/)
- [FSR](https://www.amd.com/en/products/graphics/technologies/fidelityfx/super-resolution.html)
- [DLSS](https://www.nvidia.com/en-us/geforce/technologies/dlss/)
- [OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD)
- [GPU Reshape](https://github.com/GPUOpen-Tools/GPU-Reshape)
- [OneTBB](https://github.com/uxlfoundation/oneTBB)
- [SLang](https://github.com/shader-slang/slang)
- [Tracy](https://github.com/wolfpld/tracy)
- [Tree-Sitter](https://github.com/tree-sitter/tree-sitter)
- [DirectStorage](https://github.com/microsoft/DirectStorage)
  
## Notable Sources and References for Development

[Timberdoodle](https://github.com/Sunset-Flock/Timberdoodle) research engine

[Sparse Virtual Shadow Maps](https://ktstephano.github.io/rendering/stratusgfx/svsm), by J. Stephano

[Adaptive Voxel-based order-independant transparency](https://advances.realtimerendering.com/s2025/content/AVBOIT_SIG2025_MDROBOT-final.pdf), Siggraph 2025, Michal Drobot

[Nvidia tessellated clusters sample](https://github.com/nvpro-samples/vk_tessellated_clusters)

[Brian Karis's Reyes writeup](https://graphicrants.blogspot.com/2026/02/nanite-reyes.html)

[NVidia cluster LOD sample](https://github.com/nvpro-samples/vk_lod_clusters)

[Bevy's virtualized geometry](https://jms55.github.io/posts/2024-06-09-virtual-geometry-bevy-0-14/)

[Visibility Buffer Rendering with Material Graphs](http://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/), by John Hable

[Filament](https://github.com/google/filament) for the material model, with additions from [MaterialX](https://github.com/AcademySoftwareFoundation/MaterialX)

[LearnOpenGL.com](https://learnopengl.com/)

[Erik Svjannson](https://www.youtube.com/watch?v=EtX7WnFhxtQ)'s talk on GPU-driven rendering and mesh shaders

[Medium article on occlusion culling](https://medium.com/@mil_kru/two-pass-occlusion-culling-4100edcad501)

## Repository split and dependency model

The build is being structured so the main components can live as independent repositories:

- `BasicRHI` (low-level GPU abstraction)
- `OpenRenderGraph` (render graph framework, depends on `BasicRHI`)
- `BasicRenderer` (application, depends on both)

Current dependency strategy is **package-first with submodule fallback**:

- `BasicRenderer` tries `find_package(BasicRHI CONFIG)` and `find_package(OpenRenderGraph CONFIG)` first.
- If packages are not available and fallback is enabled, it uses in-tree `add_subdirectory(...)`.

Relevant options:

- `BASICRENDERER_USE_PACKAGE_DEPS` (default `ON`)
- `BASICRENDERER_ENABLE_SUBMODULE_FALLBACK` (default `ON`)

## Standalone consumption quick start

Applications linked to `BasicRenderer::BasicRenderer` can use
`Telemetry/StatisticalSampler.h` to load a sampling configuration, select
measurements from `br::telemetry::nvperf::CaptureResult`, test convergence,
and persist an experiment. Application-specific scene setup and readiness
policy intentionally remain in the application.

Metrics use the NvPerf source by default. A metric with
`"source": "render_graph_gpu_time"` samples the render graph's raw GPU
timestamp for each configured pass instead, so applications can gather pass
timing distributions without requiring an exclusive hardware-counter session.

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

For repeatable CLOD virtual-shadow CPU measurements, run:

```powershell
.\scripts\Benchmark-CLodVsmCpu.ps1 -Runs 3 -DirectHashIngest On
```

The harness builds the RelWithDebInfo demo, runs a smooth 480-frame camera
arc using the normal renderer settings, and writes raw dependency-batch
samples plus per-run and aggregate JSON/CSV reports under
`out/clod-vsm-cpu-benchmarks`. Use `-DirectHashIngest Both -ParallelSort On`
to compare direct hash ingestion against the sorted fallback, or
`-ParallelSort On/Off` to compare sorting policies on that fallback. The benchmark deliberately avoids
the teleporting camera and graph rebuilds used by
`--clod-streaming-stress-test`.

Add `-RequestTrace` to capture the lifetime of every CLOD upload request from
decoded readback through CPU scheduling, disk I/O, upload queueing, residency
commit, and final promotion. Each run writes `request-trace.json` with stage
percentiles, full bounded request traces, the 100 worst completed requests,
and the oldest requests still active at shutdown.

The request trace separates I/O admission, task-queue, active-read, and
result-service latency, and reports the subset that was still live when
admitted. Use `-IoAdmissionDepth` to sweep the bounded I/O window. The retained
large-scene default is derived as 48 jobs per I/O worker (1,536 with the
demo's 32 workers). `-SchedulerAging On` and `-LiveBackgroundLanes On` enable the
experimental fairness policies; they remain off by default because the
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
page blobs. `SARP_CLOD_LATE_CPU_PAGE_ALLOCATION`,
`SARP_CLOD_STAGED_PAYLOAD_GROUP_LIMIT`, and
`SARP_CLOD_PAGE_CREDIT_RETRY_BUDGET` control this path. Pinned groups and GPU
DirectStorage destinations continue to reserve their required physical pages
before submission.

The same mode can be enabled for a normal demo launch by setting
`SARP_CLOD_REQUEST_TRACE_OUTPUT` to the desired JSON path. Tracing is disabled
when the variable is unset. Completed traces are capped at 100,000 records;
the report includes a dropped-record count if that bound is reached.

### 1) Build/install `BasicRHI`

```powershell
cmake -S BasicRHI -B out/build/rhi -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/rhi
cmake --install out/build/rhi --prefix out/install/rhi
```

### 2) Build/install `OpenRenderGraph` against installed `BasicRHI`

```powershell
cmake -S OpenRenderGraph -B out/build/org -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="out/install/rhi"
cmake --build out/build/org
cmake --install out/build/org --prefix out/install/org
```

### 3) Build `BasicRenderer` against installed packages

```powershell
cmake -S . -B out/build/renderer -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBASICRENDERER_USE_PACKAGE_DEPS=ON -DBASICRENDERER_ENABLE_SUBMODULE_FALLBACK=OFF -DCMAKE_PREFIX_PATH="out/install/rhi;out/install/org"
cmake --build out/build/renderer
```

## USD selection notes

Top-level CMake now supports explicit USD package selection:

- `BASICRENDERER_USD_VARIANT=dbg|rel`

When unset, it auto-selects `rel` for multi-config generators and `dbg` only for single-config Debug builds.


