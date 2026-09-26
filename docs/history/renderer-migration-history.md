# Renderer boundaries and migration

This document describes the current ownership model. The migration changes
declaration ownership and source organization, not runtime behavior.

## Consumers and dependency inventory

`renderer-header-inventory.json` records renderer headers and their direct
consumers in SARP, the demo, cache/import tools, plugins, and first-party
libraries. Categories describe the intended boundary; a category does not make
a concrete manager a supported API. Vendored headers are excluded.

Run `python scripts/Audit-RendererBoundaries.py` to reject newly introduced
legacy include edges. `--prune` removes resolved exceptions and cannot accept
new dependencies. The inventory is deliberately retained throughout migration.

SARP consumes five capabilities: renderer lifecycle and presentation; dynamic
scene ingestion; asynchronous static scene production; grass/wind and shadow
extensions; and offline asset import/cache processing. Diagnostics are shared
by these consumers. The contributor C ABI remains a separate versioned boundary.

## Scene ingestion and publication

Scene mutation is ordered before frame preparation. SARP calls
`WaitForAsyncPreparation` before mutating scene state that preparation may
still use. `SceneRenderBridge` exports and ingests snapshots, while the scene
ingestion services materialize renderer state. Static producers receive the
restricted `StaticSceneIngestionAccess` capability set rather than renderer ECS,
views, lights, or frame objects.

Static request services delegate to persistent geometry, object, material, and
workload storage. Bulk reservations, materialization, uploads, and publication
are distinct stages. A request being accepted does not imply GPU residency.
Artifact identities, revisions, readiness, and geometry coverage preserve the
relationship between prepared draw records and their geometry.

`AsyncStateGraph` coordinates dependency-driven artifact production.
`RendererStateRequestService` assembles ready roots; `RendererStatePublisher`
selects coherent immutable publications. Published ownership holds and GPU
submission records survive the producer work that created them.

## Accepted frames

`RendererFrameInputs` retains immutable update/render values for one accepted
logical frame. Its construction validates matching frame identity. Frame work
uses the selected publication/manifest lease and captured settings, not mutable
ingestion state. A logical ownership slot and a swapchain image index are
different identities and must remain different during migration.

## Geometry streaming

Import and geometry processing produce cluster/cache data. Mesh storage owns
geometry registration and disk-streaming operations. CLOD streaming consumes
GPU feedback and controls request priority, parent dependencies, physical-page
availability, upload batches, residency promotion, and retirement.

The streaming drain is an ordered owner of this state. Readback staging slots
are published by graph work and decoded by the streaming worker after their
fence completes. Upload tickets distinguish published, claimed, submitted,
completed, and cancelled work. Cancellation can require replay of the same
retained batch; page retirement cannot be replaced by immediate reclamation.

Virtual shadows observe geometry dependencies and receive upgrade/invalidation
notifications. Graph extensions schedule the rendering and transfer passes;
they do not define the lifetime of all persistent geometry state.

## Lifetimes and teardown

Device/session resources, graph-generation bindings, and accepted-frame owners
have distinct lifetimes. `ProducerPersistentState` survives topology changes;
CLOD exposes graph-resource shutdown separately from complete shutdown.
The state type now has the qualified
`BasicRenderer/Extensions/ProducerPersistentState.h` contract; its former
`Render/` path forwards for compatibility. SARP's producer core, plugin, and
cache tests consume BasicRenderer headers through `BasicRenderer::Headers`
target usage requirements instead of target-local renderer include paths.

`Renderer::Cleanup` encodes the teardown order. Producers must join before the
request service and upload/descriptor generation are destroyed. Publication
holds and version families must release device-bound roots before device
services disappear. The migration must preserve the existing explicit cleanup
sequence and member destruction order; no singleton or scheduler redesign is
part of this work.

## Migration gates

0. Inventory, boundary ratchet, baseline build/tests/scene benchmark.
1. Manager-independent static ingestion and geometry storage contracts.
2. Honest BasicScene packaging and format-neutral imported-asset descriptions.
3. Qualified public headers and migrated consumers.
4. Private headers colocated by subsystem, one build-validated move at a time.
5. Mechanical translation-unit extraction retaining owners and call order.
6. Enforced CMake include/package boundaries and removal of temporary aliases.

Build validation always uses SARP's `build.cmd` from the SARP root. Its
post-build deployment can run even with `SARP_SKIP_INSTALL=1`; the renderer
must be closed before a build that replaces its executable.

Run CTest against the configured SARP tree after the build. Public-header smoke
targets must not use the renderer PCH or SARP's dependency umbrella. Runtime
validation uses the existing MO2 scene harness with identical arguments and
configuration and `--benchmark-auto-exit`. Trust the generated stability report;
visual validation is performed by the owner. Purely structural changes require
build checks; run full runtime validation for changes where extraction mistakes
could affect behavior. Build/test success alone is not evidence of equivalent
rendering.

## Baseline, 2026-09-25

- Working tree initially clean except for existing untracked content in PyNifly.
- SARP `build.cmd` (`SARP_SKIP_INSTALL=1`, normal configuration): passed.
- SARP CTest, RelWithDebInfo: 41/41 passed.
- Build log: `../build/renderer-migration-baseline-build.log`.
- Test log: `../build/renderer-migration-baseline-tests.log`.
- Runtime baseline: `scripts/sarp_mo2_scene_benchmark.cmd --radius 100 --benchmark-auto-exit`
  reported `status=stable`, `scene_stable=1`, blocker mask `0x0`,
  120 stable frames, elapsed 12,502 ms. Initial startup required shader compilation.
  The MO2 launcher reported exit code -805306369 despite the stable report;
  record this pre-existing exit result separately from scene readiness.
- Runtime report: `../build/renderer-migration-baseline-scene.txt`.

The contract, subsystem, and diagnostics changes passed a full SARP build and
42/42 registered tests. The same radius-100 scene reported stable with zero
blockers in 11,737 ms; 40,401 active cells, 212,305 applied placements,
99,228 live static objects, 2,106 successful assets, 6,870 meshes, and the
placement-set digest matched baseline. See
`../build/renderer-migration-matched-scene.txt`. A radius-13 run also reported
stable, but its counts are not comparable to this baseline.

Current structural checks: the complete SARP build passes after moving the
transparency, virtual-shadow, Reyes, voxel, CLOD streaming, culling,
rasterization, graph-integration, ray-tracing, and menu implementation groups.
The geometry-storage interface and MeshManager adapter are private; their
unchanged request/result values are public. `CLodCacheTool` builds with the
relocated shared Reyes sources. The renderer has an explicit list of 201
implementation sources, checked against the source tree, and
`BasicRendererPublicHeaderSmoke` compiles each of the 53 published headers in
its own translation unit without a PCH. The boundary audit passes with 167
grandfathered legacy include edges remaining. `Renderer.h` now forward-declares
its pointer-owned concrete managers; its existing member ownership and cleanup
order remain intact.

NvPerf capture contracts, the statistical sampling data/API, and the sampling
control server now live under `BasicRenderer/Diagnostics`. SARP host and
preprocessor consumers and renderer sources use the qualified paths; old
`Telemetry/` headers forward to the new declarations. The public target now
declares its nlohmann JSON dependency for the control-server header. The full
SARP build and all three isolated diagnostics-header units pass; the boundary
audit dropped from 176 to 167 edges. Build log:
`../build/renderer-migration-public-diagnostics-contracts.log`.

