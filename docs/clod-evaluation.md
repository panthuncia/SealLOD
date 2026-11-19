# CLod hierarchy evaluation

`CLodEval` measures what the CLod generator produced, from the command line, so a generator or
shader change can be judged by numbers. Start here to run it or to add a metric.

| Binary | Job | Links |
|---|---|---|
| `CLodCacheTool` | Source asset to complete cache, with no GPU | `BasicRenderer`, but never creates a device |
| `CLodEval` | Orchestration, CPU analysis, comparison | `CLodCacheFormat` only |
| `CLodCapture` | Renders a job file to a bundle of images. `CLodEval` does not drive it or score bundles. | `BasicRenderer` |

Each rebuilds without the others. After a generator edit, rebuild `CLodCacheTool` and rerun the
suite. The suite notices the changed tool and rebuilds the caches. After a metric edit, rebuild
`CLodEval`, and nothing else has to run again.

### Iterating on a few meshes

A full Bistro build of an expensive variant takes a minute or more. To work on the meshes a change hurts most:

```powershell
CLodEval worst config\clod_eval\stage6e.json --workspace out\clod_eval\stage6e --variant topo-uv-uvx-tex1-r02-l2 `
    --baseline topo-uv-uvx-charts-tex1 --metric texture.cut.mesh_delta_e_mean --tag cut=16 --top 5
