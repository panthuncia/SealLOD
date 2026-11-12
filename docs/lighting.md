# Lighting

How BasicRenderer evaluates analytic lights (point, spot, directional), how they are shadowed, and where a new
shadowing technique plugs in. Image-based lighting and the environment are covered only where they meet this.

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
`PSO_CLUSTERED_LIGHTING` it walks the fragment's cluster pages, which hold light buffer indices
(`lightCulling.hlsl`); otherwise it walks the whole active list. Per light it evaluates occlusion, the light model and
the OpenPBR BRDF (`EvaluateDirectLightContribution`), and accumulates:

- `lighting`: the sum raster writes, computed exactly as before the loop was unified.
- `diffuse`, `specular`: the same contribution split by lobe (glint counts as specular). Nothing consumes these yet;
  the compiler drops them where unused.

`lightFragment` (deferred, with debug outputs) and `lightFragmentColor` (forward and ray-hit paths) wrap it with
image-based lighting and emission.

### Visibility is the caller's to state

The loop reads no root constants. Callers pass `DirectLightingOptions`:

| Field | Meaning |
|---|---|
| `punctualLights` | False skips every analytic light. |
| `virtualShadowMaps` | Look up occlusion in the virtual shadow map, for the sun and for spot and point lights with faces; its resources must be bound. |
| `sunOcclusion` | Sun occlusion known before the loop, combined with the shadow map's by max. |
| `sunLightIndex` | Which light `sunOcclusion` is for. |

`DirectLightingOptionsFromRoot()` builds them from a lighting pass's root switches (`MiscEnablePunctualLights`,
`MiscShadowingMode`). A pass whose root constants mean something else in those slots must build its options with
`MakeDirectLightingOptions` instead. Ray traced reflections do this, tracing their own ray for the sun. Defining
`LIGHTING_NO_VIRTUAL_SHADOW_MAPS` compiles the shadow map lookup out, for passes that never bind its resources.

Current callers:

| Pass | Options |
|---|---|
| Deferred (`deferred.hlsl`) | From root, plus the ray traced sun mask as `sunOcclusion` when it runs. |
| AVBOIT shading, deep visibility resolve | From root. |
| Ray traced reflections | Every light; the sun's occlusion from one ray; no shadow map. |

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
`SARP_CLOD_VSM_UPGRADE_PAGE_BUDGET`). Both are in the menu with the page render budget. In the demo the cooldown cut
upgrade re-renders during loading from about 41,600 to 12,000-13,000 and the time spent in hitch frames by 3-8x. A
30-frame cooldown let pages wait long enough for their refined groups to be streamed out, and they were re-requested
and re-rendered indefinitely; the menu caps it at 16.

**Level of detail.** A face's LOD threshold follows the sun's policy: it is set from the `shadowResolution` setting
(2048), not the 16K the face renders at, so caster detail is about 8 texels of tolerance, as for the sun's clips. At 16K
the faces asked for 8 times finer geometry than the camera had streamed, and the resulting upgrades re-rendered
hundreds of pages a frame during loading.

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

If any is nonzero the page stays admitted and is tried again next frame. This applies to the sun as well, which had a
handful of such pages retrying every frame.

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
one-texel depth bias. This is a hard shadow softened over one texel; a ray march using `sourceRadius` is planned.

### Raster paths

| Path | Local faces |
|---|---|
| Hardware mesh | Every mip. |
| Voxels | Every mip (one rasterization pass per mip, with the mip's own scissor). |
| Software raster | Every mip. One record per cluster; the rasterizer divides by w, interpolates 1/depth, and walks the face's mips, clamping each to its rectangle. Meshlets crossing the near plane go to the hardware path. |
| Reyes | Not yet. Displaced clusters render undisplaced through the hardware path. |
| SARP grass (caster provider) | Not yet. |

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

In the demo with one point light beside the camera, the statistical profiler settles after 22 s (14 s without the
light). While the first geometry streams in, about 110 frames re-render pages at 20-40 ms of shadow raster. Steady-state
cost is:

- shadow culling and raster: the same as without the light (0.18 ms and 0.005 ms);
- shadow lookup: about 0.8 ms of deferred shading at 2560×1600;
- page clears: 0.42 ms (0.13 ms without the light). The light is within the wind's reach of the camera, so its faces
  use the dynamic layer, and every active dynamic page is cleared and recomposed each frame.

## Shadowing modes

`ShadowingMode` (`RenderContext.h`; `SHADOWING_MODE_*` in `shaders/Common/defines.h`) says how analytic lights are
shadowed. It follows the pipeline recipe and reaches shaders as `MiscShadowingMode`:

| Mode | Recipe | Effect |
|---|---|---|
| `None` | no `ClusterLodShadowTechnique` | No occlusion. |
| `VirtualShadowMaps` | `ClusterLodShadowTechnique` | The sun through the virtual shadow map, plus the ray traced sun mask where `enableRayTracedShadows` is on. Spot and point lights the budget selected, through their faces in the same shadow map; others unshadowed. |

The `enableShadows` setting (the menu's Shadows checkbox) adds or removes `ClusterLodShadowTechnique`.
`LightingFrameSettings::shadowingMode` carries the mode to passes. `DeferredShadingPass` declares the shadow map's
resources only in `VirtualShadowMaps`.

### Adding a mode

A new mode, such as stochastic direct lighting, comes as a technique in the recipe, plus a value in both
`ShadowingMode` and `SHADOWING_MODE_*`. Things to settle when adding one:

- Whether the deferred pass still runs the loop for some lights (the sun, say) with that mode's visibility, or a
  separate pass shades the sampled lights and the deferred pass adds its result. Today `DirectLightingOptions` carries
  visibility for the sun only (`sunOcclusion`, which can come from a resolved texture, as the ray traced mask does);
  per-light visibility would extend `EvaluateLightOcclusion`.
- What forward and ray-hit paths get. They cannot read a screen-space result for their own fragments. In
  `VirtualShadowMaps` they share the primary pass's shadow map; in a mode without one they would be unshadowed unless
  they trace.
- Light selection. The clustered lists cover only the primary camera's frustum and loop over every light per cluster.
  Ray hits have no world-space light structure.
- Temporal reuse needs `LightInfo::generation` to validate stored slots. It also needs history this renderer does not
  keep yet: only linear depth has a previous-frame copy.
- Denoising. DLSS Ray Reconstruction's guides are produced only by the path tracer today.