The CLOD streaming implementation has begun its Phase 5 split. Pending-request
state transitions and priority-heap admission/removal now live in
`src/VirtualGeometry/Streaming/CLodStreamingAdmission.cpp`; ready-completion
page-credit and parent/page waiter methods live in
`src/VirtualGeometry/Streaming/CLodStreamingPageWaiters.cpp`; disk completion
processing lives in `CLodStreamingCompletions.cpp`; and request event recording
and report writing live in `CLodStreamingTrace.cpp`; domain snapshot rebuilding
and event handling live in `CLodStreamingDomain.cpp`; feedback readback decoding
lives in `CLodStreamingFeedback.cpp`; virtual-shadow dependency tracking and
publication live in `CLodStreamingShadowNotifications.cpp`. The shared
completion accounting, invalid-page sentinel, and cached environment settings
remain private in the streaming internals headers. The moves preserve ownership,
synchronization, scheduling, and call order. The main implementation is now
about 5,400 lines. SARP `build.cmd` passes after each split; the latest build log
is `../build/renderer-migration-clod-feedback-split.log`.

`ReadbackManager` now resides under `src/Runtime`; its only callers are renderer
implementation files. `MaterialTextureTransferService` and the material
ownership guards now reside under `src/Materials`. The ownership-guard test has
narrow private access to `src/`. These moves passed SARP `build.cmd` target
builds, and the ownership-guard test passed.
`IndirectCommandBufferManager` now resides under `src/Runtime`; SARP's
live-scene bridge no longer includes that unused concrete manager header.
`LightManager` now resides under `src/Lighting`. The public scene and forward
pass headers no longer include it, and the scene header spells its object count
as `unsigned int` instead of the equivalent Windows `UINT` typedef. The full
SARP build and independent public-header compilation pass after these moves.
ProceduralWind consumes accepted pose and renderer-state publications plus the
existing wind-palette reservation service; it does not access object, skeleton,
or view manager instances. Its unused manager includes, and grass's unused view
manager include, were removed. `SkeletonManager` and `ViewManager` now reside
under `src/Animation` and `src/Scene` respectively. The downsample pass also
had an unused view-manager include. Full SARP builds pass after each manager
move. Wind and grass now use public extension access for the render device,
shared indirect-command signatures, shader compilation, pipeline creation and
registration, and typed setting reads. The functions forward to the existing
managers at the original call sites. Shader compilation request/result types
were moved unchanged from the PSO manager header. Frame-selected data still
arrives through accepted publications.

`EnvironmentManager` now resides under `src/Lighting`; its three pass headers
no longer include it unnecessarily. Terrain material and region descriptions
are public in `Scene/TerrainTypes.h`, while `TerrainManager` resides under
`src/Terrain`. Texture-streaming stats are public diagnostics values, while
`TextureStreamingManager` resides under `src/Materials`; two
`MaterialManager` forwarding methods moved out of its header to allow the
concrete type to be forward-declared. SARP's static producer now calls the
public material texture-gate request function, and `MaterialManager` resides
under `src/Materials`. The unchanged `SerializedTaskPump` is a public streaming
coordination type. SARP host, host core, and preprocessor obtain renderer
include paths through the BasicRenderer target rather than manual paths.
`ObjectManager` now resides under `src/Scene`, and `MeshManager` under
`src/Mesh`. SARP static import invokes the existing packet-build and
batch-finalization operations through `StaticObjectRequestService`; its
request/result values remain the same public types. Static-state artifacts
and their test now name `StaticMeshTemplateRef` directly. Two render-pass
headers no longer include `MeshManager` unnecessarily.

The object-Reyes-atlas and terrain-RVT telemetry request methods, including
their readback decoding helpers, now reside in
`src/Diagnostics/RendererTelemetryRequests.cpp`. The method bodies are unchanged;
the renderer still invokes them at the same point in frame orchestration and
retains the same readback flags and callback ownership. The SARP full build
passes, and the matched radius-100 exit-on-stability report is stable with zero
blockers and baseline-identical deterministic counts and placement digest. See
`../build/renderer-migration-telemetry-extraction-scene.txt`.
The CLOD visibility and virtual-shadow telemetry request methods now reside
in `src/Diagnostics/RendererCLodTelemetryRequests.cpp`, with their existing
readback code and CLOD-specific enablement helpers. Their member definitions
match the original text, and frame call sites and state remain with the
renderer. A full SARP build and matched radius-100 exit-on-stability run pass;
the report has zero blockers and baseline-identical deterministic counts and
placement digest. See `../build/renderer-migration-clod-telemetry-scene.txt`.

`FFXManager`, `UpscalingManager`, the retained upscaling generation service,
and its render pass now live under `src/PostProcessing`. The upscaling mode,
quality enum, and display-name values retain their global names and values in
the standalone public `Pipeline/UpscalingTypes.h` contract. SARP's benchmark
host names the quality enum through that contract instead of the concrete
manager. The complete SARP build and independent public-header smoke target
pass after the move. The boundary audit now reports 228 legacy include edges.
The SSSR generation service and pass header also reside under
`src/PostProcessing` beside `FFXManager`; the full SARP build passes after
that separate move.

The unchanged texture semantic, normal-map, and stochastic-artifact values now
reside in public asset contract headers. SARP's asset bridge requests the
existing blocking artifact operation and cache-path lookup through
`TextureProcessingRequestService`. `TextureProcessingManager` remains the
owner and now lives under `src/Assets`; no SARP source includes its concrete
header. The full SARP build and independent public-header smoke target pass.
The matched radius-100 exit-on-stability report is stable with zero blockers,
baseline-identical counts and placement digest, and no asset blocker rows. See
`../build/renderer-migration-texture-processing-scene.txt`.
The current OpenRenderGraph DebugUI source obtains its preview device and
graphics queue through callbacks set by the renderer menu at the existing
inspector draw site; its DebugUI target does not link BasicRenderer.

`Renderer::SetSettings` now resides in `src/Runtime/RendererSettings.cpp`;
the 636-line definition is text-identical to its former body. Its settings
helpers are shared with the remaining renderer implementation through a small
private header. The full SARP build passes, and the matched radius-100 report
is stable with zero blockers and baseline-identical counts and placement
digest. See `../build/renderer-migration-settings-extraction-scene.txt`.

The header-only forward render pass now lives at
`src/Lighting/ForwardRenderPass.h`; only the private render-graph build helper
uses it. `Mesh.h` no longer includes ORG's `DeletionManager` shim because it
does not reference that type. The full SARP build passes after both structural
header changes.

`RendererECSManager` and the unused BasicRenderer `ECSManager` now reside under
`src/Scene`. The sole in-tree test that uses the former has narrowly scoped
private include access. `TerrainRvtPasses.h` moved under `src/Terrain` with the
terrain graph implementation.

`DirectStorageManager` now resides under `src/Runtime`; its asynchronous
request handle is a small public streaming contract because texture
declarations retain it by value. `MeshManager` consumes that handle and a
private DirectStorage copy descriptor without including the manager itself.
The full SARP build and 30-header smoke target pass after the move.