CLodEval run config\clod_eval\stage6e.json --workspace out\clod_eval\stage6e --variant topo-uv-uvx-tex1-r02-l2 --meshes 662,1211,483
```

- `worst` ranks meshes by one metric, or by its change from `--baseline` (a results file or a variant), keeping each
  glTF mesh's worst primitive and tag combination, and ends with the `--meshes` list for them.
- `--meshes` takes glTF mesh indices (the N in `/glTF/Mesh/N`) on any suite command. Each variant becomes
  `<variant>~m<indices>` (or `~m<count>x<hash>` for more than four), with its own build, results and captures beside
  the full ones. CLodCacheTool builds only those meshes (`--only-mesh-index`), and the scene it writes holds only
  them, so captures draw and frame just those parts. Each mesh's caches are byte-identical to the same mesh's in a
  full build made with the same tool.
- `analyze --meshes` without a subset build measures those meshes of the full build, with no build at all.
- `--reuse-builds` keeps a build whose settings and assets match even though `CLodCacheTool` was rebuilt since. Use it
  for variants the change cannot affect, such as ones that leave the new feature off; a plain `build` redoes them.

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
| `build <suite>` | Builds each variant's caches. Skipped if the tool, assets and settings are unchanged. `--force` rebuilds, `--reuse-builds` ignores a changed tool. |
| `analyze <suite>` | Writes `results/<variant>.jsonl` and `.csv`, and reports over-budget metrics. |
| `run <suite>` | `build`, then `analyze`. |
| `inspect <path>` | Describes caches level by level. `--group N` adds one group, `--json` for scripts. |
| `analyze-cache <path> --out f.jsonl` | Analyzes caches that did not come from a suite, such as `SARPPreprocessor`'s. |
| `compare <a> <b>` | Exit 0 same, 1 different or over budget, 2 not comparable, 3 error. |
| `worst <results or suite>` | Meshes ranked worst first by `--metric`, optionally filtered by `--tag key=value` and against `--baseline`, then a `--meshes` list. |
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
  building the wrong thing. Inherited `BASICRENDERER_CLOD_*` variables are cleared first. A variant with no
  settings builds the builder defaults, which are the recommended build
  ([Defaults](clod-appearance-stage6.md#defaults-and-the-single-cache-format)); to measure against the build without a
  default feature, set its variable to `0` (older suites list as settings what are now defaults, which builds the same).
- **`asset_settings`** on a variant selects a per-asset rules file. Without one the variant gets an
  empty file, because the normal lookup walks up from the working directory and would otherwise find
  whichever `config/asset_settings.json` is nearest.
- **`analyzers`** limits what runs. Leave it out to run all of them.
- **`cut_rules`** adds runtime LOD selection rules to `quality.appearance`'s cut sweep. See
  [Appearance at runtime cuts](#appearance-at-runtime-cuts).
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
| `quality.normals` | Simplified triangles whose shading normals face away from their own face or from the source surface, by depth and by simplifier path. See [Normal integrity](#normal-integrity). |
| `quality.appearance` | Pointwise shading-normal residual, bidirectional distance and coverage against the leaves: of stored simplified geometry, and of what uniform runtime cuts draw against how many triangles. See [Appearance at runtime cuts](#appearance-at-runtime-cuts). |
| `quality.texcoords` | UV drift of stored simplified triangles against the leaves' UVs at matched points (UV set 0, as the pages store it), by depth and by simplifier path. See [Texcoord drift](#texcoord-drift). |
| `quality.texture` | Base-colour difference (CIE76 dE) against the leaves' **texture** at runtime cuts, read from the source .glb's materials, and simulated appearance bakes (`texture_bakes`). See [Texture appearance](#texture-appearance). |
| `quality.specular` | Specular **lobes** at runtime cuts against the leaves' seen through a pixel-sized footprint, with and without the stored normal spread widening roughness (stage 6 step 3). See [Specular appearance](#specular-appearance). |

**`voxel.predicted_footprint_px` is an estimate, not a measurement.** A group is largest on screen
just before it hands over to its children, where its width projects to `width / childError` times the
LOD threshold. That is cube-edge length on the optical axis, so real pixels will be larger off-axis and
along cube diagonals.

### Normal integrity

Simplification keeps original vertices, so a coarse triangle shades with normals authored for other triangles.
Geometric error cannot see it when they point the wrong way, since the positions can be exactly on the surface.
`quality.normals` reads the decoded pages and reports, as fractions of simplified triangle area:

| Metric | Means |
|---|---|
| `normals.inverted_area_fraction` | The shading normal at the triangle's centre points away from its own face: folded triangles and wrong normals alike. Compare with `normals.leaf_inverted_area_fraction`, what the source already has (foliage cards often do). |
| `normals.flipped_vs_source_area_fraction` | The triangle's shading normals, averaged over 7 points, point away from the leaf surface's averaged over the same points. Each point's reference is the nearest leaf triangle facing the same way, so coincident two-sided sheets compare like with like, and averaging keeps a coarse triangle over fine detail from counting as aliasing. This is the headline number. |
| `normals.facing_flipped_vs_source_area_fraction` | No leaf surface facing the triangle's way lies near its centre: its winding folded. |
| `normals.severe_vs_source_area_fraction`, `normals.mean_deviation_vs_source` | More than 60 degrees from the source, and the area-weighted mean angle. |

Each comes as a total, per `depth`, and per `method`: the meshoptimizer path that produced the triangle (`regular`,
`permissive` fallback or `sloppy` fallback), which the builder records on the refined group in
`CLOD_GROUP_SIMPLIFY_METHOD_*` group flag bits. Caches built before those bits existed read `unknown`. Fractions roll up as
ratios of sums (`MetricSink::EmitRatio`), so the asset value is a fraction of the asset's area, not a mean of meshes.
Reference lookups are capped at 400,000 triangles per mesh, and sampled at a stride above that.

To look at one mesh, set `CLOD_EVAL_NORMALS_DUMP=<dir>` and run `analyze-cache` on its `.clodmeta`. That writes the
groups with problems as CSV, an OBJ of the flagged triangles, and a CSV of every triangle with its status. `normals.json`
is the suite for this, over Bistro, with variants for the builder settings that matter:
`BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK`, `BASICRENDERER_CLOD_SIMPLIFY_PERMISSIVE` and
`BASICRENDERER_CLOD_NORMAL_WEIGHT`. A suite's `"textures": false` builds with `--no-textures`.

### Texcoord drift

Simplification that moves vertices gives them new UVs, and a UV solve that does not follow the surface slides the texture across
it, which neither geometric error nor the normal residuals see. `quality.texcoords` samples each simplified triangle at the four
points `quality.appearance` uses (a quarter of its area each) and at its three corners. At each it takes the leaf triangles facing
the same way within 1.1 times the nearest distance (as `quality.normals` matches) and the smallest distance between the decoded UV
and their interpolated UVs at their closest points, in UV units: the smallest, because on a UV seam triangles of two charts are
equally near, and taking the best-facing one there read the other chart's UV (whole UV units across wrap seams) as drift. It has
no texture sizes: 1/1024 UV is a texel of a 1024-texel texture. Interiors carry a floor that exists when no vertex moves (a coarse
triangle interpolates UVs linearly over leaves whose UVs are not linear in it, and matches leaves of other pieces): on Bistro,
no-sloppy's interior mean is 0.10 UV with 6% of area beyond 1/16. Compare builds, not against zero; corners show what a vertex's
own UV did.

| Metric | Means |
|---|---|
| `texcoords.drift_mean`, `texcoords.drift` (p50/p95/p99) | Area-weighted drift of the interior samples, in UV units |
| `texcoords.drift_over_area_fraction` (`over=1/1024,1/256,1/64,1/16`) | Sampled area drifting by more than that many UV units |
| `texcoords.corner_drift_mean`, `texcoords.corner_drift` | The same at the corners, per corner |
| `texcoords.slide_mean` | Interior drift over the matched leaf triangle's UV scale (UV units per mesh unit): how far along the surface it slid, in mesh units |
| `texcoords.unmatched_samples` | Samples with no leaf facing their way |

Totals, per `depth` and per `method`, rolled up as ratios of sums. Simplified triangles past 400,000 per mesh are sampled at a stride.

### Texture appearance

UV drift cannot tell a slide across a uniform wall from one that drags a door's paint over its frame. `quality.texture` reads the
source asset's base colour (factor times texture through its `KHR_texture_transform`, from the .glb or .gltf a suite names, images
embedded, in buffers or in files beside it: PNG and JPEG with stb_image, KTX2 with the generator's decoder,
[clod-appearance-stage6.md](clod-appearance-stage6.md#ktx2-base-colour); `Core/SourceTextures.h`, whose decoded mip chains,
sampling and CIE76 are the generator's own `BasicRenderer/Assets/ClusterLODTextureColor.h`, so the builder's colour-weighted texture
term and this analyzer measure colour identically) and compares, at each uniform
cut, what the coarse UV reads with what the matched leaf reads, both filtered at the mip where the cut is one texel. It needs the source:
`analyze-cache` runs have none and it emits nothing there. The suite asset's `name` must be the source file's stem (`bistro` for
bistro.glb, `zorah_textured_public.v1` for Zorah's .gltf): meshes are matched to suite assets by the importer's source identifier,
which names the file only, so another name leaves the meshes without a source and quality.texture silent. Metrics (`texture.cut.delta_e_mean`, `delta_e_over_area_fraction`,
`alpha_mismatch_area_fraction`, per-mesh `mesh_delta_e_mean`), cut rules and the simulated colour and remap bakes are described in
[clod-appearance-stage6.md](clod-appearance-stage6.md#qualitytexture). `texture.cut.triangles` counts textured meshes only; to compare
builds at equal cost, interpolate against `appearance.cut.triangles` at the same rule and cut. Builds with the colour-weighted texture
term (`BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_COLOR`, suite `config/clod_eval/stage6b.json`) key each mesh with an
`|appearance=<hash>` subset suffix; `CachedMesh::Name` drops it, so such builds compare mesh by mesh with builds without it.

Stage 6 step 2's UV remap blocks (`BASICRENDERER_CLOD_ERROR_CHANNELS_TEXTURE_REMAP`, suite `config/clod_eval/stage6c.json`) are read
as material evaluation reads them: a remapped triangle's samples take the block's UV (`clod_uv_remap::Evaluate`, the reference the
shaders follow), filtered with the remap's own UV scale there. A mesh with blocks emits `texture.remap.meshes`,
`texture.remap.area_fraction` (sampled simplified area that reads through a block), `nearest_area_fraction` (of that, the part in
sub-triangles that take a node instead of interpolating), and the remapped samples' own dE at their segment's finest cut with and
without the block (`texture.remap.delta_e_mean` against `plain_delta_e_mean`, and `delta_e_over_area_fraction` against
`plain_delta_e_over_area_fraction` beyond dE 3): what the remap recovers where it applies, within one build. `memory.pages` counts the
blocks as stream `uv_remap`.

**Along-normal correspondence** (stage 6 step 2 refinement). The reference above is the nearest leaf point facing the same way, which on
layered geometry (a door frame in front of a recessed panel) can be a surface a viewer does not see from the coarse surface. Every
sample therefore also takes a second reference: the frontmost leaf crossed by the line through it along the coarse face normal, within
the larger of the segment's error and three times the nearest distance either way, facing the same way within 60 degrees (the nearest
match where nothing is crossed; `texture.normal_crossed_samples` counts the crossings found). It is what a viewer looking at the coarse
surface sees of the leaves, the rule remap layout 2 builds with (independently implemented here, over the leaves). Unbaked cuts emit
`texture.cut.normal.delta_e_mean` and `texture.cut.normal.delta_e_over_area_fraction` (dE 3 and 10) beside the nearest-point metrics,
and remapped meshes `texture.remap.normal.delta_e_mean`, `normal.plain_delta_e_mean` and their beyond-dE-3 fractions. The two
references disagree most where remaps apply; neither is a rendered image, and live crops remain the judge of visible damage.

### Specular appearance

The normal residual of `quality.appearance` says how wrong a Lambertian term is; a glossy highlight depends on the normal far more
sharply, and what a pixel sees of fine source detail is a spread of normals, not one. `quality.specular` (stage 6 step 3,
[clod-appearance-stage6.md](clod-appearance-stage6.md#step-3-roughness-from-the-normal-spread-a-coarse-surface-hides-implemented))
compares, at four points of each sampled simplified triangle, the coarse surface's specular lobe with the mixture of the leaves'
lobes inside a footprint: a disc of half the segment's enter error (a pixel where it starts to draw at a one-pixel threshold; times
the suite's `specular_footprint`, default 1) in the coarse triangle's plane, by the 7-point disc rule, each point matched to the
nearest same-facing leaf. The coarse lobe is evaluated three ways for perceptual roughness 0.25 and 0.5 (tag `roughness`): plain,
widened by the stored normal spread exactly as material evaluation widens it (`alpha'^2 = alpha^2 + min(v, cap alpha^2)`, cap the
suite's `specular_spread_cap`, default 1 as `clodNormalSpreadRoughnessCap`), and an oracle widened the same way by the spread measured
at that point against the footprint's leaves.

