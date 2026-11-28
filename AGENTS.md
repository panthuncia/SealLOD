# AGENTS.md

BasicRenderer is a DX12 research renderer built around virtualized geometry. This file covers
orientation, build and test; [README.md](README.md) covers the feature list and the reasoning behind
the dependency model, and [TESTING.md](TESTING.md) covers the sampling and benchmark harness.

Its own code is ~263k lines across 739 files. A further ~72k lines are vendored under
`ThirdParty/` and nine vendored submodules.

## Search scope — read this before your first grep

**Use `rg`, not `grep -r`.** There is an `.ignore` at the repository root that does two jobs:

1. It re-includes `BasicRenderer/`. The parent SARP repository's `.ignore` lists `BasicRenderer/` so
   that searches of SARP's code are not swamped by this submodule — and because ripgrep applies
   parent ignore files during traversal, that same pattern matches *this* repository's own source
   directory. Without the negation, `rg` here returns thousands of vendored files and not one line
   of BasicRenderer's own code.
2. It excludes eight vendored submodules, which are ~6,400 of the ~6,800 files ripgrep would
   otherwise walk. (`ThirdParty/` is already excluded by `.gitignore`.) To search one deliberately:
   `rg --no-ignore <pattern> GPU-Reshape/`.

With both in place, `rg` sees 1,209 files instead of 6,758, and 817 of them are this repository's
own source rather than none.

`grep -r` honors neither that file nor `.gitignore`, so it will also walk `out/`, which holds
generated shader dumps — including fully-preprocessed HLSL of ~10k lines that will match almost any
shader identifier you search for.

## Layout

Supported headers live in `BasicRenderer/include/BasicRenderer/`; private headers live
beside implementations in `BasicRenderer/src/`. `Runtime/Detail` declarations are
compile-visible for existing by-value members and are not supported consumer entry points.
See [renderer boundaries](docs/renderer-architecture.md) for the ownership contracts and placement rules.

| Path under `BasicRenderer/src/` | Contents |
|---|---|
| `Runtime/` | Renderer orchestration, device/resource services, scheduling, state graph, publication, frame support, and settings. |
| `VirtualGeometry/` | Geometry storage, streaming, culling, rasterization, Reyes, voxels, and feature graph integration. |
| `Scene/`, `Animation/`, `Terrain/` | Ingestion, object/view state, skeletons and pose, and terrain residency. |
| `Assets/` | Format adapters, geometry representations/processing, textures, and caches. |
| `Pipeline/`, `Materials/` | Recipes, shader compilation, pipeline state, material evaluation, and texture streaming. |
| `Lighting/`, `VirtualShadows/`, `Transparency/`, `PostProcessing/` | Feature-owned CPU state, integration, and passes. |
| `Diagnostics/` | Telemetry, debug rendering, and menu implementation. |

Beside `BasicRenderer/`, `CLodCore/` builds the generator and cache format from these sources into
libraries the renderer and tools share, so a tool can link them without the renderer. `CLodEval/` is an
executable built on those. `CLodCacheTool/` links the renderer library for its USD importer, but must never create a
device: it fails at exit if `d3d12.dll` or `dxgi.dll` was loaded.

Pass code lives in each owning subfeature's `RenderPasses/` directory, alongside CPU
architecture rather than in a global pass bucket. TextureFactory retains ownership
of its nested passes, whose definitions live in `Assets/Textures/RenderPasses`.
Shader paths remain under `BasicRenderer/shaders/`.

`Scripting/` is C# scenario scripting: a native host library that loads .NET in-process, a renderer backend
(`BasicRendererScriptingRenderer`) that applies scripts' scene writes from a `Renderer::AddSceneMutationHook`, and a
managed runtime that compiles loose `.cs` scenarios with Roslyn and runs them one slice per frame (the demo's
`--script`). It uses only public renderer headers. See [docs/scripting.md](docs/scripting.md); its ABI tables are
append-only.

`BasicRHI/`, `OpenRenderGraph/`, `BasicTelemetry/`, `BasicScene/`, and
`ORGModuleServices/` are first-party dependencies. Other vendored submodules are not
edited as part of renderer organization work.

Shader compilation goes through DXC (`BasicRenderer/dxcompiler.dll`, vendored in-tree) and Slang, with
tree-sitter used to parse HLSL during preprocessing — `ShaderPreprocessTests` covers that path.

## Build

Two contexts, and which one you are in changes the commands.

**As a SARP submodule** (the usual case): build from the SARP root, not here. SARP's CMake pulls
this in and its presets drive it.

```powershell
.\build.cmd 1
```

See SARP's own `AGENTS.md` for the sharp edges there — notably that a successful link auto-deploys
to a Skyrim directory and fails while the renderer is running.

