# CLod hierarchy evaluation

`CLodEval` measures what the CLod generator produced, from the command line, so a generator or
shader change can be judged by numbers. Start here to run it or to add a metric.

| Binary | Job | Links |
|---|---|---|
| `CLodCacheTool` | Source asset to complete cache, with no GPU | `BasicRenderer`, but never creates a device |
| `CLodEval` | Orchestration, CPU analysis, comparison | `CLodCacheFormat` only |
| `CLodCapture` | Renders a job file to a bundle of images. `CLodEval` does not drive it or score bundles yet. | `BasicRenderer` |

Each rebuilds without the others. After a generator edit, rebuild `CLodCacheTool` and rerun the
suite. The suite notices the changed tool and rebuilds the caches. After a metric edit, rebuild
`CLodEval`, and nothing else has to run again.

## Running it

From the build's output directory, or with a full path to `CLodEval.exe`:

```powershell
cmake --build build\ninja-renderer-host --config RelWithDebInfo --target CLodEval CLodCacheTool

CLodEval run config\clod_eval\smoke.json --workspace out\clod_eval
CLodEval compare out\clod_eval\results\voxel.jsonl out\clod_eval\results\voxel-coarse.jsonl --threshold 0.02
CLodEval inspect out\clod_eval\builds\voxel\cache --group 7
CLodEval list-analyzers
```

SARP pulls BasicRenderer with `EXCLUDE_FROM_ALL`, so these targets only build when named.

