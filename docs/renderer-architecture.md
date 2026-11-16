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
