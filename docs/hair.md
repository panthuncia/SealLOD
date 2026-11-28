# Strand hair

`Plugins/Hair/` is a proof of concept for strand hair: grooms made of individual curves, with a level of detail that
removes strands and vertices continuously and a guide set sized for simulation that follows the view. It is a plugin
beside `Plugins/ProceduralWind/` and is expected to be reworked several times.

Two libraries exist:

- `HairCore`, the offline half: a static library with no device and no renderer dependency. It imports a groom,
  builds the arrays the GPU consumes, and reads and writes them as a `.groom` file. `HairCoreTests` checks it.
- `HairPlugin`, the renderer half ([Runtime](#runtime)): it loads grooms, attaches them to scene entities and uploads
  them. Every frame it selects a level of detail per cluster for the main view, simulates as many of the guides as the
  view asks for when [simulation](#simulation) is on (and none of a groom the view does not show), evaluates the selected
  strands into a pool, rasterizes them into a hair visibility target, and brings that target into the frame by one of
  two paths. The demo (`BasicRendererDemo`) registers it; SARP's renderer host does not.

**Two paths bring hair into the frame, chosen with `hair.render`.** On the layered path hair shadows itself and casts
the sun's shadow on everything else; on the surface path it casts none.

- **The surface path** (the default) draws hair as an ordinary opaque surface with a placeholder material. The
  [resolve](#the-resolve) gives every hair pixel a surface record, depth and a motion vector, so deferred shading lights
  it, shadows fall on it and the upscaler follows it. A strand thinner than a pixel is drawn into a one-pixel footprint
  and written there with the probability of its coverage ([The rasterizer](#the-rasterizer)), so that over the frames
  the upscaler integrates it has its true weight; a pixel still holds one opaque sample, hair or not.
- **The layered path** keeps a pixel's three nearest strands and the summed coverage of all of them, lights every drawn
  strand vertex with a light loop of hair's own, and blends the layers over the lit frame
  ([The layered path](#the-layered-path)). Nothing is accepted at random, hair is not in the frame's surface data, and
  only the pixels hair mostly covers take its depth and motion vector. A fibre's response to light is hair's own
  ([The hair shading model](#the-hair-shading-model)): the R, TT and TRT lobes of the real-time Marschner model, with a
  material per groom. Hair shadows itself there, and light scattered from fibre to fibre is accounted for, through a
  volume of hair density per groom that is rebuilt every frame ([The density volume](#the-density-volume),
  [Multiple scattering](#multiple-scattering)). [Layered path measurements](#layered-path-measurements) has what the
  path buys and costs against the surface path, [Hair shading measurements](#hair-shading-measurements) what the model
  looks like and costs, and [Self-shadowing measurements](#self-shadowing-measurements) the same for the volume. On
  this path hair is also drawn into the sun's virtual shadow map, with its coverage dithered
  ([Shadow casting](#shadow-casting), [Cast shadow measurements](#cast-shadow-measurements)); spot and point lights get
  no shadow from hair.

Strands follow their roots rigidly unless simulation is turned on (`hair.sim`):
then the groom's guides are simulated on the GPU, collide with spheres and capsules, and the other strands follow them.
How many guides are simulated follows how large the groom is on screen, and a groom the view does not show is not
simulated ([Simulation LOD](#simulation-lod)).

POC limits that shape the code: a whole groom is held in memory and on the GPU, there is no paging, streaming or
culling hierarchy, and little is tuned for speed: the layered path's rasterizer is, for hair that hides hair
([The culled rasterizer](#the-culled-rasterizer), [Rasterizer cost measurements](#rasterizer-cost-measurements)).

## Pipeline

```
procedural generator ─┐
Cem Yuksel .hair ─────┼─> GroomSource ──BuildGroom──> BuiltGroom ──SaveGroomFile──> .groom
Alembic groom ────────┘        (+ optional ScalpMesh)            <──LoadGroomFile──
```

[`GroomSource`](../Plugins/Hair/include/Hair/GroomSource.h) is the importer-neutral groom: polylines ordered root to
tip (`strandPointOffsets`, `positions`, `widths` as diameters), optional per-point `colors`, and optional per-strand
`guideFlags`, `groupIds`, `rootUVs`, `closestGuides` and `closestGuideWeights`. An optional array is either empty or
has one entry per strand. `ValidateGroomSource` checks the sizes, that every strand has at least two points, and that
closest-guide indices are strand indices or -1.

### Importers

| Source | Entry point | Notes |
|---|---|---|
| Procedural | `GenerateGroomOnSphereCap`, `GenerateGroomOnTriangles` in [`ProceduralGroom.h`](../Plugins/Hair/include/Hair/ProceduralGroom.h) | Test fixtures: straight, wavy or curly strands growing along the surface normal. With `droop` above 0 (the default is 0) the growth direction is pulled toward `droopDirection` step by step, so strands hang; a drooping strand is kept outside the sphere, 1% to 5% of its radius above it, or on the outer side of its root's face. Strand `i` depends only on `(seed, i)`. Fills no optional array. |
| Cem Yuksel `.hair` | `LoadHairFile` in [`HairFileReader.h`](../Plugins/Hair/include/Hair/HairFileReader.h) | Positions and thickness are kept in the file's own units and axes: nothing converts them. Per-point colour is kept, transparency is dropped. Strict: a strand with zero segments or a non-finite value rejects the file. The models are free for research use and are not in the repository. |
| Alembic | `LoadAlembicGroom` in [`AlembicGroomReader.h`](../Plugins/Hair/include/Hair/AlembicGroomReader.h) | The UE groom schema, below. |

### Alembic grooms

`LoadAlembicGroom` walks the archive, reads every `ICurves` object at its first sample, applies its parent
transforms, and appends the curves in traversal order into one `GroomSource`.

- **Curve types.** Linear curves are read as they are. For cubic and variable-order curves the stored control
  vertices are used as the polyline; the basis is not evaluated.
- **Widths** come from `groom_width`, else the curves schema's own width property, else the schema's 1 cm fallback.
  Widths are converted to metres but not scaled by the parent transforms.
- **Per-strand attributes:** `groom_guide`, `groom_group_id`, `groom_root_uv`, `groom_closest_guides` and
  `groom_guide_weights`. An attribute is found on the curves object (arbitrary geom params, then user properties) or
  on the nearest parent transform that has it. It may hold one value for the object, one per curve or one per point,
  told apart by the value count; any integer type and 32- or 64-bit floats are accepted, as a scalar property, an array, or an indexed
  geom param. A count that fits none of the three is an error that names the object and the attribute.
- **Closest guides** are kept only when `groom_guide_weights` is present too. Their values are `groom_id` values when
  the file has `groom_id`, otherwise curve indices in archive order; the importer turns both into strand indices of
  the `GroomSource`, and a reference to a curve that is not in it becomes -1.
- **Missing attributes.** An optional array stays empty when no curves object authors its attribute. When some
  objects do and others do not, the others get 0 (not a guide, group 0, UV 0, no closest guides).
- **Skipped curves.** A curve with fewer than two points is left out and counted in
  `AlembicGroomImportInfo::skippedStrandCount`.
- **Not read:** `groom_color`, any sample after the first, curve periodicity and basis.
  `groom_version_major` / `groom_version_minor` are reported in `AlembicGroomImportInfo` and not otherwise checked.
- **Errors:** a missing file (`cannot open file`), a file that is not an Ogawa archive, an archive with no curves, an
  archive whose curves all have fewer than two points, and malformed attributes. The library is built without HDF5,
  so pre-Ogawa Alembic files are not readable.

Alembic and its Imath come from vcpkg and are linked by `HairCore` (privately) and its tests only. When
BasicRenderer is built inside SARP the manifest in effect is SARP's, so the dependency is listed in both
[`vcpkg.json`](../vcpkg.json) and SARP's `vcpkg.json` (`renderer-host` feature).

### Builder

`BuildGroom` in [`GroomBuilder.h`](../Plugins/Hair/include/Hair/GroomBuilder.h) turns a `GroomSource`, an optional
`ScalpMesh` and `GroomBuildSettings` into a `BuiltGroom`. It is deterministic. The stages, in order
([`GroomBuilder.cpp`](../Plugins/Hair/src/GroomBuilder.cpp)):

1. **Check inputs:** `ValidateGroomSource`, at least one strand, finite positions and widths, settings in range, a
   well-formed scalp mesh.
2. **Rank.** A greedy farthest-point ordering of the root positions (`RankStrands`): each next strand is the one
   farthest from every strand already ordered, so any prefix of the ranking covers the roots evenly. Strands with
   `guideFlags` set are ordered first, among themselves by the same rule.
3. **Cluster.** Strands are split in half along the widest axis of a six-dimensional feature (root position, and tip
   minus root scaled by `clusterShapeWeight`, default 0.5) until every part has at most
   `HAIR_MAX_STRANDS_PER_CLUSTER` (64) strands. A groom with more than 64 strands therefore gets clusters of 32 to
   64. Each cluster's strands are then sorted by rank.
4. **Guide-capable set and levels.** The first `maxGuideCount` ranks (default 4096, clamped to the strand count).
   Level `j` ends at rank `guideLevelBase * 4^j` (base 64), so the default levels hold 64, 192, 768 and 3072 guides.
5. **Guide bindings.** Level 0 guides have no parents. Every other guide binds to the (up to) three nearest roots of
   the level directly below, with normalized inverse-distance weights.
6. **Per cluster:**
   - **Vertex level** `k`: the largest any of its strands asks for (one segment per `maxSegmentAngleRadians`, default
     0.35, of total turning, and never more segments than the source strand has), clamped to
     `[minVertexLevel, maxVertexLevel]` (defaults 3 and 5; the format allows up to `HAIR_MAX_VERTEX_LEVEL`, 10).
   - **Resample** every strand to `2^k + 1` points evenly spaced by arc length (`ResampleStrand`); root and tip are
     the source's.
   - **Strand record:** point offset, rank, width (the widest diameter along the strand), cluster index, and guides.
     A guide-capable strand binds to itself with weight 1. Any other strand uses its authored closest guides when all
     of them are guide-capable and the weights are finite, non-negative and not all zero; otherwise it binds to the
     three nearest guide-capable roots by inverse distance. Weights are normalized to sum to 1.
   - **Root binding.** With a scalp mesh: the nearest triangle (a linear scan with a bounding-sphere reject), its
     barycentrics, the four largest barycentric-weighted skin influences, and the triangle's normal and tangent.
     Without one: joint 0 with weight 1 and a frame around the direction the strand leaves its root.
   - **Bounds and quantization:** the box and sphere around the resampled centrelines (strand width is not
     included), and each point packed into 16 bits per axis across that box.
   - **Aggregate:** see below.

Source colours, group ids and root UVs are not carried into the built groom.

## Data model

A `BuiltGroom` is exactly the arrays in [`HairGPUTypes.h`](../Plugins/Hair/include/Hair/HairGPUTypes.h), in groom
rest space. All indices are groom-relative.

| Array | Element (bytes) | Indexed by |
|---|---|---|
| `clusters` | `HairClusterHeader` (80) | cluster |
| `aggregates` | `HairClusterAggregate` (208) | cluster |
| `strands` | `HairStrandRecord` (48) | built strand: cluster by cluster, sorted by rank inside each cluster |
| `rootBindings` | `HairRootBinding` (80) | built strand |
| `guideBindings` | `HairGuideBinding` (32) | rank, for the guide-capable set only |
| `guideStrandIndices` | `uint32` | rank -> index into `strands` |
| `guideLevelOffsets` | `uint32` | level; level `j` owns ranks `[offsets[j], offsets[j + 1])`, so there is one more entry than levels |
| `points` | `HairPackedPoint` (8) | `HairStrandRecord::pointOffset` |

- **Rank** is the global progressive order from builder stage 2; rank 0 is kept longest. It drives both strand LOD
  and the guide set.
- **Cluster.** `strandOffset` / `strandCount` name a contiguous, rank-sorted run of `strands`. `vertexLevel` is `k`:
  every strand of the cluster has `2^k + 1` points (`HairStrandPointCount`), stored back to back. `randomOffset` is
  the per-cluster stagger of the LOD rule. `parentIndex` is reserved for a culling hierarchy and is always
  `HAIR_INVALID_INDEX`.
- **Guide-capable set and levels.** Guide indices everywhere are ranks, not strand indices: `guideIndices` in a
  strand record, `parentIndices` in a guide binding, and the index into `guideBindings` itself. Guides are ordinary
  render strands, so a guide and its parents can have different point counts (they take their clusters' vertex
  levels); anything interpolating between them has to sample by curve parameter, not by point index.
  `HairGuideBinding`'s level, parents and `randomOffset` are what [simulation LOD](#simulation-lod) runs on: a guide
  of level `k` is simulated while its instance's simulation level is at least `k + randomOffset`, and follows its
  parents otherwise.
- **Bindings.** Unused guide or parent slots hold `HAIR_INVALID_INDEX` with weight 0. `HairRootBinding` carries the
  rest position, four joints and weights, a rest normal and tangent, and the scalp triangle with barycentrics
  (`triangleIndex` is `HAIR_INVALID_INDEX` without a scalp mesh).
- **Aggregate.** `HAIR_AGGREGATE_CONTROL_POINT_COUNT` (4) control points from root to tip. Each has the mean of the
  cluster's strands at that curve parameter and the PCA ellipse of their offsets across the centreline, as two axis
  vectors scaled to two standard deviations. `density` is the strand count over the mean ellipse area.
- **Packed points.** `position = quantizationOrigin + quantizationScale * (x, y, z)` with 16-bit `x`, `y`, `z`, and
  `diameter = strand width * w / 65535` with a 16-bit `w`. Decode with `DecodeHairPointPosition` and
  `DecodeHairPointWidth` in `GroomBuilder.h`.

For scale, a generated 50,000-strand curly groom with default settings builds to 1,024 clusters, 1,650,000 points
(every cluster at vertex level 5), 4,096 guides in four levels, and a 20,042,564-byte file. `HairCoreTests` prints
this summary on every run.

## The `.groom` file

[`GroomFile.h`](../Plugins/Hair/include/Hair/GroomFile.h): one little-endian file, a 176-byte `GroomFileHeader`
followed by the raw arrays, uncompressed.

| Header field | Type | Meaning |
|---|---|---|
| `magic` | `char[8]` | `BRGROOM\0` |
| `schemaVersion` | `uint32` | `kGroomSchemaVersion`, currently 1 |
| `headerBytes` | `uint32` | 176 |
| `fileBytes` | `uint64` | size of the whole file |
| `boundsMin`, `boundsMax` | 3 floats each | groom bounds |
| `arrays[8]` | `{ uint64 byteOffset; uint32 count; uint32 stride; }` | one per array |

The arrays are written in `GroomFileArray` order: clusters, aggregates, strands, root bindings, guide bindings, guide
strand indices, guide level offsets, points.

**Versioning rule:** bump `kGroomSchemaVersion` with any change to `GroomFileHeader`, to the array list, or to a
struct in `HairGPUTypes.h`. The loader accepts only the current version; there is no migration. The runtime-only
structs in `HairRuntimeGPUTypes.h` are not in the file and need no bump.

`LoadGroomFile` rejects a wrong magic, version or header size, a file whose size differs from `fileBytes`, an array
whose stride is not the reader's struct size or that lies outside the file, and then anything `ValidateBuiltGroom`
rejects (array sizes that disagree, clusters that do not partition the strands, strands that run past the points,
guides outside the guide-capable set, parents outside the level below). A failed load leaves the groom empty.

## LOD invariants

The selection rule is `HairLodStrandCount` in [`HairLod.h`](../Plugins/Hair/include/Hair/HairLod.h): at detail level
`l` in (0, 1] a cluster of `N` strands with offset `d` draws `ceil(l * (N + d))` strands, clamped to `[1, N]`, and
they are the first that many of the cluster. The rule itself, and the others a view needs (the detail level from strand
spacing, the boundary strand's weight, the width scale, the vertex level and its geomorph), are in
[`HairLodShared.h`](../Plugins/Hair/include/Hair/HairLodShared.h), which compiles as C++ and as HLSL: the select shader
includes the same file, so the two cannot drift. [Selection](#selection) says how the runtime uses them.

The tests are in [`GroomBuilderTests.cpp`](../Plugins/Hair/tests/GroomBuilderTests.cpp) unless noted;
`CheckBuiltGroom` runs on every groom built there.

| Invariant | Proved by |
|---|---|
| Rank is a permutation of the strands. | `CheckBuiltGroom` |
| Any rank prefix covers the roots: for a prefix of `m` strands (16, 64, 256, 1024, 4096, while the groom has at least twice as many), no root is further than `kPrefixCoverageFactor` (1.25) times `sqrt(rootArea / m)` from a kept root. Promised only when the source flags no guides. | `CheckBuiltGroom`, from `TestBuildAllStyles` |
| Flagged source guides take the lowest ranks. | `CheckBuiltGroom`; `TestAuthoredGuides`; `TestImportedGroomBuilds` in [`AlembicGroomTests.cpp`](../Plugins/Hair/tests/AlembicGroomTests.cpp) |
| Clusters partition the strands, hold 1 to 64 each, and are sorted by rank. | `CheckBuiltGroom`, `ValidateBuiltGroom` |
| The drawn strand count never falls as `l` rises, per cluster and in total, is at least 1, and is all strands at `l = 1`. | `TestLodHelper`, `CheckBuiltGroom` |
| The power form of the detail level (`HairLodDetailLevelPower`) is the spacing rule at exponent 1, never above it at a higher exponent, never below the floor, and still 1 from the same spacing up. | `TestSharedLodRules` |
| The shared rule (`HairLodStrandCountShared`) is `HairLodStrandCount` and is the formula above. Whole strands plus the boundary strand's weight never fall and never jump as `l` rises; the weight is in (0, 1] and is 1 at `l = 1`. The drawn vertex level is at most the stored one, and the drawn level less the geomorph is the fractional level. | `TestSharedLodRules` |
| The strands drawn at a lower `l` are a subset of those at a higher `l`, and are always the cluster's lowest ranks. | `CheckBuiltGroom` (rank-sorted clusters plus the prefix rule) |
| Every strand of a cluster has `2^k + 1` points with `k` inside the settings' range, packed back to back. | `CheckBuiltGroom` |
| Guide levels end at `guideLevelBase * 4^j`; every parent is in the level directly below; level 0 has none; parent weights sum to 1. | `CheckBuiltGroom`; `ValidateBuiltGroom` enforces the level rule on load (`TestGroomFileRejections`) |
| Every strand's guides are in the guide-capable set and its weights sum to 1; a guide-capable strand binds only to itself. | `CheckBuiltGroom` |
| Authored closest guides are kept exactly when all of a strand's guides are guide-capable, with normalized weights. | `TestAuthoredGuides`; `TestImportedGroomBuilds` for a groom that came through Alembic |
| Cluster box, cluster sphere and groom bounds contain every decoded point. | `CheckBuiltGroom` |
| A decoded point is within half a quantization step per axis of the resampled point. Built strands stay within a stated distance of the source: 0.01 mm for straight strands, 2 mm for the coarse curved fixtures, 0.5 mm for densely sampled ones. | `CheckBuiltGroom`, `TestResampleConverges` |
| Building is deterministic, and save, load, save reproduces the file byte for byte. | `TestBuildAllStyles`, `CheckRoundTrip` |

Vertex LOD (dropping from `2^k + 1` to `2^(k-1) + 1` points with a geomorph) is runtime work on this data
([Evaluation](#evaluation-and-the-strand-pool)): the built data guarantees the point count that makes it possible. The
runtime rules are checked on the GPU by the [scenario](#checking-it), not by `HairCoreTests`.

## Runtime

`HairPlugin` is three pieces, all in namespace `br::hair`:

| Piece | File | Role |
|---|---|---|
| `HairRuntime` | [`HairRuntime.h`](../Plugins/Hair/include/Hair/HairRuntime.h) | Owns the groom assets, the instances and the GPU buffers. One per application, shared by every frame graph. |
| `HairExtension` | [`HairExtension.h`](../Plugins/Hair/include/Hair/HairExtension.h) | The `org::IRenderGraphExtension` that adds the hair passes to a frame graph ([Passes](#passes)). |
| `RegisterHairCommands` | [`HairCommands.h`](../Plugins/Hair/include/Hair/HairCommands.h) | The control commands, for the control pipe and C# scenarios. |

A host creates the runtime, adds the extension to its recipe in the Visibility epoch, and registers the commands. The
demo does this in `BasicRenderer.cpp`:

```cpp
const auto hairRuntime = std::make_shared<br::hair::HairRuntime>();
recipe.AddExtension("Hair", [hairRuntime] { return std::make_unique<br::hair::HairExtension>(hairRuntime); },
    br::render::kVisibilityEpoch);
renderer.Initialize(hwnd, width, height, std::move(recipe));
hairRuntime->Initialize(renderer);                      // the scene hook
br::hair::RegisterHairCommands(commands, hairRuntime);
// ... and hairRuntime->Shutdown() before renderer.Cleanup().
```

**Raster only.** The path-traced recipe drops every recipe extension (`MakePathTracedRecipe`), so in path tracing
mode the hair passes do not exist and `hair.stats` shows `frames` standing still. Path tracing is the default where
the device supports it: start with `SARP_RT_PATH_TRACING=0`, or turn the `enablePathTracing` setting off.

### Commands

A failed command answers `"ok": false` with the reason. There are no hair settings: the renderer has no public way
for a plugin to register one, so the LOD controls are a command too.

| Command | Fields | Reply |
|---|---|---|
| `hair.load` | `path` (a `.groom`, `.hair` or `.abc` file), or `procedural`: `{style, strands, seed, points, length, radius, center, axis, cap_angle_degrees, root_width, tip_width, wave_amplitude, wave_count, droop, down}`. For `.hair` and `.abc`: `unit_scale`, `up_axis` (`"y"` or `"z"`), `offset` (`[x, y, z]`). For everything but a `.groom`: `max_guides`, `max_vertex_level` (the builder's `maxGuideCount` and `maxVertexLevel`). | `asset` |
| `hair.attach` | `asset`, `entity` (a scene entity's name), optional `bone`, and the material: `color` (`[r, g, b]`, linear) and `roughness` for the surface path; for the layered path `preset`, `melanin`, `redness`, `hair_color`, `sigma_a`, `beta_m`, `beta_n`, `alpha_degrees`, `eta`, `melanin_variation`, `roughness_variation` ([Material parameters](#material-parameters)) | `instance` |
| `hair.material` | `instance`, and any of `hair.attach`'s material fields: only those given change | `instance`, `material` |
| `hair.clear` | | Drops every instance and asset. |
| `hair.selfcheck` | `enabled` (default true), `roots` per instance (default 8) | `enabled` |
| `hair.lod` | Any of `error_pixels`, `max_width_scale`, `min_detail`, `force_detail`, `segment_pixels`, `freeze`, `detail_exponent`, `coverage` (`off`, `white` or `interleaved`; `true` and `false` are `white` and `off`), `coverage_accumulate` | Every value. See [Selection](#selection) and [The rasterizer](#the-rasterizer). |
| `hair.render` | Any of `path` (`surface` or `layered`), `depth_alpha`, `coverage_saturation`, `reactive`, `reactive_strength`, `reactive_pixels`, `ambient`, `lights` (`list` or `grid`), and the layered path's rasterizer: `raster` (`auto`, `direct` or `culled`), `auto_width_pixels`, `first_pass` (`previous`, `prefix`, `both` or `all`), `opaque_coverage`, `occlude_opaque`, `occlude_saturated`, `saturation`, `hidden_detail`, `detail_rate_up`, `detail_rate_down`, `hint_decay`, `raster_profile`, `late_pass` | Every value. See [The layered path](#the-layered-path) and [The culled rasterizer](#the-culled-rasterizer). |
| `hair.volume` | Any of `enabled`, `dual_scattering`, `voxel_pixels`, `max_resolution`, `voxel_budget`, `steps`, `environment_steps`, `bias`, `density_factor`, `margin`, `scalp_depth`, `splat` (`trilinear` or `nearest`), `jitter` (`vertex` or `frame`) | Every value, and the last frame's volumes. See [The density volume](#the-density-volume). |
| `hair.shadows` | Any of `enabled`, `detail_scale`, `force_detail`, `coverage` (`temporal`, `fixed` or `opaque`), `receiver_bias`, `dynamic_radius` | Every value, and the last frame's shadow casting on the CPU. See [Shadow casting](#shadow-casting). |
| `hair.debug` | `view`: `off`, `cluster`, `detail`, `strand`, `rank`, `vertex_level`, `coverage`, `layers`, `alpha`, `reactive`, `lobe_r`, `lobe_tt`, `lobe_trt`, `environment`, `direct`, `density`, `transmittance`, `fibre_count`, `scatter_global`, `scatter_local` or `vertex_density` | `view`. See [Debug view](#debug-view). |
| `hair.capture` | `path` (a `.bmp` file to write) | `state`: `pending`. `hair.stats` reports when it is written. |
| `hair.probe` | `path` (a binary file to write) | `state`: `pending`. See [Probing the targets](#probing-the-targets). |
| `hair.sim` | Any of `enabled`, `gravity` (`[x, y, z]`), `damping`, `shape_compliance`, `bend_compliance`, `stretch_compliance`, `step_seconds`, `substeps`, `max_steps`, `wind` (`[x, y, z]`), `gust_amplitude`, `gust_frequency`, `friction`, `collision_margin`, `teleport_distance`, `frame_seconds`, `lod_full_coverage`, `lod_level0_coverage`, `lod_off_coverage`, `lod_rate`, `lod_force_level`, `lod_point_budget`, `promote_seconds`, `demote_seconds`, `lod_view_grace_seconds`, `lod_settle_seconds`, `guide_point_budget`, `guide_keep_seconds`; `reset: true` | Every control, the step counters and each instance's last metrics. See [Simulation](#simulation), [Simulation LOD](#simulation-lod) and [Guide rows](#guide-rows). |
| `hair.simlevel` | `instance`, `level` | `instance`, `level`. Forces one instance's simulation level; a negative level gives it back to `hair.sim`'s rule. For testing. |
| `hair.colliders` | `instance`, `colliders`: up to 8 of `{center, radius}` (a sphere) or `{a, b, radius}` (a capsule), in the entity's mesh space | `instance`, `colliders` (the count). An empty list removes them. |
| `hair.dump` | `buffer` (`pool_vertices`, `visible_clusters`, `guide_points`, `instances`, `clusters`, `strands` or `guide_bindings`), `path`, optional `rows` | `state`: `pending`. See [Dumping buffers](#dumping-buffers). |
| `hair.stats` | `continuous` (optional): read the GPU counters and the simulation's metrics back every frame | See [Checking it](#checking-it). |

- **Sources.** A `.groom` is loaded as it is. Everything else is built in memory with `BuildGroom` and default
  settings (but for `max_guides` and `max_vertex_level`, when given), with no scalp mesh, on the calling thread:
  nothing is cached on disk.
- **Procedural** is `GenerateGroomOnSphereCap`. `style` is `straight`, `wavy` or `curly`; the other fields default to
  `ProceduralGroomSettings`' values (metres, a 10 cm sphere at the origin, the upper hemisphere, no droop). `droop`
  around 3 to 6 makes strands hang toward `down` (default `[0, -1, 0]`).
- **`.hair` and `.abc`** are converted to metres with `unit_scale` and, when `up_axis` is `z`, rotated onto +Y up,
  `(x, y, z) -> (x, z, -y)`; `offset` (metres) is then added to every position. `.abc` defaults to 0.01 and `y`, the
  UE groom schema's centimetres. `.hair` defaults to `z` and 0.003: the files name no unit, and at that scale the head
  under Cem Yuksel's `wStraight` and `wCurly` is 18 cm across (their roots lie on a sphere of radius 30 file units
  around (-11.3, 0, 29.5), the face along +x).
- **Reference models.** `wStraight.hair` (50,000 strands of 25 points) builds to 608,992 points and `wCurly.hair`
  (50,000 strands of 57 to 73 points) to 1,650,000, each in 1,024 clusters. The builder's default of at most 33 points
  a strand is fewer than `wCurly`'s curls have, and up close they are visibly polygonal.
- **Placing a file groom.** A groom's rest space is its entity's mesh space, so the entity's scale applies to it, and
  the file's origin is rarely where the entity's is: pass `unit_scale` divided by the entity's scale, and an `offset`
  that brings the head's centre to the mesh's.
- **Material.** Each path has its own. `color` and `roughness` are the surface path's placeholder
  ([the resolve](#the-resolve)): by default a dark brown, `[0.09, 0.05, 0.03]`, at roughness 0.6. The layered path takes
  the fibre's scattering parameters ([Material parameters](#material-parameters)): by default brown hair. `hair.material`
  changes an instance's material afterwards. A `.hair` file's colours are not used.
- **Assets and instances are not freed one at a time.** They accumulate until `hair.clear`. What an instance costs
  beyond its 96-byte row is guide state, which it borrows while it is simulated ([Guide rows](#guide-rows)).

### Attachment

An instance is one asset on one scene entity. A groom's rest space is the entity's mesh space: a root at rest
position `p` is drawn where the mesh's vertex at `p` would be.

`hair.attach` names the entity by its `Components::Name`. The instance starts `pending`; the next scene update finds
the entity (the entity itself when it has meshes, otherwise its first child that has: a model's root names its first
part) and the instance becomes `attached`, or `failed` with the reason (`hair.stats` lists both). It then follows
that entity in one of three modes:

| Mode | When | A root follows |
|---|---|---|
| `rigid` | No `bone`, and the entity has no skeleton or the groom has no skin bindings. | The entity's model matrix. |
| `bone` | `bone` names a joint of the entity's skeleton. | That joint's skin matrix, then the model matrix. The glTF importer names a bone `<node name>#node_<index>`; the node name alone is accepted. |
| `per_root` | No `bone`, the entity is skinned and the groom was built with a scalp mesh. | The blend of the root's own four joints and weights (`HairRootBinding`), then the model matrix. |

An instance whose entity the renderer does not draw (not loaded yet, destroyed) is skipped that frame.

**No scalp mesh at attach time.** `BuildGroom` can transfer skin weights from a `ScalpMesh`, but the runtime cannot
give it one: a renderer `Mesh` keeps no CPU vertices or skin weights after upload (the importers pass
`MeshCpuDataPolicy::ReleaseAfterUpload`; what stays is cluster LOD pages and one vertex's debug sample). So a groom
built at load time attaches rigidly or to one bone. `per_root` needs a `.groom` built offline with the scalp, and
nothing in the tree writes one outside the tests. Giving the runtime a scalp would take an importer path that keeps a
mesh's positions, indices, joints and weights (or decodes them from the cluster LOD cache) and hands them to
`BuildGroom` at attach.

### GPU data

The runtime makes thirty-five pooled buffers when the first groom is loaded, at fixed sizes (`HairCapacities`, 568 MB
at the defaults), and never resizes them. A groom that does not fit the asset buffers, or an instance past the
instance rows, is refused with an error that names the row count and the capacity. Three kinds of buffer are shared
out at run time and can run short without anything being refused: `Hair.VisibleClusters` and the two pool buffers hold
one frame's selection and are filled on the GPU (what does not fit is dropped for the frame and counted,
[Selection](#selection)), and `Hair.GuidePoints` is lent to the instances that are simulated (an instance that gets
none follows its roots rigidly and is counted, [Guide rows](#guide-rows)). `Hair.VolumeVoxels` is shared out anew every
frame among the grooms a view shows (when they do not go round, every volume is made coarser,
[The density volume](#the-density-volume)).

| Buffer | Row | Default rows | Written |
|---|---|---|---|
| `Hair.Clusters`, `Hair.ClusterAggregates` | `HairClusterHeader`, `HairClusterAggregate` | 16,384 | once per asset |
| `Hair.Strands`, `Hair.RootBindings` | `HairStrandRecord`, `HairRootBinding` | 262,144 | once per asset |
| `Hair.GuideBindings`, `Hair.GuideStrandIndices` | `HairGuideBinding`, `uint` | 65,536 | once per asset |
| `Hair.Points` | `HairPackedPoint` | 8,388,608 | once per asset |
| `Hair.Instances` | `HairInstanceGPU` | 1,024 | when an instance's row changes |
| `Hair.FrameState` | `HairFrameGPU` | 1 | every frame with an active instance |
| `Hair.Counters` | `uint` | 84 | by the select, evaluate, raster, shade-list, resolve, shade and composite passes and the three shadow passes; reset by an upload every frame |
| `Hair.VisibleClusters` | `HairVisibleCluster` (48 B) | 65,535 | by the select pass |
| `Hair.PoolVertices` | `HairPoolVertex` | 4,194,304 | by the evaluate pass |
| `Hair.PoolSegmentIds` | `uint` | 4,194,304 | by the evaluate pass |
| `Hair.PoolColors` | `uint2` (three half floats) | 4,194,304 | on the layered path: the rasterizer marks the rows to light, the shade pass writes their lit colours |
| `Hair.ShadeList` | `uint` | 4,194,304 | on the layered path, by the shade-list pass: the pool rows the rasterizer marked this frame |
| `Hair.Probe` | `uint` | 1,310,792 | by the probe pass, in a frame `hair.probe` asked for |
| `Hair.RasterItems` | `uint` | 8,388,608 | by the [culled rasterizer](#the-culled-rasterizer)'s classify pass: the chunks its first pass draws |
| `Hair.PoolLate` | `uint` | 4,194,304 | by the same pass: each pool row's chunks left for the late pass |
| `Hair.RasterArgs` | `uint` | 4 | by the same pass: the first pass's dispatch size |
| `Hair.ClusterState` | `uint` | 1,048,576 (4 per cluster of every instance, 262,144 clusters) | by the culled rasterizer and the select pass: when each cluster last drew a pixel, and its detail scale |
| `Hair.Volumes` | `HairVolumeGPU` | 1,024 | on the layered path with the density volume on: every frame the volumes change, a row per instance row |
| `Hair.VolumeVoxels` | `uint` | 2,097,152 | by the voxelize pass: the voxels of the frame's volumes |
| `Hair.ScatterTables` | `HairScatterRow` | 16,384 (16 per instance) | when an instance's material changes |
| `Hair.ShadowClusters`, `Hair.ShadowClusterMasks` | `HairVisibleCluster`, `uint` | 65,535 | by the shadow select pass, in a frame that draws hair into the shadow map: the clusters the shadow view keeps and the clipmaps each is drawn into |
| `Hair.ShadowPoolVertices`, `Hair.ShadowPoolSegmentIds` | `HairPoolVertex`, `uint` | 1,048,576 | by the shadow evaluate pass: the shadow view's strands |
| `Hair.GuidePointRanges` | `HairGuidePointRange` | 65,536 | once per asset |
| `Hair.GuidePoints` | `HairGuidePoint` | 2,097,152 | by the simulate and guide-level passes: the rows of the instances that are simulated |
| `Hair.SimGroups` | `HairSimGroup` | 131,072 | every frame an instance is simulated: the thread groups of the simulate and guide-level passes |
| `Hair.Colliders` | `HairColliderGPU` | 8,192 (8 per instance) | when an instance's colliders change |
| `Hair.SimFrame` | `HairSimFrameGPU` | 1 | every frame |
| `Hair.SimMetrics` | `uint` | 32,768 (32 per instance) | by the simulate, guide-level and evaluate passes; reset by an upload every frame the simulation is on |
| `Hair.RootCheckList`, `Hair.RootCheck` | `HairRootCheckSample`, `HairRootFramePair` | 4,096 | the self-check's: an upload and the root-check pass, in a frame that samples |

There are also two textures, made at the render resolution of the first frame and made again when that changes:
`Hair.Visibility`, the [hair visibility target](#the-hair-visibility-target), and `Hair.Coverage`, the
[coverage target](#the-coverage-target). The [layered path](#the-layered-path) adds four of the same size the first
time it is turned on, and keeps them: `Hair.Layer1` and `Hair.Layer2` (a pixel's second and third layers, in the
visibility target's format), `Hair.CompositeInfo` (`R8G8_UNorm`: the composited hair alpha and a few bits about the
pixel) and `Hair.ReactiveMask` (`R8_UNorm`: what the upscaler is given). At 2560x1600 the first two are 31.3 and 15.6 MB
and the layered path's four 74.2 MB (31.3, 31.3, 7.8 and 3.9). With them come the
[culled rasterizer](#the-culled-rasterizer)'s six, 63 MB more: `Hair.CoverageFar` (`R32_UInt`, 15.6 MB), `Hair.Occlusion`
(`R32G32_UInt`, 31.3 MB), `Hair.SplitGuess` (`R32_UInt`, 15.6 MB), and `Hair.OcclusionCoarse`, `Hair.OcclusionHint` and
`Hair.TileFlags` (`R32_UInt`, a texel per 8x8 pixels, 0.25 MB each). `hair.stats` lists them (`targets`).

Where the 568 MB are: guide state 160 MB, the strand pool 128 MB, its segment ids 16 MB, its colours 32 MB and the
shade list 16 MB, the culled rasterizer's item list 32 MB, late masks 16 MB and cluster state 4 MB, the groom points 64 MB, the shadow view's pool, ids and list 38 MB, strands and root bindings 32 MB,
the density volumes' voxels 8 MB, and 1 MB to 5 MB each for the rest. `hair.stats` reports every buffer's capacity,
what is in use and the most that was ([Checking it](#checking-it)).

- Assets sit back to back in the groom buffers. Indices inside an asset stay groom-relative (`strandOffset`,
  `pointOffset`, guide strand indices): a shader adds the instance's bases.
- [`HairInstanceGPU`](../Plugins/Hair/include/Hair/HairRuntimeGPUTypes.h) holds an instance's bases and counts and
  three references into the renderer's own buffers:
  its entity's row of `Builtin::PerObjectBuffer` (model and previous model matrix), its skinning instance slot
  (`SkinningInstanceInfo`, which locates the current and previous bone palettes in `BoneTransforms`) and the attach
  bone. The entity's transform is not copied: the shader reads the row the entity's mesh is drawn with. It also holds
  the instance's materials (the surface path's base colour and roughness, the layered path's fibre parameters and
  per-strand variation: [Material parameters](#material-parameters)), its first row of guide state
  (`HAIR_INVALID_INDEX` while it holds none), its colliders' rows,
  the simulation flags (`HAIR_SIM_FLAG_ENABLED`, `HAIR_SIM_FLAG_RESET`, `HAIR_SIM_FLAG_KEEP_SHAPE`,
  `HAIR_SIM_FLAG_UNSEEN`) and its simulation level: every shader reads whether the instance is simulated from its row.
- **An instance owns nothing but that row.** No buffer holds root frames: every pass works out the ones it needs
  ([Root frames](#root-frames)). Guide state is borrowed ([Guide rows](#guide-rows)).
- A guide keeps its cluster's stored vertex level, so guides differ in point count; `Hair.GuidePointRanges` gives each
  guide's first row and point count, by rank, relative to its instance's first row. A guide's first row also holds its
  simulation LOD state (`guideState`, `guideBlend`), and its second the stamp of the frame its rows were last written
  in and the instance they were written for (`HAIR_GUIDE_STAMP`).
- All uploads go through one hair pass's own upload on the render thread
  (`RenderGraphPass::UploadBufferData`), in the root-check pass's `Update`. Nothing uses a deferred resize: no buffer
  is ever resized or made again (the two textures are, with the render resolution). That `Update`
  (`HairRuntime::UpdateFrame`) is the plugin's CPU work for the frame: it also uploads the LOD controls
  (`HairFrameGPU`), zeroes the frame's counters, lends and takes back guide rows and decides the frame's plan
  (`HairFramePlan`), which every hair pass records from.
- `FindSceneEntityRenderState` ([`Extensions/SceneEntityRenderState.h`](../BasicRenderer/include/BasicRenderer/Extensions/SceneEntityRenderState.h))
  is the renderer extension point this needed: it gives a scene entity's per-object row and skinning slot. The
  per-object buffer is versioned, and a frame can bind a version a few frames behind the scene, so the pass takes the
  frame's published version (`PublishedObjectBufferState`) and skips an instance whose row that version does not
  have yet.

### Guide rows

Guide state (`HairGuidePoint`, 80 bytes a guide point) is the one per-instance thing of any size: 2.6 MB for a groom of
1,024 guides at 33 points, 10.3 MB for one of 4,096. It is lent out of `Hair.GuidePoints`, a contiguous range per
instance, as many rows as its groom has guide points (every guide of a simulated instance has live rows, whichever of
them the solver owns: [Simulation LOD](#simulation-lod)). The lending is the CPU's, once a frame, with the simulation's
planning (`PlanSimLevels`, `AcquireGuideRows` in [`HairRuntime.cpp`](../Plugins/Hair/src/HairRuntime.cpp)):

- **Taking.** An instance takes rows in the frame it starts being simulated, in view or to settle unseen. Its first
  simulated frame writes every row (it starts from its rigid pose), so what the rows held before does not matter.
- **Holding.** It holds them while it is simulated, and after it stops for being out of view: the rows are then the
  shape it kept, which is what lets it go on seamlessly when a view shows it again
  ([Following the view](#following-the-view)).
- **Giving back, nothing lost.** At once when it stops with nothing to keep: its level went to 0 and it blended back
  onto its rigid pose, the simulation was turned off, a reset while it was stopped, `hair.clear`. And when it holds a
  kept shape but is now too small on screen to be simulated at all (its coverage asks for less than the level a groom
  starts at): it would be drawn rigid if a view turned to it.
- **Taken back, something lost (an eviction).** When an instance *in view* needs rows and none are free, or the budget
  is reached, rows are taken from the instances no view shows, the smallest on screen first (by coverage: the size it
  would have in view), whether it keeps a shape or is being simulated out of view (settling unseen, or in the quarter
  second after leaving). With `guide_keep_seconds` above 0, a kept shape is also taken back once its instance has not
  been simulated for that long. An evicted instance is not settled unseen again; it starts from its rigid pose when a
  view next shows it, and is seen settling.
- **Never between instances in view.** An instance in view keeps its rows. When the instances in view need more rows
  than there are, the ones that asked last get none (within a frame the largest on screen ask first): they are
  **starved**, follow their roots rigidly exactly as with the simulation off, ask again every frame, and start from
  their rigid pose when rows come free. Nothing reads rows it was not lent.
- **Settling unseen takes free rows only.** A groom that would settle out of view does not evict anything; without
  free rows it is left unsettled and starts from its rigid pose if a view turns to it.
- **Lowering the budget** below what is lent out takes rows back at once in the same order, and after the instances
  out of view from the smallest in view, which then starve.

| `hair.sim` field | Default | Meaning |
|---|---|---|
| `guide_point_budget` | 0 | The most rows lent out at once. 0: the buffer's capacity. |
| `guide_keep_seconds` | 0 | Above 0: a kept shape is given up after this many simulated seconds without its instance being simulated. 0: it is kept until an instance in view needs the rows. |

**Why kept shapes are not given up by age unless asked.** The buffer has a fixed size, so rows given back return no
memory to anyone; they only make the next entry of that groom a visible settle from its rigid pose. Kept until the
rows are needed, shapes are a cache that fills the buffer. The age limit is for a camera that travels through more
grooms than the buffer holds: it frees the shapes left behind for grooms ahead to settle in unseen
([On-demand memory measurements](#on-demand-memory-measurements) has both).

**Why the smallest and not the oldest.** Taking the shape simulated longest ago was built first. With more grooms near
enough to simulate than the buffer holds, a camera turning in place comes to them in turn, and the one simulated longest
ago is the one it shows next: every entry was a start from the rigid pose, each of those is simulated for
`lod_settle_seconds` whether or not it is still in view, and those starts evicted each other (measured below: 73
grooms simulated for 21 in view, 1,468 evictions in a few seconds). By size, the same turn keeps the shapes of the
large grooms and trades rows among the smallest.

**The allocator** is first fit over a sorted list of free ranges, merged when ranges are freed. Ranges never move. A
request can therefore fail with enough rows free in total but in pieces; it then evicts as if there were not enough,
and an instance in view can starve on fragmentation alone. Grooms come in a few sizes (one per asset), so freed ranges
are mostly reused whole.

**Checked on the GPU.** The simulate and guide-level passes stamp row 1 of every guide they write with the instance
and the frame (`HAIR_GUIDE_STAMP`). The evaluate pass counts the drawn strands of simulated instances that read a guide
whose stamp is not this frame's and this instance's, or whose instance has no rows (`stale_guides_drawn`). The
simulate and guide-level passes count the guides that go on from the state their rows hold (every guide but one that
starts or is reset) and do not find there the stamp of this instance and the frame before (`stale_guides` in the
simulation's metrics). Both are 0 unless the lending and the passes disagree: rows read after they were given to
another groom, rows never written, a guide skipped by both passes, an instance that changed rows without starting
again. A pass does nothing with an instance flagged simulated that has no rows.

`hair.stats` reports the lending under `memory.guide_state` ([Checking it](#checking-it)).

**What the capacity admits.** 2,097,152 rows are 62 grooms of 1,024 guides at 33 points, or 15 of the builder's
default 4,096 guides, simulated or kept at once. `HairCapacities` is a constructor argument; the buffer is not grown at
run time. Growing it would mean a new buffer and a copy of the live rows on the render thread's upload path, with every
pass declaring again, as the visibility target is replaced; it was not built.

### Passes

| Pass | Where | Shader |
|---|---|---|
| `Hair::Select` | Before `CLodOpaque::HierarchicalCullingPass1` | `HairSelectCS` in [`hairSelectEvaluate.hlsl`](../Plugins/Hair/shaders/hairSelectEvaluate.hlsl) |
| `Hair::RootCheck` | After it | [`hairRootCheck.hlsl`](../Plugins/Hair/shaders/hairRootCheck.hlsl); it records only in a frame the self-check samples |
| `Hair::Simulate` | After it | [`hairSimulate.hlsl`](../Plugins/Hair/shaders/hairSimulate.hlsl) |
| `Hair::GuideLevels` | After it | [`hairGuideLevels.hlsl`](../Plugins/Hair/shaders/hairGuideLevels.hlsl) |
| `Hair::Evaluate` | After it | `HairEvaluateCS` in `hairSelectEvaluate.hlsl` |
| `Hair::Voxelize` | After `Hair::Evaluate`, before `CLodOpaque::HierarchicalCullingPass1` | `HairVolumeClearCS` and `HairVoxelizeCS` in [`hairVolume.hlsl`](../Plugins/Hair/shaders/hairVolume.hlsl); the layered path only, in a frame that has density volumes |
| `Hair::ShadowSelect`, `Hair::ShadowEvaluate`, `Hair::ShadowRaster` | In the shadow map's chain, where it gathers its caster providers' raster passes (before its `VirtualShadowFinalizeFallbackPagesPass`); only in a graph with a virtual shadow map | `HairShadowSelectCS` and `HairShadowRasterCS` in [`hairShadow.hlsl`](../Plugins/Hair/shaders/hairShadow.hlsl), `HairEvaluateCS` between them; the layered path only, with `hair.shadows` on |
| `Hair::Classify` | After `MaterialHistogramPass`, before `Hair::Raster` | `HairClassifyCS`, `HairRasterArgsCS` and `HairOcclusionInitCS` in [`hairRasterCulled.hlsl`](../Plugins/Hair/shaders/hairRasterCulled.hlsl); only in a frame the [culled rasterizer](#the-culled-rasterizer) draws |
| `Hair::Raster` | After `MaterialHistogramPass`, before `EvaluateMaterialGroupsPass` | [`hairRaster.hlsl`](../Plugins/Hair/shaders/hairRaster.hlsl), or in a frame the culled rasterizer draws `HairRasterItemsCS`, `HairOcclusionUpdateCS`, `HairOcclusionHintCS` and `HairRasterLateCS` in `hairRasterCulled.hlsl` |
| `Hair::Resolve` | After `EvaluateMaterialGroupsPass` and `CLodOpaque::LinearDepthCopyPass2`, before `CLodOpaque::LinearDepthDownsamplePass2` | `HairResolveCS` in [`hairResolve.hlsl`](../Plugins/Hair/shaders/hairResolve.hlsl); the surface path only |
| `Hair::Probe` | After `Hair::Resolve` | `HairProbeCS`, same file; the surface path only |
| `Hair::ShadeList` | After `Hair::Raster`, before `EvaluateMaterialGroupsPass` | [`hairShadeList.hlsl`](../Plugins/Hair/shaders/hairShadeList.hlsl); the layered path only |
| `Hair::Shade` | Shading epoch, after `DeferredShadingPass` and the reflection passes, before `Hair::Composite` | [`hairShade.hlsl`](../Plugins/Hair/shaders/hairShade.hlsl); the layered path only |
| `Hair::Composite` | Shading epoch, after `DeferredShadingPass` and the reflection passes | [`hairComposite.hlsl`](../Plugins/Hair/shaders/hairComposite.hlsl); the layered path only |
| `Hair::CompositeProbe` | After `Hair::Composite` | `HairProbeCS`; the layered path only |
| `Hair::DebugOverlay` | Overlay epoch, after `TonemappingPass`, before the present pass | [`hairDebugOverlay.hlsl`](../Plugins/Hair/shaders/hairDebugOverlay.hlsl) |

- The first five need the entities' transforms and the main view's camera and nothing of the frame's opaque geometry,
  so they run before culling. Evaluation is last of them: it draws the strands selection chose from the guide state the
  two simulation passes leave. `Hair::RootCheck` is where the plugin's CPU work for the frame runs
  ([GPU data](#gpu-data)); on the GPU it does something only for the [self-check](#checking-it).
- The rasterizer tests against the opaque visibility buffer, so it runs once that is complete. Every pass that writes
  the buffer is placed before `MaterialHistogramPass`, the first pass that reads the finished buffer. "After
  `MaterialHistogramPass`" is therefore after all of them, whatever the CLod configuration, without naming them.
- It runs before `EvaluateMaterialGroupsPass` because the resolve follows that pass and reads the target.
- The resolve overwrites what two renderer passes wrote at hair pixels, so it follows both: material evaluation (the
  canonical surface) and the frame's last depth copy (`CLodOpaque::LinearDepthCopyPass2`, which derives the depth
  targets from the opaque visibility buffer). It runs before `CLodOpaque::LinearDepthDownsamplePass2` builds the linear
  depth's mips from mip 0, and that puts it before everything that reads the frame's depth: virtual shadow page
  marking, ambient occlusion, light culling, deferred shading and the upscaler. A CLod configuration without the second
  depth copy does not have these anchors, and the resolve's place among the depth passes is then not pinned.
- The overlay is a debug view, so it is forwarded to the Overlay epoch (`ExternalPassDesc::Epoch`), as procedural
  wind's skeleton view is.
- The layered path's passes are forwarded to the Shading epoch. They need the lit opaque frame (the deferred pass also
  draws the sky) with its reflections added, the light buffers and the virtual shadow map's pages, and they must run
  before the Post epoch's motion vector dilation and upscaler, which read what the composite writes. The anchors are
  `DeferredShadingPass` and, when the reflection settings are on, the screen-space or ray traced reflection passes and
  their composite. The renderer's own transparents name `SkyboxPass` and the screen-space reflection passes as theirs
  (`MakeTransparentCompositeInsertPoint` in `CLodAlphaVariant.cpp`); the first no longer exists, and the demo's recipe
  has neither. The order of the hair composite against those other transparents is not pinned.
- Both paths' passes are in every graph. Which of them record is decided per frame from the plan
  (`HairFramePlan::renderPath`), so `hair.render` switches path without a graph rebuild.
- The three shadow passes are not placed by the extension: the shadow map asks its caster providers for them
  (`IVirtualShadowCasterProvider::GatherVirtualShadowRasterPasses`) and chains them after its own page management
  ([Shadow casting](#shadow-casting)). They read the guide state the simulation passes left and write nothing the
  main view's passes read. Their place against `Hair::Simulate` and `Hair::GuideLevels` is not pinned by a
  constraint; `shadow_stale_guides_drawn` counts a shadow strand that read guide rows not written in its frame.

Each pass declares the plugin's buffers, which appear with the first groom, and declares again when the visibility
target is replaced. Until then it records nothing.

### Root frames

A root's frame is its position, normal and tangent **in world space**, for this frame and for the previous frame's
matrices (the previous bone palette and the previous model matrix):

```
position = restPosition * skin * model          (row vectors, as the renderer skins a vertex)
normal   = normalize(restNormal * skin3x3 * model3x3), tangent likewise
```

`skin` is the identity in `rigid` mode. Skeleton LOD remaps (procedural wind's transient palettes) are not followed.
Normals use the matrices' upper 3x3 as they are, so a non-uniform scale skews them.

**No buffer holds root frames.** Every pass that needs one works it out, with the one function in
[`hairRootFrame.hlsli`](../Plugins/Hair/shaders/hairRootFrame.hlsli) (`HairMakeRootFramePair`): the select pass for a
cluster's first strand, the evaluate pass for each strand it draws, the simulate and guide-level passes for each guide
they write. Its inputs are the root's binding (`HairRootBinding`), the entity's row of the per-object buffer and, for a
skinned instance, the bone palettes: all the frame's own. So a root's frame is always this frame's and belongs to a root
the frame uses, by construction, and an instance needs no storage for it.

It used to be a pass and a buffer. First one dispatch per instance over every root, into a row per strand of every
instance (0.67 ms and 92 MB for the crowd's million roots, wherever the camera looked); then two dispatches over the
drawn strands and the simulated instances' guides, into the same rows, with each row stamped with its frame and the
readers counting stale ones. Working the frame out where it is read costs less than writing and reading the row
([On-demand memory measurements](#on-demand-memory-measurements)).

### Selection

`Hair::Select` dispatches once per active instance, one thread per cluster, for the main view only. A cluster:

1. **Follows its first strand's root.** Cluster bounds are in rest space. They are moved as the cluster's strands are
   ([Evaluation](#evaluation-and-the-strand-pool)), using the root frame of the cluster's first strand and the entity's
   uniform scale. That is exact while every root of the instance moves with one matrix (`rigid` and `bone`); with a
   skin binding per root it is approximate.
2. **Is frustum-tested** as a sphere against the camera's six view-space planes, like CLod's culling. Bounds cover
   strand centrelines at rest, so the radius is grown by 25% for strands that will move under simulation
   (`HAIR_CULL_RADIUS_MARGIN`) and by 1 cm for strand width (`HAIR_CULL_WIDTH_MARGIN`). There is no occlusion test.
3. **Gets its detail level** `l = clamp((spacingPixels / errorPixels) ^ detailExponent, minDetail, 1)`
   (`HairLodDetailLevelPower`; with the exponent at 1 it is `HairLodDetailLevel`). The spacing
   is `1 / sqrt(density)` from the cluster's aggregate (strands per cross-section area), or from the bounds when the
   aggregate has no area. It is projected as CLod projects geometric error: by view depth, not distance, at the near
   side of the bounds, with the same pixels-per-unit factor (`projection[1][1] * 0.5 * renderHeight`).
4. **Gets its drawn strand count** from `l` by the rule above, and the weight of its last drawn strand
   (`HairLodBoundaryWeight`): the fraction of a strand that `l * (N + d)` has over the strands before it.
   On the layered path, in a frame the [culled rasterizer](#the-culled-rasterizer) draws, `l` is then multiplied by the
   cluster's **detail scale** ([Hidden clusters](#hidden-clusters)), and kept at `min_detail` or above.
5. **Gets its drawn vertex level.** The fractional level is `f = log2(lengthPixels / segmentPixels)`, clamped to
   `[0, k]`, where the length is that of the aggregate's centreline. The drawn level is `ceil(f)` and the geomorph is
   `ceil(f) - f`. The centreline of a curly cluster is shorter than its strands, so curls drop levels earlier than their
   arc length would.
6. **Is appended** to the visible-cluster list and takes its range of the strand pool, both with an atomic counter.

A cluster that passes the frustum test also sets its instance's bit in the counter buffer, which is where
`instances_visible` comes from.

A `HairVisibleCluster` row holds the instance, the cluster, the drawn strand count, the pool range, `l`, the boundary
weight, the vertex level and the geomorph, and the cluster's row of the cluster state with whether it drew a pixel
lately ([Hidden clusters](#hidden-clusters)). Neither allocator gives anything back: when the pool or the list is full,
that cluster and every later one of the frame is dropped and counted in `overflow_clusters`.

The controls (`hair.lod`), with their defaults:

| Field | Default | Meaning |
|---|---|---|
| `error_pixels` | 1 | A cluster draws every strand while its strands are at least this far apart on screen. |
| `max_width_scale` | 64 | `wMax`: the most a drawn strand is widened. 64 is the most strands a cluster has, so a cluster down to its last strand still carries all its coverage. It was 8 before coverage, when the width did not show. |
| `min_detail` | 0.02 | The lowest `l`. |
| `force_detail` | 0 | Above 0, every cluster takes this `l`. For testing. |
| `segment_pixels` | 4 | The length on screen a drawn segment aims for. |
| `freeze` | false | Keep the last selection while the camera moves. The select pass does not run; the kept strands are still evaluated and rasterized from the current camera. A change in the active instances selects once more. |
| `detail_exponent` | 1 | The power the spacing rule is raised to, 1 to 3. At 1 the drawn strands fall as one over the distance; at 2 as its square, like the area the groom covers on screen. The kept strands' width scale makes up the coverage, so `max_width_scale` has to allow it. |
| `coverage` | `white` | How the rasterizer treats a strand thinner than its footprint: `off` writes every footprint pixel (a strand is at least a pixel wide and opaque, as before coverage existed); `white` and `interleaved` write a pixel with the probability of the strand's coverage there, and differ in the random pattern. See [The rasterizer](#the-rasterizer). |
| `coverage_accumulate` | false | Add every segment's coverage into the [coverage target](#the-coverage-target). |

### Evaluation and the strand pool

`Hair::Evaluate` runs one thread group per visible cluster, one thread per drawn strand. The CPU does not know how
many clusters are visible, so the dispatch covers every cluster of the active instances and the extra groups return at
once. A thread writes its strand's `2^level + 1` drawn vertices as `HairPoolVertex` rows: position and width, in world
space, for this frame's root frame and for the previous frame's.

Without simulation:

- **Rigid following.** A rest-space point keeps its offset from the strand's root, expressed in the root's frame
  (tangent, bitangent, normal): `current root position + R * (point - rest root position) * scale`. `R` takes the rest
  frame in `HairRootBinding` to the [root's frame](#root-frames); `scale` is the entity's uniform scale, taken from the
  model matrix because root frames are unit vectors. The previous position uses the previous root frame and the
  previous model matrix.
- **Vertex LOD.** Drawn vertex `j` is stored point `j << (k - level)`. Odd vertices are the ones the level below
  drops: they move to the midpoint of their two neighbours by the geomorph, and are gone when the level steps down.
- **Width.** The stored diameter times the width scale, and for the cluster's last drawn strand also times the
  boundary weight, so a strand grows from nothing as `l` rises. With coverage on the scale is the exact one,
  `min(N / clamp(l * (N + d), 1, N), wMax)` (`HairLodCoverageWidthScale`): the cluster's strands over the strands'
  worth it draws, so the drawn widths always sum to the cluster's own, down to its last strand. With `coverage` `off`
  it is `min(1 / l, wMax)` as before (`HairLodWidthScale`), which is the same thing but for the offset `d` and for a
  cluster already down to one strand; under the one-pixel floor neither showed.
- **Previous frame.** The previous position is this frame's selection at last frame's root. A strand that was not
  drawn last frame, or was drawn at another level, still gets one.

With simulation (the instance's `HAIR_SIM_FLAG_ENABLED`), positions come from the guides instead; widths, the drawn
level and the pool layout are the same:

- **A guide** is drawn from its own state: drawn vertex `j` is guide point `j << (k - level)`. The state is the
  solver's for a guide that is simulated and the guide-level pass's for one that is not
  ([Simulation LOD](#simulation-lod)); the evaluate pass does not tell them apart.
- **Every other strand** is skinned to its (up to) three guides, `HairStrandRecord::guideIndices` and `guideWeights`
  (linear hair skinning with a rotation). For a stored point at curve parameter `u = index / 2^k`:

  ```
  rigid    = the point following its own root rigidly (as above)
  for each guide g:  G = g's simulated curve at u,  Gr = g's rigid curve at u
                     R = the smallest rotation from Gr's tangent at u to G's tangent at u
                     p_g = G + R * (rigid - Gr)
  position = sum of weight_g * p_g, over the sum of the weights
  ```

  A guide is sampled **by curve parameter**, not by point index, because guides differ in point count: positions are
  interpolated between the two guide points around `u`, and the tangent between the central differences at those two
  points, so the rotation is continuous along the strand. A guide's rigid curve is its rest curve through its own root
  frame, which the simulate pass stores beside the simulated one (`HairGuidePoint::rigid`). While a guide is on its
  rigid pose the strand is exactly on its own.
- **Roots.** A drawn strand's first vertex is put exactly on its root frame's position, guide or not. How far the
  evaluation had left it from there is measured first (`guide_root_error`, `strand_root_error` in the simulation's
  metrics): it is the quantization of the stored root point, micrometres. Its previous position stays the evaluated
  one.
- **Vertex LOD.** The geomorph moves an odd vertex to the midpoint of its two neighbours' evaluated positions, so at 1
  it is on the segment the level below draws.
- **Previous frame.** The same evaluation with the guides' simulated and rigid positions of the previous frame
  (`previousPosition`, `previousRigid`) and the strand's previous root frame. So the previous position of a pool row is
  where that vertex was a frame ago, simulated motion included, and the resolve's motion vectors follow the hair.
- **Which guides.** Every guide of a simulated instance has live state, so a strand's guides always count. Which of
  them the solver owns is decided in one place, `HairGuideIsSimulated` in
  [`hairCommon.hlsli`](../Plugins/Hair/shaders/hairCommon.hlsli), and concerns the simulate and guide-level passes only.
- **Limits.** The rotation is the smallest one between two tangents, so it carries no twist: a strand whose guide hangs
  the opposite way from its rest direction keeps its offset turned about an arbitrary axis. Cluster bounds are still the
  rest pose's, grown by 25% ([Selection](#selection)): hair that the simulation moves further is culled early at the
  edge of the view.

**Pool layout.** A visible cluster owns `strandCount * (2^level + 1)` consecutive rows from `poolVertexBase`; strand
`s`, vertex `j` is row `poolVertexBase + s * (2^level + 1) + j`. `Hair.PoolSegmentIds` is parallel to it: a row that
starts a segment holds the segment's id (below), and a strand's last vertex holds `HAIR_INVALID_INDEX`. The pool has
a fixed capacity (4,194,304 vertices) and an overflow counter, and is rewritten every frame.

### Simulation

`Hair::Simulate` dispatches [`hairSimulate.hlsl`](../Plugins/Hair/shaders/hairSimulate.hlsl) once a frame, over every
simulated instance: one thread per guide, with a serial loop over the guide's points. The dispatch's thread groups are
rows of `Hair.SimGroups` (`HairSimGroup`: an instance and 64 of its guides, by rank), which the CPU writes each frame,
so the dispatch covers only the guide levels that can hold a simulated guide ([Simulation LOD](#simulation-lod)). A
thread whose guide the instance's simulation level does not ask for returns at once; what follows is about the guides
that are simulated. The simulation is off by default, and `hair.sim {enabled: false}` gives back the rigid evaluation
exactly.

**State.** `HairGuidePoint`, in rows the instance borrows while it is simulated ([Guide rows](#guide-rows)), in world
space, so that root motion gives the hair inertia: the point's position, where
the last substep started and how long that substep was (velocity is the difference over it), the simulated position at
the end of the previous frame, and the point's rigid pose under this frame's root frame and under the previous
frame's. A guide starts on its rigid pose, at rest, when its instance is first simulated, on `hair.sim {reset: true}`
or when the simulation is turned on, and when its root moved further than `teleport_distance` in one frame. An instance
that was simulated before and not in the frame before (no view showed it, or its entity was not drawn) starts again
from the shape its rows still hold, at rest ([Simulation LOD](#simulation-lod), "Following the view").

**Time.** A fixed step (`step_seconds`, 1/60 s) of `substeps` substeps (4). The frame's time goes into an accumulator
and whole steps come out of it; a frame takes at most `max_steps` (4), and the time a longer frame had beyond that is
dropped and counted (`dropped_seconds`, `capped_frames`), so a long frame costs a bounded amount and the hair moves in
slow motion. With the scenario system's fixed time step the run is deterministic. The steps of a frame and every
control are in `Hair.SimFrame`, so the recorded dispatch does not change with them.

- **More substeps where things move far.** A guide divides the frame's simulated time into more, shorter substeps than
  asked for when its root, or its fastest point at its current speed, would otherwise cover more than 8 mm in one
  (`HAIR_SIM_SUBSTEP_TRAVEL`), up to 64 (`HAIR_SIM_MAX_SUBSTEPS`). A root or a collider that crosses a segment's length
  in one substep tears at the strand; this is what keeps a jump of the entity from doing so. Each guide decides for
  itself, so only the guides that need them pay for them.
- **A frame that takes no step** (its time has not filled one) still moves the root and the colliders. The hair then
  goes with its root as it is: every point moves as the root does, and one substep of no length follows that puts the
  kinematic points on the root frame, takes the other points out of the colliders and solves stretch. What that moves
  keeps its velocity.

**One substep**, for a guide of `n` points:

1. **Root.** Points 0 and 1 are kinematic: they sit on the rigid pose, which fixes the root's position and the
   direction the strand leaves it. Across the frame's substeps they go from where the last frame left them to this
   frame's root frame.
2. **Integrate** the other points (position Verlet): drag toward the air's velocity at rate `damping`, then gravity.
   The air's velocity is `wind` with its speed varied by `gust_amplitude` at `gust_frequency`, along and across the
   wind; with no wind the drag is plain damping.
3. **Shape.** Each point is pulled toward its rigid pose with compliance `shape_compliance * u^2`, `u` running from 0 at
   the root to 1 at the tip. This is what keeps the style: which way a strand leaves the head and falls. A free tip
   settles up to `9.81 * shape_compliance` metres from its rigid pose under gravity.
4. **Bend,** from the root to the tip. Point `i` belongs at `x[i-1] + R * (rest[i] - rest[i-1])`, `R` being the smallest
   rotation from the rigid direction of segment `(i-2, i-1)` to its current direction: the rest curvature, carried in
   the root's frame. The correction is shared by the three points by the constraint's gradient (for a straight rest
   shape, the usual `(1, -2, 1)`). One sweep makes this stiff against kinks and weak against long bends, which the shape
   pull covers.
5. **Collide, then stretch,** three times (`HAIR_STRETCH_PASSES`). Stretch is a distance constraint between neighbours
   with `stretch_compliance` (0). A strand's stretch constraints form a chain, so the linear system for their
   multipliers is tridiagonal, and it is solved exactly (Thomas algorithm) in one pass up and one down the strand.
6. **Collide** once more, with friction, so a substep ends with no point inside a collider.

**XPBD or PBD.** The constraints are XPBD in its small-step form (Macklin et al. 2019): one solve per substep, so the
multipliers start at zero and none are stored, and a compliance means the same at any substep count. With the default
stretch compliance of 0 the stretch constraint is PBD's. What matters more is how it is solved. A sweep of pairwise
projections along a hanging strand leaves each segment stretched in proportion to the points below it (about 2 cm on a
32-segment strand at 4 substeps), and more sweeps help slowly; the direct solve leaves about 1e-3 of a segment's length
after one pass and 1e-5 after two, each at the cost of two sweeps.

Three forms of the bend constraint were tried first and are not what is there. Moving only the child (follow the
leader) whipped strands apart within a second. Sharing the correction between the child and its parent alone looked
right but kept some strands moving for ever at some settings: it leaves a torque with no reaction. Distance constraints
two and three segments apart were stable and left strands crumpled after fast motion.

**Colliders.** Up to 8 per instance (`hair.colliders`): capsules, a sphere being a capsule of no length, given in the
rest space of the instance's entity (its mesh space, the groom's). They follow the entity's transform, and the
instance's attach bone when it has one (not each root's joints in `per_root` mode). A collider is tested at the pose it
has at each substep, between the previous frame's and this frame's, which is the sweep; the substeps are short enough
that it moves at most 8 mm in one unless the frame would need more than 64 of them. A point inside
`radius + collision_margin` (2 mm) is put on that surface along the collider's normal, and loses `friction` (0.3) of its
sliding speed relative to the collider. The two kinematic points are not collided. There are no signed distance fields
and hair does not collide with hair.

**Controls** (`hair.sim`), with their defaults:

| Field | Default | Meaning |
|---|---|---|
| `enabled` | false | Simulate the guides and evaluate the other strands from them. |
| `gravity` | `[0, -9.81, 0]` | m/s^2. |
| `damping` | 3 | 1/s: how fast a point's velocity goes to the air's. |
| `shape_compliance` | 0.01 | s^2, at the tip. 0 holds the rigid pose. |
| `bend_compliance` | 1e-5 | s^2. With 0 a tightly curled strand on a weak shape pull may never come to rest. |
| `stretch_compliance` | 0 | s^2. |
| `step_seconds`, `substeps`, `max_steps` | 1/60, 4, 4 | The fixed step, its substeps, and the most steps a frame takes. |
| `wind`, `gust_amplitude`, `gust_frequency` | `[0, 0, 0]`, 0.5, 0.7 | m/s; the share of its speed the wind varies by; Hz. |
| `friction`, `collision_margin` | 0.3, 0.002 | See colliders. |
| `teleport_distance` | 1 | Metres a root may move in a frame before its guide starts again from the rigid pose. |
| `frame_seconds` | 0 | Above 0: every frame simulates this much time, whatever the frame's own time step. For measuring: the statistical profiler's frames have a time step of 0, in which the simulation would take no step. |
| `lod_full_coverage`, `lod_level0_coverage`, `lod_off_coverage` | 1, 0.125, 0.03 | The coverages at which every guide, level 0 alone and nothing is simulated. See [Simulation LOD](#simulation-lod). |
| `lod_rate` | 2 | Levels per simulated second: the most the simulation level moves toward what the coverage asks for. |
| `lod_force_level` | -1 | 0 and above: every instance takes this simulation level at once (8 is every guide of any groom). For testing. |
| `lod_point_budget` | 0 | Above 0: the most guide points simulated over all instances. |
| `promote_seconds`, `demote_seconds` | 0.25, 0.25 | How long a promoted guide is blended from, and a demoted one back onto, what its parents give it. 0: at once. |
| `lod_view_grace_seconds` | 0.25 | How long an instance that no view shows any more is still simulated. |
| `lod_settle_seconds` | 1.5 | How long an instance that starts from its rigid pose is simulated whether or not a view shows it. 0: an instance out of view is never started. |
| `guide_point_budget`, `guide_keep_seconds` | 0, 0 | The most guide rows lent out at once (0: the buffer), and how long a kept shape is held (0: until the rows are needed). See [Guide rows](#guide-rows). |
| `reset` | | `true` puts every guide back on its rigid pose and zeroes the step counters. |

Compliances are per unit mass and per constraint, so the same values are stiffer on a groom with longer segments; they
are not normalized by segment length. Lengths are metres whatever the entity's scale.

**Metrics.** The simulate pass measures each instance's guides at the end of every frame, and the evaluate pass the
roots it draws, into `Hair.SimMetrics`. `hair.sim` and `hair.stats` (`sim`) return the last readback of each instance,
and each call asks for the next; with `hair.stats {continuous: true}` every frame is read back. Stretch, speed,
deviation and collider distance are over the simulated points of the guides, that is every point but each guide's first
two.

| Field | Meaning |
|---|---|
| `frame`, `steps` | The frame the metrics are of, and the steps taken up to and including it. |
| `guides`, `reset_guides` | Guides simulated (those being demoted included), and those put back on the rigid pose in that frame. `guides` is 0 for an instance that is not simulated. |
| `resumed_guides` | Guides that started in that frame from the shape their instance was left in when it stopped for being out of view. |
| `stale_guides` | Guides that went on from rows not written for this instance in the frame before. 0 unless the lending of guide rows and the passes disagree ([Guide rows](#guide-rows)). |
| `holds_rows`, `starved` | Whether the instance held guide rows in that frame (simulated, or keeping a shape), and whether it is in view and to be simulated but got none: it then follows its roots rigidly. |
| `stretch_max`, `stretch_mean`, `stretched_guides` | `abs(length / rest length - 1)` of a guide segment: the largest, the mean over guides of each guide's mean, and the guides with a segment over 2%. |
| `root_shift` | Metres: the furthest a guide's root was, when the frame began, from the frame's previous root frame. See below. |
| `speed_max`, `speed_mean` | m/s: the fastest point, and the mean over guides of each guide's mean point speed. |
| `deviation_mean` | Metres from the rigid pose, the mean over guides of each guide's mean. |
| `collider_distance` | The smallest signed distance from a point to a collider's surface, metres; absent without colliders. At rest on a collider it is the margin. |
| `guide_roots`, `guide_root_error` | Drawn strands that are guides, simulated or not, and how far the evaluation left any of their first vertices from its root frame before it was put there. |
| `strand_roots`, `strand_root_error` | The same for drawn strands interpolated from guides. |
| `sim_level`, `target_level`, `coverage` | The instance's simulation level in that frame, what it is moving toward, and the coverage that was computed from (0 when the entity's matrix was not on the CPU). |
| `in_view`, `view_extent` | Whether the main view shows any part of the groom's bounds, and the size on screen of the part it shows, in the coverage's units (1e9 when the camera is inside the bounds). |
| `level_guides`, `simulated_points` | Guides simulated, by level, and the points they have. |
| `evaluated_guides` | Guides not simulated: evaluated from their parents. With `guides` it is every guide of the groom. |
| `blending_guides`, `promoted_guides`, `demoted_guides` | Simulated guides in a promotion or demotion blend; guides that became simulated in that frame; guides whose demotion ended in it. |
| `transition_motion`, `steady_motion` | Metres. A guide's motion is the furthest any of its points moved in the frame relative to its root's own move. The largest over the guides that changed state in that frame or the one before or are in a blend, and the largest over all the others. A jump at a state change would show as the first far above the second. |

`hair.sim` also returns `steps`, `simulated_seconds`, `dropped_seconds`, `capped_frames` and `last_frame_steps`.

**Frames that do not follow one another.** A root frame pair is the entity's row of the per-object buffer the frame
binds: its model matrix and its previous one, from one published version of the scene. Measured on a moving entity, the
previous root frame is often not where the root was in the frame before: in about a third of the frames it is one or
two frames' travel of the entity away, as if the frame had bound the version of the frame before again, or skipped one
or two. Over a few frames the entity then stands still and covers two or three frames' travel in one, at different
frames in every run. Why the versions arrive that way was not looked into; it is the renderer's, and the entity's own
mesh is drawn with the same rows. The simulation does not take the previous root frame's word for it: a guide's
kinematic points start each frame from where the last one left them, and the shape pull's goal and the colliders'
previous pose are moved by the same difference (`rootShift` in the shader, `root_shift` in the metrics). The previous
positions the evaluate pass writes are the guides' own, so hair's motion vectors are of the motion that was drawn. The
jumps themselves remain, and are what the extra substeps are for ([Checking it](#checking-it) has numbers).

**Not built:** twist and material frames, hair against hair, signed distance field colliders, interpolating the
simulated state between steps for frames shorter than a step.

### Simulation LOD

How many guides an instance simulates follows how large its groom is on screen, and an instance the view does not
show is not simulated at all ([Following the view](#following-the-view)). The guide-capable set is in levels
(64, 192, 768 and 3,072 guides with the builder's defaults), and every guide above level 0 has up to three parents in
the level below, with weights (`HairGuideBinding`). The runtime takes up to `HAIR_MAX_GUIDE_LEVELS` (8) levels.

**The simulation level.** Each instance has a fractional simulation level, `sFrac` (`HairInstanceGPU::simLevel`),
between 0 and its groom's level count:

- above 0, its level 0 guides are simulated;
- a guide of level `k >= 1` is simulated when `sFrac >= k + randomOffset`, its own offset in [0, 1), so guides come and
  go one at a time as `sFrac` moves, not a level at a time;
- at the level count every guide is. At 0 none is: the instance is not simulated, and its strands follow their roots
  rigidly, exactly as with the simulation off.

So between 0 and 1 only level 0 is simulated, and nothing changes until `sFrac` reaches 0.
`HairGuideIsSimulated` in [`hairCommon.hlsli`](../Plugins/Hair/shaders/hairCommon.hlsli) is the one place that applies
the rule.

**Choosing it** is the CPU's work, once a frame (`PlanSimLevels` in
[`HairRuntime.cpp`](../Plugins/Hair/src/HairRuntime.cpp)), from the instance's *coverage*: the radius of the groom's
rest bounds (half the diagonal of its box, times the entity's scale) over its distance from the main camera, times
`projection[1][1]`. That is the diameter of the bounds on screen over the height of the view, so a groom whose bounds
fill the view from top to bottom has coverage 1. The scenario's groom, a 0.5 m sphere with 0.9 m of hanging hair, has
coverage 0.72 from 4.2 m with a 50 degree field of view.

| Coverage | `sFrac` |
|---|---|
| `lod_full_coverage` (1) and above | the level count: every guide |
| `lod_level0_coverage` (0.125) | 1: level 0 alone |
| `lod_off_coverage` (0.03) and below | 0: nothing |

In between `sFrac` is linear in the logarithm of the coverage. With four levels and the defaults that is one level for
each halving of the groom's size on screen: a level holds four times the guides of the one below, so the simulated
guides per screen area stay roughly the same.

That is for a groom wholly in view. One partly outside it takes the level of the part that is inside, and one wholly
outside is not simulated ([Following the view](#following-the-view)).

- **Rate.** `sFrac` moves toward what the coverage asks for by at most `lod_rate` (2) levels per simulated second.
- **Staying put.** `sFrac` sets off only once its target is more than 0.02 levels away, and then goes all the way; an
  instance that is not simulated starts only when its coverage asks for a level of 0.25 or more. A groom at a steady
  distance therefore does not flicker between states.
- **Starting.** An instance starts at its target level at once, with every guide on its rigid pose and at rest in
  world space, which is where its strands were. Its guides do not take up the entity's velocity. (An instance that
  stopped for being out of view starts differently: see below.)
- **Stopping.** Once `sFrac` is 0 the instance stays simulated for `demote_seconds` and two more steps, while its
  level 0 guides blend back onto the rigid pose (below), and then stops.
- **Forced levels.** `lod_force_level`, and `hair.simlevel` for one instance, apply at once, with no rate limit, and
  wherever the instance is: a forced level is simulated out of view too.
- **Budget.** With `lod_point_budget` above 0, when the levels asked for would simulate more guide points than that
  over all instances, every instance's level is multiplied by one factor, found by bisection over each groom's sorted
  table of the level every guide is simulated from. A level is never scaled below level 0 alone, so the budget does not
  turn an instance off and cannot be met where the level 0 guides alone pass it.
- The coverage uses the entity's matrix from the per-object buffer the frame binds, and the attach bone's skin matrix
  for an instance on a bone. Without that buffer's rows on the CPU the instance simulates every guide.

#### Following the view

`PlanSimLevels` asks one function, `OnScreenExtent` in [`HairRuntime.cpp`](../Plugins/Hair/src/HairRuntime.cpp), whether
a view needs an instance's hair and how much of it: the size on screen, over the view's height, of the part of the
groom's bounding sphere that is inside the view (the sphere's coverage times the smaller of the shares of its width and
of its height that are on screen). 0 means no view shows any of it. There is one view, the main camera: hair is in no
reflection, and the shadow view does not count (a groom only its shadow shows is not simulated and casts the shadow of
its rigid pose: [Shadow casting](#shadow-casting)). A second view would be a second `HairView` in the frame's inputs and the larger of the
two answers; nothing else knows how many views there are.

- **The test** is the select pass's, on the CPU, in the same frame: the sphere against the main camera's six view-space
  planes. The sphere is centred on the middle of the groom's rest bounds, through the entity's matrix (and the attach
  bone's), and holds every cluster's bounding sphere. The select pass grows a cluster's sphere by 25%; this one is
  grown by 50%, so an instance counts as in view a little before any strand of it can be drawn, and a frame's
  difference between the CPU's camera or bones and the GPU's does not matter. The evaluate pass counts the strands it
  draws of instances the CPU found out of view (`unseen_strands_drawn`): it has to be 0.
- **Partly in view.** The level is the one the coverage gives while the visible part is at least as large as the rest
  bounds' own coverage, and the level of the visible part's size below that, but never less than level 0 alone while
  any part is in view and the coverage asks for level 1 or more. So a groom that slides out of the view loses levels as
  it goes, guide by guide with the usual blends, and one that slides in gains them. It makes no difference which part
  is in view: a head on screen with its hair off it and hair on screen with its head off it are both the sphere of the
  whole groom, partly in view.
- **Leaving.** An instance wholly out of view is still simulated for `lod_view_grace_seconds` (0.25), with level 0
  alone as its target, and then stops as it is: it is not blended back to the rigid pose, nothing is drawn of it, and
  its guide rows keep the shape it had. The grace is what keeps a groom at the edge of the view from stopping and
  starting; a groom that comes back within it was never interrupted. It keeps those rows until an instance in view
  needs them ([Guide rows](#guide-rows)); a groom whose rows were taken has no kept shape, and enters as one that was
  never simulated but is not settled unseen first.
- **Entering.** An instance that stopped that way starts again in the frame its sphere touches the view, before the
  frame's strands are evaluated, from the shape it kept (`HAIR_SIM_FLAG_KEEP_SHAPE`): every guide the solver owned when
  it stopped, and that the level still asks for, starts at rest with each point at the offset from its rigid pose that
  it had; the others are evaluated from those. It starts at the level it stopped at (or its target, if lower), and the
  level then rises at `lod_rate` with ordinary promotions. So a camera that turns to a groom, however fast, finds it in
  the shape it last had, hanging and not on its rigid pose, and nothing is drawn of it before that frame. What is lost
  is its motion: it starts at rest. The offsets are kept in world space and not turned with the head, so a head that
  turned a long way while out of view starts from a shape that is wrong by that turn, and settles from there.
- **Settling unseen.** An instance with no kept shape, which would be simulated if it were in view, is started out of
  view too, at level 0 alone, and stops like any other once it has had `lod_settle_seconds` (1.5) of simulation since
  it started from its rigid pose. Otherwise a groom that came near behind the camera, or was there when the simulation
  was turned on, would be seen settling from its rigid pose when the camera turned to it. This is the one case in which
  hair nobody sees is simulated: for a second and a half, once, at 64 guides a groom. With `lod_settle_seconds` 0 it is
  not, and such a groom is seen settling. Settling unseen takes free guide rows only: when there are none the groom
  is left as it is.
- An instance that starts in view with no kept shape (a groom attached in view, the simulation turned on, a reset)
  starts from its rigid pose and is seen settling, as before.
- A groom whose roots follow their own joints (`per_root`) has no bounds on the CPU and always counts as in view.

`hair.sim`'s metrics give each instance's `in_view` and `view_extent`, and `resumed_guides` in the frame it starts from
a kept shape.

**Guides the solver does not own.** `Hair::GuideLevels`
([`hairGuideLevels.hlsl`](../Plugins/Hair/shaders/hairGuideLevels.hlsl)) runs after the simulate pass: one dispatch per
guide level, coarsest first, over that level's guides of every simulated instance (rows of `Hair.SimGroups`, like the
simulate pass), with a barrier after each, because a level reads the rows of the level below and writes its own in the
same buffer. A guide that is not simulated is evaluated from its parents. For
a point at curve parameter `u`:

```
rigid    = the point following its own root rigidly
for each parent p:  P = p's curve at u,  Pr = p's rigid curve at u
                    R = the smallest rotation from the chord (Pr - p's rigid root) to the chord (P - p's root)
                    q_p = P + R * (rigid - Pr)
position = sum of weight_p * q_p, over the sum of the weights
```

A level 0 guide has no parents and goes to its rigid pose. The rows of such a guide hold what a simulated guide's do:
the position, the position at the end of the previous frame (so motion vectors follow it like any guide), the rigid
pose, and a velocity, the point's motion over the frame's simulated time, in the solver's own fields. Every guide of a
simulated instance therefore has live rows; deeper levels and the evaluate pass read them without asking who wrote them.

This is the rule render strands follow their guides by, with one difference: the rotation comes from the parent's
chord, from its root to the sampled point, and not from its tangent there
(`HairSkinToGuideChord` in [`hairGuideSkin.hlsli`](../Plugins/Hair/shaders/hairGuideSkin.hlsli)). With the tangent,
guides evaluated through three levels thrashed. The largest motion of any guide in a frame, relative to its root, with
the head swaying 5 cm:

| | every guide simulated | level 3 | level 2 | level 1 (64 guides simulated) |
|---|---|---|---|---|
| wavy groom, tangent | 11.6 mm | 23.1 mm | 128.7 mm | 350.9 mm |
| wavy groom, chord | 11.8 mm | 9.8 mm | 13.1 mm | 13.6 mm |
| straight groom, tangent | 8.5 mm | 11.9 mm | 25.5 mm | 29.7 mm |
| straight groom, chord | 11.4 mm | 8.7 mm | 8.9 mm | 9.9 mm |

A guide can be 20 cm from its parent, and what it gets is the next level's parent. The likely cause is that the
tangent's rotation carries no twist: along a wavy parent it swings from side to side with the waves, which over such an
offset puts waves of its own into the child, whose tangents then turn further than the parent's did. The chord is an
integral of the curve and not a derivative, so an error in a parent does not grow in its children. Render strands keep
the tangent: they are about a centimetre from their guides and one step from them.

**Promotion and demotion.** A guide's state is in the first of its rows: whether the solver owns it, whether it is in a
blend, and how far into it (`HAIR_GUIDE_STATE_*`, `guideBlend`). The simulate pass keeps it; the guide-level pass does
the blending, after the frame's steps, by moving a blending guide's simulated state toward its evaluated state by a
share of the distance between them (`HairGuideBlendPull`). It moves the solver's previous position with the point, so
the pull adds no velocity.

- **Promotion.** A guide the level newly asks for is simulated from its rows as they are: its evaluated position and
  velocity. The solver at once enforces what an interpolated shape does not satisfy (rest lengths, the colliders) and
  the strand starts toward a shape of its own, so a guide that is only seeded moves abruptly in its first frames. The
  blend holds it: in its first frame the guide is put back exactly on its evaluated state (a share of 1), and the share
  falls to 0 along a smoothstep over `promote_seconds`.
- **Demotion.** A guide the level no longer asks for is simulated for `demote_seconds` more. Each frame it is moved by
  the share of the remaining distance that a smoothstep covers in that frame, and by all of it in the last, after which
  it is no longer simulated. Were neither state moving, it would follow the smoothstep from one to the other.
- **Turning round.** A demotion on a guide the level asks for again just ends, and the guide goes on from where the
  blend had brought it. A promotion on a guide the level drops becomes a demotion from its start.
- **Level 0** is demoted to the rigid pose, when `sFrac` reaches 0; it is promoted by the instance starting.
- A reset or a teleport puts a guide on its rigid pose and ends its blend.

What the blends buy, measured with the level sweeping between 2 and 3.5 at 1/150 of a level a frame under the sway (the
largest motion in a frame of a guide that changes state, and the mean over the frames of that largest, against the same
for the other guides):

| | changing guides | other guides |
|---|---|---|
| promotions, 0.25 s blend | 25.8 mm (mean 6.9 mm) | 36.8 mm (mean 8.6 mm) |
| promotions, no blend | 59.2 mm (mean 9.5 mm) | 33.8 mm (mean 10.5 mm) |
| demotions, 0.25 s blend | 11.9 mm (mean 4.8 mm) | 12.6 mm (mean 4.8 mm) |
| demotions, no blend | 65.6 mm (mean 10.9 mm) | 31.5 mm (mean 5.9 mm) |

**Which guides the passes cover.** Only instances that are simulated this frame: one that no view shows is in none
of the lists. The guide-level pass covers every guide of a simulated instance: a thread whose
guide is simulated and in no blend returns after reading three rows. The simulate pass covers an instance's guide
levels 0 to `floor(h)`, where `h` is the highest `sFrac` the instance has had in the last `demote_seconds` (and a few
steps): a guide of level `k` is simulated from `sFrac = k + randomOffset`, and one the level has stopped asking for is
still simulated for `demote_seconds`. In the frame an instance starts, starts again from a kept shape or is reset it
covers every guide, because that frame writes every guide's state. Inside the covered levels a thread whose guide is not simulated returns after reading
four rows (the instance, the guide's binding, its point range, its first point).

Both passes used to be dispatched per instance (the guide-level pass per instance and level, with a barrier each). With
a hundred grooms that cost far more than the work: [POC measurements](#poc-measurements) has the numbers before and
after.

**Limits.**

- **The simulate pass's time has a floor.** A guide is one thread running a long serial loop, so the dispatch takes
  about as long as its slowest thread group however few groups have work. Reducing the level therefore reduces the
  pass's time by less than the guides it drops. The pass is one dispatch for every instance, so the floor is paid once
  a frame and not once per groom ([POC measurements](#poc-measurements)).
- **The guide-level pass follows the simulated grooms, not the drawn strands.** It evaluates every guide of every
  simulated instance that the solver does not own. Simulated instances are the ones in view, and measured, the strands
  drawn of a simulated groom wholly in view depend on every one of its guides, or all but a few in a thousand, while
  its guides are a seventh of its strands or fewer ([View-following measurements](#view-following-measurements)), so
  marking the guides in use would save little. It would save about half on a groom that is half out of view, and on a
  groom with as many guides as strands, in proportion to the strands the LOD drops.
- **Visibility is the frustum's.** A groom behind a wall, or behind other grooms, is in view and is simulated.
- Evaluated guides are not collided: a guide between parents on either side of a collider can pass through it. A
  promoted guide comes out of it over its blend.
- When `sFrac` reaches 0 with the groom still large on screen (a forced level), the hair moves from its simulated shape
  to the rigid one in `demote_seconds`: a quick, visible settle, with no jump. At the default off coverage the groom is
  a few dozen pixels tall.
- Parents are strictly in the level below, with inverse-distance weights, and the chord's rotation carries no twist.
- One level per instance, from the main view only (`OnScreenExtent` is where another view would be asked).

### The hair visibility target

`Hair.Visibility` is an `R32G32_UInt` texture at the render resolution, read and written as one `uint64_t` per pixel,
exactly as the opaque visibility buffer is. `Hair::Raster` clears it to all ones ("no hair") every frame and writes it
with `InterlockedMin`, so the nearest segment wins.

| Bits | Field |
|---|---|
| 63..33 | Depth: `asuint(linear view depth) >> 1`, the depth field of the opaque visibility buffer (`PackVisKey` in `visibilityPacking.hlsli`), so the two compare directly. |
| 32 | Unused, 0. |
| 31..16 | Visible-cluster index: the row of `Hair.VisibleClusters`. |
| 15..10 | Strand index within the cluster; 0 is its lowest rank. |
| 9..0 | Segment index along the strand, from the root, at the cluster's drawn vertex level. |

Bits 31..0 are the segment id. The list holds at most 65,535 clusters, one fewer than its field could name, so no real
id is `HAIR_INVALID_INDEX`. The constants are in
[`HairRuntimeGPUTypes.h`](../Plugins/Hair/include/Hair/HairRuntimeGPUTypes.h) and the packing in
[`hairCommon.hlsli`](../Plugins/Hair/shaders/hairCommon.hlsli). A reader finds the strand through the visible cluster:
its instance, its cluster, then the cluster's `strandOffset` plus the strand index. Visible-cluster indices are only
good for the frame that wrote them.

On the [layered path](#the-layered-path) the target is the nearest of a pixel's three layers: `Hair.Layer1` and
`Hair.Layer2` hold the same kind of key for its second and third nearest segments, all ones where there is none. So
everything that reads the nearest key (the debug views, the probe) works on both paths.

### The rasterizer

`Hair::Raster` runs one thread per pool row. A row that starts a segment draws it:

- Both ends are projected with the main camera's view-projection, the one the opaque geometry was rasterized with. A
  segment behind the near plane is dropped; one that crosses it is clipped.
- The segment's *footprint* is a screen-space trapezoid with round ends: a pixel is in it when its centre is within the
  segment's half-width of the nearest point on the centreline. The half-width is interpolated between the two ends and
  is at least half a pixel, so the footprint of every strand is at least one pixel wide and has no gaps: no strand is
  missed, however thin.
- Pixels are walked along the segment's longer screen axis, and in each column only those near the centreline are
  tested. A segment tests at most 4,096 pixels (`HAIR_RASTER_MAX_PIXELS_PER_SEGMENT`); one that needs more is cut short
  and counted in `segments_capped`.
- A footprint pixel's depth (perspective-correct along the segment) is compared with the opaque visibility buffer's
  depth field at that pixel. Hair at or behind the opaque surface is rejected and counted in `pixels_occluded`.
- **Coverage.** The share of a footprint pixel the strand covers is its true width there (the pool's width: the LOD's
  width scale and the boundary strand's fade included) over the footprint's: `min(width / max(width, 1 px), 1)`. A
  footprint's pixel count follows its area, so the coverage summed over the footprint is the strand's true projected
  area, whatever its direction on screen. A strand a pixel wide or more has coverage 1 and hard edges, as before.
- **Stochastic acceptance** (`coverage` `white` or `interleaved`). The pixel is written only when a random number in
  [0, 1) is below the coverage. The number is per pixel, per strand and per frame: it changes every frame, so the
  temporal upscaler averages the outcomes into the strand's true weight, and it is independent between the strands over
  one pixel, so several thin strands combine as independent occluders. The nearest *accepted* segment wins, so a pixel
  shows strand `k` with probability `c_k` times the product of `1 - c_j` over the strands in front of it, which is what
  compositing them front to back would give, and what is behind sparse hair (other hair, the scalp, the background)
  shows through in the frames no strand is accepted. A strand of zero width (the boundary strand at the start of its
  fade) is never written. `white` hashes the pixel, the strand and the frame (PCG). `interleaved` is interleaved
  gradient noise over the pixels, shifted each frame, plus a hash of the strand, so the accepted pixels along one strand
  are evenly spaced; measured, the two are not told apart after the upscaler
  ([Partial coverage measurements](#partial-coverage-measurements)).
- **Joints.** With coverage on, a strand's segments meet end to end: a pixel past one of a segment's ends belongs to the
  neighbouring segment, and only the strand's own two ends are round. The round joints of the opaque form put the pixels
  around every vertex in two footprints, which would count their coverage twice (about a fifth more for a segment four
  pixels long). At a bend the two footprints overlap on the inside and leave a wedge on the outside, of equal areas to
  first order. With `coverage` `off` the joints are round, exactly as before, and every footprint pixel in front of the
  opaque surface is written.

A pixel in which no strand is accepted holds no key: the resolve leaves it alone, and it keeps the surface, depth and
motion vector of whatever opaque surface is behind the hair.

**On the layered path** nothing is accepted at random, whatever `coverage` says, and the joints are always end to end:

- **Three layers.** Every owned footprint pixel in front of the opaque surface, of a strand with any width, offers its
  key to the pixel's layers by a chain of `InterlockedMin`. The write to the first layer returns what the layer held;
  the larger of that and the key goes on to the second layer, and the larger of those to the third. Each step keeps the
  smaller value and passes the other on, so whatever the order the threads run in, the three layers end holding the
  pixel's three smallest keys, nearest first.
- **Early reject against the third layer.** Before the chain the key is compared with the third layer by a plain load,
  as Indiana Jones does. That layer only falls during the pass, so a key not below the value read can never be one of
  the pixel's three nearest: a stale read can only fail to reject, which costs the three atomics it would have saved,
  and never rejects a key that belongs. In a dense groom most footprint pixels are behind three strands already and
  stop here (`layer_rejected`).
- **Coverage is always added**, rejected or not, into the [coverage target](#the-coverage-target): the composite needs
  the sum over every strand. A strand that covers its pixel whole (coverage 1) adds 16 pixels' worth, not 1
  (`opaque_coverage`, on): nothing shows through it, so the pixel is opaque from it on wherever among the pixel's
  strands it lies, and the composite's remainder says so. Before, such a strand behind the third layer counted as
  `1 - exp(-1)`, 63% opaque. With `coverage_saturation` above 0 a pixel whose sum has reached that many pixels' worth
  takes no more (`coverage_skipped`): the same kind of plain load, of a value that only rises.
- **Marks for the shade pass.** A segment that offers a key to any pixel's layers marks its two rows of
  `Hair.PoolColors` with the frame's number. Only such a segment can be in a layer when the pass ends (a key that ends
  in a layer was below the third layer when it was compared), so the shade pass lights the marked vertices and no
  others. `Hair::ShadeList` then gathers the marked rows into `Hair.ShadeList`
  ([The layered path](#the-layered-path)).

This is the simple form, the **direct rasterizer**: no tiles, no binning, and no anti-aliasing of the edges of a strand
wider than a pixel. Every strand of the pool is drawn, hidden or not. The main view is assumed to be a perspective view
(clip `w` is the linear depth). The surface path always uses it; the layered path uses it or the culled rasterizer.

### The culled rasterizer

The direct rasterizer's cost follows every strand of the pool, and in a close groom nearly all of them are behind
other hair: with `wStraight` filling the view it visits 35 footprint pixels for each pixel that shows hair.
[`hairRasterCulled.hlsl`](../Plugins/Hair/shaders/hairRasterCulled.hlsl) fills the same targets from the same
footprints, pixel for pixel, and skips hair that hair hides. It draws the layered path when `hair.render` `raster` is
`culled`, or `auto` (the default) and a groom in view has strands at least `auto_width_pixels` (0.75) wide on screen at
its middle; `direct` never uses it. [Rasterizer cost measurements](#rasterizer-cost-measurements) has what it buys,
where it loses and why the rule is the strand width.

**The hidden depth.** `Hair.Occlusion` holds, for every pixel, the depth from which hair adds nothing to the pixel. It
starts each frame as the opaque surface's depth (`HairOcclusionInitCS`), and hair lowers it during the pass in two
ways:

- **A strand that covers its pixel whole** (coverage 1: a strand a pixel wide or more) lets nothing through. The hidden
  depth falls to its depth (`occlude_opaque`). It adds nothing to the coverage sums: the pixel is marked saturated, and
  the composite takes the remainder of a marked pixel as fully opaque. In the layers it takes its own place only; what
  it pushes out of a layer is behind it and is dropped.
- **Saturated coverage** (`occlude_saturated`). Every pixel has a *split depth*, a guess carried from the frame before
  (`Hair.SplitGuess`). Coverage is summed apart in front of it (`Hair.Coverage`) and behind it (`Hair.CoverageFar`).
  When the sum in front reaches `saturation` (10 pixels' worth) the hair in front of the split depth alone hides what
  is behind it, to within `exp(-(saturation - 3))` of the pixel, whatever is behind: the hidden depth falls to the
  split depth, and the pixel is marked saturated. The guess moves from frame to frame toward the nearest depth that
  saturates (`HairOcclusionUpdateCS`: nearer while the sum in front is over twice the saturation, further while it is
  under it and the whole sum is not). A wrong guess costs time and not the image: too near and the pixel does not
  saturate, too far and less is skipped.

A footprint pixel at or behind the hidden depth is skipped: no coverage add, no layer write. The hidden depth only
falls during a pass and is read by a plain load, so a stale read draws a pixel it could have skipped, and never skips
one it should have drawn.

**Why the composite cannot tell.** A strand that covers the pixel whole is either in the pixel's three layers, and then
the layers are opaque with it and nothing behind is blended, or behind them, and then the remainder is opaque (the
coverage rule of [The rasterizer](#the-rasterizer), which the direct rasterizer applies too). Hair behind such a strand
adds nothing in either case. Hair behind a saturated split depth adds to a remainder that is already `1 - exp(-7)` or
more. So the two rasterizers give the composite the same pixel, to that tolerance; the layers a pixel holds behind an
opaque one can differ and are not looked at.

**Chunks and tiles.** A segment is cut into chunks of 8 pixels along its major axis, aligned to screen tiles of 8x8
pixels (`HAIR_RASTER_TILE_SIZE`). A chunk is the unit of work: one thread draws one chunk, so the lanes of a wave have
work of the same length (0.64 to 0.80 of the lanes are busy in the measured views), and a hidden chunk costs one test
and none of its pixels. `Hair.OcclusionCoarse` holds the largest hidden depth of each tile; a chunk whose nearest
point is behind that in every tile it touches is hidden. Within a column of a chunk only the pixels whose centre can be
inside the footprint are looked at (the centreline's place in the column, plus or minus the half-width times
`sqrt(1 + slope^2)`): about one pixel looked at for each pixel of the footprint, where the direct rasterizer's wider
column bound looks at three.

**Two passes, and the frame before as a guess only.**

1. `HairClassifyCS` (pass `Hair::Classify`), one thread per pool row, sorts the row's chunks. The **first pass** gets
   those that are probably in front (`first_pass`): `previous`, the default, takes the chunks that the tile depths of
   the last frames do not hide (`Hair.OcclusionHint`, below), of clusters that drew a pixel in the last two frames;
   `prefix` takes the first eighth of each cluster's drawn strands, which the rank order spreads evenly over the
   cluster; `both` either; `all` every chunk, with no late pass. The chunks go into `Hair.RasterItems` (a pool row and a
   chunk number in a `uint`), a wave taking its rows of the list with one atomic; the others are a bit each in the row's
   late mask (`Hair.PoolLate`; a segment longer than 31 chunks shares the last bit among the rest, which then go
   together).
2. `HairRasterItemsCS`, one thread per item, by an indirect dispatch of the list's length (`HairRasterArgsCS`).
3. `HairOcclusionUpdateCS`, one thread group per tile, takes the largest hidden depth of the tile's pixels as the
   first pass left them. A tile no chunk touched hides nothing. `HairOcclusionHintCS` then folds it into the guess for
   the next frames: a tile's guess rises at once to a larger depth and comes back down to a smaller one keeping
   `hint_decay` (0.8) of the difference a frame, so it is the largest tile depth of the last dozen frames or so.
4. `HairRasterLateCS`, one thread per pool row with a late mask, tests each late chunk against **this frame's** tile
   depths and draws it if it is not hidden.

**Why the guess spans frames.** The camera is jittered by a fraction of a pixel every frame for the upscaler, and each
jitter looks through different gaps between strands, so a tile's largest hidden depth is not the same from one frame
to the next even in a still view. With the last frame's depths alone as the guess, the first pass leaves pixels without
the strand that hides them, such a tile then hides nothing, and the late pass walks every late chunk over it. With the
span the first pass is larger and the late pass nearly idle; the time is the same either way
([Rasterizer cost measurements](#rasterizer-cost-measurements)), and about twice what a camera without jitter costs.

Nothing waits a frame to be seen. What the first pass leaves out is tested against the hidden depths of the frame
being drawn, which are exact, so hair that comes into view (a camera cut, a head turn, hair swinging open, a groom
entering) is drawn in the frame it does; `late_pixels_written` counts it. A bad guess makes the first pass draw too
little and the late pass, which is the slower one for the same work (its threads have segments of different lengths),
draw the rest. When the item list is full the chunks that do not fit are left to the late pass
(`raster_items_overflow`).

**Marks.** A segment that writes a layer marks its two pool rows for the shade pass, as in the direct rasterizer. Far
fewer segments do: a hidden strand is never lit.

#### Hidden clusters

The select pass needs to know which clusters the camera does not see: they still have to be in the pool, because the
[density volume](#the-density-volume) is filled from it, but not with every strand. `Hair.ClusterState` keeps two
things per cluster of every instance (`HairInstanceGPU::clusterSeenBase` is an instance's first cluster):

- the frame it last drew a footprint pixel in front of the hidden depth (the rasterizer's stamp);
- its **detail scale**, which the select pass moves a step a frame toward a target and multiplies the cluster's detail
  level `l` by: 1 for a cluster that drew a pixel in the last two frames, and `hidden_detail` (0.25) for one that drew
  none, behind the head or under hair that hides it.

A hidden cluster at a quarter of its detail keeps a quarter of its strands in the pool, **at the width they have at
the cluster's own level** (`HairVisibleCluster::widthDetail`): they are the first strands of the ones it would draw in
view, not fewer and wider ones. A hidden cluster drawn fat would poke through the hair that hides it, be seen, be drawn
thin again and hide again; that was built first and measured (below). What the voxelizer deposits for each kept
strand is scaled up instead (`densityScale`, the ratio of the two levels' width scales), so the density in the volume
is the whole cluster's. Evaluation, classification, the late pass's tests and voxelization all cost a hidden cluster's
kept strands only.

The scale rises by `detail_rate_up` (0.25) a frame and falls by `detail_rate_down` (0.02), and strands come and go by
the boundary strand's fade, as with any change of `l`. A cluster that comes into view is drawn in that frame with the
strands it kept, at their true width, and has all of them four frames later: for those frames it is thinner than it
should be, by up to three quarters of its strands. The scale is held for the first three frames after the culled
rasterizer takes over from the direct one, whose frames leave no stamps. In frames the direct rasterizer draws the
scale is not applied.

| `hair.render` field | Default | Meaning |
|---|---|---|
| `raster` | `auto` | `direct`, `culled`, or `auto`: the culled rasterizer in a frame where a groom in view has strands at least `auto_width_pixels` wide, the direct one otherwise (it lets go a tenth below). `BR_HAIR_RASTER=direct` or `culled` in the environment sets the start value. |
| `auto_width_pixels` | 0.75 | The strand width on screen (the groom's widest strand, at the distance of the middle of its bounds) from which `auto` takes the culled rasterizer. |
| `first_pass` | `previous` | Which chunks the first pass draws: `previous`, `prefix`, `both`, or `all` (no late pass). |
| `opaque_coverage` | true | A strand that covers its pixel whole saturates the pixel's coverage. Both rasterizers. Off: it adds 1, as before, and `occlude_opaque` is off with it. |
| `occlude_opaque` | true | Such a strand hides the hair behind it. |
| `occlude_saturated`, `saturation` | true, 10 | A pixel whose coverage in front of its split depth reaches `saturation` pixels' worth hides the hair behind that depth. |
| `hidden_detail` | 0.25 | The share of its strands a cluster keeps in the pool while no pixel shows it. 1: all. |
| `detail_rate_up`, `detail_rate_down` | 0.25, 0.02 | How far the scale moves in a frame. |
| `hint_decay` | 0.8 | How long the first pass's guess remembers a tile's larger hidden depths: the share of the difference kept a frame. 0: the last frame's depths alone. |
| `raster_profile` | false | Count the loop iterations of the two passes' waves (`wave_iterations`). |
| `late_pass` | true | False, for measurement only: the late pass draws nothing, and hair the first pass left out is missing. |

**Not built:** a guess that survives the camera's jitter (one per jitter phase, or a first pass ordered front to back); a limit on the segments drawn in a frame or over a screen tile; 32-bit layer keys; occlusion baked into
the groom; a sort of the first pass's chunks by tile, or batching of pixel writes across a wave; any use of the hidden
depths by the shadow view, which has its own selection.



### The coverage target

`Hair.Coverage` is an `R32_UInt` texture the size of the visibility target. With `coverage_accumulate` on, the
rasterizer adds (`InterlockedAdd`) the coverage of every footprint pixel that is in front of the opaque surface, written
or not, in units of 1/65536 (`HAIR_COVERAGE_UNIT`): the summed coverage of the hair over each pixel, overlaps counted,
in the manner of the additive coverage layer of *Indiana Jones and the Great Circle*. It is cleared every frame. On
the surface path nothing shades from it: it is for the `coverage` [debug view](#debug-view) and for measurement, and is
off unless asked for. On the [layered path](#the-layered-path) it is always accumulated, and the composite takes from it
the coverage of the hair beyond a pixel's three layers. The culled rasterizer sums coverage in two targets, this one
for the hair in front of a pixel's split depth and `Hair.CoverageFar` for the rest, and the composite adds them. The
counters `pixels_covered` and `coverage_sum` ([Checking it](#checking-it)) are the same sum over the frame and do not
need the target. With `coverage` `off` the sum still uses the end-to-end joint rule, so the two modes report the same
coverage.

### The resolve

`Hair::Resolve` runs one thread per pixel of the hair visibility target. The rasterizer already rejected hair behind
the opaque surface, so every pixel that holds a key is resolved. Hair needs no extension point for this: every target
below is a named renderer resource (`Builtin::Surface::*`, `Builtin::PrimaryCamera::*`) that a plugin pass can declare
for unordered access, as SARP's grass does for the canonical surface.

**The hit.** The key names a visible cluster, a strand and a segment, and so two rows of the strand pool. The shader
projects them as the rasterizer did, takes the pixel centre's nearest point on the segment, and converts that screen
parameter to a perspective-correct one. From it come the world position on the centreline, the previous frame's
position, the width, and the tangent (the segment's direction, toward the tip).

**What a hair pixel gets.** This is the full set a pixel needs for the passes after material evaluation to treat it as
an opaque surface in the demo's raster pipeline:

| Target | Written | Read by |
|---|---|---|
| `Builtin::Surface::BaseColorOpacity` | the instance's base colour, opacity 1 | deferred shading, stochastic lighting |
| `Builtin::Surface::NormalRoughness` | the shading normal and the instance's roughness | deferred shading, ambient occlusion, stochastic lighting, reflections |
| `Builtin::Surface::SpecularAo` | F0 0.04, occlusion 1 | deferred shading, stochastic lighting |
| `Builtin::Surface::Emissive` | 0 | deferred shading |
| `Builtin::Surface::Motion` | current minus previous position in NDC, both projected without jitter | the upscaler, motion vector dilation, stochastic lighting's reprojection, screen-space reflections |
| `Builtin::Surface::Identity`, `Builtin::Surface::Records` | one record at the pixel's own index: semantic family 0, payload profile 0, no flags, OpenPBR table row 0 | deferred shading |
| `Builtin::PrimaryCamera::LinearDepthMap`, mip 0 | the key's depth (linear view depth) | virtual shadow page marking, ambient occlusion, light culling, deferred shading (position and the sky test); its other mips are made from it afterwards |
| `Builtin::PrimaryCamera::ProjectedDepthTexture` | `-M22 + M32 / depth` of the unjittered projection | the upscaler, motion vector dilation, the lens passes |
| `Builtin::Surface::DeviceDepth` | the same value | nothing in the renderer today |

Not written:

- `Builtin::Surface::Payload0` and `Payload1` (coat and fuzz): payload profile 0 tells readers to take them as zero.
- `Builtin::PrimaryCamera::DepthTexture`, the hardware depth buffer. A compute pass cannot write it. Screen-space
  reflections, the specular IBL pass, ray traced reflections and the debug spheres read it, and see the surface behind
  the hair.
- The opaque visibility buffer. Passes that decode it directly (ray traced shadows and reflections, CLod inspection)
  see the surface behind the hair.
- `Builtin::DebugVisualization`: the renderer's own debug outputs (`outputType`) show the surface behind the hair.

**Material.** A placeholder: the instance's base colour and roughness (`hair.attach`) as a dielectric with F0 0.04,
lit by the existing surface model. The shading normal is the direction to the camera with its part along the strand
removed, so a strand is lit as the side of a cylinder facing the viewer and its brightness follows the angle between
the light and the strand. Across a strand more than a pixel wide the normal turns from one edge to the other, which
shows the strand as round. The tangent is computed and is not stored: the canonical surface holds one normal, a
roughness and a specular colour, and its two payloads belong to the coat and fuzz lobes. A hair BSDF needs a place for
it (a payload profile of its own, or its own target).

**Motion vectors.** The evaluate pass writes each pool vertex's position under the previous frame's root frame and
previous model matrix; the resolve interpolates both positions at the hit and projects them with the camera's current
and previous view and unjittered projection, the convention of the renderer's own surfaces
(`ComputeClodMotionVectorFromPrevWorld`). They cover the entity's motion, the skeleton's and the camera's. The previous
position is this frame's selection at last frame's root, so a level-of-detail change does not appear as motion.

**Shadows.** Hair receives shadows: the virtual shadow map marks pages from the linear depth the resolve wrote, and
deferred shading looks them up at the hair pixel's position. On this path hair casts none
([Shadow casting](#shadow-casting) is the layered path's).

**Cost.** `Hair.Counters` row 16 counts the pixels resolved in a frame (`pixels_resolved`).

### The layered path

`hair.render {path: "layered"}` turns the resolve off and three things on: the rasterizer keeps
[three layers](#the-rasterizer) a pixel and always accumulates the [coverage target](#the-coverage-target),
`Hair::Shade` lights every drawn strand vertex, and `Hair::Composite` blends the layers over the lit frame. The scheme
is that of *Strand Hair in Indiana Jones and the Great Circle* (SIGGRAPH 2025, Advances in Real-Time Rendering); where
this differs from it, it says so.

**Shading, per strand vertex.** `Hair::Shade` writes a lit colour into `Hair.PoolColors`, parallel to the pool (three
half floats in a `uint2`), for the rows the rasterizer marked this frame: the vertices of every segment that reached
some pixel's layers ([The rasterizer](#the-rasterizer)). The mark is the frame's number in the row's fourth half, which
a colour leaves zero. Vertices of segments behind the opaque surface or behind three strands everywhere are not lit; the
marked ones are, whether or not their segment is still in a layer at the end.

- **The shade list.** The marked rows are scattered through the pool, under half of it in the views measured, and a
  pass that walks the pool and skips the others runs most of its thread waves with idle lanes. `Hair::ShadeList` runs
  one thread per pool row right after the rasterizer and appends the marked rows to `Hair.ShadeList` (one atomic a
  wave; the order is the pool's within a wave); `Hair::Shade` then runs one thread per row of the list. The GPU alone
  knows the list's length (`shade_list` in the counters), so the shade pass's dispatch covers the most rows the frame's
  pool can use and the groups past the end of the list return at once: there is no indirect dispatch. The list is the
  frame's set of lit vertices, and the rows the density volume's marches start from.

- **The shading point is the vertex, on the strand's centreline.** A strand has one colour across its width. A strand
  many pixels wide is therefore not shaded as round (the surface path turns its normal across the strand), and a
  shadow's edge is resolved along a strand only, at the spacing of its vertices: `segment_pixels` on screen, 4 pixels by
  default. The tangent is the direction between the vertex's two neighbours on its strand.
- **The light loop is hair's own** (`HairShadeCS`), built from the renderer's lower-level pieces: `EvaluateLightAtPoint`
  and `LightIncidentRadiance` (`lightModel.hlsli`) for what a light delivers, and `EvaluateLightOcclusion`
  (`lighting.hlsli`) for its shadow. The renderer's loop (`EvaluateDirectLighting`) is the surface model's and returns
  nothing for a light behind the surface; a fibre has no behind.
- **The response is one function,** `HairEvaluateBsdf(tangent, view, light, material, path, lobeMask)` in
  [`hairBsdf.hlsli`](../Plugins/Hair/shaders/hairBsdf.hlsli): the radiance toward the viewer per unit of irradiance
  arriving from the light on a plane facing it, so the cosine is included
  ([The hair shading model](#the-hair-shading-model)). `path` (`HairLightPath`) is what the hair between the light and
  the vertex, and around the vertex, does to it: the number of fibres in the way, which the vertex counts by marching
  its groom's [density volume](#the-density-volume) toward the light, and the groom's scatter table
  ([Multiple scattering](#multiple-scattering)). **This is how hair shadows itself.** An instance without a volume
  (`hair.volume {enabled: false}`, or no voxels left for it) is lit by every light in full, as before the volume
  existed: the far side of its groom is as bright as the near side.
- **The material** is the vertex's strand's: its instance's, with the strand's own offsets of absorption and roughness
  ([Material parameters](#material-parameters)).
- **Which lights** (`lights`). `list`, the default, walks every active light and tests its range. `grid` reads the
  vertex's froxel in the [light grid](lighting.md#the-light-grid), its forward run, which exists only in a recipe with
  `ClusterLodAlphaTechnique`. The demo's recipe has none: there the forward run is the opaque run, a froxel in front of
  the nearest opaque surface holds no light at all, the sun included, and `grid` leaves hair in front of the sky or the
  floor unlit in tile-sized blocks (measured below). `list` is right in every recipe and costs a range test per light
  per vertex. A vertex off screen or behind an opaque surface also has no froxel of its own, which `list` does not care
  about.
- **Shadows** are looked up at the vertex with the "normal" the lookup wants set to the direction of the light, so its
  receiver offset moves the lookup a few shadow texels toward the light. Hair receives the sun's
  [virtual shadow map](lighting.md) and spot and point lights' faces. **The shadow map only holds the pages that pixels
  of the frame's depth buffer asked for, and on this path hair is not in that buffer** when pages are marked. A vertex
  whose page nothing asked for is looked up in up to two coarser clipmaps (`shadow_coarser`); with no page there either
  it is lit (`shadow_missing`), as is one that reads a texel nothing was drawn into (`shadow_cleared`). The error is
  therefore always toward light, never a false shadow. Pages are marked through buffers private to the shadow map's
  passes, so hair cannot ask for its own without a core change; the counters bound it instead
  ([Layered path measurements](#layered-path-measurements): no vertex in any measured view found no page). The lookup's
  soft-shadow rays are randomized per pixel and frame; a vertex uses the pixel it projects to.
- **The environment** is the same response integrated against the renderer's image-based light, by four directions
  ([The environment](#the-environment)), each occluded by a march of the density volume that way, times `ambient`. It
  is added only in a frame that has image-based lighting.
- In stochastic lighting mode there is no shadow map and no entry for a receiver that is not a surface: hair is lit by
  every light, unshadowed.

**The composite.** `Hair::Composite` runs one thread per pixel. For each layer a pixel holds, nearest first, it finds
the hit again (`HairReconstructHit` in [`hairHit.hlsli`](../Plugins/Hair/shaders/hairHit.hlsli), shared with the
resolve), takes the layer's coverage `c` from its segment and width with the rasterizer's own arithmetic (the true
width over the footprint's, at least a pixel), and its colour `L` from the two vertex colours of the segment,
interpolated at the hit. Then:

```
front:      colour = sum over layers of T * c_i * L_i,   T = product of (1 - c_j) over the layers in front
remainder:  r = coverage target - (c_1 + c_2 + c_3)                      (only where all three layers are held)
            a = T_3 * (1 - exp(-r))                                      T_3: what the three layers let through
            colour += a * (sum of w_i * L_i) / (sum of w_i),   w_i = c_i * 0.25 ^ (3 - i)
alpha = 1 - T_3 + a;   scene colour = colour + scene colour * (1 - alpha)
```

- The three layers are blended exactly, so a pixel under three strands or fewer has no remainder and no approximation.
  A layer behind one that covers the pixel whole is not looked up: nothing of it shows.
- In a frame the culled rasterizer drew, a pixel whose hidden depth hair lowered is opaque from that depth on, and its
  remainder is taken as saturated whatever the sums hold: the hair skipped behind that depth is not in them.
- **The remainder** is the hair beyond the third layer, of which only the summed coverage is known. It is lit as a
  weighted average of the three samples that favours the farther ones, with the weights of the Indiana Jones deck
  (slide "Coverage reconstruction", read from the rendered slide: `w_i = C_i * 0.25^(3 - i)`).
- **Where this differs from the deck:** the deck converts the whole sum to a total coverage with a fitted quadratic
  (`x - 0.5 x saturate(0.5 x)`, chosen because it looks more voluminous than the better fit) and subtracts the front
  three's. Here the sum is converted only for what the three layers do not account for, as thin occluders overlapping at
  random (`exp`), so that the front is exact and the two agree where there is no fourth strand. One strand of coverage 1
  beyond the third layer is under-counted by this (63% where it is 100%).
- Hair is blended premultiplied over `Builtin::Color::HDRColorTarget`, after the opaque frame is lit, and so over the
  sky and over anything in front of which the rasterizer's opaque depth test let it through.

**Depth and motion.** A pixel whose hair alpha is at least `depth_alpha` (0.5) takes hair's, in the forms the passes
after the composite read: `Builtin::Surface::Motion` (current minus previous position in NDC without jitter, as the
resolve writes it) and `Builtin::PrimaryCamera::ProjectedDepthTexture`, both from the pixel's **nearest** layer, as
Unreal's default takes them. Below the threshold the pixel keeps the motion vector and depth of what is behind the
hair. The readers are motion vector dilation, the upscaler and the lens passes. The linear depth map is not written:
everything that read it ran before the composite, and what reads it afterwards is the next frame's history, which would
then cull and reproject against hair that only half covers its pixels. `Builtin::Surface::DeviceDepth` is not written
either.

**The reactive mask.** One motion vector cannot describe a pixel that is part hair and part background when the two
move differently. `Hair.ReactiveMask` says where: `reactive_strength` times the share of the pixel that does not move as
its motion vector says (the background's share where the pixel took hair's vector, hair's share where it did not),
times how far apart the two motions are, reaching 1 at `reactive_pixels`. Still hair over a still background is not
reactive at all. The upscaler gets it through an extension point this step added to the renderer,
[`Extensions/UpscalerReactiveMask.h`](../BasicRenderer/include/BasicRenderer/Extensions/UpscalerReactiveMask.h):
`br::render::SetUpscalerReactiveMask(texture)` names a render-resolution single-channel texture that the upscaling pass
then declares and hands to DLSS Super Resolution as its bias-current-colour hint and to FSR as its reactive mask; null
(the default) takes it away, and DLSS Ray Reconstruction and no upscaling ignore it. The runtime sets it while the
layered path is on with `reactive` true and clears it otherwise.

| `hair.render` field | Default | Meaning |
|---|---|---|
| `path` | `surface` | `surface`: the resolve. `layered`: this section. |
| `depth_alpha` | 0.5 | The hair alpha from which a pixel takes hair's depth and motion vector. |
| `coverage_saturation` | 0 | Above 0: the coverage target stops taking adds at this many pixels' worth. The alpha it then gives is 1 to within `exp(-(saturation - 3))`. 0: never. |
| `reactive` | true | Give the upscaler the reactive mask. |
| `reactive_strength`, `reactive_pixels` | 1, 1 | The mask's scale, and the motion difference in pixels at which it is reached. |
| `ambient` | 1 | Scale of the environment term. |
| `lights` | `list` | `list`: every active light. `grid`: the light grid's forward runs (a recipe with CLod alpha only). |
| `raster` and the rasterizer's other controls | `auto` | Which rasterizer draws the path, and how: [The culled rasterizer](#the-culled-rasterizer). |

**What the rest of the frame sees.** Hair is in none of the surface targets, the linear depth or the opaque visibility
buffer, so everything before the composite works on the scene without hair: virtual shadow page marking, ambient
occlusion (hair gets none and casts none on the scalp), the light grid's depth bounds, deferred shading, stochastic
lighting, screen-space and ray traced reflections (hair is in no reflection, and reflects nothing but the diffuse
environment), and the specular image-based light. After it, bloom and the tonemapper see hair as part of the scene
colour, and the upscaler, motion vector dilation and the lens passes see its depth and motion where it took them.
[Layered path measurements](#layered-path-measurements) has captures and numbers.

**Not there yet:** shadows cast by hair from spot and point lights, and on the head it grows on at the shadow map's
default resolution ([Shadow casting](#shadow-casting)), any
anti-aliasing of a strand wider than a pixel (it has hard edges, as on the surface path), sorting against the
renderer's other transparents, and more than one motion vector a pixel.

### The density volume

On the layered path every groom a view shows gets, every frame, a box of voxels holding how much hair is where
(`hair.volume`). The shade pass marches it from every lit vertex toward every light and toward the environment's
directions and counts the fibres in the way; what that count does to the light is
[Multiple scattering](#multiple-scattering). Nothing of a volume outlives its frame.

**Which grooms, and how fine** (`PlanVolumes` in [`HairRuntime.cpp`](../Plugins/Hair/src/HairRuntime.cpp), on the CPU,
once a frame). An instance gets a volume when a view shows any of its groom: the test the simulation uses
(`OnScreenExtent`, [Following the view](#following-the-view)). Its box is the groom's rest bounds grown on every side
by `margin` (a tenth of their longest side), in the space the roots follow: the entity's, or the attach bone's, so the
grid moves with the head and hair at rest stays in the same voxels. A groom whose roots follow their own joints
(`per_root`) is taken to follow its entity. The voxels are cubes. Their side is `voxel_pixels` (8) on screen at the
groom's distance, but no smaller than gives `max_resolution` (96) voxels along the box's longest side: a groom far away
has few, large voxels, and one that fills the view stops at 96.

**The budget.** The volumes' voxels are consecutive ranges of one buffer, `Hair.VolumeVoxels` (2,097,152 voxels of 4
bytes), handed out in instance order from its start, anew every frame. `voxel_budget` (0: the buffer) is the most all
of them may take. Over it, every volume's voxels are enlarged by one factor, found by bisection, until they fit
(`coarsening` in `hair.volume`'s reply); a volume is never coarser than 2 voxels along its longest side, and if those do
not fit either, the instances smallest on screen get no volume (`dropped_volumes`) and are lit as without one, by every light in full.
An instance whose entity's matrices the CPU does not have that frame gets none either (`unknown_instances`). There is
no sharing of one volume between far grooms: a far groom's own volume is a few dozen voxels.

**What a voxel holds** (`HairVolumeGPU` in
[`HairRuntimeGPUTypes.h`](../Plugins/Hair/include/Hair/HairRuntimeGPUTypes.h)): one `uint`.

- Bits 30..0, the fibre density: the summed length times diameter of the segments in the voxel over the voxel's volume
  (the projected area of fibre per volume, 1/m), in units of 1/4096 per metre. A ray crosses, per metre, that density
  times the sine of its angle to the fibres; the volume keeps no directions, and a march uses the mean of that sine over
  directions, pi / 4 (`HAIR_VOLUME_ISOTROPIC_SINE`).
- Bit 31, the scalp: see below.

**Filling it.** `Hair::Voxelize` ([`hairVolume.hlsl`](../Plugins/Hair/shaders/hairVolume.hlsl)) runs after evaluation:
it zeroes the voxels in use and then runs one thread per row of the strand pool, like the rasterizer. A row that starts
a segment deposits the segment's length times its diameter at one point per voxel of its length (four at most).

- **From the pool, not from what was rasterized.** The pool holds every strand selection kept for the frame, whether
  the rasterizer then finds it in front of other hair or not, at the frame's level of detail and where the simulation
  left it. A groom's interior is in its volume however little of it is seen.
- **The same density at every level of detail.** A diameter is the pool's, which carries the LOD's width scale: the
  strands a cluster draws have the summed width of all its strands (`HairLodCoverageWidthScale`,
  [Evaluation](#evaluation-and-the-strand-pool)), so length times diameter summed over the drawn strands is that of the
  whole groom, and the boundary strand's deposit fades in with its width.
- **Spread over eight voxels.** A deposit is shared by the eight voxels around it by its distance to their centres
  (`splat` `trilinear`), so the density is smooth across voxels and hair that moves through the grid changes it
  gradually. `nearest` gives it all to one voxel, at an eighth of the atomic adds.
- **Whole units.** A share is rounded down or up at random in proportion to its fraction, by a number that is the same
  every frame for the same pool row, so the sum over a voxel's many segments is right and does not flicker.
- Hair outside the box is not in the volume (`volume_outside` counts its segments).

**The scalp.** The volume holds hair only, and what hair grows on is opaque: light from the far side of a head does
not reach the hair on the near side. For the sun the shadow map says so; for the environment's directions and for
lights without a shadow map nothing did. So a strand's first segment marks the voxels under its root as scalp
(`scalp_depth`, 3 voxels, starting 2 voxels along the strand's own direction into the head): a shell under the skin
wherever hair grows, and a march that reads it ends there with nothing arriving. It starts 2 voxels down so that no
voxel a strand vertex can be in is marked. Where no hair grows (a face, a bald patch) there is no shell.

**Marching it** (`HairMarchVolume` in [`hairVolume.hlsli`](../Plugins/Hair/shaders/hairVolume.hlsli)). From the vertex
toward the light, from `bias` (1) voxels along the ray to where the ray leaves the box (or reaches a spot or point
light), in `steps` (8) samples toward a light and `environment_steps` (4) toward an environment direction. Sample `i`
of `n` is at the fraction `((i + offset) / n)^2` of that distance and stands for the stretch between the squares
around it: short steps at the vertex, where the hair that shadows it most is, long ones toward the far side. The
first two samples are interpolated between the eight voxels around them; the others read one voxel. `offset` is the
vertex's own number in [0, 1), the same in every frame and at every level of detail that draws the vertex (`jitter`
`vertex`; `frame` changes it every frame), and each march of a vertex takes a different one.

- **Why the bias.** The deposits are spread over a voxel either side of the hair, so a vertex on the lit face of a
  groom reads hair between itself and the light that is really beside and behind it. Skipping the first voxel of the
  ray takes most of that away, and with it whatever really is within a voxel of the vertex.
- **What a voxel cannot hold.** A layer of hair thinner than a voxel is one density to every vertex in it: the
  outermost fibre and the innermost get the same count. The measurements
  ([Self-shadowing measurements](#self-shadowing-measurements)) say what that costs.

The arithmetic of deposits and marches is [`HairVolumeShared.h`](../Plugins/Hair/include/Hair/HairVolumeShared.h),
one text for the shaders and for `HairCoreTests`, which builds and marches a slab of known density with it.

| `hair.volume` field | Default | Meaning |
|---|---|---|
| `enabled` | true | Off: no volume is built, and hair is lit exactly as before the volume existed. |
| `dual_scattering` | true | Off: the fibres in the way only shadow (the light that met none arrives, nothing else). |
| `voxel_pixels` | 8 | The side of a voxel on screen at the groom's distance. |
| `max_resolution` | 96 | The most voxels along a volume's longest side. |
| `voxel_budget` | 0 | The most voxels over all volumes; 0: the buffer's 2,097,152. |
| `steps`, `environment_steps` | 8, 4 | Samples of a march toward a light, and toward an environment direction. |
| `bias` | 1 | Voxels a march skips at its start. |
| `density_factor` | 0.7 | Zinke et al.'s `d_f` and `d_b` ([Multiple scattering](#multiple-scattering)). |
| `margin` | 0.1 | The box's growth beyond the groom's rest bounds, as a share of their longest side. |
| `scalp_depth` | 3 | Voxels of scalp shell under every root; 0: none. |
| `splat` | `trilinear` | How a deposit is shared between voxels. |
| `jitter` | `vertex` | Whether a march's offset is the vertex's own always, or new every frame. |

The reply also has the last frame's `volumes`, `voxels`, `voxel_bytes`, `capacity_voxels`, `budget_voxels`,
`high_water_voxels`, `coarsening`, `largest_resolution`, `smallest_voxel`, `largest_voxel` (metres), `dropped_volumes`
and `unknown_instances`; `hair.stats` has them under `volume`, and each instance's `volume_resolution` and
`volume_voxel_size`.

**Not built:** directions in the volume (a mean tangent or an orientation moment, for an extinction that depends on
the angle between the ray and the fibres); a lookup of the volume from opaque surfaces (hair's shadow on the head);
the volume as input to the simulation (hair against hair): `Hair::Voxelize` runs after `Hair::Simulate`, so a solver
would read the previous frame's volume or the pass would have to move.

### Shadow casting

On the layered path hair is drawn into the sun's [virtual shadow map](lighting.md) (`hair.shadows`, on by default), so
it shadows the ground, the head it grows on and everything else that takes the sun's shadow through the deferred path.
`HairExtension` is a caster provider of the shadow map
([`Extensions/VirtualShadowCasterProvider.h`](../BasicRenderer/include/BasicRenderer/Extensions/VirtualShadowCasterProvider.h),
as SARP's grass is): a graph that has a shadow map gathers three passes from it, which run once the shadow map has
decided which pages take depth this frame and before anything looks a shadow up. Everything is in
[`hairShadow.hlsl`](../Plugins/Hair/shaders/hairShadow.hlsl). No core file was changed for it. On the surface path hair
casts no shadow.

**What the provider reaches.** The sun's clipmaps only: 22 orthographic views around the camera, each twice as coarse
as the one before, in pages of 128 texels that exist only where a pixel of the main view asked for one. A page holds
one depth a texel. Spot and point lights' faces are not offered to a provider (below).

**Which hair** (`Hair::ShadowSelect`, `HairShadowSelectCS`). One dispatch per active instance, one thread per cluster,
like the main view's select pass, and independent of it: whether the main view shows a cluster plays no part. A cluster
is kept when its bounding sphere, seen from the sun, lies over a page that some clipmap draws this frame (the shadow
map's active-block lists, as grass culls). That is what "its shadow can reach the view" comes to: the shadow map only has
pages under receivers the main view shows, so a groom behind the camera whose shadow falls in front of it is kept, and a
groom in view whose shadow falls on nothing in view is kept only for the pages of its own head. Pages are coarse (128
texels: metres across near the camera), so "over a drawn page" is generous.

- **Detail from the shadow texel.** The main view's rule with the shadow texel in the pixel's place:
  `l = clamp((spacingTexels / (errorPixels * detailScale)) ^ detailExponent, minDetail, 1)`, the texel being the finest
  one among the clipmaps the cluster was kept for. `detail_scale` (2) counts a shadow texel as that many: the shadow
  view draws the strands the main view would at twice the distance. Width compensation is the main view's
  (`HairLodCoverageWidthScale`), so a cluster drawn with fewer strands covers the same share of texels.
- **Vertex level from the texel alone.** `f = log2(lengthTexels / segmentPixels)`, without `detail_scale`: a strand
  that bends, drawn with fewer vertices, runs along its chords inside the shape it has, and its shadow is smaller
  ([Cast shadow measurements](#cast-shadow-measurements): a fifth less on the floor at half the vertices). Fewer strands
  cost nothing of the kind.
- **One level for every clipmap** a cluster is drawn into, the finest one's. A coarser clipmap gets more strands than it
  needs; their widths still sum to the cluster's.

**A list and a pool of its own.** The kept clusters go to `Hair.ShadowClusters` (rows of `HairVisibleCluster`, with the
clipmaps each is drawn into in `Hair.ShadowClusterMasks`) and take ranges of `Hair.ShadowPoolVertices`, a second pool a
quarter the size of the main view's (1,048,576 rows; 38 MB with its segment ids and the list). The main view's pool is
not reused: its strands are chosen for the screen (more of them, at a finer vertex level, and none for a cluster outside
the frustum), and reading a prefix of them would have tied the two selections together. What the second pool costs is
its memory and a second evaluation of the strands the shadow view keeps: 3,553 of a 50,000-strand groom near the camera.
A cluster that does not fit either is dropped for the frame and counted (`shadow_overflow_clusters`).

**Evaluation** (`Hair::ShadowEvaluate`). The evaluate pass's own shader, `HairEvaluateCS`, run over the shadow list
into the shadow pool (`HAIR_EVALUATE_SHADOW`): the strands are where the main view's are, simulated when their instance
is, from the same guide rows of the same frame. An instance no view shows is not simulated
([Following the view](#following-the-view)) and the shadow view does not change that: **a groom out of view casts the
shadow of its rigid pose**, whether or not it keeps a simulated shape. When the camera turns to it, its simulation
starts again (from the kept shape, if it has one) and the shadow takes the simulated shape in that frame; with the
default pull toward the rest pose the two differ by the hair's sag, millimetres to a centimetre or two. Such strands are
counted (`shadow_unseen_strands`), and the main view's check on them (`unseen_strands_drawn`) stays its own.

**The rasterizer** (`Hair::ShadowRaster`, `HairShadowRasterCS`). One thread per row of the shadow pool; a row that
starts a segment draws it into every clipmap its cluster was kept for. Both ends are projected with the clipmap's view,
and the segment is walked as in the main view: a strip at least one texel wide, the strand's segments meeting end to
end. A segment whose strip touches no page that takes depth this frame stops there. Depth is the linear depth along the
light, written with the caster helper's encoding and atomic minimum (`VirtualShadowCasterWriteDepthAtVirtualTexel`),
which also marks the page as holding content.

**Coverage.** A page holds one depth a texel and hair is not opaque to light, so a texel of the strip is written with
the probability of the strand's coverage of it: its true width (the pool's, with the level of detail's scale) over the
strip's. Strands over a texel are independent, so the share of texels left open under a layer of hair is the share of
light it lets through, and the shadow lookup's rays and the upscaler average that into a partial shadow.

- `temporal` (the default): the random number is of the texel, the strand, the clipmap and the frame. Where hair is
  redrawn every frame the pattern is new every frame, and what the receiver shows is the upscaler's average: smooth in
  a still view (measured below).
- `fixed`: of the texel, the strand and the clipmap. Nothing crawls, and a sparse layer is a field of texel-sized
  blocks, each several pixels across: shadow texels are larger than pixels (below).
- `opaque`: every texel of the strip is written. A strand then shadows as if a texel wide, however thin it is.

**Layers, caching and invalidation.** Hair is a deformable caster (`VirtualShadowCasterMobility::SkinnedOrDeformable`).
The shadow map keeps two layers. In the clipmaps it classes as dynamic (those within the largest radius any caster
asks for; 12 of the 22 in the demo) a deformable caster writes the dynamic layer, which every frame starts as a copy of
the cached depth of the rigid casters and is drawn again: there hair's shadow is this frame's, by construction, and
`temporal` has a new pattern each frame. In the other clipmaps hair writes the cached pages themselves, when the shadow
map draws them; a cached page holds the pattern of the frame it was drawn in, whichever the mode, and a stale shadow
until it is drawn again.

- `HairExtension::GetRequestedDynamicShadowRadius` asks for `dynamic_radius` (16 m) while hair casts shadows. The demo's
  own radius (procedural wind's reach) is larger, so there the request changes nothing.
- `HairRuntime` (`PlanShadows`, once a frame on the CPU) enqueues bounds on the shadow map's invalidation queue for a
  groom whose matrix differs from the last frame's, that is simulated, that starts or stops casting (attached, cleared,
  the feature or the path switched), and for every groom when a control that changes what is drawn changes: its bounding
  sphere where it was and where it is. A groom that stands still enqueues nothing. The queue only applies a deformable
  caster's bounds to the clipmaps that are not dynamic, so near the camera they cost nothing. A simulated groom counts
  as moving in every frame, whether its hair moves or not: the CPU does not know.

**Hair's own shading: no double shadow.** The [density volume](#the-density-volume) already shadows hair with hair, and
the shade pass multiplies that with the shadow map's answer. With hair in the map a vertex would be shadowed by its own
groom twice, so a vertex that has a volume asks the map two narrower questions (`hairShade.hlsl`):

- *What is in the way outside my groom's box?* The same lookup as before (`EvaluateLightOcclusion`, soft), started at
  the point where the ray to the sun leaves the volume's box. A clipmap is orthographic, so that moves the receiver
  along the light and changes nothing else: every occluder outside the box shadows as it did, another groom's hair
  included, and nothing inside the box is seen.
- *What opaque thing is in the way inside the box?* The head, above all. The renderer's single-texel lookup
  (`CLodVirtualShadowLookupDirectionalOcclusion`) is given the shadow map's **cached pages** in place of the frame's:
  in a dynamic clipmap those hold the rigid casters and no hair. An occluder there, nearer the sun than the vertex by
  more than `receiver_bias` plus two texels and no farther than the box's edge, shadows the vertex fully. The cached
  pages have no name a pass can declare; the extension hands the shade pass the texture the provider interface gave it.

What this leaves: the second answer is one texel's, hard where the first is soft; it does not know casters that are
themselves drawn into the dynamic layer, so **a skinned head shadows its own hair only through the volume's scalp
shell**, which a march can step over; in a clipmap that is not dynamic the cached pages hold hair too and the second
question is not asked. A vertex without a volume (the volume off, or out of budget) is looked up as before, and is then
shadowed by the hair in the map, its own groom's included. A third way was built first and removed: leaving one texel in
sixteen free of hair for hair receivers to read. At the shadow map's resolution those texels are plain holes in the
shadow on the ground.

**What the shadow map's resolution allows.** In the demo's configuration a receiver near the camera reads the fifth
clipmap or a coarser one, whose texel is about 2 cm, a dozen screen pixels at 2.5 m; and a receiver's lookup starts
two texels off its surface. So a groom's shadow on the ground is its silhouette resolved to 2 cm, and **hair within
about 4 cm of the head casts nothing on it**: the scalp under hair is as lit as without this. With the shadow map's
level-of-detail bias lowered until a texel is under 2 mm (`clodDirectionalVirtualShadowAutoLodBias` off,
`clodDirectionalVirtualShadowLodBias` 2), the shadow appears on the head; the measurements have both.

| `hair.shadows` field | Default | Meaning |
|---|---|---|
| `enabled` | true | Off: hair is in no shadow map, the three passes record nothing, and hair is lit exactly as before. |
| `detail_scale` | 2 | A shadow texel counts as this many when the shadow view picks a cluster's strand count. |
| `force_detail` | 0 | Above 0: every cluster takes this `l` in the shadow view (its vertex level stays the texel's). For testing. |
| `coverage` | `temporal` | `temporal`, `fixed` or `opaque`: above. |
| `receiver_bias` | 0.005 | Metres, on top of two texels, by which an opaque occluder inside a groom's box must be nearer the sun than a hair vertex to shadow it. 1 turns that read off in effect. |
| `dynamic_radius` | 16 | The distance from the camera within which hair asks the shadow map to redraw deformable casters every frame. |

The reply also has the last frame's `active` (hair was drawn into the shadow map), `instances`, `moved_instances`,
`invalidations` (bounds enqueued), `total_invalidations`, `dropped_invalidations` (the queue was full), and the shadow
map's own `sun_clipmaps` and `dynamic_clipmaps`. The GPU's counts are in `gpu_frame` ([Checking it](#checking-it)).

**Spot and point lights.** Hair casts no shadow from them, and the provider interface cannot give it one: it hands a
provider the sun's clipmaps and a depth write for them. A provider that drew into local faces would need, from the
core: the faces themselves in the build context (how many are live, each face's slices, its light, its near and far
plane, whether it is a cube face, whether it is dynamic, and its perspective view, which the compact shadow camera
buffer a provider gets today does not hold); a depth write for a face that maps the face's coordinates into each mip's
rectangle on its canvas and writes **every mip the caster covers**, which is the rule every raster path of the faces
keeps ([lighting.md](lighting.md#pages)) and what the software rasterizer does there (it divides by w, interpolates
1/depth and walks the mips); active-block lists that cover the faces' slices; and invalidation bounds that reach faces
(the queue's mask names the sun's clipmaps). The density volume could stand in for those lights on opaque receivers
instead: a full-screen pass that marches each groom's volume from the opaque pixel toward each local light in range and
keeps a transmittance per pixel and light. The volume has what that needs; the deferred pass does not: its light loop
takes occlusion known beforehand for the sun alone (`DirectLightingOptions::sunOcclusion`), so the core would have to
take a per-light occlusion term from an extension. Neither was built.

**Not built:** a level of detail per clipmap; shadows of simulated hair for grooms out of view; the shadow of hair on
the surface path; transmitted colour (a shadow is grey, whatever the hair's).

### The hair shading model

A fibre's response to light on the layered path is the real-time form of Marschner et al.'s model: three lobes, **R**
(reflected off the cuticle: white, the primary highlight), **TT** (through the fibre: coloured, forward, what makes hair
glow against the light) and **TRT** (through, reflected inside and out again: more strongly coloured, the secondary
highlight), each the product of a longitudinal function M of the two inclinations to the fibre, a distribution D around
the fibre and an attenuation A (Fresnel and absorption). The surface path does not use it.

**One text for the shader and the CPU.** The scattering function is
[`HairBsdfShared.h`](../Plugins/Hair/include/Hair/HairBsdfShared.h), written like `HairLodShared.h` to compile as HLSL and
as C++: [`hairBsdf.hlsli`](../Plugins/Hair/shaders/hairBsdf.hlsli) includes it for the shade pass and adds a strand's
material and the entry point, and `HairCoreTests` compiles the same file and checks it
([`HairBsdfTests.cpp`](../Plugins/Hair/tests/HairBsdfTests.cpp)). So the shader and the reference cannot differ, and there
is no GPU self-check for it. `hairBsdf.hlsli` also declares `HairSampleBsdf` and `HairPdfBsdf` for a path tracer; they
have no bodies.

**Conventions.** `tangent` runs from root to tip; `view` and `light` point away from the shading point; the inclination
of a direction is its angle above the plane normal to the fibre, toward the tip (`sin(theta) = dot(tangent, direction)`);
`phi` is the angle between the two directions around the fibre. `HairBsdfEvaluate` returns the radiance toward the
viewer per unit of irradiance from the light on a plane facing the light: the sum of M A D over the lobes, which is
pbrt's `f` times `|cos(theta_i)|`. A caller multiplies by the light's incident radiance and no cosine. The model is
far-field (integrated across the fibre's width): one colour across a strand.

**Where each part comes from.** "Karis" is *Physically Based Hair Shading in Unreal* (SIGGRAPH 2016 course), "Frostbite"
*Strand-based Hair Rendering in Frostbite* (SIGGRAPH 2019), "pbrt" pbrt-v4's `HairBxDF` (Chiang et al. 2016's model);
slide numbers are the PDFs' pages. "Ours" is neither's.

| Part | Form | Source |
|---|---|---|
| M, every lobe | A Gaussian in `sin(theta_i) + sin(theta_o)` about the lobe's shift, normalized over `sin(theta_i)` | Karis, slide 18 |
| ... its width | Standard deviation `0.726 b + 0.812 b^2 + 3.7 b^20` for R (`b` = `beta_m`), half of it for TT, twice for TRT | pbrt (`v`, `v / 4`, `4 v` as variances); Karis slide 44 has the same ratios |
| ... its shift | `-sin(2 alpha)` for R, `sin(alpha)` for TT, `sin(4 alpha)` for TRT | pbrt's multiples of the tilt; Karis slide 44's multiples and signs (`-2 Shift`, `Shift`, `4 Shift`) |
| ... scaled for the view | Each lobe divided by the share of its Gaussian that lies on real directions (`sin(theta_i)` in [-1, 1]) for the view, at most ten times | Ours. Without it a view along the fibre loses the part of the lobe past grazing: R kept a third of pbrt's energy at 70 degrees |
| D, R | `cos(phi / 2) / 4` | Karis, slide 20 |
| A, R | Schlick's Fresnel at half the angle between the directions, `F0 = ((1 - eta) / (1 + eta))^2` | Karis, slide 20 |
| D, TT | A logistic about `phi = pi`, normalized over the circle (pbrt's `TrimmedLogistic`), of scale `sqrt(0.35^2 + s^2)` with `s` Chiang's `sqrt(pi / 8) (0.265 b + 1.194 b^2 + 5.372 b^22)` (`b` = `beta_n`) | Karis slide 29 for 0.35, the smooth fibre; pbrt for `s`; combining them is ours (Frostbite uses a fitted table that is not published) |
| A, TT | `(1 - f)^2 T`, with `T = exp(-sigma_a 2 / cos(theta_t))`, the path through the middle of the fibre, and `f` Schlick's Fresnel averaged over the fibre's width in closed form | Karis slide 23 for the form; Frostbite slide 17 for the path at `h = 0`; pbrt for `T`; the averaged Fresnel is ours (at `h = 0` the lobe returns up to 10% more than the fibre receives) |
| D, TRT | `0.75 exp(kappa (cos(phi) - 1))` normalized as a von Mises lobe, `kappa = 17 clamp(1.5 (1 - beta_n), 0.15, 1)` | Karis slide 32 (`exp(17 cos(phi) - 16.78)`, three quarters of a logistic of scale 0.15); Frostbite slide 21 for the widening with `beta_n`; keeping its integral at three quarters is ours (Frostbite's form goes to zero as `beta_n` nears 1) |
| A, TRT | `(1 - f)^2 f T^2` at `h = sqrt(3) / 2`, with `T = exp(-sigma_a 2 cos(gamma_t) / cos(theta_t))`, `sin(gamma_t) = h / eta'`, `eta' = sqrt(eta^2 - sin^2(theta_d)) / cos(theta_d)` | Karis slides 23, 26 and 32; Frostbite slide 21; pbrt for `T` |

`theta_d` is half the difference of the two inclinations and `theta_t` its refraction into the fibre. Karis's own
absorption terms (his base colour raised to a power, slides 28 and 32) and his fitted offset for TT (slide 25) are not
used: absorption is the physical one, so that the material is Chiang's.

**Checked against the sources.** The constants this step was handed as recalled, against Karis's slides read as
rendered pages:

| Constant | Found | Used |
|---|---|---|
| Roughness per lobe: `Roughness^2`, half, twice | Slide 44 | The ratios, on Chiang's mapping of `beta_m` |
| Shift per lobe: `-2 Shift`, `Shift`, `4 Shift` | Slide 44 | Yes, with `Shift = sin(alpha)` |
| `Shift` = 0.035 | Not on any slide. It is `sin(2 degrees)` (0.0349), and 2 degrees is the tilt pbrt's text calls typical | As the default tilt, 2 degrees |
| `F0` = 0.046 | The number is not on a slide; the formula is (slide 20) and gives 0.0465 at `eta` 1.55 | The formula |
| `eta' ~ 1.19 / cos(theta_d) + 0.36 cos(theta_d)` | Slide 26, for `eta` 1.55, "error < 0.68%". Checked: 0.675% at most up to 85 degrees | The exact form, since `eta` is a parameter |
| `h_TT ~ (1 + a (0.6 - 0.8 cos(phi))) cos(phi / 2)` | Slide 25 | No (Frostbite's `h = 0`) |
| `D_TT ~ exp(-3.65 cos(phi) - 3.98)` | Slide 29, a logistic of scale 0.35 about `pi`. Checked: within 0.017 of it (peak 0.71) | The logistic itself, widened by `beta_n` |
| `D_TRT ~ exp(17 cos(phi) - 16.78)` | Slide 32, three quarters of a logistic of scale 0.15. Checked: integral 0.763; ours is within 0.022 of it (peak 1.25) at `beta_n` 0 | Yes, as the concentration 17 and the integral 0.75 |
| `T_TRT = C^(0.8 / cos(theta_d))`, `T_TT = C^(sqrt(1 - h^2 a^2) / (2 cos(theta_d)))` | Slides 32 and 28 | No |
| Environment: the lobes with `R` scaled by `saturate(dot(light, view) + 1)`, TT removed | Slide 48 | Yes ([The environment](#the-environment)) |

Not read: Unreal's shader source (it is not public; its per-lobe scale factors are therefore not used), and Marschner
et al. 2003 (the conventions are taken from Karis's and pbrt's). pbrt's formulas are from its source
(`bxdfs.h`, `bxdfs.cpp`, `util/math.h`, `util/scattering.h`); of its book chapter the first half was read.

#### Material parameters

The parameters are Chiang's, one for one what pbrt's `HairBxDF` takes, so a path tracer can use the same material.
They are per instance (`HairInstanceGPU`), set by `hair.attach` and `hair.material`.

| Field | Default | `HairBxDF` | Meaning |
|---|---|---|---|
| `sigma_a` (`[r, g, b]`) | brown, `[0.838, 1.394, 2.74]` | `sigma_a` | Absorption coefficient of the fibre's interior per colour channel, per fibre diameter. |
| `beta_m` | 0.3 | `beta_m` | Longitudinal roughness, 0 to 1 (0.05 at least in the shader: hair is lit at strand vertices). |
| `beta_n` | 0.3 | `beta_n` | Azimuthal roughness, 0 to 1. |
| `alpha_degrees` | 2 | `alpha` | The cuticle scales' tilt. |
| `eta` | 1.55 | `eta` | Index of refraction. |

pbrt's sixth argument, `h`, the offset across the fibre, has no counterpart: this model is integrated over it. The
tangent here runs from root to tip and R's cone of directions is turned toward the root (on hair draped over a head the
white highlight then sits toward the tips from the coloured one, as in the captures); a path tracer must give its
`HairBxDF` the curve direction that turns R the same way.

`sigma_a` can be given directly, or as one of (the last one given wins):

- `preset`: `black`, `brown`, `blond`, `red`, `grey` or `white`, an amount of melanin and its redness: 8 and 0, 2 and 0,
  0.15 and 0, 0.6 and 0.9, 0.03 and 0, 0 and 0. Black and brown are the absorptions pbrt's text shows for black and brown
  hair (figure 9.47), blond its 0.143 rounded; red, grey and white were chosen by eye.
- `melanin` and `redness`: `sigma_a = eumelanin (0.419, 0.697, 1.37) + pheomelanin (0.187, 0.4, 1.05)`, Chiang's
  coefficients (pbrt's `SigmaAFromConcentration`), with `eumelanin = melanin (1 - redness)` and
  `pheomelanin = melanin redness`. The split by `redness` is ours; the two concentrations are pbrt's.
- `hair_color` (`[r, g, b]`): the colour a head of such hair shows after multiple scattering, by Chiang's fit (pbrt's
  `SigmaAFromReflectance`): `sigma_a = (ln(c) / (5.969 - 0.215 b + 2.532 b^2 - 10.73 b^3 + 5.574 b^4 + 0.245 b^5))^2`
  with `b` = `beta_n`. Without multiple scattering, which this step does not have, hair shaded with it is darker than
  the colour.

`melanin_variation` (0.15) and `roughness_variation` (0.1) make strands differ: a strand's absorption is `sigma_a`
times `1 + melanin_variation r` and its `beta_m` is `beta_m` times `1 + roughness_variation r'`, with `r` and `r'` in
[-1, 1) from a hash of the strand's cluster and its place in it, the same in every frame and at every level of detail.

#### Multiple scattering

Light reaches a fibre through other fibres, and leaves a groom after bouncing between them: that is what makes light
hair light. [`HairScatterShared.h`](../Plugins/Hair/include/Hair/HairScatterShared.h), one text for the shader and the
tests like the scattering function, is the dual scattering approximation of Zinke, Yuksel, Weber and Keyser (*Dual
Scattering Approximation for Fast Multiple Scattering in Hair*, SIGGRAPH 2008; "Zinke" below). `HairEvaluateBsdf`
calls it with the fibre count the vertex's march found.

- **Global.** Of a light's radiance, the share `exp(-n)` reaches the fibre having met none of the `n` fibres expected
  in the way (*direct*), and lights it through its own three lobes. The share `T_f - exp(-n)`, with
  `T_f = exp(-n (1 - a_f))`, arrives scattered forward by every fibre it met, coloured by them (`a_f` is the fibre's
  average forward attenuation, per colour channel), spread around the fibre over the half circle facing the light and
  along the fibre by a variance `sigma_f^2` that grows with the fibres met. It lights the fibre through the same lobes,
  each longitudinal function widened by that variance and each azimuthal distribution averaged over that half circle
  (Zinke's `N^G`), times the density factor `d_f`.
- **Local.** The fibres around the shading point scatter light back toward the viewer. That is one more lobe of the
  response (Zinke's `f_back`), of attenuation `A_b`, shift `Delta_b` and variance `sigma_b^2` computed from the fibre's
  average forward and backward attenuations, shifts and spreads (Zinke's equations 14, 16 and 17), broad around the
  fibre, times `d_b`. It is lit by the direct and the forward scattered light both.
- **The scatter table.** `a_f`, `a_b`, the shifts and the variances depend on the material and on the light's
  inclination to the fibre. `HairScatterMakeRow` computes them on the CPU by summing the scattering function itself
  over the outgoing directions of the half away from the light and of the half toward it (Zinke's equations 6 and 12);
  16 rows per instance (`Hair.ScatterTables`), for inclinations of 0 to 85 degrees, made when the instance's material
  changes (2,000 to 9,000 evaluations a row, tables cached by material) and interpolated by the shader. The table is of
  the instance's material: a strand's own offsets of absorption and roughness are not in it.

Where it departs from the paper:

| | The paper | Here | Why |
|---|---|---|---|
| Fibres in the way | An integer count; lit either directly (none) or by forward scattered light | The expected count `n`, a real number, with the fibres met Poisson distributed about it: direct share `exp(-n)`, `T_f = exp(-n (1 - a_f))`, and `n a_f / (1 - exp(-n a_f))` scattering events on average for the scattered share | The count comes from a density, and the paper's two cases are the limits |
| Units | Gaussians in the half angle, Marschner's normalization (`2 / pi`, `cos^2`) | Gaussians in `sin(theta_i) + sin(theta_o)`, every lobe keeping its integral: the backscattering lobe integrates to `d_b A_b`; shifts and variances measured in that variable from the lobes | The scattering function's own units ([Conventions](#the-hair-shading-model)) |
| The `pi` on `f_back` under scattered light (Figure 5) | There | Not carried over | It belongs to the normalization of the paper's `N^G` table |
| `N^G` | A precomputed table; equation 25 as printed does not say over which half circle | Each distribution averaged over the half circle centred on the light: closed forms for TT and TRT, four samples for R with its Fresnel term | No table to store or filter |
| Variance of a half | The average of the lobes' variances | The variance of the light the half receives, which also holds the spread of the lobes' means and what the end of the range cuts off | It is what the brute-force sum gives |
| Backscattering lobe around the fibre | Constant over the half circle facing the light | `(1 + cos(phi)) / (2 pi)` | The same without an edge |
| A wide backscattering lobe | | Scaled back to its integral where part of its Gaussian lies past the fibre's axis, as the fibre's own lobes are | Otherwise light hair loses a fifth of it |
| Density factors | `d_f` = `d_b` = 0.7, "between 0.6 and 0.8" | The same, `density_factor` | |
| Fibres along the path | The inclination at each fibre met | The light's inclination at the shading point for all of them | The volume keeps no directions |

Not from the paper at all: the scalp shell, the bias, and the isotropic sine of [the density volume](#the-density-volume).

#### The environment

`HairBsdfEnvironmentSample` gives four directions and a weight for each; the shade pass sums weight times the
environment's radiance along the direction. The radiance is the renderer's second-order spherical harmonics of the
environment, which are kept convolved with a cosine lobe: the irradiance of a surface facing the direction over pi, a
wide blur of the environment (the prefiltered environment map is 8 bits and clips the sky, and is not used).

Because that radiance varies slowly, the longitudinal functions integrate out, and each lobe is left on the cone of
directions the fibre mirrors the view into: R as `cos(phi / 2) / 4` times its Fresnel term around that cone, by one
direction for each quarter of the circle (at the quarter's centre of `cos(phi / 2)`, weighted by the quarter's
integral), and TRT's three quarters of its attenuation on the two directions nearer the viewer.

**With a density volume** each of the four directions is marched like a light (`environment_steps` samples) and takes
the weight `HairBsdfEnvironmentWeightDual` gives it for the fibres that way: the lobes' weights above, times the light
that arrives (direct and forward scattered, as for a light; nothing is widened, the environment being smooth already),
plus the backscattering lobe's share of that quarter of the circle. Karis's two stand-ins for occlusion (slide 48) are
gone there: R is whole, and TT is in, half of its attenuation on each of the two directions on the far side of the
fibre. What they stood for is now the fibre count that way and the [scalp shell](#the-density-volume): behind a fibre
on a head is the head.

**Without one** (`hair.volume {enabled: false}`, an instance over the budget) nothing occludes the environment, and
the two stand-ins are still what the weights are: TT is left out (it is the environment straight behind the fibre,
which on a head is the head), and R is scaled by `saturate(dot(light, view) + 1)`, which fades the light from behind
the fibre.

Four directions were kept: they are what the lobes' quadrature around the fibre needs (two for R and TRT on the
viewer's side, two for R and TT on the far side), and their marches are half the length of a light's.

#### What the tests establish

[`HairScatterTests.cpp`](../Plugins/Hair/tests/HairScatterTests.cpp), on the texts the shaders compile
(`HairScatterShared.h`, `HairVolumeShared.h`; the numbers are the tests' own output):

- **The scatter table against the integral.** The table's rows, read as the shader reads them at 0, 30 and 60 degrees,
  against the same sum done finely in double precision (1,200 by 96 directions), for white, blond, brown and black
  hair and for smoother and rougher blond and brown: attenuations within 0.74% (0.49% up to 30 degrees), spreads
  within 0.47%, shifts within 0.002. Across the fibre the green channel's `a_f` is 0.873 for white hair, 0.714 for
  blond, 0.084 for brown and 0.033 for black, and `a_b` 0.109, 0.089, 0.035 and 0.033; the backscattering lobe's `A_b`
  is 0.422, 0.096 and under 0.001 for brown and black. (A first table, each lobe integrated around the fibre on its own
  cone with the lobe's own shift and width, was within 1% at 0 degrees and 16% to 53% off at 60, where the lobes are
  cut off by the fibre's axis; the table is the brute-force sum for that reason.)
- **Dual scattering:** never negative or not finite over 60,000 random materials, directions, fibre counts and part
  masks; with no fibre in the way the direct part is the single fibre's response bit for bit; the parts add up; more
  fibres in the way never means more light; the backscattering lobe's energy is `d_b A_b` to 3%. Energy toward a view
  across the fibre, summed over light directions, green: white hair 0.98 from its own lobes plus 0.28 backscattered
  in the open, 0.91 in all behind one fibre, 0.51 behind four, 0.09 behind sixteen; blond 0.87, 0.55, 0.19, 0.005;
  brown 0.12, 0.05, 0.003; black 0.066, 0.025, 0.001. Behind four fibres black hair has 2% of its light in the open and
  blond 22%.
- **The density volume, a slab of known density** (4,000 parallel fibres of 80 micrometres in a 2 cm slab, 5 mm
  voxels), built and marched on the CPU with the shaders' arithmetic: the voxels hold 99.2% of length times diameter
  over a voxel's volume (the rest is rounding and the box's ends); a march across the slab counts 1.53, 1.60, 1.59 and
  1.58 fibres with 4, 8, 16 and 64 steps where the density says 1.571, at 24 degrees 1.74 where it says 1.72, and
  **1.57 for the same slab drawn with a fifth of the strands, each five times as wide**. Between rays and offsets the
  count varies by 0.57 (one standard deviation) at 8 steps and 0.30 at 16 for this slab, which the march meets far from
  its start, where its steps are as long as the slab is thick. From the slab's lit face toward the light, where there
  is no hair, a march reads 0.087 fibres without bias, 0.022 at half a voxel and 0.007 at one; from the middle of the
  slab, where 0.785 are in the way, 0.79, 0.60 and 0.39: the bias takes away what is within a voxel.

`HairBsdfTests.cpp`, on the shader's own text (the numbers are the tests' own output):

- **Never negative, never not finite:** 200,000 random materials, tangents and direction pairs, each lobe mask.
- **White furnace** (no absorption, integrated over every incident direction): R + TT + TRT is at most 1.002 and at
  least 0.757 over `beta_m` and `beta_n` in {0.1, 0.3, 0.6, 0.9} and view inclinations of 0 to 85 degrees; the test
  allows 1.01. At the defaults it is 0.982 looking across the fibre, 0.936 at 60 degrees and 0.757 at 85. The shortfall
  is the orders after TRT, which the model does not have (pbrt gives them 0.6% at 0 degrees and 5% at 70), and the
  lobes' approximations at grazing views.
- **Reciprocity.** Before the lobes are scaled for the view the response is exactly the same with the two directions
  swapped (so the BSDF itself, the response over `cos(theta_i)`, differs by the ratio of the two cosines, as pbrt's
  does). The scaling for the view breaks that: of random pairs of directions with a response above 0.001, 57% differ by
  more than 5% when swapped, and the worst by the scaling's limit of ten times. The pairs are uniform in
  `sin(theta)`, so most have one direction near the fibre's axis. At the default roughness the scale for a view
  across the fibre, and at 30, 45, 60 and 75 degrees from that, is 1.00, 1.08, 1.29, 1.70 and 2.22 for R, 1.00, 1.00,
  1.01, 1.14 and 1.47 for TT, and 1.10, 1.17, 1.30, 1.47 and 1.62 for TRT.
- **Absorption is monotonic in melanin:** in 20,000 random configurations no channel of the response rises with more
  melanin, and the albedo falls: at the defaults and 30 degrees, (0.983, 0.983, 0.983) with none, (0.865, 0.796, 0.653)
  for blond, (0.222, 0.120, 0.077) for brown, (0.075, 0.074, 0.074) for black, which is R alone.
- **The mappings** are pbrt's expressions, and the black and brown presets its figure's absorptions.
- **Against pbrt.** `HairBxDF::f` was transcribed into the test (with what it calls), in double precision, and averaged
  over the offset across the fibre. Each lobe's energy, ours / pbrt's, green channel, defaults:

  | Hair | View inclination | R | TT | TRT | pbrt's later orders |
  |---|---|---|---|---|---|
  | white | 0 | 0.066 / 0.069 | 0.864 / 0.866 | 0.052 / 0.056 | 0.006 |
  | white | 40 | 0.085 / 0.095 | 0.829 / 0.822 | 0.067 / 0.069 | 0.011 |
  | white | 70 | 0.172 / 0.271 | 0.619 / 0.546 | 0.090 / 0.131 | 0.053 |
  | blond | 0 | 0.066 / 0.069 | 0.701 / 0.713 | 0.037 / 0.039 | 0.004 |
  | blond | 40 | 0.085 / 0.095 | 0.659 / 0.661 | 0.045 / 0.045 | 0.006 |
  | blond | 70 | 0.172 / 0.271 | 0.479 / 0.421 | 0.057 / 0.078 | 0.022 |
  | brown | 0 | 0.066 / 0.069 | 0.053 / 0.066 | 0.000 / 0.000 | 0.000 |
  | brown | 40 | 0.085 / 0.095 | 0.039 / 0.045 | 0.000 / 0.000 | 0.000 |
  | brown | 70 | 0.172 / 0.271 | 0.020 / 0.017 | 0.000 / 0.000 | 0.000 |

  Within 5% of pbrt lobe by lobe up to 40 degrees but for brown's TT (20% under at 0: the path through the middle is
  the longest) and R at 40 (11% under). At 70 degrees R is 37% under (Schlick's Fresnel against the exact one, and
  what the Gaussian still loses), TT 13% over and TRT 30% under. Only energies are compared, not the lobes' shapes.
- **The environment's four directions** against the integral they stand for, for environments that vary as
  second-order harmonics do: 7.8% off on average over 24 random cases, 31% at worst, 22% at worst under a uniform sky
  (views near the fibre's axis, where the Fresnel term changes fastest around the cone).

### Probing the targets

`hair.probe` runs `Hair::Probe` for one frame, after the resolve. For one pixel in four each way that holds hair it
copies what the frame's targets hold there into `Hair.Probe`: the key's depth, the linear depth, the projected depth,
the surface device depth, the motion vector, the length of the stored normal and the roughness, behind a header with
the frame's view and unjittered projection matrices and the previous frame's. The runtime reads the buffer back after
the frame and writes it to the file as it is; `HairRuntimeGPUTypes.h` gives the layout (`HAIR_PROBE_*`), and
`hair.stats` reports `probe.state` and, as `probe.width`, the rows written. A script that knows how the scene moved can
then check the motion vectors and the depths ([Checking it](#checking-it)).

On the layered path the probe is `Hair::CompositeProbe`, after the composite, at the pixels that hold a nearest layer.
The same row then ends with the composited hair alpha and whether the pixel took hair's depth and motion vector (1 or
0), in place of the normal's length and the roughness; the linear depth and surface device depth it copies are those of
what is behind the hair.

### Dumping buffers

`hair.dump` writes one of the plugin's buffers as the next frame leaves it to a binary file: the frame number (8
bytes), then the buffer's rows, or its first `rows`. `pool_vertices` is the strand pool (`HairPoolVertex`),
`visible_clusters` the frame's selection (`HairVisibleCluster`) and `guide_points` the guide state (`HairGuidePoint`):
the whole buffer, in which an instance's rows start at the `guide_row_base` that `hair.stats` gives for it while it
holds any ([Guide rows](#guide-rows)).
Requests made before a frame are all read back after that same frame, with a `hair.probe` made then too, so a script
can compare a frame's motion vectors with the pool they were made from, and one frame's pool with the next
([Checking it](#checking-it)). `hair.stats` counts the files (`dumps`: `pending`, `written`, `failed`). Each dump copies
the whole buffer back.

### Debug view

`hair.debug` turns on `Hair::DebugOverlay`, a full-screen pass over the presentation colour that colours every pixel
holding a hair segment and leaves the rest of the frame as it is. It draws over the lit hair.

| `view` | Colour |
|---|---|
| `cluster` | One colour per cluster. |
| `detail` | The cluster's `l`: red at 0, green at 1. |
| `strand` | One colour per strand. |
| `rank` | The strand's rank in its groom: white is kept longest, blue is dropped first. |
| `vertex_level` | One colour per drawn vertex level, darker as the geomorph nears the level below; odd segments are dimmed. |
| `coverage` | The [coverage target](#the-coverage-target) over the whole frame: black where there is no hair, a grey level equal to the summed coverage up to 1, then from yellow (1) to red (64) by its doublings. The target is accumulated while this view is shown, whatever `coverage_accumulate` says. |
| `layers` | The layered path's, over the whole frame: the layers a pixel holds. Black none, blue one, green two, yellow three, red three with coverage beyond them (a remainder). Black on the surface path. |
| `alpha` | The layered path's, over the whole frame: the composited hair alpha as a grey level. Black on the surface path. |
| `reactive` | The layered path's, over the whole frame: the reactive mask the composite wrote for the upscaler, as a grey level. Black on the surface path. |
| `lobe_r`, `lobe_tt`, `lobe_trt` | The layered path's: hair lit by the lights through that lobe of its response alone, without the environment. These five are drawn by the shade pass, not over the frame: the hair in the frame is what changes. |
| `environment` | The layered path's: hair lit by the environment term alone. |
| `direct` | The layered path's: hair lit by the lights through all three lobes, without the environment. |
| `density` | Over the whole frame: the density volumes as a maximum projection, the densest voxel along each pixel's ray as the fibres a ray crosses in one voxel's length there (white at 2), over a dim grey where the ray is inside a volume's box; blue where the ray meets the scalp shell. |
| `transmittance` | The layered path's, drawn by the shade pass like the five above: what reaches each vertex of the first light through the hair (`T_f`, per colour channel; with `dual_scattering` off the direct share). The shadow map is not in it. |
| `fibre_count` | The fibres between each vertex and the first light: black none, through red (5) and yellow (11) to white at 16. |
| `scatter_global`, `scatter_local` | Hair lit by the lights through the forward scattered light alone, and through the backscattering lobe alone. |
| `vertex_density` | The volume's density in each vertex's own voxel (white at 2 fibres a voxel's length); blue in a scalp voxel. |
| `off` | The pass records nothing. |

`hair.capture` writes a presented frame (with the overlay, when one is on, and the menu) to a 32-bit `.bmp`. It reads the frame's
presentation colour texture back, so it needs no visible window and works with the display asleep or the session
locked, where a screen capture is black. The file is written a few frames later: hold the view still, and poll
`capture.state` in `hair.stats` (`pending`, `written` or `failed`).

### Shaders

The shaders are in [`Plugins/Hair/shaders/`](../Plugins/Hair/shaders). The build stages them into `HairShaders/` in
the renderer's runtime root (beside `shaders/`), with `HairGPUTypes.h`, `HairRuntimeGPUTypes.h`, `HairLodShared.h`,
`HairBsdfShared.h`, `HairScatterShared.h` and `HairVolumeShared.h` in `HairShaders/Hair/`; they are compiled at run time like the renderer's. `hairStructs.hlsli` includes the three headers,
and `hairCommon.hlsli` adds what the simulate, guide-level, select, evaluate, raster, resolve and debug shaders share (the
main camera, root following, point decoding, the visibility key, which guides are simulated and how they blend), and
`hairGuideSkin.hlsli` how a point follows a guide, for the evaluate and guide-level shaders. `hairHit.hlsli` finds a
pixel's point on its strand again from a key, for the resolve and the composite, and `hairShading.hlsli` holds the
marks and the packing of a vertex colour, for the rasterizer, the shade passes and the composite. `hairBsdf.hlsli` is
the fibre's response to light for the shade pass: it includes `Hair/HairBsdfShared.h`, the scattering function itself,
which `HairCoreTests` compiles as C++ ([The hair shading model](#the-hair-shading-model)), and
`Hair/HairScatterShared.h`, multiple scattering, shared the same way. `hairRasterCulled.hlsl` is the layered path's [culled rasterizer](#the-culled-rasterizer). `hairVolume.hlsl` clears and fills the density
volumes and `hairVolume.hlsli` reads and marches them, for the shade pass and the density view, both on
`Hair/HairVolumeShared.h`. `hairShadow.hlsl` is the shadow view's selection and its rasterizer into the virtual shadow
map's pages; it includes the renderer's `clodVirtualShadowClipmap.hlsli` and `virtualShadowCaster.hlsli`. `hairShade.hlsl` includes the renderer's `lighting.hlsli`.
A shader includes it by its path from the runtime root (`"HairShaders/hairStructs.hlsli"`): the main file is
compiled from memory, so it has no folder of its own to search. `scripts/deploy_demo.ps1` copies the folder.

`HairGPUTypes.h` compiles under DXC as it was written: the schema version is unchanged.

### Checking it

`hair.stats` reports:

| Field | Meaning |
|---|---|
| `assets`, `instances`, `attached_instances`, `active_instances`, `failed_instances` | Counts. An active instance was dispatched in the last frame. |
| `strands`, `points`, `rows` | Rows used and their capacities, of the asset buffers, the instance rows and the guide state lent out. `instance_strands` is the strands summed over instances. |
| `buffers_created`, `buffer_bytes` | The pooled buffers and their size. |
| `memory` | `buffers`: for every pooled buffer its `row_bytes`, and capacity, use now and high-water mark in rows and bytes. `capacity_bytes`, `used_bytes` and `high_water_bytes` are the sums (the last is the sum of each buffer's own high-water mark: an upper bound on the most in use at once). For the buffers a frame fills on the GPU (the visible-cluster list and the strand pool) use is that of the newest frame read back, and the high-water mark is over the frames read back: every frame with `continuous` on. `guide_state`: the lending of guide rows, below. |
| `asset_bytes_uploaded`, `instance_bytes_uploaded`, `bytes_uploaded`, `pending_upload_assets` | Upload totals. |
| `frames`, `evaluate_dispatches` | Frames the plugin was updated in, and the evaluate dispatches those frames asked for. |
| `gpu_counter` | The last readback of the GPU's own count of evaluate dispatches, with the CPU count and frame number of the frame it was read after. Each call asks for the next readback, so two calls a few frames apart see a fresh one. |
| `gpu_frame` | The same readback's per-frame counters, below. |
| `gpu_history` | With `continuous` on: the counters of the last 32 frames read back, oldest first. A frame's counters arrive a few frames after it, so a caller that polls once a frame sees every frame. |
| `volume` | What `hair.volume` returns: the density volume controls and the last frame's volumes ([The density volume](#the-density-volume)). |
| `shadows` | What `hair.shadows` returns: the shadow casting controls and the last frame's shadow casting on the CPU ([Shadow casting](#shadow-casting)). |
| `lod`, `render`, `debug_view`, `visibility_target`, `targets`, `capture`, `probe`, `dumps` | The LOD controls, the rendering controls (`hair.render`), the debug view, the target's size, every texture the plugin holds with its bytes, the last `hair.capture`, the last `hair.probe` and the `hair.dump` counts. |
| `raster_active`, `raster_width_pixels` | Which rasterizer drew the last frame's layered path (`direct` or `culled`), and the strand width on screen the `auto` rule saw: the widest strand of a groom in view, at the distance of the middle of its bounds. |
| `sim` | What `hair.sim` returns: the controls, the step counters and each instance's metrics ([Simulation](#simulation)). |
| `self_check` | The self-check's last result and its worst since it was enabled. |
| `instance_list` | Each instance's entity, bone, state, mode, materials (`color` and `roughness`; `material`, the layered path's) and error, its `strands`, the guide rows it needs to be simulated (`guide_rows`) and its first row while it holds any (`guide_row_base`, -1 otherwise). |

`memory.guide_state` ([Guide rows](#guide-rows)):

| Field | Meaning |
|---|---|
| `capacity_rows`, `budget_rows`, `row_bytes` | The buffer, the most that may be lent out (`guide_point_budget`, or the capacity), and 80. |
| `rows_in_use`, `rows_active`, `rows_kept` | Rows lent out now; those of instances simulated in the last frame; the rest, which are kept shapes. |
| `high_water_rows`, `largest_free_rows` | The most rows lent out at once since the buffers were made, and the largest range a request could take now. |
| `holders`, `active_holders`, `kept_holders` | Instances holding rows, simulated in the last frame, and keeping a shape. |
| `allocations`, `releases` | Ranges lent, and ranges given back with nothing lost. |
| `evictions`, `evictions_for_rows`, `evictions_by_age` | Ranges taken back with a kept shape or a running simulation lost: all of them, those taken for an instance in view or a lowered budget, and those taken by `guide_keep_seconds`. |
| `allocation_failures`, `starved_instances` | Times an instance in view could not get rows (counted once each time it starts to starve), and the instances starved now: in view, to be simulated, following rigidly. |
| `unsettled_instances` | Instances out of view that would settle unseen and found no free rows. |

`gpu_frame` and each row of `gpu_history`:

| Field | Meaning |
|---|---|
| `frame`, `camera_position`, `selected` | The frame, its main camera position, and whether it ran the select pass (false: frozen). |
| `stale_guides_drawn`, `unseen_strands_drawn` | Drawn strands of a simulated instance that read guide rows not written in that frame for that instance ([Guide rows](#guide-rows)), and drawn strands of instances that are not simulated because the CPU found them in no view. Both are 0 unless the CPU and the passes disagree. |
| `clusters_tested`, `clusters_visible` | Clusters the select pass looked at and kept. |
| `instances_visible` | Instances with at least one cluster inside the frustum. |
| `strands_selected`, `segments_selected` | Drawn strands and segments of the kept clusters. |
| `pool_vertices`, `visible_clusters` | Rows used and capacity of the strand pool and the visible-cluster list. |
| `overflow_clusters` | Visible clusters dropped because the pool or the list was full. |
| `pixels_covered` | Footprint pixels in front of the opaque surface, counted once for each segment over them (with coverage, by the end-to-end joint rule). With `coverage` `off` every one of them is written. |
| `coverage_sum` | The coverage of those pixels, summed: the hair's true projected area in pixels, overlaps counted. Each segment's share is rounded to 1/32 of a pixel, up or down at random in proportion. |
| `segments_drawn` | Segments that wrote at least one pixel. |
| `pixels_written` | Visibility writes. A pixel under several segments counts once for each. With coverage on these are the accepted writes, and their mean over frames is `coverage_sum`. |
| `pixels_occluded` | Covered pixels rejected by the opaque depth. |
| `segments_capped` | Segments cut short at the per-segment pixel limit. |
| `pixels_resolved` | Hair pixels the resolve wrote into the surface targets: at most `pixels_written`. On the layered path, the pixels the composite blended hair into. |
| `layer_rejected` | Layered path, raster: footprint pixels behind their pixel's third layer, which skipped the layer writes. `pixels_written` counts the others. |
| `coverage_skipped` | Layered path, raster: footprint pixels whose coverage add was skipped at `coverage_saturation`. |
| `layer_pixels` | Layered path, composite: pixels holding exactly one, two and three layers. |
| `remainder_pixels`, `remainder_alpha` | Layered path, composite: three-layer pixels with coverage beyond the third layer, and what the remainder term added to the hair alpha, summed over pixels. |
| `alpha_sum`, `depth_pixels` | Layered path, composite: the hair alpha summed over pixels (the hair's visible area in pixels), and the pixels that took hair's depth and motion vector. |
| `shade_list` | Layered path, shade list: pool vertices the rasterizer marked, gathered for the shade pass. |
| `shaded_vertices` | Layered path, shade: pool vertices lit: the list's rows. The two are equal unless a listed row is not the frame's. |
| `volume_segments`, `volume_deposits`, `volume_outside` | Layered path, voxelize: pool segments deposited into a density volume, the voxel adds they made, and the segments of an instance with a volume whose middle is outside its box. |
| `volume_vertices`, `volume_marches`, `volume_steps` | Layered path, shade: lit vertices whose instance has a volume, the marches made from them (one per light in range and not fully shadowed by the shadow map, and one per environment direction), and the samples of those marches. |
| `chunks`, `raster_items`, `chunks_late`, `chunks_late_drawn`, `raster_items_overflow` | Culled rasterizer: chunks of the frame's on-screen segments; those the first pass drew; those left for the late pass; those of them the late pass found not hidden, and drew; and chunks sent to the late pass because the item list was full (they are still drawn). |
| `pixels_tested`, `coverage_adds`, `hidden_writes`, `pixels_saturated`, `late_pixels_written` | Culled rasterizer: pixels the two passes looked at; coverage adds; hidden depths lowered by a strand that covers its pixel whole; pixels whose near coverage reached the saturation; and layer writes of the late pass, which is the hair the first pass's choice missed and the frame still drew. `pixels_occluded` then counts the footprint pixels behind the hidden depth, the opaque surface's or hair's. |
| `hair_tiles`, `clusters_unseen`, `clusters_reduced`, `wave_iterations` | Culled rasterizer: tiles a chunk touched; kept clusters that drew no pixel in the last frames; kept clusters drawn at a detail scale under 1; and, with `raster_profile`, the lane-iterations the passes' waves ran, idle lanes included (`pixels_tested` over it is the lane occupancy). |
| `shadow_lookups`, `shadow_coarser`, `shadow_missing`, `shadow_cleared` | Layered path, shade: the sun's shadow map lookups, and those that read a coarser clipmap than the vertex asks for, found no page (lit), or read a texel nothing was drawn into (lit). They are of the light the renderer's shadow map debug views show, the sun unless another is picked. |

| `shadow_clusters_tested`, `shadow_clusters` | Shadow select: clusters looked at, and kept for lying over a shadow page drawn this frame ([Shadow casting](#shadow-casting)). |
| `shadow_strands`, `shadow_segments`, `shadow_pool_vertices` | The kept clusters' drawn strands and segments, and the rows of the shadow pool they use. |
| `shadow_overflow_clusters` | Kept clusters dropped because the shadow pool or the shadow list was full. |
| `shadow_unseen_strands` | Shadow evaluate: strands of instances no view shows, drawn into the shadow map on their rigid pose. |
| `shadow_stale_guides_drawn` | As `stale_guides_drawn`, for the shadow view's strands: 0 likewise. |
| `shadow_segments_drawn`, `shadow_texels_covered`, `shadow_texels_written`, `shadow_segments_capped` | Shadow raster: (segment, clipmap) pairs that wrote a texel; strip texels on pages that take depth this frame, counted for each segment; those accepted and written; and segments cut short at 1,024 texels in a clipmap. |
| `shadow_clipmap_mask` | The clipmaps any kept cluster is drawn into, a bit each. |
| `shadow_pages_admitted`, `shadow_pages_invalidated` | The shadow map's own counts, copied while hair casts: cached pages it draws this frame, and page table entries invalidated by bounds (hair's and any other provider's). |

While the LOD is frozen the selection counters are those of the frozen selection. The counters are zeroed by an upload
at the start of the frame and read back after it.

The **self-check** (`hair.selfcheck`, off by default) checks [root frames](#root-frames) against the CPU. Every fourth
frame it lists a few roots per instance, spread across each groom and drawn or not (`Hair.RootCheckList`), and the
root-check pass writes their frames into `Hair.RootCheck` with the function every hair pass uses; the CPU reads that
back and compares it with the same arithmetic of its own. The CPU's inputs are the root bindings, the entity's row
of the per-object buffer version the frame bound, and the skin matrices of the entity's pose (inverse bind times
pose, this frame's and the previous frame's). It logs each comparison (`Hair self-check: frame ...`) and reports the
largest position and direction differences, how far the roots moved between the two frames, and how many instances
it could not compare (the frame's per-object rows were not on the CPU, or the list was full: it holds 4,096 roots).

[`Scripting/Scenarios/hair/hair_single.cs`](../Scripting/Scenarios/hair/hair_single.cs) is the end-to-end check of
attachment: a
drooping procedural groom on a moving sphere and a tuft on the swinging `tip` joint of `assets/skinned_bar.glb`, both
drawn lit. It passes when both grooms are uploaded, the GPU ran exactly one evaluate dispatch per frame over 65
frames, no instance holds guide rows (nothing is simulated), the
self-check agrees within 0.1 mm over at least ten checks, current and previous frames, with both instances moving, the
resolve wrote hair pixels (no more than the rasterizer's visibility writes), and a capture of the frame was written.
On the development machine the differences are about 2e-7. Captures go to `HAIR_CAPTURE_DIR`, or to `hair_captures` in
the system's temporary directory. The hair scenarios are in a folder of their own because the
scripting tests run every scenario beside the examples on a fake host, which has no hair commands.

[`Scripting/Scenarios/hair/hair_lod_sweep.cs`](../Scripting/Scenarios/hair/hair_lod_sweep.cs) checks selection,
evaluation, the rasterizer and the resolve. A 24,000-strand groom (512 clusters) sits on a sphere, drawn lit, and the
camera dollies straight back from 2 m to 80 m in 421 frames, the distance growing by the same ratio each frame. The scenario
reads every frame's counters and records each as a check named `sample ...` in the result document, which has no other
place for data. It passes when:

- every strand is drawn at the near end and under a quarter at the far end, the drawn strand count never rises along
  the way, and adjacent frames differ by at most 3 strands per cluster (compared between frames that saw the same
  number of clusters, since a cluster entering the frustum adds its strands);
- hair is rasterized and resolved in every dolly frame, no segment is capped, and the pool never overflows;
- a frozen LOD keeps the far selection at a near camera, and `force_detail` 0.25 draws a quarter of each cluster;
- on the layered path, with each of its two rasterizers forced in turn at one camera, the culled one is given the
  strands the direct one is and brings hair into the same pixels to 0.5% (two checks the surface path does not make);
- with the groom behind the camera no cluster is selected and nothing is written or resolved;
- with a cube between the camera and the groom the clusters are still selected, no pixel is written or resolved, and
  the covered pixels are counted as occluded;
- captures of the lit groom at 2 m, 12 m and 80 m and of the `detail` debug view were written.

On the development machine at 2560x1600: 24,000 strands and 384,000 segments at 2 m, 4,726 strands and 9,452 segments
at 80 m, at most 0.35 strands per cluster between adjacent frames, 408,000 pool vertices at most.

With `HAIR_SWEEP_SIMULATE=1` in the environment the same sweep runs with the groom simulated, at the simulation level
its coverage gives it along the dolly, and passes the same checks: the selection, the pool and the rasterizer do not
depend on where the strands are.

[`Scripting/Scenarios/hair/hair_sim.cs`](../Scripting/Scenarios/hair/hair_sim.cs) checks the simulation, collision and
interpolation. A drooping, wavy groom of 20,000 strands (4,096 guides, 135,168 guide points) sits on a sphere of radius
0.5 m with a sphere collider of the same size. The simulation level is forced to every guide (`lod_force_level` 8): the
scenario is about the solver. It reads the simulation's metrics of every frame and passes when:

- *At rest:* every guide starts on its rigid pose and is not reset again; the hair settles (the guides' mean speed stays
  under 3 mm/s for 30 frames) off its rigid pose; no guide segment is ever more than 2% off its rest length; no
  simulated guide point is ever inside the collider; evaluated roots are within 0.1 mm of their root frames before they
  are put on them; with a fixed 1/60 s frame the simulation takes one step a frame.
- *Swing:* the entity moves 0.35 m to each side twice (a sine of period 1.2 s) and stops where it began. The hair leaves
  its settled shape, is still moving after the entity has stopped, and settles into the same shape. No point enters the
  collider. While the entity moves no guide segment is more than 10% off its rest length, at most a hundredth of the
  guides have one over 2%, in at most a tenth of the frames, and the guides' mean stretch error stays under 1%; settled
  again, none is over 2%.
- *Reset and wind:* `reset` puts every guide back and the hair settles exactly as it did at first. A wind of
  (5, 0, 1.5) m/s moves it and gusts keep it moving; with the wind off it settles into the same shape. Stretch and
  collision as at rest.
- *Off again:* with the simulation off the rasterizer's counters are the rigid groom's from before it was turned on.
  The camera is jittered for the upscaler, so they are compared as the sorted counters of 32 consecutive frames. Strands
  and segments must be equal, and pixel counts equal to one part in 10,000: a rigid groom whose entity has been moved
  and put back draws a few pixels in five million differently from then on, with no simulation at all (the entity's row
  is not bit for bit what it was). That showed once the scenario ran at 60 frames a second and more.
- *A reference model,* when `HAIR_SIM_WSTRAIGHT` names Cem Yuksel's `wStraight.hair` (skipped otherwise): on a head
  sphere with a sphere and a neck capsule as colliders it settles, with stretch and collision as at rest.
- Captures of the rigid groom, the settled one, mid-swing, settled again, under wind, rigid again and the reference
  model were written.

[`Scripting/Scenarios/hair/hair_sim_lod.cs`](../Scripting/Scenarios/hair/hair_sim_lod.cs) checks simulation LOD, on
the same groom, with the head swaying 5 cm to each side every two seconds so that the hair is never still. It needs
the fixed 1/60 s frame. It reads the metrics of every frame and passes when:

- *Forced level, slowly:* from every guide (level 4) down to 0 in 600 frames and back. Every guide is simulated at the
  top; between 1 and 0 only the 64 of level 0; at 0 none, and the instance is not simulated. The simulated count never
  moves against the sweep and changes by at most 48 guides a frame, never a level at once. Then across a twentieth of
  level 3 in 300 frames: guides are demoted at most three in a frame, and one at a time in at least 40 frames.
- *Automatic level:* the camera dollies from 2 m, where the groom's coverage is 1.5 and every guide is simulated, to
  120 m, where it is 0.025 and nothing is, and back. The level never rises on the way out and never falls on the way
  back, does not move while the camera stands still half way, and every guide is simulated again at the end.
- *Forced level, fast:* a triangle between 0.5 and 4 every 24 frames, so that blends turn round before they end;
  every guide and level 0 alone by turns, six frames each; through 0 and back in 40 frames; off and on, half a second
  each. Afterwards every guide is simulated, none is in a blend and none is stretched.
- *Budget:* with `lod_point_budget` 40,000 at the highest level, between 36,000 and 40,000 guide points are simulated.
- *Jumps.* In every phase: the largest `transition_motion` of any frame is at most 1.5 times the largest
  `steady_motion` of the phase plus 0.5 mm (2.5 times in the fast sweeps); no simulated segment is more than 10% off its rest length; no simulated
  point is inside the collider; every guide is either simulated or evaluated.
- *The same guide in neighbouring frames.* `hair.dump` writes the guide state of 36 consecutive frames of a slow sweep
  and 30 of the fast one. For every guide and frame the scenario takes how much further the guide moved (relative to
  its root) than in the frame before and the frame after, whichever is more. The largest such excess among guides in
  the frame they become or stop being simulated, and among guides in a blend, is at most 1.5 times the largest among
  all other guides plus 0.5 mm (2.5 times in the fast sweep).
- *Fidelity.* Two such grooms on two heads that swing together (0.25 m to each side, twice), the first with every guide
  simulated and the second forced to level 4, 3, 2 and 1 in turn (`hair.simlevel`), each from a reset. Their guide
  state is dumped at ten frames, and each guide point's offset from its rigid pose compared between the two. At the
  same level they are under 0.05 mm apart at the median and 1 mm at the 95th percentile; at every reduced level the difference is smaller than the hair's
  own displacement from its settled shape, and it grows as the level falls.

[`Scripting/Scenarios/hair/hair_crowd.cs`](../Scripting/Scenarios/hair/hair_crowd.cs) is the many-groom scenario: an
N x N grid of 11 cm heads 2.5 m apart (`HAIR_CROWD_N`, default 10, and `HAIR_CROWD_SPACING`), each with one of four
procedural grooms of 8,000 to 12,000 strands and 1,024 guides (a straight, two wavy and a curly one; 1,004,000 strands
over a hundred heads), simulated at the level their size on screen gives them unless `HAIR_CROWD_SIMULATE=0`. The camera
stands 2 m in front of the first row looking down the grid, flies down the aisle between the two middle columns to
behind the last row, looks away from the grid, and then looks back at the whole grid from 30 m in front of it. Every
frame's counters are recorded as checks named `sample ...`: instances and clusters in view, drawn strands and segments,
strand pool use, pixels written and resolved, and the simulation's instances, guides, guide points and the spread of
`sFrac` over the instances. It passes when every head takes a groom and all are dispatched, grooms are drawn from the
front with fewer strands than the scene has, nothing is selected or drawn facing away, and facing away no groom is
simulated and no guide rows are in active use, every groom is in view from the
far camera with under half the scene's strands drawn, none simulated and none holding guide rows, the strand pool and
the visible-cluster list never overflow, every groom a view showed got the guide rows it asked for, and in no frame is
a strand drawn from guide rows that were not written in that frame for its groom, or from a groom left unsimulated for
being out of view. It writes three captures (near, middle of the fly-through, far).

```powershell
BasicRenderer.exe --script scenarios\hair\hair_single.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_lod_sweep.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_sim.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_sim_lod.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_crowd.cs --script-exit --script-fixed-dt 0.0166667
```

`hair_sim_lod.cs` writes about 60 captures of 16 MB and, for a while, up to 400 MB of dumps.

The simulation follows the view, and `hair_sim.cs` and `hair_sim_lod.cs` are about the solver and the levels: both
check that their grooms were in view in every frame whose metrics they read.

Every hair scenario passes `HAIR_PATH`, when it is set in the environment, to `hair.render` as `path`:
`HAIR_PATH=layered` runs it on the layered path. No check differs between the paths but the two `hair_lod_sweep.cs`
makes of the layered path's rasterizers: `pixels_resolved` is the pixels
hair was brought into the frame in on either, and where a scenario compares pixel counters with a tolerance, the
tolerance is the one `HAIR_COVERAGE` picks.

Every hair scenario passes `HAIR_COVERAGE`, when it is set in the environment, to `hair.lod` as `coverage`:
`HAIR_COVERAGE=off` runs it with the opaque rasterizer. With coverage on, `hair_sim.cs` compares the rigid groom's pixel
counters before and after the simulation to 2% and not to one part in 10,000: the written pixels are then random draws.

**Measured for simulation LOD** (development machine, 2560x1600, fixed 1/60 s frames, default controls, the run at
about 7 frames a second with the display asleep; the scripts that probed the motion vectors and compared the blends and
the two rotations are not in the tree):

- *Guides against level, forced.* Level 4: 4,096 simulated (64, 192, 768, 3,072 by level). 3.68: 3,451. 3.35: 2,471.
  3.01: 1,428. 2.68: 864. 2.34: 612. 2.01: 338. 1.67: 225. 1.34: 162. 1.01: 85. 1 and below: 64. 0: none. (Falling
  at 1/150 of a level a frame; the counts include the guides still in their demotion, a quarter of a second's worth.) At 1/150 of a level a frame the count changes by 9.9 guides a frame on average
  and 35 at most; across a twentieth of level 3 in 300 frames, 134 guides are demoted, one in 86 frames, two in 21 and
  three in 2.
- *Guides against distance, automatic.* Coverage 1.50 at 2 m: level 4, every guide. 0.78: level 3.64, 3,595 guides.
  0.55: 3.15, 2,130. 0.39: 2.66, 915. 0.28: 2.16, 542. 0.20: 1.67, 236. 0.14: 1.18, 149. 0.10: 0.85, 64. 0.036: 0.13,
  64. 0.026: off. Coming back, simulation starts again at coverage 0.043 (level 0.25).
- *Jumps, from the metrics.* The largest motion in a frame of a guide that changes state or is in a blend, against
  the largest of any other guide in the same phase: slow sweep down 12.8 mm against 13.6 mm; slow sweep up 27.8 mm
  against 40.2 mm; very slow sweep 10.1 mm against 11.5 mm; dolly away 12.6 mm against 14.2 mm; dolly back 22.1 mm
  against 33.6 mm. Frame by frame the first is 0.75 to 0.97 of the second at the median. With every guide simulated
  and none changing, the largest is 11 mm: the sway, and the entity's jumps ([Simulation](#simulation)).
- *Jumps under the fast sweeps.* 35.1 mm against 18.7 mm, 1.9 times (1.7 to 2.1 times over three runs): every guide is in a blend at once, and a blend
  carries a guide across the gap between its simulated and evaluated shapes in a quarter of a second. The scenario
  allows 2.5 times there and 1.5 elsewhere. Without blends a slow sweep already gives 2.1 times (table above).
- *The same guide in neighbouring frames.* How much further a guide moved in a frame than in the frame before and the
  frame after, at most: in a slow sweep, 3.5 mm for guides in the frame they become or stop being simulated (107
  guide-frames), 5.3 mm for guides in a blend (1,599), 6.8 mm for all others (133,462). In the fast sweep, 4.8 mm
  (8,163), 6.5 mm (40,112) and 6.5 mm (62,317). The means are 0.4 to 0.6 mm in every group.
- *Stretch.* No simulated segment is more than 1.2% off its rest length in the slow sweeps and the dolly, 1.6% in the
  fast sweeps.
- *Budget.* 40,000 points at level 4: the level goes to 3.05 and 39,996 guide points (1,212 guides) are simulated.
- *Fidelity,* over nine frames of the swing, in which the fully simulated groom's guide points are 24 to 28 mm from
  their settled place on average. Level 3 (1,024 guides simulated): the reduced groom's guide points are 3.6 mm from
  the full one's on average, 2.5 mm at the median, 11.3 mm at the 95th percentile. Level 2 (256): 5.0, 3.8 and
  12.8 mm. Level 1 (64): 6.6, 4.8 and 18.0 mm. Settled, before the swing: 1.8, 2.6 and 3.5 mm on average. Two grooms
  both at level 4: 0.004 mm at the median and 0.24 mm at the 95th percentile, but 0.10 mm on average and 109 mm at
  most: a few guides in contact with the collider part ways between two grooms 2.6 m apart. The largest differences
  at the reduced levels (190 to 250 mm) are of that kind too.
- *Motion vectors,* as for the simulation above (strand pools of two consecutive frames and a probe of the second,
  LOD frozen, the head swinging 0.35 m), with the level dropping from 4 to 2 and coming back (3,840 guides demoted and
  promoted together), at eight moments: the first frames of the demotion, the middle of its blend, its last frame, the
  frame after, a few frames later, and the first, middle and last frames of the promotion. In all eight the second
  frame's previous positions are the first frame's positions to 1.3e-7 m on average and 9.3e-6 m at most, and the
  stored motion vectors are within 0.08 pixels of the pool's on average (0.33 at the 99th percentile, 1.5 at most) for
  motions of up to 17 pixels on average. The same eight pairs with the level left at 4: 0.10, 0.37 and 2.8.
- *Cost,* from the pass timing panel, as an indication only (the run was at 7 frames a second and readings move by a
  tenth of a millisecond or two between sessions; the passes were then dispatched per instance, and
  [POC measurements](#poc-measurements) has profiled times). `Hair::Simulate` and `Hair::GuideLevels`: every guide simulated,
  1.19 ms and under 0.1 ms; level 2.5 (650 guides simulated), 0.68 and 0.23 ms; level 1 (64 guides), 0.62 and
  0.34 ms; off by distance, both under 0.05 ms. A groom of 64 strands, all of them level 0 guides with nothing to
  skip: 0.70 ms. One of 1,024 strands, all guides: 1.15 ms. So the simulate pass does not get cheaper in proportion to
  the guides it skips: its time is about that of one thread group's serial loop (see the limits of
  [Simulation LOD](#simulation-lod)). Two grooms, one at level 4 and one at level 1: 2.02 and 0.35 ms.
- *Captures.* Twelve captures four frames apart in the slow sweep, between levels 3.04 and 2.75, and twelve in the fast
  sweep: the fringe of the hair sways with the head from one to the next, and no strand or clump stands apart from its
  neighbours in any of them; the mean difference between consecutive captures over the hair is 2.9 to 5.6 of 255
  levels in both sets, with no capture standing out. Captures four frames apart cannot show a one-frame pop; the
  metrics above are the evidence for those.

**Measured for the simulation** (development machine, 2560x1600, fixed 1/60 s frames unless said, default controls;
the scripts that dumped and probed the pool and captured the swing are not in the tree):

- *`hair_sim.cs`, the procedural groom.* At rest it settles in 1.75 simulated seconds, 16.3 mm off its rigid pose on
  average. Settled, the largest stretch error of any guide segment is 1.6e-5 of its rest length and the mean 2.2e-6;
  while settling the largest is 0.5%. The nearest any simulated point comes to the collider is 1.999 mm: the 2 mm
  margin. Evaluated roots are at most 9.3e-6 m (guides) and 9.1e-6 m (interpolated strands) from their root frames
  before they are put on them. After the swing it settles in about 2.9 s, and after the wind in 2.8 s, both to within
  0.01 mm of the first settled shape; under the wind the largest stretch error is 0.3%.
- *The same run, twice.* Everything but the swing is the same number for number: the settle times, the stretch errors,
  the deviations, the collider distance, the rigid counters, the reference model. The swing is not, because the entity
  itself does not move the same way twice: in about 115 to 125 of the swing's 350 frames the frame did not follow the
  one before (as if a scene version were drawn twice, or one or two skipped, see [Simulation](#simulation)), so the root stood
  still and then covered up to 60 to 90 mm in one frame, at different frames in each run. The hair is off its settled
  shape by 65 to 87 mm at most, on average over the guides, depending on the run.
- *Stretch while the entity swings.* Over three runs, 1 to 7 of those 350 frames have a guide with a segment more than
  2% off its rest length: 1 to 6 of the 4,096 guides, the worst segment 2.1% to 3.9% off for that frame, all of them
  within three frames of a jump of the entity. The mean stretch error over the guides stays under 1e-5. That is with
  the extra substeps a jump gets; with the 4 a frame asks for, the same jumps left 35 to 150 guides stretched by 13% to
  27%, because a collider that covers two or three frames' travel in a few substeps pushes the points it reaches out
  after the last stretch solve of each, and that push is the error (a substep ends with a collision so that no point is
  left inside).
- *In real time,* on a machine whose frames alternated between about 15 ms and 250 ms: of 51 s of frame time 13.5 s was
  simulated and 37.5 s dropped, in 195 frames that hit the step cap; frames shorter than a step took none. The swing
  then moves the entity by wall time, up to 675 mm between two frames. The scenario's checks all pass: the largest
  stretch error during the swing is 6.6% (2 guides, in 5 of 132 frames), no point is inside the collider at the end of
  any frame, and the hair settles to the same shape after the swing, the reset and the wind.
- *`wStraight.hair`* (50,000 strands) settles in 1.1 simulated seconds, 3.6 mm off its rigid pose, with a largest
  stretch error of 2.6e-5 and no point inside its two colliders (0.3 mm at the nearest while settling, 2.06 mm settled).
- *Previous positions.* The strand pool of two consecutive frames mid-swing, with the LOD frozen (`hair.dump`): the
  second frame's previous positions are the first frame's positions to 1.1e-7 m on average and 9.3e-6 m at most (the
  roots, which are put on their root frames after evaluation), while the vertices moved 32 mm on average between the
  frames and up to 73 mm.
- *Motion vectors.* `hair.probe` in the second of those frames, against the motion of each probed pixel's point on its
  segment computed from that frame's pool: 22,368 pixels, motion 13.4 pixels on average and up to 27, error 0.018
  pixels on average, 0.16 at the 99th percentile and 1.1 at most. The check places the pixel on its segment with the
  unjittered projection, the resolve with the jittered one, and segments here differ in motion from end to end by
  several pixels; the target's 16-bit floats keep about 0.01 pixels at these magnitudes.
- *Under the upscaler* (DLSS at native resolution). Captures every tenth frame through a swing, and 2 and 25 frames
  after the entity stops, show no trail and no second copy of the hair. Strands inside the hair are softer in some of
  the moving frames: the contrast of fine detail there (the standard deviation of a Laplacian over a 300 pixel square)
  is 16.1 at rest, between 11.7 and 23.1 in the seven swing captures, 14.2 two frames after the stop and 15.6 after 25.
- *Cost,* from the demo's pass timing panel, as an indication only: `Hair::Simulate` 1.5 ms for the procedural groom
  at rest (4,096 guides of 33 points, one step of 4 substeps a frame) and 2.2 ms through the swing, where guides take
  extra substeps; 0.44 ms for `wStraight`. `Hair::Evaluate` 0.35 ms with the simulation on against 0.22 ms off (20,000
  strands), 0.24 ms for `wStraight`.

**Measured for the resolve** (development machine, DLSS with render and output resolution both 2560x1600; the scripts
that did it are not in the tree):

- *Motion vectors.* `hair.probe` in a fixed-step run, compared with the motion the scene's known movement gives each
  probed pixel at its depth. A groom moving (0.01, 0.0025, 0.005) m a frame under a still camera: 30,228 pixels,
  expected motion 4.0 to 5.6 pixels, error 0.0015 pixels on average and 0.005 at most. A still groom under a camera
  sliding 0.01 m a frame and turning: 56,283 pixels, expected motion 3.2 to 6.9 pixels, error 0.0025 on average and
  0.0054 at most. The target is `R16G16_Float`, whose 11 bits put a 5 pixel motion within about 0.005. In both, the
  linear depth target equalled the key's depth at every probed pixel, and the projected and device depths matched
  `-M22 + M32 / depth` to 6e-9.
- *After camera motion.* A capture two frames after that camera stops against one 120 frames later, at the same pose:
  in the band of thin strand tips the mean difference is 4.7 of 255 levels, against 0.4 between two converged
  captures; on a cube and the floor beside it, 0.4 against 0.3. In the images the strands are a little softer two
  frames after the stop and sharpen; there is no displaced copy or trail.
- *Continuity along the dolly.* `hair_lod_sweep.cs`'s path, a capture at every second step (210 pairs), with the LOD
  live (24,000 strands falling to 4,726) and frozen at the near end (24,000 throughout). The mean difference between
  consecutive captures falls from 3.1 levels at 2 m to 0.01 at 80 m in both runs. No pair stands out from the six
  around it by more than 1.49 times with the LOD live, 1.27 times frozen; 8 pairs exceed 1.25 times live (between 20
  and 44 m, and at 80 m, where the groom covers a few thousand pixels or fewer), 1 frozen.

With the D3D12 debug layer on (`BASICRENDERER_VULKAN_VALIDATION=1` in a RelWithDebInfo build) a scenario cannot run
to its end in this tree: the layer reports error 1047 for the renderer's packed small buffers within the first
frames, hair or not, and any validation error stops a scenario. Drive the commands over the control pipe to check
hair under the layer.

## POC measurements

Measured on 2026-11-27 for the proof of concept's gate: does the cost of hair follow the hair on screen, and are the
LOD transitions continuous. The scripts that drove the runs are not in the tree.

**Conditions.** The development machine (GeForce RTX 5070 Laptop GPU, 12 GB), `BasicRendererDemo` in raster mode with
its default features, DLSS with render and output resolution both 2560x1600, the display awake at 60 Hz and the session
unlocked. The demo presents with vsync by default; for timing it ran uncapped (`allowTearing`), at 12 to 25 ms a
frame: 11.8 ms with no hair, 13.9 ms with the crowd rigid and 18.1 ms with it simulated.

**Method.** Times are from the statistical profiler ([TESTING.md](../TESTING.md)): `profile.run` over the render graph's
GPU timestamp of each hair pass, one sample a frame, 52 to 60 samples per experiment, stopped at a 95% confidence
half-width of 3% of the mean. Each experiment holds the camera still. Counters are `hair.stats` readbacks of a frame of
the same view.

- The profiler runs its frames with a time step of 0, in which the simulation would take no step, so `hair.sim`'s
  `frame_seconds` was 1/60: one step of four substeps a frame. Heads sway 2 cm to each side every two seconds while
  simulated.
- The profiler waits for frames with no scene task in flight before it samples. In a scripted run nearly every frame
  has one, so its `stable_frames` was 1.
- `DeferredShadingPass`, profiled beside the hair passes, read 5.74 to 5.84 ms in every experiment from the crowd's near
  camera, whatever the hair cost: the GPU was not throttled within the session.
- **A small pass's time depends on what runs beside it.** `Hair::RootBinding` does the same work for one groom at any
  distance and read from 0.03 to 0.13 ms; `Hair::Select` read 0.010 or 0.022 ms for the same clusters. Read times under
  about 0.1 ms as "small", not as that figure.

### Many grooms and the per-instance dispatches

[`hair_crowd.cs`](../Scripting/Scenarios/hair/hair_crowd.cs)'s crowd: 100 grooms of four assets, 1,004,000 strands.
As first built, the simulate pass was dispatched once per simulated instance and the guide-level pass once per
simulated instance and guide level, each behind a barrier. With 48 to 60 instances simulated that was 12 ms of hair, 8
to 10 ms of it in the guide-level pass's 144 to 180 small dispatches. Both passes are now one dispatch (one per level
for the guide levels) over every instance ([Simulation](#simulation)). Times in ms:

| Camera | Simulation | Dispatches | In view | Strands drawn | Hair pixels | Simulated | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| near: 2 m in front, looking down the grid | off | either | 70 | 293,838 | 259,244 | 0 | 0.67 | 0.00 | 0.00 | 0.03 | 0.52 | 0.56 | 0.13 | **1.90** |
| near: 2 m in front, looking down the grid | on | per instance | 70 | 293,838 | 251,585 | 48 | 0.67 | 1.21 | 8.38 | 0.04 | 1.24 | 0.58 | 0.13 | **12.24** |
| near: 2 m in front, looking down the grid | on | merged | 70 | 293,838 | 251,623 | 48 | 0.67 | 0.94 | 1.77 | 0.04 | 1.22 | 0.56 | 0.13 | **5.33** |
| behind the last row, facing away | off | either | 0 | 0 | 0 | 0 | 0.68 | 0.00 | 0.00 | 0.03 | 0.02 | 0.09 | 0.05 | **0.86** |
| behind the last row, facing away | on | per instance | 0 | 0 | 0 | 60 | 0.67 | 1.28 | 10.47 | 0.03 | 0.02 | 0.09 | 0.05 | **12.61** |
| behind the last row, facing away | on | merged | 0 | 0 | 0 | 60 | 0.67 | 0.94 | 2.21 | 0.03 | 0.02 | 0.09 | 0.05 | **4.01** |
| 52 m in front, the whole grid small | off | either | 100 | 99,004 | 7,149 | 0 | 0.67 | 0.00 | 0.00 | 0.03 | 0.17 | 0.12 | 0.05 | **1.04** |
| 52 m in front, the whole grid small | on | per instance | 100 | 99,004 | 7,174 | 0 | 0.67 | 0.00 | 0.00 | 0.03 | 0.17 | 0.12 | 0.05 | **1.06** |
| 52 m in front, the whole grid small | on | merged | 100 | 99,004 | 7,149 | 0 | 0.67 | 0.00 | 0.00 | 0.04 | 0.18 | 0.12 | 0.05 | **1.06** |

The same crowd with nothing on screen cost 12.6 ms and costs 4.0 ms; from the far camera the simulation is off by
distance, and the two forms are the same. The root-binding and select passes are still dispatched per instance:
`Hair::Select` is 0.03 ms for 100 dispatches, and `Hair::RootBinding`'s 0.67 ms is its work, a million roots.

### Cost against distance, one groom

One groom, alone in view, the camera looking at it from each distance. A procedural groom of 30,000 wavy strands of 33
points with 4,096 guides on an 11 cm head (about 20 cm of hair), and `wStraight` (50,000 strands) on its 8 cm head.
"Pixels written" counts every visibility write, so a pixel under 40 strands counts 40 times; "hair pixels" are the pixels
the resolve wrote. Times in ms.

Procedural groom, rigid:

| Distance | Strands | Segments | Pixels written | Hair pixels | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.4 m | 30,000 | 960,000 | 15,768,925 | 1,875,181 | 0.06 | 0.00 | 0.00 | 0.01 | 0.40 | 1.25 | 0.67 | **2.38** |
| 0.8 m | 30,000 | 960,000 | 9,054,521 | 498,823 | 0.06 | 0.00 | 0.00 | 0.01 | 0.40 | 0.61 | 0.20 | **1.28** |
| 1.6 m | 30,000 | 960,000 | 4,802,250 | 122,347 | 0.08 | 0.00 | 0.00 | 0.01 | 0.40 | 0.29 | 0.07 | **0.85** |
| 3.2 m | 29,550 | 945,600 | 2,654,618 | 30,922 | 0.10 | 0.00 | 0.00 | 0.01 | 0.38 | 0.22 | 0.05 | **0.75** |
| 6.4 m | 18,763 | 300,208 | 844,957 | 7,807 | 0.10 | 0.00 | 0.00 | 0.01 | 0.10 | 0.11 | 0.05 | **0.37** |
| 12.8 m | 9,712 | 77,696 | 214,811 | 1,977 | 0.11 | 0.00 | 0.00 | 0.01 | 0.03 | 0.07 | 0.05 | **0.27** |
| 25.6 m | 4,979 | 19,916 | 51,803 | 495 | 0.10 | 0.00 | 0.00 | 0.01 | 0.02 | 0.05 | 0.05 | **0.23** |
| 51.2 m | 2,616 | 5,232 | 11,249 | 126 | 0.11 | 0.00 | 0.00 | 0.01 | 0.01 | 0.05 | 0.05 | **0.23** |
| 100 m | 1,442 | 1,442 | 2,811 | 35 | 0.12 | 0.00 | 0.00 | 0.01 | 0.01 | 0.05 | 0.05 | **0.23** |

Procedural groom, simulated at the level its coverage gives it:

| Distance | Strands | Segments | Pixels written | Hair pixels | Guides simulated | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.4 m | 30,000 | 960,000 | 16,446,513 | 1,843,036 | 4,096 | 0.03 | 1.15 | 0.01 | 0.02 | 0.53 | 1.32 | 0.66 | **3.72** |
| 0.8 m | 30,000 | 960,000 | 9,046,457 | 463,277 | 3,335 | 0.03 | 0.92 | 0.08 | 0.02 | 0.52 | 0.62 | 0.19 | **2.38** |
| 1.6 m | 30,000 | 960,000 | 4,796,493 | 115,317 | 833 | 0.03 | 0.65 | 0.24 | 0.02 | 0.52 | 0.30 | 0.07 | **1.83** |
| 3.2 m | 29,550 | 945,600 | 2,648,083 | 29,645 | 218 | 0.03 | 0.61 | 0.33 | 0.02 | 0.72 | 0.22 | 0.05 | **1.98** |
| 6.4 m | 18,763 | 300,208 | 843,179 | 7,667 | 64 | 0.03 | 0.60 | 0.36 | 0.02 | 0.28 | 0.12 | 0.05 | **1.45** |
| 12.8 m | 9,712 | 77,696 | 213,598 | 1,944 | 64 | 0.03 | 0.60 | 0.36 | 0.02 | 0.11 | 0.07 | 0.05 | **1.25** |
| 25.6 m | 4,979 | 19,916 | 51,302 | 499 | 0 | 0.03 | 0.00 | 0.00 | 0.02 | 0.02 | 0.05 | 0.05 | **0.17** |
| 51.2 m | 2,616 | 5,232 | 11,488 | 124 | 0 | 0.03 | 0.00 | 0.00 | 0.02 | 0.01 | 0.05 | 0.05 | **0.16** |
| 100 m | 1,442 | 1,442 | 2,659 | 35 | 0 | 0.03 | 0.00 | 0.00 | 0.02 | 0.01 | 0.05 | 0.05 | **0.16** |

`wStraight`, rigid:

| Distance | Strands | Segments | Pixels written | Hair pixels | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.4 m | 50,000 | 558,992 | 65,757,845 | 1,861,212 | 0.08 | 0.00 | 0.00 | 0.01 | 0.16 | 4.75 | 0.53 | **5.52** |
| 0.8 m | 50,000 | 558,992 | 23,091,030 | 403,120 | 0.08 | 0.00 | 0.00 | 0.01 | 0.16 | 1.43 | 0.14 | **1.83** |
| 1.6 m | 50,000 | 558,992 | 11,706,595 | 95,851 | 0.12 | 0.00 | 0.00 | 0.01 | 0.16 | 0.63 | 0.06 | **0.98** |
| 3.2 m | 47,878 | 536,504 | 5,813,442 | 23,771 | 0.13 | 0.00 | 0.00 | 0.01 | 0.16 | 0.31 | 0.05 | **0.66** |
| 6.4 m | 26,663 | 301,784 | 1,743,699 | 5,950 | 0.12 | 0.00 | 0.00 | 0.01 | 0.09 | 0.15 | 0.05 | **0.41** |
| 12.8 m | 13,672 | 130,880 | 480,574 | 1,518 | 0.13 | 0.00 | 0.00 | 0.01 | 0.03 | 0.09 | 0.05 | **0.30** |
| 25.6 m | 7,160 | 45,548 | 132,326 | 386 | 0.13 | 0.00 | 0.00 | 0.01 | 0.02 | 0.06 | 0.05 | **0.27** |
| 51.2 m | 3,844 | 12,200 | 34,534 | 100 | 0.13 | 0.00 | 0.00 | 0.01 | 0.02 | 0.05 | 0.05 | **0.25** |
| 100 m | 2,183 | 3,556 | 9,860 | 27 | 0.12 | 0.00 | 0.00 | 0.01 | 0.01 | 0.04 | 0.05 | **0.24** |

`wStraight`, simulated:

| Distance | Strands | Segments | Pixels written | Hair pixels | Guides simulated | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.4 m | 50,000 | 558,992 | 65,697,337 | 1,811,352 | 4,096 | 0.04 | 0.39 | 0.01 | 0.02 | 0.26 | 4.76 | 0.53 | **6.01** |
| 0.8 m | 50,000 | 558,992 | 23,220,306 | 401,521 | 2,405 | 0.04 | 0.31 | 0.06 | 0.02 | 0.26 | 1.46 | 0.14 | **2.29** |
| 1.6 m | 50,000 | 558,992 | 11,770,578 | 95,836 | 601 | 0.04 | 0.23 | 0.11 | 0.02 | 0.26 | 0.63 | 0.06 | **1.36** |
| 3.2 m | 47,878 | 536,504 | 5,839,215 | 23,570 | 161 | 0.04 | 0.21 | 0.15 | 0.02 | 0.26 | 0.31 | 0.05 | **1.05** |
| 6.4 m | 26,663 | 301,784 | 1,751,298 | 5,905 | 64 | 0.04 | 0.22 | 0.16 | 0.02 | 0.24 | 0.15 | 0.05 | **0.88** |
| 12.8 m | 13,672 | 130,880 | 482,669 | 1,503 | 64 | 0.04 | 0.22 | 0.16 | 0.02 | 0.20 | 0.08 | 0.05 | **0.77** |
| 25.6 m | 7,160 | 45,548 | 131,752 | 382 | 0 | 0.05 | 0.00 | 0.00 | 0.02 | 0.02 | 0.06 | 0.05 | **0.21** |
| 51.2 m | 3,844 | 12,200 | 34,198 | 99 | 0 | 0.04 | 0.00 | 0.00 | 0.02 | 0.02 | 0.05 | 0.05 | **0.18** |
| 100 m | 2,183 | 3,556 | 9,550 | 28 | 0 | 0.04 | 0.00 | 0.00 | 0.02 | 0.01 | 0.04 | 0.05 | **0.17** |

- **Strands fall as one over the distance; pixels fall as its square.** From 6.4 m to 100 m the procedural groom's
  hair pixels fall 220 times (7,807 to 35) and its strands 13 times (18,763 to 1,442). The detail level is the strand
  spacing in pixels, a length, and the area the groom covers shrinks with its square. So a far groom is drawn with 40
  strands for each of its pixels, and `wStraight` at 100 m with 81.
- **The rasterizer's cost is the pixels written, not the pixels that show.** Each strand is at least a pixel wide and
  every strand of the groom's depth is written: 8 writes per hair pixel for the procedural groom filling the view, 86 at
  3.2 m, and 35 and 245 for `wStraight`. Filling the view, `wStraight` writes 65.7 million pixels for 1.9 million that
  show, in 4.7 ms. There is no occlusion among strands and no early out.
- **At a distance the cost is the passes' floors.** Below about 500 hair pixels the total stops at 0.16 to 0.27 ms:
  the resolve visits every pixel of the target (0.05 ms), the rasterizer clears the target and dispatches over every
  point of the groom (0.05 ms), and root binding writes every root (0.03 to 0.13 ms as read).
- **Simulation.** Every guide simulated costs `Hair::Simulate` 1.15 ms for the procedural groom (4,096 guides of 33
  points) and 0.39 ms for `wStraight` (4,096 guides of 9 to 17 points). At 64 guides it is still 0.60 and 0.22 ms: the
  pass's floor, a thread's serial loop over a strand. The guide-level pass rises as the solver's share falls, to 0.36
  and 0.16 ms for 4,032 evaluated guides. Between 12.8 and 25.6 m the groom's coverage passes 0.03, it stops being simulated, and
  both are nothing.

**What a rule that followed area would draw.** The same procedural groom with the detail level forced to the square of
the level the rule gave (`force_detail`; the floor of 0.02 holds it at 1,024 strands from 25.6 m). The difference is the
mean absolute difference of 8-bit luma between captures of the two, per hair pixel; "same image twice" is two captures of
the rule as built.

| Distance | Rule | Strands | Segments | Pixels written | Hair pixels | Evaluate + Raster | Difference from the rule as built | Same image twice |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 6.4 m | as built | 18,763 | 300,208 | 845,057 | 7,793 | 0.211 |  |  |
| 6.4 m | squared | 12,115 | 193,840 | 540,261 | 7,588 | 0.152 | 4.1 | 0.2 |
| 12.8 m | as built | 9,712 | 77,696 | 215,070 | 1,975 | 0.094 |  |  |
| 12.8 m | squared | 3,584 | 28,672 | 78,552 | 1,860 | 0.072 | 6.6 | 0.5 |
| 25.6 m | as built | 4,979 | 19,916 | 51,742 | 496 | 0.067 |  |  |
| 25.6 m | squared | 1,024 | 4,096 | 10,607 | 457 | 0.058 | 11.6 | 1.2 |
| 51.2 m | as built | 2,616 | 5,232 | 11,560 | 126 | 0.057 |  |  |
| 51.2 m | squared | 1,024 | 2,048 | 4,618 | 115 | 0.057 | 9.8 | 3.1 |
| 100 m | as built | 1,442 | 1,442 | 2,663 | 35 | 0.055 |  |  |
| 100 m | squared | 1,024 | 1,024 | 1,960 | 31 | 0.055 | 33.7 | 13.5 |

Squared, the groom draws a third to a fifth of the strands between 12.8 and 25.6 m, loses 3% to 11% of its hair pixels,
and a hair pixel changes by 4 to 12 levels of 255 out to 51 m (at 100 m the groom is 35 pixels and two captures of the
same thing differ by 13). That is not nothing, and nothing makes up for the lost coverage (see
[Continuity](#continuity)). The times barely move because one far groom is already at the floors. What it would buy is in the strand pool
and in every later pass that walks the drawn strands. The same lever on the crowd, from its near camera, through
`error_pixels` (the spacing at which strands start to be dropped):

| `error_pixels` | Strands drawn | Pool vertices | Pixels written | Hair pixels | Evaluate | Raster |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 293,838 | 2,382,030 | 7,525,697 | 259,128 | 0.54 | 0.56 |
| 2 | 165,512 | 1,477,736 | 5,349,263 | 257,268 | 0.37 | 0.40 |
| 4 | 91,326 | 872,042 | 3,334,025 | 249,883 | 0.27 | 0.28 |

### Cost against groom count

Grids of 1, 4, 16, 36, 64 and 100 grooms: the rows of the crowd nearest the camera, from the near camera (2 m in front
of the first row, looking down the grid; some of the nearest row is outside the view). Times in ms. Rigid:

| Grooms | In view | Strands drawn | Pool vertices | Pixels written | Hair pixels | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 12,000 | 114,392 | 1,430,638 | 84,154 | 0.02 | 0.00 | 0.00 | 0.02 | 0.04 | 0.12 | 0.06 | **0.26** |
| 4 | 4 | 43,390 | 643,294 | 3,680,605 | 195,213 | 0.04 | 0.00 | 0.00 | 0.02 | 0.18 | 0.25 | 0.10 | **0.59** |
| 16 | 12 | 97,416 | 1,189,148 | 5,184,918 | 227,944 | 0.12 | 0.00 | 0.00 | 0.02 | 0.30 | 0.38 | 0.11 | **0.93** |
| 36 | 26 | 160,775 | 1,653,053 | 6,197,691 | 241,443 | 0.24 | 0.00 | 0.00 | 0.03 | 0.37 | 0.44 | 0.12 | **1.20** |
| 64 | 46 | 230,589 | 2,097,397 | 7,105,843 | 255,071 | 0.43 | 0.00 | 0.00 | 0.03 | 0.45 | 0.52 | 0.12 | **1.54** |
| 100 | 70 | 293,838 | 2,382,030 | 7,526,542 | 259,058 | 0.67 | 0.00 | 0.00 | 0.03 | 0.52 | 0.55 | 0.13 | **1.91** |

Simulated at the level each groom's coverage gives it ("total, per-instance dispatches" is the same grid as first
built):

| Grooms | In view | Simulated | Guides simulated | Guides evaluated | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total | Total, per-instance dispatches |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 1 | 223 | 801 | 0.02 | 0.18 | 0.08 | 0.02 | 0.07 | 0.12 | 0.06 | **0.54** | 0.56 |
| 4 | 4 | 4 | 597 | 3,499 | 0.04 | 0.59 | 0.21 | 0.02 | 0.30 | 0.26 | 0.10 | **1.52** | 1.90 |
| 16 | 12 | 16 | 1,395 | 14,989 | 0.12 | 0.62 | 0.65 | 0.03 | 0.66 | 0.38 | 0.11 | **2.57** | 5.05 |
| 36 | 26 | 32 | 2,420 | 30,348 | 0.25 | 0.92 | 1.21 | 0.03 | 1.03 | 0.46 | 0.12 | **4.01** | 8.66 |
| 64 | 46 | 42 | 3,059 | 39,949 | 0.43 | 0.88 | 1.61 | 0.03 | 1.15 | 0.52 | 0.12 | **4.75** | 10.84 |
| 100 | 70 | 48 | 3,443 | 45,709 | 0.67 | 0.94 | 1.77 | 0.04 | 1.24 | 0.55 | 0.13 | **5.34** | 12.26 |

The fly-through of `hair_crowd.cs` as nine cameras along its path, each held still and profiled (the grid's rows are at
z = 0 to -22.5; the camera looks along -z). One camera's profile with the simulation on did not complete.

| Camera z | In view | Strands drawn | Pool vertices | Hair pixels | Simulated grooms | Guides simulated | Guides evaluated | Hair total, simulation off | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2.0 | 70 | 293,850 | 2,382,242 | 245,981 | 63 | 4,406 | 60,106 | 1.91 | 0.67 | 1.49 | 2.72 | 0.04 | 1.64 | 0.55 | 0.12 | **7.24** |
| -1.4 | 56 | 249,521 | 2,157,003 | 116,890 | 71 | 5,653 | 67,051 | 1.71 | 0.67 | 1.56 | 3.02 | 0.04 | 1.52 | 0.44 | 0.09 | **7.33** |
| -4.9 | 44 | 225,710 | 2,447,266 | 163,255 | 74 | 6,275 | 69,501 | 1.88 | 0.67 | 1.70 | 3.13 | 0.04 | 1.46 | 0.48 | 0.10 | **7.59** |
| -8.3 | 30 | 180,632 | 1,859,328 | 198,621 | 88 | 6,964 | 83,148 | 1.68 | 0.67 | 2.04 | 3.75 | 0.04 | 1.19 | 0.44 | 0.10 | **8.24** |
| -11.8 | 18 | 117,208 | 1,406,416 | 100,262 | 97 | 7,535 | 91,793 | 1.48 | 0.67 | 1.90 | 4.16 | 0.04 | 1.00 | 0.34 | 0.08 | **8.19** |
| -15.2 | 10 | 74,485 | 1,324,849 | 139,496 | 99 | 7,864 | 93,512 | 1.50 | 0.67 | 2.10 | 4.27 | 0.04 | 0.83 | 0.33 | 0.08 | **8.32** |
| -18.6 | 4 | 29,400 | 440,600 | 31,684 | 97 | 7,405 | 91,923 | 1.04 | not measured | not measured | not measured | not measured | not measured | not measured | not measured | not measured |
| -22.1 | 0 | 0 | 0 | 0 | 84 | 6,645 | 79,371 | 0.85 | 0.67 | 1.66 | 3.64 | 0.04 | 0.02 | 0.09 | 0.05 | **6.16** |
| -25.5 | 0 | 0 | 0 | 0 | 72 | 4,800 | 68,928 | 0.85 | 0.67 | 1.26 | 3.15 | 0.04 | 0.02 | 0.09 | 0.05 | **5.27** |

**Does each pass's cost follow the hair on screen?**

| Pass | Follows on-screen hair? | What its cost follows, as measured |
|---|---|---|
| `Hair::RootBinding` | No | The roots of every attached groom, in view or not: 0.67 ms for 1,004,000 roots, the same facing away from all of them. |
| `Hair::Simulate` | No | The grooms near the camera in any direction (the simulation level is from distance alone) and, weakly, their simulated guide points. A floor of about 0.6 ms as soon as one groom is simulated, then 0.94 ms for 3,446 guides (90,000 points) over 48 grooms and 2.10 ms for 7,864 guides (215,000 points) over 99. |
| `Hair::GuideLevels` | No | Every guide of every simulated groom, simulated or not, drawn or not: about 0.04 ms per thousand guides evaluated (1.77 ms for 45,700, 4.27 ms for 93,500). It is the largest hair pass in the crowd. |
| `Hair::Select` | No, and it does not matter | Every cluster of every attached groom: 0.03 ms for 22,528. |
| `Hair::Evaluate` | Yes, through the LOD | The drawn vertices (the strand pool): 0.52 ms for 2.38 million rigid vertices, 1.2 to 1.6 ms for the same when interpolated from guides. Nothing when nothing is in view (0.02 ms). The drawn vertices follow the view, but fall with distance more slowly than the pixels do. |
| `Hair::Raster` | Partly | The pixels written, about 0.06 to 0.07 ms per million, over a floor of 0.09 ms in the crowd (the clear, and a dispatch over every point of every groom up to the pool's size). Pixels written are 29 times the hair pixels from the crowd's near camera and 36 times from the far one. |
| `Hair::Resolve` | Yes | Hair pixels, about 0.3 ms per million, over a floor of 0.05 ms for visiting every pixel. |

So: with the crowd rigid, hair costs 0.85 ms with nothing in view and 1.9 ms with 70 grooms and 259,000 hair pixels in
view; nearly half of the 1.9 ms is root binding and floors that do not depend on the view. With the crowd simulated the
cost does not follow the screen at all: flying into the grid it rises from 7.2 to 8.3 ms while the grooms in view fall
from 70 to 10, because more grooms are within the simulation's distance, and it is 5.3 to 6.2 ms with the whole grid
behind the camera. Only the far camera, where every groom is beyond the simulation's distance, is cheap: 1.06 ms for
100 grooms in 7,100 pixels, of which 0.67 ms is root binding.

### Memory

The pooled buffers are 673 MB at `HairCapacities`' defaults, which were raised for this crowd: root frames from
262,144 to 1,048,576 rows, guide points from 1,048,576 to 4,194,304 and the strand pool from 2,097,152 to 4,194,304
(at the old size the pool overflowed from the crowd's near camera). Use, for the crowd:

| | Rows used | Bytes |
|---|---:|---:|
| Assets, shared: four grooms (clusters, aggregates, strands, root bindings, guides, points) | 40,000 strands, 1,038,392 points | 13.2 MB |
| ... a straight groom of 12,000 strands (9 or 17 points a strand) | 114,392 points | 2.5 MB |
| ... a wavy groom of 10,000 strands (33 points) | 330,000 points | 3.9 MB |
| ... the curly groom of 8,000 strands (33 points) | 264,000 points | 3.1 MB |
| Root frames, per instance: 96 B a strand | 1,004,000 of 1,048,576 | 91.9 MB |
| Guide state, per instance: 80 B a guide point | 2,754,576 of 4,194,304 | 210.2 MB |
| ... an instance of a 33-point groom with 1,024 guides | 33,792 | 2.6 MB |
| ... an instance of the straight groom | 8,807 | 0.7 MB |
| Strand pool, per frame: 36 B a drawn vertex | up to 2,678,493 of 4,194,304 along the fly-through | 92 MB used of 144 MB |
| Visible clusters, per frame | up to 15,872 of 65,535 | 0.5 MB of 2 MB |
| Hair visibility target (not in the 673 MB) | 2560x1600 | 31 MB |

- **An instance's own state is 23 times its share of the asset data.** 302 MB of root frames and guide state for 100
  instances, against 13 MB for the four grooms they share. Both are held for every instance whether it is in view,
  simulated or neither.
- **The crowd's grooms have 1,024 guides because of this.** With the builder's default of 4,096 guides a 33-point groom
  needs 10.3 MB of guide state per instance, 1.03 GB for a hundred.
- **The visible-cluster index is 16 bits.** The crowd's grooms have 128 or 256 clusters; a 50,000-strand groom has
  1,024, so the list holds 63 such grooms in view.

### Continuity

The dolly of the resolve's measurements, again: `hair_lod_sweep.cs`'s groom and path, a capture at every second step,
four frames after the camera moves, and the mean absolute difference of 8-bit luma between consecutive captures over
the frame without the menus. "Frozen" keeps the selection of the near end. With the simulation on the groom is simulated
at the level its coverage gives it; frozen, that level is held too. A pair is "over 1.25x" when its difference is more
than 1.25 times the median of the six pairs around it. The last column is the difference between two captures from the
same camera, four frames apart, at six distances from 20 m on.

| LOD | Simulation | Strands, 2 m to 80 m | Mean difference 2-8 m | 8-20 m | 20-44 m | 44-80 m | Pairs over 1.25x | Largest ratio | Same camera twice, 20-80 m |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| live | off | 24,000 to 4,726 | 1.3373 | 0.0821 | 0.0149 | 0.0081 | 6 | 1.48 | 0.0036 to 0.0077 |
| live, again | off | 24,000 to 4,726 | 1.3364 | 0.0820 | 0.0149 | 0.0081 | 3 | 1.38 | 0.0051 to 0.0076 |
| frozen | off | 24,000 to 24,000 | 1.3374 | 0.0888 | 0.0163 | 0.0083 | 10 | 1.46 | 0.0039 to 0.0065 |
| live, vertex LOD off | off | 24,000 to 4,726 | 1.3370 | 0.0890 | 0.0162 | 0.0083 | 4 | 1.31 | 0.0045 to 0.0087 |
| live, every strand drawn | off | 24,000 to 24,000 | 1.3361 | 0.0821 | 0.0149 | 0.0079 | 8 | 1.64 | 0.0033 to 0.0067 |
| live | on | 24,000 to 4,726 | 1.4080 | 0.0853 | 0.0150 | 0.0081 | 8 | 1.40 | 0.0031 to 0.0054 |
| frozen, simulation level held | on | 24,000 to 24,000 | 1.4173 | 0.0923 | 0.0162 | 0.0084 | 3 | 1.41 | 0.0023 to 0.0092 |

- **The earlier finding was noise.** It was 8 pairs over 1.25x with the LOD live against 1 frozen, between 20 and 44 m.
  Here the frozen run has more such pairs than either live run, two runs of the same thing have 6 and 3, and only 2 of
  those are the same pairs. From 20 m on a pair's difference is 0.008 to 0.016 levels and two captures of a still view
  differ by 0.003 to 0.009, so a ratio of 1.25 is inside the noise. Per pair the selection drops 80 to 320 strands
  there and moves a dozen clusters a vertex level; the ratio has no correlation with either (between -0.05 and 0.10 in
  every run).
- **Neither LOD changes the differences.** Live and frozen agree to 9% in every band. With the vertex LOD off the 8 to
  20 m band reads as the frozen run does (0.089 against 0.082), and with every strand drawn as the live one: what
  difference there is comes from the vertex LOD, and it lowers it.
- **With the simulation on** the picture is the same from 8 m. Nearer, both runs are 5% higher than without it, and
  two captures of a still view differ by 1.6 levels at 2 m against 0.13: the hair was still settling when the dolly
  began.
- **What this metric cannot see.** At 80 m the groom is 364 pixels of four million, so a strand that pops changes the
  mean by less than the noise. The numbers above bound flicker of the whole image, not of a strand.

**Width does nothing at a distance.** The count is made continuous by fading the boundary strand's width, and dropped
strands are made up for by widening the kept ones. Both act on a strand's width, and the rasterizer draws every strand
at least a pixel wide. Hair pixels of the dolly's groom (the mean of 32 frames) with `max_width_scale` 8 and 1, with
every strand drawn, and the same for a groom of strands four times as wide:

| Distance | Strands drawn | Hair pixels, `max_width_scale` 8 | `max_width_scale` 1 | Every strand drawn | 4x wider strands: scale 8 | scale 1 | every strand |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 m | 24,000 | 593,412 | 593,412 | 593,412 | 602,869 | 602,869 | 602,869 |
| 4 m | 24,000 | 139,111 | 139,111 | 139,111 | 139,111 | 139,111 | 139,111 |
| 8 m | 24,000 | 34,083 | 34,083 | 34,083 | 34,083 | 34,083 | 34,083 |
| 12 m | 24,000 | 15,127 | 15,127 | 15,127 | 15,127 | 15,127 | 15,127 |
| 16 m | 22,437 | 8,519 | 8,519 | 8,522 | 8,519 | 8,519 | 8,522 |
| 24 m | 15,231 | 3,796 | 3,796 | 3,818 | 3,796 | 3,796 | 3,818 |
| 32 m | 11,472 | 2,151 | 2,151 | 2,169 | 2,151 | 2,151 | 2,169 |
| 48 m | 7,722 | 972 | 972 | 984 | 972 | 972 | 984 |
| 80 m | 4,726 | 364 | 364 | 369 | 364 | 364 | 369 |

The compensation changes no pixel at any distance, and a strand four times as wide changes none from 4 m. The rule
starts dropping strands when they are closer together than a pixel, and a strand is thinner than the gap to its
neighbour, so by then every strand is already under the one-pixel floor: with this rasterizer the width compensation
and the boundary strand's fade are inert by construction, at every distance, for the default `error_pixels` and for
any value below the ratio of a groom's strand spacing to its strand width (about ten here). A strand leaves
at its full one pixel of width in one frame. It does not show in the numbers because its pixels are still covered by its
neighbours (the LOD's hair pixels are within 1.4% of the full groom's at every distance), and because the upscaler
averages what is left. This is a property of the scheme without coverage, not of its parameters: fading needs a
partial-coverage visibility sample.

### Curls under reduction

`wCurly` (50,000 strands of 57 to 73 points, built at 33) on its head. Differences are mean absolute differences of
8-bit luma over the square around the groom, against the full groom (every strand, no vertex LOD) from the same camera.

Forced detail levels at 0.8 m, where the groom is 355,000 pixels:

| Forced `l` | Strands | Segments | Pixels written | Hair pixels | Difference from the full groom | Same image twice | Mean brightness |
|---:|---:|---:|---:|---:|---:|---:|---:|
| full (no vertex LOD) | 50,000 | 1,600,000 | 29,187,031 | 355,543 | | 0.81 | 164.2 |
| 1 | 50,000 | 1,600,000 | 29,187,474 | 355,615 | 0.86 | 0.39 | 164.1 |
| 0.5 | 25,600 | 819,200 | 20,843,619 | 352,690 | 5.74 | 0.75 | 163.4 |
| 0.25 | 13,312 | 425,984 | 21,969,066 | 354,466 | 11.56 | 0.61 | 159.1 |
| 0.125 | 7,168 | 229,376 | 24,228,233 | 358,116 | 13.44 | 0.93 | 158.8 |
| 0.06 | 3,072 | 98,304 | 11,594,605 | 347,750 | 14.63 | 0.63 | 159.7 |
| 0.03 | 2,048 | 65,536 | 5,621,526 | 328,190 | 15.84 | 0.71 | 161.9 |

And at 3.2 m (21,000 pixels):

| Forced `l` | Strands | Segments | Pixels written | Hair pixels | Difference from the full groom | Same image twice | Mean brightness |
|---:|---:|---:|---:|---:|---:|---:|---:|
| full (no vertex LOD) | 50,000 | 1,600,000 | 7,919,368 | 21,365 | | 0.25 | 204.1 |
| 1 | 50,000 | 1,595,296 | 7,886,031 | 21,333 | 0.57 | 0.27 | 204.1 |
| 0.5 | 25,600 | 816,800 | 4,035,884 | 20,988 | 2.84 | 0.29 | 204.9 |
| 0.25 | 13,312 | 424,736 | 2,100,054 | 20,626 | 4.90 | 0.29 | 205.9 |
| 0.125 | 7,168 | 228,704 | 1,489,170 | 20,418 | 6.55 | 0.34 | 206.4 |
| 0.06 | 3,072 | 98,016 | 652,929 | 19,755 | 8.73 | 0.34 | 207.8 |
| 0.03 | 2,048 | 65,344 | 382,295 | 18,924 | 10.68 | 0.33 | 209.4 |

The automatic rule against the full groom. "Strand LOD alone" and "vertex LOD alone" turn the other one off.

| Distance | Share of strands drawn | Segments, full | Segments, automatic | Hair pixels, full | Hair pixels, automatic | Difference | Strand LOD alone | Vertex LOD alone | Same image twice | Brightness, full | Brightness, automatic |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.8 m | 1.00 | 1,600,000 | 1,600,000 | 355,575 | 355,662 | 0.79 | 0.86 | 0.98 | 0.88 | 164.2 | 164.2 |
| 1.6 m | 1.00 | 1,600,000 | 1,600,000 | 85,429 | 85,465 | 0.49 | 0.52 | 0.49 | 0.32 | 195.0 | 195.0 |
| 3.2 m | 0.99 | 1,600,000 | 1,581,824 | 21,331 | 21,332 | 0.74 | 0.58 | 0.74 | 0.33 | 204.0 | 204.1 |
| 6.4 m | 0.71 | 1,600,000 | 821,936 | 5,423 | 5,363 | 2.82 | 1.73 | 2.35 | 0.38 | 205.1 | 205.9 |
| 12.8 m | 0.36 | 1,600,000 | 204,836 | 1,400 | 1,328 | 5.43 | 3.30 | 4.40 | 0.43 | 206.8 | 210.7 |
| 25.6 m | 0.18 | 1,600,000 | 52,202 | 369 | 337 | 7.17 | 4.02 | 5.83 | 0.59 | 207.4 | 213.7 |
| 51.2 m | 0.10 | 1,591,376 | 13,730 | 103 | 90 | 3.03 | 1.51 | 2.60 | 0.23 | 238.4 | 241.2 |

What the captures show:

- *Forced, close.* At `l` 0.5 the groom still reads as fine hair. At 0.25 the strands are four times as wide and it
  reads as straw: the fine fibre texture is gone, the bends of the 33-point strands show as corners, and fly-away
  strands become wires. At 0.06 and 0.03 it is a head of thick noodles with visibly polygonal bends and gaps opening in
  the fringe. The outline of the mass and its volume hold throughout (hair pixels fall 2% at 0.06 and 8% at 0.03) and
  the mean brightness moves by 3%. So what goes first, close, is the strand's own look (fineness, then the smoothness
  of its curve), not the shape of the hairstyle. The automatic rule never picks these levels this close.
- *Forced, 3.2 m.* Down to 0.25 the groom is hard to tell from the full one. At 0.06 and 0.03 it turns stringy:
  separate thick strands with light between them, the dark shading inside the mass washes out, and the lower edge
  recedes (hair pixels down 8% and 11%).
- *Automatic.* Side by side the automatic and full grooms are hard to tell apart at 6.4 m and 12.8 m. The automatic
  one is a little lighter and flatter (brightness 205.9 against 205.1 at 6.4 m, 210.7 against 206.8 at 12.8 m, 213.7
  against 207.4 at 25.6 m) and its fringe a little thinner (hair pixels down 1%, 5% and 9%). Curl shape is not what
  degrades: where the rule reduces the groom, curls are smaller than a pixel. Brightness and contrast go first, then the
  silhouette.
- **The vertex LOD changes the image more than the strand LOD** at every distance where either does anything (4.4
  against 3.3 at 12.8 m, 5.8 against 4.0 at 25.6 m), for a similar saving in segments. It takes its level from the
  cluster's centreline, which is shorter than a curly strand, so curls lose vertices early and are drawn along their
  chords.

**The 33-point cap.** `wCurly` built with `max_vertex_level` 5 (the default), 6 and 7, viewed from 0.22 m (the hair
fills the view) and from 0.8 m. The builder gives a cluster no more segments than its source strands have, so level 7
adds little to level 6. Times in ms.

| `max_vertex_level` | Points a strand | Asset points | Asset size | Guide state per instance | Evaluate, close | Raster, close | Hair total, close | Hair total, 0.8 m |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 5 | 33 | 1,650,000 | 19.1 MB | 10.3 MB | 0.60 | 8.16 | 9.43 | 2.35 |
| 6 | up to 65 | 3,250,000 | 31.4 MB | 20.3 MB | 1.38 | 8.28 | 10.35 | 3.23 |
| 7 | up to 129 | 3,824,976 | 35.7 MB | 24.0 MB | 1.64 | 8.39 | 10.72 | 3.51 |

From 0.22 m the 33-point strands show corners at their tighter bends, on a minority of strands; at 65 points they are
smooth. The difference is small in the captures: `wCurly`'s curls are loose. The cost is in memory and evaluation, not
in the rasterizer: twice the asset's points (12 MB more), twice the guide state of every instance (10 MB more each),
and 0.8 ms more in `Hair::Evaluate` whenever the groom is near, because the vertex LOD only sheds the extra points
with distance.

### Weak points

As measured for the proof of concept, before partial coverage. Point 4 is partly answered on the layered path, whose
rasterizer stops at a pixel's third layer ([Layered path measurements](#layered-path-measurements)). Points 3 and 5 are what
[Partial coverage measurements](#partial-coverage-measurements) is about: 5 no longer holds with coverage on, and 3 can
now be traded against with `detail_exponent`. Point 1, and the root binding of point 2, are what
[View-following measurements](#view-following-measurements) is about: the simulation and root binding now follow the
view. The memory of point 2 is what [On-demand memory measurements](#on-demand-memory-measurements) is about: it is
no longer held per instance. In order of what they cost the claims under test:

1. **The simulation's cost follows the grooms nearby, not the hair on screen** (the fly-through, and 4.0 ms facing
   away). The simulation level is from distance alone, and the guide-level pass evaluates every guide of every simulated
   groom whether a drawn strand uses it or not.
2. **Per-instance state is held for every instance.** 96 B a strand of root frames and 80 B a guide point, in view or
   not: 302 MB for the crowd, 1.1 GB at the builder's default guide count. Root binding writes every root every frame
   for the same reason.
3. **The detail level falls with distance, and coverage with its square.** A far groom is drawn at tens of strands per
   pixel; from the far camera the crowd draws 99,000 strands into 7,100 pixels.
4. **The rasterizer pays for depth.** 29 to 245 writes per hair pixel. Nothing rejects a strand behind other strands.
5. **Width compensation and the boundary strand's fade are inert** with a one-pixel-minimum rasterizer and no coverage.
6. **The vertex LOD is cruder than the strand LOD on curls**, as the known gap in [Selection](#selection) predicted.
7. **The simulate pass has a floor of about 0.6 ms** (0.2 ms for short guides) however few guides are simulated.
8. **Fixed limits.** The strand pool overflowed at its first size with 100 small grooms; the visible-cluster index
   holds 63 full-size grooms.

### Limitations

- One machine, one session, about 60 frames per figure. Times under about 0.1 ms are not resolved (see the method).
- The CPU side of the plugin was not measured; the frame times above are for context.
- The profiler's frames have a time step of 0: the simulation was stepped by `frame_seconds`, and the heads' sway is the
  only motion.
- The crowd's grooms are small (8,000 to 12,000 strands, 1,024 guides) on heads without bodies, on an empty floor:
  nothing occludes hair but heads. Production grooms are five to ten times the strands.
- Image differences are of presented 8-bit frames after the upscaler, over regions that are mostly background; they
  compare runs, and are not a perceptual measure.
- Hair is still an opaque placeholder surface with no shadow of its own; none of the passes that would follow
  (shadows, a hair BSDF, layered visibility) is in these costs.

## Partial coverage measurements

Measured on 2026-11-27 for the first step after the proof of concept: analytic coverage in the rasterizer, used by
stochastic acceptance into the one visibility layer ([The rasterizer](#the-rasterizer)), a coverage target, and a
steeper detail rule. Same machine, conditions and method as [POC measurements](#poc-measurements): raster mode, DLSS
at 2560x1600 in and out, the display awake, uncapped, times from the statistical profiler at a 3% half-width. "Off" is
`coverage` `off`, which is the rasterizer of the proof of concept; "on" is `white` unless said. Counters are means over
32 consecutive frames (the camera's jitter and the random pattern both change every frame). Image differences are mean
absolute differences of 8-bit luma of presented frames, after the upscaler. The scripts are not in the tree.

**What the counters mean here.** `pixels_resolved` is the hair pixels a frame shows: with coverage off, every pixel
under any footprint; with it on, the pixels in which some strand was accepted, so its mean over frames is the hair's
expected visible area. `coverage_sum` is the strands' true projected area with overlaps counted, an optical depth
summed over the screen, and does not depend on the mode.

### The test grooms are thin

The first thing true coverage shows is what the grooms are. The proof of concept's dolly groom (24,000 strands of 80
to 20 micrometres on a head a metre across) has 126,000 pixels of true strand area over the 593,000 pixels its
one-pixel strands fill at 2 m: with coverage on it is a faint haze over a bare sphere. That is what such a groom is,
and the one-pixel floor was drawing it five times too dense. The measurements below therefore use grooms with enough
hair: the same groom with strands of 0.4 to 0.2 mm ("thin": 63% of the opaque pixels show hair at 2 m) and 1.6 to
0.8 mm ("thick": 97%), the distance series' groom (30,000 strands of 88 to 44 micrometres on an 11 cm head, about 20 cm
long: 74% at 0.8 m, 69% at 3.2 m, with the scalp showing through the crown where the hair lies one layer deep), and
`wCurly` (50,000 strands: all of them at 0.8 m, 95% at 3.2 m). The crowd's grooms (8,000 to 12,000 such strands a head) are
sparse too: from the crowd's near camera 119,000 pixels show hair against 259,000 opaque, and from the far one 2,600
against 7,100. A groom authored against the opaque form needs more or wider strands to look the same with coverage.

### The mechanisms are live

The proof of concept's width test ([Continuity](#continuity)), which showed no pixel changing with the width scale or
with a strand four times as wide. The thin groom; "hair pixels" is `pixels_resolved`, "area" is `coverage_sum`. These
runs predate the exact width scale (below), so the compensated area is about 1% over the full groom's.

| Distance | Strands drawn | Hair pixels, off (any width scale) | On, `max_width_scale` 8 | On, 1 | On, every strand drawn | Area, scale 8 | Area, scale 1 | Area, every strand |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 m | 24,000 | 593,412 | 374,948 | 374,910 | 374,985 | 772,405 | 772,407 | 772,405 |
| 8 m | 24,000 | 34,083 | 19,326 | 19,366 | 19,367 | 41,490 | 41,489 | 41,492 |
| 16 m | 22,437 | 8,519 | 4,735 | 4,564 | 4,705 | 10,039 | 9,266 | 9,959 |
| 24 m | 15,231 | 3,796 | 2,127 | 1,660 | 2,111 | 4,459 | 2,763 | 4,414 |
| 32 m | 11,472 | 2,151 | 1,210 | 789 | 1,194 | 2,545 | 1,180 | 2,517 |
| 48 m | 7,722 | 972 | 549 | 267 | 541 | 1,151 | 355 | 1,138 |
| 80 m | 4,726 | 364 | 204 | 68 | 202 | 428 | 79 | 422 |

- **Width compensation now does what it was for.** At 80 m a fifth of the strands are drawn. Without widening they
  show a third of the hair pixels of the full groom (68 against 202); widened, the same (204). Off, the three columns
  are one number at every distance, as the proof of concept found.
- **Strand width shows.** The thick groom shows 29,509 hair pixels at 8 m and 310 at 80 m against the thin one's 19,326
  and 204; off, both are 34,083 and 364.
- **The opaque form over-covers.** Its hair pixels are 1.6 to 1.8 times what the thin groom's true coverage shows, at
  every distance, and 5.6 times for the proof of concept's own strand widths.
- `white` and `interleaved` gave the same means to within their frame-to-frame spread in every cell (not shown).
- Accepted writes (`pixels_written`) equal `coverage_sum` to 0.1% in every cell: the acceptance is unbiased. From 8 m on they
  are a twenty-fifth to a fiftieth of the opaque form's writes (20,100 against 430 at 80 m).
- With coverage off the counters are those of the proof of concept's tables to the pixel (593,412, 34,083, 3,796, 364).

### One strand leaving

What the frame-mean metric of [Continuity](#continuity) could not see. A sparse groom (1,000 straight strands 0.28
pixels wide at the root, well apart on screen), the camera still, and the forced detail level swept from 0.9 to 0.5 in
160 steps, so that about two and a half strands of the groom leave per step, one cluster at a time. Each step waits 8
frames and captures; the change from the step before is taken per 8x8 pixel block (a strand is local), clear of the
menus. "Static" is the same state captured twice.

| | Off | On |
|---|---:|---:|
| Strands drawn | 914 to 512 | 914 to 512 |
| Footprint pixels (what the opaque form shows) | 119,726 to 67,558 | (not shown as such) |
| True area, `coverage_sum` | 34,582 to 34,962 | 34,587 to 34,975 |
| Largest block change in a step: median, 90th percentile, largest of the sweep | 15.2, 18.8, 21.8 | 2.3, 4.4, 4.4 |
| Largest block change, static | 3.3 (4.5 at most) | 2.3 (4.4 at most) |
| Blocks changing by more than 4 levels in a step, mean | 20.7 | 0.13 |

With coverage off every step has a block that changes by 15 levels of 255 (a strand goes at its full one pixel in one
frame, as the proof of concept said it must), and the groom loses 44% of its drawn pixels over the sweep while its
true area is constant. With coverage on the largest change of any block in a step is the noise of a still image: the
leaving strand fades by width, the kept ones widen, and neither can be found in the frames.

### The detail rule

With coverage real, the kept strands' width has to carry the dropped strands' coverage, and two things were in the
way.

- **`max_width_scale` 8 was the limiter, for the spacing rule too.** Beyond the distance where a cluster draws under an
  eighth of its strands the widening stopped: at 51 m the distance series' groom kept 61% of its area with the strand
  LOD alone (188 pixels of 307), and with `detail_exponent` 2 it kept 67% at 12.8 m and 21% at 25.6 m, visibly thinning
  to the scalp. The default is now 64, the most strands a cluster holds.
- **`1 / l` is not exactly the compensation.** It ignores the cluster's offset in the count (1% too wide on average)
  and grows without bound once a cluster is down to its one strand (7% too much area at 25.6 m and 30% at 51 m under
  exponent 2). The scale is now the cluster's strands over the strands' worth it draws
  ([Evaluation](#evaluation-and-the-strand-pool)).

With both, the strand LOD keeps the groom's area at every exponent tried: 4,688, 4,691, 4,699 and 4,686 pixels at
12.8 m for exponents 1, 1.5, 2 and 3 against 4,694 for every strand, and within 0.3% at every other distance, for both
grooms.

Each setting against the full groom (every strand, no vertex LOD) from the same camera, coverage on in both, as
[Curls under reduction](#curls-under-reduction) did; each image is the mean of 8 captures. "Strand LOD alone" turns the
vertex LOD off. `max_width_scale` 64 throughout.

The distance series' procedural groom:

| Distance | Same image twice | Exponent | Strands | Segments | Footprint pixels | Hair pixels | Area | Difference | Strand LOD alone: difference |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 6.4 m | 1.42 | full | 30,000 | 960,000 | 1,073,942 | 5,334 | 18,646 | | |
| | | 1 | 18,763 | 300,208 | 668,877 | 5,324 | 18,405 | 1.74 | 1.67 |
| | | 1.5 | 15,085 | 241,360 | 540,287 | 5,320 | 18,408 | 1.84 | 1.83 |
| | | 2 | 12,336 | 197,376 | 444,369 | 5,320 | 18,389 | 2.00 | 2.01 |
| | | 3 | 8,687 | 138,992 | 316,912 | 5,339 | 18,381 | 2.46 | 2.39 |
| 12.8 m | 1.36 | full | 30,000 | 960,000 | 543,714 | 1,344 | 4,694 | | |
| | | 1 | 9,712 | 77,696 | 171,908 | 1,330 | 4,523 | 1.95 | 1.67 |
| | | 1.5 | 5,765 | 46,120 | 102,683 | 1,334 | 4,525 | 2.16 | 1.77 |
| | | 2 | 3,563 | 28,504 | 64,125 | 1,327 | 4,531 | 2.45 | 2.12 |
| | | 3 | 1,769 | 14,152 | 32,435 | 1,338 | 4,516 | 3.70 | 3.36 |
| 25.6 m | 1.37 | full | 30,000 | 960,000 | 278,467 | 343 | 1,194 | | |
| | | 1 | 4,979 | 19,916 | 42,187 | 328 | 1,077 | 3.06 | 1.53 |
| | | 1.5 | 2,201 | 8,804 | 18,707 | 329 | 1,079 | 3.17 | 1.87 |
| | | 2 | 1,252 | 5,008 | 10,710 | 333 | 1,074 | 3.64 | 2.18 |
| | | 3 | 1,060 | 4,240 | 9,024 | 331 | 1,074 | 3.76 | 2.28 |
| 51.2 m | 0.66 | full | 30,000 | 960,000 | 144,420 | 90 | 307 | | |
| | | 1 | 2,616 | 5,232 | 9,879 | 77 | 245 | 4.01 | 0.69 |
| | | 1.5 | 1,124 | 2,248 | 4,298 | 79 | 244 | 3.90 | 0.79 |
| | | 2 | 1,035 | 2,070 | 4,014 | 78 | 244 | 4.05 | 0.83 |
| | | 3 | 1,024 | 2,048 | 3,982 | 78 | 244 | 4.03 | 0.83 |

`wCurly`:

| Distance | Same image twice | Exponent | Strands | Segments | Footprint pixels | Hair pixels | Area | Difference | Strand LOD alone: difference | Brightness | Brightness, strand LOD alone |
|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 6.4 m | 0.73 | full | 50,000 | 1,600,000 | 3,368,679 | 5,008 | 273,077 | | | 209.1 | |
| | | 1 | 35,336 | 821,936 | 2,298,236 | 4,979 | 265,020 | 1.58 | 0.97 | 209.7 | 209.1 |
| | | 1.5 | 30,014 | 693,688 | 1,946,795 | 4,972 | 265,071 | 1.66 | 1.12 | 209.7 | 209.1 |
| | | 2 | 25,799 | 592,368 | 1,668,540 | 4,964 | 265,016 | 1.82 | 1.28 | 209.9 | 209.2 |
| | | 3 | 19,704 | 446,568 | 1,269,027 | 4,986 | 265,098 | 2.11 | 1.66 | 209.7 | 209.0 |
| 12.8 m | 0.73 | full | 50,000 | 1,600,000 | 1,695,811 | 1,262 | 68,457 | | | 211.7 | |
| | | 1 | 17,843 | 204,836 | 560,951 | 1,233 | 63,928 | 3.32 | 1.11 | 213.8 | 211.9 |
| | | 1.5 | 10,886 | 124,048 | 340,968 | 1,238 | 63,943 | 3.37 | 1.37 | 213.7 | 211.6 |
| | | 2 | 6,780 | 76,648 | 211,528 | 1,241 | 63,929 | 3.60 | 1.88 | 213.7 | 211.6 |
| | | 3 | 2,914 | 32,552 | 101,567 | 1,252 | 64,306 | 5.14 | 4.50 | 212.9 | 211.1 |
| 25.6 m | 0.79 | full | 50,000 | 1,600,000 | 863,450 | 326 | 17,394 | | | 214.0 | |
| | | 1 | 9,123 | 52,202 | 141,345 | 310 | 15,710 | 4.24 | 1.15 | 217.0 | 214.0 |
| | | 1.5 | 4,125 | 23,416 | 63,689 | 313 | 15,717 | 4.25 | 1.68 | 216.7 | 213.8 |
| | | 2 | 2,062 | 11,674 | 31,827 | 312 | 15,725 | 5.20 | 2.92 | 216.9 | 214.3 |
| | | 3 | 1,040 | 6,018 | 16,156 | 312 | 15,713 | 5.74 | 3.92 | 216.7 | 214.4 |
| 51.2 m | 0.31 | full | 50,000 | 1,591,376 | 443,598 | 87 | 4,464 | | | 241.1 | |
| | | 1 | 4,805 | 13,730 | 36,065 | 82 | 3,788 | 1.90 | 0.41 | 242.7 | 241.1 |
| | | 1.5 | 1,771 | 5,031 | 13,276 | 82 | 3,788 | 1.97 | 0.72 | 242.8 | 241.2 |
| | | 2 | 1,030 | 2,981 | 7,744 | 82 | 3,787 | 1.99 | 0.89 | 242.7 | 241.3 |
| | | 3 | 1,024 | 2,969 | 7,709 | 82 | 3,788 | 1.94 | 0.90 | 242.7 | 241.2 |

At 3.2 m every exponent draws 98% of the strands or more and every difference is the noise.

- **Curls, strand LOD alone: the drift is gone.** The proof of concept measured the strand LOD alone at 3.30 levels from
  the full groom at 12.8 m and 4.02 at 25.6 m, with the groom brighter and its fringe thinner (hair pixels down 5% and
  9% with both LODs). With coverage it is 1.11 and 1.15 against 0.73 and 0.79 for the same image twice, the brightness
  is the full groom's to 0.2 levels (211.9 against 211.7, 214.0 against 214.0), and the hair pixels are the full
  groom's (1,263 and 327 against 1,262 and 326).
- **What drift remains is the vertex LOD's.** With it on the groom is 2 to 3 levels brighter at 12.8 and 25.6 m, loses
  7% and 10% of its area and 2% and 5% of its hair pixels, and differs by 3.3 and 4.2 levels, at every exponent. The
  drawn level comes from the cluster's centreline ([Selection](#selection)), and curls drawn along their chords are
  shorter strands: less area. The straight-ish procedural groom loses 4% at 12.8 m, 10% at 25.6 m and 20% at 51 m the
  same way. Coverage made this measurable; nothing compensates for it yet (the width could carry the lost length).
- **How far the exponent can go.** Against the spacing rule, with the strand LOD alone: 1.5 adds 0.1 to 0.5 levels
  (0.10 and 0.34 on the procedural groom at 12.8 and 25.6 m, 0.26 and 0.53 on `wCurly`) for 39% to 56% fewer strands
  there; 2 adds 0.45 to 1.8 for 62% to 77% fewer; 3 adds 0.75 to 3.4 and its fewer, wider strands read as grain (the
  temporal noise falls as the image coarsens). The same image twice differs by 0.7 to 1.4. With the vertex LOD on, its
  own difference hides most of this. From 51 m every exponent is at one strand a cluster (1,024) and they are the same.
- **Proposed default: 1.5, with `max_width_scale` 64.** The shipped default of `detail_exponent` is still 1: the
  evidence for 1.5 is that its cost is under the noise of the measurement on still images, which is not yet evidence
  about motion, and what it buys in time is small (below). `max_width_scale` did change, because 8 loses coverage
  under the rule as it was.

### Cost

`Hair::Raster` in ms, one groom by distance (the proof of concept's series), and the crowd. "Target" adds
`coverage_accumulate`. Written pixels are the visibility writes.

| | Off | On | On + target | Off + target | Written, off | Written, on |
|---|---:|---:|---:|---:|---:|---:|
| Procedural, 0.4 m | 1.30 | 1.00 | 1.62 | 2.07 | 15,773,294 | 5,212,475 |
| Procedural, 0.8 m | 0.71 | 0.52 | 0.69 | | 9,054,521 | 1,275,080 |
| Procedural, 1.6 m | 0.35 | 0.32 | 0.36 | | 4,802,250 | 302,913 |
| Procedural, 3.2 m | 0.26 | 0.23 | 0.25 | 0.33 | 2,654,618 | 74,130 |
| Procedural, 6.4 m | 0.14 | 0.13 | 0.13 | | 844,957 | 18,490 |
| Procedural, 25.6 m | 0.08 | 0.07 | 0.07 | | 51,803 | 1,023 |
| Procedural, 100 m | 0.07 | 0.07 | 0.07 | | 2,811 | 19 |
| `wStraight`, 0.4 m | 4.92 | 4.90 | 10.37 | 10.41 | 65,784,446 | 65,133,147 |
| `wStraight`, 0.8 m | 1.46 | 1.24 | 2.09 | | 23,094,581 | 15,039,867 |
| `wStraight`, 1.6 m | 0.62 | 0.60 | 0.71 | | 11,709,798 | 3,676,148 |
| `wStraight`, 3.2 m | 0.35 | 0.34 | 0.37 | 0.52 | 5,812,555 | 915,165 |
| `wStraight`, 6.4 m | 0.18 | 0.16 | 0.17 | | 1,742,928 | 229,677 |
| `wStraight`, 25.6 m | 0.09 | 0.08 | 0.09 | | 132,133 | 13,779 |
| `wStraight`, 100 m | 0.07 | 0.07 | 0.07 | | 9,800 | 280 |
| Crowd, near camera | 0.65 | 0.58 | 0.63 | 0.81 | 7,527,778 | 229,094 |
| Crowd, far camera | 0.16 | 0.15 | 0.15 | 0.16 | 259,802 | 2,846 |

- **Coverage itself costs nothing and saves a little.** The pass's time is the footprint pixels it tests and the
  atomics it issues; an unaccepted pixel skips its `InterlockedMin`. On is 0 to 27% cheaper than off, most where
  strands are thin on screen. The random number and the coverage arithmetic are not visible in the times.
  `interleaved` reads as `white` (1.63 against 1.62 and 0.25 against 0.25 with the target, 0.62 against 0.63 in the
  crowd).
- **The coverage target is not a small fraction, so it is off by default.** It is a second atomic for every footprint
  pixel in front of the opaque surface: +5.5 ms (2.1 times) with `wStraight` filling the view, +0.6 ms (1.6 times) for
  the procedural groom there, +7% to +70% between 0.8 and 3.2 m, nothing measurable far away. The `coverage` debug
  view turns it on while it is shown. A composite that needs it would have to pay this or bound the overdraw first
  (weak point 4).
- `Hair::Evaluate` does not change. `Hair::Resolve` read 0.73 against 0.68 ms for the procedural groom at 0.4 m, with a
  fifth fewer hair pixels; why was not looked into. Elsewhere it is within 0.02 ms.

The crowd under the detail rule, coverage on with the target (hair total is all seven passes, simulation off; root
binding is 0.67 ms of it):

| Camera | Exponent, `max_width_scale` | Strands drawn | Footprint pixels | Hair pixels | Evaluate | Raster | Hair total |
|---|---|---:|---:|---:|---:|---:|---:|
| near | off, 8 (the proof of concept) | 293,838 | 7,527,778 | 259,244 | 0.53 | 0.65 | 2.01 |
| near | 1, 8 | 293,838 | 6,474,283 | 119,480 | 0.53 | 0.63 | 2.00 |
| near | 1.5, 64 | 209,733 | 5,618,669 | 119,623 | 0.45 | 0.54 | 1.83 |
| near | 2, 64 | 160,720 | 5,073,905 | 119,711 | 0.40 | 0.49 | 1.73 |
| far | off, 8 (the proof of concept) | 99,004 | 259,802 | 7,149 | 0.19 | 0.16 | 1.10 |
| far | 1, 8 | 99,004 | 236,325 | 2,002 | 0.19 | 0.15 | 1.10 |
| far | 1.5, 64 | 36,623 | 86,256 | 2,603 | 0.17 | 0.13 | 1.05 |
| far | 2, 64 | 24,245 | 58,828 | 2,654 | 0.16 | 0.13 | 1.04 |

With the proposed default the crowd's hair is 1.83 ms from the near camera and 1.05 ms from the far one, 9% and 5%
under the rule as it is, for 29% and 63% fewer strands. The far camera's hair pixels rise from 2,002
to 2,603 because `max_width_scale` 8 was losing coverage there. Time is the smaller part of what the exponent buys:
the floors (root binding, the clear, the dispatch over the pool) do not move. The strand pool does.

### Temporal quality

A pixel is hair in some frames and background in others, and the upscaler has to make a steady blend of it. Each
figure is over the square around the groom. "Temporal noise" is the per-pixel standard deviation of luma over 8
captures 3 frames apart with everything still, averaged over the square; "fine contrast" is the standard deviation of
a Laplacian (what [Checking it](#checking-it) used for softness), still and with the head swaying 2 cm to each side
every two seconds.

| Groom, distance | Hair pixels, off | On | Temporal noise, off | On | 99th percentile pixel, off | On | Fine contrast still, off | On | Swaying, off | On |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Procedural, 0.8 m | 498,837 | 369,394 | 0.83 | 1.58 | 27.7 | 10.0 | 47.5 | 42.3 | 40.9 | 40.0 |
| Procedural, 3.2 m | 30,911 | 21,419 | 0.13 | 1.22 | 1.0 | 7.2 | 22.6 | 15.0 | 14.7 | 12.8 |
| Procedural, 12.8 m | 1,976 | 1,326 | 0.13 | 1.20 | 1.2 | 6.8 | 28.9 | 21.3 | 23.4 | 18.9 |
| `wCurly`, 0.8 m | 354,002 | 356,996 | 0.90 | 1.00 | 32.4 | 22.2 | 46.4 | 44.1 | 40.3 | 39.5 |
| `wCurly`, 3.2 m | 21,225 | 20,211 | 0.17 | 0.54 | 1.1 | 4.1 | 26.4 | 13.6 | 19.8 | 11.6 |
| `wCurly`, 12.8 m | 1,327 | 1,233 | 0.19 | 0.54 | 1.2 | 4.1 | 31.0 | 22.1 | 26.5 | 20.9 |

- **Still camera: a residue the upscaler does not remove.** In the middle and far views a still frame is three times
  noisier with coverage on for the dense groom and nine times for the sparse one, where most pixels are a blend: about
  half a level and one level of 255 on average, 4 to 7 at the worst hundredth of the pixels. In the captures it reads
  as a fine, slow grain in the translucent parts (the fringe, and the scalp seen through thin hair), not as flicker of
  the outline. Close up, where strands are a pixel wide or more, it is the opaque form's own (1.0 against 0.9), and the
  worst pixels are calmer than the opaque form's crawling strand edges (22 against 32).
- **What it looks like.** `wCurly` at 3.2 m: off is hard-edged, with wiry fly-aways a full pixel wide and a ragged,
  stair-stepped outline; on is soft, the fly-aways are faint, and the outline feathers out as hair does. Half the fine
  contrast is gone (13.6 against 26.4): most of that was aliasing, some of it is strand detail that one sample a pixel
  cannot hold. At 12.8 m off is a blocky blob and on a smooth one of the same shape. Close up (0.8 m) the two differ
  only at the fringe, where single strands go from solid lines to thinner, lighter ones. Dense hair does not let the
  background through: where the full groom's strands stack up the first accepted strand is nearly always a strand.
- **Moving.** With the head swaying, fine contrast with coverage on falls by 5% to 15% from still, against 13% to 35%
  for the opaque form: the softened image has less to lose. No trail or second copy in the captures looked at. A camera sliding
  0.6 m a second past a sparse groom 2 to 3.5 m away is the worst case found: the translucent fringe turns into
  a streaky, grainy veil while the camera moves (one frame's accepted pixels, smeared, where the opaque form shows
  separate strands), and what is behind it shows through unevenly. It settles when the camera stops. Two frames after the
  stop the hair region is 5.5 levels from the converged image (3.3 for the opaque form), and two converged captures
  30 frames apart differ by 2.1 against 0.4.
- **Motion vectors.** `hair.probe` agrees as before with coverage on: for the moving groom 22,196 probed pixels,
  expected motion 3.5 to 5.6 pixels, error 0.0016 pixels on average and 0.0052 at most; for the moving camera 43,462
  pixels, 3.2 to 6.9 pixels, error 0.0023 and 0.0054. A pixel in which no strand was accepted has no hair in it that
  frame: it holds the motion vector, depth and surface of the opaque surface behind, so in a blended pixel the upscaler
  sees two motions and two depths alternate. That is where the residue and the veil under camera motion come from,
  and no choice of random pattern changes it.
- **`white` against `interleaved`.** Not told apart: temporal noise 0.60 against 0.60 and 0.49 against 0.49 on a
  procedural groom at 12 and 48 m, 0.63 against 0.63 and 0.59 against 0.60 on `wCurly`, and the images are alike.

### The dolly

The dolly of [Continuity](#continuity) again (a capture at every second step, four frames after the camera moves), on
the thin groom, with the selection live and frozen at the near end, and live with coverage off for reference.

| LOD | Coverage | Strands, 2 m to 80 m | Mean difference 2-8 m | 8-20 m | 20-44 m | 44-80 m | Pairs over 1.25x | Largest ratio | Same camera twice: 2 m, 10 m, 21 m, 80 m |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| live | on | 24,000 to 4,726 | 0.8983 | 0.0644 | 0.0145 | 0.0081 | 2 | 1.50 | 0.525, 0.034, 0.011, 0.007 |
| frozen | on | 24,000 to 24,000 | 0.8982 | 0.0648 | 0.0145 | 0.0080 | 5 | 1.39 | 0.515, 0.034, 0.009, 0.003 |
| live | off | 24,000 to 4,726 | 1.3365 | 0.0821 | 0.0146 | 0.0082 | 6 | 1.56 | 0.110, 0.016, 0.006, 0.006 |

- **Live and frozen agree to 1% in every band** with coverage on (they agreed to 9% without), and the frozen run again
  has more outlying pairs than the live one. No outlying pair is shared between the two, and the ratio has no
  correlation with the strands dropped or the clusters changing vertex level (-0.01 and -0.05). As before, the
  outliers are all beyond 34 m, where a pair's difference is about a hundredth of a level and so is a still pair's.
- **The still-camera residue shows here too:** two captures of one view differ by 0.52 levels at 2 m with coverage on
  against 0.11 off, by 0.034 against 0.016 at 10 m, and by the same from about 28 m, where the groom is too small to
  move the frame's mean.
- This metric still bounds the whole image and not a strand; [One strand leaving](#one-strand-leaving) is the measure
  of that.

### What this says about the design

Stochastic coverage into the one layer is good enough to build on for the level of detail, which is what it was for:
the count and width mechanisms now act, a strand leaves without a trace, the strand LOD holds coverage, brightness and
outline to the noise, and it costs nothing in the rasterizer. As the final image it is a trade and not a clear win.
Dense hair looks better at every distance beyond arm's length (anti-aliased, with a true fringe). Sparse or
translucent hair carries a grain when still and turns to a noisy veil under fast camera motion, because one opaque sample a pixel
cannot hold a blend and the upscaler is given alternating depths and motions. That part is what the layered target
with a hair composite would fix (hair blended over a stable opaque frame, with its own motion), and it is the reason to
build it; the coverage arithmetic, the joint rule, the exact width scale and the coverage target carry over to it
unchanged. Until then `coverage` `off` remains for content that needs the steadier image.

Not addressed here: the vertex LOD's loss of area on curled strands, a strand wider than a pixel still has hard edges,
hair still casts no shadow (thinner hair over an unshadowed scalp reads lighter than it should), and the grooms and
scenarios were authored against the opaque form.

**Scenarios.** All five pass with coverage on (the default) and with `HAIR_COVERAGE=off`, which every hair scenario
reads and passes to `hair.lod`. With it off, the selection counters of every sampled frame are those of the runs before
coverage. One check changed: `hair_sim.cs` compares the rigid groom's rasterizer counters before and after the
simulation to one part in 10,000, and with coverage on the written and resolved pixels are random draws that differ by
a few parts in a thousand between frames of one view, so there it allows 2%.

## View-following measurements

Measured on 2026-11-27 for the second step after the proof of concept: the simulation, the guide-level pass and root
binding follow the view. Same machine, conditions and method as [POC measurements](#poc-measurements): raster mode,
DLSS at 2560x1600 in and out, the display awake, uncapped, times from the statistical profiler at a 3% half-width with
the camera held still (or turning at a steady rate, where said), `frame_seconds` 1/60, heads swaying 2 cm while
simulated. "Before" is the tree as [Partial coverage measurements](#partial-coverage-measurements) left it, measured
again in the same session; coverage is on (`white`) in both. The scripts are not in the tree.

**What changed.** Three things, which show in separate columns below:

1. *Root binding* runs after selection and writes the roots of the drawn strands and of simulated instances' guides
   (the root-binding pass, since removed: [Root frames](#root-frames)), not every root of every instance. It is one dispatch (two with
   simulation) whatever the number of grooms. The select pass works out its one root per cluster itself.
2. *Simulation* is for instances a view shows ([Following the view](#following-the-view)): the CPU tests each
   instance's bounds against the main camera's frustum every frame. An instance out of view stops after a quarter of a
   second with its shape kept, and goes on from it in the frame it comes back.
3. *The guide-level pass* was not changed: it covers the simulated instances, and those are now the ones in view. The
   guides in use were measured and marking them was not built (below).

### The crowd, before and after

[`hair_crowd.cs`](../Scripting/Scenarios/hair/hair_crowd.cs)'s crowd, 100 grooms and 1,004,000 strands, from the three
cameras of [Many grooms](#many-grooms-and-the-per-instance-dispatches). Times in ms.

| Camera | Simulation | | In view | Strands drawn | Roots bound | Simulated grooms | Guides simulated | Guides evaluated | Root binding | Simulate | Guide levels | Select | Evaluate | Raster | Resolve | Hair total |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| near: 2 m in front, looking down the grid | off | before | 70 | 293,838 | 1,004,000 | 0 | 0 | 0 | 0.67 | 0.00 | 0.00 | 0.03 | 0.53 | 0.58 | 0.13 | **1.94** |
| near: 2 m in front, looking down the grid | off | after | 70 | 293,838 | 293,838 | 0 | 0 | 0 | 0.13 | 0.00 | 0.00 | 0.03 | 0.53 | 0.58 | 0.13 | **1.40** |
| near: 2 m in front, looking down the grid | on | before | 70 | 293,838 | 1,004,000 | 48 | 3,446 | 45,706 | 0.67 | 0.93 | 1.78 | 0.04 | 1.25 | 0.59 | 0.13 | **5.39** |
| near: 2 m in front, looking down the grid | on | after | 70 | 293,838 | 320,462 | 26 | 2,006 | 24,618 | 0.16 | 0.63 | 0.96 | 0.03 | 1.26 | 0.59 | 0.13 | **3.76** |
| behind the last row, facing away | off | before | 0 | 0 | 1,004,000 | 0 | 0 | 0 | 0.67 | 0.00 | 0.00 | 0.03 | 0.02 | 0.11 | 0.05 | **0.87** |
| behind the last row, facing away | off | after | 0 | 0 | 0 | 0 | 0 | 0 | 0.02 | 0.00 | 0.00 | 0.03 | 0.02 | 0.11 | 0.05 | **0.22** |
| behind the last row, facing away | on | before | 0 | 0 | 1,004,000 | 60 | 4,032 | 57,408 | 0.67 | 0.94 | 2.22 | 0.04 | 0.02 | 0.11 | 0.05 | **4.04** |
| behind the last row, facing away | on | after | 0 | 0 | 0 | 0 | 0 | 0 | 0.02 | 0.00 | 0.00 | 0.03 | 0.02 | 0.11 | 0.05 | **0.22** |
| 52 m in front, the whole grid small | off | before | 100 | 99,004 | 1,004,000 | 0 | 0 | 0 | 0.67 | 0.00 | 0.00 | 0.03 | 0.18 | 0.15 | 0.05 | **1.08** |
| 52 m in front, the whole grid small | off | after | 100 | 99,004 | 99,004 | 0 | 0 | 0 | 0.06 | 0.00 | 0.00 | 0.03 | 0.17 | 0.15 | 0.05 | **0.47** |
| 52 m in front, the whole grid small | on | before | 100 | 99,004 | 1,004,000 | 0 | 0 | 0 | 0.67 | 0.00 | 0.00 | 0.04 | 0.18 | 0.16 | 0.05 | **1.10** |
| 52 m in front, the whole grid small | on | after | 100 | 99,004 | 99,004 | 0 | 0 | 0 | 0.07 | 0.00 | 0.00 | 0.03 | 0.18 | 0.15 | 0.05 | **0.48** |

- **With the whole crowd behind the camera, hair costs 0.22 ms, simulated or not** (it was 4.04 ms simulated and
  0.87 ms rigid). The simulate and guide-level passes record nothing. Root binding is 0.02 ms: one dispatch over the
  22,528 clusters the frame could show, each thread group of which returns at once. What is left is the floors of
  [Cost against distance](#cost-against-distance-one-groom): the select pass's 100 dispatches (0.03 ms), the
  rasterizer's clear and its dispatch (0.11 ms) and the resolve's pass over every pixel (0.05 ms).
- **Root binding follows the drawn strands:** about 0.4 ms per million roots written, over 0.02 ms. 0.13 ms for the
  near camera's 293,838 strands and 0.06 ms for the far camera's 99,004, against 0.67 ms for every root. From the far
  camera, where it was two thirds of hair's cost, hair goes from 1.08 to 0.47 ms. Skipping only the instances out of
  view would have left the far camera where it was (every groom is in view there) and the near one at about 0.47 ms
  (70 of 100 grooms in view): binding the drawn roots is what bought most of it.
- **From the near camera the simulated crowd costs 3.76 ms, from 5.39 ms.** 22 of the 48 grooms that were simulated
  were beside the camera and out of its view. The simulate pass is at its floor (0.63 ms; below), the guide-level pass
  halves with the grooms, and evaluation, the rasterizer and the resolve are unchanged, as they should be.

### The fly-through

The nine cameras of the proof of concept's fly-through (z from 2.0 to -25.5 down the aisle, looking along -z), each
held still. The claim to check was that the simulated crowd's cost falls as grooms leave the view; before, it rose.

| Camera z | In view | Strands drawn | Hair pixels | Simulated grooms, before | after | Guides simulated, before | after | Guides evaluated, before | after | Hair total, simulation off, before | after | Root binding, before | after | Simulate, before | after | Guide levels, before | after | Hair total, before | after |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2.0 | 70 | 293,850 | 113,619 | 48 | 26 | 3,445 | 2,006 | 45,707 | 24,618 | 1.97 | 1.43 | 0.68 | 0.16 | 0.94 | 0.63 | 1.78 | 0.96 | **5.39** | **3.74** |
| -1.4 | 56 | 249,521 | 47,006 | 62 | 25 | 5,080 | 1,675 | 58,408 | 23,925 | 1.74 | 1.18 | 0.67 | 0.14 | 1.31 | 0.63 | 2.24 | 0.95 | **6.02** | **3.52** |
| -4.9 | 44 | 226,294 | 68,145 | 73 | 23 | 6,204 | 1,720 | 68,548 | 21,832 | 1.92 | 1.36 | 0.67 | 0.14 | 1.66 | 0.62 | 2.64 | 0.85 | **7.03** | **3.70** |
| -8.3 | 30 | 181,032 | 89,335 | 88 | 24 | 6,963 | 1,883 | 83,149 | 22,693 | 1.73 | 1.15 | 0.67 | 0.13 | 1.77 | 0.71 | 3.24 | 0.85 | **7.44** | **3.47** |
| -11.8 | 18 | 117,283 | 40,716 | 97 | 18 | 7,527 | 1,268 | 91,801 | 17,164 | 1.49 | 0.91 | 0.67 | 0.10 | 1.71 | 0.63 | 3.54 | 0.74 | **7.33** | **2.88** |
| -15.2 | 10 | 74,481 | 59,926 | 99 | 10 | 7,852 | 939 | 93,524 | 9,301 | 1.56 | 0.93 | 0.67 | 0.07 | 1.81 | 0.61 | 3.60 | 0.42 | **7.31** | **2.32** |
| -18.6 | 4 | 29,408 | 14,767 | 97 | 4 | 7,405 | 395 | 91,923 | 3,701 | 1.08 | 0.44 | 0.67 | 0.04 | 1.82 | 0.59 | 3.54 | 0.21 | **6.57** | **1.37** |
| -22.1 | 0 | 0 | 0 | 84 | 0 | 6,646 | 0 | 79,370 | 0 | 0.88 | 0.22 | 0.67 | 0.02 | 1.46 | 0.00 | 3.08 | 0.00 | **5.43** | **0.22** |
| -25.5 | 0 | 0 | 0 | 72 | 0 | 4,801 | 0 | 68,927 | 0 | 0.88 | 0.22 | 0.67 | 0.02 | 1.19 | 0.00 | 2.66 | 0.00 | **4.74** | **0.22** |

Before, hair went from 5.4 to 7.4 ms while the grooms in view went from 70 to 10, and was still 4.7 ms with none in
view. Now it goes from 3.7 to 2.3 ms over the same cameras, 1.4 ms with four grooms in view and 0.22 ms with none. The
simulated grooms are the ones in view that are near enough (26 of 70 at the first camera; from z = -11.8 every groom
in view is). The simulate pass does not fall with them: it is 0.6 to 0.7 ms from 26 grooms down to 4, its floor.

### Turning in place

The camera in the middle of the crowd (z = -11.25), still and then turning about the vertical at a steady rate. The
profiler's samples are spread over the turn; the counts are the range over six readings along it.

| Camera | Simulation | | In view | Simulated grooms | Guides simulated | Guides evaluated | Root binding | Simulate | Guide levels | Evaluate | Hair total |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| still | on | before | 18 to 19 | 97 | 7,415 | 91,913 | 0.66 | 2.04 | 4.11 | 0.91 | **8.14** |
| still | on | after | 18 to 19 | 21 | 1,403 | 20,101 | 0.10 | 0.63 | 0.82 | 0.91 | **2.89** |
| still | off | before | 19 | 0 | 0 | 0 | 0.66 | 0.00 | 0.00 | 0.33 | **1.43** |
| still | off | after | 19 | 0 | 0 | 0 | 0.08 | 0.00 | 0.00 | 0.33 | **0.83** |
| turning 0.5 degrees a frame | on | before | 19 to 23 | 97 | 7,415 | 91,913 | 0.66 | 2.04 | 4.11 | 1.10 | **8.47** |
| turning 0.5 degrees a frame | on | after | 19 to 23 | 21 to 23 | 1,625 to 1,781 | 19,879 to 21,884 | 0.11 | 0.77 | 0.86 | 1.12 | **3.42** |
| 2 degrees a frame | on | before | 20 to 22 | 97 | 7,415 | 91,913 | 0.66 | 2.05 | 4.12 | 1.16 | **8.51** |
| 2 degrees a frame | on | after | 19 to 23 | 26 to 30 | 1,931 to 2,281 | 24,692 to 28,439 | 0.12 | 0.91 | 1.09 | 1.09 | **3.74** |
| 2 degrees a frame | off | before | 19 to 24 | 0 | 0 | 0 | 0.66 | 0.00 | 0.00 | 0.42 | **1.63** |
| 2 degrees a frame | off | after | 19 to 24 | 0 | 0 | 0 | 0.09 | 0.00 | 0.00 | 0.41 | **1.02** |
| 6 degrees a frame | on | before | 19 to 23 | 97 | 7,415 | 91,913 | 0.66 | 2.04 | 4.12 | 1.15 | **8.50** |
| 6 degrees a frame | on | after | 20 to 23 | 42 to 43 | 2,919 to 3,037 | 40,089 to 41,085 | 0.12 | 1.13 | 1.61 | 1.11 | **4.51** |

Before, 97 grooms were simulated wherever the camera looked: 8.1 to 8.5 ms. Now a still camera simulates the 21 it
sees, 2.9 ms. Turning costs more the faster it is: a groom stays simulated for a quarter of a second after it leaves
the view (`lod_view_grace_seconds`), and one that enters starts at the level it stopped at and takes up to a second
to climb to its own, during which its other guides are evaluated. At 6 degrees a frame (a full turn a second at 60
frames a second) 42 grooms are simulated for 20 to 23 in view, and hair is 4.5 ms: still about half of what it was.

### Entering and leaving the view

A scripted run at a fixed 1/60 s frame, with the crowd's grooms given strands four times as wide (0.35 to 0.18 mm, so
that they read as hair with coverage on) and the camera 0.9 m in front of one head, facing away from it, between two
rows. After six seconds: a turn of 180 degrees in six frames (30 degrees a frame) and back, the same in one frame (a
cut) and back, the same at 3 degrees a frame and back, two full turns at 2 degrees a frame and three at 6. Then, for
comparison, the first turn again with `lod_settle_seconds` 0 after a reset, so that the grooms behind the camera have
no kept shape and start from their rigid pose when the camera reaches them. Every frame's counters and every
instance's metrics were recorded: 2,772 frames.

- **The CPU's view test and the GPU's selection agree.** In no frame was a strand drawn of a groom the CPU had left
  unsimulated for being out of view (`unseen_strands_drawn`), in no frame was a strand drawn or a guide simulated or
  evaluated with a root frame not written that frame (`stale_roots_drawn`, `stale_roots`). That includes the cut and
  the turns at 30 degrees a frame.
- **Every groom that entered did so from its kept shape, in the frame it entered.** 44 grooms started over the six
  frames of the first turn and 19 in the one frame of the cut; none from its rigid pose, though none of them had been
  in view before (they had settled unseen in the first second and a half). In the comparison run the same 44 started
  from the rigid pose.
- **No settle.** For the 19 grooms that came into view and stayed there, the mean distance of a groom's guides from
  their rigid pose, which is 0 on the rigid pose and 13 to 19 mm settled:

  | | At entry | 1.5 s later | Largest change in the first second | Its range over the next 2 s | Peak of the guides' mean speed, first half second | Next 2 s |
  |---|---:|---:|---:|---:|---:|---:|
  | kept shape, turn at 30 degrees a frame | 17.06 mm | 17.11 mm | 0.32 mm (0.54 at most) | 0.22 mm (0.65) | 21 mm/s (88) | 104 mm/s (113) |
  | kept shape, cut | 17.10 mm | 17.13 mm | 0.23 mm (0.51) | 0.20 mm (0.35) | 83 mm/s (92) | 108 mm/s (118) |
  | kept shape, turn at 3 degrees a frame | 17.14 mm | 17.13 mm | 0.29 mm (0.72) | 0.14 mm (0.24) | 95 mm/s (128) | 95 mm/s (99) |
  | rigid pose (the comparison) | 0.00 mm | 17.01 mm | 22.72 mm (25.08) | 2.62 mm (2.74) | 278 mm/s (296) | 87 mm/s (89) |

  (Medians over the grooms, the largest in brackets.) A groom that enters with its kept shape is within a third of a
  millimetre of where it will be, and what it does in its first second is what it does in any other: the sway. From
  the rigid pose it travels 23 mm in that second, at three times the speed of the sway.
- **No jump.** The discontinuity measure of [Simulation LOD](#simulation-lod): the furthest any guide of a groom moves
  in one frame, relative to its root, in the groom's first half second in view against the two seconds after. Kept
  shape: 1.2 mm against 4.4 mm at the median over the grooms for the turn at 30 degrees a frame (24.0 against 29.9 mm
  for the worst groom), 3.7 against 5.4 mm for the cut (14.7 against 16.6), 5.1 against 4.0 mm at 3 degrees a frame
  (15.7 against 16.4), 4.0 against 4.2 mm turning at 6 degrees a frame (12.4 against 16.7). From the rigid pose: 10.4
  against 5.3 mm (31.4 against 32.0). The first frame itself has no motion to measure, because nothing of the groom
  was drawn in the frame before.
- **The nearest groom** (0.9 m away, its bounds three quarters of the view's height) entered the first time at level
  1, because unseen it had settled with level 0 alone, and reached its own level, 2.7, in 0.9 s by ordinary promotions:
  for that long its other guides follow the 64 of level 0, which is the look of a lower simulation level and not a
  pop. When the camera turned away it stayed simulated for a quarter of a second, in which its level fell by half a
  level, and on the cut it entered at 2.2 with 434 guides going on from their own kept shape.
- **The captures** (the frame before the turn, each of its six frames, each of the next twelve, and three later ones;
  the simulation and the heads held still while each was written). With the kept shape the near groom is, in its
  first whole frame on screen, a head of hair hanging straight down at the sides with a ragged fringe below, and it is
  that in every later capture: the outline does not move from one to the next, and the sway shows only in the last
  ones. From the rigid pose its first frame is visibly wider at the bottom, the sides standing off the head as the
  groom was generated; over the next dozen frames they draw in, and at a third and two thirds of a second they have
  swung out again past where they end up: the settle, seen. In both runs the first frame after the turn is grainy
  along one side of the groom for that frame, which is the upscaler without history after a turn of 30 degrees and not
  the hair.

What remains visible: a groom that starts in view with no kept shape (attached in view, the simulation turned on, a
reset) settles in view as before; a groom whose head turned far while it was out of view starts from a shape that is
wrong by that turn; and a groom that enters has lost its motion, which with these gentle sways cannot be seen and
under a wind would show as hair that takes a moment to start moving.

### Guides in use

Point 2 of this step: how many of the guides the guide-level pass evaluates does a drawn strand depend on, directly or
through a guide evaluated from it? From a frame's selection and the groom data (`hair.dump`), with the automatic levels.
One groom alone in view at a distance, the same half out of the side of the view, and the crowd. "Share of its roots"
is drawn strands over the strands of the grooms that draw any: what root binding now writes over what an
instance-level skip would.

| Groom, view | Strands drawn | Share of its roots | Simulation level | Guides the solver owns | Guides evaluated | Evaluated guides a drawn strand depends on | Share of all guides in use |
|---|---:|---:|---:|---:|---:|---:|---:|
| crowd groom (10,000 strands, 1,024 guides), 0.8 m | 10,000 | 100% | 2.84 | 902 | 122 | 122 (100.0%) | 100.0% |
| crowd groom (10,000 strands, 1,024 guides), 1.6 m | 10,000 | 100% | 2.18 | 393 | 631 | 631 (100.0%) | 100.0% |
| crowd groom (10,000 strands, 1,024 guides), 3.2 m | 10,000 | 100% | 1.51 | 180 | 844 | 844 (100.0%) | 100.0% |
| crowd groom (10,000 strands, 1,024 guides), 6.4 m | 8,688 | 87% | 0.88 | 64 | 960 | 960 (100.0%) | 100.0% |
| crowd groom (10,000 strands, 1,024 guides), 12.8 m | 4,870 | 49% | 0.40 | 64 | 960 | 960 (100.0%) | 100.0% |
| crowd groom (10,000 strands, 1,024 guides), 25.6 m | 2,524 | 25% | off | | | | |
| crowd groom (10,000 strands, 1,024 guides), 0.8 m, half out of view | 3,986 | 40% | 1.82 | 227 | 797 | 379 (47.6%) | 49.2% |
| crowd groom (10,000 strands, 1,024 guides), 3.2 m, half out of view | 4,612 | 46% | 1.00 | 64 | 960 | 529 (55.1%) | 56.3% |
| 30,000 strands, 4,096 guides, 0.8 m | 30,000 | 100% | 3.75 | 3,335 | 761 | 761 (100.0%) | 100.0% |
| 30,000 strands, 4,096 guides, 1.6 m | 30,000 | 100% | 2.75 | 833 | 3,263 | 3,263 (100.0%) | 100.0% |
| 30,000 strands, 4,096 guides, 3.2 m | 29,550 | 98% | 1.75 | 218 | 3,878 | 3,878 (100.0%) | 100.0% |
| 30,000 strands, 4,096 guides, 6.4 m | 18,763 | 63% | 0.88 | 64 | 4,032 | 4,032 (100.0%) | 100.0% |
| 30,000 strands, 4,096 guides, 12.8 m | 9,712 | 32% | 0.39 | 64 | 4,032 | 4,032 (100.0%) | 100.0% |
| 30,000 strands, 4,096 guides, 25.6 m | 4,979 | 17% | off | | | | |
| 30,000 strands, 4,096 guides, 0.8 m, half out of view | 11,310 | 38% | 2.30 | 495 | 3,601 | 1,570 (43.6%) | 44.7% |
| 30,000 strands, 4,096 guides, 3.2 m, half out of view | 13,418 | 45% | 1.00 | 64 | 4,032 | 2,117 (52.5%) | 52.9% |
| 4,000 strands, 4,000 guides, 0.8 m | 4,000 | 100% | 3.74 | 3,246 | 754 | 754 (100.0%) | 100.0% |
| 4,000 strands, 4,000 guides, 1.6 m | 4,000 | 100% | 2.74 | 833 | 3,167 | 3,167 (100.0%) | 100.0% |
| 4,000 strands, 4,000 guides, 3.2 m | 4,000 | 100% | 1.74 | 218 | 3,782 | 3,782 (100.0%) | 100.0% |
| 4,000 strands, 4,000 guides, 6.4 m | 3,939 | 98% | 0.87 | 64 | 3,936 | 3,875 (98.5%) | 98.5% |
| 4,000 strands, 4,000 guides, 12.8 m | 2,527 | 63% | 0.39 | 64 | 3,936 | 2,463 (62.6%) | 63.2% |
| 4,000 strands, 4,000 guides, 25.6 m | 1,340 | 34% | off | | | | |
| 4,000 strands, 4,000 guides, 0.8 m, half out of view | 2,189 | 55% | 2.48 | 631 | 3,369 | 1,867 (55.4%) | 56.2% |
| 4,000 strands, 4,000 guides, 3.2 m, half out of view | 1,813 | 45% | 1.00 | 64 | 3,936 | 1,833 (46.6%) | 46.9% |
| crowd near | 293,838 | 42% | up to 1.80 | 2,006 | 24,618 | 22,698 (92.2%) | 92.3% |
| crowd far | 99,004 | 10% | off | | | | |
| crowd fly z=-4.9 | 226,509 | 51% | up to 1.60 | 1,720 | 21,832 | 21,805 (99.9%) | 99.9% |
| crowd fly z=-11.75 | 117,206 | 67% | up to 1.33 | 1,266 | 17,166 | 16,337 (95.2%) | 95.3% |
| crowd fly z=-18.6 | 29,439 | 67% | up to 1.25 | 391 | 3,705 | 2,585 (69.8%) | 70.4% |

- **A groom wholly in view uses every guide wherever it is simulated**, while its guides are a small share of its
  strands (a tenth for the crowd's grooms, a seventh for the 30,000-strand one). Guides are the lowest ranks, the
  strand LOD drops the highest, and a groom stops being simulated (coverage 0.03, between 18 and 25.6 m here) long
  before the LOD is down to its guides. Marking would save nothing there.
- **It would save on a groom with as many guides as strands:** the guides the LOD does not draw, from 2% at 6.4 m to
  53% at 18 m, all in the range where only level 0 is simulated. That is about 0.08 ms for 2,000 guides, on a kind of
  groom nothing here uses.
- **It would save about half on a groom half out of view.** The level is already lower there (the visible part's),
  and it is the solver's guides that the level drops, not the evaluated ones.
- **In the crowd** the evaluated guides in use are 92% from the near camera, 99.9% and 95% along the fly-through, and
  70% where four grooms straddle the edge of the view. On the near camera's 0.96 ms that would be 0.08 ms.

So per-guide marking was not built: the per-instance gate is what there was to gain. The same numbers are the case
for binding only the drawn roots: the drawn strands are 42% to 67% of the roots of the grooms in view from the crowd's
cameras, and 10% from the far one.

### What each pass's cost follows now

| Pass | Follows on-screen hair? | What its cost follows, as measured |
|---|---|---|
| `Hair::RootBinding` | Yes | The roots written: the drawn strands, plus every guide of the simulated grooms. About 0.4 ms per million, over 0.02 ms for a dispatch as wide as the clusters that could be visible. |
| `Hair::Simulate` | Yes, over a floor | The grooms in view that are near enough, and for a quarter of a second those that just left it. 0.6 to 0.7 ms from 4 simulated grooms to 26, 1.1 ms for 42 (turning fast): the floor of one thread's serial loop, as before. Nothing when no simulated groom is in view. |
| `Hair::GuideLevels` | Mostly | Every guide of those grooms that the solver does not own: about 0.04 ms per thousand (0.96 ms for 24,618, 0.21 ms for 3,701). A groom partly in view has all its guides evaluated. |
| `Hair::Select` | No, and it does not matter | Every cluster of every attached groom: 0.03 ms for 22,528, in 100 dispatches. |
| `Hair::Evaluate`, `Hair::Raster`, `Hair::Resolve` | As before | Unchanged by this step. |

What does not follow the view:

- **The simulate pass's floor**, 0.6 ms as soon as one groom with 33-point guides is simulated.
- **Visibility is the frustum's.** A groom behind a wall, or one of the crowd wholly behind others, is simulated and
  its roots are bound. There is no occlusion test in selection either.
- **The quarter second after leaving, and the climb after entering**, which is what a fast turn costs (above).
- **Settling unseen:** a groom that comes near out of view is simulated at level 0 alone for a second and a half,
  once. In the run above, turning the simulation on in the middle of the crowd simulated 95 grooms at first and the
  20 in view (and near enough) from a second and a half on.
- **The floors of the passes that draw**: 0.2 ms with no hair in view at all, and the select pass over every cluster.
- **Memory.** Root frame rows and guide state rows are still held for every instance (302 MB for the crowd). A groom
  out of view now needs neither its root frames nor, but for the shape it keeps, its guide state.

(Both were done next: [On-demand memory measurements](#on-demand-memory-measurements).) For whoever allocates those on demand: root frames are written afresh every frame for exactly the roots the frame
uses, so nothing in them needs keeping, and the three readers (simulate, guide levels, evaluate) could as well work a
root's frame out as the select pass now does. Guide state is live only while an instance is simulated; the one thing
kept across a stop is the shape (`simStateValid` on the CPU, `HAIR_SIM_FLAG_KEEP_SHAPE` on the GPU). Freeing an
instance's rows means clearing that flag, and the instance then starts from its rigid pose, or settles unseen again if
it is near.

**Scenarios.** All five pass with coverage on and with `HAIR_COVERAGE=off`. Four checks changed with this step:

- `hair_single.cs` expected two root-binding dispatches a frame (one per instance) and now expects one, and checks
  that a root frame is written for every drawn strand and for no other, none of them stale.
- `hair_single.cs` forces every strand to be drawn while the self-check runs: the check samples roots across each
  groom, and root frames now exist for drawn strands only. The self-check itself skips a sampled root that was not
  written.
- `hair_crowd.cs` also checks that facing away no root frame is written and no groom is simulated, and that in no
  frame is a strand drawn with a stale root frame or from a groom left unsimulated for being out of view.
- `hair_sim.cs` and `hair_sim_lod.cs` also check that their grooms were in view in every frame whose metrics they
  read. They were, in every phase, so both exercise the solver and the levels as before.

`hair_sim_lod.cs` failed once in ten runs on "dollying away, the level never rises"; the other nine passed, and the
cause was not found (on that dolly the level comes from the coverage alone, as before this step). The check now names
the two samples when it fails.

## On-demand memory measurements

Measured on 2026-11-27 for the third step after the proof of concept: per-instance hair memory follows what is in use.
Same machine, conditions and method as [View-following measurements](#view-following-measurements): raster mode, DLSS
at 2560x1600 in and out, the display awake, uncapped, times from the statistical profiler at a 3% half-width with the
camera held still (or turning at a steady rate, where said), `frame_seconds` 1/60, heads swaying 2 cm while simulated,
coverage on (`white`). "Before" is the tree as that section left it, measured again in the same session. The scripts
are not in the tree.

**What changed.**

1. *No root-frame buffer and no root-binding pass.* Every pass works out the root frames it needs
   ([Root frames](#root-frames)). `Hair.RootFrames` (1,048,576 rows, 96 MB, a row per strand of every instance) is gone.
2. *Guide state is lent* to the instances that are simulated or keep a shape, out of one buffer, and taken back by
   stated rules ([Guide rows](#guide-rows)). An instance used to own rows for every guide point from `hair.attach` on,
   and an instance that did not fit was refused.
3. *Capacities.* `Hair.GuidePoints` went from 4,194,304 rows to 2,097,152. The pooled buffers are 417.1 MB, from
   672.7 MB: 96 MB of root frames and 160 MB of guide state less, 0.4 MB of self-check buffers more.

### Memory against the view

[`hair_crowd.cs`](../Scripting/Scenarios/hair/hair_crowd.cs)'s crowd, 100 grooms of 1,024 guides, 1,004,000 strands.
Before, the per-instance state was 302.1 MB wherever the camera was and whether or not anything was simulated (91.9 MB
of root frames, 210.2 MB of guide state). Now it is the guide rows lent out, in MB:

| Camera | Simulation | Before | Now in use | of which simulated now | of which kept shapes | Grooms holding rows (simulated + kept) |
|---|---|---:|---:|---:|---:|---:|
| near: 2 m in front, looking down the grid | off | 302.1 | 0.0 | 0.0 | 0.0 | 0 |
| near: 2 m in front, looking down the grid | on | 302.1 | 99.9 | 52.4 | 47.6 | 26 + 22 |
| behind the last row, facing away | off | 302.1 | 0.0 | 0.0 | 0.0 | 0 |
| behind the last row, facing away | on | 302.1 | 88.1 | 0.0 | 88.1 | 0 + 42 |
| 52 m in front, the whole grid small | either | 302.1 | 0.0 | 0.0 | 0.0 | 0 |

Along the fly-through (the nine cameras of the earlier sections, each held still, simulation on), and turning in place
in the middle of the crowd (z = -11.25; ranges over six readings along the turn):

| Camera | In view | Simulated grooms | Guide rows simulated now (MB) | In use (MB) | Grooms holding rows (simulated + kept) | Unsettled |
|---|---:|---:|---:|---:|---:|---:|
| fly-through, z = 2.0 | 70 | 26 | 52.4 | 99.9 | 26 + 22 | 0 |
| z = -1.4 | 56 | 25 | 51.6 | 128.7 | 25 + 37 | 0 |
| z = -4.9 | 44 | 23 | 46.5 | 149.7 | 23 + 50 | 0 |
| z = -8.3 | 30 | 24 | 47.2 | 157.4 | 24 + 52 | 5 |
| z = -11.8 | 18 | 18 | 40.9 | 158.2 | 18 + 59 | 8 |
| z = -15.2 | 10 | 10 | 22.1 | 158.2 | 10 + 67 | 7 |
| z = -18.6 | 4 | 4 | 6.6 | 154.1 | 4 + 70 | 0 |
| z = -22.1 | 0 | 0 | 0.0 | 115.0 | 0 + 56 | 0 |
| z = -25.5 | 0 | 0 | 0.0 | 88.1 | 0 + 42 | 0 |
| in the middle, still | 18 to 19 | 22 | 47.6 | 158.9 | 78 | 17 |
| turning 0.5 degrees a frame | 20 to 24 | 21 to 24 | 43.1 to 48.3 | 156.7 to 158.2 | 75 to 77 | 0 to 4 |
| turning 2 degrees a frame | 19 to 24 | 26 to 30 | 56.8 to 63.8 | 156.7 to 158.2 | 75 to 77 | 0 |
| turning 6 degrees a frame | 21 to 22 | 37 to 40 | 79.6 to 83.0 | 156.7 to 158.2 | 75 to 77 | 0 |

With the simulation off no rows are lent at any camera. "Unsettled" is the grooms out of view that would settle unseen
and found no free rows.

- **The rows of grooms simulated now follow the view**: 52 MB for the 26 simulated from the near camera, 7 MB for four,
  nothing with none in view or none near enough, 83 MB turning at a full turn a second (40 simulated: the ones in view,
  the ones in their quarter second of grace, and the ones still climbing).
- **The rest of what is in use is kept shapes**, and that follows the grooms *near enough to simulate*, seen or not:
  88 MB facing away from the grid with 42 grooms near enough behind the camera, nothing from 52 m where none is. That
  is what [Following the view](#following-the-view) asks for, so that a turn finds settled hair, and it is the larger
  share: the kept shapes fill the buffer wherever more grooms are near than it holds. In the middle of the crowd 95
  grooms are near enough and the buffer holds 78 of them (159 MB of 160): 17 of the smallest are left unsettled, and
  enter from their rigid pose.
- **Capacity against the working set.** The most rows simulated at once over all of this is 1,087,416 (83 MB); the
  buffer is 1.9 times that. At the old size (320 MB) every groom's rows fitted, so every groom near enough kept a
  shape everywhere in this crowd; at this one the crowd's middle is over capacity by the 17. Halving it again
  (1,048,576 rows) would leave the fastest turn 4% short of rows for the grooms in view.
- **The other buffers** as the crowd used them at most, of their capacity: the strand pool 74.9 of 128 MB and its
  segment ids 9.4 of 16; the groom points 7.9 of 64; strands and root bindings 4.9 of 32; visible clusters 0.7 of 2;
  the rest 0.2 MB or less each, of 15 MB in all, 5 of it the probe buffer. `hair.stats` gives these per buffer.

**The same crowd built with the builder's default 4,096 guides.** A 33-point groom then needs 135,168 guide rows
(10.3 MB) and the straight one 38,992 (3.0 MB); the hundred would need 848 MB of guide state at once.
Before this step `hair.attach` would have refused the crowd after 37 grooms or so, at 320 MB. Now all 100 attach, and the buffer
admits the grooms it has rows for:

| Camera | In view | Simulated grooms | Starved (in view, rigid) | Guide rows in use (MB) | Grooms holding rows (simulated + kept) | Hair total (ms) |
|---|---:|---:|---:|---:|---:|---:|
| near, simulation off | 70 | 0 | 0 | 0.0 | 0 | 1.23 |
| near | 70 | 21 | 5 | 157.9 | 21 + 0 | 5.78 |
| facing away | 0 | 0 | 0 | 154.9 | 0 + 20 | 0.21 |
| far | 100 | 0 | 0 | 0.0 | 0 | 0.39 |
| fly-through, z = -4.9 | 44 | 20 | 3 | 157.9 | 20 + 1 | 5.75 |
| z = -11.8 | 18 | 16 | 2 | 143.0 | 16 + 0 | 4.87 |
| z = -15.2 | 10 | 10 | 0 | 146.0 | 10 + 7 | 3.70 |
| z = -18.6 | 4 | 4 | 0 | 146.0 | 4 + 13 | 1.75 |
| in the middle, still | 18 to 19 | 18 | 3 | 148.9 | 18 + 0 | 4.81 |
| turning 2 degrees a frame | 19 to 22 | 17 to 19 | 1 to 4 | 148.9 to 154.9 | 18 to 20 | 5.49 |
| turning 6 degrees a frame | 21 to 22 | 17 to 20 | 1 to 4 | 146.0 to 154.9 | 17 to 20 | 5.47 |

- **The buffer holds 17 to 21 such grooms**, and in this crowd more than that are in view and near enough from the
  first seven fly-through cameras and in the middle: 1 to 5 grooms, the smallest on screen that asked, follow rigidly,
  and are counted. From z = -15.2 on every groom in view is simulated and what is left holds kept shapes. The
  invariant counters were 0 at every station.
- **Turning over capacity is churn**: by the end of the 6-degrees-a-frame station 1,804 ranges had been taken back and
  874 times a groom had started to starve. Every groom that enters takes the rows of one that left, starts from its
  rigid pose, and no shape is ever kept. It is bounded (17 to 20 simulated, as when still) and nothing breaks, but
  the hair that is simulated is settling most of the time.
- **Hair costs 5.8 ms from the near camera, against 3.5 ms with 1,024 guides**, for about the same simulated guides
  (2,028 against 2,006): the guide-level pass evaluates 84,000 guides instead of 24,600. Those evaluated guides are
  also what the rows are spent on: 98% of the rows lent hold guides the solver does not own.

### Root frames worked out where they are read

The cost of the choice, against the root-binding pass of the step before. Only the four passes that touch root frames
are shown; the others did not change. Times in ms.

| Camera | Simulation | Roots the frame uses | Root binding, before | Evaluate, before | now | Simulate, before | now | Guide levels, before | now | The four, before | now |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| near | off | 293,838 | 0.13 | 0.52 | 0.48 | | | | | 0.65 | 0.48 |
| near | on | 320,462 | 0.16 | 1.25 | 1.19 | 0.63 | 0.63 | 0.96 | 0.92 | 3.00 | 2.75 |
| facing away | either | 0 | 0.02 | 0.02 | 0.02 | | | | | 0.04 | 0.02 |
| far | off | 99,004 | 0.06 | 0.17 | 0.15 | | | | | 0.23 | 0.15 |
| fly-through, z = -4.9 | on | 250,323 | 0.14 | 1.43 | 1.38 | 0.62 | 0.62 | 0.85 | 0.81 | 3.04 | 2.82 |
| fly-through, z = -11.8 | on | 135,640 | 0.10 | 0.94 | 0.92 | 0.63 | 0.63 | 0.74 | 0.72 | 2.41 | 2.26 |
| fly-through, z = -18.6 | on | 33,629 | 0.05 | 0.24 | 0.23 | 0.59 | 0.60 | 0.21 | 0.21 | 1.08 | 1.04 |

- **Working a root's frame out costs less than reading its row.** The pass is gone (0.02 to 0.16 ms), and each of its
  three readers got faster or stayed where it was: a rigid or bone-attached root's frame is two matrix products of
  data the thread already has, against a 96-byte row from a buffer of a million. The crowd's hair is 3.49 ms from the
  near camera with the simulation on, from 3.75 ms, and 1.22 ms with it off, from 1.39 ms.
- So the per-frame buffer sized by the roots used, the other candidate, was not built: it could only add a pass and a
  read back.
- **Not measured: roots that follow their own joints** (`per_root`). There a root's frame is a blend of up to four
  bone matrices, twice (this frame's and the previous), in each reader, where the pass did it once. No groom here
  binds that way ([Attachment](#attachment) says why).

**The invariant checks' cost.** The first build checked, in the guide-level pass, that each parent of an evaluated
guide carried this frame's stamp: three more row reads a guide. That alone cost 0.14 ms from the near camera
(`Hair::GuideLevels` 1.06 ms with it, 0.92 without, 0.96 before this step). It was replaced by the check on a guide's
own rows, which the two passes read anyway; the evaluate pass's check of the guides a drawn strand reads is in the
figures above.

### Turning in place: time

| Camera | Simulation | | In view | Simulated grooms | Root binding | Simulate | Guide levels | Evaluate | Hair total |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| still | on | before | 19 | 22 | 0.10 | 0.63 | 0.87 | 0.90 | **2.94** |
| still | on | now | 18 to 19 | 22 | none | 0.63 | 0.84 | 0.88 | **2.78** |
| still | off | before | 19 | 0 | 0.08 | 0.00 | 0.00 | 0.32 | **0.83** |
| still | off | now | 19 | 0 | none | 0.00 | 0.00 | 0.33 | **0.75** |
| turning 0.5 degrees a frame | on | before | 19 to 23 | 22 to 24 | 0.11 | 0.77 | 0.86 | 1.08 | **3.39** |
| turning 0.5 degrees a frame | on | now | 20 to 24 | 21 to 24 | none | 0.85 | 0.80 | 1.01 | **3.21** |
| 2 degrees a frame | on | before | 19 to 23 | 26 to 29 | 0.11 | 0.92 | 1.07 | 1.09 | **3.75** |
| 2 degrees a frame | on | now | 19 to 24 | 26 to 30 | none | 1.00 | 1.00 | 1.07 | **3.62** |
| 6 degrees a frame | on | before | 21 to 22 | 42 to 44 | 0.12 | 1.12 | 1.61 | 1.11 | **4.50** |
| 6 degrees a frame | on | now | 21 to 22 | 37 to 40 | none | 1.05 | 1.40 | 1.07 | **4.05** |

The middle of the crowd is where the buffer is full, so this is the lending under pressure: over the three turning
stations 23, 95 and 417 ranges were taken back for grooms entering the view (up to the last reading of each station;
at 6 degrees a frame about one a frame), all from the smallest grooms out of view,
and no groom in view went without rows. At 6 degrees a frame fewer grooms are simulated than before (37 to 40 against
42 to 44): a small groom that has just left the view is the first to lose its rows, and with them its quarter second
of grace.

**Taking the oldest shape first thrashed here.** That was the first build's rule, measured in the same way: turning
at 2 degrees a frame 47 to 48 grooms were simulated (26 to 29 before this step) and hair cost 4.84 ms (3.75); at 6
degrees a frame 73 to 74 grooms (42 to 44), 6.43 ms (4.50), with 1,468 evictions by the end of that station.
[Guide rows](#guide-rows) says why: in a turn the oldest shape is the next groom the camera comes to.

### Rows taken back, and what an entry then looks like

The scripted turn test of [Entering and leaving the view](#entering-and-leaving-the-view), again: the crowd with
strands four times as wide, the camera in its middle 0.9 m from one head, a fixed 1/60 s frame, turns of 180 degrees in
six frames and back, every frame's counters, every instance's metrics and the lending of guide rows recorded (3,272
frames). Four regimes in turn:

| Regime | Budget (rows) | Rows in use (MB) | Grooms holding rows | Entries from a kept shape / from the rigid pose, per turn | Evictions in the turn | Starved in view |
|---|---:|---:|---:|---:|---:|---:|
| defaults (the buffer, full) | 2,097,152 | 157 to 159 | 76 to 78 | 35 / 11 | 9 | 0 |
| budget just above the grooms in view | 571,696 | 41 to 43 | 20 to 21 | 0 / 44 | 36 | 0 to 1 |
| `guide_keep_seconds` 2, after 4 s | 2,097,152 | 41 at the start of the turn | 20 | 0 / 44 | 0 (75 by age before it) | 0 |
| budget far below the grooms in view | 250,000 | 18 | 10 | 0 / 23 | 18 | 8 to 11 |

- **The invariants stayed 0 in all 3,272 frames**: no drawn strand read guide rows that were not written that frame
  for its groom (`stale_guides_drawn`), no guide went on from rows that were not its own of the frame before
  (`stale_guides`), no strand was drawn of a groom left out for being out of view (`unseen_strands_drawn`).
- **Rows are reused.** Of 656 ranges lent in the run, 555 lay over rows another groom had held before. The most rows
  ever lent at once was 2,087,712 of 2,097,152.
- **A groom that kept its shape enters as before this step; one that lost it settles in view.** For the grooms that
  came into view and stayed, the mean distance of a groom's guides from their rigid pose (medians over the grooms, the
  largest in brackets):

  | Entry | Grooms | At entry | 1.5 s later | Largest change in the first second | Its range over the next 2 s | Largest motion of a guide in a frame, first half second | Next 2 s | Peak of the guides' mean speed, first half second | Next 2 s |
  |---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
  | kept shape (defaults) | 15 | 17.06 mm | 17.11 mm | 0.35 mm (0.58) | 0.24 mm (0.69) | 1.7 mm (26.1) | 5.4 mm (30.0) | 32 mm/s (87) | 107 mm/s (120) |
  | rigid: never settled, the buffer was full (defaults) | 4 | 0.00 mm | 18.13 mm | 23.90 mm (25.08) | 2.74 mm (2.82) | 10.5 mm (11.2) | 11.6 mm (17.7) | 286 mm/s (294) | 112 mm/s (120) |
  | rigid: evicted by the budget | 19 | 0.00 mm | 16.98 mm | 22.72 mm (25.08) | 2.64 mm (2.85) | 9.8 mm (32.0) | 6.3 mm (29.1) | 278 mm/s (296) | 108 mm/s (117) |
  | rigid: evicted by age | 19 | 0.00 mm | 16.98 mm | 22.67 mm (25.14) | 2.60 mm (2.70) | 12.3 mm (22.6) | 5.9 mm (29.3) | 289 mm/s (318) | 99 mm/s (100) |

  An evicted groom's entry is the step before's "rigid pose" comparison to the millimetre: it travels 23 mm in its
  first second, at three times the speed of the sway, where a kept shape moves a third of a millimetre. Eviction costs
  exactly the seamless entry, for that groom, once.
- **At the defaults the entries from the rigid pose are the smallest grooms.** The four that stayed in view had a
  coverage of 0.05 (the groom's bounds are 80 pixels tall, and its settle a few pixels); the nearest groom,
  coverage 0.75, entered from its kept shape. Before this step all 44 entered from kept shapes, with a buffer twice
  the size.
- **The captures** of the turn under the small budget (the frame the turn ends and the twelve after it, the nearest
  groom): in its first frame the hair stands off the head as the groom was generated, wide at the bottom; over the
  next dozen frames it draws in. With the kept shape the same frames show the outline it has in every later one.

### More grooms in view than rows

With a budget of 250,000 rows (the last regime above) 19 to 22 grooms in view ask to be simulated and 10 get rows.

- The 10 are the largest on screen at the moment the rows were handed out; the others follow their roots rigidly,
  `starved` in their metrics, 8 to 11 of them at any time (`starved_instances`), and ask again every frame.
  `allocation_failures` counted 10 when the budget was set, 21 more over a turn of 180 degrees and 78 over half a
  spin: one each time a groom starts to starve.
- Turning, a groom that enters takes the rows of one that left (18 evictions in six frames) or starves; a groom in
  view never loses its rows to another. Spinning at 2 degrees a frame 10 to 13 are simulated and 6 to 15 starved.
- The counters stay 0 throughout: a starved groom is drawn exactly as with the simulation off, and reads no guide row.
- With the budget lifted every starved groom has rows within a frame and starts from its rigid pose, in view.
- The same happens at the buffer's own size with no budget: every guide of every groom forced (`lod_force_level` 8)
  asks for 2,754,576 rows of 2,097,152; in the debug-layer session 79 grooms were simulated and 21 starved.

### What still does not follow the view

| | Size | Why |
|---|---:|---|
| The buffers themselves | 417 MB | Fixed capacities, never resized: memory in use follows the view, memory held does not. |
| Kept shapes | up to the guide buffer, 160 MB | Held for every groom near enough to simulate, seen or not, until the rows are needed. By design ([Guide rows](#guide-rows)); `guide_keep_seconds` trades them for visible settles. |
| A simulated groom's whole guide set | 2.6 MB a groom of 1,024 guides, 10.3 MB of 4,096 | Rows are lent per instance, for every guide. A groom at level 1 simulates 64 guides and holds rows for all of them: the others' rows are evaluated state, rewritten every frame, kept for motion vectors and for promotion. |
| Grooms hidden behind something | theirs | Visibility is the frustum's: an occluded groom is simulated and holds rows. |
| The strand pool | 144 MB with its segment ids | Sized for the most a frame draws (75 MB used by this crowd at most). |
| Groom assets | 13 MB for the crowd's four, in 102 MB of asset buffers | Loaded whole and kept; no streaming. |
| The visibility and coverage targets | 31 MB and 16 MB at 2560x1600 | At the render resolution whether or not hair is on screen. |
| The probe buffer | 5 MB | For `hair.probe`, allocated with the rest. |

### Checks

**A bug the counters found.** In the debug-layer session, lowering the budget below the rows of the grooms in view
gave `stale_guides_drawn` 97 in one frame. A groom in view lost its rows to the budget and was lent other rows in the
same frame; it had been simulated the frame before, so it was not started again, and went on from rows that held
another groom's state. New rows now always start the instance again. The session was run again and stayed at 0.

**Scenarios.** All five (seven runs: the sweep and the crowd with the simulation on and off) pass with coverage on and
with `HAIR_COVERAGE=off`. Checks that changed with this step:

- `hair_single.cs`: the dispatch it counts on the GPU is the evaluate pass's (there is no root-binding pass). The
  checks that a root frame is written for every drawn strand and no other, none stale, are gone with the buffer; in
  their place, strands are drawn, no drawn strand read guide rows it was not lent, and with the simulation off no
  instance holds guide rows. It no longer forces every strand to be drawn for the self-check, which now asks for its
  own roots.
- `hair_crowd.cs`: "no root frame is written facing away" became "no guide rows are in active use facing away", and
  the stale-root check became the stale-guide one. "Root frames were written for at least the drawn strands" is gone.
  Added: from the far camera no groom holds guide rows; every groom in view got the rows it asked for (no failure, none
  starved); rows were lent only while simulating and within the buffer; no guide went on from rows that were not its
  own. The scene's strand count is read from `instance_strands`.
- `hair_sim.cs` and `hair_sim_lod.cs`: added, no guide went on from rows that were not its own of the frame before,
  and the groom never went without rows. Both read a groom's guide row count from `instance_list`.
- `hair_sim_lod.cs` reads each groom's first guide row from `hair.stats` before it parses a dump of the guide state
  (an instance's rows used to start where the one before ended), and requires the rows not to move during a swing.
- `hair_lod_sweep.cs`: unchanged.

`hair_sim_lod.cs`'s intermittent failure of the step before ("dollying away, the level never rises", 1 run in 10) did
not recur in 5 runs here, so there is still no sample pair to show.

**Debug layer.** A control-pipe session under the D3D12 debug layer: the crowd simulated; most of it put out of view
and back by narrowing the field of view (`camera.set`), six times in quick succession; budgets of 700,000 and 250,000
rows with the view changed under each; six far grooms forced on and off; the budget lifted; an age limit of 0.5 s;
the self-check (100 instances, 800 roots, positions equal, directions to 7e-8); dumps; a frozen LOD; a reset; every
guide of every groom forced; the simulation off; `hair.clear`. 288 ranges lent, 147 given back, 141 taken back, 67
failures; the invariant counters 0 throughout. The validation messages are those of a run with no hair (the same
ids), and none names a hair resource. The stage script that moves the camera is stopped by the layer's error 1047
within a few frames (the renderer's packed buffers, as before), which is why the view was changed by field of view.

## Layered path measurements

Measured on 2026-11-27 for the first step of the second stage: the [layered path](#the-layered-path) (three layers and
the summed coverage a pixel, a lit colour per strand vertex, a composite over the lit frame with hair's depth, motion
and a reactive mask) against the surface path with stochastic coverage, which is the "on" of
[Partial coverage measurements](#partial-coverage-measurements). Same machine, conditions and method as
[POC measurements](#poc-measurements): raster mode, DLSS at 2560x1600 in and out, the display awake, uncapped, times
from the statistical profiler at a 3% half-width. Counters are means over 32 consecutive frames. Image differences are
of 8-bit luma of presented frames, after the upscaler. The grooms are those of the partial coverage measurements: the
distance series' procedural groom (30,000 strands of 88 to 44 micrometres: the sparse one), `wCurly` and `wStraight`
(50,000 strands each: the dense ones), the thin dolly groom, and the crowd. The scripts are not in the tree.

**One thing found about the earlier measurements.** The near view's square (0.8 m) reaches under the demo's menu
panels, whose numbers change every frame. Their pixels were in the temporal noise and fine contrast of
[Partial coverage measurements](#partial-coverage-measurements) for that distance: with them in, this session's near
view reads a 99th percentile of 17 to 28 levels whatever the path, and without them 3 to 7 on the surface path. Here
those pixels are left out, so the near row is not comparable with that table's; the middle and far rows, clear of the
menu, are, and the surface path is measured again in the same session throughout.

### Does it remove the grain and the veil

Yes to both.

| Groom, distance | Temporal noise: surface | Layered | Layered, no mask | 99th percentile pixel: surface | Layered | Layered, no mask | Fine contrast still: surface | Layered | Swaying: surface | Layered | Layered, no mask |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Procedural, 0.8 m | 1.28 | 0.09 | 0.09 | 7.2 | 0.6 | 0.6 | 18.7 | 12.9 | 11.5 | 6.5 | 5.9 |
| Procedural, 3.2 m | 1.22 | 0.07 | 0.06 | 7.2 | 0.4 | 0.4 | 14.9 | 8.7 | 12.4 | 6.9 | 7.0 |
| Procedural, 12.8 m | 1.19 | 0.08 | 0.06 | 6.5 | 0.5 | 0.4 | 21.3 | 18.0 | 19.0 | 16.7 | 17.0 |
| `wCurly`, 0.8 m | 0.42 | 0.07 | 0.06 | 3.2 | 0.5 | 0.5 | 22.9 | 18.4 | 10.9 | 9.9 | 10.8 |
| `wCurly`, 3.2 m | 0.54 | 0.06 | 0.05 | 4.2 | 0.4 | 0.4 | 13.5 | 12.1 | 11.8 | 10.0 | 10.3 |
| `wCurly`, 12.8 m | 0.54 | 0.11 | 0.07 | 3.9 | 0.7 | 0.5 | 22.3 | 19.4 | 20.8 | 18.3 | 18.4 |

"Temporal noise" is the per-pixel standard deviation of luma over 8 captures 3 frames apart with everything still,
averaged over the square around the groom; "fine contrast" is the standard deviation of a Laplacian, still and with the
head swaying 2 cm to each side every two seconds. "No mask" is the layered path with `reactive` off.

- **Still camera: the grain is gone.** The layered path's temporal noise is 0.06 to 0.11 levels in every view, a
  fifteenth of the surface path's on the sparse groom and a fifth to a ninth on the dense one, with the worst hundredth
  of the pixels at half a level where they were at 3 to 7. That is below the opaque form's own (0.13 to 0.19 in the middle and
  far views of the earlier table): nothing in a hair pixel changes from frame to frame but the camera's jitter. In the
  captures the translucent parts (the fringe, the scalp through thin hair) are smooth where the surface path's are
  dotted, and single strands of the fringe are continuous thin lines where they were rows of dots.
- **Camera motion: the veil is gone.** The slide of the earlier measurement (0.6 m a second past the sparse groom, then
  a stop). While the camera moves, the surface path's fringe is the streaky, grainy veil described there; the layered
  path's is soft strands over an undisturbed background, in the captures looked at. Two frames
  after the stop the hair region is 2.95 levels from the converged image against 6.77 for the surface path,
  and two converged captures 30 frames apart differ by 0.13 against 2.63.
- **What it costs in the image: less fine contrast, most of all on sparse hair in motion.** Still, the sparse groom's
  fine contrast is 15% to 42% under the surface path's and the dense one's 10% to 20%; with the head swaying, 43% under
  at 0.8 and 3.2 m for the sparse groom and 9% to 15% for the dense one. Part of what is lost is the surface path's
  grain, which is fine contrast too; part is real: a strand a third of a pixel wide is blended at a third of its
  contrast where the surface path draws it in full in a third of the frames. Over the lower part of the groom in the
  frame captured during the slide the fine contrast is 12.8 against 17.9, and two frames after the stop
  12.0 against a converged 18.1: the hair sharpens over the following frames, as on the surface path
  (18.6 against 23.9). A pixel that is part hair, part background has one motion vector, and whichever it
  is, the upscaler reprojects the other part wrongly.
- **The reactive mask** is small and local (with the head swaying: 12% of the square's pixels above 0.1, none above
  0.5, mean 0.04). With DLSS it makes the moving hair sharper during the slide (fine contrast 12.8 with it,
  9.8 without) and costs a little after the stop (2.95 levels from converged with it, 2.86 without).
  With the head swaying, and with everything still (where it is all zero), it measures the same on and off. It is on by
  default. Whether DLSS takes the hint was checked by forcing the mask to 1 on every hair pixel: DLSS's still image does not change at all (temporal noise 0.059 with, 0.061 without, twice over), so it does not simply drop its history where it is told to, while FSR's does (1.6 against 0.14). What DLSS does with the hint is its own; the difference under camera motion above held in two runs (11.8 against 9.5 in the other).
- **No trail, second copy, halo or popping** was seen in the captures looked at: hair over the white floor and over the
  sky has no dark or bright rim, and the swaying captures show no strand standing apart from its neighbours. This is
  from looking at stills; nothing measures popping where two strands swap layers, and with three layers a pixel under
  many strands changes its three with every small move (the remainder term is what keeps that from showing: below).

### One strand leaving, and the dolly

The two continuity measurements of [Partial coverage measurements](#partial-coverage-measurements), on the layered
path.

- **One strand leaving** (1,000 strands 0.28 pixels wide, the forced detail level swept from 0.9 to 0.5 in 160 steps, the
  change of each 8x8 block from the step before). Layered: the largest block change in a step is 1.3 levels at the
  median, 4.4 at the 90th percentile and 5.2 at most, against 1.2 (2.2 at most) for the same state captured twice;
  0.36 blocks a step change by more than 4 levels. The surface path in the same session: 2.3, 4.4 and 4.5, against 2.1
  (4.4 at most), and 0.18 blocks. The true area stays 34,246 to 34,614 pixels over the sweep. A leaving strand cannot
  be found on either; the layered path's steps are quieter because its still image is.
- **The dolly** (the thin groom, 2 m to 80 m, a capture at every second step), live and frozen:

| LOD | Path | Strands, 2 m to 80 m | Mean difference 2-8 m | 8-20 m | 20-44 m | 44-80 m | Pairs over 1.25x | Largest ratio | Same camera twice: 2 m, 10 m, 21 m, 80 m |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| live | layered | 24,000 to 4,726 | 0.8007 | 0.0546 | 0.0130 | 0.0076 | 11 | 1.61 | 0.055, 0.015, 0.005, 0.006 |
| frozen | layered | 24,000 to 24,000 | 0.8007 | 0.0558 | 0.0136 | 0.0079 | 5 | 1.50 | 0.043, 0.013, 0.006, 0.006 |
| live | surface, coverage on (the earlier table) | 24,000 to 4,726 | 0.8983 | 0.0644 | 0.0145 | 0.0081 | 2 | 1.50 | 0.525, 0.034, 0.011, 0.007 |

  Live and frozen agree to 2% in the first two bands and 4% beyond, and no outlying pair is shared between them. The
  outliers are all beyond 24 m, where a pair differs by about a hundredth of a level, and have no correlation with the
  strands dropped or the clusters changing vertex level (0.04 and -0.05). There are more of them than on the surface
  path because the level they stand out from is lower: two captures of one view differ by 0.055 levels at 2 m where
  the surface path's differ by 0.525.

### Motion vectors

`hair.probe` on the layered path (`Hair::CompositeProbe`), as before: a groom moving (0.01, 0.0025, 0.005) m a frame
under a still camera, and a still groom under a camera sliding 0.01 m a frame and turning.

| | Probed hair pixels | Took hair's depth and motion | Expected motion, pixels | Error at those: mean, 99th percentile, largest | The others: stored motion against hair's |
|---|---:|---:|---|---|---|
| Moving groom | 30,228 | 23,563 | 4.1 to 5.6 | 0.0023, 0.0054, 0.0059 | 6,665 pixels, 4.9 pixels from hair's on average | |
| Moving camera | 56,239 | 46,827 | 3.2 to 6.9 | 0.0025, 0.0050, 0.0055 | 9,412 pixels, 4.4 pixels from hair's on average (8.8 at most) | |

At the pixels that took them, the motion vector is the nearest layer's to the precision of the target, and the
projected depth is `-M22 + M32 / depth` of the nearest key's depth to 6e-9. The pixels below the threshold hold the
motion vector and depth of what is behind the hair: with the groom moving that is a still floor, 5 pixels a frame away
from the hair's own motion, and it is those pixels the reactive mask is for. No pixel with an alpha under 0.498 took
hair's (the alpha is read back from an 8-bit target).

### Layers

| View | Hair pixels | One layer | Two | Three | Three with a remainder | Hair alpha, summed | Of it from the remainder | Pixels taking hair's depth |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Procedural, 0.8 m | 498,778 | 6.9% | 4.9% | 88.2% | 84.0% | 365,140 | 49% | 78.7% |
| Procedural, 3.2 m | 30,901 | 4.3% | 2.6% | 93.0% | 91.1% | 21,366 | 86% | 75.3% |
| Procedural, 12.8 m | 1,974 | 4.0% | 2.3% | 93.7% | 91.6% | 1,324 | 90% | 73.5% |
| `wCurly`, 0.8 m | 351,439 | 3.6% | 2.2% | 94.2% | 92.4% | 346,667 | 2% | 100.0% |
| `wCurly`, 3.2 m | 21,192 | 2.9% | 1.6% | 95.6% | 94.6% | 19,948 | 56% | 94.3% |
| `wCurly`, 12.8 m | 1,329 | 2.8% | 1.4% | 95.8% | 94.8% | 1,233 | 68% | 92.9% |
| Crowd, near camera | 259,121 | 6.0% | 4.2% | 89.7% | 86.1% | 118,939 | 75% | 40.9% |
| Crowd, far camera | 7,142 | 5.6% | 4.4% | 89.9% | 86.1% | 2,607 | 89% | 30.6% |
| Crowd, column view | 353,588 | 9.6% | 7.9% | 82.5% | 75.0% | 163,983 | 57% | 41.9% |
| Crowd, side view | 378,558 | 10.9% | 9.4% | 79.7% | 70.8% | 174,221 | 54% | 41.1% |

- **Nearly every hair pixel holds three layers and more hair behind them.** The grooms are thin strands many deep: at
  12.8 m the sparse groom puts 87 footprints over each of its pixels. One or two layers are the outline and the tips.
- **What the remainder contributes follows how wide the strands are on screen.** Close to `wCurly`, whose strands are a
  pixel wide or more, the three layers are opaque and the remainder is 2% of the hair's alpha. Where strands are a
  fraction of a pixel wide it is half of it (the sparse groom at 0.8 m) to nine tenths (at 12.8 m, and the crowd from
  far): there the three nearest strands are a sample of the hair's colour, and its opacity comes from the coverage sum.
  Without the term the same views are see-through: the sparse groom keeps half of its alpha at 0.8 m (186,228 of 365,140) and 14% at 3.2 m (2,918 of 21,366), and `wCurly` 44% at 3.2 m (8,799 of 20,069): a faint haze over a bare head.
- **The summed alpha agrees with the surface path's expected area.** The sparse groom at 0.8 m: 365,107 against a mean
  of 369,400 pixels resolved with stochastic coverage; `wCurly` at 12.8 m: 1,233 against 1,232. Front-to-back blending
  of what random acceptance draws from is what the exponential models.
- **Depth.** With the threshold at 0.5, three quarters of the sparse groom's hair pixels take hair's depth and motion,
  over nine tenths of the dense one's, and 31% to 42% of the crowd's thin hair.

### Cost

`ms` per pass, the surface path (stochastic coverage, no coverage target) against the layered path. "Hair passes" is
every hair pass's time summed (simulation off). The deferred pass is listed because the surface path's hair is lit in
it and the layered path's is not.

| View | Surface: raster | Resolve | Deferred | Hair passes | Frame | Layered: raster | Shade | Composite | Deferred | Hair passes | Frame | Vertices lit, of drawn |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Procedural, 0.4 m | 0.99 | 0.73 | 9.81 | 2.08 | 18.6 | 3.37 | 1.38 | 1.00 | 7.89 | 6.12 | 20.7 | 454,796 of 990,000 |
| Procedural, 0.8 m | 0.52 | 0.23 | 7.49 | 1.11 | 14.9 | 1.43 | 1.31 | 0.27 | 6.85 | 3.38 | 16.5 | 440,447 of 990,000 |
| Procedural, 1.6 m | 0.32 | 0.09 | 6.77 | 0.79 | 14.1 | 0.66 | 1.17 | 0.10 | 6.62 | 2.31 | 15.1 | 353,063 of 990,000 |
| Procedural, 3.2 m | 0.23 | 0.05 | 6.62 | 0.65 | 13.5 | 0.38 | 1.09 | 0.06 | 6.29 | 1.91 | 14.4 | 339,257 of 975,150 |
| Procedural, 6.4 m | 0.12 | 0.05 | 6.45 | 0.30 | 13.0 | 0.21 | 0.39 | 0.06 | 6.43 | 0.78 | 13.3 | 107,978 of 318,971 |
| Procedural, 25.6 m | 0.07 | 0.05 | 6.70 | 0.16 | 13.0 | 0.13 | 0.08 | 0.05 | 6.68 | 0.30 | 13.1 | 13,745 of 24,895 |
| Procedural, 100.0 m | 0.07 | 0.05 | 6.62 | 0.15 | 12.9 | 0.13 | 0.04 | 0.05 | 6.65 | 0.25 | 13.1 | 1,722 of 2,884 |
| `wStraight`, 0.4 m | 4.54 | 0.53 | 9.01 | 5.25 | 20.9 | 12.34 | 0.68 | 0.38 | 6.87 | 13.60 | 27.0 | 279,742 of 608,992 |
| `wStraight`, 0.8 m | 1.19 | 0.15 | 6.67 | 1.53 | 14.4 | 2.75 | 0.64 | 0.12 | 6.36 | 3.70 | 16.4 | 226,411 of 608,992 |
| `wStraight`, 1.6 m | 0.60 | 0.06 | 6.42 | 0.86 | 13.4 | 1.12 | 0.58 | 0.07 | 6.38 | 1.96 | 14.5 | 222,068 of 608,992 |
| `wStraight`, 3.2 m | 0.34 | 0.05 | 6.37 | 0.58 | 13.1 | 0.50 | 0.50 | 0.06 | 6.36 | 1.25 | 13.8 | 130,262 of 584,382 |
| `wStraight`, 6.4 m | 0.16 | 0.05 | 6.37 | 0.32 | 12.8 | 0.25 | 0.36 | 0.05 | 6.37 | 0.76 | 13.3 | 73,134 of 328,447 |
| `wStraight`, 25.6 m | 0.08 | 0.05 | 6.72 | 0.17 | 13.1 | 0.15 | 0.11 | 0.05 | 6.76 | 0.36 | 13.3 | 22,992 of 52,708 |
| `wStraight`, 100.0 m | 0.06 | 0.05 | 6.67 | 0.15 | 13.0 | 0.13 | 0.04 | 0.05 | 6.66 | 0.25 | 13.0 | 3,344 of 5,739 |
| Crowd, near camera | 0.59 | 0.13 | 5.55 | 1.27 | 12.8 | 1.04 | 2.83 | 0.13 | 5.42 | 4.56 | 16.1 | 1,085,337 of 2,382,030 |
| Crowd, far camera | 0.15 | 0.05 | 7.42 | 0.39 | 14.3 | 0.22 | 0.46 | 0.06 | 7.38 | 0.92 | 14.6 | 122,561 of 245,212 |

- **A frame costs 0.2 to 6 ms more.** Far away the difference is 0.2 to 0.3 ms (the composite's full-screen floor,
  0.05 ms, the rasterizer's two more clears and the shade pass). With a groom filling the view it is 2.1 ms for the
  sparse one and 6.1 ms for `wStraight`, nearly all of it the rasterizer; for the crowd from the near camera 3.2 ms,
  most of it shading. Those are after what the deferred pass gives back, 0.1 to 2 ms: it no longer lights hair pixels.
- **The composite is cheap:** 0.05 ms with no hair on screen, 0.13 ms for the crowd, 1.0 ms with 1.9 million hair
  pixels.
- **Shading is the largest pass wherever hair is not filling the view,** and it does not follow the hair on screen: it
  follows the vertices of the segments that reached a layer anywhere, 2.4 to 3 ns each (1.4 ms for the sparse groom's
  455,000, 2.8 ms for the crowd's 1.1 million), each with the sun's soft shadow lookup. The rasterizer's marks cut the
  lit vertices to under half of the drawn ones, but the pass's time by a quarter only (the sparse groom at 0.8 m: 1.79 ms
  for all 990,000, 1.31 ms for 440,000; the crowd: 3.93 ms for 2.38 million, 2.83 ms for 1.09 million): the marked rows
  are scattered through the pool, so most thread waves still run with some lanes idle. Compacting the marked rows into
  a list before shading is what would make the time follow them; it was not built.

**The rasterizer, and where the coverage target stands.** The earlier measurement had the coverage target at 2.1 times
the rasterizer's cost with `wStraight` filling the view. The layered path needs that target always, and three layers:

| View | Surface | Surface + coverage target | Layered | Layered, saturation 8 | Layered, no early reject | Footprint pixels | Rejected at the third layer | Adds skipped at saturation 8 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Procedural, 0.4 m | 0.99 | 1.56 | 3.37 | 3.73 | 4.29 | 15,358,581 | 6,408,454 (42%) | 760,766 (5%) |
| Procedural, 0.8 m | 0.52 | 0.66 | 1.43 | 1.71 | 1.81 | 8,522,732 | 4,975,414 (58%) | 370,958 (4%) |
| Procedural, 3.2 m | 0.23 | 0.25 | 0.38 | 0.47 |  | 2,097,334 | 1,543,399 (74%) | 91,782 (4%) |
| `wStraight`, 0.4 m | 4.54 | 9.72 | 12.34 | 9.58 | 25.77 | 65,138,015 | 47,638,102 (73%) | 51,341,451 (79%) |
| `wStraight`, 0.8 m | 1.19 | 2.00 | 2.75 | 2.72 | 4.67 | 22,745,668 | 17,914,402 (79%) | 18,222,130 (80%) |
| `wStraight`, 3.2 m | 0.34 | 0.39 | 0.50 | 0.51 |  | 5,474,388 | 4,965,617 (91%) | 4,403,586 (80%) |
| Crowd, near camera | 0.59 | 0.65 | 1.04 | 1.20 |  | 6,474,283 | 4,025,400 (62%) | 27 (0%) |
| Crowd, far camera | 0.15 | 0.15 | 0.22 | 0.22 |  | 236,325 | 127,366 (54%) | 0 (0%) |

- **The layered rasterizer is 2.7 times the surface path's on that view** (12.3 ms against 4.5), and 3.4 times with
  the sparse groom filling the view (3.4 against 1.0). Per footprint pixel it reads the opaque depth and the third
  layer, adds to the coverage target, and for the pixels the early reject lets through runs up to three atomic
  minimums, where the surface path reads the opaque depth and runs one.
- **The early reject does its part:** 42% to 91% of the footprint pixels stop at the third layer, the more the denser
  the hair. Without it the pass is 25.8 ms against 12.3 on that view and 4.7 against 2.8 at 0.8 m, and 4.3 against 3.4 with the sparse groom filling the view: it halves the dense close-up, which is what the Indiana Jones deck reports for it (up to 50%).
- **The coverage add is what is left, and nothing here removes it.** Every footprint pixel in front of the opaque
  surface still adds, hidden or not, and on the dense close-up that is 65 million atomics a frame: the 5.2 ms it
  measured on the surface path is in the layered path's time too. `coverage_saturation` 8 stops the adds of a pixel that
  already holds 8 pixels' worth: it takes `wStraight` at 0.4 m from 12.3 to 9.6 ms (2.1 times the surface path's, where
  the earlier measurement stood), does nothing at 0.8 m, and costs the sparse groom and the crowd 10% to 25% more, for a
  load of the target that seldom finds it saturated. It is off by default. The pass is bound by its scattered memory
  traffic: a plain load of the coverage texel costs about what the add it replaces does.

**Memory.** The pooled buffers are 449 MB (417 before): `Hair.PoolColors` adds 32 MB, 8 bytes a pool row. The layered
path's four textures are 74 MB at 2560x1600 (two layers of 31 MB, 8 MB of composite information and 4 MB of reactive
mask), made the first time the path is used, beside the 47 MB of the visibility and coverage targets that both paths
have.

### The two caveats

- **Lights.** The demo's recipe has no `ClusterLodAlphaTechnique`, so its light grid has no forward runs. With
  `lights` `grid` the sparse groom at 0.8 m is lit only where a froxel also holds an opaque pixel at about the hair's
  depth: over the head it is lit, and its sides and fringe, in front of the sky and the floor, are unlit in blocks the
  size of a grid tile. `list`, the default, lights it everywhere. No core change was
  made: the smallest correct lookup is the active light list. Its cost is a range test per light per vertex, with two
  lights in these scenes; a scene with many lights in a recipe that builds the forward runs would want `grid`.
- **Shadow pages.** A lookup that finds no page tries two coarser clipmaps and then returns lit; it never returns a
  shadow that is not there. In every view measured here, including hair over the sky and the crowd from every camera,
  no lookup of the sun's shadow read a coarser clipmap, found no page or read an undrawn texel (`shadow_coarser`,
  `shadow_missing`, `shadow_cleared` all 0, of 42,000 to 1.1 million lookups a frame). Hair on a head is within a page
  of the head's own pixels, and the floor's. Hair far from any opaque surface the camera sees (a long strand held out
  against the sky, a groom with no head in view) would be lit where it should be shadowed; the counters will say when.

### The rest of the frame

The crowd from just in front of its first row, looking down a column: hair in front of other heads, behind a head and
in front of the sky. One setting at a time is turned off and on, and the change of the image taken over
the pixels that hold hair and over the rest:

| Setting | Surface path: hair pixels | The rest | Layered path: hair pixels | The rest |
|---|---:|---:|---:|---:|
| Ambient occlusion (`enableGTAO`) | 5.75 | 0.05 | 0.34 | 0.04 | |
| Screen-space reflections (`enableScreenSpaceReflections`) | 4.98 | 0.07 | 0.38 | 0.05 | |
| Bloom (`enableBloom`) | 7.76 | 9.92 | 5.90 | 9.93 | |

- **Ambient occlusion and reflections do not see hair on the layered path.** On the surface path both change hair
  pixels by 5 to 6 levels: hair is in the depth and normal targets, takes ambient occlusion and the specular
  image-based light, and occludes the scalp. On the layered path they change hair pixels by a third of a level, which
  is the scalp and floor showing through. Hair there has no ambient occlusion and no specular environment, and the
  scalp under it has none from the hair. This is part of why the layered groom looks flatter and lighter.
- **Screen-space reflections** read the hardware depth and the surface targets, so hair is in no reflection on either
  path. Turning them on rebuilds the Shading epoch's graph alone; hair is still composited after it.
- **Bloom, exposure and tonemapping** run after the composite and treat hair as part of the scene colour on both.
- **Depth readers after the composite** (motion vector dilation, the upscaler, the lens focus and flare passes) see
  hair where it took the depth, and what is behind it elsewhere. There is no depth of field pass in the raster
  pipeline.
- **Other transparents.** The demo's recipe has none. Where a recipe has CLod alpha, its composite and hair's are both
  blended over the scene colour with no order between them and no depth test against each other: hair is drawn over
  glass that is in front of it, or under it, by graph order. Not addressed.
- **FSR** takes the reactive mask too (`upscalingMode` FSR3 ran with it); its image was not measured.

### What it looks like, and what is still wrong

Captures of both paths, the procedural groom and `wCurly` at 0.8, 3.2 and 12.8 m:

- **Fringe and outline.** The layered path's are smooth at every distance. Close up a strand of the fringe is a thin
  continuous line where the surface path draws dots along it; in the middle view the surface path's fringe is visibly
  grainy and the layered path's is a soft gradient; far away both are a blob, the layered one without the speckle.
- **Shading.** The layered groom is flatter. Its brightness over the square is within 2% of the surface path's, but
  its response is the placeholder, lit from every side: there is no self-shadowing, so the side of a groom away from
  the sun and the hair deep inside it are as bright as the outside, and there is no ambient occlusion. `wCurly`'s
  clumps still read, from the strands' directions, but with less depth than on the surface path, where shadows and
  occlusion fall on the hair as on a surface. These are the next two steps' (the shading model, since built:
  [Hair shading measurements](#hair-shading-measurements); the density volume).
- **Still wrong, in order of what it costs the image:** no self-shadowing or multiple scattering; hair casts no shadow;
  softness in motion from one motion vector a pixel; a strand wider than a pixel has hard edges and one colour across
  it; no ambient occlusion or specular environment; no order against other transparents; hair is in no reflection.
- **Still wrong, in cost:** the coverage add in the rasterizer, and shading that follows marked vertices without
  compaction.

### Scenarios and checks

**Scenarios.** All five pass on the surface path, with the counters of the runs before this step, and all five pass
with `HAIR_PATH=layered`. One check changed for the layered path, in `hair_sim.cs`: the comparison of the rigid groom's
rasterizer counters before and after the simulation. On the layered path it leaves out `segments_drawn` and
`pixels_written` (a write that finds three nearer strands in its pixel is skipped, and how many find them depends on
the order the rasterizer's threads ran in: 2.0 to 2.9 million in consecutive frames of one view) and compares the
pixels hair is blended into, which are the same from frame to frame, to one part in 10,000 as with coverage off.
`HairCoreTests` passes.

**Invariants.** `stale_guides_drawn`, `unseen_strands_drawn` and `stale_guides` are 0 in every frame of every scenario
on both paths.

**Debug layer.** A control-pipe session with the D3D12 debug layer on, on the crowd: the layered path, the reactive mask off and on, both light lookups, the coverage saturation, the depth threshold at 0.9, back to the surface path and to the layered one, a probe, every debug view, a capture, then the simulation. The layer's messages are those of a session with no hair (the same fourteen message ids), and none names a hair resource. The stage script is stopped by the layer's error 1047 within a few frames (the renderer's packed buffers, as before).

## Hair shading measurements

Measured on 2026-11-27 for the second step of the second stage: [the hair shading model](#the-hair-shading-model) and
the shade list on the [layered path](#the-layered-path). Same machine, conditions and method as
[Layered path measurements](#layered-path-measurements): raster mode, DLSS at 2560x1600 in and out, the display awake,
uncapped, times from the statistical profiler at a 3% half-width, image differences of 8-bit luma of presented frames.
The scripts and captures are not in the tree.

### What it looks like

Captures of three grooms in the black, brown, blond and red presets, at 1 m and 3 m, with the light in front of the
camera's subject, to its side and behind it, and of each part of the response alone. The grooms: the procedural
drooping groom, made denser and thicker than the distance series' so that it is not a haze (50,000 strands of 176 to
88 micrometres), and Cem Yuksel's `wStraight` and `wCurly`. Defaults otherwise (`beta_m` 0.3, `beta_n` 0.3, tilt 2
degrees).

**The light is the demo's own sun** (`light1`, intensity 10, 29 degrees above the horizon), and the camera goes round the
head instead of the light. A scenario cannot turn that light off, and the frame's exposure is fixed and suits it: with a
scenario light bright enough to dominate it (30, 60), a white floor is far past white and black hair is mid grey in the
capture, which says nothing about the model. A white floor is in every capture as the reference. The sky's
image-based light is on.

- **Black stays dark but is not black.** It is an even charcoal (about a quarter of the white floor's 8-bit level) from
  every side, with a soft silver sheen where the fibres mirror the sun. All of it is R: a fibre's 6.6% reflection, on
  every fibre of the groom at full strength, since nothing shadows anything, plus the environment's R, which is about
  as bright as the sun's in these views (the `environment` view alone is a flat mid-dark grey over the whole groom,
  crown to tips, inside and out). Real black hair is darker between its highlights; the missing self-shadowing and the
  unoccluded environment are both the density volume's.
- **Brown** is the same charcoal with a warm cast: its colour comes from TRT alone (TT is under 7% at this absorption),
  and TRT is a dark red-brown that the white R and the environment's grey sit on top of.
- **Blond is too dark and too grey:** a khaki beige at about two fifths of the floor's level. Its TRT is a pale tan and its TT
  only shows against the light. The brightness and saturation of light hair in life are light scattered from fibre to
  fibre, which this step does not have. The red preset reads as a dull tan for the same reason, not as red.
- **Against the light the rim glows, coloured.** Where the sun comes through hair toward the camera (the crown and the
  outline) brown glows orange-red, blond white-yellow and red gold; black gets only a thin silver rim. That is TT, and it
  is the right behaviour in kind. In amount it is too much: there is no transmittance, so light comes through the whole
  thickness of the groom unattenuated, and `wCurly`'s sunlit top seen from behind is blown out to white for blond
  (it reaches 255). The rest of a backlit groom is dark, in the head's shadow and off the TT lobe.
- **A primary and a secondary highlight exist, and at the default roughness they do not separate.** At `beta_m` 0.3 the
  lobes are 17 degrees wide (R) and 33 (TRT) against shifts of 4 and 8: `wStraight` shows one broad sheen. At
  `beta_m` 0.15 it shows a white band (R) and, toward the roots from it, a coloured one (TRT, red-brown on brown, tan on
  blond); raising the tilt from 2 to 5 degrees moves them further apart, the white one toward the tips. The `lobe_r` and
  `lobe_trt` views show each band alone.
- **Per-strand variation** at its defaults (15% in absorption, 10% in roughness) is barely visible: in a close view of
  the procedural groom the varied and uniform captures differ by a faint streakiness in brown and not at all in blond.
- **The middle distance looks like the close one,** for all three grooms and all colours: the level of detail does not
  change the shading's overall level or colour.
- **Curls.** `wCurly` reads as curls from the strands' directions alone. Where it is backlit, single vertices in the
  shadowed part catch the TT lobe and show as bright specks, a few pixels each, in blond and red: per-vertex shading of
  a strand whose direction turns faster than its vertices follow (the builder's 33 points a strand on curls of 57 to
  73). No facets are visible along highlights at the default roughness, on curls or straight hair.
- **Also seen, not this step's:** on a backlit groom the edge of the head's shadow on the hair is stepped in blocks (the
  shadow map's texels at the page the lookup finds; the counters report no coarser or missing page), most visible in the
  single-lobe views.

### Cost

`ms` per pass on the layered path, in the stage and views of [Layered path measurements](#layered-path-measurements)
(two shadowed directional lights, default material). "Lit" is the shade list's rows in the frame the counters were read
from; it varies by a tenth or more between frames of one view, because which segments reach a layer depends on the
order the rasterizer's threads run in.

| View | Raster | Shade list | Shade | Composite | Hair passes | Lit, of drawn vertices | Shade in the earlier step |
|---|---:|---:|---:|---:|---:|---:|---:|
| Procedural, 0.4 m | 3.34 | 0.030 | 0.90 | 1.00 | 5.68 | 477,468 of 990,000 | 1.38 |
| Procedural, 0.8 m | 1.45 | 0.029 | 0.93 | 0.28 | 3.10 | 491,240 of 990,000 | 1.31 |
| Procedural, 1.6 m | 0.67 | 0.027 | 0.78 | 0.08 | 1.95 | 407,283 of 990,000 | 1.17 |
| Procedural, 3.2 m | 0.38 | 0.026 | 0.65 | 0.06 | 1.51 | 352,540 of 975,150 | 1.09 |
| Procedural, 6.4 m | 0.21 | 0.019 | 0.26 | 0.05 | 0.67 | 120,732 of 318,971 | 0.39 |
| Procedural, 25.6 m | 0.13 | 0.016 | 0.07 | 0.05 | 0.31 | 13,434 of 24,895 | 0.08 |
| Procedural, 100.0 m | 0.13 | 0.015 | 0.04 | 0.05 | 0.27 | 1,680 of 2,884 | 0.04 |
| `wStraight`, 0.4 m | 12.39 | 0.020 | 0.53 | 0.79 | 13.91 | 263,329 of 608,992 | 0.68 |
| `wStraight`, 0.8 m | 2.99 | 0.018 | 0.47 | 0.22 | 3.87 | 274,963 of 608,992 | 0.64 |
| `wStraight`, 1.6 m | 1.14 | 0.016 | 0.35 | 0.07 | 1.76 | 182,519 of 608,992 | 0.58 |
| `wStraight`, 3.2 m | 0.50 | 0.015 | 0.30 | 0.06 | 1.06 | 180,606 of 584,382 | 0.50 |
| `wStraight`, 6.4 m | 0.25 | 0.014 | 0.21 | 0.05 | 0.63 | 80,776 of 328,447 | 0.36 |
| `wStraight`, 25.6 m | 0.15 | 0.012 | 0.08 | 0.05 | 0.34 | 27,463 of 52,708 | 0.11 |
| `wStraight`, 100.0 m | 0.13 | 0.010 | 0.03 | 0.05 | 0.26 | 3,345 of 5,739 | 0.04 |
| Crowd, near camera | 1.03 | 0.084 | 2.04 | 0.14 | 3.80 | 1,067,855 of 2,382,030 | 2.83 |
| Crowd, far camera | 0.22 | 0.062 | 0.33 | 0.06 | 0.86 | 123,017 of 245,212 | 0.46 |

- **The shade pass is 22% to 42% cheaper than in the earlier step, with the new response in it.** Its time now follows
  the lit vertices: 1.7 to 2.0 ns each wherever there are more than a hundred thousand (0.93 ms for 491,000, 2.04 ms
  for 1.07 million), which is what the earlier step paid per vertex when it lit the whole pool (1.79 ms for 990,000).
- **What the list buys, same session, same response.** With the shade pass put back to walking the pool and testing the
  marks: 1.34 ms against 0.93 for the procedural groom at 0.8 m, 0.70 against 0.47 for `wStraight`, 2.95 against 2.04
  for the crowd: 31% to 34% off the pass, 28% to 31% after the list's own pass (`wStraight`'s two runs lit 275,000 and
  191,000 vertices in the frames read, so its figure is the least certain).
- **The shade-list pass** costs 0.03 ms for a pool of 990,000 rows and 0.08 ms for the crowd's 2.4 million; 0.01 to
  0.02 ms at the least. Both it and the shade pass dispatch over the most rows the frame's pool can use, which the CPU
  bounds from above, and for the crowd seen from afar that bound is what the list pass's 0.06 ms is.
- **The response itself is a few percent of the pass.** At the three near views, lit through one lobe alone the pass
  costs within 5% of all three (procedural groom: 0.90 R, 0.91 TT, 0.88 TRT, 0.93 all with the environment). The
  environment's four directions cost 0.02 to 0.06 ms (0.91 against 0.93, 0.42 against 0.47, 1.98 against 2.04 with and
  without). With the lights' lobes off and the environment alone the pass is 0.17, 0.09 and 0.42 ms: what a vertex
  costs before any light, 0.35 to 0.42 ns.
- **A light costs 0.65 to 0.77 ns a vertex,** shadow lookup included: the two shadowed lights are 0.74 ms of the procedural
  groom's 0.93, 0.33 of `wStraight`'s 0.47 and 1.56 of the crowd's 2.04. It is the light's evaluation and its soft
  shadow lookup, not the response, that the pass spends its time on. (Turning `enablePunctualLighting` off did not
  change the pass's time: its declaration reads the setting when the graph is built and the change did not rebuild it.
  Not looked into.)
- **Memory.** `Hair.ShadeList` adds 16 MB (4 bytes a pool row): 465 MB of pooled buffers. An instance's row grew from 96
  to 128 bytes.

### Temporal check

The earlier step's two measures on the layered path, with the default material and with the one whose highlights are
sharpest (the blond preset at `beta_m` 0.15):

| Groom, distance | Temporal noise: default | Blond, smooth | 99th percentile pixel: default | Blond, smooth | Fine contrast still: default | Blond, smooth | Swaying: default | Blond, smooth |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Procedural, 0.8 m | 0.085 | 0.126 | 0.59 | 0.66 | 14.1 | 19.5 | 7.8 | 10.9 |
| Procedural, 3.2 m | 0.063 | 0.115 | 0.43 | 0.65 | 9.3 | 16.4 | 7.4 | 11.2 |
| Procedural, 12.8 m | 0.068 | 0.101 | 0.46 | 0.47 | 19.5 | 19.8 | 18.2 | 18.4 |
| `wCurly`, 0.8 m | 0.075 | 0.108 | 0.76 | 0.80 | 24.0 | 30.5 | 11.8 | 15.5 |
| `wCurly`, 3.2 m | 0.057 | 0.088 | 0.46 | 0.59 | 14.1 | 18.9 | 11.8 | 15.6 |
| `wCurly`, 12.8 m | 0.069 | 0.103 | 0.56 | 0.55 | 25.3 | 26.3 | 24.2 | 25.0 |

- **Still, highlights do not shimmer.** The default material's temporal noise is 0.06 to 0.09 levels, where the
  placeholder's was 0.06 to 0.11, and the smooth blond's is 0.09 to 0.13 with the worst hundredth of the pixels under
  one level. (The surface path's was 0.4 to 1.3.) The image has more fine contrast than with the placeholder (14.1
  against 12.9 for the sparse groom at 0.8 m, 24.0 against 18.4 for `wCurly`), more still with the smooth material.
- **The camera slide** (0.6 m a second past the sparse groom, then a stop), default material: two frames after the stop
  the hair is 3.35 levels from the converged image (2.95 with the placeholder), two converged captures 30 frames apart
  differ by 0.14 (0.13), and the fine contrast is 11.1 while moving and 11.7 just after the stop against 19.4 converged
  (12.8, 12.0 and 18.1). The same softness in motion as before, from the one motion vector a pixel; nothing new.
- Not measured: a highlight sliding along curls while the light or the head turns. The stills show specks on backlit
  curls ([above](#what-it-looks-like)); whether they twinkle in motion was not looked at.

### What the density volume changes

Everything wrong in the captures but the highlights' separation is the absence of the next step: transmittance toward
each light (backlit hair no longer lit through its whole thickness, the far side of a groom darker than the near
side), occlusion of the environment (TT back in the environment term, the two stand-ins gone, dark hair darker), and
the scattering between fibres that gives light hair its brightness and colour. `HairEvaluateBsdf` takes the
transmittance already; the fibre count along the light's path and the widened lobes of dual scattering are not
parameters yet.

### Scenarios and checks

**Scenarios.** All five (seven runs with their variants) pass on the surface path and with `HAIR_PATH=layered`. No
check was changed. `HairCoreTests` passes with the scattering function's tests.

**Invariants.** `stale_guides_drawn`, `unseen_strands_drawn` and `stale_guides` are 0 in every frame of every scenario on
both paths and in the measurement runs; `shade_list` equals `shaded_vertices` in every frame read; no shadow lookup
found no page or a coarser one.

**Debug layer.** The earlier step's control-pipe session on the crowd with the D3D12 debug layer on, plus
`hair.material` on five instances (a preset, melanin and redness, a colour, an absorption, the other parameters) and
every new debug view: the layer's messages are the same fourteen ids as in that session, and none names a hair resource.

## Self-shadowing measurements

Measured on 2026-11-27 for the third step of the second stage: [the density volume](#the-density-volume) and
[multiple scattering](#multiple-scattering) on the layered path, against the same path without them
(`hair.volume {enabled: false}`, which is the earlier step exactly). Same machine, conditions and method as
[Hair shading measurements](#hair-shading-measurements): raster mode, DLSS at 2560x1600 in and out, the display awake,
uncapped, times from the statistical profiler at a 3% half-width, image differences of 8-bit luma of presented frames
over the square around the groom (floor and sky included, so differences of the hair itself are larger than the
numbers). The light is the demo's sun and its sky. The scripts and captures are not in the tree.

### What it looks like

The earlier step's cases again, each captured without and with the volume: the procedural drooping groom (50,000
strands of 176 to 88 micrometres), `wStraight` and `wCurly`; black, brown, blond and red; the sun in front, to the side
and behind; 1 m and 3 m.

- **Hair has its colour.** Brown reads brown and blond blond from every side, where both were charcoal and khaki: the
  backscattering lobe is most of what light hair shows when lit from the front (the `scatter_local` view alone is a
  saturated tan for blond and black for brown and black hair), and the forward scattered light is little of any view.
  The square's mean brightness rises by 4 to 14 levels for blond and falls by 2 to 6 for black.
- **Lit from behind, light hair is no longer blown out.** The crown of `wCurly` and of the procedural groom, white at
  255 before, is a bright rim; blond and red keep their colour in it. Brown's orange glow through the whole crown is
  gone: brown lets a twelfth through a fibre.
- **Depth.** `wCurly`'s clumps separate, with dark between them; the side of a groom away from the sun is darker than
  the side toward it.
- **Black is darker, and still not black.** A dark charcoal with a broad sheen, 2 to 6 levels under the earlier step's
  over the square. What is left is the fibre's own R lobe on the hair the light reaches first, at the default
  roughness a wide sheen, and the sky's R. Tripling the melanin changes nothing visible.
- **The middle distance looks like the close one** for all three grooms.
- **Still wrong.** (1) Seen against the sun, the side of light hair facing the camera is nearly as bright as lit from
  the front: almost all of it is the environment term (the `environment` view), whose sky this stage has about as
  bright as the sun's own lobes, with only four directions and four samples each of occlusion. Whether that is right
  needs a reference. (2) The head's shadow on backlit hair is the shadow map's, stepped as before, and now more
  visible, a dark shape with an edge on bright hair. (3) Backlit `wStraight` in blond shows lengthwise streaks, strands
  of different brightness side by side. (4) Fine self-shadowing is absent: a voxel is 4 to 5 mm at best here, and what
  is nearer than a voxel is skipped. Strands do not shadow their neighbours, locks do. (5) The outermost hair is
  shadowed by the hair under it where light grazes a groom (below). (6) Hair still casts no shadow on the head.

### Level of detail

The same groom at forced detail 1, 0.5, 0.25 and 0.1 (50,000, 25,600, 13,312 and 5,120 strands) from 1 m, with the
volume on and off: the mean of the square, and of the `transmittance` view, which is the volume's own answer without
the response.

| Case | Transmittance view, detail 1 to 0.1 | Image mean, volume on: change at 0.1 | Volume off: change at 0.1 | RMS difference from detail 1 at 0.1: on | Off |
|---|---|---:|---:|---:|---:|
| Procedural, sun in front, blond | 194.2, 193.4, 192.7, 192.7 | -3.0 | -2.5 | 15.0 | 17.0 |
| Procedural, sun in front, black | 191.7, 189.6, 188.2, 187.7 | -4.6 | -3.4 | 21.5 | 20.2 |
| Procedural, sun behind, blond | 146.5, 146.5, 145.1, 142.5 | -1.9 | -4.0 | 15.5 | 25.1 |
| Procedural, sun behind, black | 139.7, 139.4, 138.0, 135.3 | -5.1 | -4.3 | 24.1 | 23.8 |
| `wStraight`, sun in front, blond | 185.6, 185.4, 185.5, 185.8 | -0.7 | -1.4 | 10.5 | 11.7 |
| `wStraight`, sun in front, black | 179.9, 178.9, 178.8, 180.1 | -1.7 | -1.8 | 15.1 | 14.2 |
| `wStraight`, sun behind, blond | 137.5, 137.2, 136.8, 136.7 | +0.6 | -0.4 | 11.6 | 13.1 |
| `wStraight`, sun behind, black | 133.2, 133.0, 132.7, 132.6 | -0.2 | -0.3 | 13.7 | 13.2 |

**Self-shadowing does not change with the strands drawn beyond 3% of the transmittance view's mean** (0.1% to 0.6% for
`wStraight`, 0.8% to 3.1% for the procedural groom, at a tenth of the strands), and the image changes with the level
of detail as much with the volume as without it: the differences are the level of detail's own (fewer, wider strands).
The voxelize pass deposits half as much work at half the strands (20.8, 10.7, 5.5 and 2.1 million voxel adds) into the
same density.

### Resolution, steps and bias

The procedural groom from 1 m, with the voxels as fine as `max_resolution` allows. Differences are from a reference of
144 voxels (3.7 mm) and 32 and 16 steps.

| Voxels along the longest side | 16 | 32 | 48 | 64 | 96 | 128 |
|---|---:|---:|---:|---:|---:|---:|
| Voxel, mm | 33.2 | 16.6 | 11.1 | 8.3 | 5.5 | 4.2 |
| Voxels | 2,704 | 21,632 | 73,008 | 173,056 | 569,184 | 1,357,952 |
| RMS difference, black, sun in front | 1.2 | 1.1 | 1.0 | 0.8 | 0.7 | 0.7 |
| ... blond, sun in front | 2.8 | 2.8 | 2.4 | 2.0 | 1.5 | 1.3 |
| ... blond, sun behind | 10.6 | 8.1 | 7.0 | 6.1 | 5.2 | 4.9 |
| Mean brightness, blond, sun in front | 172.8 | 172.7 | 172.6 | 172.4 | 172.2 | 171.9 |
| `Hair::Voxelize`, ms | 1.45 | 0.96 | 0.65 | 0.57 | 0.82 | 0.93 |
| `Hair::Shade`, ms | 1.74 | 1.84 | 1.88 | 1.90 | 1.98 | 2.05 |

| Steps (light, environment) | 4, 2 | 6, 3 | 8, 4 | 8, 2 | 12, 4 | 16, 8 | 32, 16 |
|---|---:|---:|---:|---:|---:|---:|---:|
| RMS difference, blond, sun in front (96 voxels) | 2.0 | 1.7 | 1.5 | 2.0 | 1.6 | 1.2 | 1.1 |
| ... blond, sun behind | 8.8 | 6.6 | 5.3 | 8.6 | 5.3 | 3.1 | 1.8 |
| `Hair::Shade`, ms (1.13 with the volume off) | 1.85 | 1.93 | 2.04 | 1.97 | 2.11 | 2.45 | 3.29 |

- **The brightness of a groom hardly moves with the resolution** (a level over a factor of eight in voxel size), which
  is what keeps a groom the same as it recedes and its voxels grow. The image converges slowly and evenly: there is no
  resolution below which it breaks, and none here at which it has converged. The default, 96, is where the voxel is 5
  mm (the size the dual scattering paper used); 64 costs a quarter of a millisecond less in the voxelize pass and
  0.5 to 0.9 levels more difference.
- **A coarse volume costs more to fill, not less:** with 16 voxels a side every strand's deposits land in the same few
  voxels and the atomic adds contend (1.45 ms against 0.57 at 64). Far grooms have coarse volumes and few strands.
- **Steps matter where the light comes through hair far from the vertex** (the sun behind): the difference halves from
  8 and 4 steps to 16 and 8, for 0.4 ms. The environment's steps count as much as the lights' there. At the default
  the march is 0.9 ms of the shade pass for 600,000 lit vertices, most of it not the steps: 4 and 2 steps still cost
  0.72 ms (the two interpolated samples of every march, the table, the second evaluation of the lobes).
- **Bias** (96 voxels, RMS difference from the reference, which has the default bias of 1): 0 voxels 5.5 (black, sun in
  front) and 7.4 (blond), darker by 3 to 4 levels; 0.5: 3.2 and 4.3; 1.5: 0.9 and 2.6; 2: 1.5 and 4.0, lighter. Before
  the bias and the interpolated samples existed, at 48 voxels (11 mm), the `fibre_count` view had 4 to 5 fibres between
  the sun and the outermost hair of the groom's lit side, in arches that followed the voxels: the groom is a layer 5 mm
  thick, thinner than a voxel, and every vertex in it read the layer's own hair as in its way.
- **What is still there of that:** where light grazes a layer (the sun on the top of a head), a ray leaves the layer
  slowly, a voxel along the ray is less than a voxel away from the layer, and the outermost hair reads the hair beside
  it. This is the artifact the Indiana Jones deck shows for density volumes and gives as the reason some grooms needed
  256 voxels a side.
- **Deposits to the nearest voxel** (`splat` `nearest`): 0.20 ms against 0.82 in the voxelize pass, 2.6 million adds
  against 20.8 million, and 0.7 to 2.4 levels from the reference where the default is 0.6 to 1.1. Not the default:
  the slab test has its marches twice as noisy.
- **Directions in the volume were not built,** and nothing here measures what they would buy. The isotropic sine is
  0.785; the sun in these captures is at about 60 degrees to hanging hair (0.87), so its fibre counts are about a
  tenth low. A light along the hair (above a head of straight hair) would be overshadowed several times over.
- **Without the scalp shell** the images are as far from the reference as with it in the front views (0.6 to 1.6
  levels against 0.6 to 1.1) and a little further in the blond backlit one (5.7 against 4.7): in these views, where the
  shadow map has the head for the sun, the shell changes little. It was not measured under a light with no shadow map.

### Motion

- **Still.** The temporal noise of the earlier steps' measure (per-pixel standard deviation of luma over 8 captures 3
  frames apart) with the volume on is that without it: 0.087 against 0.085 (sparse procedural groom, 0.8 m), 0.057
  against 0.060 (3.2 m), 0.066 against 0.064 (12.8 m), 0.069 against 0.065, 0.050 against 0.057 and 0.057 against
  0.069 for `wCurly`. A march's offset is the vertex's own and the deposits' rounding the pool row's own, so a still
  groom's volume and marches are the same every frame. With `jitter` `frame` the noise is 0.08 to 0.15 (blond: 0.125
  against 0.095 at 0.8 m, 0.146 against 0.083 for `wCurly`): the upscaler does not average it away, and it is not the
  default.
- **A simulated groom swaying** (every guide simulated, the head moving 2 cm to each side every two seconds, 20
  captures 6 frames apart): the square's mean brightness varies over the captures by 0.09 to 0.16 levels with the
  volume and 0.11 to 0.17 without, and consecutive captures differ by 5.1 to 7.7 levels with it and 5.9 to 8.6
  without. The `transmittance` view's mean varies by 0.05 levels with the sun in front and 0.2 with it behind. No
  flicker, swimming or lag was seen in the captures or shows in these numbers: the grid moves with the head, and the
  volume is built from the positions the frame draws. No segment was outside its volume's box.
- **The camera slide** (0.6 m a second past the sparse groom, then a stop): two frames after the stop the hair is 3.45
  levels from the converged image with the volume and 3.26 without, two converged captures 30 frames apart differ by
  0.15 and 0.16, and the fine contrast while moving is 13.2 and 11.2. A volume's resolution follows the camera's
  distance and changes by a voxel at a time as it moves; nothing of that showed.
- Not measured: a groom crossing the distance at which its volume changes resolution, frame by frame; hair that the
  simulation throws outside the box (the box is the rest bounds and a tenth).

### Cost and memory

`ms` per pass on the layered path, in the stage and views of the earlier steps (two shadowed directional lights,
default material, the sparse procedural groom of the distance series), without and with the volume.

| View | Shade, off | Hair passes, off | Voxelize | Shade | Hair passes | Frame, off | Frame | Lit vertices | Segments deposited | March samples | Volumes | Voxels | Voxel side, mm |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Procedural, 0.4 m | 0.94 | 5.77 | 0.55 | 1.64 | 7.02 | 20.2 | 21.5 | 464,859 | 960,000 | 14,576,587 | 1 | 645,504 | 5 |
| Procedural, 0.8 m | 0.96 | 3.15 | 0.55 | 1.68 | 4.33 | 16.2 | 17.4 | 511,568 | 960,000 | 16,007,745 | 1 | 645,504 | 5 |
| Procedural, 1.6 m | 0.81 | 2.03 | 0.28 | 1.35 | 2.83 | 14.5 | 15.3 | 389,368 | 960,000 | 12,279,048 | 1 | 206,976 | 7 |
| Procedural, 3.2 m | 0.66 | 1.56 | 0.46 | 1.07 | 2.43 | 14.0 | 14.8 | 312,376 | 945,600 | 9,874,797 | 1 | 25,872 | 15 |
| Procedural, 6.4 m | 0.26 | 0.67 | 0.17 | 0.40 | 0.97 | 13.2 | 13.5 | 113,982 | 300,208 | 3,583,829 | 1 | 3,332 | 30 |
| Procedural, 25.6 m | 0.07 | 0.30 | 0.05 | 0.07 | 0.36 | 13.2 | 13.2 | 13,094 | 19,916 | 103,005 | 1 | 80 | 120 |
| Procedural, 100.0 m | 0.04 | 0.27 | 0.02 | 0.04 | 0.29 | 13.1 | 13.0 | 1,766 | 1,442 | 26,660 | 1 | 8 | 469 |
| `wStraight`, 0.4 m | 0.49 | 13.67 | 0.60 | 0.87 | 14.85 | 27.1 | 28.1 | 304,337 | 558,992 | 9,113,561 | 1 | 547,200 | 4 |
| `wStraight`, 0.8 m | 0.43 | 3.74 | 0.84 | 0.76 | 6.07 | 16.3 | 17.1 | 275,195 | 558,992 | 8,159,648 | 1 | 547,200 | 4 |
| `wStraight`, 1.6 m | 0.36 | 1.77 | 0.58 | 0.60 | 2.58 | 14.3 | 15.1 | 261,234 | 558,992 | 7,363,192 | 1 | 110,880 | 7 |
| `wStraight`, 3.2 m | 0.30 | 1.07 | 0.42 | 0.49 | 1.67 | 13.5 | 14.1 | 145,944 | 536,504 | 3,830,788 | 1 | 14,168 | 15 |
| `wStraight`, 6.4 m | 0.22 | 0.64 | 0.28 | 0.30 | 1.00 | 21.6 | 13.5 | 123,332 | 301,784 | 2,996,763 | 1 | 1,848 | 30 |
| `wStraight`, 25.6 m | 0.08 | 0.34 | 0.10 | 0.10 | 0.46 | 13.2 | 13.3 | 24,845 | 45,548 | 606,173 | 1 | 36 | 120 |
| `wStraight`, 100.0 m | 0.03 | 0.26 | 0.01 | 0.04 | 0.28 | 13.0 | 13.2 | 3,435 | 3,556 | 68,604 | 1 | 8 | 469 |
| Crowd, near camera | 2.06 | 3.83 | 1.14 | 3.18 | 6.09 | 15.2 | 17.4 | 1,071,262 | 2,088,192 | 25,212,926 | 73 | 175,014 | 11 to 126 |
| Crowd, far camera | 0.33 | 0.86 | 0.08 | 0.45 | 1.06 | 14.6 | 14.8 | 122,748 | 146,208 | 2,450,351 | 100 | 808 | 250 to 357 |
| Crowd, facing away | 0.09 | 0.42 | 0.00 | 0.09 | 0.42 | 11.5 | 11.6 | 0 | 0 | 0 | 0 | 0 | |

(`wStraight` at 0.8 m with the volume has its rasterizer at 4.06 ms in this run against 2.99 without, and at 6.4 m the
frame without it reads 21.6 ms: single readings, not looked into; the rasterizer does nothing different.)

- **The volume costs a hero groom 1.2 ms** (0.55 to fill, 0.7 in the shade pass) and **the crowd from its near camera
  2.3 ms** (1.14 and 1.12), on frames of 16 and 15 ms. Far away it is 0.02 to 0.1 ms, and **facing away from the
  crowd nothing**: no instance is in view, no volume is planned, and the voxelize pass records nothing.
- **Filling follows the pool's segments,** 0.5 to 0.9 ns each, the more the coarser the volume against the strands
  (contention, above): 0.55 ms for 960,000 at 5 mm, 0.46 ms for 946,000 at 15 mm. Every selected segment deposits,
  seen or not. **Marching follows the lit vertices:** 1.0 to 1.4 ns a vertex for six marches (two lights and four
  environment directions), about 0.045 ns a sample.
- **Memory.** The voxel buffer is 8.4 MB (2,097,152 voxels of 4 bytes), the volume rows 80 KB and the scatter tables
  1.5 MB: 475 MB of pooled buffers. In use: 2.6 MB for the hero groom at 96 voxels, 0.7 MB for the crowd's 73 volumes
  from the near camera, 3 KB for all hundred from the far camera, none facing away.
- **A budget the volumes do not fit** (the crowd from its near camera, 175,014 voxels wanted): at 20,000 voxels every
  voxel is 2.1 times larger (23 to 264 mm) and the passes cost 1.00 and 3.00 ms; at 2,000, 5.1 times larger, 0.56 and
  2.96 ms; at 150, where the coarsest volumes of all 73 do not fit, the 18 largest on screen keep volumes of 8 voxels
  and 55 are lit without one.

### The two defaults the earlier step left open

- **Longitudinal roughness.** `wStraight` in brown and blond at `beta_m` 0.3, 0.25, 0.2 and 0.15, with scattering:
  0.3 and 0.25 are told apart only side by side, a broad sheen over the whole head; at 0.2 a highlight band is there,
  and at 0.15 it is distinct and the hair reads as glossy. The default was left at 0.3: nothing here says which is
  right, only that 0.25 would change little and 0.2 or less is a different, shinier hair. A reference is what would
  settle it.
- **Melanin.** Black at 4, 8 and 16 is the same image: what black hair shows is not its interior. Brown at 1, 2 and 3
  goes from mid brown to near black, with 2 a dark brown. Blond at 0.08, 0.15 and 0.3 goes from ash to beige to dark
  blond; 0.15 is a pale, slightly grey blond. Red (0.6, redness 0.9) is a golden light brown and at 1.0 nearer auburn:
  it does not read as red. No preset was changed.

### What was approximated, and what a reference would settle

- Read: Zinke et al. 2008 in full (sections 3 and 4, equations 16, 17, 23 to 25 and the pseudo code of figure 5 from
  rendered pages); Karis's multiple scattering slide (39: a wrapped diffuse term tinted by `(C / luma)^(1 - shadow)`
  times the square root of the colour, an "artistic hack" by its own notes) and its notes on shadows; Tafuri's
  slides 24 to 27 (dual scattering with fibre counts from four deep opacity map layers and an attenuation table; no
  equations are given); the Indiana Jones deck's slides 47 to 60 and 67 to 70. Not read: Unreal's shader source,
  pbrt's chapter beyond what the earlier step read.
- From Zinke: the split into direct, forward scattered and backscattered light, the average attenuations and how
  they are measured, the backscattering lobe's attenuation, shift and spread, the widening of the lobes, the density
  factors. From the Indiana Jones deck: that a density volume gives soft, low-frequency self-shadowing with
  self-occlusion artifacts, which is what was found. Nothing of Karis's term is used.
- Approximated, in order of what is likely to matter: one density with no directions; every fibre on the path taken
  to lie as the shading point's does; a voxel of 4 mm and more, with the nearest voxel skipped; expected fibre counts
  with Poisson statistics in place of the paper's counts; the environment by four directions; the scalp as a shell of
  voxels; the table for the instance's material and not the strand's; the backscattering lobe's shape around the
  fibre.
- A path-traced reference of the same grooms and materials would settle: whether light hair is as bright and as
  saturated as it should be (the density factor 0.7 is the paper's, for its hair models); how dark the side of a groom
  away from the light should be, under the sun and under the sky; whether black hair's residual grey is right; and the
  roughness default.

### Scenarios and checks

**Scenarios.** All five (seven runs with their variants) pass with `HAIR_PATH=layered`, where the volume is on by
default, and on the surface path, where nothing of it runs; no check was changed. One surface run of
`hair_sim_lod.cs` failed "dollying away, the level never rises" and passed when run again: the failure the
view-following step recorded at one run in ten. `HairCoreTests` passes with the new tests, in about seven seconds.

**Invariants.** `stale_guides_drawn`, `unseen_strands_drawn` and `stale_guides` are 0 in every frame of every scenario
on both paths and in the measurement runs; no shadow lookup found no page or a coarser one; no segment was outside its
volume's box (`volume_outside`).

**Debug layer.** The earlier steps' control-pipe session on the crowd with the D3D12 debug layer on, plus
`hair.volume` through its controls (off and on, dual scattering off, nearest deposits, budgets of 2,000 and 150
voxels, no scalp, per-frame jitter, 16 voxels with 16 and 8 steps) and the six new debug views: the layer's message
ids are a subset of the earlier session's, and none names a hair resource.

## Cast shadow measurements

Measured 2026-11-28 for step A4 of stage 2 ([Shadow casting](#shadow-casting)), on the development machine at
2560x1600, DLSS at native resolution, the layered path with the density volume on, the display awake and `allowTearing`
on; times from the statistical profiler. The stage is a floor, heads of 11 cm radius 1.6 m above it and the demo's sun,
29 degrees up; a second sun height (69 degrees) is the same stage built tilted by 40 degrees, because a scenario cannot
move the demo's sun. Grooms: *dense* (the procedural drooping groom, 50,000 strands of 0.18 to 0.09 mm), *sparse* (the
same with 2,500 strands), `wCurly` (50,000 strands) and a *tuft* (40 strands). The scripts are not in the tree.

**The shadow map sets the scale of everything below.** In the demo's configuration (directional LOD bias 1.2 plus the
automatic bias of its page budget) receivers within a few metres of the camera read the fifth clipmap, and the shadow
view's own counts put its texel at about 2 cm: a dozen screen pixels at 2.5 m. 12 of the 22 clipmaps are dynamic, with
or without hair's request.

### What it looks like

- **On the ground, dense hair.** The dense groom and `wCurly` cast their whole silhouette where the head alone cast a
  small disc: as dark as the head's own shadow, with the same soft edge, and no dither noise, blocks or holes in a still
  view. Under the low sun and the high one alike. Believable against the hair's density: light does not get through
  these grooms.
- **On the ground, sparse hair.** The 2,500-strand groom adds a faint, smooth veil around the head's shadow, darkest
  where the strands hang close together: the floor under it is darker by 1.1 of 255 on average over the frame's centre,
  against 2.9 for a 12,000-strand groom and 3.2 for `wCurly` (the head's own shadow is 26 of 255 darker than the lit
  floor: the demo's ambient light is strong). Partial, as it should be, and without visible grain in a still view.
- **The tuft and a single strand: nothing.** 40 strands write 1 or 2 texels a frame. A strand 0.18 mm wide covers a
  hundredth of a 2 cm texel, and the real shadow of such a strand 1.6 m above the ground is as faint (the sun's disc
  spreads it over 15 mm). The map cannot show a thin strand's shadow close to its receiver either: see the head.
- **On the head, as configured: nothing.** With a light-coloured head, the skin not covered by hair has the same
  luma with casting off and on to 0.1 of 255 (sparse: 199.6 and 199.5; `wCurly`: 183.2 and 183.3), and to 0.3 with
  `opaque` coverage (199.2). The receiver's lookup starts two texels (4 cm) off the skin and the hair is nearer than that. No acne and no
  detached shadow, because there is no shadow.
- **On the head, with texels under 2 mm** (automatic bias off, bias 2: 1.7 mm). Under the sparse groom the skin is
  darker by 4 of 255 on average, smoothly and most where the hair is densest, with `temporal` coverage; with `fixed`
  it is a field of dark blocks, each a texel, three to five pixels across. Under `wCurly`'s fringe a shadow band lies
  on the forehead, attached to the hairline with no gap and no acne on the lit skin, its edge stepped in texels. At
  bias 3 the skin is 1.5 darker, at bias 1 (finer still) 5.3. `wCurly` then draws 33,500 strands and 718,000 segments
  into the shadow map and writes 1.1 million texels a frame at bias 2 (12 million at bias 1): the shadow view's cost
  follows the texels it is asked to fill.
- **Crawl under a still groom.** The luma of the pixels that are not hair (skin and floor) in six captures: its
  standard deviation over time, averaged over those pixels, is 0.2 to 0.5 of 255 with casting off, 0.3 to 0.7 with
  `temporal` and 0.2 to 0.8 with `fixed`, at every resolution tried. The 99th percentile over pixels is 0.9 to 1.5 off;
  under the sparse groom at the finer resolutions it is 2.3 to 4.4 with `temporal` and 1.6 to 2.3 with `fixed`; under
  `wCurly` at the default resolution it is 6.1 and 5.6, with either pattern: there the edge of the groom's shadow on the
  floor shimmers. So the pattern crawls a little where the shadow is partial and fine, below what a still capture shows;
  captures are several frames apart and say nothing about single-frame flicker.

### Level of detail

The darkening of the floor (mean over the frame's centre, of 255; two captures with casting off differ by 0.007):

| Shadow detail | sparse, 2,500 strands | 12,000 strands | `wCurly` |
|---|---|---|---|
| forced 1 (drawn strands 14,500 / 14,500 / 50,000) | 1.152 | 2.894 | 3.135 |
| forced 0.5 | 1.148 | 2.907 | 3.166 |
| forced 0.25 | 1.126 | 2.919 | 3.142 |
| forced 0.1 | 1.183 | 2.946 | 3.161 |
| automatic, `detail_scale` 2 (1,623 / 1,623 / 4,058) | 1.149 | 2.949 | 3.156 |
| automatic, `detail_scale` 1 | 1.145 | 2.963 | 3.146 |
| automatic, `detail_scale` 4 | 1.204 | 3.041 | 3.206 |
| automatic, `detail_scale` 0.25 | 1.139 | 2.875 | 3.149 |

(The two procedural grooms were in view together: their strand counts are the sum.) The automatic level is within 2%
of full detail, and a tenth of the strands within 3%; the texels written a frame are the same at every level (885 to
901, and 10,500 for `wCurly`). The darkening rises a little as strands get fewer and wider, as it must: wide strands
overlap less. With the vertex level also halved by `detail_scale`, as first built, the automatic level gave 20% less
shadow under the drooping grooms (0.92 against 1.17, 2.40 against 2.93): strands drawn along two chords cover less
ground. That is why the vertex level ignores `detail_scale`.

### Hair's own shading

Hair pixels (alpha above a half) of a close view, casting on against off, in levels of 255; "noise" is off against off:

| Groom, view | noise: mean, 99th | on: mean, 99th, over 8 | on without the cached-page read: mean, 99th, over 8 |
|---|---|---|---|
| dense, lit side | 0.16, 1.0 | 0.10, 0.7, 0 | 0.14, 1.0, 0 |
| dense, shadowed side | 0.16, 1.0 | 0.20, 1.3, 0.004% | 0.64, 7.7, 0.9% |
| dense, across the light | 0.17, 1.0 | 0.12, 1.0, 0 | 0.13, 1.0, 0 |
| `wCurly`, lit side | 0.21, 1.7 | 0.13, 1.0, 0 | 0.21, 2.0, 0.07% |
| `wCurly`, shadowed side | 0.20, 1.7 | 0.26, 3.0, 0.07% | 0.87, 10.7, 2.0% |
| `wCurly`, across the light | 0.18, 1.7 | 0.18, 2.0, 0.008% | 0.45, 7.0, 0.7% |

Hair's shading does not change with casting on, to within the captures' noise but for a few pixels in a thousand on the
side away from the sun. Without the read of the cached pages the head's shadow on the hair behind it leaks: that is the
last column. The heads here are rigid; a skinned head was not tested, and by the design would be the last column.

### Motion and off-screen casters

- **A swaying, simulated groom** (the head moving 5 cm to each side, every guide simulated): the shadow follows in every
  capture of a strip taken through the sway, with no second copy and no patch left behind; `shadow_stale_guides_drawn`
  and the main view's two checks are 0. The shadow map's passes cost 0.26 ms with the head still and 0.85 to 0.87 ms
  with it swaying, the same with casting off and on: what is redrawn is the head's own pages, 5 a frame. Hair adds
  0.11 to 0.12 ms of its own passes.
- **The same with no dynamic clipmaps** (`clodSkinnedShadowDynamicClipmapCountOverride` 0: hair's shadow is then in
  cached pages and lives by invalidation). With the head **still** and the groom simulated, the shadow map redraws 4
  pages a frame and its passes go from 0.26 to 0.85 ms: a simulated groom is invalidated every frame whether its hair
  moves or not. Swaying: 5 pages, 0.86 ms. That is what invalidation costs when hair is beyond the dynamic clipmaps;
  within them it costs nothing.
- **The crowd swaying** (100 heads, rigid grooms): the shadow map redraws 67 pages a frame from the near camera and 17
  from the far one, and its passes cost 0.96 and 0.68 ms against 0.32 still, the same with casting off and on: the
  heads' own movement. Hair enqueues 200 bounds in a frame the heads moved, none of which reach a dynamic clipmap.
- **A groom out of view whose shadow is in view** (the camera over its shadow on the floor, the head behind it,
  simulation on): 3,553 strands are drawn into the shadow map and none into the frame, all of them on the rigid pose
  (`shadow_unseen_strands` 3,553), and the main view's checks stay 0. Turning the camera to the head, the floor's mean
  difference from its final state is 0.57 of 255 in the first capture, 0.32 sixteen frames later and 0.28 to 0.31
  from then on (two captures of a simulated groom at rest differ by about that much): the shadow settles from the
  rigid shape to the simulated one within a quarter of a second, by a few millimetres with the default stiffness.
- **A floor turned and turned back** (the stage tilted by 40 degrees and returned) leaves the spheres without any
  shadow, with hair's casting off as well as on. It is the shadow map's, and not looked into; the stage is built
  tilted instead.

### Cost and memory

GPU time in ms. "Shade" is the shade pass with casting off and on (the two lookups in place of one); the shadow map's
own passes (culling, rasterization, clear, compose, mark, invalidate) are 0.31 to 0.33 ms in every row but the swaying
ones above, off and on.

| View | `ShadowSelect` | `ShadowEvaluate` | `ShadowRaster` | Sum | Shade | Clusters kept | Strands (of the main view's) | Segments | Texels written |
|---|---|---|---|---|---|---|---|---|---|
| dense groom, 0.8 m | 0.026 | 0.021 | 0.021 | 0.07 | 2.27 to 2.31 | 1,024 of 1,024 | 3,553 (50,000) | 14,180 | 3,164 |
| dense groom, 3.2 m | 0.025 | 0.021 | 0.020 | 0.07 | 1.42 to 1.50 | 1,024 | 3,553 (48,509) | 14,180 | 3,155 |
| dense groom, 25.6 m | 0.025 | 0.017 | 0.018 | 0.06 | 0.11 to 0.11 | 1,024 | 2,174 (7,684) | 4,340 | 1,092 |
| `wStraight`, 0.8 m | 0.026 | 0.021 | 0.015 | 0.06 | 0.72 to 0.70 | 1,024 | 3,269 (50,000) | 16,774 | 10,605 |
| `wStraight`, 25.6 m | 0.026 | 0.017 | 0.022 | 0.07 | 0.11 to 0.11 | 1,024 | 2,029 (7,160) | 5,125 | 3,808 |
| crowd, near | 0.031 | 0.187 | 0.220 | 0.44 | 3.34 to 3.43 | 22,528 of 22,528 | 87,624 (293,838) | 244,977 | 25,395 |
| crowd, far | 0.031 | 0.147 | 0.057 | 0.24 | 0.46 to 0.48 | 22,528 | 30,932 (99,004) | 30,932 | 3,234 |
| crowd, facing away | 0.028 | 0.044 | 0.019 | 0.09 | 0.10 | 1,834 | 8,514 (0) | 26,038 | 1,168 |
| crowd, facing away, shadows in view | 0.028 | 0.057 | 0.029 | 0.11 | 0.10 | 3,626 | 16,696 (0) | 51,412 | 3,629 |
| crowd, near, simulated and swaying | 0.031 | 0.421 | 0.225 | 0.68 | 3.28 to 3.40 | 22,528 | 87,624 (293,838) | 244,977 | 25,545 |

- **What it scales with.** `ShadowSelect` with the clusters of the active instances (one thread each, a constant here);
  `ShadowEvaluate` with the strands kept, more when they are skinned to guides; `ShadowRaster` with the kept segments
  times the clipmaps each is drawn into, and with the texels they cover. All three follow the shadow pages the frame
  draws under hair, not the hair in view: facing away from the crowd costs 0.09 ms for the 8 grooms in a hundred whose
  shadows share a drawn page with something in view, and 0.11 ms with the last row's shadows in view.
- **Not zero when nothing is in view** because a page is metres across: clusters over a drawn page are kept whether
  or not the texels they shadow are seen.
- **The shade pass** gains up to about 0.1 ms near (the single-texel read of the cached pages per lit vertex: -0.02
  to 0.12 ms over the rows above, with a confidence half-width of 0.03) and nothing measurable far.
- **Memory.** Four buffers, 38.3 MB: the shadow pool 32.0 MB and its segment ids 4.0 MB, the shadow cluster list 2.0 MB
  and its masks 0.25 MB. The pooled total is 513 MB. The crowd near the camera uses 332,600 of the pool's 1,048,576
  rows. The list was 16,384 rows at first and the crowd overflowed it (22,528 clusters): it now has the main list's
  65,535.

### What a dithered single depth can and cannot hold

- It **can** hold the shadow of hair that is dense along the light: enough strands over every texel that none stays
  open, which is every groom here but the sparse one. The result is the silhouette's shadow at the shadow map's own
  resolution and softness, the same at every level of detail (the table above).
- It **can** hold a partial shadow *in aggregate*: the share of texels left open equals the light a thin layer lets
  through, and the upscaler averages a pattern that changes every frame (1.1 of 255 under the sparse groom, smooth in a
  still view; a little crawl, measured above). It needs the pages redrawn every frame, which the dynamic clipmaps
  give; a cached page holds one frozen pattern, which at these texel sizes is a field of blocks (the `fixed` capture).
  Beyond the dynamic clipmaps sparse hair's shadow is therefore blocky; nothing redraws it there, and it was not
  measured at such a distance.
- It **cannot** hold anything finer than a texel: no strand's own shadow, no strand-on-skin contact shadow (a texel is
  2 cm here and the lookup starts two texels off the surface: nothing within 4 cm), no soft gradation across a
  hairline other than through the dither.
- It **cannot** say what is behind hair: one depth is the nearest caster's. That is why hair's own shading reads the
  cached pages for the head, and why a skinned head, which shares the dynamic layer with hair, is not seen by its own
  hair.
- It **cannot** carry colour or the light hair scatters forward: the shadow of blond hair is as grey as that of black
  hair with the same strands.

### Scenarios and checks

- All seven scenario runs (`hair_single.cs`, `hair_lod_sweep.cs` rigid and simulated, `hair_sim.cs` with `wStraight`,
  `hair_sim_lod.cs`, `hair_crowd.cs` simulated and not) pass on the layered path with shadow casting on (its default)
  and on the surface path, where hair casts nothing; no check was changed. The `hair_sim_lod.cs` dolly failure seen
  once in ten runs earlier did not recur in these two runs. `HairCoreTests` passes.
- In every measured view `stale_guides_drawn`, `unseen_strands_drawn`, `shadow_stale_guides_drawn` and
  `shadow_overflow_clusters` are 0. The shadow pool does overflow when asked for more than it holds: the crowd's
  million strands at `force_detail` 1 drop 16,749 of 22,528 clusters, counted.
- A session under the D3D12 debug layer, driven over the control pipe (the crowd, swaying, every `hair.shadows` control
  in turn, the shadow map without dynamic clipmaps, then simulated): no validation message names hair.

## Rasterizer cost measurements

Measured on 2026-11-28 for the fifth step of the second stage: [the culled rasterizer](#the-culled-rasterizer) and
[hidden clusters](#hidden-clusters), against the direct rasterizer on the layered path. Same machine, conditions and
method as [Layered path measurements](#layered-path-measurements): raster mode, the display awake, uncapped, times
from the statistical profiler at a 3% half-width, counters as means over 32 consecutive frames, image differences of
8-bit luma of presented frames. Render resolution is 2560x1600 (DLSS with render equal to output) unless a row says
DLSS Quality, which renders 1707x1067 for the same output. `wStraight` and `wCurly` are Cem Yuksel's models (50,000
strands each) on an 18 cm head; "0.4 m" is the camera's distance from the head's middle, at which the groom fills the
view. The scripts are not in the tree.

**What the step was asked for.** About 1.5 ms of rasterizer with `wStraight` filling the view at 2560x1600, about 1 ms
at a normal upscaled resolution, and 2 to 3 ms for all of hair's rendering in such a close-up; before it the
rasterizer took 12.3 ms there.

**What it reached.** "Rasterizer" is `Hair::Classify` plus `Hair::Raster`; "all six" adds voxelize, shade list, shade
and composite.

| `wStraight` at 0.4 m | Direct rasterizer (before) | Culled, jitter on (in use) | Culled, jitter off | Target |
|---|---:|---:|---:|---:|
| Rasterizer, 2560x1600 | 12.06 | 3.63 | 1.86 | about 1.5 |
| Rasterizer, DLSS Quality (1707x1067) | 4.89 | 1.91 | 0.95 | about 1 |
| All six, 2560x1600 | 14.39 | 5.74 | 3.42 | 2 to 3 |
| All six, DLSS Quality | 6.85 | 3.48 | 2.07 | 2 to 3 |

**The targets are not met in use.** With the camera's jitter on, which is how every frame is drawn, the rasterizer is
3.3 times faster than it was and 2.1 ms over its target at 2560x1600. The figures that do meet or approach
the targets are those with the jitter off, which is what the profiler measures unless told otherwise (next section),
and they hold only for a still camera with no upscaler jitter. Both sets hold only for hair whose strands are about a
pixel wide on screen, which is what these two models are at this distance (0.3 mm strands, four times a real hair's);
for hair of real width the step bought nothing ([Fine hair](#fine-hair)).

### The camera's jitter, and what the profiler measures

A profile run puts the renderer in its deterministic sampling mode, which turns the camera's sub-pixel jitter off
(`Renderer::SetDeterministicSamplingMode`) and leaves it off for the rest of the session, while the `enableJitter`
setting still reads true. **Every profiled time in this document before this section was measured without jitter.**
That did not matter to passes whose work does not depend on the frame before. It matters to the culled rasterizer,
whose first pass is chosen from the frames before: with jitter each frame samples the hair at different sub-pixel
places and sees through different gaps between strands. The rows marked "jitter on" turn the setting off and on again
just after a profile run starts, which puts the jitter back for the run.

| View | Direct: raster, all six | Culled, jitter on: classify + raster, all six | Culled, jitter off: classify + raster, all six | First-pass chunks, millions: on, off | Footprint pixels past the depths, millions: direct, on, off | Late chunks drawn a frame: on, off | Strands in the pool: on, off |
|---|---:|---:|---:|---:|---:|---:|---:|
| `wStraight`, 0.4 m | 12.06, 14.39 | 3.63, 5.74 | 1.86, 3.42 | 2.94, 0.74 | 65.1, 7.5, 4.5 | 148,764, 31 | 46,644, 28,933 |
| `wStraight`, 0.6 m | 4.05, 5.88 | 3.07, 4.87 | 1.63, 3.06 | 3.42, 1.23 | 30.5, 9.9, 5.3 | 55,786, 1,318 | 50,000, 43,540 |
| `wStraight`, 0.8 m | 2.79, 4.25 | 2.89, 4.35 | 2.02, 3.24 | 3.21, 1.69 | 22.7, 15.7, 9.9 | 6,950, 2,754 | 50,000, 49,802 |
| `wCurly`, 0.4 m | 9.95, 13.07 | 2.87, 5.46 | 1.51, 3.19 | 2.38, 0.59 | 69.5, 4.9, 3.3 | 125,248, 60 | 46,484, 31,478 |
| `wCurly`, 0.6 m | 3.80, 6.76 | 3.39, 6.25 | 1.77, 4.00 | 4.17, 1.32 | 34.0, 11.1, 5.2 | 54,228, 3,515 | 49,998, 47,139 |
| `wCurly`, 0.8 m | 2.57, 5.28 | 3.01, 5.69 | 1.81, 4.10 | 4.37, 1.73 | 25.4, 16.0, 8.0 | 12,637, 7,688 | 50,000, 49,729 |
| Fine dense groom, 0.4 m | 9.22, 15.46 | 10.81, 16.83 | 10.62, 16.61 | 9.90, 9.54 | 61.6, 52.9, 51.4 | 1,341,586, 1,062,197 | 105,305, 105,194 |
| DLSS Quality: `wStraight`, 0.4 m | 4.89, 6.85 | 1.91, 3.48 | 0.95, 2.07 | 2.25, 0.57 | 32.0, 4.5, 2.2 | 101,566, 98 | 46,898, 30,959 |
| DLSS Quality: `wCurly`, 0.4 m | 4.06, 7.22 | 1.94, 4.57 | 1.16, 3.22 | 2.05, 0.70 | 34.4, 3.5, 2.1 | 90,065, 8,558 | 49,136, 42,382 |

- **The direct rasterizer does not care**; the culled one does twice the work with the jitter on: three to four times
  the chunks in its first pass, and 1.6 times the footprint pixels past the hidden depths.
- Why: a tile's guess is the largest hidden depth of its 64 pixels over the last frames, and one pixel that looks
  through a gap to a deep strand, in one jitter of the cycle, sets it. Without jitter the same gaps are there every
  frame and the guess settles on them; with it the guess is their union over the cycle.
- Three other guesses were built and measured under jitter on `wStraight` at 0.4 m, and none was faster: the last
  frame's tile depths alone (`hint_decay` 0: the first pass shrinks to 2.1 million chunks and the late pass draws 1.5
  million, 3.2 ms); tiles of 4 pixels (half the pixels walked, twice the chunks to sort, 3.5 ms and 0.65 ms of
  classifying); and a stamp per segment, set by the rasterizer or by the composite for the segments a pixel shows,
  with the first pass drawing those stamped in the last 24 frames (2.4 to 2.6 million chunks, 3.4 to 3.5 ms). The
  segments shown over a jitter cycle are about three times those one frame shows, and the first pass draws them in no
  order, so most of them pass the hidden depth before the strand that hides them is drawn. The segment stamps were
  taken out again.
- What would help, not built: a guess per jitter phase for a still camera (the cycle is 8 phases at this resolution
  and repeats exactly), and a first pass in two steps, the strands shown last frame and then the rest.

### What each change bought

`wStraight` and `wCurly` at 0.4 m, 2560x1600, **jitter off**, one control changed at a time from the top down, on the
build before the last change to the guess (which costs these rows about 0.1 ms). "Reference" is the culled rasterizer
with nothing culled and every strand kept, which is what the image comparisons are against. Times in ms; "per hair
pixel" is over the pixels that end up showing hair (1.86 million for `wStraight`, 1.21 million for `wCurly`).

| `wStraight` | Classify | Raster | Voxelize | Shade | Composite | All six | Looked at / hair px | Pass the depths | Coverage adds | Image: mean, worst block |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Reference: chunks, nothing culled | 0.29 | 11.73 | 0.60 | 0.79 | 0.88 | 14.30 | 40.5 | 35.0 | 35.0 | itself again: 0.007, 2.5 |
| Direct rasterizer | 0.00 | 11.37 | 0.60 | 0.76 | 0.84 | 13.60 | about 114 | 35.0 | 35.0 | 0.008, 2.5 |
| + a strand that covers its pixel hides (one pass) | 0.28 | 4.30 | 0.60 | 0.68 | 0.88 | 6.75 | 40.5 | 5.2 | 1.0 | 0.009, 2.5 |
| + saturated coverage hides (one pass) | 0.28 | 4.37 | 0.60 | 0.67 | 0.88 | 6.82 | 40.5 | 5.0 | 0.9 | 0.009, 2.5 |
| Two passes, first: a rank prefix | 0.24 | 4.52 | 0.60 | 0.66 | 0.88 | 6.91 | 25.8 | 4.5 | 0.8 | 0.009, 2.5 |
| Two passes, first: what the frame before saw | 0.26 | 1.70 | 0.60 | 0.32 | 0.85 | 3.75 | 5.1 | 2.4 | 0.5 | 0.009, 2.5 |
| Two passes, first: both | 0.27 | 2.39 | 0.60 | 0.42 | 0.86 | 4.56 | 10.1 | 3.3 | 0.6 | 0.009, 2.5 |
| ... the frame before, without the saturation rule | 0.25 | 1.64 | 0.60 | 0.32 | 0.85 | 3.68 | 5.1 | 2.4 | 0.5 | 0.009, 2.5 |
| + hidden clusters at a quarter (the default) | 0.21 | 1.64 | 0.34 | 0.32 | 0.85 | 3.37 | 5.1 | 2.4 | 0.5 | 0.009, 2.5 |

| `wCurly` | Classify | Raster | Voxelize | Shade | Composite | All six | Looked at / hair px | Pass the depths | Coverage adds | Image: mean, worst block |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Reference: chunks, nothing culled | 0.37 | 10.40 | 1.13 | 1.51 | 0.62 | 14.08 | 68.4 | 57.4 | 57.4 | itself again: 0.023, 3.1 |
| Direct rasterizer | 0.00 | 10.07 | 1.13 | 1.62 | 0.61 | 13.48 | over 100 | 57.4 | 57.4 | 0.026, 3.3 |
| + a strand that covers its pixel hides (one pass) | 0.38 | 3.49 | 1.13 | 1.23 | 0.63 | 6.90 | 68.3 | 5.8 | 1.1 | 0.028, 4.0 |
| + saturated coverage hides (one pass) | 0.38 | 3.54 | 1.13 | 1.26 | 0.63 | 6.99 | 68.3 | 5.7 | 1.0 | 0.029, 4.1 |
| Two passes, first: a rank prefix | 0.32 | 3.04 | 1.13 | 1.19 | 0.63 | 6.36 | 33.1 | 4.8 | 0.9 | 0.031, 4.2 |
| Two passes, first: what the frame before saw | 0.34 | 1.33 | 1.14 | 0.60 | 0.62 | 4.07 | 6.1 | 2.7 | 0.5 | 0.032, 4.1 |
| Two passes, first: both | 0.35 | 1.84 | 1.13 | 0.81 | 0.62 | 4.80 | 14.8 | 3.9 | 0.7 | 0.032, 4.3 |
| ... the frame before, without the saturation rule | 0.34 | 1.33 | 1.14 | 0.61 | 0.61 | 4.07 | 6.1 | 2.8 | 0.5 | 0.033, 4.3 |
| + hidden clusters at a quarter (the default) | 0.27 | 1.25 | 0.72 | 0.60 | 0.61 | 3.49 | 6.1 | 2.8 | 0.5 | 0.033, 4.4 |

- **Chunks alone buy nothing.** One thread a chunk with every chunk drawn costs what one thread a segment did: the time
  is in the pixels, not in idle lanes. Lane occupancy is 0.66 (`wStraight`) and 0.59 (`wCurly`) with every chunk
  drawn, and 0.80 and 0.64 in the first pass of the last rows. The Indiana Jones rasterizer reports 0.45 rising to 0.80
  with its pixel request stacks, for a factor of two; here the lanes were not the limit, and the stacks were not built.
- **A strand that hides what is behind it is the largest single step**: 11.7 to 4.3 ms in one pass, with the order the
  threads happen to run in. The pixels looked at are the same 40 a hair pixel, but 5 of them pass the hidden depth
  where 35 did, and the coverage adds fall from 35 a hair pixel to 1: a strand that covers its pixel whole marks it and
  adds nothing.
- **Drawing what the frames before saw first is the second**: 4.3 to 1.7 ms. The pixels looked at fall from 40 a hair
  pixel to 5, because a chunk the tile depths hide is not walked at all, and those that pass the hidden depth from 5 to
  2.4. The rank prefix as the first pass does much less (4.5 ms, 26 pixels looked at): an eighth of the strands leaves
  holes that the late pass, the slower of the two, then fills. Both together are worse than the frames before alone
  (2.4 ms): the prefix adds chunks the frames before knew to be hidden. **With jitter on this second step is worth
  less**: the final rasterizer is at 3.6 ms (`wStraight`) and 2.9 ms (`wCurly`), not 1.9 and 1.5.
- **The saturation rule does nothing for these two** (their strands are opaque one by one) and costs 0.05 ms. It is
  what culls where strands are just under a pixel wide.
- **Hidden clusters** take three quarters of the strands of 591 of `wStraight`'s 1,024 clusters and 517 of `wCurly`'s
  out of the pool (50,000 strands to 28,800 and 31,500): 0.26 and 0.41 ms less voxelizing, 0.05 and 0.07 ms less
  classifying, and evaluation falls with it (0.17 to 0.09 ms, 0.60 to 0.33). With jitter on fewer clusters stay
  hidden (about 300 of `wStraight`'s, 45,000 strands): a cluster counts as seen when any pixel of it passes the hidden
  depth, and under jitter more are drawn before what hides them.
- **The shade list follows the rasterizer**: 245,000 lit vertices to 80,000 (`wStraight`), 0.79 to 0.32 ms of shading,
  because a hidden strand no longer marks its vertices.
- **The image does not change**, to within what two captures of the reference differ by: a mean of 0.007 to 0.03
  levels, a 99.9th percentile of 1 to 5 and a worst 16x16 block of 2.5 to 4.5 in every row, the reference's own
  repeat included (the columns drift upward together over the run).

**Coverage only where it is used** (the third required item) is the marked pixel: a strand that covers its pixel whole
adds nothing, and the composite reads the mark. Adds fall from 35 a hair pixel to 0.5. A1's `coverage_saturation` lost
10% to 25% on sparse hair because it paid a load of the coverage texel on every footprint pixel to save an add on few;
here the test is the load of the hidden depth, which replaces the load of the opaque depth the rasterizer made anyway.
For strands thinner than a pixel the adds are all still made: every one of them is used (below).

### Where the final rasterizer's time is

`wStraight` at 0.4 m, 2560x1600, per pixel that shows hair:

| | Direct | Culled, jitter off | Culled, jitter on |
|---|---:|---:|---:|
| Footprint pixels looked at | about 114 | 5.1 | 19.4 |
| ... that pass the opaque and hidden depths | 35.0 | 2.4 | 4.0 |
| Coverage adds | 35.0 | 0.5 | |
| Layer writes (a pixel under several strands counts for each) | 7.9 to 9.5 | 2.4 | |
| Chunks drawn by the first pass, millions (of 4.6 to 7.2 in the frame) | | 0.74 | 2.94 |
| Late chunks found not hidden, and drawn | | under 10,000 | 148,764 |

- The first pass draws 2.4 strands a hair pixel where 1 would do, and 4 with jitter: it draws every chunk in front of
  the *largest* hidden depth of an 8x8 tile, in no order, so the strands between the nearest and the farthest surface
  of a tile all pass until the nearest has been drawn. With 16-pixel tiles it was 2.8 and 0.15 ms more (jitter off).
- The late pass's tests of 3.9 million hidden chunks cost 0.03 to 0.1 ms (measured by turning it off), and the classify
  pass 0.21 to 0.27 ms: time spent on hair that is not drawn.
- The rest is the first pass's memory traffic: a load of the hidden depth for every pixel looked at, and for those that
  pass an atomic minimum on it, a load of the third layer and one to three atomic minimums on the layers; then the
  per-frame clears of five full-screen targets and the two passes over the hair's tiles.
- **The composite's time has two values** between rows of one session, 0.39 and 0.85 ms at 2560x1600 with the same
  hair on screen, on both rasterizers; which one a row gets was not explained. Totals carry whichever the row had.

### Where it is used, and where it loses

The culled rasterizer is the faster of the two only where hair hides hair strand by strand. Forced on everywhere
against the direct one (jitter off, on an earlier build of this step, as an indication):

| View | Direct: raster | All six | Culled, forced: classify + raster | All six | | Strands in the pool | Footprint pixels past the depths, millions |
|---|---:|---:|---:|---:|---|---:|---:|
| `wStraight`, 0.4 m | 12.76 | 14.63 | 1.82 | 2.94 | | 50,000, 29,279 | 65.1, 4.5 |
| 0.8 m | 2.87 | 4.36 | 2.97 | 4.45 | | 50,000, 50,000 | 22.7, 16.1 |
| 3.2 m | 0.52 | 1.46 | 0.79 | 1.69 | | 47,878, 47,878 | 5.5, 4.0 |
| 25.6 m | 0.19 | 0.46 | 0.32 | 0.61 | | 7,160, 7,160 | 0.1, 0.1 |
| `wCurly`, 0.4 m | 10.93 | 14.19 | 1.51 | 3.20 | | 50,000, 31,875 | 69.5, 3.3 |
| 0.8 m | 2.83 | 5.80 | 3.27 | 6.17 | | 50,000, 50,000 | 25.4, 17.6 |
| Sparse procedural groom, 0.4 m | 3.47 | 6.70 | 3.78 | 6.78 | | 30,000, 26,817 | 15.4, 15.4 |
| 0.8 m | 1.48 | 3.88 | 1.83 | 4.24 | | 30,000, 28,082 | 8.5, 8.5 |
| 3.2 m | 0.42 | 2.04 | 0.73 | 2.30 | | 29,550, 28,335 | 2.1, 2.1 |
| 100 m | 0.17 | 0.31 | 0.30 | 0.44 | | 1,442, 1,412 | 0.0, 0.0 |
| Fine dense groom, 0.4 m | 9.60 | 15.89 | 9.77 | 15.65 | | 120,000, 104,791 | 61.6, 46.5 |
| 1.6 m | 1.85 | 5.83 | 2.62 | 6.22 | | 120,000, 111,265 | 17.0, 13.2 |
| Crowd, near camera | 1.05 | 5.87 | 1.75 | 6.11 | | 293,838, 267,265 | 6.5, 6.5 |
| Crowd, far camera | 0.26 | 0.95 | 0.46 | 1.17 | | 99,004, 90,161 | 0.2, 0.2 |
| Crowd, facing away | 0.20 | 0.42 | 0.36 | 0.58 | | 0, 0 | 0.0, 0.0 |

So `raster` `auto` picks by the strand width on screen: the culled rasterizer from `auto_width_pixels` (the groom's
widest strand, at the distance of the middle of its bounds), the direct one below. Around the threshold, with jitter
on: at 0.85 pixels (0.6 m) the culled rasterizer is ahead (`wStraight` 3.07 ms against 4.05, `wCurly` 3.39 against 3.80), and at 0.64 pixels (0.8 m) it is behind (2.89 against 2.79, 3.01 against 2.57), so the default is 0.75. Without jitter it is ahead at both. The rows are in the table above.

With `auto`, every view outside the close-ups costs what it did (jitter off; the direct rasterizer's time does not
depend on it):

| View | Direct: raster | All six | `auto`: classify + raster | All six | Rasterizer | Strands in the pool | Footprint pixels past the depths, millions |
|---|---:|---:|---:|---:|---|---:|---:|
| Sparse procedural groom, 0.4 m | 3.41 | 6.70 | 3.46 | 6.74 | direct | 30,000, 30,000 | 15.4, 15.4 |
| 0.8 m | 1.44 | 4.09 | 1.44 | 4.01 | direct | 30,000, 30,000 | 8.5, 8.5 |
| 1.6 m | 0.69 | 2.46 | 0.70 | 2.54 | direct | 30,000, 30,000 | 4.2, 4.2 |
| 3.2 m | 0.43 | 2.15 | 0.43 | 2.11 | direct | 29,550, 29,550 | 2.1, 2.1 |
| 6.4 m | 0.24 | 0.90 | 0.24 | 0.92 | direct | 18,763, 18,763 | 0.7, 0.7 |
| 25.6 m | 0.17 | 0.38 | 0.16 | 0.37 | direct | 4,979, 4,979 | 0.0, 0.0 |
| 100 m | 0.16 | 0.30 | 0.16 | 0.30 | direct | 1,442, 1,442 | 0.0, 0.0 |
| `wStraight`, 0.4 m | 11.74 | 13.57 | 1.82 | 2.89 | culled | 50,000, 28,823 | 65.1, 4.5 |
| 0.8 m | 2.89 | 4.46 | 2.89 | 4.46 | direct | 50,000, 50,000 | 22.7, 22.7 |
| 1.6 m | 1.14 | 2.46 | 1.15 | 2.50 | direct | 50,000, 50,000 | 11.4, 11.4 |
| 3.2 m | 0.55 | 1.54 | 0.55 | 1.59 | direct | 47,878, 47,878 | 5.5, 5.5 |
| 6.4 m | 0.28 | 0.96 | 0.28 | 0.96 | direct | 26,663, 26,663 | 1.6, 1.6 |
| 25.6 m | 0.19 | 0.46 | 0.19 | 0.46 | direct | 7,160, 7,160 | 0.1, 0.1 |
| 100 m | 0.17 | 0.30 | 0.16 | 0.29 | direct | 2,183, 2,183 | 0.0, 0.0 |
| `wCurly`, 0.4 m | 10.59 | 13.79 | 1.53 | 3.18 | culled | 50,000, 31,114 | 69.5, 3.4 |
| 0.8 m | 2.75 | 5.57 | 2.78 | 5.68 | direct | 50,000, 50,000 | 25.4, 25.4 |
| Crowd, near camera | 1.07 | 5.95 | 1.07 | 5.96 | direct | 293,838, 293,838 | 6.5, 6.5 |
| Crowd, far camera | 0.25 | 0.94 | 0.25 | 0.94 | direct | 99,004, 99,004 | 0.2, 0.2 |
| Crowd, facing away | 0.20 | 0.40 | 0.19 | 0.40 | direct | 0, 0 | 0.0, 0.0 |
| DLSS Quality: `wStraight`, 0.4 m | 4.60 | 6.45 | 0.93 | 2.05 | culled | 50,000, 30,687 | 32.0, 2.2 |
| DLSS Quality: `wCurly`, 0.4 m | 4.20 | 7.54 | 0.98 | 2.65 | culled | 50,000, 35,975 | 34.4, 1.9 |
| DLSS Quality: `wCurly`, 0.8 m | 1.65 | 4.02 | 1.65 | 4.03 | direct | 50,000, 50,000 | 17.0, 17.0 |
| DLSS Quality: fine dense groom, 0.4 m | 5.65 | 10.99 | 5.53 | 10.82 | direct | 120,000, 120,000 | 41.1, 41.1 |
| DLSS Quality: fine dense groom, 1.6 m | 1.28 | 5.38 | 1.28 | 5.51 | direct | 118,233, 118,233 | 11.3, 11.3 |
| DLSS Quality: crowd, near camera | 0.57 | 3.52 | 0.57 | 3.54 | direct | 209,775, 209,775 | 3.5, 3.5 |
| DLSS Quality: crowd, far camera | 0.15 | 0.65 | 0.15 | 0.65 | direct | 69,500, 69,500 | 0.1, 0.1 |
| DLSS Quality: crowd, facing away | 0.11 | 0.30 | 0.12 | 0.30 | direct | 0, 0 | 0.0, 0.0 |
| DLSS Quality: crowd, fly-through | 0.44 | 2.95 | 0.45 | 3.00 | direct | 77,791, 77,550 | 1.6, 1.6 |

### Fine hair

The two reference models have strands of 0.3 mm. A groom of 120,000 strands of 88 to 66 micrometres, about a real
head's, at the same 0.4 m has strands a third of a pixel wide, and **costs 15 to 17 ms, with either rasterizer**:

| Fine dense groom | Evaluate | Voxelize | Raster | Shade | Composite | Hair pixels | Footprint pixels past the depths | Lit vertices |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.4 m, 2560x1600 | 1.55 | 1.98 | 9.19 | 3.53 | 0.54 | 2,091,715 | 61,563,592 | 1,016,514 |
| 1.6 m, 2560x1600 | 1.58 | 1.22 | 1.84 | 2.52 | 0.09 | 130,136 | 17,020,621 | 749,741 |
| 0.4 m, DLSS Quality | 1.31 | 2.02 | 5.65 | 2.99 | 0.23 | 945,364 | 41,071,265 | 864,128 |

- No strand covers a pixel, so none hides anything by itself, and a pixel's opacity is the sum of some thirty strands'
  coverage: 61.6 million footprint pixels pass the opaque depth for 2.09 million hair pixels, 29 each, at a coverage of
  a third. The saturation rule can stop at 10 pixels' worth, which is itself 30 strands. With the culled rasterizer
  forced, 47 million still pass and the time is the same.
- What brings the cost down is fewer, wider strands: with the detail level forced (`hair.lod` `force_detail`), which
  widens the kept strands by the same factor, the same view costs 13.6 ms at 0.7, 10.3 at 0.5 and 5.6 at 0.35 (jitter
  off), where strands reach a pixel and the culled rasterizer takes hold (rasterizer 2.3 ms, 6.1 million footprint
  pixels in place of 52 million). It also changes the picture: at 0.35 the strands are visibly coarser, at 0.5 a
  little. This is what the Indiana Jones renderer's budgets do (2,000 segments a 64x64 tile, 2 million a frame, strand
  count traded for thickness), and it is the change this step did not build.
- The select pass's detail rule cannot do it as it is: it looks at how far apart a cluster's strands are across the
  view, 3.4 pixels here, and not at how many lie behind one another.

### Reveal cases

The frames before are only a guess at what to draw first; what the first pass leaves out is tested against the frame's
own hidden depths. The check is what the composite shows, under motions that make the guess as wrong as it can be,
with the culled rasterizer (hidden clusters on) against the reference (nothing culled, every strand): the hair alpha
summed over the frame, the pixels holding hair and the pixels taking hair's depth, as means over 320 frames. These
runs are not profiled, so the jitter is on.

| Groom | Motion | Hair alpha, culled over reference | Hair pixels | Depth pixels | Late layer writes a frame: mean, most | Footprint pixels past the depths: culled, reference | Strands in the pool, culled |
|---|---|---:|---:|---:|---:|---:|---:|
| `wStraight`, 0.6 m | Still | 1.00000 | 1.00000 | 1.00000 | 1,160, 2,383 | 10.7 M, 30.5 M | 50,000 |
| `wStraight`, 0.6 m | A cut every frame | 1.00000 | 1.00000 | 1.00000 | 4,523, 21,294 | 13.4 M, 29.9 M | 50,000 |
| `wStraight`, 0.6 m | A cut every 3 frames | 1.00000 | 1.00000 | 1.00000 | 9,490, 100,191 | 14.0 M, 29.9 M | 49,998 |
| `wStraight`, 0.6 m | Orbit, 6 degrees a frame | 1.00891 | 1.00885 | 1.00885 | 6,210, 46,148 | 14.9 M, 31.2 M | 49,999 |
| `wStraight`, 0.6 m | Orbit, 20 degrees a frame | 0.99582 | 0.99584 | 0.99584 | 2,540, 16,502 | 15.7 M, 31.4 M | 50,000 |
| `wStraight`, 0.6 m | Head turning 60 degrees each way, 40 frames a cycle | 0.99940 | 0.99943 | 0.99943 | 17,333, 442,754 | 15.6 M, 31.3 M | 49,997 |
| `wStraight`, 0.6 m | Camera panning the groom out of view and back, 30 frames a cycle | 0.99256 | 0.99256 | 0.99255 | 69,542, 640,430 | 5.6 M, 23.1 M | 47,654 |
| `wStraight`, 0.6 m | Simulated, head swinging 12 cm each way | 0.99988 | 0.99988 | 0.99988 | 2,153, 14,073 | 14.0 M, 30.7 M | 50,000 |
| `wCurly`, 0.5 m | Still | 1.00000 | 1.00000 | 1.00000 | 1,300, 2,012 | 3.4 M, 44.8 M | 45,130 |
| `wCurly`, 0.5 m | A cut every frame | 1.00000 | 1.00000 | 1.00000 | 5,343, 42,214 | 6.5 M, 37.5 M | 49,441 |
| `wCurly`, 0.5 m | A cut every 3 frames | 1.00051 | 1.00051 | 1.00050 | 34,225, 510,112 | 6.5 M, 37.5 M | 49,500 |
| `wCurly`, 0.5 m | Orbit, 6 degrees a frame | 1.03716 | 1.03698 | 1.03698 | 17,392, 68,807 | 6.9 M, 41.2 M | 49,512 |
| `wCurly`, 0.5 m | Orbit, 20 degrees a frame | 0.99692 | 0.99695 | 0.99695 | 37,802, 144,756 | 7.3 M, 42.3 M | 49,928 |
| `wCurly`, 0.5 m | Head turning 60 degrees each way, 40 frames a cycle | 1.00209 | 1.00209 | 1.00209 | 40,233, 1,031,833 | 6.6 M, 42.7 M | 49,476 |
| `wCurly`, 0.5 m | Camera panning the groom out of view and back, 30 frames a cycle | 0.98418 | 0.98418 | 0.98418 | 17,025, 296,937 | 3.6 M, 35.8 M | 35,642 |
| `wCurly`, 0.5 m | Simulated, head swinging 12 cm each way | 1.00268 | 1.00275 | 1.00275 | 16,225, 110,424 | 4.5 M, 45.3 M | 42,867 |

- **A cut every frame** (the camera alternating between the front and a view from 140 degrees round) **gives the same
  sums to five figures**, as the still view does. The frames are the same two in both runs, so this row is a
  comparison of like with like; so is `cut every 3 frames` but for which of the two views the sample holds more of.
- The orbit, turn, pan and swing rows compare two runs whose frames are not the same poses; the reference against itself, run the same way on `wStraight`, gives 1.009 (orbit at 6 degrees), 0.998 (at 20), 1.0003 (turn), 0.993 (pan) and 0.999 (swing), so `wStraight`'s rows say only that the two agree to what two such runs differ by. The reference was not run against itself on `wCurly`, whose orbit row is 3.7% off (3.3% on an earlier build of this step): the samples are 320 frames of a 60-frame orbit, and where in the orbit a run's sample starts is not controlled.
- `late layer writes` is the hair the guess missed and the frame drew.
- **What a reveal does cost is strands, for four frames.** A cluster that was hidden comes back with the quarter of its
  strands it kept, at their true width, and has them all four frames later. In the cut rows the sums are equal because
  under jitter nearly every cluster of these grooms counts as seen within a frame or two (the strand column); a cluster that has been hidden
  for a while (the far side of a head, turned to face the camera) is thinner than it should be for those frames. With
  `hidden_detail` 1 nothing is. The first form of this drew the kept strands wider, to carry the cluster's coverage;
  they poked through the hair in front, were seen, were drawn thin, and were hidden again, round and round (`wCurly`:
  1,740 late layer writes a frame and 46,755 strands in a still view without jitter, against 0 and 31,500).
- In the captures taken in motion (`hairA5\reveal_r1`, `hairA5\reveal_r2`, `hairA5\reveal_r3` in the session's scratch
  folder) no frame of the culled rasterizer shows a hole or a missing lock.

### Temporal quality, level of detail and self-shadowing

The earlier steps' measures, on the layered path with the direct rasterizer and with the culled one forced
(`BR_HAIR_RASTER`), in separate runs, on the build before the last change to the guess. Their grooms have strands a
fraction of a pixel wide, where `auto` picks the direct rasterizer; forcing the culled one is the harder test. The
captures are of jittered frames.

| Measure | Direct | Culled, forced |
|---|---|---|
| Still noise (per-pixel standard deviation of luma over 8 captures), sparse procedural groom at 0.8, 3.2 and 12.8 m | 0.088, 0.058, 0.066 | 0.087, 0.056, 0.067 |
| ... `wCurly` | 0.083, 0.052, 0.059 | 0.076, 0.049, 0.072 |
| Fine contrast (standard deviation of a Laplacian), still: sparse groom | 14.1, 9.0, 20.1 | 14.1, 8.9, 20.1 |
| ... `wCurly` | 25.6, 13.6, 26.4 | 25.4, 13.5, 26.7 |
| Fine contrast with the head swaying 2 cm: sparse groom | 7.0, 7.4, 19.1 | 7.6, 7.0, 18.9 |
| ... `wCurly` | 12.1, 11.7, 25.2 | 13.8, 11.8, 25.1 |
| Camera slide past the sparse groom: two frames after the stop against the converged image, levels | 3.42 | 3.29 |
| ... two converged images 30 frames apart | 0.68 | 0.71 |
| ... fine contrast just stopped, converged | 13.5, 18.3 | 12.8, 18.6 |
| One strand leaving (detail swept 0.9 to 0.5): largest 8x8 block change a step, median and 90th percentile | 13.0, 50.8 | 12.3, 50.2 |
| ... the same state captured twice | 26.6 | 26.7 |
| ... true area over the sweep, pixels | 34,224 to 34,609 | 34,227 to 34,612 |
| Dolly 2 m to 80 m, live LOD: mean difference between consecutive captures, culled over direct | | 1.02 |
| Self-shadowing against forced detail (`wStraight`, sun behind, blond; mean luma at detail 1, 0.5, 0.25, 0.1) | 154.28, 154.05, 153.89, 154.81 | 154.28, 154.07, 153.90, 154.80 |
| ... the transmittance view's mean at those | 137.05, 136.81, 136.45, 136.28 | 137.02, 136.79, 136.46, 136.28 |

- Nothing separates the two columns. The still noise is that of [Layered path measurements](#layered-path-measurements)
  (0.06 to 0.11 there).
- The one-strand measure is not what it was in that section (1.3 at the median, 1.2 for the same state twice): the hair
  now casts a shadow whose coverage is dithered anew each frame ([Shadow casting](#shadow-casting)), and the blocks
  under it change by tens of levels between any two captures. A leaving strand could not be found in it on either
  rasterizer; the measure would have to mask the shadow to be useful again.
- The dolly's culled run starts with 22,085 of the 24,000 strands: the clusters on the far side of the head are hidden
  and keep a quarter of theirs.
- The forced-detail rows bypass the hidden clusters' scale (a forced level is taken as it is). What hidden clusters do
  to the density is measured apart:

| View, culled rasterizer, jitter off | Strands in the pool: hidden clusters at a quarter, all | Voxel deposits | Transmittance view, mean | ... mean difference from all | The same with all, captured again |
|---|---:|---:|---:|---:|---:|
| `wStraight`, 0.4 m | 35,837, 50,000 | 12.2 M, 16.7 M | 197.54, 197.61 | 0.144 | 0.150 |
| `wStraight`, 0.6 m, from 120 degrees round | 42,083, 50,000 | 14.2 M, 16.7 M | 188.92, 188.95 | 0.108 | 0.120 |
| `wCurly`, 0.4 m | 31,469, 50,000 | 19.7 M, 31.2 M | 207.40, 207.40 | 0.150 | 0.158 |

  A third of the strands out of the pool leaves what a lit vertex sees of the volume where it was: the kept strands
  deposit for the others. With jitter on the same three views keep 47,100, 50,000 and 46,600 strands, and differ less.

### Memory

The culled rasterizer adds four buffers, 52 MB (`Hair.RasterItems` 32, `Hair.PoolLate` 16, `Hair.ClusterState` 4), and
six textures, 63 MB at 2560x1600; the two cluster lists' rows grew from 32 to 48 bytes (2 MB). Pooled buffers are
568 MB (513 before), and the layered path's targets 184 MB (121 before).

### Scenarios and checks

**Builds and tests.** `HairPlugin`, `HairCoreTests` and `BasicRendererDemo` build; `HairCoreTests` passes.

**Scenarios.** All five (seven runs) pass on the surface path and with `HAIR_PATH=layered`, on the final build.
`hair_lod_sweep.cs` gained two checks on the layered path (464 where there were 462): with each rasterizer forced in turn
at one camera, the culled one is given the strands the direct one is, and brings hair into the same pixels to 0.5%. They
are there because `auto` picks the direct rasterizer for every scenario's groom (strands a fraction of a pixel wide), so
that without them no scenario would run the culled one. No existing check was changed.

With the culled rasterizer forced for a whole run (`BR_HAIR_RASTER=culled`) `hair_single.cs`, `hair_sim_lod.cs` and
both crowd runs pass, and two scenarios fail checks that assume what it changes on purpose. `hair_lod_sweep.cs`, five:
every strand drawn at the near end, the count never rising along the dolly, the frozen selection, the occluded groom
still selected with the same strands (hidden clusters keep a quarter of their strands, so the count follows what is
seen), and the covered pixels behind the wall counted as rejected by the opaque depth (their chunks are skipped whole
and no pixel is counted). `hair_sim.cs`, one: the rigid groom's strand count before and after the simulation (18,724
and 18,805, for the same reason).

**Invariants.** `stale_guides_drawn`, `unseen_strands_drawn`, `overflow_clusters` and `raster_items_overflow` are 0 in
every frame of the reveal runs and of the scenarios. There is no counter for hair that should have been drawn and was
not: by construction a chunk is drawn by the first pass or tested by the late pass against the frame's own hidden
depths, and the reveal table's equal sums under a cut every frame are the measurement of it.

**Debug layer.** A control-pipe session with the D3D12 debug layer on, on the crowd with the culled rasterizer forced:
every `first_pass`, the two occlusion rules off and on, `saturation` 6, `opaque_coverage` off, `hidden_detail` 1 and
0.25, `raster_profile`, the late pass off, `direct`, `auto` with the threshold at 0 and at its default, then the
earlier steps' session (materials, the volume's controls, shadows, every debug view, a capture, the simulation) with
the culled rasterizer left forced. Hair is in the same pixels to 0.1% across all of them (259,000 to 259,150). The
layer's messages are those of the earlier steps' sessions (ids 575, 579, 989, 1477, 1508, 1047, 580, 576, 572, 274,
1281, 577, 570, 32) and none names a hair resource. The stage script is stopped by the layer within a few frames, as
before, so the camera could not be moved to a groom of wide strands: the session ran the culled rasterizer on the
crowd only.

**Found and not fixed.** Changing the render resolution with hair loaded on the layered path (`upscaler.set` with a
new quality) stops the demo: `HairRasterPass failed to record: Recording attempted to resolve an undeclared resource`.
It did so before this step's changes (the first baseline run hit it). The measurements at DLSS Quality set the quality
before loading the groom. The profiler's deterministic mode leaving the jitter off is described above.

**Captures.** In the session's scratch folder `hairA5`: `pairs\sheet_pairs.png` (the reference, the culled rasterizer
and their difference times 16 for three views; the pairs cannot be told apart, and the difference is a faint dotted
outline of the silhouette, as between two captures of the reference), `reveal_r3\sheet_r3_culled.png` (the culled
rasterizer in each of the eight motions; the hair is whole in all of them), `detail_t1\sheet.png` (the fine groom at
forced detail 1, 0.5, 0.35 and 0.15).

## Units and axes

- **Units.** `GroomSource`, `ScalpMesh`, `BuiltGroom` and `.groom` files are in metres. Importers convert:
  `LoadAlembicGroom` multiplies positions and widths by `AlembicGroomImportSettings::unitScale`, default 0.01 because
  the UE groom schema is in centimetres. `LoadHairFile` does not convert (the runtime's `hair.load` does, see
  [Commands](#commands)), and the procedural generator works in whatever units its settings are given (the defaults
  are metres).
- **Axes.** The renderer is right-handed with +Y up and -Z forward. Its USD importer rotates Z-up stages onto +Y up;
  its Assimp importer applies no axis conversion. Alembic has no up-axis metadata, so `LoadAlembicGroom` takes
  `AlembicGroomImportSettings::upAxis`: `GroomUpAxis::Y` (the default) keeps positions, `GroomUpAxis::Z` applies the
  USD importer's rotation, `(x, y, z) -> (x, z, -y)`. Neither option mirrors, so handedness is unchanged.
- **Scalp mesh.** A triangle list with counter-clockwise front faces, in the same space and units as the groom.
- **Widths** are diameters.

## Build and test

`HairCore`, `HairCoreTests` and `HairPlugin` are defined in
[`Plugins/Hair/CMakeLists.txt`](../Plugins/Hair/CMakeLists.txt) with explicit source lists; add new files there by
hand, and new shaders or shared headers to the staging lists below them. `HairPlugin` links the renderer and uses its
public headers only. From a SARP checkout, in a Visual Studio developer environment (the Ninja presets need it):

```powershell
cmake --build build\ninja-renderer-host --config RelWithDebInfo --target HairCoreTests HairPlugin BasicRendererDemo
ctest -C RelWithDebInfo --test-dir build\ninja-renderer-host -R HairCore --output-on-failure
```

Build these targets on their own: SARP's default targets link and deploy the renderer host (see SARP's `AGENTS.md`).
The plugin has no CPU tests; its check is the [scenario](#checking-it), run in a copy of the demo in the runtime
root (`build\<preset>\BasicRenderer`).

`HairCoreTests` is one executable with its own checks and no test framework:
[`HairCoreTests.cpp`](../Plugins/Hair/tests/HairCoreTests.cpp) holds `main` and the generator, `.hair` and
`GroomSource` tests, `GroomBuilderTests.cpp` the builder, LOD and `.groom` tests, `AlembicGroomTests.cpp` the
importer tests, which write their own archives with `OCurves`, and `HairBsdfTests.cpp` the tests of the scattering
function ([The hair shading model](#the-hair-shading-model)), which take about five seconds, and
`HairScatterTests.cpp` those of multiple scattering and the density volume's arithmetic. Temporary files go to the system temp directory.