**Standalone**, three stages, because `BasicRHI` and `OpenRenderGraph` are consumed as packages
first and only fall back to in-tree `add_subdirectory`:

```powershell
cmake -S BasicRHI -B out/build/rhi -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/rhi
cmake --install out/build/rhi --prefix out/install/rhi

cmake -S OpenRenderGraph -B out/build/org -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="out/install/rhi"
cmake --build out/build/org
cmake --install out/build/org --prefix out/install/org

cmake -S . -B out/build/renderer -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBASICRENDERER_USE_PACKAGE_DEPS=ON -DBASICRENDERER_ENABLE_SUBMODULE_FALLBACK=OFF -DCMAKE_PREFIX_PATH="out/install/rhi;out/install/org"
cmake --build out/build/renderer
```

Options worth knowing: `BASICRENDERER_USE_PACKAGE_DEPS` (default `ON`),
`BASICRENDERER_ENABLE_SUBMODULE_FALLBACK` (default `ON`), `BASICRENDERER_USD_VARIANT=dbg|rel`,
`BASICRHI_ENABLE_RESHAPE` (default `OFF`), `BASICRENDERER_BUILD_BRNIFLY` (default `ON`).

To package the demo as a folder that runs by double-clicking `BasicRenderer.exe` (runtime DLLs, shaders, textures and
a warm shader cache, no models), run `scripts\deploy_demo.ps1` (`-Build` builds `BasicRendererDemo` first,
`-Destination` defaults to `D:\BasicRendererDemo`). It finds SARP's build when this is SARP's submodule, otherwise
the newest under `out\build`.

The root `CMakeLists.txt` handles repository configuration. Target definitions and
explicit renderer source lists are in `BasicRenderer/CMakeLists.txt`. Shared
first-party dependency discovery is in `cmake/RendererDependencies.cmake`.

## Test

Renderer tests and boundary audits are registered in `BasicRenderer/CMakeLists.txt`.
The current SARP RelWithDebInfo tree registers 44 tests, including first-party and
ProceduralWind tests. Query the configured tree rather than relying on test numbers:

```powershell
ctest -N -C RelWithDebInfo --test-dir build\vs2026-renderer-host
ctest -C RelWithDebInfo --test-dir build\vs2026-renderer-host --output-on-failure
```

The build compiles 149 public-header smoke units without the renderer PCH. The
include audit is `python scripts/Audit-RendererBoundaries.py`; its fixtures are
`python scripts/TestRendererBoundaries.py`. Explicit source-list and header-smoke
coverage is checked by `python scripts/Audit-RendererBuildInputs.py`. Independent installed first-party
package consumption is checked by `scripts/Test-InstalledPackages.ps1`.

`CLodCacheFormatTests` and `CLodEvalTests` are the tests that run the CLod generator, cache and page
decoding. To measure a generator change, use `CLodEval`: see
[docs/clod-evaluation.md](docs/clod-evaluation.md).

CPU tests do not draw frames. For changes that could affect execution or lifetimes,
use SARP's documented exit-on-stability harness and inspect its reports. Structural
moves require a build and boundary checks. Visual validation remains with the owner.

## Code style

There is no `.clang-format` or `.editorconfig`. Indentation is predominantly four spaces (about 80%
of `src/` files), with a tab-indented minority. **Match the file you are editing** rather than
imposing a style, and do not reformat surrounding code as a side effect of a change — an unrelated
whitespace diff across one of the large files is very hard to review.

## Documentation state

Be aware of what does and does not exist before you go looking:

- [README.md](README.md) — feature list, dependency model, standalone build. Accurate.
- [TESTING.md](TESTING.md) — sampling harness, named-pipe experiment control, the CLOD
  virtual-shadow CPU benchmark and its request tracing. Detailed and accurate.
- `docs/` — references for systems as they exist now (listed below).
- `docs/performance/` — **performance-experiment writeups and measurements** (CLOD compute vs work-graph,
  VSM rerender, deferred-shading, small-pass and material-evaluation rounds, stochastic lighting, lighting and
  texture streaming measurements). These are lab notebooks tied to specific experiments, not architecture
  references.
- `docs/architecture/` — the SysML model and a subsystem overview.

[docs/performance/pass-optimization-workflow.md](docs/performance/pass-optimization-workflow.md) is the workflow for making a GPU pass faster:
moving-camera A/B timing with hot-swapped shaders, NVPerf limiters and register arithmetic, Nsight per-line register
exports (`scripts/nsight_shader_lines.py`), validation, and the patterns that have and haven't paid.

[docs/texture-streaming.md](docs/texture-streaming.md) is the reference for material texture streaming: per-material
feedback, the residency policy and its VRAM budget.

[docs/scripting.md](docs/scripting.md) is the reference for C# scenarios: running them, the threading rule, the
result document, and the ABI rules for adding API.