`RenderPhase`, `MaterialCompileFlags`, and `DrawWorkloadKey` now have public
pipeline contract headers. `TechniqueDescriptor` consumes those value types,
and the public object/workload request declarations include them directly
instead of importing the renderer's workload helper header. The full SARP
build and 32-header smoke target pass after this extraction.

`RendererSettings.h` now has a public pipeline path. SARP's host, grass
extension, and ProceduralWind consumers use the qualified include; the old
`Render/` path remains a forwarding header. The public-header smoke target
compiles this contract independently.

The visibility utility pass headers now live under
`src/Materials/Passes/VisUtil`, and `TerrainRvtPasses.h` lives under
`src/Terrain`. `CommandSignatureManager` and its implementation live under
`src/Runtime`. `PipelineRecipeTests` has private source access for its internal
pass tests; the renderer target and that test build, and the focused test
passes. The public header smoke count remains 33 and the include audit passes
with 217 legacy edges.

`Renderer.h` no longer includes the full scene or render-graph declarations,
the unused DLSS/telemetry headers, or state-publication headers needed only for
pointer-owned members. It uses the public scene component contract and forward
declarations while keeping all by-value members and their ownership unchanged.
The renderer implementation now includes its implementation dependencies directly, and the
static import coordinator and pump include `RendererStateRequestService` where
they call it. The full SARP build and all 35 public-header smoke units pass.

`RendererStateRequestService` now has a public streaming header and SARP's
grass, live-scene, and static-streaming consumers use the qualified path. Its
publication fragment kind and count are in `PublishedTypes.h`, allowing the
service declaration to forward-declare the full publication objects. The old
`Render/` include remains a forwarding header. The full SARP build,
`AsyncStateGraphTests`, and all 35 public-header smoke units pass; the boundary
audit now has 215 legacy include edges.

The task scheduler declarations now have a public streaming header. SARP
host, preprocessing, grass-cache, and plugin consumers plus renderer internals
use that qualified include. The former manager header remains a forwarding
header until final compatibility cleanup.

Cache path declarations now live in the public `Assets/CachePaths.h` contract.
SARP host, preprocessing, and cache-tool callers and renderer import/cache
implementations use the qualified path; the prior utility header forwards to
it for compatibility. The standalone public-header compile set now includes
this contract. The boundary audit passes with 195 legacy include edges.

SARP's `SARPBasicRendererTests` aggregate now depends on
`BasicRendererPublicHeaderSmoke`, making the independent header compile target
reachable despite the renderer subdirectory being excluded from the default
build. The full SARP build compiled the smoke set and the target also builds
directly through `build.cmd`. `TaskSchedulerManagerTests` passes after the
scheduler contract migration. Build log: `../build/renderer-migration-cachepaths-public.log`.

`Environment` is now declared in the public BasicRenderer scene API and its
implementation includes that contract directly. The renderer-specific header
has been removed from BasicScene's exported include tree; the old BasicRenderer
`Scene/Environment.h` path forwards to the new public path. The SARP full build
compiled the new standalone header unit and passed. Boundary audit: 194 legacy
include edges. Build log: `../build/renderer-migration-environment-public.log`.

The D3D12 indirect-command record layouts used by the grass and wind extensions
now live in `BasicRenderer/Extensions/IndirectCommand.h`. Renderer and SARP
consumers use that supported path; the old `Render/` path forwards to it. Its
public-header smoke unit compiles independently. The boundary audit passes
with 191 legacy include edges. The full SARP `build.cmd` and its public-header
aggregate pass; build log: `../build/renderer-migration-indirect-command-public.log`.

Generated builtin resource identifiers now have the supported
`BasicRenderer/Extensions/BuiltinResources.h` path. The legacy `Render/`
wrapper and extension-facing renderer declarations use it, while implementation
files may continue including the generated header directly. The public-header
smoke target explicitly depends on resource generation. The full SARP build
and public-header aggregate pass; the boundary audit has 189 legacy include
edges. Build log:
`../build/renderer-migration-extension-resources-public.log`.

Pose state publication and versioned GPU-buffer request declarations now have
qualified streaming headers. ProceduralWind and grass use those paths, as do
renderer internals and state-graph tests. `PoseState.h` forward-declares the
async graph instead of including its implementation-facing header. The full
SARP build compiled both standalone header units; `AsyncStateGraphTests` and
the boundary audit pass with 186 legacy include edges. Build log:
`../build/renderer-migration-streaming-publications.log`.

Published renderer snapshots, fragments, and resource-catalog queries now
have a qualified `BasicRenderer/Streaming/PublishedRendererState.h` contract.
GrassRuntime includes it directly for catalog selections; other consumers use
the same public path. Its former `Render/` header is a compatibility forwarder.
The versioned-buffer header now depends only on public artifact and publication
vocabulary, not the full graph or renderer-state headers. The full SARP build
and all 43 public-header smoke units pass; `AsyncStateGraphTests` passes, and
the boundary audit is down to 185 legacy include edges. Build log:
`../build/renderer-migration-published-state-public.log`.

The output/debug visualization enum and its display labels now have a public
diagnostics header. `PSOFlags`, required by the public material declaration,
now has a qualified pipeline header. Both old `Render/` headers forward to
their public paths, and all in-tree users now include those paths directly.
The full SARP build and 45-header smoke target pass; the boundary audit reports
182 legacy include edges. Build log:
`../build/renderer-migration-diagnostics-and-pso-contracts.log`.

`RendererHostDependencies.h` imports workload keys and compile flags from the
public pipeline contract. Mesh/material workload derivation and material
evaluation variants now have qualified pipeline contracts; the old
`Render/DrawWorkload.h` is a compatibility forwarder. Static import
preparation and streaming use the public workload helpers. The material
evaluation helper preserves the existing skinning bit, displacement predicate,
telemetry flag, and shader-key mask. The full SARP build and focused material
variant test pass, and the radius-100 exit-on-stability scene run reported
stable with zero blockers and the same placement digests and 7,894 direct
templates as baseline. See `../build/renderer-migration-material-eval-public-consumers.log`
and `../build/renderer-migration-material-eval-contract-scene.txt`. The
boundary audit passes with 177 legacy include edges.

An earlier full SARP build and 42/42 tests passed. A second matched radius-100
exit-on-stability run after moving the constructor out of line reported stable
with zero blockers in 16,016 ms, and matched the baseline deterministic scene
and asset counts and placement-set digest. See
`../build/renderer-migration-latest-scene.txt`. The extension access migration
also passed a full SARP build and the matched radius-100 exit-on-stability run:
stable, zero blockers, and baseline-identical scene counts and placement digest.
See `../build/renderer-migration-extension-settings-scene.txt`.
The latest full SARP build, public-header check, and boundary audit pass. In
the latest 42-test run, `OpenRenderGraphCopyQueueUploadTests` failed once and
passed when rerun alone; the other 41 tests passed. The matched radius-100
scene report after the texture and material-manager moves is stable with zero
blockers and baseline-identical counts and placement digest. See
`../build/renderer-migration-material-manager-scene.txt`.
After the object and mesh moves, the full SARP build, public-header check,
four focused tests, and boundary audit pass. The matched radius-100 scene
report remains stable with zero blockers and identical counts and placement
digest. See `../build/renderer-migration-object-mesh-scene.txt`.

