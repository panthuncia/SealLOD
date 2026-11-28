# Lighting

How BasicRenderer evaluates analytic lights (point, spot, directional), how they are shadowed, and where a new
shadowing technique plugs in, plus screen-space ambient occlusion. Image-based lighting and the environment are covered
only where they meet this.

## Light data

A light is one `LightInfo` row (`include/BasicRenderer/Extensions/ShaderBuffers.h`, mirrored in
`shaders/Include/structs.hlsli`; 112 bytes, checked by a `static_assert`). `LightManager`
(`src/Lighting/Lights/LightManager.cpp`) owns the light buffer and the sorted active-index list, and publishes three
tables through the state graph, in `LightManager::CaptureTableImages` order:

| Variant (`LightStateArtifacts.h`) | Contents |
|---|---|
| `LightInfoTableVariant` | `LightInfo` rows, indexed by light buffer index. Rows of removed lights are reused. |
| `LightDirectionalViewTableVariant` | Camera buffer indices of each shadow-mapped directional light's clipmap views. |
| `LightActiveIndexTableVariant` | The light buffer indices in use, sorted. Shaders loop over this. |

Fields worth knowing:

- `range`, `sourceRadius`: the point/spot falloff (below). `Scene::CreatePointLightECS` and `CreateSpotLightECS` take
  both; a range of zero picks the distance where `intensity / d^2` falls to 0.3.
- `sourceHalfAngleDegrees`: a directional light's angular radius. For a shadow-casting sun, `LightManager` writes the
  virtual shadow map's source-angle setting here every frame, so the shadow map, the path tracer, the ray traced sun
  shadow and the lens flares all use one sun size. Unset (0), consumers that need a disc use
  `LIGHT_DEFAULT_SUN_HALF_ANGLE_DEGREES` (0.27°); the shadow map treats 0 as a hard shadow.
- `generation`: bumped by `LightManager` each time a slot is given to a light, and stamped on every upload. A slot index
  kept across frames (a temporal reservoir, say) is still the same light only if the generation matches.
- `shadowCaster`: a directional shadow caster gets the shadow map's clipmaps. Point and spot shadow casters compete for
  the shadow map's local faces ("Spot and point light shadows" below).
- `shadowViewInfoIndex`: for a spot or point light with faces, its first page-table slice; -1 without.

### The sun

`PerFrameCB::sunLightIndex` (light buffer index) and `sunLightSlot` (position in the active list) name the sun: the
first shadow-casting directional light by buffer index, else the first directional one; both are `LIGHT_INDEX_NONE`
without one (`LightManager::GetSunLight`). Shaders use these instead of scanning the light list.

## The light model

`shaders/Include/lightModel.hlsli` is the one definition of what a light delivers to a point:

- Point and spot lights fall off as inverse square, bounded below by `sourceRadius` (or a 1 cm floor) and windowed by
  `(1 - (d/range)^4)^2`, so the light is finite everywhere and reaches zero at `range` without a step.
- Spot lights fade between `outerConeCos` and `innerConeCos` with a smoothstep.
- `EvaluateLightAtPoint` returns direction, distance, falloff and cone; `LightIncidentRadiance` is
  `color * intensity * falloff * cone`.
- `LightSampleConeDirection` draws a direction from a directional light's disc.

Raster's light loop, the path tracer (`pathTracingLights.hlsli`), the ray traced sun shadow, ray traced reflections and
the lens flare sources all evaluate lights through it. A change to falloff or cone belongs here and nowhere else.

## The light loop

`shaders/Include/lighting.hlsli` has one direct-light loop, `EvaluateDirectLighting`. With
`PSO_CLUSTERED_LIGHTING` it walks the fragment's froxel in the light grid (below), a contiguous run of light buffer
indices; otherwise it walks the whole active list. Per light it evaluates occlusion, the light model and
the OpenPBR BRDF (`EvaluateDirectLightContribution`), and accumulates:

- `lighting`: the sum raster writes.
- `diffuse`, `specular`: the same contribution split by lobe (glint counts as specular). The compiler drops them
  where unused.

`lightFragment` (deferred, with debug outputs) and `lightFragmentColor` (forward and ray-hit paths) wrap it with
image-based lighting and emission.

One loop is not this one: strand hair's layered path lights strand vertices in `Plugins/Hair/shaders/hairShade.hlsl`
with a loop of its own, because a fibre has no surface normal and takes light from behind. It is built from the same
pieces (`EvaluateLightAtPoint`, `LightIncidentRadiance`, `EvaluateLightOcclusion`, and optionally the light grid's forward
runs), so the light model and the shadow lookup stay single; see [hair.md](hair.md#the-layered-path).

### Visibility is the caller's to state

The loop reads no root constants. Callers pass `DirectLightingOptions`:

| Field | Meaning |
|---|---|
| `punctualLights` | False skips every analytic light. |
| `virtualShadowMaps` | Look up occlusion in the virtual shadow map, for the sun and for spot and point lights with faces; its resources must be bound. |
| `sunOcclusion` | Sun occlusion known before the loop, combined with the shadow map's by max. |
| `sunLightIndex` | Which light `sunOcclusion` is for. |
| `stochasticLights` | Skip every analytic light: stochastic lighting shades them, and the caller adds its result. |

`DirectLightingOptionsFromRoot()` builds them from a lighting pass's root switches (`MiscEnablePunctualLights`,
`MiscShadowingMode`). A pass whose root constants mean something else in those slots must build its options with
`MakeDirectLightingOptions` instead. Ray traced reflections do this, tracing their own ray for the sun. Defining
`LIGHTING_NO_VIRTUAL_SHADOW_MAPS` compiles the shadow map lookup out, for passes that never bind its resources.

Current callers:

| Pass | Options |
|---|---|
| Deferred (`deferred.hlsl`) | From root, plus the ray traced sun mask as `sunOcclusion` when it runs. In `StochasticRayTraced`, no analytic light: it adds stochastic lighting's result. |
| AVBOIT shading, deep visibility resolve | From root. In `StochasticRayTraced` there is no shadow map, so every light is unshadowed here. |
| Ray traced reflections | Every light; the sun's occlusion from one ray; no shadow map. |

## The light grid

Every consumer of per-pixel light lists reads one structure, built each frame by three compute passes in
`shaders/lightCulling.hlsl` (`src/Lighting/Clustered/RenderPasses/LightCullingPass.h`, registered by
`BuildLightClusteringPipeline`).

**Shape.** Froxels are tiles of `lightGridTileSize` pixels square (default 32, a multiple of 16;
`SARP_LIGHT_GRID_TILE`) by `lightGridSlices` depth slices (default 24; `SARP_LIGHT_GRID_SLICES`): 4 uniform slices
to `PerFrameCB::clusterZSplitDepth` (6 m), logarithmic beyond. `PerFrameCB::lightClusterGridSize*` hold the counts and
`lightGridTileSize` the tile; `MakeLightGridShape` (`lightCullingRootConstants.h`) derives them from the render resolution.
Changing either setting rebuilds the graph.

**Passes.**

| Pass | Work |
|---|---|
| `LightGridPreparePass` | One 32-byte `LightCullingRecord` per active light (`Builtin::Light::CullingRecords`): its bounding sphere in view space and its light buffer index; directional lights get a negative radius, meaning "everywhere". Resets the depth bounds. |
| `LightGridDepthBoundsPass` | Each opaque pixel of `LinearDepthMap` widens its froxel's nearest and farthest depth, and its tile's farthest depth (`Builtin::Light::GridDepthBounds`), through group-shared bounds per 16×16 pixels. A background pixel makes the tile's farthest depth unbounded. |
| `LightCullingPass` | One thread group per 4×4×4 froxels, one thread per froxel. Each batch of 64 lights is tested against the union of the group's froxel boxes and compacted in group-shared memory; each froxel then tests the survivors against its own boxes. A first sweep counts, one wave-aggregated atomic per wave allocates each froxel's run in `Builtin::Light::GridIndices`, and a second sweep fills it, in active-list order. |