[docs/lighting.md](docs/lighting.md) is the reference for analytic lights: the light model, the single direct-light
loop and its visibility options, and the shadowing modes a new technique slots in beside.

[docs/stochastic_lighting.md](docs/stochastic_lighting.md) is the reference for stochastic (MegaLights-style) direct
lighting.

[docs/hair.md](docs/hair.md) is the reference for the strand-hair proof of concept in `Plugins/Hair/`: the importers
(procedural, `.hair`, Alembic grooms), the builder and its data model, the `.groom` file, the LOD invariants and the
tests that prove them, and the runtime plugin (`HairPlugin`, registered by the demo): its commands, attachment modes,
GPU buffers and passes (LOD selection, strand evaluation, the compute rasterizer into the hair visibility
target, the resolve of that target into the canonical surface, depth and motion vectors, and a debug overlay). On that
path, the default, hair is lit as an opaque surface with a placeholder material, and a strand thinner than a pixel is
written with the probability of its coverage (`hair.lod` `coverage`; "Partial coverage measurements" has what that buys
and costs). `hair.render {path: "layered"}` selects the other path: three depth-ordered layers and the summed coverage a
pixel, every drawn strand vertex lit by hair's own light loop with a hair shading model (the R, TT and TRT lobes of the
real-time Marschner model with Chiang's parameters, in `HairBsdfShared.h`, one text for the shader and the CPU tests;
"The hair shading model", "Hair shading measurements"), and a composite over the lit frame that gives the upscaler
hair's depth, motion and a reactive mask ("The layered path", "Layered path measurements"; the mask goes through
`Extensions/UpscalerReactiveMask.h`). On that path hair shadows itself and takes light scattered between fibres through a density volume per groom, rebuilt every frame and marched from every lit vertex (`hair.volume`; dual scattering in `HairScatterShared.h`; "The density volume", "Multiple scattering", "Self-shadowing measurements"). On that path hair also casts the sun's shadow: `HairExtension` is a caster provider of the virtual shadow map (`hair.shadows`; a shadow view that keeps the clusters over shadow pages drawn this frame, a second, smaller strand pool, and a compute rasterizer that writes depth with coverage dithered; "Shadow casting", "Cast shadow measurements"). Spot and point lights get no shadow from hair, and on the surface path hair casts none. The layered path has two rasterizers: the direct one, a thread per segment, every strand drawn, and the culled one (`hairRasterCulled.hlsl`; `hair.render` `raster`, `auto` by default, which takes it where strands are about a pixel wide), which keeps a hidden depth per pixel that hair itself lowers, cuts segments into 8-pixel chunks, draws first what the frame before saw and then tests the rest against the frame's own hidden depths, and keeps a quarter of a hidden cluster's strands in the pool ("The culled rasterizer", "Hidden clusters", "Rasterizer cost measurements": 12.1 ms to 3.6 ms with a groom of wide strands filling the view, 1.9 ms when the camera is not jittered, which is how the profiler measures unless told otherwise, and no gain for hair of real width). It also covers
the opt-in simulation (`hair.sim`): guides simulated on the GPU with a fixed timestep, sphere and capsule colliders, and
render strands interpolated from their guides. Simulation follows the view: a groom the main camera
does not show is not simulated ("View-following measurements"). Per-instance memory follows it too: no buffer holds root
frames (each pass works out the ones it needs), and guide state is lent to the instances that are simulated or keep a
shape, out of a fixed buffer, with a rigid fallback when it runs out ("Guide rows", "On-demand memory measurements").
Its "POC measurements" section has the
many-groom scenario's numbers: cost against distance and groom count, memory, continuity and curls under reduction.

[docs/clod-simplification-validation.md](docs/clod-simplification-validation.md) covers the opt-in validation of CLod
simplification results against the original surface: checks, retry policy, settings and Bistro measurements. It also covers
the later opt-in stages: replacement vertices, optimal placement, controlled topology changes (seam relaxation), and error channels with appearance-aware LOD selection
(`CLodSelectionError`, the runtime settings `clodLodGeometricChannel` and `clodLodAppearanceWeight`).
[docs/clod-appearance-stage6.md](docs/clod-appearance-stage6.md) is stage 6's design (independent appearance: remap and baked
atlases, filtered normals) and its increments: the texture term of the appearance channel, CLodEval's `quality.texture`, its colour
weighting, opt-in UV remap blocks in triangle pages (a material-evaluation lookup, `clodUvRemap.hlsli`), the builder defaults (the
recommended build) and the single CLod cache format (schema 84).

[docs/renderer-architecture.md](docs/renderer-architecture.md) covers renderer boundaries: ownership contracts,
cross-subsystem dependencies and placement rules. [docs/unified-binding-ownership.md](docs/unified-binding-ownership.md)
covers descriptor binding ownership across ORG publications.