| `specular.cut.*` | Definition |
|---|---|
| `triangles` | Triangles drawn |
| `lobe_distance`, `plain_lobe_distance`, `oracle_lobe_distance` | Area-weighted lobe distance over everything drawn (leaves exact): each lobe a Gaussian in slope space (per-axis variance `alpha^2 / 2`), `1 - 2 <R, C> / (<R, R> + <C, C>)`, 0 identical, 1 disjoint. It counts where a highlight is and how wide and bright it is |
| `simplified_lobe_distance` and its `plain_` / `oracle_` forms | The same over drawn simplified area only |
| `response_error`, `plain_response_error` | GGX specular (Fresnel 1, Smith height-correlated, times N.L) at five view / light pairs in each coarse triangle's frame, `sum |coarse - reference| / sum reference` over drawn simplified samples |
| `energy_ratio`, `plain_energy_ratio` | Drawn over reference specular at those pairs |
| `spread_mean`, `oracle_spread_mean`, `spread_area_fraction` | Mean stored (and oracle) `v` over drawn simplified area; drawn area whose triangles carry spread |

Widening never changes selection, so plain and widened compare at exactly the same triangles. `specular.spread_area_fraction`,
`specular.unmatched_samples` and `specular.coarse_zero_normal_samples` are per mesh. Simplified triangles past 60,000 per mesh are
sampled at a stride; Bistro takes about 7 minutes per variant. `memory.pages` counts the spread blocks as stream `normal_spread`.
See [clod-appearance-stage6.md](clod-appearance-stage6.md#step-2-uv-remap-blocks-for-chart-crossing-groups-implemented).

### Appearance at runtime cuts

`quality.appearance` measures decoded simplified geometry against the leaves point by point, with no averaging that
could cancel a residual, and then measures what the runtime would draw at fixed cuts. The cut sweep is the number to
judge a builder change by: triangles drawn against error, cut by cut. A build that looks better only by drawing more
triangles at the same cut shows it there. The pointwise machinery is `Core/Appearance.h`.

**Sampling.** Each simplified triangle is measured at four points: its centroid and the points at weights (2/3, 1/6, 1/6)
and permutations, each standing for a quarter of its area. Corners are left out, since kept original vertices sit on the
source by construction. Past 250,000 simplified triangles per mesh they are taken at a stride, and sampled sums are scaled
to the exact area. Leaves replaced at a cut are measured at their centroids, at most 65,536 per distinct cut selection.
Every number is sampled: maxima are named `sampled_max` and are not Hausdorff bounds. A full Bistro analysis takes
about 25-35 s on 24 threads (`--analyzer quality.appearance`).

**Correspondence.** At each point the nearest leaf point is found (distance d), and the nearest leaf point on a triangle
whose face normal has a positive dot product with the measured triangle's. The same-facing one is the reference when it
is no more than `clamp(max(0.5 tol, 0.1 d), 1e-6 size, 0.02 size)` farther than the nearest, which keeps coincident
two-sided sheets apart. Otherwise the point is **ambiguous** and is measured against the nearest, which is what a folded
triangle meets. A point farther than `max(2 tol, 1e-3 size)` from any leaf is **unmatched**: it has no normal residual
and is counted, not dropped. `tol` is the allowed error below, `size` the leaves' bounding diagonal, in mesh units.

**Normals.** Corner normals are normalised as decoded, interpolated and normalised again. An interpolated normal shorter
than 0.01 cancels. A coarse normal that cancels counts as 180 degrees (chord² 4) and is counted
(`coarse_zero_normal_*`); a source normal that cancels leaves nothing to compare and is counted
(`source_zero_normal_*`) and excluded. For unit normals chord and angle satisfy `|n1 - n2| = 2 sin(theta / 2)`.

| Metric | Definition | Unit |
|---|---|---|
| `normal_q` | Q_N = ∫ ‖n_coarse − n_source‖² dA over measured area | mesh units² |
| `normal_q_per_area` | Q_N over measured area: the area-weighted mean chord² (0 to 4) | ratio |
| `angle`, `chord` | Area-weighted p50/p95/p99 from 8-bin-per-octave histograms; under 1e-4 reads 0 | radians, ratio |
| `angle_over_area_fraction` (`over=10deg/30deg/90deg`) | Measured area whose residual exceeds the angle | ratio |
| `distance_over_error` | Coarse-to-leaf distance over the triangle's own declared error (its refined group's error) | ratio |
| `spurious_area_fraction` | Area farther from the leaves than that error | ratio |
| `ambiguous_area_fraction`, `unmatched_area_fraction` | As defined above, of all sampled area | ratio |

