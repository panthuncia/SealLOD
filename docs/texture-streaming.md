# Material texture streaming

A streamed material texture keeps only the mips its surfaces need, within a VRAM budget. Each texture holds a window,
mips `[top, last]`, loaded whole by its residency job (`TextureStreamingManager::RequestPipelineResidency`). The
residency policy decides `top`: one footprint per material, a 4 Hz policy tick with settle and slack rules, and a
budget controller that pulls demand back before anything overflows. It uses the same model as the Community Shaders
DXVK texture streamer (`external_refs/cs-dxvk-org`, `docs/development/texture-streaming.md` there).

Measurements on Zorah are in [performance/texture-streaming-results.md](performance/texture-streaming-results.md).

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
`MaterialManager::JournalMaterialRow`). Samples inside the scope write nothing of their own.

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

## How shaders find the image

A material texture with a streaming ID samples the image its row in the texture image table
(`Builtin::Material::TextureStreamingMetadataBuffer`) publishes, else the material's own descriptor
(`ResolveMaterialTextureDescriptor`, `materialTextureBinding.hlsli`). Per pixel that is a dependent chain: the material
record, then the row, then the sample.

Shaders find the table itself through the per-frame buffer (`PerFrameCB::textureImageTableDescriptorIndex`, written by the
frame from `TextureStreamingManager::PublishedImageTableDescriptorIndex`), not through the pass's binding. The table's
resolver is late bound (`PublishedStateResourceResolver::MakeLateBound`): passes still declare
`Builtin::Material::TextureStreamingMetadataBuffer`, but it resolves to a fixed stand-in, so a new table is not a binding
change and kept recordings stay current. The declaration still carries the table's upload waits. A pass whose shader reads
the table must declare `Builtin::PerFrameBuffer`. `BASICRENDERER_TEXTURE_TABLE_LATE_BOUND=0` binds the table as before (for
A/B; shaders still read the per-frame index).

Material evaluation doesn't chain. `ResolveMaterialTextureDescriptorsPass` runs every frame just before it (~6 us on
Zorah) and writes `Builtin::Material::ResolvedTextureDescriptors`: per material evaluation table row, the descriptor each of
eight texture slots resolves to. Evaluation reads it by material index (`ResolveMaterialEvalTextureDescriptor`). The table
covers the material tables' capacity, at least 65536 rows. When the material tables grow past it, the renderer grows the
`materialResolvedTextureCapacity` setting and rebuilds the graph.

The pass re-resolves every row each frame because streaming does not patch the evaluation table when an image's
descriptor changes.

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
