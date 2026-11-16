// Stochastic lighting's estimator, kernel 3 of 3: shading and the visible lists (docs/stochastic_lighting.md "Kernel split").
// Each sampled pixel sums its sample slots (stochasticSplit.hlsli) whose rays reached their lights, or that took no ray,
// into the unbiased average f * V / p that selection (stochasticSelect.hlsl) prepared, and each 8x8 tile builds next
// frame's visible list from the lights its rays reached (docs/stochastic_lighting.md "Guiding", "Guiding at scale"). The
// output, the running average and the reduced-resolution writes are as before the split.
#define LIGHTING_NO_VIRTUAL_SHADOW_MAPS 1

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/utilities.hlsli"
#include "include/lighting.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/materialFlags.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#include "PerPassRootConstants/stochasticLightingRootConstants.h"
#include "include/clodRayTracingHits.hlsli"
#include "include/stochasticLighting.hlsli"
#include "include/stochasticSplit.hlsli"

#define STOCHASTIC_ACCUMULATE_MAX_SAMPLES 65536.0f
#define STOCHASTIC_NO_KEY 0xFFFFFFFFu


// 0: no guiding whatever stochasticLightingGuiding says (as STOCHASTIC_GUIDING in stochasticSelect.hlsl; set both).
#ifndef STOCHASTIC_GUIDING
#define STOCHASTIC_GUIDING 1
#endif
#define STOCHASTIC_GUIDING_ENABLED (STOCHASTIC_GUIDING != 0)

// The visible list build: each wave's sorted run, then the merged list, as packed build entries (below). One uint an entry
// keeps the group's shared memory at 2 KB: more costs the pass L1 cache.
#define STOCHASTIC_GROUP_KEYS (STOCHASTIC_TILE_SIZE * STOCHASTIC_TILE_SIZE * STOCHASTIC_MAX_SAMPLE_COUNT)
groupshared uint gs_runEntries[STOCHASTIC_GROUP_KEYS];
groupshared uint gs_runCounts[STOCHASTIC_TILE_SIZE * STOCHASTIC_TILE_SIZE / 4u]; // Waves of at least 4 lanes.
groupshared uint gs_mergedEntries[STOCHASTIC_GROUP_KEYS];

// A build entry: the light index (20 bits), the quadrants reached (4 bits) and the weight as a log2 code (8 bits, steps of
// 2^(1/4); 0 is no weight). Only the order of weights matters (which lights a full list keeps).
uint StochasticPackBuildEntry(uint lightIndex, uint quadrants, float weight)
{
    const uint code = weight > 0.0f ? uint(clamp(round(log2(weight) * 4.0f) + 128.0f, 1.0f, 255.0f)) : 0u;
    return lightIndex | (quadrants << 20u) | (code << 24u);
}

float StochasticBuildEntryWeight(uint entry)
{
    const uint code = entry >> 24u;
    return code != 0u ? exp2((float(code) - 128.0f) * 0.25f) : 0.0f;
}

