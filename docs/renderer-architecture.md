# Renderer boundaries and remaining migration

This is the current status, not a chronological progress log. Earlier architecture,
baseline, and validation records are preserved in [migration history](history/renderer-migration-history.md).
The historical include inventory is retained there as well. Low private-include counts
are not an architectural success criterion.

## Contracts and ownership

Supported headers live under `include/BasicRenderer/`: host/runtime, Scene, Assets,
Streaming, Pipeline, Extensions, and Diagnostics. `Runtime/Detail` is compile-visible
implementation needed by existing by-value members, not an external entry point.
Private code lives under `src/`. Preserve member ownership and cleanup order; a full
PImpl conversion is deferred. The contributor C ABI is unchanged.

Scene ingestion remains serialized before frame preparation. Static requests express
bulk/asynchronous work and artifact readiness, not immediate GPU residency. The async
graph coordinates artifact production; immutable publications retain resources for
accepted frames. Geometry residency, page retirement, GPU completion, and shadow
notifications retain their current owners and ordering. Graph topology changes do
not end persistent producer lifetimes. Producers join before their request services
and device-bound resources are destroyed.

## Compatibility cleanup inventory

The approved cleanup batch removed:
- The empty `SettingsAccess` facade and singleton accessor; callers use the same
  getter/setter factories at the original capture/invocation points.
- Two unused immediate setting reads and eight unused immediate writes.
- The static-only texture-processing service class; the same blocking operations
  are now namespace functions delegating to the same backend.
- The duplicate pipeline-kind enum; registration and diagnostics share the original
  extension enum values and uint8 representation through a leaf Pipeline header.
- The graph/request-service suspension-ID wrappers, retaining one atomic allocator.
- The DrawWorkload, TerrainRvtTelemetry, and TaskSchedulerManager forwarding headers.
- Fifty migration aliases in mesh, object, material, and PSO managers. Existing
  aliases, including SkeletonManager's TransientWindRegion, remain.

The static-state registration header now forward-declares the graph; its implementation
includes the graph directly. This is an honest implementation dependency.

Retained: the used external device/pipeline entry points, setting subscription RAII,
existing scene/request services, and stateful environment/wind/material-transfer work.
These are useful boundaries or pre-existing services, not include-count workarounds.

SARP `build.cmd 1` and deployment passed after this batch; log:
`../build/vs2026-renderer-host/migration-compatibility-cleanup.log`.

## Boundary enforcement

Run `python scripts/Audit-RendererBoundaries.py`. The audit rejects external private
or detail includes, exported-header private dependencies (including transitive
first-party dependencies), and renderer access to other packages' private headers.
It does not forbid ordinary implementation includes. Directory dependency counts
are informational, not a cleanup target. Cross-subsystem implementation edges have
specific responsibilities: renderer composition wires feature owners; graph adapters
register local passes and coordinate geometry, virtual shadows, and transparency;
pipeline and runtime resource services supply device, PSO, and buffer facilities;
asset import uses processing and cache implementations; diagnostics reads feature
state for menus and telemetry. Generic state-graph and scheduler code may not gain
new feature-manager dependencies. The audit enforces the public/private boundary
and those infrastructure edges without forbidding feature integration.

`renderer-boundary-policy.json` records reviewed exceptions and reasons. Stale
exceptions fail. The demo now uses supported public entry points and has no private
renderer includes. The only two remaining exceptions are pre-existing ORG
lifecycle-manager headers used by the renderer's device integration. They are
recorded cross-package implementation dependencies, not public consumer access.
The renderer's internal validation target has explicit internal source status,
like its tests.

Run `python scripts/TestRendererBoundaries.py` for allowed/private, cross-feature,
relative-path, transitive leak, detail access, demo-role, and cross-package fixtures.
`--write-inventory <path>` writes classified dependencies without approving exceptions.
The audit is a source check; isolated header compilation verifies SDK/include setup.

## Placement rules

Use subfeature-first folders: `VirtualGeometry/Streaming/RenderPasses`, for example.
Private headers accompany their implementations. Pass-local bindings and recording
helpers go with the passes; CPU state, residency, scheduling, and import logic stay
outside RenderPasses. Graph adapters live with their feature. Shader files and paths
remain unchanged. Cross-feature graph composition is an intentional dependency.