Renderer graph construction now lives in `RendererGraphConstruction.cpp`,
separate from the remaining frame/runtime implementation. Its 471-line method
body moved unchanged, with the same member-owned state, call site, and
registration/compile/setup order. The 14 out-of-line definitions in the
included `RenderGraphBuildHelper.h` are explicitly `inline`, as appropriate for
header-defined functions; this prevents duplicate symbols as renderer work is
split across translation units. SARP's `build.cmd` passes and the boundary
audit remains at 167 legacy include edges. This was a structural-only change,
so runtime stability and visual validation were not repeated. Build log:
`../build/renderer-migration-renderer-graph-construction-split.log`.

SARP's two grass-cache targets now obtain the BasicRenderer header root
through `BasicRenderer::Headers`, rather than target-local include paths. The
header target is defined before those subdirectories so both targets can link
it at configure time. The only remaining BasicRenderer include-root reference
in SARP CMake is the one on that shared target. A serialized `build.cmd 1`
passes through post-build deployment, and the include-boundary audit remains
at 167 edges. The initial parallel build attempts encountered concurrent
post-build file-use races; serial validation is clean. Build log:
`../build/renderer-migration-graph-and-grass-include-targets-serial.log`.

At this point the SARP and tool consumers use the supported contracts, private
manager/pass headers live under `src/`, and the renderer lifecycle, frame,
diagnostics, menu, and CLOD streaming implementations have been decomposed
into focused translation units. The remaining boundary work is internal: reduce
the implementation dependencies still visible through `Renderer.h`, retire
the remaining in-tree legacy include spellings where safe, and keep checking
the exported target against installed-package consumption.

SARP host, preprocessor, scene-bridge, terrain-preprocessor, and grass consumers
now include the published scene and vertex-flag contracts directly. This
removes nine legacy include edges; `Audit-RendererBoundaries.py --prune` and a
second audit pass, leaving 158 edges. The full serialized SARP build and
post-build deployment pass. Build log:
`../build/renderer-migration-qualified-scene-contract-consumers.log`.

CLOD builder defaults and configuration-hash queries now have a public asset
contract at `BasicRenderer/Assets/DefaultCLodSettings.h`; the legacy mesh path
forwards to it. SARP's preprocessor, grass CLOD import paths, host settings
test, and the implementation include the qualified asset headers. The new
header is compiled independently by the public-header smoke target. After a
full serial SARP build and pruning resolved audit entries, the audit passes at
148 legacy include edges. Build log:
`../build/renderer-migration-public-clod-settings-contract.log`.

The imported `MeshData` payload now lives at
`BasicRenderer/Assets/MeshData.h`, with unchanged field order/types/defaults
and a legacy `Import/MeshData.h` forwarder. It now includes its direct standard
library dependencies and no longer imports vertex flags transitively. The
public CLOD asset contract and renderer/SARP users include the qualified asset
path. `Assets_MeshData.cpp` passes the standalone public-header smoke build;
the full serial SARP build and deployment pass. The audit is now at 145 legacy
include edges. Build log:
`../build/renderer-migration-public-mesh-data-contract.log`.

Frame task-graph telemetry declarations now sit beside their implementation in
`src/Telemetry`; the three obsolete `include/Telemetry` forwarding headers
were removed after their in-tree callers moved to `BasicRenderer/Diagnostics`.
The implementation includes its colocated header, so SARP's direct compilation
of that source does not need private renderer include paths. The serial SARP
build and deployment pass; the boundary audit remains at 145 edges. Build log:
`../build/renderer-migration-private-diagnostics-header-retry.log`.

After every in-tree consumer had moved to the qualified asset paths, the
obsolete `Import/MeshData.h`, `Mesh/ClusterLODTypes.h`, and
`Mesh/DefaultCLodSettings.h` forwarders were removed. SARP's full serial build
and deployment pass, and a source audit finds no remaining references to those
paths. Build log:
`../build/renderer-migration-remove-legacy-asset-forwarders.log`.

The vertex-flag enum's old `Mesh/VertexFlags.h` forwarding header is also
removed. Renderer code and tests now include the qualified scene contract;
the full serial SARP build and deployment pass, with no remaining references
to the legacy path. Build log:
`../build/renderer-migration-remove-vertexflags-forwarder.log`.

Eight concrete post-processing pass declarations have moved from the exported
include tree to `src/RenderPasses/PostProcessing`, alongside their renderer
implementation ownership. Renderer construction and its private graph-build
helper resolve them through the implementation include root. The SARP build
and deployment pass, and the boundary audit still passes at 145 consumer edges.
Build log:
`../build/renderer-migration-private-postprocessing-pass-headers.log`.

The three GTAO pass declarations are now private under
`src/RenderPasses/GTAO`; only the renderer's private graph-build helper uses
them. They are listed explicitly in the renderer target, and the serial SARP
build and deployment pass. The boundary audit remains at 145 external legacy
edges. Build log:
`../build/renderer-migration-private-gtao-pass-headers.log`.

The two FidelityFX pass declarations are now private under
`src/RenderPasses/FidelityFX`. Renderer construction, CLOD graph integration,
and the private graph-build helper continue to resolve them through the
implementation include root. The serial SARP build and deployment pass; the
boundary audit remains at 145 external legacy edges. Build log:
`../build/renderer-migration-private-fidelityfx-pass-headers.log`.

Ten renderer-owned frame, visibility, lighting, and debug pass declarations
now live under `src/RenderPasses/Core`. Prepared recording payloads remain
public for extension use. The serial SARP build and deployment pass, and the
boundary audit remains at 145 external legacy edges. Build log:
`../build/renderer-migration-private-core-pass-headers.log`.

The eight prepared recording/dispatch contract headers have moved from the
legacy `RenderPasses/` public path to
`BasicRenderer/Extensions/PreparedRenderGraph/`. Renderer code and
ProceduralWind now use the qualified path; the contract types and namespaces
are unchanged. SARP's serial build and deployment pass. Build log:
`../build/renderer-migration-public-prepared-graph-contracts.log`.

Skeletal animation contracts now live under
`BasicRenderer/Scene/Animation/`, and renderer, asset, and ProceduralWind
consumers use qualified includes. Seven independent public-header smoke units
compile each header without relying on a prior include. The serial SARP build
and deployment pass; the include-boundary audit is at 143 legacy edges. Build
log: `../build/renderer-migration-public-scene-animation-contracts-final.log`.

CLOD telemetry counters and published snapshots now use the qualified
`BasicRenderer/Diagnostics/CLodTelemetry.h` path. Renderer, host, grass, and
ProceduralWind consumers migrated, and the header has an independent public
smoke unit. The serial SARP build and deployment pass; the boundary audit is
at 139 edges. Build log:
`../build/renderer-migration-public-clod-telemetry-contract-retry.log`.

Shared GPU-facing data declarations from the root `ShaderBuffers.h` now use
`BasicRenderer/Extensions/ShaderBuffers.h`. Renderer, host, and ProceduralWind
consumers migrated without changing declarations or layouts, and the header
compiles independently in the public-header smoke target. The serial SARP
build and deployment pass; the boundary audit is at 134 edges. Build log:
`../build/renderer-migration-public-shader-buffer-contract.log`.