// One thread per sampled pixel, in selection's 8x8 groups: a group's tile, and its visible list, covers 8x8 sampled pixels.
[numthreads(STOCHASTIC_TILE_SIZE, STOCHASTIC_TILE_SIZE, 1)]
void StochasticShadeCS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const uint downsample = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? 2u : 1u;
    const uint2 screen = uint2(perFrame.screenResX, perFrame.screenResY);
    const uint2 blocks = (screen + downsample - 1u) / downsample;
    // No early returns: every thread takes part in the tile's list build below.
    const bool inBounds = all(id.xy < blocks);
    const uint2 pixel = StochasticDownsampledPixel(id.xy, CLOD_RT_SEC_FRAME, downsample, screen);
    const bool accumulate = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_ACCUMULATE) != 0u;
    const bool guiding = STOCHASTIC_GUIDING_ENABLED && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_GUIDING) != 0u;
    const uint tilePixels = STOCHASTIC_TILE_SIZE * downsample;
    const uint2 tiles = (screen + tilePixels - 1u) / tilePixels;
    const uint sampleCount = clamp(STOCHASTIC_SAMPLE_COUNT, 1u, STOCHASTIC_MAX_SAMPLE_COUNT);
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];

    const uint sampledIndex = id.y * blocks.x + id.x;
    RWStructuredBuffer<uint> pixelWords = ResourceDescriptorHeap[STOCHASTIC_PIXEL_WORDS_UAV];
    const uint pixelWord = inBounds ? pixelWords[sampledIndex] : 0u;
    const bool valid = (pixelWord & STOCHASTIC_PIXEL_VALID) != 0u;

    // Each slot's light and the quadrant its ray reached, for the tile's list. STOCHASTIC_NO_KEY: none.
    // keyWeights: the slot's estimate's luminance, which decides what a full list keeps.
    uint keys[STOCHASTIC_MAX_SAMPLE_COUNT];
    uint keyQuadrants[STOCHASTIC_MAX_SAMPLE_COUNT];
    float keyWeights[STOCHASTIC_MAX_SAMPLE_COUNT];
    float3 diffuse = 0.0f;
    float3 specular = 0.0f;
    RWStructuredBuffer<uint3> slots = ResourceDescriptorHeap[STOCHASTIC_SLOTS_UAV];
    [unroll]
    for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
    {
        keys[k] = STOCHASTIC_NO_KEY;
        keyQuadrants[k] = 0u;
        keyWeights[k] = 0.0f;
        if (!valid || k >= sampleCount)
        {
            continue;
        }
        const uint3 slot = slots[sampledIndex * sampleCount + k];
        // Visible: its ray reached the light, or it took none.
        const bool visible = (slot.x & STOCHASTIC_SLOT_USED) != 0u
            && ((slot.x & STOCHASTIC_SLOT_RAY) == 0u || (pixelWord & (1u << k)) != 0u);
        if (!visible)
        {
            continue;
        }
        const float3 sampleDiffuse = StochasticUnpackRgb9e5(slot.y);
        const float3 sampleSpecular = StochasticUnpackRgb9e5(slot.z);
        diffuse += sampleDiffuse;
        specular += sampleSpecular;
        keys[k] = slot.x & STOCHASTIC_ENTRY_INDEX_MASK;
        keyQuadrants[k] = 1u << ((slot.x >> STOCHASTIC_SLOT_QUADRANT_SHIFT) & 3u);
        const float estimate = StochasticLuminance(sampleDiffuse + sampleSpecular);
        keyWeights[k] = estimate > 0.0f && estimate < 3.0e38f ? estimate : 0.0f; // Finite: a full list ranks by it.
    }


    // ---- This tile's visible list for next frame: the sorted unique lights its rays reached, each with the union of the
    // quadrants reached; when more than STOCHASTIC_VISIBLE_LIST_CAPACITY, the ones whose samples' estimates sum highest.
    // Each wave first takes its own lanes' keys in order (one WaveActiveMin per unique light, no group barriers), then
    // the first wave merges the waves' sorted runs.
    float debugTileLights = -1.0f; // STOCHASTIC_DEBUG_PROBE 5: this tile's unique visible lights before the capacity cut.
    if (guiding)
    {
        const uint laneCount = WaveGetLaneCount();
        const uint wave = groupIndex / laneCount;
        const uint waveCount = (STOCHASTIC_TILE_SIZE * STOCHASTIC_TILE_SIZE + laneCount - 1u) / laneCount;
        const uint runBase = wave * laneCount * STOCHASTIC_MAX_SAMPLE_COUNT;
        uint runCount = 0u;
        for (;;)
        {
            uint smallest = STOCHASTIC_NO_KEY;
            [unroll]
            for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
            {
                smallest = min(smallest, keys[k]);
            }
            const uint key = WaveActiveMin(smallest);
            if (key == STOCHASTIC_NO_KEY)
            {
                break; // Wave-uniform.
            }
            uint quadrants = 0u;
            float weight = 0.0f;
            [unroll]
            for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
            {
                if (keys[k] == key)
                {
                    quadrants |= keyQuadrants[k];
                    weight += keyWeights[k];
                    keys[k] = STOCHASTIC_NO_KEY;
                }
            }
            quadrants = WaveActiveBitOr(quadrants);
            weight = WaveActiveSum(weight);
            if (WaveIsFirstLane())
            {
                gs_runEntries[runBase + runCount] = StochasticPackBuildEntry(key, quadrants, weight);
            }
            ++runCount;
        }
        if (WaveIsFirstLane())
        {
            gs_runCounts[wave] = runCount;
        }
        GroupMemoryBarrierWithGroupSync();

        if (wave == 0u)
        {
            // Merge: lane w holds the head of wave w's run. Each round takes the smallest head, ORs the quadrants and sums
            // the weights of every run that has it, and advances those runs. With one wave the run is already the list.
            uint mergedCount = runCount;
            if (waveCount > 1u)
            {
                const uint headRun = WaveGetLaneIndex();
                const uint headEnd = headRun < waveCount ? gs_runCounts[headRun] : 0u;
                const uint headBase = headRun * laneCount * STOCHASTIC_MAX_SAMPLE_COUNT;
                uint cursor = 0u;
                mergedCount = 0u;
                for (;;)
                {
                    const uint packed = cursor < headEnd ? gs_runEntries[headBase + cursor] : STOCHASTIC_NO_KEY;
                    const uint head = cursor < headEnd ? (packed & STOCHASTIC_ENTRY_INDEX_MASK) : STOCHASTIC_NO_KEY;
                    const uint key = WaveActiveMin(head);
                    if (key == STOCHASTIC_NO_KEY)
                    {
                        break;
                    }
                    const bool take = head == key;
                    const uint quadrants = WaveActiveBitOr(take ? (packed >> 20u) & 0xFu : 0u);
                    const float weight = WaveActiveSum(take ? StochasticBuildEntryWeight(packed) : 0.0f);
                    if (WaveIsFirstLane())
                    {
                        gs_mergedEntries[mergedCount] = StochasticPackBuildEntry(key, quadrants, weight);
                    }
                    cursor += take ? 1u : 0u;
                    ++mergedCount;
                }
            }
            debugTileLights = float(mergedCount);

            // Write the list, in index order (the estimator binary-searches it). When it is longer than the capacity, an
            // entry is kept if fewer than STOCHASTIC_VISIBLE_LIST_CAPACITY entries outweigh it (ties by position, so exactly
            // the capacity is kept), and the kept entries are compacted in order.
            RWStructuredBuffer<uint> lists = ResourceDescriptorHeap[STOCHASTIC_VISIBLE_LISTS_UAV];
            const uint listBase = (groupId.y * tiles.x + groupId.x) * STOCHASTIC_VISIBLE_LIST_STRIDE;
            const bool full = mergedCount > STOCHASTIC_VISIBLE_LIST_CAPACITY;
            uint written = 0u;
            for (uint chunk = 0u; chunk < mergedCount; chunk += laneCount)
            {
                const uint i = chunk + WaveGetLaneIndex();
                bool keep = i < mergedCount;
                uint packed = 0u;
                if (keep)
                {
                    packed = waveCount > 1u ? gs_mergedEntries[i] : gs_runEntries[i];
                    if (full)
                    {
                        const uint weight = packed >> 24u;
                        uint outweighedBy = 0u;
                        for (uint j = 0u; j < mergedCount; ++j)
                        {
                            const uint other = (waveCount > 1u ? gs_mergedEntries[j] : gs_runEntries[j]) >> 24u;
                            outweighedBy += (other > weight || (other == weight && j < i)) ? 1u : 0u;
                        }
                        keep = outweighedBy < STOCHASTIC_VISIBLE_LIST_CAPACITY;
                    }
                }
                const uint slot = written + WavePrefixCountBits(keep);
                if (keep && slot < STOCHASTIC_VISIBLE_LIST_CAPACITY)
                {
                    const uint lightIndex = packed & STOCHASTIC_ENTRY_INDEX_MASK;
                    lists[listBase + 1u + slot] = StochasticPackEntry(lightIndex, lights[lightIndex].generation, (packed >> 20u) & 0xFu);
                }
                written += WaveActiveCountBits(keep);
            }
            if (WaveIsFirstLane())
            {
                lists[listBase] = min(written, STOCHASTIC_VISIBLE_LIST_CAPACITY);
            }
        }
    }

    if (!inBounds)
    {
        return;
    }
    RWTexture2D<float4> outputDiffuse = ResourceDescriptorHeap[STOCHASTIC_DIFFUSE_UAV];
    RWTexture2D<float4> outputSpecular = ResourceDescriptorHeap[STOCHASTIC_SPECULAR_UAV];
    if ((pixelWord & STOCHASTIC_PIXEL_DEBUG) != 0u)
    {
        return; // Selection or the trace wrote a debug probe's value here.
    }
    // A downsampled thread's other pixels (not with the denoiser, whose input is one texel per sampled pixel): their own
    // running averages when accumulating, else this sample (nearest; the denoiser is what reconstructs full resolution).
    const bool writeBlock = downsample > 1u && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_DENOISED) == 0u;
    float3 blockDiffuse[4] = { diffuse, diffuse, diffuse, diffuse };
    float3 blockSpecular[4] = { specular, specular, specular, specular };
    if (accumulate)
    {
        // For checking against the reference: a running average while nothing changes, per pixel. Downsampled, each pixel
        // averages only the frames that sampled it, so every pixel still converges to its own value.
        RWTexture2D<float4> accumulationDiffuse = ResourceDescriptorHeap[STOCHASTIC_ACCUMULATION_DIFFUSE_UAV];
        RWTexture2D<float4> accumulationSpecular = ResourceDescriptorHeap[STOCHASTIC_ACCUMULATION_SPECULAR_UAV];
        const bool restartAll = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_RESTART_ACCUMULATION) != 0u;
        const bool restart = !valid || restartAll;
        const float4 previousDiffuse = restart ? 0.0f : accumulationDiffuse[pixel];
        const float4 previousSpecular = restart ? 0.0f : accumulationSpecular[pixel];
        const float count = min(previousDiffuse.a + 1.0f, STOCHASTIC_ACCUMULATE_MAX_SAMPLES);
        diffuse = lerp(previousDiffuse.rgb, diffuse, 1.0f / count);
        specular = lerp(previousSpecular.rgb, specular, 1.0f / count);
        accumulationDiffuse[pixel] = valid ? float4(diffuse, count) : 0.0f;
        accumulationSpecular[pixel] = valid ? float4(specular, count) : 0.0f;
        if (writeBlock)
        {
            [unroll]
            for (uint q = 0u; q < 4u; ++q)
            {
                const uint2 other = id.xy * 2u + uint2(q & 1u, q >> 1u);
                if (any(other >= screen) || all(other == pixel))
                {
                    continue;
                }
                if (restartAll)
                {
                    accumulationDiffuse[other] = 0.0f;
                    accumulationSpecular[other] = 0.0f;
                    blockDiffuse[q] = 0.0f;
                    blockSpecular[q] = 0.0f;
                }
                else
                {
                    blockDiffuse[q] = accumulationDiffuse[other].rgb;
                    blockSpecular[q] = accumulationSpecular[other].rgb;
                }
            }
        }
    }
