# BasicRenderer architecture model

This directory is the starting point for a hierarchical SysML v2 model of BasicRenderer. It describes the system as it exists; it does not imply that every current dependency is desirable.

## Model views

| View | File | Question answered |
| --- | --- | --- |
| System context and decomposition | [`basic-renderer.sysml`](basic-renderer.sysml) | What are the major subsystems, and which boundaries connect them? |
| Asynchronous state production | [`async-state-graph.sysml`](async-state-graph.sysml) | How do source changes become immutable, GPU-visible renderer state? |
| Documentation plan and findings | [`documentation-plan.md`](documentation-plan.md) | What should be documented next, and which design problems should be tested? |

The `.sysml` files deliberately use a portable subset of SysML v2 textual notation: packages, part definitions, part usages, ports, connections, and standard typed views. This keeps them usable across validators and viewers without encoding tool-specific presentation detail into the model.

## Authored diagram views

The models contain explicit SysML v2 `view` usages rather than relying on a viewer to invent a diagram from package contents. Every diagram view has one or more `expose` statements and specializes either `StandardViewDefinitions::GeneralView` or `StandardViewDefinitions::InterconnectionView`. The standard view type selects the diagram semantics without Sensmetry-specific `depth` or `render` members.

`basic-renderer.sysml` provides:

- `basicRendererHierarchy`
- `basicRendererInterconnections`

`async-state-graph.sysml` provides:

- `asyncStateOverview`
- `asyncStateInterconnections`
- `artifactLifecycle`
- `dependencyAndExecutionPolicies`
- `producerDependencyGraph`
- `publicationPipeline`

General views emphasize containment and broad relationships; interconnection views emphasize parts, ports, and connectors. Spec42 can validate and render these authored views directly.

## Validation

The validation helper prefers the Spec42 CLI bundled with the Spec42 VS Code extension and treats warnings as errors:

```powershell
BasicRenderer/scripts/validate-sysml.ps1 `
    BasicRenderer/docs/architecture/basic-renderer.sysml `
    BasicRenderer/docs/architecture/async-state-graph.sysml
```

The PowerShell launcher enforces a 75-second timeout outside the Node validator and terminates its exact process tree on timeout. The Node validator also enforces a 30-second timeout around each Spec42 invocation. It locates the newest installed `elan8.spec42-*` extension; set `SPEC42_PATH` to select another Spec42 executable. If Spec42 is unavailable, it falls back to the Sensmetry Editor language server; set `SYSIDE_PATH` to select that executable. This makes the same command useful in local development and CI while keeping Spec42 as the authoritative validator for the current models.

## Reading the architecture

BasicRenderer contains two dependency graphs with different responsibilities and lifetimes:

1. The **async state graph** is a persistent CPU-side graph. It selects immutable artifact versions, waits on dependencies and GPU upload milestones, schedules producers, cancels superseded work, and hands completed manifest fragments to the publisher.
2. The **frame render graph** is a per-accepted-frame GPU graph. OpenRenderGraph declares resource use, compiles queue/barrier/aliasing plans, prepares and records passes, submits queue work, and retires frame-owned state.

The boundary between them is the immutable `PublishedRendererState` lease. A frame must not query mutable producers after crossing this boundary.

```text
source changes
    -> typed ingestion/services
    -> AsyncStateGraph artifacts
    -> RendererStatePublisher / PublishedRendererState lease
    -> OpenRenderGraph accepted frame
    -> BasicRHI queues and resources
    -> presentation
```

## Modeling conventions

- A **part** is an ownership or responsibility boundary, not necessarily one C++ class or CMake target.
- A **connection** means a meaningful runtime dependency or exchange. Connectors that a focused interconnection view is intended to render are named and terminate on ports; Spec42 uses those port endpoints to place edges on node boundaries.
- A focused diagram exposes one concrete root part usage. Do not recursively expose a definition's members as separate roots: that flattens their ownership context and prevents the renderer from reconstructing the intended interconnection diagram.
- `DependencyNode` and `DependencyPort` are intentionally coarse placeholders in the async overview. Refine their payloads into subsystem-specific ports as implementation evidence is added; do not regress to direct part-to-part connectors merely to avoid that refinement.
- External libraries and tools are shown only when they alter a boundary (for example USD import, DirectStorage, vendor upscalers, or contributor ABI).
- Mutable storage belongs behind typed service ports. Immutable artifacts and accepted-frame values may cross into rendering.
- Every model refinement should cite implementation anchors in its companion Markdown page and record unresolved questions rather than silently guessing.

## Source anchors

- Renderer composition and frame lifecycle: `BasicRenderer/include/Renderer.h`, `BasicRenderer/src/Renderer.cpp`
- Persistent artifact graph: `BasicRenderer/include/Render/AsyncStateGraph.h`, `BasicRenderer/src/Render/AsyncStateGraph.cpp`
- Immutable publication boundary: `BasicRenderer/include/Render/PublishedRendererState.h`, `BasicRenderer/src/Render/PublishedRendererState.cpp`
- Frame graph runtime: `OpenRenderGraph/include/Render/RenderGraph/`, `OpenRenderGraph/src/Render/RenderGraph/`
- Backend abstraction: `BasicRHI/rhi.h`, `BasicRHI/rhi_dx12.cpp`, `BasicRHI/rhi_vulkan.cpp`
- Current ownership intent: `OpenRenderGraph/docs/renderer-state-ownership.md`
- Async frame implementation status: `OpenRenderGraph/docs/async-frame-implementation.md`

## Status

This is a level-0/level-1 model. Connections are evidence-backed at subsystem level, while multiplicities, complete port payloads, threading constraints, and feature-internal pass graphs are intentionally deferred to later views.