SARP's Preprocessor no longer includes `PSOManager` or `SettingsManager` to
initialize shader artifact compilation, precompile shader artifacts, or
register its fixed headless import defaults. Those existing operations now
sit behind the `BasicRenderer/Assets/ShaderArtifactCompilation` service;
shader request ordering, calls, and default values are preserved. The shared
host dependency header no longer includes the two singleton headers. The
independent public-header smoke target and full serial SARP build pass; the
boundary audit is at 130 edges. Build log:
`../build/renderer-migration-preprocessor-manager-service.log`.

RendererHost pipeline status and live compile-control requests now use
`BasicRenderer/Diagnostics/PipelineControl.h`. The public snapshot/job
vocabulary is shared with `PSOManager` through type aliases, while the public
functions delegate to the same manager calls. RendererHost no longer includes
the concrete PSO manager header; its pipeline reporting and request call sites
use the diagnostics API. The header smoke target and serial SARP build pass;
the boundary audit is at 129 edges. Build log:
`../build/renderer-migration-host-pipeline-control-contract.log`.

The four old `Scene/*.h` forwarding headers have been removed after converting
renderer internals to `BasicRenderer/Scene` and `BasicScene/MovementState`
includes. This keeps BasicScene's independently owned movement contract
separate from renderer scene types. Renderer-owned ECS component declarations
are public at `BasicRenderer/Scene/RendererComponents.h`; dependencies on them
are explicit in the renderer headers that require them. The serial SARP build
and deployment pass. Build log:
`../build/renderer-migration-remove-scene-forwarders-retry.log`.

Warnings already present include deprecated TBB task headers. These are not
changed as part of this migration.

RendererHost's typed setting reads and writes now use the public
`BasicRenderer/Extensions/SettingAccess.h` facade. It creates the same
`SettingsManager` getter/setter closures at the call site, preserving the
existing typed validation, captured accessors, notifications, and revision
updates. The public `Scene` and `Renderer` contracts now use the renderer-owned
`SettingSubscription` value type instead of importing the concrete settings
manager. The serial SARP build and deployment pass; the include-boundary audit
was at 128 legacy edges before the singleton header moves below.

The seven remaining singleton implementation headers (`DeletionManager`,
`DescriptorHeapManager`, `DeviceManager`, `PSOManager`, `ResourceManager`,
`SettingsManager`, and `TaskSchedulerManager`) now live in `src/Managers/Singletons`.
The exported `include/Managers` tree is down to `SerializedTaskPump.h`.
Public-header smoke uncovered and removed transitive manager includes from
`MaterialTextureStreaming.h`, `TextureFactory.h`, and
`Material.h`; settings-dependent texture behavior remains out of line with its
existing defaults and fallback. Renderer implementation files now include the
concrete managers they use, and CLOD setting reads use the typed settings
facade. The standalone public-header target and full serial SARP build and
deployment pass; the boundary audit reports 91 legacy edges. Build log:
`../build/renderer-migration-private-singleton-managers.log`.

The renderer's host-input API now lives at `BasicRenderer/Extensions/Input/`:
`InputAction`, `InputContext`, and `InputManager` retain their existing global
names and behavior, while `Renderer.h` includes the qualified `InputManager`
path. The old `include/Input` and `include/Managers/InputManager.h` locations
are removed, and the new manager header compiles in the standalone public
header smoke target. Full serial SARP build and deployment pass; the boundary
audit remains at 91 legacy edges. Build log:
`../build/renderer-migration-public-input-contract.log`.

Mesh vertex layout declarations and the mesh-instance batch factory now live
under `BasicRenderer/Assets/Geometry/`. SARP's renderer host, preprocessor,
terrain preprocessing, and renderer import/mesh consumers use the qualified
paths. Independent smoke units compile both headers, and the full serial SARP
build and deployment pass. The boundary audit is down to 83 legacy edges.
Build log: `../build/renderer-migration-qualified-geometry-contracts.log`.

Core mesh and instance declarations now use
`BasicRenderer/Assets/Geometry/{Mesh,MeshInstance}.h`, and the generic
`MaterialDescription` contract uses `BasicRenderer/Assets/MaterialDescription.h`.
The renderer host, Preprocessor, import pipeline, and renderer public headers
have migrated to those qualified paths; the old three header paths have been
removed. Three additional standalone smoke units compile the moved headers.
The full serial SARP build and deployment pass, and the include-boundary audit
is down to 76 legacy edges. Build log:
`../build/renderer-migration-public-mesh-assets.log`.

The import-settings declaration now lives at
`BasicRenderer/Assets/ImportSettings.h`; `USDLoader::ImportSettings` retains
its original type identity, with `br::import::ImportSettings` as an alias.
Model and NIF loader declarations use qualified
`BasicRenderer/Assets/Import/` paths, and the USD adapter declaration is at
`BasicRenderer/Assets/USD/USDLoader.h`. The NIF public header now depends on
the generic settings and imported-payload contracts rather than the USD loader
header. SARP host declarations use the generic contract, while the USD-specific
implementation includes the adapter directly. Standalone public-header smoke,
the full serial SARP build and deployment, and the boundary audit pass; the
audit reports 68 legacy include edges. Build log:
`../build/renderer-migration-public-import-contracts.log`.

The CLOD common extension declarations and setting-backed helpers now use
`BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h`. Renderer passes,
virtual-shadow integration, SARP grass, and the host use the qualified path;
the legacy `Render/GraphExtensions/ClusterLOD/CLodCommon.h` path is removed.
The independent public-header smoke target and full serial SARP build and
deployment pass. The include-boundary audit is down to 63 legacy edges. Build
log: `../build/renderer-migration-qualified-clod-extension.log`.

The texture value and CPU sampling contracts now live at
`BasicRenderer/Assets/{Texture,CpuTextureSampler}.h`. Renderer internals,
SARP, and the tools use the qualified asset paths; the old `Resources/` header
paths are removed. Both headers compile independently in the public-header
smoke target. The full serial SARP `build.cmd 1` build and deployment pass,
and the include-boundary audit reports 55 legacy edges.

The runtime material contract now lives at `BasicRenderer/Assets/Material.h`.
SARP host, preprocessing, and grass/terrain code plus renderer consumers use
the qualified path, and the old `Materials/Material.h` path is removed. Its
standalone public-header smoke unit compiles in the SARP build. The full serial
SARP `build.cmd 1` build and deployment pass; the include-boundary audit now
reports 50 legacy edges.

CLOD cache reads and writes now have a USD-free contract at
`BasicRenderer/Assets/CLodCacheStore.h`. The legacy loader header retains only
USD identity construction and includes the shared cache-store contract.
SARP's terrain and grass preprocessing consumers use the generic contract,
while renderer importers continue using the USD adapter. The scene frame
snapshot value types are now at `BasicRenderer/Scene/SceneFrameSnapshot.h`;
the old `Render/` path is removed. Both contracts compile independently in
public-header smoke. The full SARP `build.cmd 1` build and deployment pass,
and the include-boundary audit reports 44 legacy edges.

Shader variant request vocabulary and its request service declaration now live
at `BasicRenderer/Pipeline/ShaderVariantRequestService.h`. The renderer,
SARP preprocessing, and scene ingestion consumers use the qualified path;
the old `Render/` path is removed. Its public-header smoke unit and the full
serial SARP `build.cmd 1` build and deployment pass. The include-boundary audit
now reports 41 legacy edges.