| Command | Does |
|---|---|
| `build <suite>` | Builds each variant's caches. Skipped if the tool, assets and settings are unchanged. `--force` rebuilds. |
| `analyze <suite>` | Writes `results/<variant>.jsonl` and `.csv`, and reports over-budget metrics. |
| `run <suite>` | `build`, then `analyze`. |
| `inspect <path>` | Describes caches level by level. `--group N` adds one group, `--json` for scripts. |
| `analyze-cache <path> --out f.jsonl` | Analyzes caches that did not come from a suite, such as `SARPPreprocessor`'s. |
| `compare <a> <b>` | Exit 0 same, 1 different or over budget, 2 not comparable, 3 error. |
| `view <suite>` | Opens the viewer on a built variant. See [The viewer](#the-viewer). |
| `merge-report <path>` | Describes merge-hierarchy roots. See [Merge-hierarchy roots](#merge-hierarchy-roots). `--json`, or `--out f.json`. |
| `view-root <root>` | Opens the viewer on one merge-hierarchy root and its parts. |
| `merge-census <tree> --out prefix` | Every merge root in a cache tree through `hierarchy.merge`: results files, `prefix.roots.csv` with a row per root, totals and worst roots. `--reach-cut`, `--threads`. |

## Suites

A suite names assets, build variants and budgets. See `config/clod_eval/smoke.json`.

```json
{
  "name": "smoke",
  "assets": [ { "name": "dragon", "path": "../../models/dragon.glb" } ],
  "variants": [
    { "name": "voxel", "settings": { "BASICRENDERER_CLOD_COVERAGE_PRESERVATION_MODE": "voxel" } }
  ],
  "analyzers": [ "memory.pages" ],
  "budgets": [ { "metric": "validity.bad_pages", "value": 0 } ]
}
```

- **Paths** are relative to the suite file.
- **Settings** must start with `BASICRENDERER_CLOD_`, so a misspelt name fails instead of quietly
  building the wrong thing. Inherited `BASICRENDERER_CLOD_*` variables are cleared first.
- **`asset_settings`** on a variant selects a per-asset rules file. Without one the variant gets an
  empty file, because the normal lookup walks up from the working directory and would otherwise find
  whichever `config/asset_settings.json` is nearest.
- **`analyzers`** limits what runs. Leave it out to run all of them.
- **Budgets** take `metric`, optional `asset`, `mesh` and `tags`, an `op` of `<=` (default) or `>=`, and
  either `value` or `baseline_factor`. `baseline_factor` only applies in `compare`, against the first
  results file. A budget that matches no metric fails, so renaming a metric cannot silently disable it.
  Budgets apply to the asset rollup (mesh `*`) unless they name a mesh.

The workspace defaults to `clod_eval/<suite>` under the working directory:

```
builds/<variant>/cache/        the caches
builds/<variant>/manifest.json what CLodCacheTool wrote, and the settings that produced it
builds/<variant>/build.json    build key, timing and git state
builds/<variant>/build.log     CLodCacheTool's output
results/<variant>.jsonl .csv   metrics
```

## Reading results

Every metric is a record of id, value, unit, whether lower or higher is better, asset, mesh and tags,
so `compare`, budgets and CSV work on any metric. Mesh `*` is the asset rollup. `compare` shows the
rollup alone where one exists, and `--all` shows each mesh.

| Analyzer | Answers |
|---|---|
| `hierarchy.structure` | LOD levels, triangles per level, how much each level shrinks, cluster fill. |
| `hierarchy.validity` | Counts that should be zero: non-monotonic error, escaping bounds, unreachable groups, undecodable pages. |
| `memory.pages` | Bytes by page kind and stream. `memory.whatif.*` estimates voxel attribute encodings, with the error they would add. |
| `voxel.distribution` | Voxel widths, predicted on-screen size, cell opacity and SGGX spread. |

**`voxel.predicted_footprint_px` is an estimate, not a measurement.** A group is largest on screen
just before it hands over to its children, where its width projects to `width / childError` times the
LOD threshold. That is cube-edge length on the optical axis, so real pixels will be larger off-axis and
along cube diagonals.

**Comparing builds only means something for the same source.** Each results file records a hash of its
terminal triangles that ignores clustering and order, and `compare` exits 2 if two builds disagree. It
also exits 2 if an analyzer's version changed.

## Adding a metric

1. Write an `IAnalyzer` in `CLodEval/src/Analyzers/` and emit through the `MetricSink`. Use
   `Rollup::Sum` or `Rollup::Max` for values that add up to an asset, and `EmitDistribution` for samples.
2. Add its factory to `Analyzers.h` and to the explicit list in `Analyzers.cpp`.
3. Add new source files to `CLodEval/CMakeLists.txt`. There is no globbing.
4. Add a check to `CLodEval/tests/CLodEvalTests.cpp` that compares the number to one you can work out
   independently. A metric that is only checked for being present can be wrong for a long time.

Bump `AnalyzerInfo::version` when a change makes earlier numbers incomparable.

Read pages through `CachedMesh`. `ClusterLOD*` metadata has traps that it handles for you:

- A segment's `pageIndex` is a slot in its group's page map, not a page. Use `ResolveSegmentMeshPage`.
- Voxel mode rewrites `bounds.error` on children of voxel groups to be a cut boundary, so it is not raw
  geometric error in voxel builds.
- Terminal and root groups use a sentinel error near `FLT_MAX`.

## Production caches

For USD sources, `CLodCacheTool` runs the importer the runtime runs. Its geometry, skeleton, manifest and
textures are byte-identical to what the renderer builds for the same settings, when it is given the same
source identifier. `SARPPreprocessor` can still differ in source identifier, tree wind and mesh merging, and
NIF sources in the tool still import skinning as a rigid bind pose. glTF has none of these problems.

To measure what actually ships, point `inspect` or `analyze-cache` at a deployed cache directory. They
read loose `.clodmeta` files. Entries that exist only in a `world.sarppack` are not listed.

## Merge-hierarchy roots

A root (SARP's `docs/merge_hierarchies.md`) is a CLod cache whose assembly instances are part portals. The cache knows
its parts only by ordinal, so whoever builds a root also writes `<root>.clodparts` beside its `.clodmeta`
(`CLodPortalParts.h`): each part's cache key and its metadata path relative to the cache tree, and the palette's
parts. Tools need nothing else. The renderer never reads the file.

`Core/MergeHierarchy.h` reads the tree back from the cache: nodes (a geometry group refining into a page-less
children group), portals (proxy groups) and, from the parts file, the parts. `merge-report`, the `hierarchy.merge`
analyzer and the viewer all use it.

| `merge-report` section | Says |
|---|---|
| Summary | Nodes, levels, portals, parts; node triangles against the parts' coarsest triangles; what fully open draws. |
| By level | Per builder level: fanout, triangles, E(N), radius, E/radius, E over the largest child error (what one merge step adds), child fit (how far a child pokes out of its parent's sphere), draw window. |
| Draw window | E(parent)/E(N): the range of cuts a node's own geometry draws in. Under 1.1 it hardly ever draws. |
| Portal depth | Nodes above each portal. |
| Parts | Sharing, and the most-used parts with their coarse and leaf triangles. |
| Cut sweep | The whole region at one world cut, from the root's split down by halves: nodes, node and part triangles and clusters, opened portals, and "coarsest" portals, which are open but whose part draws nothing finer than the node had merged. |

The sweep is a uniform cut, as in telephoto: the runtime's per-group distance is not modelled. Its distance column
reads a cut as the camera distance where one pixel is that cut, at 1080 lines and 60 degrees.

Analysis runs one mesh per worker thread (`--threads`, default one per hardware thread), each into its own sink,
joined in input order, so results do not depend on the thread count. Roots in a run share a `MergePartLibrary`, so a
part many roots open is opened and measured once: the census of 6387 SaveLab roots takes about 12 s on 24 threads.

`hierarchy.merge` also emits `merge.issue.*` at a reach cut (`--reach-cut`, default 440: one pixel at 1080 lines and
60 degrees from about 410,000 units, the far edge of a radius-100 scene). Nodes with E(N) over it never draw, and
portals under them are open at every distance. See `MergeIssues` in `Core/MergeHierarchy.h`.

`hierarchy.merge` emits the same numbers as metrics, with the sweep at fixed world cuts (2^-2 to 2^14), so two
builds of one region compare cut by cut with `compare` whatever their settings.

`view-root <root>` writes `clod_eval/roots/<entry>/viewer.json` and runs the viewer with `SARP_CACHE_ROOT` set to the
tree the root is in. The viewer builds the root and its parts from their caches, with one flat material per palette
slot coloured by its part, and indexes only those caches. Presets frame the region at each level's mean split. The
**Merge** overlay colours node geometry by merge level and parts opened through portals white; the selected group
names its merge node or, for a part, the root that opens it; the Hierarchy window shows the root's report.

Pixels are joined to hierarchies by global group index, through each registered mesh's `groupsBase`, so clusters a
portal opened resolve to their part rather than to the drawn root.

## Rendering a cache

`CLodCapture --job job.json` runs from the directory holding `shaders/` and `D3D/`, with no visible
window. It must be given the same environment as the build, or it computes a different config hash and
misses the cache:

```powershell
$env:SARP_CACHE_ROOT = "<workspace>\builds\voxel\cache"
$env:SARP_ASSET_SETTINGS_CONFIG = "<workspace>\builds\voxel\no_asset_settings.json"
$env:BASICRENDERER_CLOD_COVERAGE_PRESERVATION_MODE = "voxel"
.\CLodCapture.exe --job job.json
```

```json
{
  "assets": [ { "path": "C:/models/dragon_static.glb" } ],
  "resolution": [ 1280, 720 ],
  "output": "C:/out/capture",
  "views": [ { "name": "near", "position": [ 0, 0.1, 0.2 ], "look_at": [ 0, 0, 0 ], "fov_degrees": 60, "near": 0.001 } ],
  "probes": [
    { "name": "production", "lod_error_pixels": 1.0, "accumulate_frames": 64, "layers": [ "visibility" ] },
    { "name": "leaf", "lod_error_pixels": -1.0, "accumulate_frames": 64, "layers": [ "visibility" ] }
  ]
}
```

- A negative `lod_error_pixels` forces leaf triangles, including groups with exactly zero error.
- `accumulate_frames` writes `hdr-mean.dds` and `hdr-luminance-variance.dds`. Use 64, the period of the
  SGGX sample sequence.
- `bundle.json` is written last, so its presence means the capture finished. Exit code 3 means it failed.

Known limits:

- **Skinned parts do not draw yet.** They are created as scene entities after the first frame, which avoids
  a stall, but nothing is requested or covered: `resident_clod_groups` stays at 1 at every distance tried,
  with or without a held bind pose. The cause is not known.
- **Every view shares one field of view and near plane**, because a running scene can only move its camera.
- **Frame from world bounds, not cache or accessor bounds.** glTF node transforms can shrink a model a lot:
  `dragon.glb` is about 0.12 units wide once its node chain is applied, against a mesh-space radius of 3.36.
- **The readiness gate shows stability only.** It waits for 3 s of idle, unchanging residency. It does not
  yet prove the view has every page it wants, because that needs the page-miss counters.

Do not turn `enableMaterialTextureStreaming` off for a capture. A material row is written once, usually before
its image exists, and with streaming off nothing rewrites it, so every texture reads as black.

### The standard test asset

`models/Trees/Tree_Baltic_Pine_01_A.usd` is the standard voxel test. `models/` is not tracked, so it has to be
copied there by hand. Build and analyze it with the `tree` suite:

```powershell
.\CLodEval.exe run ..\..\..\BasicRenderer\config\clod_eval\tree.json --workspace build\clod_eval
```

The build takes about 4.5 minutes from empty and about a second when nothing changed. It writes 3.1 GB: 23 material
assemblies and their source meshes (about 80 s), plus 11 textures (about 3 minutes, most of it BC7 on the CPU).

To render that cache, give `CLodCapture` the same settings and `"cache_only": true`. It should log that the
assembly loaded without opening the source, and report no violations:

```powershell
$env:SARP_CACHE_ROOT = "<workspace>\builds\voxel\cache"
$env:SARP_ASSET_SETTINGS_CONFIG = "<workspace>\builds\voxel\no_asset_settings.json"
$env:BASICRENDERER_CLOD_COVERAGE_PRESERVATION_MODE = "voxel"
```

## The viewer

`CLodEval view <suite> --variant voxel` opens a window on a built variant, with the environment its captures
get. It renders with the capture pipeline plus the debug and UI techniques, so what it shows is what the captures
measure. Options: `--asset name`, `--size 1920x1080`, `--workspace dir`, and `--script steps.json`, which drives it
unattended. Screenshots go to `view/<variant>/<asset>/screenshots`, and the log to `viewer.log` beside them.

The cut is the idea to hold on to. A group draws while its finest child's error is at or under the world cut and its
own error is over it, where world cut = threshold x world size of a pixel. The viewer shows both, and can hold the
world cut still while the camera moves.

| Window | For |
|---|---|
| **CLod viewer** | Camera presets, including each transition stage the capture found. Telephoto or perspective. Threshold and world cut. Lock world cut, force leaves, voxels only, force depth. A/B flip between two cuts. Every renderer debug view. Hierarchy overlays. |
| **Hierarchy** | Each cached mesh, with how much of the frame it draws. The hand-over map: where each hierarchy changes from triangles to voxels, clickable to set the cut. Level table of the selected mesh. |
| **Selected group** | What drew the clicked pixel: the range of cuts it draws in, its cell size in pixels, its LOD chain to the root, its children and parents, and for a voxel its cube's occupancy slices. Buttons set the cut either side of its entry, its exit and its hand-over. |
| **Transition** | Live sweep of voxel fraction against cut, plotted over the captured curve, with the frames either side of the first switch. Snapshots for comparing. |
| **Compare** | Two snapshots as A, B, difference and paths, or flip, or wipe, over lit, base colour, normal, coverage or path, with zoom and the same numbers as `transition.pop.*`. |

Overlays are painted on the CPU from a matched read of visibility and visible clusters, joined to the hierarchies on
disk, so a new one needs no shader. They are: path, mesh, group, cluster, triangle or cube, LOD depth, voxel level,
cell size in pixels, position in the group's window, cut multiple to the next switch, the cut where each part became
voxels, and selection. Magenta means a pixel's mesh could not be found on disk, which should never happen.

| Input | Does |
|---|---|
| Left click | Selects what drew that pixel. |
| Hold Alt | Reads the pixel under the cursor. |
| Right drag, middle drag or WASD, wheel or QE | Orbits, pans, zooms. |
| `[` and `]` | Steps the threshold by the square root of 2. |
| F | Flips between cuts A and B. |
| O, Tab, F12 | Toggles the overlay, hides the panels, saves a screenshot. |

To look at a cut up close, turn on **Lock world cut** and zoom: the same groups stay on screen, larger. It is exact in
telephoto and close in perspective, because the runtime test uses each group's own depth.

Limits:

- **Numbers assume one scale.** Cell sizes and cuts use the asset's scale, not each instance's transform.
- **Thresholds per pixel are estimates.** The runtime divides by depth minus the group's bounding radius, and the
  viewer uses the pixel's depth. In telephoto the two agree; close up in perspective they can differ.
- **The renderer has no frozen cut.** Lock world cut stands in for one, and groups still stream in and out.
- **Sweeps wait 0.5 s of idle per step**, and snapshots 1 s. The headless capture waits 3 s, so use it for numbers
  that will be reported.
- **Snapshots of fewer than 64 frames are not converged.** Voxel normals are stochastic over 64 frames.
- **Captures and the viewer render raster.** Path tracing is the renderer's default where the device runs it, and
  CLodCapture sets `SARP_RT_PATH_TRACING=0` before the renderer starts.
- **Coverage above 1 at a distance is not an error in itself.** Dropped coverage on thin triangles makes the triangle
  and leaf renders too sparse there, so neither is ground truth for aggregate coverage.

## Design notes

- **Silent rebuilds are prevented with cache-only mode.** `CLodCache::SetCacheOnlyMode` makes the builder
  entry points throw and stops `TryLoadPrebuilt` deleting stale entries. Importers may catch that
  exception and carry on without the mesh, so a caller must also check `GetCacheOnlyViolationCount`.
  `CLodCacheTool --cache-only` exposes it.
- **The build needs no GPU, and the tool checks.** `CLodCacheTool` links the renderer library because the USD
  importer lives there, but it creates no device. `d3d12.dll` and `dxgi.dll` are delay-loaded, and the tool fails at
  exit if either was loaded. Processed textures are compressed on the CPU, and their file layout is computed by
  `PlaceSubresource`, which `CLodCacheFormatTests` pins to sizes the D3D12 device produced.
- **GPU BC7 compression is off (`kEnableGpuBc7Compression`).** It corrupted sRGB base colors, differently between
  runs: lower mips came back blank or too bright. Mip generation alone passes `BR_TEXTURE_JOB_SELF_TEST`.
- **Hermetic builds.** The build key covers the tool binary, assets and settings. Each variant has its
  own cache root, because the cache root and settings are resolved once per process.
- **`CLodCacheFormat` must stay free of USD and D3D12**, or `CLodEval` loses its fast rebuilds.
  `CLodCacheDirectStorage.cpp` and `CLodCacheIdentityUsd.cpp` exist to keep it that way.