These come as a total, rolled up by `depth` and by `method` as for `quality.normals`. Quantiles roll up exactly as far
as their bins resolve: `MetricSink::EmitQuantiles` adds the meshes' histograms (`Rollup::Quantiles`), so the asset's
p95 is the asset's, not a mean of meshes'.

**The cut sweep.** At each cut T = 2^k mesh units, k = -14 to 8, a segment draws when `enter <= T < leave`: `leave` is its
own group's error, `enter` the error of the group it refines (0 for leaves); errors at or over 1e19 are sentinels and
read as infinite. That is the runtime's pair of tests (the node's error over the threshold, the refined child's at or
under it) for a uniform, telephoto cut, as `hierarchy.merge`'s sweep uses. Per-group distance, perspective and instance
scale are not modelled, so the cuts are in mesh units and compare meshes as if they shared a scale. Everything is
whole-mesh, so nothing depends on how groups partition the surface. Leaves drawn as themselves are exactly on the source.
Simplified triangles drawn are measured with tol = T; the leaves they replace are measured against everything drawn.

| `appearance.cut.*` | Definition |
|---|---|
| `triangles`, `simplified_triangles` | Triangles drawn, and how many are simplified |
| `simplified_area_fraction` | Simplified area over drawn area |
| `normal_q`, `normal_q_per_area` | Q_N of what is drawn (leaves contribute zero), over drawn measured area |
| `angle`, `angle_sampled_max`, `angle_over_area_fraction` | Distribution over drawn area, leaves at zero |
| `spurious_area_fraction` | Drawn area farther than T from the leaves |
| `coarse_to_source_over_cut` | Drawn-to-leaf distance over T: quantiles and `_sampled_max` |
| `missing_area_fraction` | Leaf area farther than T from anything drawn: removed surface, which drawn-to-leaf distance cannot see |
| `source_to_drawn_over_cut` | Leaf-to-drawn distance over T: quantiles and `_sampled_max` |
| `source_normal_q_per_area` | ∫ ‖n_source − n_drawn‖² dA over leaf area, against the drawn surface's normal at the corresponding point |
| `ambiguous_area_fraction`, `unmatched_area_fraction`, `source_ambiguous_area_fraction`, `source_unmatched_area_fraction` | Correspondence confidence, each way |

