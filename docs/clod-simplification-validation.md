# CLod simplification validation

Stage 1 of the appearance-preserving simplification plan
([../../docs/clod_appearance_simplification_plan.md](../../docs/clod_appearance_simplification_plan.md)): every
simplification result the cluster LOD builder produces (regular, permissive and sloppy) can be checked against the
**original** surface before the builder accepts it. A rejected result is replaced by another candidate, or the group
keeps its finer geometry. Validation only chooses among results. It never edits indices, positions or normals, so it
cannot change the topology the next level simplifies (the lesson recorded in the plan about rewritten indices).

It is **off by default**. With it off, the build, its caches and their hashes are unchanged.

> **Defaults since 2026-11-17.** This document records stages 1-5 as they were introduced: each off by default, with environment
> overrides hashed into the cache key only when set. The builder defaults are now the recommended build (no sloppy fallback, stage 2
> replacement vertices without measured error, the UV transfer follow-up, stage 3 error channels, stage 5 topology with UV seams relaxed
> under the chart test, plus stage 6's texture term and colour-weighted chart test); validation (stage 1), optimal placement (stage 4)
> and measured error stay off, and the key hashes every setting's effective value. "Off by default" and "hashed only when set" below
> describe the time of each stage. See [Defaults and the single cache format](clod-appearance-stage6.md#defaults-and-the-single-cache-format).

## Where it lives

| Piece | File |
|---|---|
| Policy: which candidates, in what order; terminal on rejection; counters; group outcome | [clusterlod.h](../BasicRenderer/include/ThirdParty/meshoptimizer/clusterlod.h) (`simplifyValidated`, `clodConfig::simplify_validate*`, `clodValidateCandidate`, `clodValidationStats`) |
| Measurements: original-surface index, checks, thresholds | [ClusterLODSimplifyValidation.h](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.h) / [.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.cpp) |
| Wiring, settings, logging, group flags | `BuildClusterLODArtifactsFromGeometry` in [ClusterLODClustering.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODClustering.cpp) |
| Settings | `ClusterLODBuilderSettings::simplifyValidation*` in [ClusterLODTypes.h](../BasicRenderer/include/BasicRenderer/Assets/ClusterLODTypes.h) |

The builder indexes the original triangles once per mesh in a BVH (`SimplifyTriangleBvh`), and before each level it
indexes that level's whole surface with the group owning each triangle (`clodValidateLevel`). Candidates are
validated from the parallel iteration tasks; the validator is read-only while they run.

## Policy (clusterlod.h)

For each target, starting at the configured one (`simplify_ratio`) and then `retries` less aggressive targets evenly
spaced up to `simplify_threshold`:

1. The attribute-aware result (regular, or the permissive fallback when configured), if it reaches the target.
2. If it got stuck above the target: the **sloppy** result at that target (when sloppy is enabled).
3. Then the stuck attribute-aware result itself, if it is under `simplify_threshold`.

The first candidate the validator accepts is used, with its own error (sloppy's scaled by `simplify_error_factor_sloppy`).
Candidates above `simplify_threshold` are never offered: those groups are terminal as before. If every offered
candidate is rejected, the group is **terminal**: it keeps its finer geometry and reports `FLT_MAX` error, as a stuck
group always has. Sloppy output is therefore accepted only when it validates (quarantined).

Each group's outcome is stored in `clodGroup::simplify_validation` and in group flag bits 9-10
(`CLOD_GROUP_SIMPLIFY_VALIDATION_*` in `ClusterLODShaderTypes.h`; read by no shader): 0 not validated, 1 first candidate
accepted, 2 accepted after a rejection, 3 every candidate rejected (terminal).

## Checks (ClusterLODSimplifyValidation.cpp)

All checks sample each triangle at four points (the centroid and the points at barycentric weights (2/3, 1/6, 1/6) and
permutations), each standing for a quarter of its area. Nothing is averaged before a test: each sample passes or fails.

The **radius** is `radiusScale` times the error the group would report if the candidate is accepted (its own error
merged with its clusters' errors, as `runIterationTask` would store it), floored at 1e-4 of the mesh's bounding
diagonal.

**Matching to the original.** A candidate sample is matched to the original triangles about as near as the nearest one
(within 1.1 times its distance plus 1e-5 of the diagonal), choosing the one whose face agrees best with the sample's
face. Coincident opposite sheets of a two-sided surface therefore resolve to the side the sample faces. Nothing within
the radius: the sample is **unmatched**.

| Reason (`clodValidateReject_*`) | Failing sample | Rejects when |
|---|---|---|
| `Fold` | The matched original face meets the candidate's face beyond `foldAngle` | new folded area (below) over candidate area > `foldArea` |
| `Shading` | The candidate's interpolated normal and the original's interpolated normal at the matched point meet at or beyond `shadingAngle`, or the candidate's cancels out | new severe area over candidate area > `shadingArea` |
| `Spurious` | Candidate sample unmatched (farther than the radius from any original surface) | unmatched area over candidate area > `coverageArea` |
| `Coverage` | Input sample with no candidate triangle within the radius whose face meets it within 120 degrees | uncovered area over input area > `coverageArea` |
| `SourceCoverage` | Original triangle owned by the group (below) whose centroid has no candidate triangle within the radius meeting it within 120 degrees | uncovered area over owned area > `coverageArea` |

**New, not inherited.** A group's input may already fold or shade wrongly against the original. Fold and shading
compare against that baseline: the input is measured against the original the same way (unbounded radius) and its own
fractions are added to the allowance. A depth-0 input is the original, so its baseline is zero. The baseline is measured
once per group, only when a candidate exceeds the bare allowance, and kept for that group's later candidates. Once it is
known, sampling a candidate stops early when both fold and shading are certain to fail.

**Coverage is checked both ways and against the original.** `Coverage` compares the candidate with its own input.
`SourceCoverage` compares it with the original surface the group stands for, so drift accumulated over earlier levels
cannot hide: the original triangles near the input's bounds (grown by the radius) whose nearest point on the current
level's surface lies on this group's input are the group's, and each needs candidate surface within the radius. Large
regions are sampled by stride (at most 4096 original triangles per candidate). The 120-degree facing allowance lets a
shallow recess wall fold into its panel while the opposite side of a coincident sheet does not count as cover.

Shading uses the source vertex normals at `MeshVertexLayout::NormalOffset`; meshes without normals skip it.

## Settings

All default to the values below; `simplifyValidation` is off. Asset-settings JSON keys are the field names; environment
overrides are hashed into the cache key only when set.

| Field / JSON key | Environment | Default | Meaning |
|---|---|---|---|
| `simplifyValidation` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION` | off | Turns validation on |
| `simplifyValidationRetries` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_RETRIES` | 2 | Less aggressive targets after the configured one (capped at 16) |
| `simplifyValidationFoldAngle` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_FOLD_ANGLE` | 90 | Degrees |
| `simplifyValidationFoldArea` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_FOLD_AREA` | 0.01 | Fraction |
| `simplifyValidationShadingAngle` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_SHADING_ANGLE` | 75 | Degrees |
| `simplifyValidationShadingArea` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_SHADING_AREA` | 0.02 | Fraction |
| `simplifyValidationRadiusScale` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_RADIUS_SCALE` | 1 | Radius over the group's error |
| `simplifyValidationCoverageArea` | `BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_COVERAGE_AREA` | 0.01 | Fraction, for all three coverage checks |

`BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_LOG=1` (diagnostics, not hashed) logs one line per candidate with its measured
fractions.

These thresholds are experimental, not production constants.

## Counters

Each mesh build logs one line, `ClusterLOD simplify validation: ...`, with: candidates offered, accepted, rejected,
rejections per reason (one candidate may count under several), groups accepted at the first candidate
(`groups_first`), after a rejection (`groups_retried`), made terminal (`groups_terminal`), sloppy candidates accepted and
rejected, and CPU time (index build, total validation, and its phases: candidate against original, coverage, baseline,
source coverage). Sum the lines over a `build.log` for an asset.

## Results on Bistro

Suite [config/clod_eval/stage1.json](../config/clod_eval/stage1.json), measured 2026-11-16. "Loose" is shading 90
degrees / 10%, fold 5%, coverage 5%. Aggregates are `quality.normals` over all stored simplified geometry;
the cut rows interpolate `appearance.cut.normal_q_per_area` to equal drawn-triangle counts.

| | baseline | no-sloppy | validate | validate, no sloppy | loose | loose, no sloppy |
|---|---:|---:|---:|---:|---:|---:|
| flipped vs source (area) | 12.0% | 1.56% | 0.11% | 0.09% | 2.31% | 0.91% |
| severe vs source (area) | 20.8% | 5.18% | 0.53% | 0.46% | 7.21% | 3.56% |
| inverted (area) | 16.6% | 4.36% | 0.20% | 0.18% | 2.85% | 1.74% |
| sloppy flipped (area) | 28.5% | - | 0.72% | - | 3.97% | - |
| coarsest triangles | 183k | 466k | 1.30M | 1.31M | 620k | 868k |
| groups | 4686 | 4065 | 3121 | 3046 | 3962 | 3496 |
| groups terminal by rejection | - | - | 621 | 491 | 377 | 260 |
| cut Q_N/area at 1.5M triangles | 0.0053 | 0.0008 | 0.0037 | 0.0029 | 0.0060 | 0.0030 |
| cut Q_N/area at 1M triangles | 0.0140 | 0.0151 | - | - | 0.0232 | 0.0428 |
| cut missing area at 1M triangles | 0.17% | 0.25% | - | - | 0.07% | 0.02% |

What this establishes:

- The checks find real failures. Regular meshoptimizer output commonly folds 2-9% of a group's area against the source
  (cumulative rotation), and sloppy output fails most of the time (904 of 1013 sloppy candidates rejected at defaults).
- Fold and shading each limit reduction on their own: either one alone leaves about 1.1M coarsest triangles. Coverage
  alone barely limits it, and does nothing for normals.
- **Rejection alone does not pay at runtime cuts.** A group left terminal draws leaves at every cut, so at an equal
  triangle budget the rest of the mesh must go coarser. At 1.5M triangles no-sloppy beats every validated variant, and
  validated builds cannot reach 1M at defaults. Their stored levels are far cleaner, and missing surface at cuts
  drops (the coverage checks work), but the curve says better candidates, not stricter rejection, are what's missing:
  stage 2.

Cost: one Bistro build goes from 2.5 s to 6.5 s wall time (run alone). Validation takes about 115 s of CPU over 24
threads: candidate against original 49 s, input coverage 23 s, source coverage 24 s, baseline 19 s, original indexing
2.4 s. Retries also re-run the simplifier. With validation off, nothing runs.

## Stage 2: replacement vertices

Stage 2 of the plan: simplification may emit **new vertices** (moved positions, normals fitted from the original, UVs
solved with the positions) instead of only reusing source vertices, so that candidates shade like the original and pass
validation. It is **off by default** (`simplifyUpdate`); with it off nothing runs, and caches and hashes are unchanged
(checked on Bistro: the same `hash=` values and byte-identical caches as stage 1's baseline and no-sloppy builds).

This is deliberate optimized geometry, not output repair: new vertices are part of the build representation and feed later
levels, and every level fits normals from the **original** surface, never from a coarser level's shading.

### Where it lives

| Piece | File |
|---|---|
| Policy: the update candidate, its order among the others, the serial commit, ids, remap and locks | [clusterlod.h](../BasicRenderer/include/ThirdParty/meshoptimizer/clusterlod.h) (`simplifyUpdate`, `acceptUpdate`, `commitReplacements`, `simplifyWithReplacements`, `clodConfig::simplify_update_*`, `clodReplacementCandidate`, `clodReplacementBatch`, `clodReplacementStats`) |
| Build vertex store, normal fit, UV bounds, records, commit, measured error | [ClusterLODReplacementVertices.h](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODReplacementVertices.h) / [.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODReplacementVertices.cpp) |
| Original-normal sampling, `MeasureDeviation` | `SimplifyValidator` ([ClusterLODSimplifyValidation.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.cpp)) |
| Wiring, UV attribute streams, settings, logging, group flag | `BuildClusterLODArtifactsFromGeometry` in [ClusterLODClustering.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODClustering.cpp) |

### Representation

- **Private buffers.** Each group's update candidate copies the group's vertices (positions, the whole attribute row,
  lock flags) into task-local arrays, in increasing vertex id, and runs `meshopt_simplifyWithUpdate` on them. Nothing
  shared is written while tasks run.
- **Locks.** Group-boundary vertices carry `meshopt_SimplifyVertex_Lock`. meshoptimizer 1.3 moves only manifold and complex
  vertices (`solvePositions`) and never writes externally locked ones (`finalizeVertices`); clusterlod also restores and
  counts any locked vertex that moved (`locked_moved`, 0 on Bistro) and never replaces a locked vertex, so boundary
  positions and shading match the neighbours and every mixed cut.
- **Which vertices become new.** Every vertex the result uses that moved, plus unmoved unlocked vertices whose fitted
  normal beats their current one and differs by more than `simplifyUpdateRefitAngle`. An unlocked vertex's position is
  used by its group only, so its replacement changes nothing another group sees.
- **Ids and commit.** An accepted candidate's result names new vertices `0x80000000 | n`. In `clodBuildEx`'s serial output
  and split loop, in group order and before the group's result is clusterized, the commit callback appends them to the
  builder's store, and they get ids from the current vertex count onwards; the result is rewritten to those ids. Ids and
  bytes therefore do not depend on thread count (Bistro rebuilds are byte-identical).
- **Per-vertex state that grows with them.** The builder's store holds full vertex records (a copy of the source, then
  replacements: position, fitted normal, tangent made orthogonal to it, UVs, the replaced vertex's color), every UV set,
  the simplification attribute stream and the coverage-priority flags (inherited from the replaced vertex); the commit
  updates `clodMesh`'s pointers and count. clusterlod extends its position remap (a replacement that kept its position
  shares its parent's entry; moved wedges of one position share theirs) and its lock flags, including protect bits on
  attribute seams as for source vertices. Pointers change only during the commit, while no task runs.
- **Output.** The output callback and `BuildClusterLODGroupOutput` resolve every id from the store, so new vertices are
  ordinary vertices in the group vertex chunks: no page or shader format changes. Groups simplified by an update candidate
  get `CLOD_GROUP_FLAG_SIMPLIFY_REPLACED` (bit 11, diagnostics, read by no shader).
- **Restrictions.** Meshes without normals, with skinning (moved positions would need new weights), or with
  `preserveImportedNormals` off (the output recomputes normals) are built without it; the build logs that.

### UVs

UV streams (the record texcoord, then as many UV sets as meshoptimizer's 32 attributes allow) are appended to the
simplification attribute stream with weight 0.01 and passed only to update candidates (`clodMesh::update_attribute_*`), so
`simplifyWithUpdate` solves them with the positions while they hardly change which collapses happen; the regular path still
reads only its own attributes. Charts are respected because meshoptimizer solves attributes per wedge (a wedge's triangles
lie in one chart) and seam wedges share their position. A moved vertex's solved UV is clamped into the box spanned by its own
and its neighbours' stored UVs (`uv_clamped`), and a non-finite one keeps its old value (`uv_nonfinite`, 0 on Bistro), so by
induction UVs never leave the original's range. An unmoved replacement keeps its UVs. UV sets past the attribute budget keep
the replaced vertex's values.

### Normal fitting

For each used, unlocked vertex the fit samples every triangle around it at five points (the vertex, towards it from the
centre and from each edge midpoint, and the centre). At each point it takes the original's interpolated, normalized normal
at the side-aware match (`SimplifyValidator::OriginalNormalAt`, built on `MatchFacing`), weighted by triangle area times the
match's facing; matches facing away are skipped, so a recess wall the face folded over lends it no shading. Wedges are
fitted separately (each from its own triangles), which keeps hard edges. Candidates:

1. the current normal;
2. the hat-weighted average of the original normals (each sample weighted by the vertex's interpolation share);
3. the vertex's least-squares normal with its neighbours' current normals held (one Gauss-Seidel step of fitting the patch).

Each is scored by the area-weighted squared chord between the shading it would produce (interpolated with the neighbours'
normals and normalized) and the original's, and the lowest wins; an interpolation that cancels scores as 180 degrees. A
vertex with nothing to sample keeps its current normal unless that faces away from its triangles (`fit_fallbacks`).
meshoptimizer's linear normal solve is never used as the output normal.

### Policy and errors

With validation, each target first offers the update candidate, then stage 1's candidates in their order. Its output view
points at the private positions and fitted normals, so the same checks apply. Without validation the update candidate
replaces the regular result; when it is stuck above the target and sloppy is on, sloppy runs as before.

With `simplifyUpdateMeasuredError` (off by default: on Bistro it removed missing area at cuts but raised normal error by 45-80% at 1.5M and 1M drawn triangles, and normal error is the more visible defect) every candidate's reported error becomes the larger of its own
and `MeasureDeviation`: the largest distance from an output sample (four per triangle and the corners) to the original, and
from an input sample to the output. With the input's own error that bounds how far the original is from the output. It is
sampled, not certified. The merge with the clusters' errors and the validation radius use it.

### Settings

| Field / JSON key | Environment | Default | Meaning |
|---|---|---|---|
| `simplifyUpdate` | `BASICRENDERER_CLOD_SIMPLIFY_UPDATE` | off | Turns replacement vertices on |
| `simplifyUpdatePositions` | `BASICRENDERER_CLOD_SIMPLIFY_UPDATE_POSITIONS` | on | Off: `simplifyWithAttributes`, nothing moves, normals are still fitted |
| `simplifyUpdateRefitAngle` | `BASICRENDERER_CLOD_SIMPLIFY_UPDATE_REFIT_ANGLE` | 5 | Degrees an unmoved vertex's best fitted normal must differ by to replace it |
| `simplifyUpdateMeasuredError` | `BASICRENDERER_CLOD_SIMPLIFY_UPDATE_MEASURED_ERROR` | off | Report max(own error, measured deviation) for every candidate |

Environment overrides are hashed into the cache key only when set. Each build logs `ClusterLOD simplify update: ...`:
update candidates built, offered and accepted, replacement vertices committed and how many moved, locked vertices moved,
fit counters (vertices fitted, fallbacks, refits, UV clamps and non-finite UVs, over every candidate fitted, accepted or
not), measurements and how often they raised the error, and CPU time for fitting and measuring.

### Results on Bistro

Suite [config/clod_eval/stage2.json](../config/clod_eval/stage2.json), measured 2026-11-16, Bistro without textures. Cut rows
interpolate `appearance.cut.*` to equal drawn-triangle counts; "-" means the build cannot draw that few.

"Update" is `simplifyUpdate` with its defaults (positions moved, measured error); "unmeasured" turns the measured error off;
"fit only" turns position updates off. Loose validation is stage 1's (shading 90 degrees / 10%, fold 5%, coverage 5%).
Stage 1's validate-no-sloppy and loose-no-sloppy columns are repeated from above for comparison.

| | baseline | no-sloppy | update (sloppy) | update | update, unmeasured | fit only | update + validate | update + loose validate | stage 1 validate | stage 1 loose |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sloppy | on | off | on | off | off | off | off | off | off | off |
| flipped vs source (area) | 12.0% | 1.56% | 12.2% | 0.59% | 0.59% | 0.69% | 0.12% | 0.35% | 0.09% | 0.91% |
| severe vs source (area) | 20.8% | 5.18% | 18.4% | 1.19% | 1.19% | 1.25% | 0.20% | 0.61% | 0.46% | 3.56% |
| inverted (area) | 16.6% | 4.36% | 16.3% | 3.47% | 3.47% | 3.47% | 0.18% | 0.98% | 0.18% | 1.74% |
| coarsest triangles | 183k | 466k | 183k | 465k | 465k | 466k | 1.15M | 764k | 1.31M | 868k |
| groups | 4686 | 4065 | 4688 | 4074 | 4070 | 4068 | 3275 | 3755 | 3046 | 3496 |
| groups terminal by rejection | - | - | - | - | - | - | 380 | 170 | 491 | 260 |
| groups accepted after a retry | - | - | - | - | - | - | 339 | 231 | - | - |
| build wall time (s) | 3.1 | 2.7 | 16.0 | 15.2 | 9.3 | 15.2 | 36.7 | 41.7 | ~6.5 | - |
| cut Q_N/area at 1.5M | 0.0053 | 0.0008 | 0.0071 | 0.0011 | **0.0006** | 0.0012 | 0.0014 | 0.0026 | 0.0029 | 0.0030 |
| cut Q_N/area at 1M | 0.0140 | 0.0151 | 0.0197 | 0.0174 | **0.0123** | 0.0174 | - | 0.0127 | - | 0.0428 |
| cut Q_N/area at 750k | 0.0300 | 0.0320 | 0.0309 | 0.0213 | 0.0216 | 0.0212 | - | - | - | - |
| cut Q_N/area at 500k | 0.0484 | 0.1242 | 0.0638 | **0.0435** | 0.0442 | 0.0466 | - | - | - | - |
| cut > 90 degrees (area) at 1M | 0.19% | 0.24% | 0.28% | 0.26% | 0.18% | 0.26% | - | 0.09% | - | - |
| cut > 90 degrees (area) at 500k | 0.65% | 1.52% | 0.95% | 0.52% | 0.52% | 0.56% | - | - | - | - |
| cut missing area at 1M | 0.17% | 0.25% | 0 | 0 | 0.25% | 0 | - | 0 | - | 0.02% |
| cut missing area at 500k | 0.56% | 1.71% | 0 | 0 | 1.20% | 0 | - | - | - | - |

Counters (update, no sloppy): 2538 groups simplified with replacement vertices, 971k replacement vertices committed (439k
moved, the rest refitted in place), no locked vertex moved, 87 fit fallbacks, 0 non-finite UVs. Decoding every simplified
vertex of the update builds (3.9M) found no non-finite UV and none outside its mesh's leaf UV range; an earlier version that
bounded a moved vertex's UV by its neighbours' unclamped solutions left 0.03% outside, which is why the bound uses stored
values. Rebuilding the update variants gives byte-identical caches (3064 files each).

What this establishes:

- **Fitted normals are the gain.** Without sloppy, update+fit cuts stored flipped area from 1.56% to 0.59% and severe from
  5.2% to 1.2% at the same reduction (465k coarsest), and at runtime cuts it beats both baseline and no-sloppy at every
  triangle count once errors are not measured (0.0006 / 0.0123 / 0.0216 / 0.044 against no-sloppy's 0.0008 / 0.0151 / 0.032 /
  0.124 and baseline's 0.0053 / 0.0140 / 0.030 / 0.048 at 1.5M / 1M / 750k / 500k). At 500k it matches baseline (which uses
  sloppy) with none of sloppy's flipped surface. The facade's depth-3 level loses nearly all of no-sloppy's flipped (red)
  panels and window recesses.
- **Moving positions adds little.** Fit only is within noise of update everywhere except 500k (0.0466 against 0.0435); the
  reduction is the same. meshoptimizer's post-collapse solve cannot change which collapses happen.
- **Scoring the fit matters.** Always taking the hat-weighted average of the original normals (first version) was worse than
  no-sloppy at 1.5M (0.0011); choosing per vertex between the current normal, the average and the least-squares normal by
  their sampled shading residual gives 0.0006, with the other counts about equal.
- **Measured errors trade normal accuracy at a cut for honest coverage.** They raise about a third of the groups' errors,
  which removes missing surface at every cut (0.25% to 0 at 1M) but, at an equal triangle budget, pushes other groups coarser:
  Q_N/area rises 0.0006 to 0.0011 at 1.5M and 0.0123 to 0.0174 at 1M. Q_N does not count geometric error, so which is better
  depends on how the two are weighed; the measurement also costs about 6 s of the 15 s build.
- **Sloppy still poisons the curve.** With sloppy on, update+fit cannot fix sloppy output (12% flipped); do not combine them.
- **Validation is now limited by folds, not shading.** With stage 1 thresholds, update+fit lowers rejections and terminal
  groups (491 to 380) but reaches only 1.15M: 2831 of 2930 rejections are folds, which no normal fit repairs. With the loose
  thresholds it reaches 764k coarsest with 0.35% flipped, and at 1M draws Q_N/area 0.0127 with no missing surface and the
  lowest > 90 degree area of any build (0.09%), against stage 1 loose's 0.0428. Its floor (836k drawn) is set by folds.

Tests in [CLodEvalTests.cpp](../CLodEval/tests/CLodEvalTests.cpp): replacement vertices off leaves no flags and builds
identically twice; on, the leaves are unchanged and two builds are identical; validation outcomes are set on every
simplified group when on and nowhere when off; on a flat textured plane decoded replacement vertices stay on the plane with
the plane's normal and UVs equal to their position (also where a varying normal field makes the update move them); and on a
flat panel whose normals carry baked relief, update+fit with validation reduces at least 20% further than validation
alone, without flips.

## Stage 4: optimal placement during collapse ranking

Stage 4 of the plan: instead of ranking collapses at their endpoints and solving positions after the loop (meshoptimizer's
`meshopt_simplifyWithUpdate`), a separate collapse loop solves each candidate collapse's replacement position and wedge
attributes while ranking, costs the candidate at that position, and validates its neighbourhood at that position, including a
persistent orientation check against the source sheet. It is **off by default** (`simplifyOptimal`); with it off nothing runs,
and caches and hashes are unchanged (checked on Bistro, below).

### Where it lives

| Piece | File |
|---|---|
| Collapse loop (meshoptimizer-derived), source-sheet references, counters | [ClusterLODOptimalSimplify.h](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODOptimalSimplify.h) / [.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODOptimalSimplify.cpp) (`SimplifyOptimal`, `OptimalSimplifier`) |
| Policy: the candidate's place among the others | [clusterlod.h](../BasicRenderer/include/ThirdParty/meshoptimizer/clusterlod.h) (`clodConfig::simplify_optimal`, `clodOptimalRequest`, `OptimalGroupState`, `simplifyUpdate`, `simplifyValidated`, `simplifyWithReplacements`) |
| Original face lookup | `SimplifyValidator::OriginalFaceNormalAt` ([ClusterLODSimplifyValidation.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODSimplifyValidation.cpp)) |
| Wiring, settings, logging | `BuildClusterLODArtifactsFromGeometry` in [ClusterLODClustering.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODClustering.cpp) |

meshoptimizer itself is untouched: `ThirdParty/meshoptimizer1.3/src/simplifier.cpp` and every `meshopt_*` call behave as before.
The loop is a copy of `meshopt_simplifyEdge` in BasicRenderer's tree. The top of the `.cpp` lists every divergence from upstream,
each marked `stage 4:` where it happens.

### Representation and policy

It is an update candidate (stage 2) whose private buffers are simplified by the new loop instead of meshoptimizer. Everything
else is stage 2's: the private copy, locks, attribute stream (ranked attributes, then the UV streams solved with the positions),
normal fit from the original (`ReplacementVertexFitter::Fit`; raw interpolated normal attributes are never emitted),
replacement-vertex records, serial commit and ids. Turning it on turns replacement vertices on, under the same restrictions
(normals, no skinning, `preserveImportedNormals`). With validation, each target offers the optimal candidate first, then stage 2's
update candidate, then stage 1's candidates; stage 1's validator decides. Without validation the optimal candidate is used when it
reaches the target, or when it is stuck within `simplify_threshold` and sloppy is off; otherwise stage 2's path runs.

### The candidate solve and its cost

For each candidate edge (meshoptimizer's own selection, kinds and loops), the placements are:

- the endpoints, as upstream (both, for edges collapsible either way);
- the optimum on the edge segment, for a border or proper seam pair collapsing along its own loop (both wedges of a seam move
  together) and for manifold or complex targets. The merged position quadric with the incident wedges' attribute quadrics
  eliminated (Hoppe's new quadric metric, which upstream's `solvePositions` applies after the loop) is a parabola along the edge;
- for manifold or complex targets, that quadric's free optimum (with upstream's volume constraint and a small pull towards the
  edge midpoint), offered only within half an edge length of the edge.

Each placement is costed at its own position by what it adds to the target. That is the source patch's position quadric error
there (or the target patch's increase, if larger), plus each merged wedge's attribute quadric error, less the target wedge's
current error. The attribute values are the optimal ones when the target moves, or the target's kept values at an endpoint. At an
endpoint this is upstream's cost. The collapse is ranked by its best placement and keeps the second best (an endpoint when the
best moves).

### Validation at the solved position

In the pass, a placement is checked against the current geometry, including the pass's earlier collapses:

- **Per collapse**: no surviving face around either endpoint (the target moves too) may rotate past `simplifyOptimalFlipAngle`
  (75.5 degrees, upstream's limit) or degenerate.
- **Persistent, against the source sheet**: each triangle carries a reference, the area-weighted face orientation of the original
  surface under it. The builder measures it once per group for the input triangles: four samples per triangle, matched side-aware
  with `MatchFacing`. When a collapse removes faces, their references are merged into the surviving faces that share their
  surviving corner. A face is therefore compared with the patch it now represents, not with every detail that disappeared. A
  surviving face must meet its merged reference within `simplifyOptimalSheetAngle` (60 degrees), unless it was already beyond
  that and the collapse does not make it worse (inherited, not new).
- **Vanishing components**: a collapse that leaves no surviving face around it deletes the rest of a component. Only an endpoint
  placement may do that, since a moved position's quadric error (at an edge midpoint, say) understates the surface that
  disappears. Without this rule, ivy leaf cards vanished at half stage 2's error, and missing area at 1.5M-triangle cuts rose
  twentyfold.
- Locked vertices (group boundaries) never move.

If the best placement fails, the second is tried when it is within the pass's error goal. An accepted collapse writes its position
into every wedge of the target at once. After the pass, the moved wedges' attributes are solved from their merged attribute
quadrics (upstream's `solveAttributes` rule). Geometry, attributes and the source-sheet correspondence (the references) are
therefore committed together.

### Settings

| Field / JSON key | Environment | Default | Meaning |
|---|---|---|---|
| `simplifyOptimal` | `BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL` | off | Turns optimal placement on (and replacement vertices with it) |
| `simplifyOptimalSheetAngle` | `BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_SHEET_ANGLE` | 60 | Degrees a face may turn from its source-sheet reference |
| `simplifyOptimalFlipAngle` | `BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_FLIP_ANGLE` | 75.5 | Degrees a face may rotate in one collapse |
| `simplifyOptimalMoves` | `BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_MOVES` | 7 | Placements besides the endpoints, bits: 1 border/seam along the edge, 2 interior along the edge, 4 interior free solve |
| `simplifyOptimalPreserveFolds` | `BASICRENDERER_CLOD_SIMPLIFY_OPTIMAL_PRESERVE_FOLDS` | off | meshoptimizer's fold quadrics in the loop (comparison only) |

Environment overrides are hashed into the cache key only when set. Each build logs a `ClusterLOD simplify optimal: ...` line:
candidates built, offered and accepted; collapses, how many moved freely or along their edge, and second-placement fallbacks;
placements rejected per check (flip, sheet, vanish); free solves discarded; reference samples and unmatched ones; and CPU time for
references and for the loop.

### Results on Bistro

Suite [config/clod_eval/stage4.json](../config/clod_eval/stage4.json), measured 2026-11-16, Bistro without textures, all without
measured errors. Cut rows interpolate `appearance.cut.*` to equal drawn-triangle counts; "-" means the build cannot draw that few.
"Loose" is stage 1's loose validation (shading 90 degrees / 10%, fold 5%, coverage 5%). "Interior" is `simplifyOptimalMoves=6` (no
border/seam moves). Stage 2's measured-error loose row (764k coarsest) is not comparable: its errors are measured.

| | baseline | no-sloppy | update | optimal | optimal, interior | update + loose | optimal + loose | optimal + loose, interior | optimal + validate | optimal, sloppy |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sloppy | on | off | off | off | off | off | off | off | off | on |
| flipped vs source (area) | 12.0% | 1.56% | 0.59% | 0.53% | 0.61% | 0.35% | 0.35% | 0.35% | 0.10% | 11.0% |
| severe vs source (area) | 20.8% | 5.18% | 1.19% | 1.09% | 1.16% | 0.60% | 0.72% | 0.59% | 0.16% | 17.6% |
| inverted (area) | 16.6% | 4.36% | 3.47% | 2.53% | 3.31% | 0.98% | 1.25% | 1.07% | 0.12% | 16.0% |
| coarsest triangles | 183k | 466k | 465k | 468k | 471k | 845k | 743k | 827k | 1.19M | 185k |
| fewest triangles drawn | 183k | 466k | 465k | 472k | 476k | 917k | 820k | 904k | 1.19M | - |
| groups | 4686 | 4065 | 4070 | 4215 | 4170 | 3534 | 3881 | 3581 | 3197 | 4696 |
| groups terminal by rejection | - | - | - | - | - | 230 | 160 | 213 | 391 | - |
| fold rejections | - | - | - | - | - | 1525 | 1950 | 1942 | 3770 | - |
| build wall time (s) | 2.5 | 2.5 | 9.1 | 14.6 | 13.8 | 22.7 | 39.9 | 37.3 | 32.2 | 16.3 |
| cut Q_N/area at 1.5M | 0.0053 | 0.0008 | **0.0006** | 0.0008 | 0.0006 | 0.0024 | 0.0016 | 0.0025 | 0.0022 | 0.0048 |
| cut Q_N/area at 1M | 0.0140 | 0.0151 | 0.0123 | 0.0252 | 0.0158 | 0.0147 | **0.0110** | 0.0153 | - | 0.0170 |
| cut Q_N/area at 750k | 0.0300 | 0.0320 | 0.0216 | **0.0142** | 0.0215 | - | - | - | - | 0.0328 |
| cut Q_N/area at 500k | 0.0484 | 0.1242 | 0.0442 | 0.0581 | 0.0644 | - | - | - | - | **0.0353** |
| cut > 90 degrees (area) at 1M | 0.19% | 0.24% | 0.18% | 0.36% | 0.23% | 0.10% | **0.08%** | 0.10% | - | 0.22% |
| cut > 90 degrees (area) at 750k | 0.45% | 0.43% | 0.28% | **0.12%** | 0.25% | - | - | - | - | 0.44% |
| cut > 90 degrees (area) at 500k | 0.65% | 1.52% | **0.52%** | 0.59% | 0.70% | - | - | - | - | 0.41% |
| cut missing area at 1M | 0.17% | 0.25% | 0.25% | 0.10% | 0.28% | 0.03% | 0.04% | 0.02% | - | 0.13% |
| cut missing area at 500k | 0.56% | 1.71% | 1.20% | 2.37% | 2.16% | - | - | - | - | 0.37% |

Counters (optimal, no sloppy): 2463 groups simplified by optimal candidates, 715 candidates stuck above `simplify_threshold` (stage 2
then ran); 1.50M collapses, of which 183k moved freely and 682k along their edge; 996k placements rejected by the per-collapse
test, 987k by the source-sheet test, 6.6M moved placements refused because they would delete a component (mostly ivy and foliage
leaf cards, each then taking an endpoint), 254k free solves too far from their edge. CPU: references 90 s, loop 9 s, stage 2's
fit 203 s (more vertices move: 704k against 439k). Rebuilding optimal and optimal + loose gives byte-identical caches (3064 files
each). With it off, baseline and no-sloppy have the same `hash=` values as stage 2's (0x83D874F403D6A3EE, 0xB5917D62BF19ACEE) and
byte-identical caches (the scene file differs only in its creation time).

What this establishes:

- **With validation, it is the better candidate.** At loose thresholds, optimal placement reaches 743k coarsest (820k drawn)
  against stage 2's 845k (917k), with fewer terminal groups (160 against 230). At equal drawn triangles it is better at every
  cut either can draw: Q_N/area 0.0016 against 0.0024 at 1.5M, and 0.0110 against 0.0147 at 1M. At 1M it also beats unvalidated
  stage 2 (0.0123), with less than half its area beyond 90 degrees (0.08% against 0.18%) and a seventh of its missing area. The
  gain comes from border and seam moves: with interior moves only, it is no better than stage 2.
- **It does not unlock the stuck meshes.** Folds still dominate validation's rejections (1950 of 2412), and without sloppy the
  coarsest level stays at about 466k with or without it. The ~466k floor is topology (groups whose locked boundaries and seams leave
  no legal collapse), not fold rejection: this stage changes where a collapse puts its vertex, not which collapses exist. That is
  stage 5's problem (pair collapses, regrouping, proxies).
- **Without validation it trades one cut against another.** It is best of all at 750k (0.0142 against 0.0216, and 0.12% beyond 90
  degrees against 0.28%), but worse at 1M (0.0252 against 0.0123) and 500k (0.058 against 0.044). Border and seam moves cause both
  (interior moves alone track stage 2). They lower the error of collapses along open borders and seams, so foliage leaf cards (the
  cypresses, whose Q_N/area is above 0.5 in every build) and facade seams reach their coarser levels at smaller errors. That moves
  where each mesh switches level, and at 1M the cypresses draw their finer, worse-shaded level. The facade's depth-1 level shades
  worse in this build (0.009 against 0.0024 Q_N/area, same triangles) for a reason not yet isolated.
- **Missing surface at cuts follows the errors.** The vanishing rule keeps it at stage 2's level or below down to 750k; at 500k
  it is higher (2.4% against 1.2%).
- **`meshopt_SimplifyPreserveFolds` changes nothing measurable** (same curve within noise), as the plan expected.
- **With sloppy on**, it gives the best 500k and 300k cuts of any build (0.035 and 0.160 against baseline's 0.048 and 0.190), but
  keeps sloppy's 11% flipped area; sloppy is still the wrong fallback.

Tests in [CLodEvalTests.cpp](../CLodEval/tests/CLodEvalTests.cpp): off, the optimal settings change nothing (a build with them
equals stage 2's); on, plain and validated builds are each identical twice, simplify with replacement vertices and differ from
stage 2. On two rough synthetic panels (random spikes, and deep one-cell slots, smooth normals), stage 2 leaves inverted area
(0.20% and 0.08%) and optimal placement much less (0.03% and 0). Neither folds against the source under either path: the
synthetic cases tried (smooth recesses, hard-edged window recesses, fins, studs, noise) did not reproduce Bistro's folds, which need
thin walls inside large groups.

For stage 3: optimal placement reports quadric errors at the solved position, which are smaller than stage 2's for the same
collapse wherever a vertex moves along a border or seam. Traversal compares them with the same thresholds, so the level switch
moves to coarser cuts, and that shifting is what the cut curve shows (better at 750k, worse at 1M). Separate measured geometric and
appearance channels would make the switch independent of where the vertex was put.

## Stage 3: error channels and appearance-aware selection

Stage 3 of the plan: every group's simplified output is measured against the **original** surface after the build, as channels
kept apart from meshoptimizer's quadric error, and the runtime can select levels with them, so a level that shades badly is not
drawn as soon as its geometric error allows. Both halves are **off by default**: the builder measures only with
`lodErrorChannels`, and the runtime selects by the quadric error alone unless `clodLodGeometricChannel` or
`clodLodAppearanceWeight` say otherwise. With the build setting off, caches and hashes are unchanged (checked on Bistro, below).

### Where it lives

| Piece | File |
|---|---|
| Measurement, ownership, monotone envelope | [ClusterLODErrorChannels.h](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODErrorChannels.h) / [.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODErrorChannels.cpp) (`MeasureClusterLODErrorChannels`) |
| Wiring, settings, logging; dropped for voxel and forked meshes | `BuildClusterLODArtifactsFromGeometry` in [ClusterLODClustering.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODClustering.cpp) |
| Storage in the traversal nodes | `BuildClusterLODTraversalHierarchy` in [ClusterLODHierarchy.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.cpp); `ClusterLODTraversalMetric` in [ClusterLODTypes.h](../BasicRenderer/include/BasicRenderer/Assets/ClusterLODTypes.h) |
| Upload into the GPU group and node copies, fallback | `MeshManager::AddMesh` in [MeshManager.cpp](../BasicRenderer/src/VirtualGeometry/GeometryStorage/MeshManager.cpp) |
| Selection rule and every test that uses it | `CLodSelectionError`, `CLodGroupSelectionError`, `CLodNodeSelectionError` in [workGraphCulling.hlsl](../BasicRenderer/shaders/ClusterLOD/workGraphCulling.hlsl); node tests in [computeCulling.hlsl](../BasicRenderer/shaders/ClusterLOD/computeCulling.hlsl) |
| View constants | `CullingCameraInfo::lodAppearanceWeight`, `lodGeometricChannel` ([ShaderBuffers.h](../BasicRenderer/include/BasicRenderer/Extensions/ShaderBuffers.h), [structs.hlsli](../BasicRenderer/shaders/Include/structs.hlsli)), filled in `BuildCullingCameraInfo` ([ViewManager.cpp](../BasicRenderer/src/Scene/Views/ViewManager.cpp)) |
| CPU mirror for the cut sweep | [ErrorChannels.h](../CLodEval/src/Core/ErrorChannels.h) / [.cpp](../CLodEval/src/Core/ErrorChannels.cpp); `quality.appearance`'s `cut_rules` ([clod-evaluation.md](clod-evaluation.md#appearance-at-runtime-cuts)) |

### Channels

A group's error describes **S(g)**, the representation that replaces its clusters at a cut: every cluster whose refined group is
g, in whichever group it was placed (`enter <= T < leave` uses g's error as its clusters' `leave` and as S(g)'s `enter`). The
channels are measured from the group streams the builder already holds (positions as stored; normals octahedral-encoded and
decoded as the pages store them), after every group is built, so they do not depend on which candidate or vertex placement
produced S(g):

- **Geometric** (mesh units): the larger of two sampled maxima. Outward: the farthest S(g) sample from the original surface (four
  area points and the three corners per triangle, `SimplifyTriangleBvh::NearestDistanceSquared` on the original). Inward: the
  farthest original triangle **owned** by g from S(g) (its centroid). Ownership follows each original triangle up the DAG: it starts
  with the leaf group holding it, and at each group passes to the group holding the nearest S(g) triangle, which is what replaces it
  there. So the inward term is measured directly against the original at every level, never through a coarser level, and it sees
  removed surface (cut corners, vanished components) that outward distance cannot. Sampled maxima, not certified bounds.
- **Appearance** (mesh units): `sqrt(Q_N)` with `Q_N = sum over S(g)'s area samples of (area / 4) * |n_coarse - n_source|^2`: the
  plan's integral, over S(g). `n_coarse` is the decoded corner normals interpolated and normalised; `n_source` the original's
  interpolated normal at the side-aware match (`SimplifyValidator::OriginalNormalAt`, so a coincident opposite sheet is not the
  reference). A coarse normal that cancels counts as chord² 4; a source normal that cancels is skipped and counted
  (`samples_without_normal`).

**Monotone envelope.** In child-to-parent order (by depth) each channel becomes `max(own, children's)`, the children being the
groups g's own clusters refine into. Every segment's `enter <= leave` then holds for each channel, so it holds for any combination
of them the runtime takes (below). The quadric error is monotone already (meshoptimizer's merge), so `max` with it is too. A
terminal group (nothing replaces it) gets geometric `FLT_MAX`, as its quadric error is, and appearance 0. Raw per-group values are
not stored; `BASICRENDERER_CLOD_ERROR_CHANNELS_LOG=1` (diagnostics, not hashed) logs each group's own outward, inward and Q_N
measurements beside the envelope.

Meshes that end up with voxel groups or a forked DAG keep no channels: their boundaries are set by voxel payloads, which these
measurements do not describe. Merge hierarchies and assemblies build their own nodes and have none either.

### Storage: no format change

The channels live in the traversal nodes' three padding words, which every cache already stores as zeros
(`ClusterLODTraversalMetric::maxMeasuredGeometricError`, `maxAppearanceError`, `channelFlags`): a segment leaf holds its owning
group's (the geometric one through `TraversalNodeErrorFromGroupError`, as `maxQuadricError`), an internal node the largest of its
subtree's, a structural node at least the structural error, and `CLOD_NODE_CHANNELS_PRESENT` is set on all of them. Nothing else in
the cache changes: groups, segments and pages of a channel build are byte for byte those of the same build without channels
(a test compares them), and a cache built without channels has zeros there, as before. **The cache schema is not bumped**: old
caches are not misread (zero flags read as "no channels"), and builds with channels get their own build key from the hashed setting.

On upload, the GPU copy of each group takes its channels where the CPU copy keeps `groupVertexCount` and `terminalSegmentCount`
(no shader reads either; the same trick puts `partIndex` in `firstGroupVertex`): `clodStructs.hlsli` names them
`measuredGeometricError` and `appearanceError`. A mesh without channels, or whose boundaries come from voxels, forks, merges or
assemblies, gets its stored error and 0 there and in every node, so every rule selects exactly as the quadric error does for it.
A forked mesh's voxel view takes them from the shadow record with its bounds (`CLodViewGroup`).

### Runtime selection

```hlsl
float CLodSelectionError(float quadricError, float measuredError, float appearanceError, CullingCameraInfo lodCam)
{
    float error = quadricError;                                          // clodLodGeometricChannel 0
    if (lodCam.lodGeometricChannel == 1) error = measuredError;           // 1: measured
    else if (lodCam.lodGeometricChannel == 2) error = max(quadricError, measuredError); // 2: max
    if (lodCam.lodAppearanceWeight > 0.0f) error = max(error, lodCam.lodAppearanceWeight * appearanceError);
    return error;
}
```

Every test that compared an error with the threshold now compares this, with `ProjectedGeometricError` unchanged: the leaf's live
group boundary (`CLodPrepareRenderableLeaf`), the refined child's (`CLodRefinedChildSuppressesParent`, from traversal and from the
per-cluster pass), node and child prefilters in pure compute and in the work graph (`CLodNodeSelectionError`: a node's subtree
maximum of the combination is the combination of its subtree maxima, since `max` and a non-negative weight commute), instance-root
proxies, and the streaming request priority. With channel 0 and weight 0 it returns the quadric error itself, bit for bit, so
the default renderer selects exactly what it did.

**Why the appearance channel projects like a distance.** `sqrt(Q_N)` is a length, and `ProjectedGeometricError` multiplies it by p,
the pixels per unit at the group's depth (instance scale and the orthographic and ray-tracing variants included): `p * sqrt(Q_N) =
sqrt(p^2 Q_N)`, the root of the screen-integrated squared shading error the plan proposes (unit Lambertian light, matched albedo,
unchanged visibility: `|dI| <= |n_coarse - n_source|`). Refining when `w * p * sqrt(Q_N) > tau` (tau the pixel threshold) refines
when the group's integrated squared shading error exceeds `(tau / w)^2` pixels: the weight sets a budget in squared-error pixels
(w = 0.1 at 1 pixel: 100 pixels' worth of full-intensity error per group). Angular error is never projected as a distance; it
enters through area, which shrinks with distance as p² does. The budget is per group, so it depends on how groups partition the
surface (the plan's caveat); groups hold similar triangle counts, which keeps it in check, but it is not a whole-image budget.

Shadow views take weight 0 (`BuildCullingCameraInfo` with the view's shadow flag, and the virtual shadow culling compiles the
appearance term out): shading normals never reach a shadow map. The geometric channel applies to every view. Every other view gets
the same settings, so inside the frustum the ray tracing view and unified culling still select what raster selects. The coarse-cut
shortcut (`CLodRtTakeCoarseCutWithoutTraversal`) works from per-mesh quadric bounds, so a view selecting with channels traverses
those instances instead.

### Settings

| Setting | Kind | Default | Meaning |
|---|---|---|---|
| `lodErrorChannels` / `BASICRENDERER_CLOD_ERROR_CHANNELS` | build (hashed when set) | off | Measure and store the channels |
| `BASICRENDERER_CLOD_ERROR_CHANNELS_LOG` | build diagnostics (not hashed) | off | One log line per group |
| `clodLodGeometricChannel` (`SARP_CLOD_LOD_GEOMETRIC_CHANNEL`) | runtime setting | 0 | 0 quadric error, 1 measured deviation, 2 the larger |
| `clodLodAppearanceWeight` (`SARP_CLOD_LOD_APPEARANCE_WEIGHT`) | runtime setting | 0 | Weight of the appearance channel (0: unused) |

The environment variables give the runtime settings' starting values for headless runs. Each build logs `ClusterLOD error channels:
...`: groups measured, simplified triangles and samples, samples without a source normal, original triangles followed, groups whose
geometric channel exceeds their quadric error, groups the envelope raised, and the measurement's time.

### Results on Bistro

Suite [config/clod_eval/stage3.json](../config/clod_eval/stage3.json), measured 2026-11-17, Bistro without textures. Every
variant with channels is the stage 2 or stage 4 build of the same name plus `BASICRENDERER_CLOD_ERROR_CHANNELS=1` ("update" is stage
2 without measured errors; "optimal + loose" is stage 4's loose validation). Rules are `quality.appearance` cut rules (`q` quadric,
`m` measured, `max` the larger; `w` the appearance weight). Values interpolate `appearance.cut.*` to equal drawn triangles; "-"
means the build cannot draw that few. Each build's untagged sweep (and its `q0` rule) is exactly the stage 1/2/4 row for it.

Q_N/area (area-weighted squared normal chord against the leaves, lower is better):

| build : rule | fewest drawn | 1.5M | 1M | 750k | 500k |
|---|---:|---:|---:|---:|---:|
| baseline : stored | 183k | 0.0053 | 0.0140 | 0.0299 | 0.0484 |
| baseline : q w0.1 | 183k | 0.0019 | 0.0078 | 0.0178 | 0.0555 |
| no-sloppy : stored | 466k | 0.0008 | 0.0151 | 0.0320 | 0.1242 |
| no-sloppy : q w0.1 | 466k | 0.0008 | 0.0064 | 0.0312 | 0.1242 |
| update : stored | 465k | 0.0006 | 0.0123 | 0.0216 | 0.0442 |
| update : q w0.03 | 465k | 0.0006 | 0.0088 | 0.0194 | 0.0442 |
| update : q w0.1 | 465k | 0.0006 | **0.0036** | 0.0178 | 0.0442 |
| update : q w0.3 | 465k | 0.0007 | **0.0035** | **0.0133** | 0.0442 |
| update : q w1 | 465k | 0.0010 | 0.0043 | 0.0158 | 0.0442 |
| update : measured | 465k | 0.0116 | 0.0216 | 0.0216 | 0.0629 |
| update : max | 465k | 0.0011 | 0.0176 | 0.0215 | 0.0431 |
| update : max w0.1 | 465k | 0.0009 | 0.0053 | 0.0221 | 0.0431 |
| update : m w0.3 | 465k | 0.0011 | 0.0053 | 0.0194 | 0.0628 |
| optimal : stored | 472k | 0.0008 | 0.0252 | 0.0142 | 0.0581 |
| optimal : q w0.03 | 472k | 0.0008 | 0.0082 | **0.0131** | 0.0581 |
| optimal : q w0.1 | 472k | 0.0008 | 0.0055 | 0.0193 | 0.0581 |
| optimal : measured | 468k | 0.0085 | 0.0164 | 0.0141 | 0.0622 |
| optimal : max w0.1 | 472k | 0.0009 | 0.0061 | 0.0189 | 0.0559 |
| optimal + loose : stored | 820k | 0.0016 | 0.0110 | - | - |
| optimal + loose : q w0.1 | 820k | 0.0008 | 0.0101 | - | - |
| optimal + loose : q w0.3 | 820k | 0.0010 | 0.0102 | - | - |
| optimal + loose : measured | 815k | 0.0045 | 0.0146 | - | - |
| optimal + loose : max w0.1 | 820k | 0.0009 | 0.0112 | - | - |

Area beyond 90 degrees and missing area (leaf area farther than the cut from anything drawn) at 1M and 750k:

| build : rule | > 90° @1M | > 90° @750k | missing @1M | missing @750k |
|---|---:|---:|---:|---:|
| update : stored | 0.18% | 0.28% | 0.25% | 0.57% |
| update : q w0.1 | 0.023% | 0.20% | 0.39% | 0.69% |
| update : q w0.3 | 0.023% | 0.11% | 0.47% | 0.62% |
| update : max | 0.27% | 0.25% | 0 | 0 |
| update : max w0.1 | 0.035% | 0.26% | 0 | 0 |
| optimal : stored | 0.36% | 0.12% | 0.10% | 0.49% |
| optimal : q w0.03 | 0.082% | 0.097% | 0.14% | 0.49% |
| optimal : max w0.1 | 0.033% | 0.16% | 0 | 0 |
| optimal + loose : stored | 0.079% | - | 0.036% | - |
| optimal + loose : q w0.1 | 0.063% | - | 0.030% | - |

**Off is unchanged.** With the build setting off, baseline and no-sloppy have stage 4's `hash=` values (0x83D874F403D6A3EE,
0xB5917D62BF19ACEE) and byte-identical caches (3065 files; the scene file differs only in its creation time). With it on, groups,
segments and pages are unchanged and only the nodes' spare words differ: per-mesh metrics of baseline and baseline + channels are
identical (193k records; rollups agree to 1e-12, summed in manifest order), and every rule on a build without channels selects
exactly the stored errors (545k records equal). On every channel build `q0` equals the untagged sweep in every cut metric (50-55k
records each), and no segment's channels decrease (`appearance.channels.monotonic_violations` 0).

What this establishes:

- **The appearance channel is the gain.** At equal drawn triangles it lowers Q_N/area wherever the build has levels to trade: on
  update, 0.0123 to 0.0036 at 1M (w 0.1) and 0.0216 to 0.0133 at 750k (w 0.3), with area beyond 90 degrees down eightfold at 1M;
  on optimal it removes stage 4's 1M regression (0.0252 to 0.0082 at w 0.03) while keeping, and improving, its 750k result (0.0131).
  It helps every build, sloppy baseline included (0.0140 to 0.0078 at 1M). Weights from 0.03 to 0.3 all beat none at 1M and 750k;
  the best weight depends on the cut and the build, and past 0.3 it gets worse again.
- **It costs missing area, which it does not measure.** At the same triangle count the shading-dominated groups refine and others
  go coarser: missing area at 1M rises from 0.25% to 0.39% (update, w 0.1).
- **A separate measured geometric channel does not avoid stage 2's regression.** `max` (measured or quadric, whichever is larger)
  reproduces stage 2's measured-error curve almost exactly (0.0011 / 0.0176 against stage 2's 0.0011 / 0.0174 at 1.5M / 1M) with no
  missing area. The regression was never the build's coupling (merged errors, validation radius); it is the trade itself: honouring
  geometric deviation spends triangles where shading did not need them.
- **Measured geometric alone is worse.** Its sampled maxima exceed the quadric error for a quarter to a third of groups, mostly at
  fine levels, so fine cuts go coarse elsewhere: 0.0116 against 0.0006 at 1.5M on update. On optimal it partly decouples selection
  from vertex placement, as stage 4 predicted (1M 0.0164 against 0.0252), but less well than the appearance channel.
- **Geometry and shading together.** `max` with weight 0.1 keeps zero missing area at every cut and still beats the stored errors at
  1M (update 0.0053 against 0.0123; optimal 0.0061 against 0.0252), at a small cost at 1.5M. It is the rule to take where missing
  surface matters as much as shading.
- **No rule moves the floor.** At 500k every no-sloppy build draws nearly its coarsest level whatever the rule (466k-472k); selection
  only chooses among levels that exist. That is stage 5's problem.
- **Validated builds gain least.** Optimal + loose already rejects most mis-shaded candidates; the channel still halves Q_N at 1.5M
  (0.0016 to 0.0008) and trims 1M (0.0110 to 0.0101).

Cost: the measurement adds about 5 s of wall time to a Bistro build (no-sloppy 2.6 to 7.5 s, update 9.1 to 14.4 s, optimal 14.6 to
20.1 s), 90-150 s of CPU summed over meshes (`measure_ms`). Cache bytes do not change. At runtime each selection test reads two more
floats from the group or node record it already loads and does two compares and a multiply under a uniform branch; no buffer grows.
Each cut rule adds about 10-15 s to a Bistro `quality.appearance` run.

Tests in [CLodEvalTests.cpp](../CLodEval/tests/CLodEvalTests.cpp): off stores nothing, and on leaves groups and pages identical with
only the nodes' spare words set; the quadric rule with weight 0 reproduces every untagged cut metric; channels never decrease from a
group to the groups it refines, internal nodes hold their subtree's largest and leaves their group's; and on a flat plane with baked
relief in its normals (simplified levels stay on the plane), the appearance channel dominates the geometric one where a plane with
flat normals has none, no group's selection error falls when the weight is added and some rise, every cut draws at least as many
triangles, and at some cut it draws more that shade better.

Limits: the appearance channel is one number per group (no peak or tail, no spatial scales, no glossy response); its budget is per
group, not per image; and like every measurement here it is sampled, not certified. Meshes with voxel groups, forks, merge proxies
or assemblies keep no channels and select as before. Validated only offline (cut sweeps and compiled shaders); not yet in a running
renderer.

## Stage 5: controlled topology changes

Stage 5 of the plan: groups that would stay terminal (the coarse-level triangle floor of builds without the sloppy fallback) try
candidates that change topology the ordinary simplifier refuses, validated against the original like every other candidate. It is
**off by default** (`simplifyTopology`); with it off nothing runs, and caches and hashes are unchanged (checked on Bistro, below).
A diagnostic (`BASICRENDERER_CLOD_TOPOLOGY_DIAG=1`, not hashed, changes nothing built) classifies why terminal groups stop.

### Why groups stop: the floor on Bistro

Without sloppy, Bistro's coarsest cut holds 466k triangles. The diagnostic re-runs each group that ends stuck above
`simplify_threshold` with one restriction relaxed at a time and records the first relaxation that lets meshoptimizer reach the
threshold, weighted by the group's triangles (what the coarsest cut draws). Stage 2 update, no sloppy (no-sloppy alone is the same
within 0.1%):

| Terminal triangles | Groups | Triangles | Share |
|---|---:|---:|---:|
| Roots (the final single cluster of a mesh: not stuck) | 1035 | 60k | 13.0% |
| Stuck: group boundary locks | 0 | 0 | 0 |
| Stuck: protected seams that are not UV seams (normal hard edges, and the tangent differences they cause) | 107 | 133k | 28.6% |
| Stuck: protected seams including UV chart seams | 392 | 272k | 58.4% |
| Stuck: unprotected seams, components, other meshoptimizer limits | 0 | 0 | 0 |

- **Group boundaries do not matter.** Groups are large (partitions of 384 clusters, capped at 8 refined groups), so most Bistro
  meshes are one group per level; only 16 positions of all stuck groups are boundary-locked. Regrouping has nothing to unlock.
- **Disconnected components are not the first limit either**, although stuck groups hold 16k components and 98k triangles in
  components of 16 triangles or fewer (leaf cards): with every seam relaxed, positions alone always reach the threshold without
  `meshopt_SimplifyPrune`.
- **Protect bits are the floor.** BasicRenderer protects every normal and tangent discontinuity (`attribute_protect_mask`), and in
  permissive mode a protected seam vertex cannot collapse across its seam. Dropping protect bits only where normals differ never
  suffices, because the tangent (orthogonalised against the normal) differs at every hard edge too; so the classes split by whether
  the seam is also a **UV seam** (the vertex's UVs differ from its position's canonical vertex).
- **UV seams are real chart cuts.** Of the 313k UV-seam vertices in stuck groups, 0.7% differ by less than 1/1024 and 0.3% by a
  whole number (a wrap seam a repeating texture would hide); the rest are genuine chart boundaries, which no topology change can
  cross without moving texture.

So the safe technique is seam relaxation where stage 2's fit can rebuild the signal (normals), keeping UV seams; crossing UV seams
needs a measured texture criterion. Pair collapses, regrouping and pruning were not built: the diagnosis shows they would not move
this floor (see "Not built" below).

### Where it lives

| Piece | File |
|---|---|
| Policy: when a group tries topology candidates, which seams relax, counters, diagnostics | [clusterlod.h](../BasicRenderer/include/ThirdParty/meshoptimizer/clusterlod.h) (`simplifyTopology`, `relaxableSeam`, `diagnoseStuckGroup`, `clodConfig::simplify_topology*`, `clodConfig::topology_*`, `clodTopologyStats`) |
| Texture check | [ClusterLODTopology.h](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODTopology.h) / [.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODTopology.cpp) (`MeasureTextureDisplacement`) |
| Texcoord export and transfer for topology candidates | `ReplacementVertexFitter::Fit`, `OriginalTexcoordAt` ([ClusterLODReplacementVertices.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODReplacementVertices.cpp)) |
| Wiring, settings, logging | `BuildClusterLODArtifactsFromGeometry` in [ClusterLODClustering.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODClustering.cpp) |

meshoptimizer is untouched (`ThirdParty/meshoptimizer1.3`): topology candidates only change the lock flags they pass it. Every
addition to clusterlod.h is marked `BR (stage 5)` and gated by `clodConfig::simplify_topology` (candidates) or
`clodConfig::topology_stats` (counters, diagnostics), both null by default.

### Candidates and policy

A group about to be terminal (stuck above `simplify_threshold` on every earlier candidate, or every candidate rejected by
validation) tries, for each target from the configured one towards the threshold (`simplifyTopologyRetries` less aggressive ones):

1. An update candidate (stage 2: private buffers, `meshopt_simplifyWithUpdate`; stage 4's loop when `simplifyOptimal` is on) whose
   relaxable seam vertices have lost their protect bit in the private copy, so meshoptimizer's permissive mode treats them as complex
   vertices that may collapse across the seam. Nothing shared changes: the group's other candidates and every other group keep their
   locks.
2. Its shading is refitted from the **original** by stage 2's fit (per wedge, so the hard edges that survive stay hard; collapsed ones
   get the original's normals at their new triangles), and its texcoords are transferred from the original (below).
3. It is validated: by stage 1's validator with the `simplifyValidation*` thresholds, also when `simplifyValidation` is off (then only
   topology candidates are validated, and the level surface is indexed for the source-coverage check), plus the texture check.

The first accepted candidate is used, with its own error merged as usual; when none is accepted the group stays terminal (finer
geometry kept). A candidate still stuck above the threshold ends the group's attempts (less aggressive targets give the same).

**Relaxable seams.** A protected seam vertex is relaxable unless it is a UV seam: its UV streams (stage 2 carries them as update
attributes) differ from its position's canonical vertex by more than 1/1024 (relative above 1; separately solved wedges of one moved
position differ by rounding, and an exact comparison made them look like seams and stalled the next level). Without UV streams, a
vertex whose protected attributes differ only in the normal is relaxable. `simplifyTopologyRelaxUv` relaxes UV seams too.

**Texcoords.** meshoptimizer's attribute solve drifts on curved surfaces (on a faceted dome with UV = xy, a third of a cell after one
level and 1.5 cells after two), and topology candidates move many more vertices than stage 2's, level after level. A topology
candidate's moved vertex therefore takes the original's texcoord at its side-aware match, when that lies inside its stage 2 UV box
(its own and its neighbours' stored UVs) grown by a quarter, which keeps it in its own chart; otherwise the clamped solve stays.
Stage 2's own candidates are unchanged. (Only the record texcoord: on meshes with UV sets, every glTF asset, the pages store the sets
and this transfer does not reach them; see [Follow-ups](#follow-ups-to-stages-2-4-and-5).)

**Texture check** (`MeasureTextureDisplacement`). Crossing a UV seam moves texture without moving geometry or shading, which neither
stage 1's checks nor Q_N see. Each candidate triangle is sampled at stage 1's four points; each point is matched side-aware to the
original (within the radius), and the texture slide there is the difference between the candidate's interpolated texcoord and the
original's at the match, divided by the matched triangle's texcoord scale (the square root of its UV area over its area): how far,
along the surface, the texture moved. A candidate is rejected (`clodValidateReject_Texture`) when the area sliding farther than
`simplifyTopologyTextureSlide` radii exceeds `simplifyTopologyTextureArea`. Matching alone moves a point along the surface by up to
about its distance (more over creases), and stage 2's solve drifts, so the tolerance is several radii: on the faceted dome 8 radii
clears legitimate candidates (0.4% of area beyond it) where 4 rejects them (10%). It catches crossings whose slide exceeds 8 radii;
at coarse levels the radius can exceed the charts' separation and crossings pass. It applies to every topology candidate of a mesh
with texcoords, relaxed UV seams or not.

**Recorded as.** Groups simplified by a topology candidate record `clodSimplifyMethod_Topology`, the permissive method's value
(BasicRenderer runs permissive regular simplification without the permissive fallback, so the value is otherwise unused there), so
`quality.normals`' per-method rows attribute their triangles; their validation outcome bits are set as validated (accepted, or
accepted after a rejection), and they carry `CLOD_GROUP_FLAG_SIMPLIFY_REPLACED` like any update candidate.

Restrictions are stage 2's: meshes without normals, with skinning, or without `preserveImportedNormals` are built without it.

### Settings

| Field / JSON key | Environment | Default | Meaning |
|---|---|---|---|
| `simplifyTopology` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY` | off | Turns topology candidates on (and replacement vertices with them) |
| `simplifyTopologyRetries` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_RETRIES` | 2 | Less aggressive targets after the configured one (capped at 16) |
| `simplifyTopologyRelaxUv` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_RELAX_UV` | off | Relax UV seams too, where the texture check allows it |
| `simplifyTopologyTextureArea` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_AREA` | 0.02 | Area fraction allowed to slide past the tolerance; 1 or more turns the check off |
| `simplifyTopologyTextureSlide` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_SLIDE` | 8 | Slide tolerance, in validation radii |

The stage 1 thresholds (`simplifyValidation*` and their environment variables) also set how topology candidates are validated.
Environment overrides are hashed into the cache key only when set. `BASICRENDERER_CLOD_TOPOLOGY_DIAG=1` (not hashed) adds the
classification. Each build with either logs `ClusterLOD topology: ...`: roots, groups terminal by rejection and stuck, with
triangles; with the diagnostic, the stuck classes, components, small-component triangles, boundary-locked positions and UV-seam
vertices (tiny, whole-number); and the topology counters: groups that tried, candidates offered, rejected (per reason, texture
included), still stuck, accepted with their input triangles, relaxed vertices, texture checks and their time.
`BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_LOG=1` also logs each texture check (`ClusterLOD topology texture: ...`, with the area
beyond 1, 2, 4 and 8 radii, for calibrating the tolerance).

### Results on Bistro

Suite [config/clod_eval/stage5.json](../config/clod_eval/stage5.json), measured 2026-11-17, Bistro without textures. Every
variant has stage 3's channels on (`lodErrorChannels`); "update" is stage 2 without measured errors and sloppy off, and the stage 5
variants add to it: "topology" (UV seams kept), "+ loose" (topology candidates validated with stage 1's loose thresholds: shading 90
degrees / 10%, fold 5%, coverage 5%; validation stays off for everything else), "+ UV" (UV seams relaxed, texture check at its
defaults) and "+ UV unchecked" (texture check off: an upper bound, not a usable build). Rules: "stored" is the stored error,
`max w0.1` stage 3's `max` geometric rule with appearance weight 0.1. Values interpolate `appearance.cut.*` to equal drawn
triangles; "-" means the build cannot draw that few.

Q_N/area (lower is better):

| build : rule | fewest drawn | 1.5M | 1M | 750k | 500k | 350k | 250k |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline (sloppy) : stored | 183k | 0.0053 | 0.0140 | 0.0299 | 0.0484 | 0.1380 | 0.2245 |
| baseline (sloppy) : max w0.1 | 183k | 0.0027 | 0.0106 | 0.0231 | 0.0626 | 0.0856 | 0.2033 |
| update : stored | 465k | 0.0006 | 0.0123 | 0.0216 | 0.0442 | - | - |
| update : max w0.1 | 465k | 0.0009 | 0.0053 | 0.0221 | 0.0431 | - | - |
| topology : stored | 417k | 0.0006 | 0.0108 | 0.0211 | 0.0185 | - | - |
| topology : max w0.1 | 417k | 0.0009 | 0.0047 | 0.0189 | 0.0203 | - | - |
| topology + loose : stored | 399k | 0.0006 | 0.0108 | 0.0211 | **0.0169** | - | - |
| topology + loose : max w0.1 | 399k | 0.0009 | 0.0047 | 0.0187 | 0.0193 | - | - |
| topology + UV : stored | 251k | 0.0006 | 0.0100 | 0.0209 | 0.0175 | **0.0328** | - |
| topology + UV : max w0.1 | 251k | 0.0009 | **0.0044** | **0.0161** | 0.0199 | 0.0334 | - |
| topology + UV unchecked : stored | 233k | 0.0006 | 0.0094 | 0.0207 | 0.0185 | 0.0267 | 0.1059 |
| topology + UV unchecked : max w0.1 | 233k | 0.0009 | 0.0043 | 0.0150 | 0.0211 | 0.0283 | 0.1032 |

Area beyond 90 degrees and missing area:

| build : rule | > 90° @1M | > 90° @500k | > 90° @350k | missing @1M | missing @500k | missing @350k |
|---|---:|---:|---:|---:|---:|---:|
| baseline : stored | 0.19% | 0.65% | 2.28% | 0.17% | 0.56% | 0.70% |
| baseline : max w0.1 | 0.11% | 0.76% | 1.05% | 0 | 0 | 0 |
| update : stored | 0.18% | 0.52% | - | 0.25% | 1.20% | - |
| update : max w0.1 | 0.035% | 0.51% | - | 0 | 0 | - |
| topology : stored | 0.16% | 0.13% | - | 0.24% | 0.49% | - |
| topology : max w0.1 | 0.031% | 0.14% | - | 0 | 0 | - |
| topology + UV : stored | 0.14% | 0.14% | 0.23% | 0.23% | 0.65% | 0.07% |
| topology + UV : max w0.1 | 0.028% | 0.17% | 0.24% | 0 | 0 | 0 |

Builds and stored geometry:

| | baseline | update | topology | + loose | + UV | + UV unchecked |
|---|---:|---:|---:|---:|---:|---:|
| coarsest triangles | 183k | 465k | 417k | 399k | 245k | 228k |
| groups | 4686 | 4070 | 4180 | 4200 | 4926 | 5024 |
| terminal: roots / rejected / stuck | 88k / 0 / 95k | 60k / 0 / 405k | 61k / 52k / 303k | 61k / 26k / 312k | 71k / 174k / 0 | 72k / 156k / 0 |
| groups that tried topology candidates | - | - | 533 | 549 | 970 | 1027 |
| candidates offered / rejected / still stuck | - | - | 202 / 103 / 400 | 177 / 60 / 409 | 2034 / 1376 / 0 | 2081 / 1345 / 0 |
| rejections: fold / shading / texture | - | - | 79 / 49 / 0 | 13 / 7 / 36 | 1153 / 728 / 122 | 1229 / 791 / - |
| groups simplified by topology candidates | - | - | 99 | 117 | 658 | 736 |
| flipped vs source (stored, area) | 12.0% | 0.59% | 0.59% | 0.60% | 1.04% | 1.04% |
| of which topology triangles (method permissive) | - | - | 0.61% of 66k | - | 1.46% of 248k | - |
| inverted (stored, area) | 16.6% | 3.47% | 3.40% | 3.38% | 2.27% | 2.26% |
| build wall time (s) | 8.1 | 14.5 | 16.1 | 15.7 | 18.1 | 18.1 |
| cache bytes (MB) | 201 | 202 | 206 | 207 | 217 | 218 |

**Off is unchanged.** With it off, baseline and no-sloppy have stage 4's `hash=` values (0x83D874F403D6A3EE, 0xB5917D62BF19ACEE)
and byte-identical caches (3064 files each; the scene file differs only in its creation time). **Deterministic:** rebuilding
topology and topology + UV in a second workspace gives byte-identical caches (3064 files each).

What this establishes:

- **The floor is protect bits, and most of it is UV chart seams.** Group boundaries and components do not limit Bistro's coarse
  levels (diagnosis above). Relaxing normal hard edges (with fitted normals) unlocks the 29% of the stuck floor that is not held by
  UV seams; what stays stuck afterwards is entirely UV seams (303k triangles) plus candidates validation rejects (52k, mostly folds).
- **Relaxing hard edges pays at every cut it reaches.** Topology lowers the fewest drawn triangles from 465k to 417k (399k with
  loose validation of its candidates), and at 500k drawn triangles halves Q_N/area against update and baseline (0.0185 against 0.0442
  and 0.0484; 0.0169 loose) with a quarter of their area beyond 90 degrees (0.13% against 0.52% and 0.65%) and less missing area than
  either (0.49% against 1.20% and 0.56%). It also trims 1M (0.0108 against 0.0123) and, under `max w0.1`, beats every reference at
  1M and 750k. The triangles it adds are no worse shaded than the rest (0.61% flipped against 0.59%). Cost: 1.6 s of build wall time
  and 2% more cache bytes than update.
- **Crossing UV seams reaches baseline's territory at a fraction of its shading error, but moves texture.** With UV seams relaxed
  under the texture check the hierarchy reaches 251k drawn (baseline 183k): at 350k, Q_N/area 0.0328 against baseline's 0.1380 and
  0.0856 (`max w0.1`), with a tenth of its area beyond 90 degrees (0.23% against 2.28%) and of its missing area (0.07% against
  0.70%). Its stored flipped area (1.04%) is a twelfth of baseline's. Q_N does not see texture, and neither does this suite (built
  without textures): the texture check rejected only 122 candidates, and the unchecked build's curve is nearly the same, so most
  accepted crossings slide texture less than 8 radii on 98% of their area by the check's sampling, which is not a claim that they
  look right. Its floor is now validation (174k triangles terminal by rejection, 1153 fold and 728 shading rejections), not seams.
  It needs textured renders before it can be recommended; it stays opt-in.
- **Facade 686 and foliage do not move.** The facade's coarsest level (10367 triangles) is the same in every variant: its terminal
  groups are rejected by validation once relaxed. Foliage cards (mesh 66's double-sided leaves) stop at the same level
  (10252 triangles) in every topology variant: their cards are separate UV islands, and joining cards would need pair collapses across UV islands, which
  have the same texture problem.
- **Validation now limits the relaxed builds.** Topology candidates fail mostly on folds (79 of 103; 1153 of 1376 with UV), which no
  normal fit repairs; loose thresholds for them alone accept more (117 groups against 99) and give the best 500k cut.

### Not built, and why

- **Pair collapses / virtual edges between components**: components are never the first limit on Bistro (all stuck groups reach the
  threshold with every seam relaxed and no pruning), and the remaining component-heavy meshes (foliage cards) are separate UV islands,
  so joining them is a UV-seam crossing first.
- **Regrouping boundary-limited regions**: 16 boundary-locked positions in all stuck groups; nothing to regroup.
- **`meshopt_SimplifyPrune`**: deletes whole components (missing area), and is not needed by any stuck group.
- **Voxel remeshing and proxies**: noted by the plan as separate options; the UV-seam floor is the case for them (or for stage 6's
  independent appearance), since a triangle simplification that keeps texture mapping cannot go below the chart network.

Tests in [CLodEvalTests.cpp](../CLodEval/tests/CLodEvalTests.cpp): off, the topology settings change nothing (a build with them
equals stage 2's) and on, plain and validated builds are each identical twice, use topology candidates and differ from stage 2; on a
flat-shaded dome (every vertex a hard edge, UVs continuous) stage 2 without sloppy cannot simplify at all (8192 coarsest triangles)
while topology candidates reach 770, plain and validated, with no flipped or inverted area, and decoded UVs stay within two cells of
their positions (1.1% of corners beyond half a cell); on a dome tiled into turned UV charts, a strict texture check (1 radius) keeps
the coarsest level that keeping UV seams gives, while the same candidates unchecked cross and halve it.

## Follow-ups to stages 2, 4 and 5

Three builder-side follow-ups, each behind its own setting, **off by default**. With them off nothing changes: Bistro's baseline,
no-sloppy, update, topology and topology + UV builds have stage 5's `hash=` values and byte-identical CLod caches (3064 files each),
and every build with them on is byte-identical when rebuilt. No runtime, shader, page or format change; meshoptimizer is untouched.

### Where it lives

| Piece | File |
|---|---|
| UV transfer for every update candidate | `ReplacementVertexFitter::Fit`, `OriginalMatchAt` ([ClusterLODReplacementVertices.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODReplacementVertices.cpp)); `ReplacementVertexLayout::transferUvs` |
| Optimal topology candidates with stage 2's as fallback | `simplifyTopology` in [clusterlod.h](../BasicRenderer/include/ThirdParty/meshoptimizer/clusterlod.h) (`clodConfig::simplify_topology_update_fallback`) |
| Chart and texel tests | `MeasureTextureDisplacement` (`TextureCheckOptions`), `ComputeTextureCharts` ([ClusterLODTopology.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODTopology.cpp)); per-vertex charts grown by `ReplacementVertexFitter::Commit`; `clodValidateCandidate::output_vertex_ids` |
| Wiring, settings, logging | `BuildClusterLODArtifactsFromGeometry` in [ClusterLODClustering.cpp](../BasicRenderer/src/Assets/GeometryProcessing/ClusterLOD/ClusterLODClustering.cpp) |
| UV drift measurement | `quality.texcoords` ([TexcoordAnalyzers.cpp](../CLodEval/src/Analyzers/TexcoordAnalyzers.cpp), [clod-evaluation.md](clod-evaluation.md#texcoord-drift)) |

### Settings

| Field / JSON key | Environment | Default | Meaning |
|---|---|---|---|
| `simplifyUpdateUvTransfer` | `BASICRENDERER_CLOD_SIMPLIFY_UPDATE_UV_TRANSFER` | off | Every update candidate (stages 2, 4, 5) transfers every solved UV stream from the original |
| `simplifyTopologyUpdateFallback` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_UPDATE_FALLBACK` | off | With `simplifyOptimal`, a rejected or stuck optimal topology candidate is followed by stage 2's |
| `simplifyTopologyTextureCharts` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_CHARTS` | off | Texture check: chart identity |
| `simplifyTopologyTextureTexels` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_TEXELS` | 0 (off) | Texture check: slide tolerance in texels of a nominal texture |
| `simplifyTopologyTextureResolution` | `BASICRENDERER_CLOD_SIMPLIFY_TOPOLOGY_TEXTURE_RESOLUTION` | 512 | That nominal resolution |

Environment overrides are hashed into the cache key only when set. The `ClusterLOD simplify update` log line gains `uv_transferred`
and `uv_transfer_outside`; the `ClusterLOD topology` line gains `texture_rejected_slide`, `texture_rejected_uv` and
`texture_rejected_charts` (candidates each test alone would reject; set only when a follow-up test is on), and
`BASICRENDERER_CLOD_SIMPLIFY_VALIDATION_LOG=1` adds each test's area to the per-candidate texture line.

### 1. UV transfer for every update candidate

**What was wrong.** Stage 5's transfer replaced only the record texcoord (stream 0), and only for topology candidates. Importers that
give UV sets (glTF always does: `TEXCOORD_n`) have their pages store the **UV sets**, not the record texcoord
(`BuildClusterLODGroupOutput` writes the record texcoord only when there are no sets), and the update solves each set as its own
stream. On every glTF asset, Bistro included, stage 5's transfer therefore never reached the rendered UVs; and its texture check
compared the transferred record texcoord, not what is drawn. (The synthetic tests have no UV sets, which is why they did not see it.)

**Design.** With `simplifyUpdateUvTransfer`, every moved vertex of every update candidate (stage 2's, stage 4's, stage 5's) is matched
once to the original (side-aware, `MatchFacing`, as normals are fitted), and **each solved UV stream** (the record texcoord and every
solved UV set) takes the original's value there, read from the source vertices' attribute rows, when it lies inside that stream's
stage 2 box (the vertex's own and its neighbours' stored values) grown by a quarter; otherwise the clamped solve stays (counted as
`uv_transfer_outside`). Unmoved vertices keep their UVs. Without the setting, stage 5's own transfer runs exactly as before.

### 2. Stage 4's optimal loop on topology candidates

The combination runs as written: a topology candidate with `simplifyOptimal` is an optimal-loop candidate on the private copy whose
relaxable seam vertices lost their protect bit, so the copied loop's permissive classification (upstream's) treats them as complex
vertices; the per-collapse flip test and the persistent source-sheet test apply (its references are measured for the topology
candidates' input in their own `OptimalGroupState`), and stage 2's fit refits normals and exports texcoords afterwards. Nothing in the
loop had to change. What was missing is policy: `simplifyTopology` offered **only** the optimal candidate, where validated
simplification offers the optimal candidate and then stage 2's. `simplifyTopologyUpdateFallback` makes topology do the same: for each
target, the optimal candidate, then (if it is rejected or stuck) stage 2's relaxed candidate; each kind stops at its own stuck size.

### 3. Texel and chart tests for the texture check

**What the builder knows.** Nothing about textures: `BuildClusterLODArtifactsFromGeometry` gets vertices, UV sets and builder settings;
the glTF, Assimp and USD importers call it without the material (the only material-derived input is `doubleSided`), and the CLod
cache key does not include textures. So a texel tolerance is over a **nominal** resolution (`simplifyTopologyTextureResolution`, 512):
a UV-space distance. Plumbing real texture sizes would change the importers and the cache key; not done.

- **Texels** (`simplifyTopologyTextureTexels` T > 0): a sample is displaced when its texcoord differs from the original's at the match
  by more than T / resolution UV units, whatever the radius.
- **Charts** (`simplifyTopologyTextureCharts`): the source's UV charts are its connected components of triangles sharing a vertex,
  where vertices at one position whose texcoords agree within 1/1024 (stage 5's UV-seam rule) are one vertex, so normal hard edges do
  not cut charts and UV seams do (`ComputeTextureCharts`, numbered by lowest vertex: deterministic). Every build vertex carries its
  chart (a replacement inherits its parent's, appended in `Commit`); update candidates hand the validator their private vertices'
  ids (`clodValidateCandidate::output_vertex_ids`). A candidate triangle whose corners belong to different charts is displaced whole
  (it interpolates across the atlas between them); otherwise a sample is displaced when its chart differs from the matched original
  triangle's (texture from another chart, however near in mesh units).

Both are OR-ed with stage 5's slide test per sample, against the same area limit (`simplifyTopologyTextureArea`).

### Results on Bistro

Suite [config/clod_eval/followups.json](../config/clod_eval/followups.json), measured 2026-11-17, Bistro without textures. Every
variant except baseline has stage 3's channels and is stage 5's "update" (stage 2 without measured errors, sloppy off) plus what its
name says: "transfer" `simplifyUpdateUvTransfer`, "optimal" `simplifyOptimal`, "fallback" `simplifyTopologyUpdateFallback`, "loose"
stage 1's loose thresholds for topology candidates, "charts" and "texels 4" the new texture tests (4 texels of a nominal 512).
Each cell is stored rule / stage 3's `max w0.1`; "-" means the build cannot draw that few.

Q_N/area at equal drawn triangles (lower is better):

| build | fewest drawn | 1.5M | 1M | 750k | 500k | 350k | 250k |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline (sloppy) | 183k | .0053 / .0027 | .0140 / .0106 | .0299 / .0231 | .0484 / .0626 | .1380 / .0856 | .2245 / .2033 |
| update (stage 2) | 465k | .0006 / .0009 | .0123 / .0053 | .0216 / .0221 | .0442 / .0431 | - | - |
| update + transfer | 466k | .0006 / .0009 | .0123 / .0053 | .0216 / .0223 | .0447 / .0436 | - | - |
| topology (stage 5) | 417k | .0006 / .0009 | .0108 / .0047 | .0211 / .0189 | .0185 / .0203 | - | - |
| topology + transfer | 413k | .0006 / .0009 | .0108 / .0047 | .0211 / .0189 | .0178 / .0198 | - | - |
| topology + optimal | 424k | .0008 / .0009 | .0225 / .0056 | .0166 / .0184 | .0218 / .0218 | - | - |
| topology + optimal + fallback | 423k | .0008 / .0009 | .0225 / .0056 | .0166 / .0184 | .0217 / .0217 | - | - |
| topology + optimal + loose | 408k | .0008 / .0009 | .0224 / .0056 | .0168 / .0183 | .0206 / .0210 | - | - |
| topology + optimal + fallback + loose | 407k | .0008 / .0009 | .0224 / .0056 | .0168 / .0183 | .0206 / .0210 | - | - |
| topology + UV (stage 5) | 251k | .0006 / .0009 | .0100 / .0044 | .0209 / .0161 | .0175 / .0199 | .0328 / .0334 | - |
| topology + UV + transfer | 249k | .0006 / .0009 | .0099 / .0044 | .0209 / .0163 | .0175 / .0198 | .0328 / .0333 | .1249 / .1249 |
| topology + UV + optimal | 260k | .0008 / .0009 | .0208 / .0054 | .0192 / .0165 | .0178 / .0191 | .0417 / .0411 | - |
| topology + UV + optimal + fallback | 226k | .0008 / .0009 | .0205 / .0053 | .0196 / .0161 | .0172 / .0193 | .0280 / .0280 | .0984 / .0966 |
| topology + UV + optimal + loose | 172k | .0008 / .0009 | .0205 / .0055 | .0204 / .0160 | .0169 / .0202 | .0241 / .0242 | .0768 / .0737 |
| topology + UV + optimal + fallback + loose | 151k | .0008 / .0009 | .0202 / .0054 | .0207 / .0159 | .0168 / .0203 | .0235 / .0236 | .0641 / .0623 |
| topology + UV + charts | 406k | .0006 / .0009 | .0101 / .0046 | .0210 / .0176 | **.0169** / .0186 | - | - |
| topology + UV + transfer + charts | 406k | .0006 / .0009 | .0101 / .0046 | .0210 / .0177 | **.0169** / .0185 | - | - |
| topology + UV + transfer + texels 4 | 457k | .0006 / .0009 | .0121 / .0052 | .0215 / .0216 | .0367 / .0364 | - | - |
| topology + UV + transfer + charts + optimal + fallback | 413k | .0008 / .0009 | .0210 / .0055 | .0179 / .0179 | .0208 / .0210 | - | - |
| the same + loose | 407k | .0008 / .0009 | .0210 / .0055 | .0182 / .0178 | .0205 / .0208 | - | - |

Area beyond 90 degrees and missing area, stored rule (every build's `max w0.1` has no missing area at any of these cuts):

| build | > 90° @1M | @500k | @350k | @250k | missing @1M | @500k | @350k | @250k |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline | 0.19% | 0.65% | 2.28% | 3.84% | 0.17% | 0.56% | 0.70% | 0.62% |
| topology | 0.16% | 0.13% | - | - | 0.24% | 0.49% | - | - |
| topology + optimal + fallback + loose | 0.31% | 0.13% | - | - | 0.09% | 0.54% | - | - |
| topology + UV | 0.14% | 0.14% | 0.23% | - | 0.23% | 0.65% | 0.07% | - |
| topology + UV + transfer | 0.14% | 0.14% | 0.23% | 1.68% | 0.23% | 0.65% | 0.09% | 0 |
| topology + UV + optimal + fallback | 0.28% | 0.10% | 0.16% | 1.02% | 0.09% | 0.50% | 0.07% | 0.58% |
| topology + UV + optimal + fallback + loose | 0.28% | 0.10% | 0.15% | 0.58% | 0.09% | 0.52% | 0.55% | 1.45% |
| topology + UV + charts | 0.14% | 0.12% | - | - | 0.23% | 0.63% | - | - |
| topology + UV + transfer + texels 4 | 0.18% | 0.36% | - | - | 0.25% | 0.41% | - | - |

Texcoord drift of stored simplified geometry against the leaves (`quality.texcoords`, UV units). "Topology triangles" are those
of groups simplified by topology candidates (the permissive method):

| build | corners p95 / p99 | interior mean | interior > 1/64 | interior > 1/16 | topology triangles > 1/16 |
|---|---:|---:|---:|---:|---:|
| baseline (sloppy) | 1e-5 / 0.47 | 0.867 | 39.5% | 36.5% | - |
| no-sloppy (no vertex moves: the floor) | 1e-5 / 2e-5 | 0.104 | 8.73% | 6.01% | - |
| update | 0.0011 / 0.072 | 0.104 | 9.16% | 6.37% | - |
| update + transfer | **1e-5 / 0.0023** | 0.104 | 8.91% | 6.09% | - |
| topology | 0.0013 / 0.077 | 0.106 | 9.72% | 6.67% | 21.9% |
| topology + transfer | **1e-5 / 0.0073** | 0.106 | 9.50% | 6.41% | 22.0% |
| topology + UV | 0.0067 / 0.84 | 0.306 | 21.3% | 17.3% | 30.7% |
| topology + UV + transfer | 1e-5 / 0.49 | 0.319 | 21.7% | 18.3% | 33.8% |
| topology + UV + optimal + fallback + loose | 0.12 / 1.3 | 0.467 | 30.1% | 25.3% | 39.8% |
| topology + UV + charts | 0.0016 / 0.10 | 0.105 | 9.38% | 6.42% | 8.6% |
| topology + UV + transfer + charts | 1e-5 / 0.018 | 0.105 | 9.09% | 6.15% | 9.5% |
| topology + UV + transfer + texels 4 | 1e-5 / 0.0025 | 0.104 | 8.90% | 6.08% | 0.5% |

Topology candidates (summed `ClusterLOD topology` counters):

| build | groups tried | offered | rejected: fold / shading / texture | texture by test: slide / texels / charts | accepted groups | terminal rejected triangles |
|---|---:|---:|---:|---:|---:|---:|
| topology | 533 | 202 | 79 / 49 / 0 | - | 99 | 52k |
| topology + optimal | 517 | 172 | 73 / 56 / 2 | - | 84 | 49k |
| topology + optimal + fallback | 522 | 276 | 145 / 108 / 3 | - | 96 | 50k |
| topology + optimal + fallback + loose | 541 | 229 | 28 / 10 / 74 | - | 117 | 28k |
| topology + UV | 970 | 2034 | 1153 / 728 / 122 | - | 658 | 174k |
| topology + UV + optimal | 972 | 2054 | 1193 / 889 / 104 | - | 647 | 177k |
| topology + UV + optimal + fallback | 1066 | 3534 | 2336 / 1767 / 192 | - | 816 | 137k |
| topology + UV + charts | 588 | 1662 | 585 / 364 / 917 | 116 / - / 916 | 101 | 345k |
| topology + UV + transfer + texels 4 | 543 | 1612 | 540 / 329 / 968 | 122 / 968 / - | 46 | 397k |

Build wall time and cache bytes: update 14.0 s, transfer 13.8 s (no measurable cost: one more original match per moved vertex);
topology 15.3 s / 215 MB; topology + UV 17.3 s / 227 MB; charts 17.5 s / 215 MB (the chart labelling is negligible, the chart test
adds a few seconds of CPU to texture checks); optimal adds 5-8 s to topology (20.7 s, 223 MB) and the fallback 1-7 s more (22.1 s;
31.6 s with UV seams relaxed). Rebuilding update + transfer, topology + UV + transfer + charts, topology + UV + transfer + texels,
topology + optimal + fallback + loose and topology + UV + optimal + fallback in a second workspace gives byte-identical caches (3064
files each).

**Drift, measured.** On the synthetic domes (UV = xy, drift against the leaves at the match, per depth; `TestUpdateUvTransfer`): on
the faceted dome with a TEXCOORD_0 set, meshoptimizer's per-wedge solve drifts 5.5e-4 UV inside triangles at depth 1 and 1.0e-3 at
depth 2, and the transfer brings that to 1.3e-4 and 4.9e-4 (corners 6.1e-4 to 1.7e-4, 1.1e-3 to 6.6e-4); cells are 1/64. It also
lets that dome reach 794 coarsest triangles instead of 2048: solved wedges of one position no longer agree to 1/1024, so stage 5 read
them as UV seams and stopped. On a smooth dome (shared wedges) the solve is already accurate (it reproduces UVs linear in position):
corners improve (2e-5 to 7e-6: the pages' quantization), interiors get about twice the solve's drift (1.6e-4 to 3.0e-4 mean, under a
texel of a 1024 texture at every depth), because the solve is a least-squares fit over each triangle's area where the transfer
interpolates exact corners. On Bistro the vertex UVs are where the solve drifted: corner drift p95 1.1e-3 to 1e-5 and p99 0.072 to
0.0023 UV (no-sloppy, which moves nothing, has 2e-5). Interior drift on Bistro is dominated by a floor that exists without any moved
vertex (no-sloppy 0.104 mean, 6.0% of area beyond 1/16 UV): coarse triangles interpolate UVs linearly over leaves whose UVs are not
linear in them, and match leaves of other pieces. The transfer brings update's interior tail to within 0.1 point of that floor (beyond
1/16: 6.37% to 6.09%; no-sloppy 6.01%).

What this establishes:

- **The transfer fixes vertex UVs at no cost to shading.** Q_N is unchanged within interpolation noise everywhere (at 500k update
  0.0442 to 0.0447, topology 0.0185 to 0.0178), the flipped and inverted areas are unchanged, and build time and bytes are the same.
  It also makes stage 5's transfer reach the UVs that are drawn on glTF assets. **Recommendation: make it the default whenever update
  is on**, after the textured live A/B confirms it visually; the one cost, interiors of smooth regions drifting a fraction of a texel
  more than the solve, is far below the solve's errors on hard-edged geometry.
- **Optimal placement does not remove topology candidates' folds.** Their fold rejections are barely over the limit (median 1.7% of
  area against 1%, many at depth 0) and the optimal loop leaves them where they were (73 of 172 offered against 79 of 202). Its sheet
  test compares each face with the area-weighted mean of the source orientation it has absorbed; across a relaxed hard crease that mean
  lies between the two sides, so a face within 60 degrees of it can still meet one side beyond 90 degrees, which is a fold to the
  validator's per-sample test. Only loose thresholds unlock them (fold rejections 28 of 229). The stored rule shows stage 4's 1M
  regression (0.0225 against 0.0108; the level switch moves with optimal's smaller errors) and stage 4's 750k gain (0.0166 against
  0.0211); `max w0.1` removes the regression (0.0056) but is then no better than topology alone (0.0047 at 1M, 0.0189 at 750k, worse
  at 500k).
- **The fallback is needed for the combination to be usable.** Optimal-only topology accepts fewer groups than plain topology (84
  against 99): its candidates fail where stage 2's pass, and on the faceted dome every one fails stage 5's texture check (its tolerance
  is in radii of the candidate's error, and the optimal loop reports far smaller errors), leaving the dome at its 8192 leaves. With the
  fallback the dome reaches 773 (topology alone 770); on Bistro it adds 12 accepted groups (169 with UV seams relaxed, fewest drawn 260k
  to 226k). **Recommendation: whenever topology and optimal are both on, turn the fallback on; do not make optimal + topology the
  default** (it does not beat topology alone at equal triangles under `max w0.1`, and costs 5-8 s per Bistro build).
- **The chart test catches what the slide test passes.** Of topology + UV's candidates, the chart test rejects 916 where the slide test
  rejects 116; with it, relaxing UV seams accepts 101 groups, about plain topology's 99, and its topology triangles drift no more than
  ordinary simplification (8.6% of their area beyond 1/16 UV, against 30.7% unchecked, 21.9% for plain topology's). It still gains: the
  collapses it lets through (along seams, inside charts) give the best 500k cut of any build without texture crossings (0.0169 against
  topology's 0.0185) and 406k fewest drawn against 417k. Nearly every accepted crossing of stage 5's topology + UV moved texture from
  another chart onto the surface: its 251k is not reachable without moving texture. **Recommendation: whenever UV seams are relaxed,
  turn the chart test on; prefer topology + UV + transfer + charts over topology for textured assets.**
- **The texel test is the wrong tool without texture sizes.** At 4 texels of a nominal 512 it rejects 968 candidates, including
  legitimate ones whose UVs slide by matching alone (a sample matched across a curved surface moves by its distance, which at coarse
  levels is many texels and is not visible at the distance the level is drawn), and accepts only 46 groups (457k fewest drawn, worse
  than topology). Slide that is proportional to the error radius is what a viewer at the level's distance sees; crossing charts is the
  failure that is not. Not recommended.
- **Charts and optimal do not combine usefully**: 413k fewest drawn and 0.0208 at 500k against charts alone's 406k and 0.0169.

Tests in [CLodEvalTests.cpp](../CLodEval/tests/CLodEvalTests.cpp): with update off the transfer changes nothing, and the new topology
settings change nothing with topology off; `TestUpdateUvTransfer` (smooth dome with and without a UV set, faceted dome with a set:
deterministic, leaves unchanged, corners improve at every depth, faceted interiors drop by at least 30% at every depth both builds have
and the faceted hierarchy goes at least twice as far, smooth interiors stay within two texels of a 1024 texture);
`TestOptimalTopology` (faceted dome: optimal-only topology stays at its leaves; with the fallback, plain and validated, it reaches
within 1.5x of topology alone with no flipped or inverted area; every build identical twice); `TestTopologyTextureChartsAndTexels`
(chart dome: stage 5's default slide test lets crossings halve the coarsest level, 127 to 63 triangles, while the chart test and the
texel test each keep 127, deterministically). `CLOD_EVAL_TEST_FILTER=<substring>` runs only the matching tests.

For stage 6: below about 400k drawn triangles on Bistro every further reduction found here crosses UV charts (topology + UV, optimal +
loose: 251k, 151k), so a triangle simplification that keeps the texture mapping stops at the chart network. Going lower needs
appearance that does not depend on the source parameterization: per-patch baked or re-parameterized textures, filtered normals and
albedo. The interior UV floor above (a tenth of a UV unit mean even when nothing moves) is the same mismatch at every level: coarse
triangles interpolate UVs linearly over detail whose UVs are not linear. Stage 6 measurements should compare against the leaves'
texture, not their UVs, and the builder will need texture data it does not get today (importers pass no material; the CLod cache key
has none).

Stage 6 is in [clod-appearance-stage6.md](clod-appearance-stage6.md): its design, and a first increment (a texture term in the
appearance channel, and CLodEval's `quality.texture`, which measures sampled base colour against the leaves).

## Building blocks

`SimplifyValidator::Validate` takes `SimplifySurfaceView`s (positions, normals, indices, strides), not original vertex
indices; stage 2 points the output view at an update candidate's private buffers and the input view at the build vertex
store. The pieces are public (`MeasureAgainstOriginal`, `MeasureCoverage`, `MeasureSourceCoverage`, `MeasureBaseline`,
`MeasureDeviation`, `OriginalNormalAt`, `OriginalFaceNormalAt`, `SimplifyTriangleBvh::MatchFacing`) for builders that combine them differently.
`MatchFacing` also comes in two parts: `GatherNear` (the traversal, which does not depend on the face) and `SelectFacing`. Use
`GatherNearJoint` for several reaches and the nearest distance at one point in one traversal, and `MatchFacingCache` to memoize per
exact point. They return exactly what `MatchFacing` and `NearestDistanceSquared` return. How the defaults use them, and the build cost,
are in [clod-appearance-stage6.md](clod-appearance-stage6.md) under "Build cost".
