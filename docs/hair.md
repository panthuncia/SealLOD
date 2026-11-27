# Strand hair

`Plugins/Hair/` is a proof of concept for strand hair: grooms made of individual curves, with a level of detail that
removes strands and vertices continuously and a guide set sized for simulation that follows the view. It is a plugin
beside `Plugins/ProceduralWind/` and is expected to be reworked several times.

Two libraries exist:

- `HairCore`, the offline half: a static library with no device and no renderer dependency. It imports a groom,
  builds the arrays the GPU consumes, and reads and writes them as a `.groom` file. `HairCoreTests` checks it.
- `HairPlugin`, the renderer half ([Runtime](#runtime)): it loads grooms, attaches them to scene entities and uploads
  them. Every frame it writes each strand root's frame in world space, simulates as many of the guides as the view
  asks for when [simulation](#simulation) is on, selects a level of detail per cluster for the main view, evaluates the selected
  strands into a pool, rasterizes them into a hair visibility target, and resolves that target into the frame's surface
  data. The demo (`BasicRendererDemo`) registers it; SARP's renderer host does not.

**Hair is drawn as an ordinary opaque surface with a placeholder material.** The [resolve](#the-resolve) gives every
hair pixel a surface record, depth and a motion vector, so deferred shading lights it, shadows fall on it and the
upscaler follows it. There is no hair BSDF, hair casts no shadow, and a strand is at least one pixel wide with no
coverage or anti-aliasing of its own. Strands follow their roots rigidly unless simulation is turned on (`hair.sim`):
then the groom's guides are simulated on the GPU, collide with spheres and capsules, and the other strands follow them.
How many guides are simulated follows how large the groom is on screen ([Simulation LOD](#simulation-lod)).

POC limits that shape the code: a whole groom is held in memory and on the GPU, there is no paging, streaming or
culling hierarchy, and nothing is tuned for speed.

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
| `hair.attach` | `asset`, `entity` (a scene entity's name), optional `bone`, optional `color` (`[r, g, b]`, linear) and `roughness` | `instance` |
| `hair.clear` | | Drops every instance and asset. |
| `hair.selfcheck` | `enabled` (default true), `roots` per instance (default 8) | `enabled` |
| `hair.lod` | Any of `error_pixels`, `max_width_scale`, `min_detail`, `force_detail`, `segment_pixels`, `freeze` | All six values. See [Selection](#selection). |
| `hair.debug` | `view`: `off`, `cluster`, `detail`, `strand`, `rank` or `vertex_level` | `view`. See [Debug view](#debug-view). |
| `hair.capture` | `path` (a `.bmp` file to write) | `state`: `pending`. `hair.stats` reports when it is written. |
| `hair.probe` | `path` (a binary file to write) | `state`: `pending`. See [Probing the targets](#probing-the-targets). |
| `hair.sim` | Any of `enabled`, `gravity` (`[x, y, z]`), `damping`, `shape_compliance`, `bend_compliance`, `stretch_compliance`, `step_seconds`, `substeps`, `max_steps`, `wind` (`[x, y, z]`), `gust_amplitude`, `gust_frequency`, `friction`, `collision_margin`, `teleport_distance`, `frame_seconds`, `lod_full_coverage`, `lod_level0_coverage`, `lod_off_coverage`, `lod_rate`, `lod_force_level`, `lod_point_budget`, `promote_seconds`, `demote_seconds`; `reset: true` | Every control, the step counters and each instance's last metrics. See [Simulation](#simulation) and [Simulation LOD](#simulation-lod). |
| `hair.simlevel` | `instance`, `level` | `instance`, `level`. Forces one instance's simulation level; a negative level gives it back to `hair.sim`'s rule. For testing. |
| `hair.colliders` | `instance`, `colliders`: up to 8 of `{center, radius}` (a sphere) or `{a, b, radius}` (a capsule), in the entity's mesh space | `instance`, `colliders` (the count). An empty list removes them. |
| `hair.dump` | `buffer` (`pool_vertices`, `visible_clusters` or `guide_points`), `path`, optional `rows` | `state`: `pending`. See [Dumping buffers](#dumping-buffers). |
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
- **Material.** `color` and `roughness` are the instance's [placeholder material](#the-resolve): by default a dark
  brown, `[0.09, 0.05, 0.03]`, at roughness 0.6. A `.hair` file's colours are not used.
- **Nothing is freed one at a time.** Assets and instances only accumulate until `hair.clear`.

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

The runtime makes twenty-one pooled buffers when the first groom is loaded, at fixed sizes (`HairCapacities`, 673 MB
at the defaults, sized for the [crowd scenario](#checking-it)), and never resizes them. A groom or an instance that does not fit is refused with an error
that names the row count and the capacity. `Hair.VisibleClusters` and the two pool buffers hold one frame's selection
and are filled on the GPU: what does not fit there is dropped for the frame and counted ([Selection](#selection)).

| Buffer | Row | Default rows | Written |
|---|---|---|---|
| `Hair.Clusters`, `Hair.ClusterAggregates` | `HairClusterHeader`, `HairClusterAggregate` | 16,384 | once per asset |
| `Hair.Strands`, `Hair.RootBindings` | `HairStrandRecord`, `HairRootBinding` | 262,144 | once per asset |
| `Hair.GuideBindings`, `Hair.GuideStrandIndices` | `HairGuideBinding`, `uint` | 65,536 | once per asset |
| `Hair.Points` | `HairPackedPoint` | 8,388,608 | once per asset |
| `Hair.Instances` | `HairInstanceGPU` | 1,024 | when an instance's row changes |
| `Hair.FrameState` | `HairFrameGPU` | 1 | every frame with an active instance |
| `Hair.RootFrames` | `HairRootFramePair` | 1,048,576 | by the root-binding pass |
| `Hair.Counters` | `uint` | 49 | by the root-binding, select, raster and resolve passes; reset by an upload every frame |
| `Hair.VisibleClusters` | `HairVisibleCluster` | 65,535 | by the select pass |
| `Hair.PoolVertices` | `HairPoolVertex` | 4,194,304 | by the evaluate pass |
| `Hair.PoolSegmentIds` | `uint` | 4,194,304 | by the evaluate pass |
| `Hair.Probe` | `uint` | 1,310,792 | by the probe pass, in a frame `hair.probe` asked for |
| `Hair.GuidePointRanges` | `HairGuidePointRange` | 65,536 | once per asset |
| `Hair.GuidePoints` | `HairGuidePoint` | 4,194,304 | by the simulate pass |
| `Hair.SimGroups` | `HairSimGroup` | 131,072 | every frame an instance is simulated: the thread groups of the simulate and guide-level passes |
| `Hair.Colliders` | `HairColliderGPU` | 8,192 (8 per instance) | when an instance's colliders change |
| `Hair.SimFrame` | `HairSimFrameGPU` | 1 | every frame |
| `Hair.SimMetrics` | `uint` | 32,768 (32 per instance) | by the simulate, guide-level and evaluate passes; reset by an upload every frame the simulation is on |

There is also one texture, `Hair.Visibility`: the [hair visibility target](#the-hair-visibility-target), made at the
render resolution of the first frame and made again when that changes.

- Assets sit back to back in the groom buffers. Indices inside an asset stay groom-relative (`strandOffset`,
  `pointOffset`, guide strand indices): a shader adds the instance's bases.
- [`HairInstanceGPU`](../Plugins/Hair/include/Hair/HairRuntimeGPUTypes.h) holds an instance's bases and counts, its
  first row in `Hair.RootFrames` (it owns one row per strand), and three references into the renderer's own buffers:
  its entity's row of `Builtin::PerObjectBuffer` (model and previous model matrix), its skinning instance slot
  (`SkinningInstanceInfo`, which locates the current and previous bone palettes in `BoneTransforms`) and the attach
  bone. The entity's transform is not copied: the shader reads the row the entity's mesh is drawn with. It also holds
  the instance's placeholder material (base colour and roughness), its first row of guide state, its colliders' rows,
  the simulation flags (`HAIR_SIM_FLAG_ENABLED`, `HAIR_SIM_FLAG_RESET`) and its simulation level: every shader reads
  whether the instance is simulated from its row.
- An instance owns one `HairGuidePoint` row for every point of every guide of its groom, whether or not the simulation
  is on and whichever guides it simulates (135,168 rows for a groom of 4,096 guides at 33 points): the persistent
  per-instance state. A guide's first row also holds its simulation LOD state (`guideState`, `guideBlend`). A guide keeps its
  cluster's stored vertex level, so guides differ in point count; `Hair.GuidePointRanges` gives each guide's first row
  and point count, by rank. An instance whose guides do not fit is refused at `hair.attach`.
- All uploads go through the root-binding pass's own upload on the render thread
  (`RenderGraphPass::UploadBufferData`), in the pass's `Update`. Nothing uses a deferred resize. That `Update`
  (`HairRuntime::UpdateFrame`) is the plugin's CPU work for the frame: it also uploads the LOD controls
  (`HairFrameGPU`), zeroes the frame's counters and decides the frame's plan (`HairFramePlan`), which every hair pass
  records from.
- `FindSceneEntityRenderState` ([`Extensions/SceneEntityRenderState.h`](../BasicRenderer/include/BasicRenderer/Extensions/SceneEntityRenderState.h))
  is the renderer extension point this needed: it gives a scene entity's per-object row and skinning slot. The
  per-object buffer is versioned, and a frame can bind a version a few frames behind the scene, so the pass takes the
  frame's published version (`PublishedObjectBufferState`) and skips an instance whose row that version does not
  have yet.

### Passes

| Pass | Where | Shader |
|---|---|---|
| `Hair::RootBinding` | Before `CLodOpaque::HierarchicalCullingPass1` | [`hairRootBinding.hlsl`](../Plugins/Hair/shaders/hairRootBinding.hlsl) |
| `Hair::Simulate` | After it | [`hairSimulate.hlsl`](../Plugins/Hair/shaders/hairSimulate.hlsl) |
| `Hair::GuideLevels` | After it | [`hairGuideLevels.hlsl`](../Plugins/Hair/shaders/hairGuideLevels.hlsl) |
| `Hair::Select` | After it | `HairSelectCS` in [`hairSelectEvaluate.hlsl`](../Plugins/Hair/shaders/hairSelectEvaluate.hlsl) |
| `Hair::Evaluate` | After it | `HairEvaluateCS`, same file |
| `Hair::Raster` | After `MaterialHistogramPass`, before `EvaluateMaterialGroupsPass` | [`hairRaster.hlsl`](../Plugins/Hair/shaders/hairRaster.hlsl) |
| `Hair::Resolve` | After `EvaluateMaterialGroupsPass` and `CLodOpaque::LinearDepthCopyPass2`, before `CLodOpaque::LinearDepthDownsamplePass2` | `HairResolveCS` in [`hairResolve.hlsl`](../Plugins/Hair/shaders/hairResolve.hlsl) |
| `Hair::Probe` | After `Hair::Resolve` | `HairProbeCS`, same file |
| `Hair::DebugOverlay` | Overlay epoch, after `TonemappingPass`, before the present pass | [`hairDebugOverlay.hlsl`](../Plugins/Hair/shaders/hairDebugOverlay.hlsl) |

- The first five need the roots and the main view's camera and nothing of the frame's opaque geometry, so they run
  before culling.
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

Each pass declares the plugin's buffers, which appear with the first groom, and declares again when the visibility
target is replaced. Until then it records nothing.

### The root-binding pass

`Hair::RootBinding` dispatches
[`hairRootBinding.hlsl`](../Plugins/Hair/shaders/hairRootBinding.hlsl) once per active instance, one thread per root,
and writes a `HairRootFramePair` per root: the root's position, normal and tangent **in world space**, for this frame
and for the previous frame's matrices (the previous bone palette and the previous model matrix).

```
position = restPosition * skin * model          (row vectors, as the renderer skins a vertex)
normal   = normalize(restNormal * skin3x3 * model3x3), tangent likewise
```

`skin` is the identity in `rigid` mode. The pass does not follow skeleton LOD remaps (procedural wind's transient
palettes). Normals use the matrices' upper 3x3 as they are, so a non-uniform scale skews them.

Each dispatch also adds one to `Hair.Counters[0]`, and each row records the frame number it was written in
(`writtenFrame`).

### Selection

`Hair::Select` dispatches once per active instance, one thread per cluster, for the main view only. A cluster:

1. **Follows its first strand's root.** Cluster bounds are in rest space. They are moved as the cluster's strands are
   ([Evaluation](#evaluation-and-the-strand-pool)), using the root frame of the cluster's first strand and the entity's
   uniform scale. That is exact while every root of the instance moves with one matrix (`rigid` and `bone`); with a
   skin binding per root it is approximate.
2. **Is frustum-tested** as a sphere against the camera's six view-space planes, like CLod's culling. Bounds cover
   strand centrelines at rest, so the radius is grown by 25% for strands that will move under simulation
   (`HAIR_CULL_RADIUS_MARGIN`) and by 1 cm for strand width (`HAIR_CULL_WIDTH_MARGIN`). There is no occlusion test.
3. **Gets its detail level** `l = clamp(spacingPixels / errorPixels, minDetail, 1)` (`HairLodDetailLevel`). The spacing
   is `1 / sqrt(density)` from the cluster's aggregate (strands per cross-section area), or from the bounds when the
   aggregate has no area. It is projected as CLod projects geometric error: by view depth, not distance, at the near
   side of the bounds, with the same pixels-per-unit factor (`projection[1][1] * 0.5 * renderHeight`).
4. **Gets its drawn strand count** from `l` by the rule above, and the weight of its last drawn strand
   (`HairLodBoundaryWeight`): the fraction of a strand that `l * (N + d)` has over the strands before it.
5. **Gets its drawn vertex level.** The fractional level is `f = log2(lengthPixels / segmentPixels)`, clamped to
   `[0, k]`, where the length is that of the aggregate's centreline. The drawn level is `ceil(f)` and the geomorph is
   `ceil(f) - f`. The centreline of a curly cluster is shorter than its strands, so curls drop levels earlier than their
   arc length would.
6. **Is appended** to the visible-cluster list and takes its range of the strand pool, both with an atomic counter.

A cluster that passes the frustum test also sets its instance's bit in the counter buffer, which is where
`instances_visible` comes from.

A `HairVisibleCluster` row holds the instance, the cluster, the drawn strand count, the pool range, `l`, the boundary
weight, the vertex level and the geomorph. Neither allocator gives anything back: when the pool or the list is full,
that cluster and every later one of the frame is dropped and counted in `overflow_clusters`.

The controls (`hair.lod`), with their defaults:

| Field | Default | Meaning |
|---|---|---|
| `error_pixels` | 1 | A cluster draws every strand while its strands are at least this far apart on screen. |
| `max_width_scale` | 8 | `wMax`: the most a drawn strand is widened. |
| `min_detail` | 0.02 | The lowest `l`. |
| `force_detail` | 0 | Above 0, every cluster takes this `l`. For testing. |
| `segment_pixels` | 4 | The length on screen a drawn segment aims for. |
| `freeze` | false | Keep the last selection while the camera moves. The select pass does not run; the kept strands are still evaluated and rasterized from the current camera. A change in the active instances selects once more. |

### Evaluation and the strand pool

`Hair::Evaluate` runs one thread group per visible cluster, one thread per drawn strand. The CPU does not know how
many clusters are visible, so the dispatch covers every cluster of the active instances and the extra groups return at
once. A thread writes its strand's `2^level + 1` drawn vertices as `HairPoolVertex` rows: position and width, in world
space, for this frame's root frame and for the previous frame's.

Without simulation:

- **Rigid following.** A rest-space point keeps its offset from the strand's root, expressed in the root's frame
  (tangent, bitangent, normal): `current root position + R * (point - rest root position) * scale`. `R` takes the rest
  frame in `HairRootBinding` to the frame in `Hair.RootFrames`; `scale` is the entity's uniform scale, taken from the
  model matrix because root frames are unit vectors. The previous position uses the previous root frame and the
  previous model matrix.
- **Vertex LOD.** Drawn vertex `j` is stored point `j << (k - level)`. Odd vertices are the ones the level below
  drops: they move to the midpoint of their two neighbours by the geomorph, and are gone when the level steps down.
- **Width.** The stored diameter times `min(1 / l, wMax)`, and for the cluster's last drawn strand also times the
  boundary weight, so a strand grows from nothing as `l` rises.
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

**State.** `HairGuidePoint`, in world space, so that root motion gives the hair inertia: the point's position, where
the last substep started and how long that substep was (velocity is the difference over it), the simulated position at
the end of the previous frame, and the point's rigid pose under this frame's root frame and under the previous
frame's. A guide starts on its rigid pose, at rest, when its instance is first simulated, when the instance was not
simulated in the frame before (not drawn, or the simulation was off), on `hair.sim {reset: true}`, and when its root
moved further than `teleport_distance` in one frame.

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
| `guides`, `reset_guides` | Guides simulated (those being demoted included), and those put back on the rigid pose in that frame. `guides` is 0 for an instance the simulation level has turned off. |
| `stretch_max`, `stretch_mean`, `stretched_guides` | `abs(length / rest length - 1)` of a guide segment: the largest, the mean over guides of each guide's mean, and the guides with a segment over 2%. |
| `root_shift` | Metres: the furthest a guide's root was, when the frame began, from the frame's previous root frame. See below. |
| `speed_max`, `speed_mean` | m/s: the fastest point, and the mean over guides of each guide's mean point speed. |
| `deviation_mean` | Metres from the rigid pose, the mean over guides of each guide's mean. |
| `collider_distance` | The smallest signed distance from a point to a collider's surface, metres; absent without colliders. At rest on a collider it is the margin. |
| `guide_roots`, `guide_root_error` | Drawn strands that are guides, simulated or not, and how far the evaluation left any of their first vertices from its root frame before it was put there. |
| `strand_roots`, `strand_root_error` | The same for drawn strands interpolated from guides. |
| `sim_level`, `target_level`, `coverage` | The instance's simulation level in that frame, what it is moving toward, and the coverage that was computed from (0 when the entity's matrix was not on the CPU). |
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

How many guides an instance simulates follows how large its groom is on screen. The guide-capable set is in levels
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

- **Rate.** `sFrac` moves toward what the coverage asks for by at most `lod_rate` (2) levels per simulated second.
- **Staying put.** `sFrac` sets off only once its target is more than 0.02 levels away, and then goes all the way; an
  instance that is not simulated starts only when its coverage asks for a level of 0.25 or more. A groom at a steady
  distance therefore does not flicker between states.
- **Starting.** An instance starts at its target level at once, with every guide on its rigid pose and at rest in
  world space, which is where its strands were. Its guides do not take up the entity's velocity.
- **Stopping.** Once `sFrac` is 0 the instance stays simulated for `demote_seconds` and two more steps, while its
  level 0 guides blend back onto the rigid pose (below), and then stops.
- **Forced levels.** `lod_force_level`, and `hair.simlevel` for one instance, apply at once, with no rate limit.
- **Budget.** With `lod_point_budget` above 0, when the levels asked for would simulate more guide points than that
  over all instances, every instance's level is multiplied by one factor, found by bisection over each groom's sorted
  table of the level every guide is simulated from. A level is never scaled below level 0 alone, so the budget does not
  turn an instance off and cannot be met where the level 0 guides alone pass it.
- The coverage uses the entity's matrix from the per-object buffer the frame binds, not an attach bone's. Without that
  buffer's rows on the CPU the instance simulates every guide.

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

**Which guides the passes cover.** The guide-level pass covers every guide of a simulated instance: a thread whose
guide is simulated and in no blend returns after reading three rows. The simulate pass covers an instance's guide
levels 0 to `floor(h)`, where `h` is the highest `sFrac` the instance has had in the last `demote_seconds` (and a few
steps): a guide of level `k` is simulated from `sFrac = k + randomOffset`, and one the level has stopped asking for is
still simulated for `demote_seconds`. In the frame an instance starts or is reset it covers every guide, because that
frame writes every guide's state. Inside the covered levels a thread whose guide is not simulated returns after reading
four rows (the instance, the guide's binding, its point range, its first point).

Both passes used to be dispatched per instance (the guide-level pass per instance and level, with a barrier each). With
a hundred grooms that cost far more than the work: [POC measurements](#poc-measurements) has the numbers before and
after.

**Limits.**

- **The simulate pass's time has a floor.** A guide is one thread running a long serial loop, so the dispatch takes
  about as long as its slowest thread group however few groups have work. Reducing the level therefore reduces the
  pass's time by less than the guides it drops. The pass is one dispatch for every instance, so the floor is paid once
  a frame and not once per groom ([POC measurements](#poc-measurements)).
- **The guide-level pass follows the scene's simulated grooms, not the simulated guides.** It evaluates every guide of
  every simulated instance that the solver does not own, whether or not a drawn strand uses it.
- **The simulation level follows distance, not visibility.** A groom behind the camera is simulated like one in front.
- Evaluated guides are not collided: a guide between parents on either side of a collider can pass through it. A
  promoted guide comes out of it over its blend.
- When `sFrac` reaches 0 with the groom still large on screen (a forced level), the hair moves from its simulated shape
  to the rigid one in `demote_seconds`: a quick, visible settle, with no jump. At the default off coverage the groom is
  a few dozen pixels tall.
- Parents are strictly in the level below, with inverse-distance weights, and the chord's rotation carries no twist.
- One level per instance, from the main view only. A groom simulated for a shadow or a reflection has no say.

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

### The rasterizer

`Hair::Raster` runs one thread per pool row. A row that starts a segment draws it:

- Both ends are projected with the main camera's view-projection, the one the opaque geometry was rasterized with. A
  segment behind the near plane is dropped; one that crosses it is clipped.
- The segment is a screen-space trapezoid with round ends: a pixel is covered when its centre is within the segment's
  half-width of the nearest point on the centreline. The half-width is interpolated between the two ends and is at
  least half a pixel, so every strand is at least one pixel wide and has no gaps.
- Pixels are walked along the segment's longer screen axis, and in each column only those near the centreline are
  tested. A segment tests at most 4,096 pixels (`HAIR_RASTER_MAX_PIXELS_PER_SEGMENT`); one that needs more is cut short
  and counted in `segments_capped`.
- A covered pixel's depth (perspective-correct along the segment) is compared with the opaque visibility buffer's
  depth field at that pixel. Hair at or behind the opaque surface is rejected and counted in `pixels_occluded`.

This is the simple form only: no tiles, no binning, no coverage and no anti-aliasing. The main view is assumed to be a
perspective view (clip `w` is the linear depth).

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
deferred shading looks them up at the hair pixel's position. Hair casts none: it is in no shadow view.

**Cost.** `Hair.Counters` row 16 counts the pixels resolved in a frame (`pixels_resolved`).

### Probing the targets

`hair.probe` runs `Hair::Probe` for one frame, after the resolve. For one pixel in four each way that holds hair it
copies what the frame's targets hold there into `Hair.Probe`: the key's depth, the linear depth, the projected depth,
the surface device depth, the motion vector, the length of the stored normal and the roughness, behind a header with
the frame's view and unjittered projection matrices and the previous frame's. The runtime reads the buffer back after
the frame and writes it to the file as it is; `HairRuntimeGPUTypes.h` gives the layout (`HAIR_PROBE_*`), and
`hair.stats` reports `probe.state` and, as `probe.width`, the rows written. A script that knows how the scene moved can
then check the motion vectors and the depths ([Checking it](#checking-it)).

### Dumping buffers

`hair.dump` writes one of the plugin's buffers as the next frame leaves it to a binary file: the frame number (8
bytes), then the buffer's rows, or its first `rows`. `pool_vertices` is the strand pool (`HairPoolVertex`),
`visible_clusters` the frame's selection (`HairVisibleCluster`) and `guide_points` the guide state (`HairGuidePoint`).
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
| `off` | The pass records nothing. |

`hair.capture` writes a presented frame (with the overlay, when one is on, and the menu) to a 32-bit `.bmp`. It reads the frame's
presentation colour texture back, so it needs no visible window and works with the display asleep or the session
locked, where a screen capture is black. The file is written a few frames later: hold the view still, and poll
`capture.state` in `hair.stats` (`pending`, `written` or `failed`).

### Shaders

The shaders are in [`Plugins/Hair/shaders/`](../Plugins/Hair/shaders). The build stages them into `HairShaders/` in
the renderer's runtime root (beside `shaders/`), with `HairGPUTypes.h`, `HairRuntimeGPUTypes.h` and `HairLodShared.h` in
`HairShaders/Hair/`; they are compiled at run time like the renderer's. `hairStructs.hlsli` includes the three headers,
and `hairCommon.hlsli` adds what the simulate, guide-level, select, evaluate, raster, resolve and debug shaders share (the
main camera, root following, point decoding, the visibility key, which guides are simulated and how they blend), and
`hairGuideSkin.hlsli` how a point follows a guide, for the evaluate and guide-level shaders.
A shader includes it by its path from the runtime root (`"HairShaders/hairStructs.hlsli"`): the main file is
compiled from memory, so it has no folder of its own to search. `scripts/deploy_demo.ps1` copies the folder.

`HairGPUTypes.h` compiles under DXC as it was written: the schema version is unchanged.

### Checking it

`hair.stats` reports:

| Field | Meaning |
|---|---|
| `assets`, `instances`, `attached_instances`, `active_instances`, `failed_instances` | Counts. An active instance was dispatched in the last frame. |
| `strands`, `points`, `rows` | Rows used and their capacities, per pooled buffer. |
| `buffers_created`, `buffer_bytes` | The pooled buffers and their size. |
| `asset_bytes_uploaded`, `instance_bytes_uploaded`, `bytes_uploaded`, `pending_upload_assets` | Upload totals. |
| `frames`, `root_binding_dispatches` | Frames the pass was updated in, and the dispatches those frames asked for. |
| `gpu_counter` | The last readback of the GPU's own dispatch count, with the CPU count and frame number of the frame it was read after. Each call asks for the next readback, so two calls a few frames apart see a fresh one. |
| `gpu_frame` | The same readback's per-frame counters, below. |
| `gpu_history` | With `continuous` on: the counters of the last 32 frames read back, oldest first. A frame's counters arrive a few frames after it, so a caller that polls once a frame sees every frame. |
| `lod`, `debug_view`, `visibility_target`, `capture`, `probe`, `dumps` | The LOD controls, the debug view, the target's size, the last `hair.capture`, the last `hair.probe` and the `hair.dump` counts. |
| `sim` | What `hair.sim` returns: the controls, the step counters and each instance's metrics ([Simulation](#simulation)). |
| `self_check` | The self-check's last result and its worst since it was enabled. |
| `instance_list` | Each instance's entity, bone, state, mode, material and error. |

`gpu_frame` and each row of `gpu_history`:

| Field | Meaning |
|---|---|
| `frame`, `camera_position`, `selected` | The frame, its main camera position, and whether it ran the select pass (false: frozen). |
| `clusters_tested`, `clusters_visible` | Clusters the select pass looked at and kept. |
| `instances_visible` | Instances with at least one cluster inside the frustum. |
| `strands_selected`, `segments_selected` | Drawn strands and segments of the kept clusters. |
| `pool_vertices`, `visible_clusters` | Rows used and capacity of the strand pool and the visible-cluster list. |
| `overflow_clusters` | Visible clusters dropped because the pool or the list was full. |
| `segments_drawn` | Segments that wrote at least one pixel. |
| `pixels_written` | Visibility writes. A pixel under several segments counts once for each. |
| `pixels_occluded` | Covered pixels rejected by the opaque depth. |
| `segments_capped` | Segments cut short at the per-segment pixel limit. |
| `pixels_resolved` | Hair pixels the resolve wrote into the surface targets: at most `pixels_written`. |

While the LOD is frozen the selection counters are those of the frozen selection. The counters are zeroed by an upload
at the start of the frame and read back after it.

The **self-check** (`hair.selfcheck`, off by default) reads `Hair.RootFrames` back every fourth frame and compares a
few roots per instance with the same arithmetic on the CPU. The CPU's inputs are the root bindings, the entity's row
of the per-object buffer version the frame bound, and the skin matrices of the entity's pose (inverse bind times
pose, this frame's and the previous frame's). It logs each comparison (`Hair self-check: frame ...`) and reports the
largest position and direction differences, how far the roots moved between the two frames, and how many instances
it could not compare because their rows were not written in that frame. Each check copies the whole buffer.

[`Scripting/Scenarios/hair/hair_single.cs`](../Scripting/Scenarios/hair/hair_single.cs) is the end-to-end check of
attachment: a
drooping procedural groom on a moving sphere and a tuft on the swinging `tip` joint of `assets/skinned_bar.glb`, both
drawn lit. It passes when both grooms are uploaded, the GPU ran exactly two dispatches per frame over 65 frames, the
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
front with fewer strands than the scene has, nothing is selected or drawn facing away, every groom is in view from the
far camera with under half the scene's strands drawn and none simulated, and the strand pool and the visible-cluster
list never overflow. It writes three captures (near, middle of the fly-through, far).

```powershell
BasicRenderer.exe --script scenarios\hair\hair_single.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_lod_sweep.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_sim.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_sim_lod.cs --script-exit --script-fixed-dt 0.0166667
BasicRenderer.exe --script scenarios\hair\hair_crowd.cs --script-exit --script-fixed-dt 0.0166667
```

`hair_sim_lod.cs` writes about 60 captures of 16 MB and, for a while, up to 400 MB of dumps.

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

In order of what they cost the claims under test:

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
`GroomSource` tests, `GroomBuilderTests.cpp` the builder, LOD and `.groom` tests, and `AlembicGroomTests.cpp` the
importer tests, which write their own archives with `OCurves`. Temporary files go to the system temp directory.