The pass-placement batch moved 172 files; the CPU-placement batch moved 224 files.
The exact path mappings are recorded in [pass moves](architecture/pass-layout-moves.json)
and [CPU moves](architecture/cpu-layout-moves.json). Moved implementation bodies were
checked for equality apart from include directives. Public paths and shader paths
were retained. `src/Render`, the global `RenderPasses`, `Import`, `Mesh`, `Factories`,
`Menu`, and `Telemetry` no longer contain files. The four nested texture pass types
now have declarations and implementations under `Assets/Textures/RenderPasses`;
their owning factory and persistent job state stay outside that folder.

Runtime composition now lives in `Runtime/Renderer`, generic async machinery in
`Runtime/StateGraph`, and publication in `Runtime/Publication`. Feature producers
live with their owners. Culling, streaming, rasterization, Reyes, and voxel passes
have separate local `RenderPasses` directories; Reyes CPU tessellation lives in
`Reyes/Tessellation`. Lighting cluster generation belongs to `Lighting/Clustered`.
Format adapters, geometry processing, representations, and caches are under Assets.
USD import now has separate stage entry, material/texture conversion, geometry
preprocessing, skeleton/animation conversion, and asset-assembly units; the
remaining `USDLoader.cpp` owns traversal and model/payload entry orchestration.
ClusterLOD validation, voxel packing, page packing and its telemetry have separate
units. `ClusterLODBuildState` and the bit writer/size calculator shared with the
clustering code remain private and inline where used in packing loops.

## Completion checklist

- [x] Remove the approved redundant compatibility layers and migrate consumers.
- [x] Enforce public/private boundaries with allowed and forbidden fixtures. The two
      reviewed ORG lifecycle integration exceptions remain explicit.
- [x] Classify cross-subsystem implementation dependencies by responsibility rather
      than treating their raw include count as migration debt.
- [x] Place pass families in subsystem-local `RenderPasses` folders; empty the old
      `src/Render` and generic source buckets.
- [x] Migrate the demo to supported public entry points.
- [x] Extract menu panels, prepared draw capture, lifecycle, and memory helpers
      from the private menu header.
- [x] Split USD loading into material/texture conversion, geometry preprocessing,
      skeleton/animation conversion, asset assembly, stage entry, and traversal.
- [ ] Finish ClusterLOD geometry-processing decomposition. Validation, voxel packing,
      page packing, and telemetry are separate; clustering, hierarchy, fallback
      candidate construction, and assembly still share `ClusterLODUtilities.cpp`.
- [x] Compile all 149 exported/detail headers in individual units without the
      renderer PCH or SARP dependency umbrella.
- [x] Consolidate first-party package discovery for root and inner entry points.
- [x] Remove SARP's manual renderer and scene include directories from host targets;
      review public dependencies against direct public-header use.
- [x] Build five independent installed first-party package consumers and seven
      build-tree BasicRenderer API-category consumers. The explicit source audit
      covers every renderer C++ source and public-header smoke unit.
- [ ] Re-run final tests and exit-on-stability after the remaining implementation
      extraction. Visual validation stays with the project owner.

## Current validation

Run SARP's `build.cmd 1` from the SARP root, with one build at a time. Structural
moves need a build and source/boundary audits. Function-body relocation also needs
relevant tests and the radius-100 exit-on-stability harness; trust its report and
leave visual validation to the owner. The shader files, shader paths, cache formats,
configuration defaults, algorithms, ownership, and execution sequencing are unchanged.

The latest completed build/deployment is
`../build/vs2026-renderer-host/migration-clod-page-packing-final.log`. All 44 SARP
CTest tests passed (`../build/migration-clod-page-packing-ctest.log`). The corresponding
report is `../build/migration-clod-page-packing-report.txt`: stable, 120 stable frames,
zero readiness blockers, zero failed/pending assets, and launcher exit 0. It matches
the baseline's 40,401 active cells, 212,305 applied placements, 99,228 live static
objects, 2,106 successful assets, 6,870 meshes, and placement digest
11950196757715935234. Elapsed time was 13,627 ms against a single baseline sample
of 12,502 ms; these are insufficient to establish performance equivalence.

The boundary audit reports zero violations, two reviewed ORG integration exceptions,
and no stale exceptions; all eleven fixtures pass. The build-input audit covers 237
explicit renderer C++ sources and 149 individual header smoke units with no missing
entries. These checks ran after the page-packing extraction. The earlier validation records
are in [migration history](history/renderer-migration-history.md).

Both root and inner CMake entry points configured with installed first-party
packages and `BASICRENDERER_ENABLE_SUBMODULE_FALLBACK=OFF`. The inner standalone
entry needs the same external package inputs as the root: SARP's existing vcpkg
installation and the selected USD package directory. This checks first-party
package discovery; BasicRenderer itself still has no installed-package export claim.
