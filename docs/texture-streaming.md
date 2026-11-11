# Material texture streaming

A streamed material texture keeps only the mips its surfaces need, within a VRAM budget. Each texture holds a window,
mips `[top, last]`, loaded whole by its residency job (`TextureStreamingManager::RequestPipelineResidency`). The
residency policy decides `top`; the rest of the path (residency jobs, binding publication, the image table) is unchanged.

The design follows the Community Shaders DXVK texture streamer (`external_refs/cs-dxvk-org`,
`docs/development/texture-streaming.md` there): one footprint per material, a 4 Hz policy tick with settle and slack
rules, and a budget controller that pulls demand back before anything overflows.

## Feedback

All demand is a **footprint**: log2 of the UV one screen pixel covers. It needs no texture's size, so one value serves
every texture of a material: `top = log2(largest side) + footprint`, rounded to the finer mip.

The feedback buffer (`Builtin::Material::TextureStreamingFeedbackBuffer`) interleaves two kinds of word, so neither
needs its own resource or pass declarations:

| Word | Holds | Written by |
| --- | --- | --- |
| `2 * materialSlot + 1` | the material's finest footprint, `(footprint + 32) * 256` | material evaluation, once per evaluation |
| `2 * streamingTextureID` | the texture's requested top mip + 1 | samplers outside a material evaluation |

Both keep the minimum (finest) value through a wave-reduced `InterlockedMin`, and the worker resets them every frame.

**Material evaluation** (`SampleMaterialFromUvCache`, `SampleMaterialEvalFromUvCache`,
`SampleMaterialFromUvCacheRuntime`, `SampleObjectTriplanarStochasticMaterial` and the vertex-tangent wrappers in
`utilities.hlsli`) opens a feedback scope: it writes the finest footprint over the material's UV sets (or its three
triplanar planes) to the slot its row names (`MaterialInfo::streamingFeedbackSlot`, the row's slot + 1, stamped by
`MaterialManager::JournalMaterialRow`). Samples inside the scope write nothing of their own. Before this, every sample
of every texture wrote its own request.

`JournalMaterialRow` also tells the streaming manager which streamed textures each slot names
(`SetMaterialFeedbackTextures`), so the worker can turn a slot's footprint into each texture's mip.

**Samplers outside a material evaluation** keep writing their texture's word: terrain layers and RVT page generation,
Reyes displacement, ray tracing's alpha (`CLodRtSampleAlpha`) and the OMM bake's test mip. Their requested mips join the
same demand, converted to a footprint (`FootprintForTop`).

**Alpha tests write nothing.** Depth, shadow and visibility raster include overdraw and shadow texels; what is seen asks
through its material evaluation, and alpha-tested textures keep their minimum resident size
(`AlphaTestedMaterialTextureMaxResidentTopMip`) whatever the budget.

## The policy tick

`RunResidencyPolicyTick` runs on the streaming worker every 250 ms (`TextureStreamingPolicy.h` holds the pure parts).
Readbacks that land between ticks fold into the finest report per slot and texture.

Each texture holds its last report:

-   **On screen** for 1 s after it (`kVisibleTicks`).
-   **Held** for the next 7 s (`kHoldTicks` = 8 s in all): out of view, but kept for a turn of the camera.
-   After that it wants its coarsest allowed window: the last mip, or a binding's cap (alpha-tested coverage, terrain's
    `maximumResidentTopMip`).

Then, per texture with a window:

-   **Upgrades** wait one tick, or start at once when 3 or more mips short. What is on screen goes first, then the
    furthest short, up to `materialTextureStreamingRateMB` per second.
-   **Downgrades** need the window to be a mip finer than wanted for 5 s. So a surface out of view lets go of its
    detail about 13 s after it was last seen, and one that is still seen but further away after 5 s.
-   **Over the allowance**, the wait is off and the largest surplus goes first, down to what is wanted.
-   A window asked for and not resident after 10 s is asked again.

Before this, a texture went coarser only after 240 frames of coarser feedback with no finer frame among them, or after
1,800 frames with none at all, so a texture seen once stayed at its finest for minutes. The per-sample feedback meant one
grazing pixel kept it fine.

## The budget

