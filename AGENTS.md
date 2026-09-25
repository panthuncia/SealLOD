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

Headers in `BasicRenderer/include/`, implementations in `BasicRenderer/src/`, mirroring each other.

| Path | Contents |
|---|---|
| `BasicRenderer/src/Render/GraphExtensions/ClusterLOD/` | **Virtualized geometry.** 65 files; with the rest of the `CLod*` code it is ~75k lines, roughly 29% of the codebase and the largest single subsystem. |
| `BasicRenderer/include/Render/`, `src/Render/` | Render graph integration, state graph, extensions. The largest header tree (150 files). |
| `BasicRenderer/include/RenderPasses/`, `src/RenderPasses/` | Individual passes (50 headers). |
| `BasicRenderer/src/Managers/` | Object, mesh, texture-streaming, PSO and other managers. `Singletons/` holds the process-wide ones. |
| `BasicRenderer/src/Import/` | USD, glTF, NIF and CLOD-cache loading. |
| `BasicRenderer/src/Mesh/`, `Resources/`, `Materials/`, `Animation/`, `Scene/` | Geometry processing, GPU resources, material model, skinning, ECS scene. |
| `BasicRenderer/shaders/` | HLSL. ~58k lines; `Include/` holds the shared `.hlsli`. |
| `BasicRenderer/tests/` | 10 unit tests. |
| `BasicRHI/`, `OpenRenderGraph/`, `BasicTelemetry/`, `ORGModuleServices/` | Separate submodules but **first-party** — the intended repository split described in the README. Deliberately left searchable; you will read and change them alongside this code. |
| `GPU-Reshape/`, `FidelityFX-SDK/`, `xatlas/`, `PyNifly/`, `geometry-central/`, `openpbr-bsdf/`, `tree-sitter-hlsl/`, `volk/`, `ThirdParty/` | Vendored. Not edited here. |

Shader compilation goes through DXC (`BasicRenderer/dxcompiler.dll`, vendored in-tree) and Slang, with
tree-sitter used to parse HLSL during preprocessing — `ShaderPreprocessTests` covers that path.

## Build

Two contexts, and which one you are in changes the commands.

**As a SARP submodule** (the usual case): build from the SARP root, not here. SARP's CMake pulls
this in and its presets drive it.

```powershell
cmake --build build\vs2026-renderer-host --config RelWithDebInfo
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

The root `CMakeLists.txt` is small (378 lines); the real target definitions are in
`BasicRenderer/CMakeLists.txt` (1,439 lines), which is also where the tests are registered.

## Test

Ten unit tests are registered at `BasicRenderer/CMakeLists.txt:1406-1415`: `ShaderPreprocessTests`,
`SkeletonArtifactCacheTests`, `PipelineRecipeTests`, `MaterialEvalVariantTests`,
`VirtualShadowBudgetTests`, `BoundedSpscQueueTests`, `TaskSchedulerManagerTests`,
`AsyncStateGraphTests`, `CLodCoordinatorTests`, `StatisticalSamplerTests`. `BasicRHI`,
`OpenRenderGraph` and `BasicTelemetry` register their own.

Built through SARP, all of these appear in SARP's ctest list as tests 16–34:

```powershell
ctest -C RelWithDebInfo --test-dir build\vs2026-renderer-host -E '^SARP'
```

Ten tests against ~263k lines is thin, and none of them draw a frame. They cover scheduling, caching
and preprocessing logic — not rendering output. Treat a green test run as evidence that you did not
break the CPU-side plumbing, and nothing more. Rendering changes need to be looked at, and the
benchmark harness in [TESTING.md](TESTING.md) is what measures them.

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
- `docs/` — eight files, all **performance-experiment writeups** (CLOD compute vs work-graph, VSM
  rerender, deferred-shading and small-pass optimization rounds). These are lab notebooks tied to
  specific experiments, not architecture references.

There is no architectural documentation. In particular the ClusterLOD subsystem — the largest and
the one the README calls a novel approach — has none, and its interesting behaviour is GPU-side,
spread across `shaders/ClusterLOD/workGraphCulling.hlsl` (~6.3k lines), `clodUtil.hlsl` (~4.4k) and
`CLodStreamingSystem.cpp` (~8.9k). Budget reading time accordingly, and if you work it out, write it
down.