The host-facing scene ingestion bundle and bridge declarations now live at
`BasicRenderer/Scene/{SceneIngestionServices,SceneRenderBridge}.h`. Extension
frame and update contexts now live at `BasicRenderer/Extensions/RenderContext.h`.
Renderer, SARP, and ProceduralWind consumers use the qualified paths, and all
three headers compile in public-header smoke. The full SARP `build.cmd 1` build
and deployment pass; the include-boundary audit now reports 35 legacy edges.

The scene-ingestion request service declarations now live together under
`BasicRenderer/Streaming/`: scene assets, static geometry, materials, objects,
and workloads. Renderer implementation units, SARP scene ingestion, the renderer
umbrella, and Scene.cpp include the qualified public paths; the stale legacy
object-service forwarding header was removed. Four newly isolated contract
smoke units compile alongside the existing object-service smoke unit. The full
serial SARP `build.cmd 1` build and deployment pass, and the boundary audit now
reports 26 legacy include edges. No runtime behavior was changed.

The asset-processing API group has been moved to `BasicRenderer/Assets/Import/`:
Assimp, glTF, USD geometry extraction, USD material-cache access, BRNifly,
CLOD cache access/loading, preprocessing payloads, prototype geometry, and
Reyes-atlas cache descriptions. Renderer import implementations, the standalone
CLOD cache tool, the SARP preprocessor, grass preprocessing, and public asset
headers now use those qualified paths. Ten import and payload declarations
compile independently through the public-header smoke target. The full serial
SARP `build.cmd 1` build and deployment pass; the boundary audit now reports 18
legacy include edges. This batch only relocates declarations and updates includes.

Material flag values now use `BasicRenderer/Assets/MaterialFlags.h`; SARP and
renderer consumers share the qualified contract, including the generic Material
header. CLOD geometry-building utilities now use
`BasicRenderer/Assets/Import/ClusterLODUtilities.h`, and the SGGX helper is
published at `BasicRenderer/Assets/SGGX.h` for geometry processing and grass.
Renderer settings consumers now include `BasicRenderer/Pipeline/RendererSettings.h`
directly, and the obsolete `Render/RendererSettings.h` forwarder is gone. The
three new asset/math public headers compile independently in the smoke target.
The full serial SARP `build.cmd 1` build and deployment pass; the boundary audit
reports 14 legacy edges.


Extension resource access declarations now use `BasicRenderer/Extensions/Resources/`:
`DynamicStructuredBuffer` and `PublishedStateResourceResolver`. Renderer internals,
ProceduralWind, and SARP grass use those qualified headers. The renderer resource
component and workload tags now live at `BasicRenderer/Extensions/ResourceComponent.h`;
the obsolete `Resources/components.h` path has been removed. Each contract has an
independent public-header smoke unit. The full SARP `build.cmd 1` build and
post-build deployment pass, and the boundary audit reports 11 legacy edges.


Static-scene artifact DTOs and immutable publication snapshots now have a
consumer-facing contract at `BasicRenderer/Streaming/StaticSceneArtifacts.h`.
SARP scene ingestion, residency, and streaming code use that contract directly.
The renderer?s producer-registration declaration remains in the private
`src/Render/StaticStateArtifacts.h` adapter, so graph wiring does not enter the
consumer API. Its standalone header smoke and the full serial SARP `build.cmd 1`
build/deployment pass. The include-boundary audit now reports 8 legacy edges.


Material usage requests and producer payloads now live in the public
`BasicRenderer/Streaming/MaterialRequests.h`, including the batch build input,
reservation, and published result used by SARP streaming. Renderer-only material
state construction and producer registration moved under `src/Render`. Published
object-buffer and indirect-workload snapshots now share
`BasicRenderer/Streaming/RendererStateArtifacts.h`; their build inputs and graph
registration remain private. SARP retirement observation and extension render
context use the snapshot API. Independent public-header smoke and the full serial
SARP `build.cmd 1` build/deployment pass. The boundary audit now reports 5
legacy edges.

The remaining implementation-only convenience headers in this batch are now
private: `Utilities/Utilities.h` moved under `src/Utilities/`, and
`spdlogStreambuf.h` moved under `src/Runtime/`. `VirtualShadowBudgetTests`
retains its existing helper coverage through a target-scoped private `src/`
include path. SARP grass now includes the public artifact request/observation
types directly rather than the concrete async state graph header. The full
serial SARP `build.cmd 1` build and deployment pass; the boundary audit passes
with 2 legacy edges, both in OpenRenderGraph's D3D12 external-resource code.
No runtime behavior was changed.

The remaining singleton manager code has been grouped by ownership. Device,
resource, deletion, descriptor, settings, and task-scheduler managers now reside
under `src/Runtime/`; PSO and shader-artifact managers plus their helper reside
under `src/Assets/`. Existing OpenRenderGraph forwarding contracts remain in
place, with their relative include paths adjusted for the new location. SARP's
grass-cache and CLOD tool source lists now reference the relocated scheduler
implementation. The full serial SARP `build.cmd 1` build and deployment pass,
and the include-boundary audit remains at 2 OpenRenderGraph edges. This is a
structural-only migration.

The final two audit edges were OpenRenderGraph implementation and test includes
of BasicRenderer's `d3d12.h` shim. Both now include DirectX-Headers directly
through the existing BasicRHI usage requirement. The full SARP `build.cmd 1`
build/deployment pass, and the external-consumer legacy edge count is zero.

SARP no longer defines the renderer header-only usage target or its include
path. `BasicRenderer::Headers` is now defined by BasicRenderer and published
through the renderer target, so SARP's header-only consumers get the path from
its owner without linking the renderer library. The full SARP build passes
with this target arrangement.

The boundary audit now scans BasicRenderer implementation, public headers,
tests, and the existing sibling consumers. It checks both public-header legacy
paths and private `src/` header paths against an explicit inventory; new edges
fail the audit. Current audit result: zero external legacy include edges and
1,372 inventoried internal legacy edges.

Nineteen implementation-only `Render/` headers with no public-header dependents
moved from `include/Render/` to `src/Render/`. The material-evaluation test
received target-scoped private source access where it directly uses two of the
moved headers. The full SARP `build.cmd 1` build and deployment pass; the public
header smoke target also builds independently of the renderer PCH.


Thirteen additional implementation-only headers moved into `src/`: direction and
setting helpers, FidelityFX adapters, resource resolvers, bounded queues, hash/math
helpers, processed-texture cache, budget allocator, generation mailbox, interop
validation declarations, and shader hooks. `Interfaces/ISetting.h` was also moved
under `src/Interfaces/`. The bounded-queue test and opt-in interop-validation target
now have private source include paths. The full SARP `build.cmd 1` build and
deployment pass. The expanded boundary audit reports zero consumer legacy edges.

The validation target now uses only the private source include root; its header
is an implementation detail, so it no longer exports the general renderer
include directory. The final SARP `build.cmd 1` build and deployment pass, and
the boundary audit remains at zero external legacy include edges.