#if defined(STOCHASTIC_DEBUG_PROBE) && STOCHASTIC_DEBUG_PROBE == 5
    // Diagnostic (pso.recompile on Stochastic.Select.PSO and Stochastic.Shade.PSO, denoiser off): guiding's lists. Diffuse:
    // the reprojected list's length (-1 where reprojection failed), this tile's unique visible lights this frame before the
    // capacity cut (the first wave's lanes; -1 elsewhere), and the capacity.
    diffuse = float3(float((pixelWord >> STOCHASTIC_PIXEL_LIST_SHIFT) & 0xFFu) - 1.0f, debugTileLights, float(STOCHASTIC_VISIBLE_LIST_CAPACITY));
    specular = 0.0f;
#endif
    if (!writeBlock)
    {
        // At the sampled pixel, or (denoised and downsampled) at the noisy inputs' texel for this block.
        const uint2 target = downsample > 1u ? id.xy : pixel;
        // Denoised, the diffuse input's alpha carries selection's unshadowed bound for the denoiser (stochasticSplit.hlsli).
        const float alpha = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_DENOISED) != 0u ? min(StochasticDecodeBound(pixelWord), 65504.0f) : 1.0f;
        outputDiffuse[target] = float4(diffuse, alpha);
        outputSpecular[target] = float4(specular, 1.0f);
        return;
    }
    [unroll]
    for (uint q = 0u; q < 4u; ++q)
    {
        const uint2 other = id.xy * 2u + uint2(q & 1u, q >> 1u);
        if (any(other >= screen))
        {
            continue;
        }
        const bool sampled = all(other == pixel) || !accumulate;
        outputDiffuse[other] = float4(sampled ? diffuse : blockDiffuse[q], 1.0f);
        outputSpecular[other] = float4(sampled ? specular : blockSpecular[q], 1.0f);
    }
}