**Two runs per froxel.** `Builtin::Light::GridCells` holds a `uint2` per froxel: the run's first index, then two
16-bit counts.

- The *opaque* count covers the lights whose sphere touches the froxel's box between its nearest and farthest opaque
  pixel. Only the primary surface's own pixels may read it: deferred shading and stochastic lighting.
- The *forward* count covers the whole froxel in front of the tile's farthest opaque pixel (a pixel of slack for
  jittered fragments), with the opaque lights first. Forward passes (AVBOIT shading, deep visibility resolve) define
  `LIGHT_GRID_FORWARD 1` to read it: a transparent fragment can lie anywhere in front of the opaque surface. It is built
  only when the recipe has `ClusterLodAlphaTechnique`; otherwise it equals the opaque count.

Froxels behind every opaque pixel of their tile get no lights at all, and froxels with no opaque pixel get only forward
lights.

**Readers** use `shaders/Include/lightGrid.hlsli`: `LightGridLoadRange(pixel, viewZ, perFrame, camera)` returns
`{first, count, slice}` and `LightGridLight(range, i)` the i-th light buffer index. The deferred loop, stochastic
lighting's estimator and reference, the AVBOIT and deep visibility forward passes and the light cluster debug outputs
(12: the froxel's slice, 13: its light count) all go through it. The depth bounds pass computes a pixel's slice with the
same function from the same depth as its readers, and a pixel within 0.1% of a slice boundary also widens the
neighbouring slice, so a reader whose arithmetic rounds differently still lands in a froxel that holds its lights.

**Capacity.** Nothing is dropped unless `GridIndices` is full. `CullingStats` (`LIGHT_CULLING_STATS_*`) counts the
entries needed (the allocator), the pairs dropped, the froxels that dropped any, and the pairs stored;
`LightManager::PollCullingStats` reads it back every 30 frames, logs it every 300
(`Light culling: N (froxel, light) pairs dropped in M froxels, at most K in a sampled frame ...; at most E index entries
needed`) and warns the first time anything is dropped. `Renderer::Update` grows `lightGridIndexCapacity` (from 4M entries, 16 MB)
past the most needed so far, and `lightGridRecordCapacity` past the active light count, by doubling and a graph rebuild,
as the material resolved-texture table grows. A froxel that finds no room keeps its opaque lights first.

## Spot and point light shadows

Spot and point lights are shadowed by the same virtual shadow map as the sun. They share its physical page pool, page
caching, admission budget and raster passes. Each light gets perspective *faces*, following Unreal's local-light
virtual shadow maps.

### Which lights

`LightManager::UpdateLocalShadows` runs every frame. Candidates are spot and point lights with `shadowCaster` set.
`LocalShadowSelection.h` holds the policy:

- Each light is scored by its range over its distance from the camera (`LocalShadowScore`).
- The highest scores are kept until the face budget runs out (`SelectLocalShadowLights`). The budget is
  `clodLocalVirtualShadowMaxFaces`: 24 by default, 48 at most. A spot light costs one face and a point light six.
- A light that already has faces is scored 1.25 times higher, so two similar lights don't trade places every frame.
- A light that loses its faces is logged and its faces are released.

Faces are given in consecutive runs (`AllocateLocalShadowFaces`). This lets `LightInfo::shadowViewInfoIndex`, the
light's first slice, name all of its faces.

### Faces

A face is one culling view: a 16384² perspective map with 8 mips of 128-texel pages. Its near plane is 0.2% of the
light's range (at least 1 cm) and its far plane is the range.

- **Spot light:** one face down the light's axis. Its field of view is the cone plus 2°.
- **Spot wider than 60°:** shadowed as a cube, like a point light.
- **Point light:** six faces fixed to the world axes, in the order +X, −X, +Y, −Y, +Z, −Z. Each face sees 4 texels
  of its coarsest mip past 90° (`kLocalShadowCubeGuardTexels`). Receivers pick their face by the dominant axis of
  their direction from the light, so filter taps and the normal offset always stay inside that face's map.

Each page-table slice is a 16K canvas. Slices 0–21 are the sun's clips. Each local face owns two slices from 22 on:

- mip 0 fills the first slice;
- mips 1–7 sit in the second slice at block-aligned origins: (0,0), (64,0), (96,0), (112,0), (120,0), (124,0) and
  (124,4) pages.

`CLodVirtualShadowLocalMip*` in `clodVirtualShadowClipmap.hlsli` defines the layout. A face's slice infos carry the
Local flag, and the Cube flag on cube faces. Their `texelWorldSize` is mip 0's texel per unit of view depth.

### Pages

**Marking.** `CLodVirtualShadowMarkLocalFaceBlocks` runs per mark tile and requests pages for receivers within the
light's range.

- Each face's mip comes from the receiver's pixel footprint: `log2(pixel / (texelWorldSize · viewDepth)) + bias`,
  where the bias is `clodLocalVirtualShadowLodBias`. The lookup uses the same function, so marking and lookup pick the
  same mip.
- A tile's nearest and farthest samples are marked separately. Across a depth edge, one rectangle spanning both would
  request large areas at every mip in between.
- Only receivers request pages, as for the sun's clips. No page is requested for a fallback: a coarse page covers a
  large part of the face, and rendering it draws every caster there, including ones that shadow nothing on screen.

**Rendering.** Culling ORs the page activity of all of a face's mips. A cluster's visible-cluster records name one
mip and one block each; the payload's 5-bit field is `23 + mip`. The mesh shader maps the face's NDC into that mip's
rectangle on the canvas. **Every raster path must write every mip a cluster covers.** Otherwise pages cached while
geometry streams in stay empty in some mips, and which ones differs from run to run.

**Upgrades.** A cluster drawn in place of a non-resident refined group leaves a fallback candidate, and its pages are
rendered again once the group arrives. A local face's candidate records the coarsest mip whose threshold wants the
refined group (`CLodVirtualShadowPackCandidateSlice`); expansion re-renders only pages at that mip and finer, since
coarser pages already have the detail they need.

Upgrades are paced for the sun and local faces alike. A page rendered within the last
`clodVirtualShadowUpgradeCooldownFrames` frames (default 8, `SARP_CLOD_VSM_UPGRADE_COOLDOWN_FRAMES`) keeps its pending
upgrade but waits, so refinements that stream in meanwhile share one re-render; none are dropped. At most
`clodDirectionalVirtualShadowUpgradePageRenderBudget` upgraded pages render per frame (default 500,
`SARP_CLOD_VSM_UPGRADE_PAGE_BUDGET`). Both are in the menu with the page render budget. The menu caps the cooldown at
16: much longer, and pages wait long enough for their refined groups to be streamed out again, so they are re-requested
and re-rendered indefinitely.

**Level of detail.** A face's LOD threshold follows the sun's policy: it is set from the `shadowResolution` setting
(2048), not the 16K the face renders at, so caster detail is about 8 texels of tolerance, as for the sun's clips. A
threshold from the 16K resolution would ask for 8 times finer geometry than the camera streams, and the resulting
upgrades would re-render hundreds of pages a frame during loading.

Culling then divides a local face's projected error by 2^m, where m is the finest mip with active pages under the
bounds being tested (`CLodVirtualShadowLocalLodErrorScale`, set up by `CLodResolveLodViewId`). Geometry that only
renders into coarse pages is drawn at that mip's resolution. A parent's bounds contain its children's, so its mip is
never coarser and the LOD cut stays consistent.

**Empty pages.** A page counts as rendered when a fragment writes to it. The sun's receivers are its casters, so its
marked pages nearly always receive depth. A local page can be covered by casters' bounds without any of their
triangles (padding past a roof edge, or a finite-range texel that sees nothing). Without help, such a page would stay
admitted and be rasterized again every frame.

So the stamp kernel (`CLodVirtualShadowStampRenderedPageGenerationsCSMain`) completes an admitted page that received
no depth with cleared depth, but only in a frame whose shadow work all ran
(`CLodVirtualShadowFrameShadowWorkComplete`). Culling and raster count every record they drop:

- the replay state's node, meshlet and Reyes drop counters, plus `shadowWorkDropped` (full output buffers, cluster
  capacity clamps, pages not resident; `CLodNoteVirtualShadowWorkDropped`);