Light and view artifact DTOs, environment-work queues, wind-palette access, and
raster bucket flags now live under qualified `BasicRenderer/Streaming`,
`Extensions`, and `Pipeline` headers. Their producer registration functions
are private in `src/Render/StateProducerRegistrations.h`; the public DTO headers
no longer include the async graph implementation contract. `RenderPhase` consumers
now include its public pipeline path, and its obsolete `Render/` forwarder is
removed. Five new independent public-header smoke units compile.

Eight renderer graph adapters (CLOD variants/components/shared declarations,
IO extension, and readback capture) moved from `include/Render/GraphExtensions/`
to `src/Render/GraphExtensions/`, matching their implementation ownership. The
full SARP `build.cmd 1` build and deployment pass; the expanded audit reports
zero external legacy include edges and 1,340 internal edges.

Assimp and glTF loader declarations, the ObjectReyes atlas baker, skeleton
artifact cache and validation declarations, and `RenderGraphIOService` now live
under `src/`. They were only consumed by renderer implementation or its cache
test, which now receives the private source include root explicitly. The full
SARP `build.cmd 1` build and deployment pass; no runtime behavior changed.

Removed fourteen obsolete forwarding headers after checking that neither SARP
or any in-tree renderer consumer included their legacy paths. The supported
`BasicRenderer/...` headers remain the sole paths for those contracts. The
header inventory no longer treats the removed paths as compatibility surface.

The CLOD streaming translation unit now delegates physical-page identity,
residency, retirement, protection, eviction, and page-allocation member
definitions to `CLodStreamingPageResidency.cpp`. This is a mechanical split at
existing function boundaries; the same class owns the state and call order is
unchanged.

A second streaming split places pending residency promotion, stale disk-I/O
reconciliation, forced nonresidency, selected-parent readiness, and page-touch
definitions in `CLodStreamingResidencyCommit.cpp`. The SARP build and deployment
pass after both splits; the audit records only the three existing private
streaming headers included by this new translation unit.

The pending-load admission check and ancestor-chain enqueue method now live in
the existing `CLodStreamingAdmission.cpp` unit beside request-priority and
waiting-state operations. The complete SARP build and deployment pass; the
boundary audit remains at zero external legacy include edges.

Renderer scene activation/append and input-handler API definitions now live in
`RendererSceneApi.cpp`, with explicit includes for the scene, device, and manager
types they use. The full SARP build and deployment pass after this translation
unit split; no frame scheduling or scene lifecycle order changed.

Added `scripts/Test-InstalledPackages.ps1` and a clean consumer project for the
installed BasicRHI, BasicTelemetry, BasicScene, OpenRenderGraph, and
ORGModuleServices packages. It installs into a unique staging prefix and builds
against that prefix plus SARP's vcpkg package prefix. The first successful run
found and closed missing header-package exports, incomplete BasicRHI installed
headers and package dependencies, the flecs target-name alias, and OpenRenderGraph
public spdlog/UTF-8 requirements. The SARP build and installed-package consumer
both pass.

The BasicRenderer target interface now keeps Microsoft.GSL, Streamline headers,
and geometry-central private while retaining them for implementation builds.
BasicTelemetry is explicitly public because the extension contract includes its
telemetry API. SARP's renderer-host core and preprocessor now declare their own
geometry-central use, revealed by the full build after removing that accidental
transitive dependency. The SARP build passed after those consumer updates.

Renderer frame-fence signaling, frame-index advancement, pipeline stalling, and
cleanup now live in `RendererLifecycle.cpp`. The cleanup method was moved intact,
including its existing shutdown order. The new unit now includes only the
manager, runtime, and service declarations required by those methods. SARP's
full build/deployment and the include-boundary audit pass after this extraction;
the audit remains at zero consumer edges.

Global GPU lookup resources, fallback environment cubemaps, frame render-target
allocation, and RTV setup now live in `RendererResources.cpp`. Their resource
descriptions, creation calls, and existing lifecycle call sites are unchanged.
The SARP `build.cmd 1` build and deployment passed after the move; the
include-boundary audit reports zero consumer and 1,428 internal legacy include
edges, and the installed-package consumer still passes.

Pipeline-recipe mutation, deferred application, and failure rollback now live in
`RendererPipeline.cpp`. The existing locking, settings synchronization, recipe
rollback, and graph-rebuild flags remain in the same method bodies and call
order. SARP's full build/deployment, installed-package consumer, and boundary
audit pass; the audit reports zero consumer and 1,428 internal legacy include
edges.

The menu header's scene-explorer snapshot/edit/display methods moved to
`MenuSceneExplorer.cpp`. Its CLOD telemetry, frame-task graph, pass-timing, and
auto-alias diagnostic panels moved together to `MenuDiagnostics.cpp`. Menu
singleton access, initialization, input dispatch, and frame rendering moved to
`MenuRuntime.cpp`. Together these remove roughly 4,900 lines of non-template
method bodies from the header, reducing it from about 5,900 lines to about
1,000. SARP's full build/deployment passed, all 31 configured non-SARP tests
passed, the installed-package consumer passed, and the audit reports zero
consumer and 1,431 internal legacy include edges.

The renderer's frame `Render()` implementation moved to
`RendererFrameRendering.cpp`; frame preparation, graph execution, presentation
tail recording, presentation, and fence signaling remain in the same sequence.
The exception-note helpers moved with their only consumer, and the shared
OpenRenderGraph settings synchronization helper now has a private declaration
in `Runtime/RendererSettingsHelpers.h`. SARP's full build/deployment passed, all
31 configured tests passed, the installed-package consumer passed, and the
boundary audit reports zero consumer and 1,456 internal legacy include edges.

The scene-to-GPU synchronization stage now lives in `RendererSceneSync.cpp`.
It still builds and uses the same ECS queries, object/material buffers, camera
and light updates, and task scheduler calls in the original order. The SARP
build/deployment, all 31 configured tests, installed-package consumer, and
boundary audit pass; the audit reports zero consumer and 1,474 internal legacy
include edges.

Scene update and snapshot ingestion helpers now live in
`RendererSceneIngestion.cpp`, alongside the scene-to-GPU sync unit. The move
includes the stable-scene-identity lookup helper, kept translation-unit-local.
The three frame-task graph capture methods moved into the existing
`RendererDiagnostics.cpp` unit. SARP's full build/deployment, all 31 configured
tests, installed-package consumer, and audit pass; the audit reports zero
consumer and 1,493 internal legacy include edges.

The renderer's `Update()` frame orchestration now lives in
`RendererFrameUpdate.cpp`, separate from frame execution and presentation in
`RendererFrameRendering.cpp`. The existing update stages and their ordering are
unchanged. The no-op command-list probe is now a shared inline private helper,
and the settings synchronization implementation is shared through its private
declaration. SARP's full build/deployment, all 31 configured tests, the
installed-package consumer, and the boundary audit pass; the audit reports zero
consumer and 1,606 internal legacy include edges.

The latest follow-up moved the remaining `Renderer` lifecycle, graph
construction, resource, and scene-ingestion definitions out of the former
monolithic `Renderer.cpp`; that translation unit is removed from the explicit
source list. Architecture references now point at the split implementation.
The full SARP build and deployment pass, all 31 configured tests pass, the
installed-package consumer passes, and the boundary audit reports zero
consumer exceptions and 1,490 internal legacy include edges.