The windows fit an **allowance**: 92% of the budget, less everything that is not a window. The budget is
`materialTextureStreamingBudgetMB`, or the OS's local-memory budget for the process when that is 0
(`QueryVideoMemoryInfo`). What is not a window is the process's local usage less the windows' bytes, smoothed: a rise
counts within a second, a fall takes tens, because a replaced window stays allocated until its last frame retires.

Each tick the controller (`StepPullback`) sums what the windows would take at the current pull-back, from each texture's
computed window sizes, with no VRAM reading involved, so it does not wind up while the windows catch up:

1.  Over the allowance, it adds up to a mip per tick to **held** demand (textures not reported in the last second).
2.  Once held demand is at its coarsest, it does the same to what is **on screen**, up to
    `materialTextureStreamingVisibleOffset` mips (default 16: the budget holds whatever it costs).
3.  Under the allowance, after 5 s, it steps back a quarter mip per tick while the step would fit 95% of the allowance.

## Results (2026-11-10)

Zorah (`D:\Caches\zorah`, 4,292 streamed textures), RelWithDebInfo demo, RTX 5070 Laptop (12 GB, an 11,202 MiB OS
budget), raster unless noted, one run each. VRAM is the process's dedicated GPU memory from Windows' `GPU Process Memory`
counters.

| Run | VRAM | Windows | Notes |
| --- | --- | --- | --- |
| Overview camera, before this change | 6,108 MiB | — | the deployed demo of the same day |
| Overview camera, after | 6,236 MiB | 639 MiB | 60 textures on screen; under budget, so it loads what feedback asks, as before |
| Overview camera, after, per-sample feedback restored in the shaders | 6,175 MiB | 639 MiB | the same 60 textures and 531–567 MiB wanted: material feedback loses no coverage |
| Close-up camera, after | 7,698 MiB | 1,014 MiB | 250 textures on screen |
| Close-up, every texture forced to mip 0 | 8,404 MiB | 1,506 MiB | unconstrained demand 85,680 MiB; held at a 3-mip pull-back within a 2.8 GB allowance, settled 7 s after load |
| Close-up, 7,800 MiB budget | 7,058–7,134 MiB | 189–597 MiB | target 7,176 MiB; never exceeded |
| Close-up, path tracing | 9,340 MiB | 681 MiB | 343 textures on screen through primary hits |

**Responsiveness (close-up, FOV changed through the control pipe's `camera.set`):**

-   FOV 80° to 10°: the 15 textures still on screen upgraded within 2 s (+290 MiB). The 235 that left the view were
    released 14 s later (8 s hold, then the 5 s downgrade wait): windows 1,301 to 349 MiB.
-   FOV back to 80°: back to about 1 GB within 2 s; the zoomed textures stepped down 5 s later.

**Under a tight budget during load.** With a 7,800 MiB budget the scene's own memory rose faster than its smoothing let
the allowance recover, so the on-screen pull-back reached 14.5 mips (everything at its coarsest) for about 30 s, then
relaxed to 0.5 over the next 40 s. Relaxing a quarter mip at a time upgrades each texture one mip at a time: about
1,700 small residency jobs for 250 textures. `materialTextureStreamingVisibleOffset` trades that for a budget overrun.

## Settings and switches

| Name | Default | Purpose |
| --- | --- | --- |
| `materialTextureStreamingBudgetMB` / `SARP_TEXTURE_STREAMING_BUDGET_MB` | 0 | The budget in MiB; 0 for the OS's |
| `materialTextureStreamingVisibleOffset` / `SARP_TEXTURE_STREAMING_VISIBLE_OFFSET` | 16 | How many mips what is on screen may step down over the budget |
| `materialTextureStreamingRateMB` / `SARP_TEXTURE_STREAMING_RATE_MB` | 1024 | Upgrades started per second, in MiB |
| `SARP_TEXTURE_FEEDBACK_FORCE_MIP=<n>` | | Every texture at mip n (within its caps), still held to the budget |
| `SARP_TEXTURE_STREAMING_LOG_SECONDS` | 10 | How often the policy logs its budget line |

The policy's last tick is in `MaterialTextureStreamingStats::budget`, and in the log:

```
Texture streaming: budget B MiB (driver D MiB), VRAM use U MiB, not windows N MiB, allowance A MiB; windows R MiB
resident, C MiB committed, W MiB wanted (V MiB on screen, X MiB unconstrained); pull-back held h on screen v; ...
```
