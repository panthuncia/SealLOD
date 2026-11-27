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
  strands into a pool, rasterizes them into a hair visibility target, and resolves that target into the frame's surface
  data. The demo (`BasicRendererDemo`) registers it; SARP's renderer host does not.

**Hair is drawn as an ordinary opaque surface with a placeholder material.** The [resolve](#the-resolve) gives every
hair pixel a surface record, depth and a motion vector, so deferred shading lights it, shadows fall on it and the
upscaler follows it. There is no hair BSDF and hair casts no shadow. A strand thinner than a pixel is drawn into a
one-pixel footprint and written there with the probability of its coverage ([The rasterizer](#the-rasterizer)), so
that over the frames the upscaler integrates it has its true weight; a pixel still holds one opaque sample, hair or
not. Strands follow their roots rigidly unless simulation is turned on (`hair.sim`):
then the groom's guides are simulated on the GPU, collide with spheres and capsules, and the other strands follow them.
How many guides are simulated follows how large the groom is on screen, and a groom the view does not show is not
simulated ([Simulation LOD](#simulation-lod)).

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
| `hair.attach` | `asset`, `entity` (a scene entity's name), optional `bone`, optional `color` (`[r, g, b]`, linear) and `roughness` | `instance` |
| `hair.clear` | | Drops every instance and asset. |
| `hair.selfcheck` | `enabled` (default true), `roots` per instance (default 8) | `enabled` |
| `hair.lod` | Any of `error_pixels`, `max_width_scale`, `min_detail`, `force_detail`, `segment_pixels`, `freeze`, `detail_exponent`, `coverage` (`off`, `white` or `interleaved`; `true` and `false` are `white` and `off`), `coverage_accumulate` | Every value. See [Selection](#selection) and [The rasterizer](#the-rasterizer). |
| `hair.debug` | `view`: `off`, `cluster`, `detail`, `strand`, `rank`, `vertex_level` or `coverage` | `view`. See [Debug view](#debug-view). |
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
- **Material.** `color` and `roughness` are the instance's [placeholder material](#the-resolve): by default a dark
  brown, `[0.09, 0.05, 0.03]`, at roughness 0.6. A `.hair` file's colours are not used.
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

The runtime makes twenty-two pooled buffers when the first groom is loaded, at fixed sizes (`HairCapacities`, 417 MB
at the defaults), and never resizes them. A groom that does not fit the asset buffers, or an instance past the
instance rows, is refused with an error that names the row count and the capacity. Three kinds of buffer are shared
out at run time and can run short without anything being refused: `Hair.VisibleClusters` and the two pool buffers hold
one frame's selection and are filled on the GPU (what does not fit is dropped for the frame and counted,
[Selection](#selection)), and `Hair.GuidePoints` is lent to the instances that are simulated (an instance that gets
none follows its roots rigidly and is counted, [Guide rows](#guide-rows)).

| Buffer | Row | Default rows | Written |
|---|---|---|---|
| `Hair.Clusters`, `Hair.ClusterAggregates` | `HairClusterHeader`, `HairClusterAggregate` | 16,384 | once per asset |
| `Hair.Strands`, `Hair.RootBindings` | `HairStrandRecord`, `HairRootBinding` | 262,144 | once per asset |
| `Hair.GuideBindings`, `Hair.GuideStrandIndices` | `HairGuideBinding`, `uint` | 65,536 | once per asset |
| `Hair.Points` | `HairPackedPoint` | 8,388,608 | once per asset |
| `Hair.Instances` | `HairInstanceGPU` | 1,024 | when an instance's row changes |
| `Hair.FrameState` | `HairFrameGPU` | 1 | every frame with an active instance |
| `Hair.Counters` | `uint` | 49 | by the select, evaluate, raster and resolve passes; reset by an upload every frame |
| `Hair.VisibleClusters` | `HairVisibleCluster` | 65,535 | by the select pass |
| `Hair.PoolVertices` | `HairPoolVertex` | 4,194,304 | by the evaluate pass |
| `Hair.PoolSegmentIds` | `uint` | 4,194,304 | by the evaluate pass |
| `Hair.Probe` | `uint` | 1,310,792 | by the probe pass, in a frame `hair.probe` asked for |
| `Hair.GuidePointRanges` | `HairGuidePointRange` | 65,536 | once per asset |
| `Hair.GuidePoints` | `HairGuidePoint` | 2,097,152 | by the simulate and guide-level passes: the rows of the instances that are simulated |
| `Hair.SimGroups` | `HairSimGroup` | 131,072 | every frame an instance is simulated: the thread groups of the simulate and guide-level passes |
| `Hair.Colliders` | `HairColliderGPU` | 8,192 (8 per instance) | when an instance's colliders change |
| `Hair.SimFrame` | `HairSimFrameGPU` | 1 | every frame |
| `Hair.SimMetrics` | `uint` | 32,768 (32 per instance) | by the simulate, guide-level and evaluate passes; reset by an upload every frame the simulation is on |
| `Hair.RootCheckList`, `Hair.RootCheck` | `HairRootCheckSample`, `HairRootFramePair` | 4,096 | the self-check's: an upload and the root-check pass, in a frame that samples |

There are also two textures, made at the render resolution of the first frame and made again when that changes:
`Hair.Visibility`, the [hair visibility target](#the-hair-visibility-target), and `Hair.Coverage`, the
[coverage target](#the-coverage-target).

Where the 417 MB are: guide state 160 MB, the strand pool 128 MB and its segment ids 16 MB, the groom points 64 MB,
strands and root bindings 32 MB, and 1 MB to 5 MB each for the rest. `hair.stats` reports every buffer's capacity,
what is in use and the most that was ([Checking it](#checking-it)).

- Assets sit back to back in the groom buffers. Indices inside an asset stay groom-relative (`strandOffset`,
  `pointOffset`, guide strand indices): a shader adds the instance's bases.
- [`HairInstanceGPU`](../Plugins/Hair/include/Hair/HairRuntimeGPUTypes.h) holds an instance's bases and counts and
  three references into the renderer's own buffers:
  its entity's row of `Builtin::PerObjectBuffer` (model and previous model matrix), its skinning instance slot
  (`SkinningInstanceInfo`, which locates the current and previous bone palettes in `BoneTransforms`) and the attach
  bone. The entity's transform is not copied: the shader reads the row the entity's mesh is drawn with. It also holds
  the instance's placeholder material (base colour and roughness), its first row of guide state
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
| `Hair::Raster` | After `MaterialHistogramPass`, before `EvaluateMaterialGroupsPass` | [`hairRaster.hlsl`](../Plugins/Hair/shaders/hairRaster.hlsl) |
| `Hair::Resolve` | After `EvaluateMaterialGroupsPass` and `CLodOpaque::LinearDepthCopyPass2`, before `CLodOpaque::LinearDepthDownsamplePass2` | `HairResolveCS` in [`hairResolve.hlsl`](../Plugins/Hair/shaders/hairResolve.hlsl) |
| `Hair::Probe` | After `Hair::Resolve` | `HairProbeCS`, same file |
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
of its height that are on screen). 0 means no view shows any of it. There is one view, the main camera: hair casts no
shadow and is in no reflection. A second view would be a second `HairView` in the frame's inputs and the larger of the
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

This is the simple form only: no tiles, no binning, and no anti-aliasing of the edges of a strand wider than a pixel.
The main view is assumed to be a perspective view (clip `w` is the linear depth).

### The coverage target

`Hair.Coverage` is an `R32_UInt` texture the size of the visibility target. With `coverage_accumulate` on, the
rasterizer adds (`InterlockedAdd`) the coverage of every footprint pixel that is in front of the opaque surface, written
or not, in units of 1/65536 (`HAIR_COVERAGE_UNIT`): the summed coverage of the hair over each pixel, overlaps counted,
in the manner of the additive coverage layer of *Indiana Jones and the Great Circle*. It is cleared every frame. Nothing
shades from it yet: it is for the `coverage` [debug view](#debug-view), for measurement, and for a later composite. The
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
| `strands`, `points`, `rows` | Rows used and their capacities, of the asset buffers, the instance rows and the guide state lent out. `instance_strands` is the strands summed over instances. |
| `buffers_created`, `buffer_bytes` | The pooled buffers and their size. |
| `memory` | `buffers`: for every pooled buffer its `row_bytes`, and capacity, use now and high-water mark in rows and bytes. `capacity_bytes`, `used_bytes` and `high_water_bytes` are the sums (the last is the sum of each buffer's own high-water mark: an upper bound on the most in use at once). For the buffers a frame fills on the GPU (the visible-cluster list and the strand pool) use is that of the newest frame read back, and the high-water mark is over the frames read back: every frame with `continuous` on. `guide_state`: the lending of guide rows, below. |
| `asset_bytes_uploaded`, `instance_bytes_uploaded`, `bytes_uploaded`, `pending_upload_assets` | Upload totals. |
| `frames`, `evaluate_dispatches` | Frames the plugin was updated in, and the evaluate dispatches those frames asked for. |
| `gpu_counter` | The last readback of the GPU's own count of evaluate dispatches, with the CPU count and frame number of the frame it was read after. Each call asks for the next readback, so two calls a few frames apart see a fresh one. |
| `gpu_frame` | The same readback's per-frame counters, below. |
| `gpu_history` | With `continuous` on: the counters of the last 32 frames read back, oldest first. A frame's counters arrive a few frames after it, so a caller that polls once a frame sees every frame. |
| `lod`, `debug_view`, `visibility_target`, `capture`, `probe`, `dumps` | The LOD controls, the debug view, the target's size, the last `hair.capture`, the last `hair.probe` and the `hair.dump` counts. |
| `sim` | What `hair.sim` returns: the controls, the step counters and each instance's metrics ([Simulation](#simulation)). |
| `self_check` | The self-check's last result and its worst since it was enabled. |
| `instance_list` | Each instance's entity, bone, state, mode, material and error, its `strands`, the guide rows it needs to be simulated (`guide_rows`) and its first row while it holds any (`guide_row_base`, -1 otherwise). |

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
| `pixels_resolved` | Hair pixels the resolve wrote into the surface targets: at most `pixels_written`. |

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

As measured for the proof of concept, before partial coverage. Points 3 and 5 are what
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