The renderer class definition now resides at its supported public path,
`include/BasicRenderer/Renderer.h`; all in-tree consumers already used that
qualified path, so the obsolete root-level `include/Renderer.h` forwarding
header was removed. Three unused headers classified as private implementation
were also removed from the exported root. SARP's full build/deployment, all 31
configured tests, installed-package consumption, and the include-boundary
audit pass; the audit reports zero consumer exceptions and 1,489 internal
legacy include edges.

The public header cleanup continues with the renderer class now defined directly
in `BasicRenderer/Renderer.h`, and the obsolete root-level `Renderer.h` removed.
Six asset-facing contracts (material blend state, material texture streaming,
technique descriptors, import file types, ClusterLOD shader types, and texture
residency) moved out of legacy top-level include folders and into
`BasicRenderer/Assets/`. Their in-tree includes use qualified paths. The
boundary audit dropped to 1,454 internal legacy include edges with no consumer
exceptions. This exposed an undeclared include usage: `SARPRendererHostCore`
headers include geometry-central, so that dependency is now public on the
host-core target. SARP build/deployment, all 31 configured tests, and
installed-package consumption pass.

The heavy `TextureFactory` declaration is now private beside
`src/Factories/TextureFactory.cpp`. Public `Texture` and `Material` contracts
forward-declare the factory; implementation units include its concrete header
directly. `Material.h` now directly includes the public shader-buffer contract
it uses, rather than receiving it accidentally through the factory header.
SARP build/deployment, all 31 configured tests, installed-package consumption,
and the boundary audit pass; the audit reports zero consumer exceptions and
1,459 internal legacy include edges.

The six low-level buffer contracts that remain part of renderer and producer
headers now live under `BasicRenderer/Extensions/Buffers/` instead of the
legacy `Resources/Buffers/` tree. In-tree consumers now use the qualified
extension paths, and each buffer header has an independent public-header smoke
translation unit. SARP build/deployment, all 31 configured tests, installed-
package consumption, and the boundary audit pass; the audit reports zero
consumer exceptions and 1,415 internal legacy include edges.

`AsyncStateGraph` is now private under `src/Render/`; the public `Renderer`
header forward-declares it and includes only artifact contracts for its trace
data. Renderer implementation units and graph-specific tests include the
concrete declaration directly. The two SARP producers that previously reached
the graph only to allocate suspension identities now use
`AllocateArtifactSuspensionIdentity()` from the artifact contract; the graph's
existing static entry point delegates to that same process-wide allocator.
This preserves identity allocation and removes a consumer dependency on the
graph class. SARP build/deployment and installed-package consumption pass. All
31 tests passed except `OpenRenderGraphCopyQueueUploadTests` on the first run;
it passed when rerun alone. The boundary audit reports zero consumer
exceptions and 1,419 internal legacy include edges.

The ABI-visible runtime support types that `Renderer` still stores by value are now
organized under `BasicRenderer/Runtime/Detail/`: depth-history publication,
scene-source storage and materialization, renderable residency, pose-instance
registration, frame timing, OpenPBR lookup resources, and material-evaluation
build inputs. This is a path and include-boundary move; the concrete types remain
unchanged so `Renderer` layout, ownership, and cleanup sequencing are preserved.
Each relocated header has an isolated public-header smoke translation unit.

SARP's `build.cmd 1` completed successfully after the runtime-detail header
moves, including the individual public-header compilation targets. The boundary
audit reports zero consumer exceptions and 1,386 remaining internal legacy
include edges. No runtime validation was needed for these path-only changes.


## Placement and extraction validation through the menu-panel batch

Use SARP's build.cmd from the SARP root, one build at a time. Deployment may still
run with SARP_SKIP_INSTALL=1. Pure path changes require a build and structural checks.
Changes that could affect execution/lifetime require relevant tests and the existing
exit-on-stability harness; trust its reports and leave visual validation to the user.
Keep cache formats, configuration defaults, algorithms, shaders, initialization,
shutdown, synchronization, and ownership unchanged. New scheduling/services and
renderer-library splitting are deferred.

The pass and CPU placement batches passed SARP build and deployment, respectively:
`migration-subsystem-pass-layout-verified.log` and
`migration-cpu-subsystem-layout-final.log` in SARP's configured build directory.
All 43 configured CTest tests passed after the CPU batch. The boundary audit reports
zero violations, ten reviewed exceptions, and no stale exceptions; all eleven audit
fixtures pass. No runtime report was required for these structural batches.

Individual header compilation exposed and corrected missing direct includes in
Filetypes, InputContext, and PreparedComputeCommands. CPU placement also exposed a
stale ModelLoader include and a source-sharing detail: SARP directly compiles the
frame-task telemetry implementation, so its sibling header uses a local include.
These failures were corrected before accepting the batch; no private include root
was added to external consumers.

Both renderer entry points now use `cmake/RendererDependencies.cmake`: existing
host targets take precedence, then packages, then the explicitly enabled in-tree
fallback. SARP's normal build/deployment passed with the shared discovery in
`migration-shared-dependency-discovery.log`; build directories and host options
were retained. Separate package-only entry-point validation remains to be done.

`scripts/Test-InstalledPackages.ps1` now configures and builds five separate minimal
consumers, each finding and linking only its requested first-party target. All five
passed. This exposed missing fmt/spdlog discovery in BasicRHI's package config;
the declarations were added without changing its link interface. These checks
establish header and exported-link-interface consumption, not runtime coverage.
BasicRenderer itself still has no installed-package support claim; its separate
build-tree category consumers remain pending.

Texture pass extraction passed SARP build/deployment in
`migration-texture-pass-extraction.log`. Its radius-100 auto-exit report is
`../build/migration-texture-pass-scene.txt`: stable, 120 stable frames, zero readiness
blockers, no failed/pending assets, and launcher exit 0. The baseline counts match:
40,401 active cells, 212,305 applied placements, 99,228 live static objects, 2,106
successful assets, 6,870 meshes, and placement digest 11950196757715935234.
Elapsed time was 15,987 ms versus the original single baseline's 12,502 ms; these
individual samples do not establish a timing distribution or performance equivalence.

Menu diagnostics now have individual VirtualGeometry, FrameTaskGraph, PassTiming,
AutoAlias, SceneExplorer, Environment, Output, and PostProcessing panel units under
`Diagnostics/Menu/Panels`. Prepared draw capture/recording is in
`Diagnostics/Menu/RenderPasses/MenuDrawData.cpp`. Member bodies, local static
initialization points, and the existing Menu owner/call sites were preserved.
The SARP build/deployment passed (`migration-menu-panel-extraction.log`), followed
by all 43 then-configured tests (`../build/migration-menu-panel-tests.log`). The
auto-exit report `../build/migration-menu-panel-scene.txt` is stable with zero
readiness blockers, 120 stable frames, launcher exit 0, and matching baseline
scene/asset counts and digest. Elapsed time was 10,799 ms. These runs do not exercise
every interactive menu operation; visual validation remains with the owner.

`scripts/Audit-RendererBuildInputs.py` checks all 228 renderer C++ sources against
the explicit source entries, and all 149 exported/detail headers against 149
individually listed smoke units. It rejects missing or stale entries and smoke
units that directly combine multiple renderer headers. It is registered as
`RendererBuildInputAudit`; the compiler remains responsible for header independence.
