# BasicRenderer architecture model

This directory holds a hierarchical SysML v2 model of BasicRenderer. It describes the system as it exists; it does not imply that every current dependency is desirable.

## Model views

| View | File | Question answered |
| --- | --- | --- |
| System context and decomposition | [`basic-renderer.sysml`](basic-renderer.sysml) | What are the major subsystems, and which boundaries connect them? |
| Asynchronous state production | [`async-state-graph.sysml`](async-state-graph.sysml) | How do source changes become immutable, GPU-visible renderer state? |
| Subsystem map and the two-graph handoff | [`overview.md`](overview.md) | Which boundaries own what, and how does async state reach an accepted frame? |

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

Spec42 (bundled with the Spec42 VS Code extension) validates and renders the authored views. The Sensmetry Editor
language server also reads them.

## Reading the architecture

BasicRenderer contains two dependency graphs with different responsibilities and lifetimes:

1. The **async state graph** is a persistent CPU-side graph. It selects immutable artifact versions, waits on dependencies and GPU upload milestones, schedules producers, cancels superseded work, and hands completed manifest fragments to the publisher.
2. The **frame render graph** is four persistent epoch graphs (Visibility, Shading, Post, Overlay) on one OpenRenderGraph `GraphHost`. They are built only when a revision asks, on a build thread, and attached between frames. Each accepted frame prepares the epochs' passes, records only stale batches of the kept recordings, submits queue work and presents on the host's submission thread, and retires frame-owned state. See [`../renderer-architecture.md`](../renderer-architecture.md), "The frame graph".

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
- `DependencyNode` and `DependencyPort` are coarse placeholders in the async overview. Refine their payloads into subsystem-specific ports rather than replacing them with direct part-to-part connectors.
- External libraries and tools are shown only when they alter a boundary (for example USD import, DirectStorage, vendor upscalers, or contributor ABI).
- Mutable storage belongs behind typed service ports. Immutable artifacts and accepted-frame values may cross into rendering.
- Model elements cite implementation anchors in their companion Markdown page.

## Source anchors

- Renderer composition and frame lifecycle: `BasicRenderer/include/BasicRenderer/Renderer.h`, with implementation split across `RendererLifecycle.cpp`, `RendererFrameUpdate.cpp`, `RendererFrameRendering.cpp`, and `RendererGraphConstruction.cpp` under `BasicRenderer/src/Runtime/Renderer/`.
- Persistent artifact graph: `BasicRenderer/src/Runtime/StateGraph/AsyncStateGraph.h`, `AsyncStateGraph.cpp`
- Immutable publication boundary: `BasicRenderer/include/BasicRenderer/Streaming/PublishedRendererState.h`, `BasicRenderer/src/Runtime/Publication/PublishedRendererState.cpp`, `PersistentRendererPublication.*`
- Frame graph runtime: `OpenRenderGraph/include/Render/RenderGraph/`, `OpenRenderGraph/src/Render/RenderGraph/`
- Backend abstraction: `BasicRHI/rhi.h`, `BasicRHI/rhi_dx12.cpp`, `BasicRHI/rhi_vulkan.cpp`
- Ownership: `OpenRenderGraph/docs/renderer-state-ownership.md`

## Scope

This is a level-0/level-1 model. Connections are evidence-backed at subsystem level. It does not model multiplicities, complete port payloads, threading constraints, or feature-internal pass graphs.
