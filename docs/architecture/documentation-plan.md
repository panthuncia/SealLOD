# Architecture documentation plan

## Major subsystem map

| Boundary | Primary responsibilities | Main implementation anchors | Depends on / feeds |
| --- | --- | --- | --- |
| Scene and source state | Editable scene identity, ordered revisions, source snapshots | `BasicScene/`; `SceneSourceStateStore`; `SceneRenderBridge` | Feeds typed ingestion requests; must not select frame GPU bindings |
| Import and preprocessing | Decode USD/glTF/Assimp/NIF data; build geometry, skeleton and CLOD artifacts | `BasicRenderer/src/Import/`; `BRNifly/`; `CLodCacheTool/` | Feeds asset/residency services and state producers |
| Renderer ingestion | Materialize entities and route asset, geometry, material, object, workload, terrain and pose requests | `SceneIngestionServices`; `SceneEntityMaterializationService`; the `*RequestService` types | Feeds persistent services and `AsyncStateGraph` intents |
| Persistent services | Allocation, caches, IO, uploads, readback, descriptors, shader/program compilation and vendor SDK state | `BasicRenderer/src/Managers/`; renderer `*Service` types; `ORGModuleServices/` | Supports producers and accepted frames without becoming frame state |
| Async state production | Versioned artifact selection, dependencies, producer scheduling, cancellation, readiness and lifetime | `AsyncStateGraph.*`; `*Artifacts.cpp` | Consumes intents/services; emits fragments and manifests |
| Renderer publication | Assemble coherent immutable fragments, resource catalog and binding ownership; retain per-frame leases | `PublishedRendererState.*`; `PersistentRendererPublication.*` | Sole mutable-to-frame boundary; feeds OpenRenderGraph |
| Render features | Declare core, CLOD, shadows, transparency, terrain, post effects and extension work | `BasicRenderer/src/Render/GraphExtensions/`; pipeline recipes | Consume accepted state and declare graph work |
| OpenRenderGraph | Resource declarations, graph compilation, queue/barrier/aliasing plans, frame slots, preparation, recording, submission and retirement | `OpenRenderGraph/include/Render/`; `OpenRenderGraph/src/Render/` | Consumes immutable frame inputs; emits backend commands |
| BasicRHI | Backend-neutral devices, resources, descriptors, queues, fences, swap chains and D3D12/Vulkan implementations | `BasicRHI/` | Executes OpenRenderGraph plans |
| Telemetry/debug | Counters, traces, sampling, graph/resource inspection | `BasicTelemetry/`; renderer `Telemetry/`; ORG `DebugUI/` | Observes every layer; should not own control flow |
| External contributor/plugin edge | Stable contributor ABI, extension registration, Procedural Wind and external modules | ORG `Contributor/`; `Plugins/ProceduralWind/`; public contributor headers | Adapts external work into declared graph work |

## The two-graph handoff

The system is easiest to reason about as two graphs joined by one ownership boundary.

| Concern | Async state graph | Frame render graph |
| --- | --- | --- |
| Lifetime | Persistent across frames | One accepted frame/slot |
| Node identity | Logical artifact address plus revision and generation | Pass/resource usage in a compiled frame plan |
| Edges | Readiness and invalidation requirements | Data hazards, ordering, queue and external synchronization |
| Work | CPU builds plus tracked GPU uploads | Declaration, preparation, command recording and submission |
| Supersession | New desired revisions may cancel/replace queued work | Accepted frame is immutable; cancellation is joined and retires ownership |
| Output | Coherent manifest fragments and retained resource ownership | Submitted queue work, presentation dependency and retired frame slot |

The hard invariant is: **an accepted frame consumes one immutable publication lease and never consults mutable producer state.** Numeric revisions or raw resource pointers alone do not carry the required ownership.

### Async artifact families

The following is the stable first-pass grouping. Exact edges are request-dependent and should be generated from captured graph traces rather than asserted as one universal DAG.