- the stats' `rasterWorkDroppedCount` (a cluster or voxel page that failed to load) and
  `blockExpandedDroppedRecordCount`;
- Reyes's split and dice overflow counters, where the variant resets them each frame.

If any is nonzero the page stays admitted and is tried again next frame. This applies to the sun's pages as well.

**Invalidation.** A face's pages are reset when its view-projection changes, meaning the light moved, or when its
dynamic-layer state flips. A moved caster invalidates the pages it covers in every mip of every face within range.
Skinned and wind-deformed casters render into a face's dynamic layer only when the light's sphere comes within the
wind's reach of the camera, as for the sun's clips. Elsewhere they are cached like rigid casters.

### Lookup

`EvaluateLightOcclusion` calls `CLodVirtualShadowLookupLocalOcclusion` (`shadows.hlsli`) for a spot or point light
with `shadowViewInfoIndex >= 0`. The lookup:

1. picks the face;
2. projects the receiver through that face's camera;
3. selects the mip from the pixel footprint;
4. walks to coarser mips until a page has content.

It offsets the receiver 1.5 texels along its normal, then compares a 2×2 bilinear-weighted footprint with a
one-texel depth bias. This is a hard shadow softened over one texel; `sourceRadius` does not widen it.

### Raster paths

| Path | Local faces |
|---|---|
| Hardware mesh | Every mip. |
| Voxels | Every mip (one rasterization pass per mip, with the mip's own scissor). |
| Software raster | Every mip. One record per cluster; the rasterizer divides by w, interpolates 1/depth, and walks the face's mips, clamping each to its rectangle. Meshlets crossing the near plane go to the hardware path. |
| Reyes | None. Displaced clusters render undisplaced through the hardware path. |
| SARP grass (caster provider) | None. |
| Strand hair (caster provider) | None. |

A caster provider (`IVirtualShadowCasterProvider`, `Extensions/VirtualShadowCasterProvider.h`) is offered the sun's
clipmaps and a depth write for them, not the faces. There are two: SARP's grass, a mesh-shader pass, and strand hair
(`Plugins/Hair`, [hair.md](hair.md#shadow-casting)), which rasterizes strands from compute with their coverage dithered
and tells in that section what a provider would need to draw into faces.

### Testing

`VirtualShadowBudgetTests` checks selection, face allocation, face counts, cone coverage, and that every direction
lands inside its cube face's guard band.

The demo takes `BASICRENDERER_DEMO_SHADOW_LIGHT=point|spot|point-unshadowed|spot-unshadowed`, which adds a light
beside the camera. `BASICRENDERER_DEMO_SHADOW_LIGHT_POSE="x,y,z,dx,dy,dz"` places it instead.
The VSM debug outputs (`OUTPUT_VSM_*`) show one light's virtual shadow map, chosen with the menu's **VSM Debug
Light** (`clodVirtualShadowDebugLight`, a light buffer index; `SARP_CLOD_VSM_DEBUG_LIGHT`). The default is the sun.
For a spot or point light the views describe its lookup at each pixel it reaches: the clipmap views show the face's mips
(preferred is the mip the receiver selects, sampled the one it read), and the page views show the receiver's own page
at that mip. The renderer publishes the lights that have a virtual shadow map (`clodVirtualShadowDebugLightLabels`) and
logs them when they change. `SARP_DEBUG_OUTPUT_TYPE` selects an output at startup.

`SARP_CLOD_VSM_TELEMETRY=1` logs the shadow map's per-frame work. If cluster records keep being emitted but the
raster's triangle and pixel counts stay near zero, pages are being retried without being written. The ownership
line's `emptyAdmittedCompleted` and `rasterWorkDropped` show empty completions and the drops that block them.

## Shadowing modes

`ShadowingMode` (`RenderContext.h`; `SHADOWING_MODE_*` in `shaders/Common/defines.h`) says how analytic lights are
shadowed. It follows the pipeline recipe and reaches shaders as `MiscShadowingMode`:

| Mode | Recipe | Effect |
|---|---|---|
| `None` | no `ClusterLodShadowTechnique` | No occlusion. |
| `VirtualShadowMaps` | `ClusterLodShadowTechnique` | The sun through the virtual shadow map, plus the ray traced sun mask where `enableRayTracedShadows` is on. Spot and point lights the budget selected, through their faces in the same shadow map; others unshadowed. |
| `StochasticRayTraced` | `ClusterLodShadowTechnique` and `StochasticDirectLightingTechnique`, on a device that traces clusters | Stochastic lighting samples, traces and shades every analytic light, the sun included, and the deferred pass adds its result ([stochastic_lighting.md](stochastic_lighting.md)). No shadow map is built: no faces, no clipmaps, no ray traced sun mask. Forward passes light unshadowed. |

The `enableShadows` setting (the menu's Shadows checkbox) adds or removes `ClusterLodShadowTechnique`, and
`enableStochasticLighting` (Stochastic Lighting) adds or removes `StochasticDirectLightingTechnique`. In
`StochasticRayTraced`, `ClusterLodShadowTechnique` stays in the recipe as the "shadows on" switch, but its graph
extension (`CLodShadow`) is not registered. `Renderer::VirtualShadowMapsActive` says whether the shadow map is built, and
the renderer publishes it at graph build as the `virtualShadowMapsActive` setting, which the passes that declare its
resources read. `LightingFrameSettings::shadowingMode` carries the mode to passes. `DeferredShadingPass` declares the
shadow map's resources only in `VirtualShadowMaps`.

### Adding a mode

A mode comes as a technique in the recipe, plus a value in both `ShadowingMode` and `SHADOWING_MODE_*`. A new mode has
to settle:

- Whether the deferred pass still runs the loop for some lights with that mode's visibility, or a separate pass shades
  the lights and the deferred pass adds its result (as `StochasticRayTraced` does). `DirectLightingOptions` carries
  visibility for the sun only (`sunOcclusion`, which can come from a resolved texture, as the ray traced mask does);
  per-light visibility would extend `EvaluateLightOcclusion`.
- What forward and ray-hit paths get. They cannot read a screen-space result for their own fragments. In
  `VirtualShadowMaps` they share the primary pass's shadow map; in a mode without one they are unshadowed unless they
  trace.
- Light selection. The light grid covers only the primary camera's frustum, and only its visible surfaces (the forward
  runs extend to transparent fragments in front of them). Ray hits have no world-space light structure.
- Temporal reuse, which needs `LightInfo::generation` to validate stored light slots.

## Ambient occlusion

Screen-space ambient occlusion has one slot in the recipe, `GtaoTechnique`, which the `enableGTAO` setting (the menu's
Ambient Occlusion checkbox) adds or removes. The `ambientOcclusionTechnique` setting picks what fills it when the graph is
built: `1` CORTAO (the default), `0` XeGTAO. Changing it rebuilds the graph. The path-traced recipe has no slot.

`ScreenSpaceAOMode` (`RenderContext.h`; `SCREEN_SPACE_AO_MODE_*` in `shaders/Common/defines.h`) reaches lighting passes
as `MiscScreenSpaceAOMode` (`GetRootScreenSpaceAOMode()`). `DeferredShadingPass` and `SpecularIBLPass` declare the output of
the technique their graph was built with and send `Off` on a frame whose mode differs from it, so the frame between a
setting change and the rebuild reads nothing undeclared. `ApplyScreenSpaceAO` (`utilities.hlsli`) fills
`FragmentInfo::diffuseAmbientOcclusion` (the smaller of the material's and the screen-space visibility) and
`FragmentInfo::bentNormalWS`, which the IBL takes as its bent normal (the diffuse irradiance is looked up along
`normalize(normal + bentNormal)`). Forward, transparent and ray-hit paths pass `SCREEN_SPACE_AO_MODE_OFF`, and their bent
normal is the shading normal. The debug outputs are AO and Bent Normal.

| Technique | Passes | Output |
|---|---|---|
| XeGTAO | `GTAOFilterPass`, `GTAOMainPass`, `GTAODenoisePass` (`shaders/GTAO.hlsl`) | `Builtin::GTAO::OutputAOTerm`, R8_UInt visibility. Fixed settings (High quality, radius 0.5). |
| CORTAO | `CORTAOPrepass`, `CORTAOMainPass`, `CORTAOFilterPass` (`CORTAOPasses.h`; `shaders/CORTAO*.hlsl`) | `Builtin::CORTAO::OutputAO`, R8G8B8A8_SNorm: world-space bent normal in rgb, visibility in a. |

### CORTAO

[CORTAO](https://github.com/theagentd/CORTAO) (v1.0.3, MIT, vendored unmodified in `shaders/CORTAO/` and
`include/ThirdParty/CORTAO/`) traces 16 rays per pixel against circular occluders built from depth and normal samples.
The passes run on the compute queue:

1. The prepass packs linear depth with a dithered spheremap normal (10 bits) into `Builtin::CORTAO::PackedData` (R32_UInt),
   mips 0 to `CortaoMipCount - 1` (at most 8) in one dispatch. Depth is the primary camera's linear depth map rather than
   a hardware depth buffer: `cortao_loadDepth` returns 1 / linear depth (0 where the map holds FLT_MAX, the sky), and the
   prepass's depth scale and offset are 1 and 0 so its linearization is the identity.
2. The main pass samples the packed mips with integer `SampleLevel` (cs_6_7) and writes visibility and the view-space bent
   normal as snorm8x4 into `Builtin::CORTAO::WorkingAO` (R32_UInt). `cortaoSampleCount` (4, 6, 8, 12, 16, 24 or 32;
   default 8) is the pipeline's `CORTAO_SAMPLE_COUNT`, so changing it rebuilds the graph. Both lookup tables, the
   24x24x16 occluder table and the 65536-entry bent normal table, are structured buffers the pass uploads once; the
   occluder table's nearest/clamp lookup is done in the shader.
3. The filter pass gathers both into a sparse depth- and normal-aware blur, 2x2 pixels per thread, and writes
   `OutputAO` with the bent normal turned to world space.

Conventions: the renderer's view space is right-handed and looks down -Z, as CORTAO's is, so normals are only rotated
(`mul(normalWS, (float3x3)view)`). The projection is the jittered one with its inverse, stored as row-vector matrices, so
the jitter sits in `[2][0]`/`[2][1]` where `populateMainPassParameters` reads it; `flipY` is set because texture v runs
down.

CORTAO has no temporal filter: it relies on the integrator's. While an upscaler runs (DLSS, FSR, Ray Reconstruction) the
random seed changes every frame and the small (5-tap) filter kernel runs. With `upscalingMode` None the seed is fixed, so
the noise is a stable pattern, and the large (13-tap) kernel runs. Both are chosen per frame.

Settings, all read every frame except the sample count: `cortaoRadius` (world units, default 1), `cortaoScreenRadiusRatio`
(the largest radius as a fraction of screen height, 0.2), `cortaoMipBias` (-4), `cortaoSlopeCompensation` (4),
`cortaoBackfaceThreshold` (0.5), `cortaoBackfaceBoost` (0), `cortaoFilterDepthSensitivity` (512) and
`cortaoFilterNormalSensitivity` (128). CORTAO's README describes each.
