# Deferred shading: optimization experiments

`DeferredShadingPass` (`shaders/deferred.hlsl`, `DeferredCSMain`) on the Zorah scene, raster with stochastic direct lighting
(`SARP_RT_PATH_TRACING=0`, `SARP_STOCHASTIC_LIGHTING=1`, `SARP_TEXTURE_FEEDBACK_FORCE_MIP=4`, `clodLodErrorPixels` 3, frozen
lights, `ZORAH_CAMERA_MOTION=profile`), 2560×1600 render resolution, RTX 5070 Laptop (GB206). VRAM ~9.2 GB of a 10.7 GB budget,
so no texture pull-back. Times are the in-situ render graph timer, A/B/A/B in one session; NVPerf for counters only.

## Shadowing-mode variants (kept, 2026-11-21)

The deferred PSO was keyed only on the global PSO flags, so one shader held every shadowing mode and chose at run time from
`GetRootShadowingMode()`. In stochastic mode the light loop and every shadow map lookup are dead, but they were compiled in and
set the register count.

| | Registers | Occupancy | Launch stall: registers | DRAM throughput | GPU time |
|---|---:|---:|---:|---:|---:|
| One shader, mode at run time | 257 | 15.8% | 24% | 51% | 0.662 ms |
| Compiled for stochastic mode | 73 | 55.2% | 23% | 74% | 0.637 ms (-3.7%) |

Now `PSOManager::GetDeferredPSO(psoFlags, shadowingMode)` compiles one variant per `ShadowingMode`, with `DEFERRED_SHADOWING_MODE`
set; the stochastic and unshadowed variants also define `LIGHTING_NO_VIRTUAL_SHADOW_MAPS`. The pass passes the mode it writes to
the root constants. Images matched within the noise floor (tile mean error 0.49% against 0.50% for A against A').

The time moved much less than the occupancy because the pass is now bandwidth-bound: about 64 bytes a pixel are read and written
(depth 4, base colour 4, normal and roughness 8, specular and AO 4, emissive 8, identity 8, CORTAO 4, stochastic diffuse and
specular 16, HDR write 8). That is ~262 MB a frame, ~410 GB/s at 0.64 ms, at this GPU's DRAM peak with L2 hits supplying the
rest.

## Not done: fewer bytes

The remaining lever is bytes a pixel. Two candidates were found and left for a decision:

- **Emissive** (8 bytes, RGBA16F) is read for every pixel and is zero on most. An identity flag bit (like the payload profile
  bits) would let `GetFragmentInfoCanonicalSurface` skip the load. It needs a new `SARPSurfaceFlags` bit in SARP's public
  contributor header (`include/SARP/RenderModule/Surface.h`), and a contributor that writes emission without setting it would
  lose its emission. An inverted "no emission" bit avoids that. Expected gain ~0.05 ms.
- **Stochastic diffuse and specular** (16 bytes) are read only to be added. They stay separate for `capture.hdr`'s
  `stochastic_diffuse` and `stochastic_specular` targets, which the stochastic lighting benchmarks use.