| Family | Representative kinds | Important incoming edges | Published result |
| --- | --- | --- | --- |
| Texture | `TextureBinding`, `TextureImageTable` | Decode/production, descriptors, upload milestones | Binding/image resources and texture-image fragment |
| Versioned buffers | `BufferVersion`, `ActiveDrawList` | Immutable bytes/capture, capacity grants, upload service | Retained GPU buffer version; reused by many roots |
| Material | `Material`, `MaterialUsageBatch`, `MaterialTable` | Texture bindings and material table buffer versions | Materials fragment and raster/compile metadata |
| Geometry | `GeometryBufferState`, `GeometryResidency` | Geometry storage/slabs/page pools and upload milestones | Geometry and residency fragments |
| Objects/workloads | `DrawRecordPage`, `ActiveDrawList`, `ViewLifetime`, `IndirectWorkload` | Object buffers, exact draw-record root, material ready gate, view lifetime and argument buffers | Draw-record, active-list and indirect fragments |
| Terrain | `TerrainState` | Six buffer versions plus exact texture bindings, all upload-submitted | Terrain fragment |
| Static scene | `StaticTransaction`, `StaticScenePage`, `StaticScene`, `StaticVisibility`, `StaticTemplateBatch` | Page/transaction ownership plus geometry, material, draw and workload roots | Static/grass-related immutable scene closure |
| View/pose/light | `ViewFamily`, `PoseState`, `LightTable` | Versioned buffers; lights depend on the exact view family containing shadow IDs | Views, poses and lights fragments |
| Manifest | `FrameManifest` | Compatible fragment roots and dependency closures | Candidate/patch consumed by `RendererStatePublisher` |

`ExactSnapshot`, `Latest`, `ReadyGate`, and `LifetimeHold` are architecturally significant and must be visible in detailed diagrams. In particular, a ready gate authorizes work but should not accidentally pin an unrelated old publication root; exact dependencies carry coherent version identity and ownership.

## Documentation order

### 1. State/publication spine

Document `AsyncStateGraph`, artifact identity/readiness, producer scheduling, tracked uploads, manifest assembly, `RendererStatePublisher`, and frame-slot leases first.

Why first: every rendering feature depends on this spine, and confusing it with the frame graph causes the most serious lifetime and stale-state bugs. Deliverables should include a dependency-policy legend, artifact lifecycle state view, lane/domain execution view, and a trace-derived example DAG.

Exit criteria:

- Every artifact kind has an owner, producer, input type, output payload and readiness milestone.
- Every producer dependency is classified as exact, latest, gate-only, lifetime-only or optional/alternative.
- The manifest compatibility rules and retained ownership are explicit.
- At least one runtime trace can be mechanically compared with the model.

### 2. Accepted-frame and OpenRenderGraph lifecycle

Document the handoff from publication lease through frame acceptance, declaration/update, structural and frame compilation, preparation, recording, queue submission, presentation tail and retirement.

Why second: this proves where persistent state stops and per-frame state starts. It also exposes cross-thread access and ownership mistakes before individual pass diagrams amplify them.

Exit criteria:

- Thread/queue ownership and frame-slot bounds are stated.
- Every mutable service request has submit-or-cancel semantics.
- No preparation/recording path silently reads mutable managers or current settings.
- External synchronization and presentation ownership are modeled.

### 3. Resource and IO substrate

Document BasicRHI, device generations, descriptor services, resource registries, uploads, readbacks, streaming, alias pools and retirement.

Why third: artifact and frame diagrams both refer to these contracts. Modeling them earlier risks describing low-level mechanisms without knowing their lifetime consumer.

### 4. Core scene-to-draw path

Follow one object from source identity through import, geometry/material residency, draw records, active lists, indirect workloads, view state and a basic raster pass.

Why fourth: it is the smallest vertical slice that tests whether the preceding boundaries actually compose.

### 5. CLOD, virtual shadows and transparency

Split these into separate hierarchical models:

- CLOD residency/streaming and hierarchical culling
- software/hardware/Reyes raster variants
- virtual shadow page prediction, allocation, raster and composition
- AVBOIT/deep-visibility setup, raster and resolve

These are the largest feature graphs and should be tackled only after shared publication, frame and resource vocabulary is stable.

### 6. Remaining feature and edge subsystems

Document terrain, environment, texture streaming, animation/poses, procedural wind, FidelityFX/Streamline, contributor ABI, debug UI and telemetry. Then assemble the level-0 system diagram from links to the stabilized lower-level views.

## Design findings to investigate

These are hypotheses or known transitional seams, not automatic refactoring instructions.

1. **`Renderer` is a composition root plus a god-object risk.** It owns the manager fleet, typed services, async graph, publisher, graph extensions, graph rebuild, frame execution, presentation, telemetry and shutdown ordering. Separate `RendererComposition`, `SceneStateCoordinator`, `FrameCoordinator`, and `PresentationCoordinator` boundaries in the model; then measure which methods cross more than one.
2. **Manager-to-service migration is incomplete.** The ownership document says rendering should consume publications, yet `Renderer.h` still directly owns the legacy managers. Inventory each manager call by phase: ingestion/storage is allowed; preparation/recording lookup is a leak.
3. **Publication logic has two orchestration paths.** Full `FrameManifest` candidates and fragment patches/rebases coexist in `RendererStatePublisher`. Model their ordering and rejection semantics explicitly; consider one successor-transaction abstraction if their invariants are equivalent.
4. **Artifact graph registration is centralized manually.** Renderer lifecycle and graph-construction translation units register producers individually, while requests are spread across managers/services. A declarative producer catalog could expose kind, lane, domain, input/output types and supported milestones for documentation and validation.
5. **Artifact kinds mix abstraction levels.** Generic buffer versions, domain roots, transactions, pages, ready gates and full manifests share one enum and scheduler. Split the model into resource artifacts, domain publications and orchestration artifacts even if the implementation remains unified.
6. **GPU work spans both graphs.** Async producers submit tracked uploads while OpenRenderGraph schedules frame GPU work. The ownership and synchronization adapter (`GpuSubmissionSet` into accepted-frame waits) deserves its own interface view; otherwise double scheduling or CPU waits are easy to hide.
7. **Global/singleton compatibility remains a boundary hazard.** Device, descriptor, task, settings and other singleton managers coexist with generation-owned services. Diagram every singleton access after frame acceptance and retire fallback paths once explicit generation ownership covers them.
8. **Feature implementation dominates the main library.** Render graph extensions, especially CLOD/VSM/AVBOIT, are large enough to be architectural subsystems but compile into the same target and are composed by the same renderer. Stable extension contracts and feature-owned state would reduce rebuild and responsibility coupling.
9. **The requested DAG is dynamic.** Dependencies can be added by `Needs(...)`, latest intents coalesce, and readiness/suspension alters execution. A hand-maintained static diagram alone will drift. Treat `AsyncStateGraph` trace output as the instance model and the SysML file as the type/constraint model.
10. **Documentation can become a conformance tool.** Add a machine-readable registry or trace exporter before attempting exhaustive hand diagrams. CI can then detect unmodeled artifact kinds, producers, dependency policies, or frame phases.

## Working method for each subsystem

For each lower-level diagram:

1. Establish its owned state and explicit non-responsibilities.
2. List ports and exchanged payloads before internal classes.
3. Add thread, queue, lifetime and mutability constraints.
4. Trace one success path, one supersession/cancellation path, one failure path and shutdown.
5. Compare the model with call sites and an available runtime trace.
6. Record leaks as issues with an evidence anchor, desired owner and removal gate.
7. Refactor only after the current-state (`as-is`) view is accepted; preserve a separate target-state (`to-be`) view.

The immediate next artifact should be an automatically checked inventory of `ArtifactKind` registrations and observed dependency edges. That gives the detailed async diagrams a reliable source of truth and prevents the architecture effort from turning into manually maintained artwork.
