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
- `shadowCaster`: only directional shadow casters get shadow views (the shadow map's clipmaps). Point and spot lights
  keep the flag for techniques that trace them; nothing shadows them today.

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
| `virtualShadowMaps` | Look up the sun's occlusion in the virtual shadow map; its resources must be bound. |
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

## Shadowing modes

`ShadowingMode` (`RenderContext.h`; `SHADOWING_MODE_*` in `shaders/Common/defines.h`) says how analytic lights are
shadowed. It follows the pipeline recipe and reaches shaders as `MiscShadowingMode`:

| Mode | Recipe | Effect |
|---|---|---|
| `None` | no `ClusterLodShadowTechnique` | No occlusion. |
| `VirtualShadowMaps` | `ClusterLodShadowTechnique` | The sun through the virtual shadow map, plus the ray traced sun mask where `enableRayTracedShadows` is on. Local lights unshadowed. |

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
