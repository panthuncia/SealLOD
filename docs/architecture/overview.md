# Architecture overview

## Major subsystem map

| Boundary | Primary responsibilities | Main implementation anchors | Depends on / feeds |
| --- | --- | --- | --- |
| Scene and source state | Editable scene identity, ordered revisions, source snapshots | `BasicScene/`; `SceneSourceStateStore`; `SceneRenderBridge` | Feeds typed ingestion requests; must not select frame GPU bindings |
| Import and preprocessing | Decode USD/glTF/Assimp/NIF data; build geometry, skeleton and CLOD artifacts | `BasicRenderer/src/Assets/`; `BRNifly/`; `CLodCacheTool/` | Feeds asset/residency services and state producers |
| Renderer ingestion | Materialize entities and route asset, geometry, material, object, workload, terrain and pose requests | `SceneIngestionServices`; `SceneEntityMaterializationService`; the `*RequestService` types | Feeds persistent services and `AsyncStateGraph` intents |
| Persistent services | Allocation, caches, IO, uploads, readback, descriptors, shader/program compilation and vendor SDK state | `BasicRenderer/src/Runtime/`; renderer `*Service` types; `ORGModuleServices/` | Supports producers and accepted frames without becoming frame state |
| Async state production | Versioned artifact selection, dependencies, producer scheduling, cancellation, readiness and lifetime | `AsyncStateGraph.*`; `*Artifacts.cpp` | Consumes intents/services; emits fragments and manifests |
| Renderer publication | Assemble coherent immutable fragments, resource catalog and binding ownership; retain per-frame leases | `PublishedRendererState.*`; `PersistentRendererPublication.*` | Sole mutable-to-frame boundary; feeds OpenRenderGraph |
| Render features | Declare core, CLOD, shadows, transparency, terrain, post effects and extension work | Feature-owned graph adapters and `RenderPasses/` under `BasicRenderer/src/`; pipeline recipes | Consume accepted state and declare graph work |
| OpenRenderGraph | Resource declarations, graph compilation, queue/barrier/aliasing plans, frame slots, preparation, recording, submission and retirement | `OpenRenderGraph/include/Render/`; `OpenRenderGraph/src/Render/` | Consumes immutable frame inputs; emits backend commands |
| BasicRHI | Backend-neutral devices, resources, descriptors, queues, fences, swap chains and D3D12/Vulkan implementations | `BasicRHI/` | Executes OpenRenderGraph plans |
| Telemetry/debug | Counters, traces, sampling, graph/resource inspection | `BasicTelemetry/`; renderer `Diagnostics/`; ORG `DebugUI/` | Observes every layer; should not own control flow |
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

Exact edges are request-dependent: dependencies are added by `Needs(...)`, latest intents coalesce, and readiness and suspension alter execution, so there is no one universal DAG. `AsyncStateGraph` trace output is the instance model; the SysML files are the type and constraint model.

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

The dependency policies are `ExactSnapshot`, `Latest`, `ReadyGate`, and `LifetimeHold`. A ready gate authorizes work but does not pin an unrelated old publication root; exact dependencies carry coherent version identity and ownership.

Publication has two orchestration paths in `RendererStatePublisher`: full `FrameManifest` candidates, and fragment patches/rebases. Async producers submit tracked GPU uploads, while OpenRenderGraph schedules frame GPU work; `GpuSubmissionSet` carries the former into accepted-frame waits.
