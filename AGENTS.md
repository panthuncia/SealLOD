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
See [renderer architecture](docs/renderer-architecture.md) for current migration status.

| Path under `BasicRenderer/src/` | Contents |
|---|---|
| `Runtime/` | Renderer orchestration, device/resource services, scheduling, state graph, publication, frame support, and settings. |
| `VirtualGeometry/` | Geometry storage, streaming, culling, rasterization, Reyes, voxels, and feature graph integration. |
| `Scene/`, `Animation/`, `Terrain/` | Ingestion, object/view state, skeletons and pose, and terrain residency. |
| `Assets/` | Format adapters, geometry representations/processing, textures, and caches. |
| `Pipeline/`, `Materials/` | Recipes, shader compilation, pipeline state, material evaluation, and texture streaming. |
| `Lighting/`, `VirtualShadows/`, `Transparency/`, `PostProcessing/` | Feature-owned CPU state, integration, and passes. |
| `Diagnostics/` | Telemetry, debug rendering, and menu implementation. |

Pass code lives in each owning subfeature's `RenderPasses/` directory, alongside CPU
architecture rather than in a global pass bucket. TextureFactory retains ownership
of its nested passes, whose definitions live in `Assets/Textures/RenderPasses`.
Shader paths remain under `BasicRenderer/shaders/`.

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
- `docs/` — eight files, all **performance-experiment writeups** (CLOD compute vs work-graph, VSM
  rerender, deferred-shading and small-pass optimization rounds). These are lab notebooks tied to
  specific experiments, not architecture references.

Architecture and migration status are now documented in
[docs/renderer-architecture.md](docs/renderer-architecture.md). Historical baseline and
validation records are in [docs/history/renderer-migration-history.md](docs/history/renderer-migration-history.md).
The boundary audit distinguishes violations from ordinary private implementation
connections; internal include counts are not a migration completion metric.
