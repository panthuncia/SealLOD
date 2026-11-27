# Renderer boundaries

## Contracts and ownership

Supported headers live under `include/BasicRenderer/`: host/runtime, Scene, Assets,
Streaming, Pipeline, Extensions, and Diagnostics. `Runtime/Detail` is compile-visible
implementation needed by existing by-value members, not an external entry point.
Private code lives under `src/`. Member ownership and cleanup order are significant;
classes are not PImpl'd.

Scene ingestion is serialized before frame preparation. Static requests express
bulk/asynchronous work and artifact readiness, not immediate GPU residency. The async
graph coordinates artifact production; immutable publications retain resources for
accepted frames. Geometry residency, page retirement, GPU completion, and shadow
notifications each have their own owners and ordering. Graph topology changes do
not end persistent producer lifetimes. Producers join before their request services
and device-bound resources are destroyed.

The demo uses only supported public entry points and includes no private renderer
headers. The renderer's device integration uses two ORG lifecycle-manager headers
directly; these are cross-package implementation dependencies, not public consumer access.

## Cross-subsystem dependencies

Implementation edges between subsystems have specific responsibilities: renderer
composition wires feature owners; graph adapters register local passes and coordinate
geometry, virtual shadows, and transparency; pipeline and runtime resource services
supply device, PSO, and buffer facilities; asset import uses processing and cache
implementations; diagnostics reads feature state for menus and telemetry. Generic
state-graph and scheduler code does not depend on feature managers.

## The frame graph

The renderer owns one `org::GraphHost` (`m_frameHost`, set up in `RendererLifecycle.cpp`).
The frame is four host epochs, each a graph of its own, executed in this order every frame
(`include/BasicRenderer/Extensions/FrameEpochs.h`): Visibility, Shading, Post and Overlay.
A fifth epoch, `Renderer::kUploadEpoch`, holds only the host's upload pass. It runs on frames
held back for publications (`SubmitUploadsWithoutFrame`).

- **Revisions.** Nothing builds a graph inline. Settings, resizes, pipelines and capacity growth
  post revisions (`FrameRevisions`, `RequestGraphRebuild`). The render thread consumes them once
  a frame at the consolidation point, `Renderer::ConsolidateFrameGraph`.
- **Builds.** A build runs on the build thread (`FrameBuildWorker`, `GraphHost::BuildEpochGraph`)
  from a settings snapshot and a render-thread snapshot of its inputs (`MakeFrameBuild`). The
  attached graphs keep rendering meanwhile. Its outputs are published at the attach
  (`PublishFrameBuild`). The finished set attaches as one staged set
  (`GraphHost::AttachStagedEpochGraphs`), so a frame never mixes graphs from two builds. A
  replaced graph is destroyed on the runtime's deleter thread. `BASICRENDERER_SYNC_FRAME_BUILD=1`
  runs the same build on the render thread, for debugging.
- **Placement.** ORG's frame placement places every epoch's transients from the frame's usage
  summaries, so a transient that several epochs share is placed once over its span. A build's
  placement is staged on the build thread. Its version keeps the running graphs' resources where
  they are.
- **Kept recordings.** Visibility, Shading and Post keep their recordings
  (`GraphHost::SetKeptEpochs`). A run reuses each batch while its passes' revisions are unchanged,
  and records only stale batches, with a named reason. A build records the kept epochs' copies on
  the build thread before the attach (staged recordings). Overlay records every frame.
  `BASICRENDERER_KEPT_RECORDINGS=0` (and an NVPerf capture) turns keeping off.
- **Resolution.** Window size and upscaler changes are a resolution revision, taken once a frame
  with no build in flight. They build a staged set like any other revision. The attach switches the
  resolution objects (`SwitchResolutionObjects`), and that frame's present resizes the swapchain
  on the submission thread (`ResizeSwapChainAtPresent`), without a device wait.
- **Submission.** The render thread runs each epoch's update and preparation (`ExecuteFrame`).
  The host's submission thread does the uploads, admission, recording, queue submission and the
  present (`EnqueuePresent`). `GetTotalFramesRendered` counts `Render` calls;
  `GetPresentedFrames` counts presents that ran.
- **Guards.** `GetFrameGuardCounters` counts builds, staged recordings found stale at their first
  run, re-records with no named reason, `StallPipeline` calls, submission drains and device-wide
  waits. In a steady frame they stay at zero; `renderer.revisions` reports them.
  `StallPipeline` is called only at shutdown and shader reload. `BASICRENDERER_CHECK_BUILD_SETTINGS=1`
  reports a live setting read on the build thread.

ORG's side of this is in `OpenRenderGraph/docs/persistent-epochs.md`. The plan that built it is
SARP's `docs/frame_graph_composition_plan.md`.

## Placement rules

Folders are subfeature-first: `VirtualGeometry/Streaming/RenderPasses`, for example.
Private headers accompany their implementations. Pass-local bindings and recording
helpers go with the passes; CPU state, residency, scheduling, and import logic stay
outside `RenderPasses`. Graph adapters live with their feature. Shaders live under
`BasicRenderer/shaders/`. Cross-feature graph composition is an intentional dependency.

| Location | Contents |
| --- | --- |
| `Runtime/Renderer` | Runtime composition |
| `Runtime/StateGraph` | Generic async machinery |
| `Runtime/Publication` | Publication |
| `VirtualGeometry/*/RenderPasses` | Culling, streaming, rasterization, Reyes, and voxel passes, each in its own directory |
| `Reyes/Tessellation` | Reyes CPU tessellation |
| `Lighting/Clustered` | Lighting cluster generation |
| `Assets/` | Format adapters, geometry processing, representations, and caches |
| `Assets/Textures/RenderPasses` | The four texture pass types nested in `TextureFactory`; the factory and its persistent job state stay outside |

Feature producers live with their owners.

USD import is split into stage entry, material/texture conversion, geometry
preprocessing, skeleton/animation conversion, and asset assembly; `USDLoader.cpp`
owns traversal and model/payload entry orchestration.

ClusterLOD clustering, hierarchy, assembly, node skinning, page packing and its
telemetry, voxel fallback and voxel-only construction, voxel packing, and validation
are separate units. `ClusterLODBuildState` and the bit writer/size calculator
shared with clustering are private and inline where used in packing loops.

## Packaging

All exported and detail headers compile in individual units without the renderer
PCH or SARP's dependency umbrella (`BasicRendererPublicHeaderSmoke`), and each API
category has a build-tree consumer (`BasicRendererApi<Category>Smoke`).

Root and inner CMake entry points both configure with installed first-party packages
and `BASICRENDERER_ENABLE_SUBMODULE_FALLBACK=OFF`. The inner standalone entry needs
the same external package inputs as the root: SARP's vcpkg installation and the
selected USD package directory. BasicRenderer itself does not export an installed package.

## Validating a change

Run SARP's `build.cmd 1` from the SARP root, one build at a time. Structural moves
need a build. Function-body relocation also needs the relevant tests and the
radius-100 exit-on-stability harness; trust its report and leave visual validation
to the owner.