Each carries a `cut` tag (`{:g}` of T), so `compare` matches two builds cut by cut. Only the asset rollup is written,
except `triangles`, `normal_q_per_area`, `angle_over_area_fraction over=90deg` and `missing_area_fraction`, which are
also written per mesh at cuts where the mesh draws simplified triangles, to find the meshes behind a rollup.

Under unit-intensity Lambertian light with matched albedo and unchanged visibility, `|ΔI| <= |n_coarse - n_source|`, so
`p² Q_N` estimates screen-integrated squared shading error at p pixels per mesh unit. That is a starting estimate under
those assumptions, not a bound for glossy materials, shadows or visibility.

**Selection rules.** A suite's `"cut_rules"` sweep the cuts again under the runtime's error-channel selection
([clod-simplification-validation.md](clod-simplification-validation.md#stage-3-error-channels-and-appearance-aware-selection)):
for each rule, `enter` and `leave` are what `CLodSelectionError` (`workGraphCulling.hlsl`) gives the two groups, and every
`appearance.cut.*` metric is emitted again with a `rule` tag. The untagged sweep is always the stored errors, unchanged.

```json
"cut_rules": [
  { "name": "w0.03", "geometric": "quadric", "appearance_weight": 0.03 },
  { "name": "measured", "geometric": "measured" }
]
```

`geometric` is `quadric` (the stored error, the default), `measured` or `max`, as the runtime setting `clodLodGeometricChannel`;
`appearance_weight` is `clodLodAppearanceWeight` (default 0). The channels are read from the traversal nodes the way
`MeshManager::AddMesh` uploads them (`Core/ErrorChannels.h`): a cache built without them, or with voxel groups, forks, merge
proxies or assemblies, selects with its stored errors under every rule, as the renderer does. A rule `quadric` with weight 0 is
the untagged sweep exactly (a test checks every metric). Rules are analysis only and do not change the build key. Meshes with
channels also emit `appearance.channels.meshes`, `appearance.channels.monotonic_violations` (segments whose group has a smaller
channel than the group it refines; 0 by construction) and `appearance.channels.measured_above_quadric_group_fraction`.

To compare builds at equal cost rather than equal cut, interpolate each build's curve to the same `triangles`: two builds
whose errors are scaled differently (a normal weight, say) draw different amounts at one T.

Limits: triangle branches only (a voxel branch view is skipped, and meshes with voxel groups, merge proxies or merge
children are measured as stored but left out of the sweep, counted in `appearance.cut_excluded_meshes`); positions and
normals only, no UVs, tangents or materials; no filtering over spatial scales.

**Comparing builds only means something for the same source.** Each results file records a hash of its
terminal triangles that ignores clustering and order, and `compare` exits 2 if two builds disagree. It
also exits 2 if an analyzer's version changed.

## Adding a metric

1. Write an `IAnalyzer` in `CLodEval/src/Analyzers/` and emit through the `MetricSink`. Use
   `Rollup::Sum` or `Rollup::Max` for values that add up to an asset, `EmitRatio` for fractions (rolled up as a
   ratio of sums), `EmitDistribution` for samples, and `EmitQuantiles` with a `LogHistogram` for weighted quantiles
   that roll up exactly. `EmitRollupOnly` and `EmitRatioRollupOnly` skip the per-mesh record, for sweeps whose
   per-mesh values would swamp the results file.
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

A root (SARP's `docs/merge_hierarchies.md`) is a merged mesh: a CLod cache in `clod/merged/` whose assembly instances
are part portals, and whose metadata names the parts (`ClusterLODPrebuiltData::externalParts`: each part's cache key,
build hash and host address, and the palette's parts; `ClusterLODMergedMesh.h`). Tools find a part's metadata in the
same cache tree with `CLodCache::RelativeMetadataPath`, so they need nothing else.

`Core/MergeHierarchy.h` reads the tree back from the cache: nodes (a geometry group refining into a page-less
children group), portals (proxy groups) and, from the root's part table, the parts. `merge-report`, the `hierarchy.merge`
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
- `accumulate_frames` writes `hdr-mean.dds` and `hdr-luminance-variance.dds`. Raster triangles draw the same every
  frame, so 1 is enough for them, and 1 is what a suite's capture block now defaults to; voxels need 64, the period of
  the SGGX sample sequence, so voxel suites (`tree.json`) set `"accumulate_frames": 64`.
- Captures are raster only. `SARP_RT_PATH_TRACING` is overridden (with a warning if it asked for path tracing), and a
  job with `"path_tracing": true` fails: every layer is read inside a raster pass (`EvaluateMaterialGroupsPass`,
  `DeferredShadingPass`) that the path-traced pipeline removes.
- `bundle.json` is written last, so its presence means the capture finished. Exit code 3 means it failed.

Known limits:

- **Skinned parts do not draw.** They are created as scene entities after the first frame, which avoids
  a stall, but nothing is requested or covered: `resident_clod_groups` stays at 1 at every distance,
  with or without a held bind pose. The cause is not known.
- **Every view shares one field of view and near plane**, because a running scene can only move its camera.
- **Frame from world bounds, not cache or accessor bounds.** glTF node transforms can shrink a model a lot:
  `dragon.glb` is about 0.12 units wide once its node chain is applied, against a mesh-space radius of 3.36.
- **The readiness gate shows stability only.** It waits for 3 s of idle, unchanging residency. It does not
  prove the view has every page it wants, which would need the page-miss counters.

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

## Implementation notes

- **Silent rebuilds are prevented with cache-only mode.** `CLodCache::SetCacheOnlyMode` makes the builder
  entry points throw and stops `TryLoadPrebuilt` deleting stale entries. Importers may catch that
  exception and carry on without the mesh, so a caller must also check `GetCacheOnlyViolationCount`.
  `CLodCacheTool --cache-only` exposes it.
- **The build needs no GPU, and the tool checks.** `CLodCacheTool` links the renderer library because the USD
  importer lives there, but it creates no device. `d3d12.dll` and `dxgi.dll` are delay-loaded, and the tool fails at
  exit if either was loaded. Processed textures are compressed on the CPU, and their file layout is computed by
  `PlaceSubresource`, which `CLodCacheFormatTests` pins to sizes the D3D12 device produced.
- **GPU BC7 compression is off (`kEnableGpuBc7Compression`).** It corrupts sRGB base colors, differently between
  runs: lower mips come back blank or too bright. Mip generation alone passes `BR_TEXTURE_JOB_SELF_TEST`.
- **Hermetic builds.** The build key covers the tool binary, assets and settings. Each variant has its
  own cache root, because the cache root and settings are resolved once per process.
- **`CLodCacheFormat` must stay free of USD and D3D12**, or `CLodEval` loses its fast rebuilds.
  `CLodCacheDirectStorage.cpp` and `CLodCacheIdentityUsd.cpp` exist to keep it that way.
