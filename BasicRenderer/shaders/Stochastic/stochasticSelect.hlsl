// Stochastic lighting's estimator, kernel 1 of 3: selection (docs/stochastic_lighting.md "Kernel split"). Each sampled pixel
// draws STOCHASTIC_SAMPLE_COUNT lights (guiding, the three sets, bounded candidates, reduced resolution: "Estimator" and on in
// the doc, unchanged from when one kernel did all of it), evaluates each chosen light's unshadowed contribution once, and
// writes per sample a slot (stochasticSplit.hlsli): the light, the emitter quadrant its ray takes and the estimate f * scale
// it adds if the ray reaches the light. Shadow rays are appended to a compacted list for the trace kernel
// (stochasticTrace.hlsl); samples of lights that cast no shadow, or that add nothing, take no ray. Shading and the
// visible-list build are kernel 3 (stochasticShade.hlsl).
//
// STOCHASTIC_DEDUPLICATE_RAYS: a light drawn by more than one of a pixel's samples (the sun's set has one reservoir, so its
// samples always share their light) traces one ray for all of them. Their rays had the same distribution, so each sample's
// estimate keeps its expectation; the samples are no longer independent, which adds variance where visibility varies over
// the emitter, and where the light's quadrants are weighted.
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
// The most of the samples the unlisted lights take while listed lights carry weight, and after reprojection fails.
#ifndef STOCHASTIC_HIDDEN_SHARE
#define STOCHASTIC_HIDDEN_SHARE 0.2f
#endif
#ifndef STOCHASTIC_HIDDEN_SHARE_UNGUIDED
#define STOCHASTIC_HIDDEN_SHARE_UNGUIDED 0.5f
#endif
// Candidates weighted by StochasticCheapImportance instead of the full BRDF: 9% cheaper at 1024 lights for +1-3% noise
// (docs/stochastic_lighting.md "Bounded candidates"). 0: the full contribution's importance.
#ifndef STOCHASTIC_CHEAP_IMPORTANCE
#define STOCHASTIC_CHEAP_IMPORTANCE 1
#endif
// How many of the cluster's entries a pixel draws as hidden-set candidates (docs/stochastic_lighting.md "Bounded
// candidates"). Clusters with no more take every light. 0: every light, unbounded.
#ifndef STOCHASTIC_CANDIDATE_COUNT
#define STOCHASTIC_CANDIDATE_COUNT 24
#endif
// The most of the samples directional lights take while local lights carry weight.
#ifndef STOCHASTIC_DIRECTIONAL_SHARE
#define STOCHASTIC_DIRECTIONAL_SHARE 0.5f
#endif
// 0: no guiding whatever stochasticLightingGuiding says (every local light is hidden; for guided-against-unguided A/B
// with pso.recompile). The surface history is still written.
#ifndef STOCHASTIC_GUIDING
#define STOCHASTIC_GUIDING 1
#endif
// 1: the hidden draw's offset is per pixel instead of per tile (docs/stochastic_lighting.md "Bounded candidates"): less
// correlated error between a tile's pixels, for about 3 ms at 1024 lights at full resolution. For A/B with pso.recompile.
// 1: each pixel's set numbers are taken in increasing order, (j + t) / N with t = frac(N v), the same strata as
// frac(v + j / N) reordered: sample j of every lane takes the directional set first, then the listed, then the unlisted, so
// a wave's lanes trace sun rays together and local rays together (docs/stochastic_lighting.md "Reduced resolution").
#ifndef STOCHASTIC_SORTED_SETS
#define STOCHASTIC_SORTED_SETS 1
#endif
#ifndef STOCHASTIC_DRAW_OFFSET_PER_PIXEL
#define STOCHASTIC_DRAW_OFFSET_PER_PIXEL 0
#endif
// Reprojection holds when last frame's surface there was this close in view depth (relative) and normal.
#define STOCHASTIC_REPROJECTION_DEPTH_TOLERANCE 0.1f
#define STOCHASTIC_REPROJECTION_NORMAL_COSINE 0.7f

// One weighted reservoir: a uniform number kept uniform by sample warping (Clarberg et al.) after every decision, and the
// light it holds. quadrants is the light's visible-list entry's mask (all four for unlisted lights).
struct StochasticReservoir
{
    float u;
    uint lightIndex;
    float weight;
    uint quadrants;
};

void StochasticReservoirUpdate(inout StochasticReservoir reservoir, uint lightIndex, float weight, float weightSum, uint quadrants)
{
    const float probability = weight / weightSum;
    if (reservoir.u < probability)
    {
        reservoir.u = reservoir.u / probability;
        reservoir.lightIndex = lightIndex;
        reservoir.weight = weight;
        reservoir.quadrants = quadrants;
    }
    else
    {
        reservoir.u = (reservoir.u - probability) / max(1.0f - probability, 1e-7f);
    }
}

StochasticReservoir StochasticReservoirCreate(float u)
{
    StochasticReservoir reservoir;
    reservoir.u = u;
    reservoir.lightIndex = LIGHT_INDEX_NONE;
    reservoir.weight = 0.0f;
    reservoir.quadrants = 0xFu;
    return reservoir;
}

// lightIndex's entry in a visible list (sorted by light index), or STOCHASTIC_NO_KEY. The caller checks the entry's
// generation: one that differs is another light that has since taken the slot.
uint StochasticFindListedEntry(RWStructuredBuffer<uint> lists, uint base, uint count, uint lightIndex)
{
    uint low = 0u;
    uint high = count;
    while (low < high)
    {
        const uint middle = (low + high) >> 1u;
        const uint entry = lists[base + 1u + middle];
        const uint index = entry & STOCHASTIC_ENTRY_INDEX_MASK;
        if (index == lightIndex)
        {
            return entry;
        }
        if (index < lightIndex)
        {
            low = middle + 1u;
        }
        else
        {
            high = middle;
        }
    }
    return STOCHASTIC_NO_KEY;
}

// A 64-bit filter of a visible list's light indices: a light whose bit is clear is certainly not listed, which spares most
// unlisted lights the binary search.
uint StochasticListFilterBit(uint lightIndex)
{
    return (lightIndex * 0x9E3779B1u) >> 26u;
}

bool StochasticListFilterMayContain(uint2 filter, uint lightIndex)
{
    const uint bit = StochasticListFilterBit(lightIndex);
    return ((bit < 32u ? filter.x : filter.y) & (1u << (bit & 31u))) != 0u;
}

// A candidate's selection importance: the log-compressed luminance of its unshadowed contribution (MegaLights), so one
// bright light does not take every sample after tone mapping, or the cheap stand-in for it. 0 out of range. Selection
// probability only; the estimate divides by it.
float StochasticCandidateWeight(LightInfo light, StochasticSurface surface)
{
#if STOCHASTIC_CHEAP_IMPORTANCE
    return StochasticCheapImportance(light, surface);
#else
    DirectLightContribution contribution;
    if (!StochasticEvaluateLightUnshadowed(light, surface, contribution))
    {
        return 0.0f;
    }
    return log2(1.0f + max(StochasticLuminance(contribution.total), 0.0f));
#endif
}

// One local candidate into a set's reservoirs (all fed the same stream).
void StochasticAddCandidate(LightInfo light, uint lightIndex, StochasticSurface surface, uint quadrants, uint sampleCount,
    inout StochasticReservoir reservoirs[STOCHASTIC_MAX_SAMPLE_COUNT], inout float weightSum, inout float linearSum)
{
    const float weight = StochasticCandidateWeight(light, surface);
    if (!(weight > 0.0f))
    {
        return; // Out of range, or facing away.
    }
    weightSum += weight;
    linearSum += exp2(weight) - 1.0f; // The unshadowed bound's (stochasticSplit.hlsli).
    [unroll]
    for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
    {
        if (k < sampleCount) // Uniform: the reservoirs past the sample count are never read.
        {
            StochasticReservoirUpdate(reservoirs[k], lightIndex, weight, weightSum, quadrants);
        }
    }
}

// One directional light into the directional set's reservoir, with its visible-list quadrants if the list has it.
void StochasticAddDirectional(StructuredBuffer<LightInfo> lights, uint lightIndex, StochasticSurface surface,
    RWStructuredBuffer<uint> lists, uint listBase, uint listCount, uint2 listFilter,
    inout StochasticReservoir reservoir, inout float weightSum, inout bool anyListed, inout float linearSum)
{
    const LightInfo light = lights[lightIndex];
    const float weight = StochasticCandidateWeight(light, surface);
    if (!(weight > 0.0f))
    {
        return;
    }
    linearSum += exp2(weight) - 1.0f;
    const uint entry = StochasticListFilterMayContain(listFilter, lightIndex)
        ? StochasticFindListedEntry(lists, listBase, listCount, lightIndex) : STOCHASTIC_NO_KEY;
    const bool listed = entry != STOCHASTIC_NO_KEY && ((entry >> 20u) & 0xFFu) == (light.generation & 0xFFu);
    weightSum += weight;
    anyListed = anyListed || listed;
    StochasticReservoirUpdate(reservoir, lightIndex, weight, weightSum, listed ? max(entry >> 28u, 1u) : 0xFu);
}

// 1: a light chosen by several of a pixel's samples traces one ray for all of them (above), which share its quadrant draw.
// 0: one ray per sample (docs/stochastic_lighting.md "Kernel split": 1.3 ms more at 16 lights, 8% less undenoised noise).
#ifndef STOCHASTIC_DEDUPLICATE_RAYS
#define STOCHASTIC_DEDUPLICATE_RAYS 1
#endif

// One thread per sampled pixel: every pixel at full resolution, one pixel of each 2x2 block with
// STOCHASTIC_DOWNSAMPLE_FACTOR 2. 8x8 groups, the shading kernel's tiles.
[numthreads(STOCHASTIC_TILE_SIZE, STOCHASTIC_TILE_SIZE, 1)]
void StochasticSelectCS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const uint downsample = STOCHASTIC_DOWNSAMPLE_FACTOR > 1u ? 2u : 1u;
    const uint2 screen = uint2(perFrame.screenResX, perFrame.screenResY);
    const uint2 blocks = (screen + downsample - 1u) / downsample;
    const bool inBounds = all(id.xy < blocks);
    const uint2 pixel = StochasticDownsampledPixel(id.xy, CLOD_RT_SEC_FRAME, downsample, screen);
    // The per-pixel sequences advance once per visit of this pixel.
    const uint visit = StochasticDownsampledVisit(CLOD_RT_SEC_FRAME, downsample);
    const bool accumulate = (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_ACCUMULATE) != 0u;
    const bool guiding = STOCHASTIC_GUIDING != 0 && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_GUIDING) != 0u;
    const bool historyValid = guiding && (STOCHASTIC_FLAGS & STOCHASTIC_FLAG_HISTORY_VALID) != 0u;
    const uint tilePixels = STOCHASTIC_TILE_SIZE * downsample;
    const uint2 tiles = (screen + tilePixels - 1u) / tilePixels;
    const uint sampleCount = clamp(STOCHASTIC_SAMPLE_COUNT, 1u, STOCHASTIC_MAX_SAMPLE_COUNT);

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    StochasticSurface surface = (StochasticSurface)0;
    const bool valid = inBounds && StochasticLoadSurface(pixel, perFrame, camera, surface);

    // Guiding and the denoiser validate reprojection against it, at every pixel: a downsampled thread writes its whole block,
    // the pixels it doesn't sample from the depth and normal textures.
    if (inBounds)
    {
        RWTexture2D<uint2> history = ResourceDescriptorHeap[STOCHASTIC_SURFACE_HISTORY_UAV];
        history[pixel] = valid
            ? uint2(asuint(-surface.fragment.fragPosViewSpace.z), StochasticPackNormal(surface.fragment.normalWS))
            : uint2(0x7F7FFFFFu, 0u);
        if (downsample > 1u)
        {
            Texture2D<float> depthTexture = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::LinearDepthMap)];
            Texture2D<float4> normalRoughness = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::NormalRoughness)];
            [unroll]
            for (uint q = 0u; q < 4u; ++q)
            {
                const uint2 other = id.xy * 2u + uint2(q & 1u, q >> 1u);
                if (any(other >= screen) || all(other == pixel))
                {
                    continue;
                }
                const float depth = depthTexture[other];
                history[other] = asuint(depth) != 0x7F7FFFFFu
                    ? uint2(asuint(depth), StochasticPackNormal(normalize(normalRoughness[other].xyz)))
                    : uint2(0x7F7FFFFFu, 0u);
            }
        }
    }


    // Each sample's light and its emitter quadrants' mask (LIGHT_INDEX_NONE: none), and 1 / (N p).
    uint sampleLight[STOCHASTIC_MAX_SAMPLE_COUNT];
    float sampleScale[STOCHASTIC_MAX_SAMPLE_COUNT];
    [unroll]
    for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
    {
        sampleLight[k] = LIGHT_INDEX_NONE;
        sampleScale[k] = 0.0f;
    }
    // The unshadowed bound (stochasticSplit.hlsli "Bits 16-29"): the linear sums of the listed and directional candidates,
    // and of the drawn hidden ones before their M / drawCount.
    float boundListed = 0.0f, boundHidden = 0.0f;
    float3 debugShares = float3(-1.0f, -1.0f, -1.0f); // STOCHASTIC_DEBUG_PROBE 2: directional share, unlisted share, reprojected.
    float3 debugCluster = float3(-1.0f, 0.0f, 0.0f); // STOCHASTIC_DEBUG_PROBE 4: cluster's count, local candidates evaluated, lights in range.
    float debugListCount = -1.0f; // STOCHASTIC_DEBUG_PROBE 5: the reprojected list's length.

    if (valid)
    {
        StochasticRng rng = StochasticRngCreate(pixel, CLOD_RT_SEC_FRAME, 1u);

        // ---- Reproject into last frame's tiles.
        RWStructuredBuffer<uint> previousLists = ResourceDescriptorHeap[STOCHASTIC_PREVIOUS_VISIBLE_LISTS_UAV];
        bool reprojected = false;
        uint listBase = 0u;
        uint listCount = 0u;
        uint2 listFilter = uint2(0u, 0u);
        const float2 tileU = StochasticRngNext2(rng);
        if (historyValid)
        {
            Texture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
            const float2 resolution = float2(perFrame.screenResX, perFrame.screenResY);
            const float2 uv = (float2(pixel) + 0.5f) / resolution;
            const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
            const float2 previousNdc = ndc - motion[pixel];
            const float2 previousPixel = float2(previousNdc.x * 0.5f + 0.5f, 0.5f - previousNdc.y * 0.5f) * resolution;
            if (all(previousPixel >= 0.0f) && all(previousPixel < resolution))
            {
                RWTexture2D<uint2> previousHistory = ResourceDescriptorHeap[STOCHASTIC_PREVIOUS_SURFACE_HISTORY_UAV];
                const uint2 previous = previousHistory[uint2(previousPixel)];
                const float previousDepth = asfloat(previous.x);
                const float expectedDepth = -mul(float4(surface.position, 1.0f), camera.prevView).z;
                const bool depthHolds = previous.x != 0x7F7FFFFFu &&
                    abs(previousDepth - expectedDepth) <= STOCHASTIC_REPROJECTION_DEPTH_TOLERANCE * max(expectedDepth, 1e-3f);
                const bool normalHolds = dot(StochasticUnpackNormal(previous.y), surface.fragment.normalWS) >= STOCHASTIC_REPROJECTION_NORMAL_COSINE;
                if (depthHolds && normalHolds)
                {
                    // One of the four nearest tiles, by bilinear weight, so list changes don't show tile edges.
                    const float2 tileCoordinate = previousPixel / float(tilePixels) - 0.5f;
                    const float2 tileFloor = floor(tileCoordinate);
                    const float2 fraction = tileCoordinate - tileFloor;
                    const int2 tile = clamp(int2(tileFloor) + int2(tileU.x < fraction.x ? 1 : 0, tileU.y < fraction.y ? 1 : 0),
                        int2(0, 0), int2(tiles) - 1);
                    listBase = (uint(tile.y) * tiles.x + uint(tile.x)) * STOCHASTIC_VISIBLE_LIST_STRIDE;
                    listCount = min(previousLists[listBase], STOCHASTIC_VISIBLE_LIST_CAPACITY);
                    reprojected = true;
                    debugListCount = float(listCount);
                    for (uint entry = 0u; entry < listCount; ++entry)
                    {
                        const uint bit = StochasticListFilterBit(previousLists[listBase + 1u + entry] & STOCHASTIC_ENTRY_INDEX_MASK);
                        if (bit < 32u) listFilter.x |= 1u << bit;
                        else listFilter.y |= 1u << (bit - 32u);
                    }
                }
            }
        }

        // ---- Reservoirs for the listed and hidden local sets, each with its own stratified random numbers.
        const float visibleStratum = StochasticTemporalSequence(pixel, visit, 0u);
        const float hiddenStratum = StochasticTemporalSequence(pixel, visit, 1u);
        StochasticReservoir visibleReservoirs[STOCHASTIC_MAX_SAMPLE_COUNT];
        StochasticReservoir hiddenReservoirs[STOCHASTIC_MAX_SAMPLE_COUNT];
        [unroll]
        for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
        {
            // u_k = frac(u + k / N): the N picks of each set fall in different strata of its distribution, and each u_k is
            // uniform on its own. That matters because which sample takes which set is itself stratified (below): with
            // u_k = (k + u) / N, the sample that took a set would always read the same strata of it, a bias.
            visibleReservoirs[k] = StochasticReservoirCreate(frac(visibleStratum + float(k) / float(sampleCount)));
            hiddenReservoirs[k] = StochasticReservoirCreate(frac(hiddenStratum + float(k) / float(sampleCount)));
        }
        float visibleWeightSum = 0.0f;
        float hiddenWeightSum = 0.0f;
        // Directional lights: one reservoir every sample that takes the set shares. Scenes have one sun, almost always.
        StochasticReservoir directionalReservoir = StochasticReservoirCreate(StochasticTemporalSequence(pixel, visit, 3u));
        float directionalWeightSum = 0.0f;
        bool directionalListed = false;

        StructuredBuffer<uint> gridIndices = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::GridIndices)];
        const LightGridRange lightRange = StochasticLoadLightRange(surface, perFrame, camera);
        const uint clusterCount = lightRange.count;
        debugCluster.x = float(clusterCount);
        const uint drawCount = STOCHASTIC_CANDIDATE_COUNT == 0u ? clusterCount : min(clusterCount, (uint)STOCHASTIC_CANDIDATE_COUNT);
        const float drawStride = drawCount > 0u ? float(clusterCount) / float(drawCount) : 1.0f;
        // When the draw takes the whole cluster (M <= STOCHASTIC_CANDIDATE_COUNT), it walks the cluster once and splits it
        // into the three sets, as the unbounded walk did; the list loop and the directional lookup below are skipped.
        const bool takeAll = drawCount == clusterCount;

        // ---- Directional lights. They are in every cluster, but a bounded draw takes only some of the cluster's entries, so
        // they are taken from the frame's sun index (the only directional light when there is one), or found among the
        // active lights when there are several.
        if (!takeAll && perFrame.numDirectionalLights == 1u && perFrame.sunLightIndex != LIGHT_INDEX_NONE)
        {
            StochasticAddDirectional(lights, perFrame.sunLightIndex, surface, previousLists, listBase, listCount, listFilter,
                directionalReservoir, directionalWeightSum, directionalListed, boundListed);
        }
        else if (!takeAll && perFrame.numDirectionalLights > 1u)
        {
            StructuredBuffer<uint> activeLights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
            for (uint a = 0u; a < perFrame.numLights; ++a)
            {
                const uint lightIndex = activeLights[a];
                if (lights[lightIndex].type == LIGHT_TYPE_DIRECTIONAL)
                {
                    StochasticAddDirectional(lights, lightIndex, surface, previousLists, listBase, listCount, listFilter,
                        directionalReservoir, directionalWeightSum, directionalListed, boundListed);
                }
            }
        }

        // ---- Local lights: first every entry of the reprojected visible list (the listed set), then a stratified draw of
        // drawCount of the cluster's M entries (the hidden set; docs/stochastic_lighting.md "Bounded candidates"). Entry i
        // of the draw is floor(offset + i * stride), stride = M / drawCount, offset uniform in [0, stride): every cluster
        // entry is drawn with probability exactly drawCount / M. Drawn entries that are listed or directional belong to the
        // other sets and are skipped. With M <= drawCount every entry is taken, as an unbounded walk would.
        // One offset per tile and frame, not per pixel: pixels that share a cluster then draw the same entries, so a wave's
        // loads of grid indices and light records coincide (per-pixel offsets cost 3 ms at 1024 lights). Each pixel's offset is
        // still uniform and independent of its other numbers, which is all the estimate needs.
#if STOCHASTIC_DRAW_OFFSET_PER_PIXEL
        StochasticRng tileRng = StochasticRngCreate(pixel, CLOD_RT_SEC_FRAME, 7u);
#else
        StochasticRng tileRng = StochasticRngCreate(groupId.xy, CLOD_RT_SEC_FRAME, 7u);
#endif
        const float drawOffset = StochasticRngNext(tileRng) * drawStride;
        // The listed set: the reprojected list's entries, each evaluated (a listed light that doesn't reach this pixel gets
        // weight 0). An entry whose generation differs is another light that has since taken the slot: not listed, and
        // the draw below treats it as hidden.
        for (uint e = 0u; e < (takeAll ? 0u : listCount); ++e)
        {
            const uint entry = previousLists[listBase + 1u + e];
            const uint lightIndex = entry & STOCHASTIC_ENTRY_INDEX_MASK;
            const LightInfo light = lights[lightIndex];
            if (((entry >> 20u) & 0xFFu) != (light.generation & 0xFFu) || light.type == LIGHT_TYPE_DIRECTIONAL)
            {
                continue;
            }
            debugCluster.y += 1.0f;
            StochasticAddCandidate(light, lightIndex, surface, max(entry >> 28u, 1u), sampleCount, visibleReservoirs, visibleWeightSum, boundListed);
        }
        // The hidden set: the drawn froxel entries that are neither listed nor directional, read straight from the froxel's
        // contiguous run in the light grid. Every drawn entry carries the same factor M / drawCount, so the reservoirs
        // choose by w alone and the factor enters hiddenWeightSum after the loop.
        for (uint d = 0u; d < drawCount; ++d)
        {
            const uint clusterEntry = min(uint(drawOffset + float(d) * drawStride), clusterCount - 1u);
            const uint lightIndex = gridIndices[lightRange.first + clusterEntry];
            const uint entry = StochasticListFilterMayContain(listFilter, lightIndex)
                ? StochasticFindListedEntry(previousLists, listBase, listCount, lightIndex) : STOCHASTIC_NO_KEY;
            const LightInfo light = lights[lightIndex];
            const bool listed = entry != STOCHASTIC_NO_KEY && ((entry >> 20u) & 0xFFu) == (light.generation & 0xFFu);
            if (light.type == LIGHT_TYPE_DIRECTIONAL)
            {
                if (takeAll)
                {
                    const float weight = StochasticCandidateWeight(light, surface);
                    if (weight > 0.0f)
                    {
                        directionalWeightSum += weight;
                        boundListed += exp2(weight) - 1.0f;
                        directionalListed = directionalListed || listed;
                        StochasticReservoirUpdate(directionalReservoir, lightIndex, weight, directionalWeightSum, listed ? max(entry >> 28u, 1u) : 0xFu);
                    }
                }
                continue; // The directional set's: taken here when the draw takes every entry, else from the sun index above.
            }
            if (listed && !takeAll)
            {
                continue; // The listed set's: the loop above evaluated it.
            }
            debugCluster.y += 1.0f;
            if (listed)
            {
                StochasticAddCandidate(light, lightIndex, surface, max(entry >> 28u, 1u), sampleCount, visibleReservoirs, visibleWeightSum, boundListed);
            }
            else
            {
                StochasticAddCandidate(light, lightIndex, surface, 0xFu, sampleCount, hiddenReservoirs, hiddenWeightSum, boundHidden);
            }
        }
        // W_hidden estimated from the draw: sum of w * M / drawCount over the drawn hidden lights, unbiased for the hidden
        // set's total. A hidden pick's probability below, (1 - delta) alpha w / W_hidden, is then its RIS contribution
        // weight, and the estimate stays unbiased (alpha > 0 whenever W_hidden > 0).
        hiddenWeightSum *= drawCount > 0u ? float(clusterCount) / float(drawCount) : 1.0f;
        boundHidden *= drawCount > 0u ? float(clusterCount) / float(drawCount) : 1.0f;

        // ---- The unlisted local lights' share of the local samples. Their own share of the local weight, capped at
        // STOCHASTIC_HIDDEN_SHARE so an occluded bright light doesn't take every ray; the cap relaxes when listed lights
        // carry little of the weight (the list is likely stale), and is at least STOCHASTIC_HIDDEN_SHARE_UNGUIDED where
        // reprojection failed.
        const float localWeight = visibleWeightSum + hiddenWeightSum;
        float hiddenShare = 0.0f;
        if (localWeight > 0.0f)
        {
            const float rawShare = hiddenWeightSum / localWeight;
            float cap = max(STOCHASTIC_HIDDEN_SHARE, 1.0f - 4.0f * (visibleWeightSum / localWeight));
            if (!reprojected)
            {
                cap = max(cap, STOCHASTIC_HIDDEN_SHARE_UNGUIDED);
            }
            hiddenShare = visibleWeightSum > 0.0f ? min(rawShare, cap) : 1.0f;
            hiddenShare = hiddenWeightSum > 0.0f ? hiddenShare : 0.0f;
        }

        // ---- Directional lights' share of all samples. Their share of the weight, capped at STOCHASTIC_DIRECTIONAL_SHARE
        // so the sun leaves local lights their samples, or at STOCHASTIC_HIDDEN_SHARE where reprojection held and the
        // tile's list doesn't have it (it was occluded there last frame). The cap relaxes when local lights carry little
        // of the weight, and with none the sun takes every sample.
        const float totalWeight = localWeight + directionalWeightSum;
        float directionalShare = 0.0f;
        if (directionalWeightSum > 0.0f)
        {
            const float capBase = (directionalListed || !reprojected) ? STOCHASTIC_DIRECTIONAL_SHARE : STOCHASTIC_HIDDEN_SHARE;
            const float cap = max(capBase, 1.0f - 4.0f * (localWeight / totalWeight));
            directionalShare = localWeight > 0.0f ? min(directionalWeightSum / totalWeight, cap) : 1.0f;
        }
        debugShares = float3(directionalShare, hiddenShare, reprojected ? 1.0f : 0.0f);

        // ---- Trace and shade each sample: f * V / (N p), p the mixture's probability of the light times the emitter
        // quadrant's density correction. One stratified number per sample picks its set: [0, directionalShare) the
        // directional set, then the listed and the unlisted local lights. The N numbers fall in different strata, so the
        // number of samples each set gets varies by at most one (a sunlit pixel isn't noisy from the choice alone).
        const float setStratum = StochasticTemporalSequence(pixel, visit, 2u);
        [unroll]
        for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
        {
            if (k >= sampleCount || totalWeight <= 0.0f)
            {
                continue;
            }
#if STOCHASTIC_SORTED_SETS
            const float setU = (float(k) + frac(setStratum * float(sampleCount))) / float(sampleCount);
#else
            const float setU = frac(setStratum + float(k) / float(sampleCount));
#endif
            const bool directional = setU < directionalShare;
            const float localU = (setU - directionalShare) / max(1.0f - directionalShare, 1e-7f);
            const bool hidden = !directional && localU >= 1.0f - hiddenShare;
            StochasticReservoir reservoir = visibleReservoirs[k];
            if (hidden)
            {
                reservoir = hiddenReservoirs[k];
            }
            if (directional)
            {
                reservoir = directionalReservoir;
            }
            if (reservoir.lightIndex == LIGHT_INDEX_NONE)
            {
                continue;
            }
            const float probability = directional
                ? directionalShare * reservoir.weight / directionalWeightSum
                : (1.0f - directionalShare) * (hidden
                    ? hiddenShare * reservoir.weight / hiddenWeightSum
                    : (1.0f - hiddenShare) * reservoir.weight / visibleWeightSum);
            sampleLight[k] = (reservoir.lightIndex & STOCHASTIC_ENTRY_INDEX_MASK) | (reservoir.quadrants << 20u);
            sampleScale[k] = 1.0f / (probability * float(sampleCount));
        }

        // ---- Each slot: its light's unshadowed estimate, evaluated once, and its shadow ray, appended to the list slot by slot
        // (per wave: a trace wave then holds the same sample of neighbouring pixels, which with sorted set numbers is mostly
        // one set). With STOCHASTIC_DEDUPLICATE_RAYS, a light an earlier sample chose is that sample's: the same set, so the
        // same probability and quadrant weights, and that sample's ray has the distribution this one's would have had. The
        // first sample of a light takes the 1 / (N p) of every sample that chose it.
        RWStructuredBuffer<uint3> slots = ResourceDescriptorHeap[STOCHASTIC_SLOTS_UAV];
        RWStructuredBuffer<uint> rayCounter = ResourceDescriptorHeap[STOCHASTIC_RAY_COUNTER_UAV];
        RWStructuredBuffer<uint3> rays = ResourceDescriptorHeap[STOCHASTIC_RAYS_UAV];
        const uint slotBase = (id.y * blocks.x + id.x) * sampleCount;
        [unroll]
        for (uint k = 0; k < STOCHASTIC_MAX_SAMPLE_COUNT; ++k)
        {
            if (k >= sampleCount)
            {
                continue;
            }
            uint3 slot = uint3(0u, 0u, 0u);
            bool traced = false;
            RayDesc ray = (RayDesc)0;
            uint lightIndex = sampleLight[k] & STOCHASTIC_ENTRY_INDEX_MASK;
            float scale = sampleScale[k];
#if STOCHASTIC_DEDUPLICATE_RAYS
            [unroll]
            for (uint j = 0; j < STOCHASTIC_MAX_SAMPLE_COUNT; ++j)
            {
                if (j < k && sampleLight[j] == sampleLight[k])
                {
                    lightIndex = LIGHT_INDEX_NONE; // An earlier sample's ray serves this one.
                }
                if (j > k && j < sampleCount && sampleLight[j] == sampleLight[k])
                {
                    scale += sampleScale[j];
                }
            }
#endif
            if (sampleLight[k] != LIGHT_INDEX_NONE && lightIndex != LIGHT_INDEX_NONE)
            {
                // Unlisted lights have all four quadrants, which is uniform sampling (scale 1).
                float2 u = StochasticRngNext2(rng);
                uint quadrant = 0u;
                scale *= StochasticSampleQuadrant(sampleLight[k] >> 20u, u, quadrant);
                const LightInfo light = lights[lightIndex];
                DirectLightContribution contribution;
                StochasticEvaluateLightUnshadowed(light, surface, contribution);
                const float3 sampleDiffuse = contribution.diffuse * scale;
                const float3 sampleSpecular = contribution.specular * scale;
                if (any(sampleDiffuse + sampleSpecular > 0.0f))
                {
                    traced = light.shadowCaster != 0u && StochasticLightShadowRay(light, surface, u, ray);
                    // The packing rounds stochastically, so the stored estimate's expectation is unchanged.
                    const float rounding = StochasticRngNext(rng);
                    slot = uint3(lightIndex | (quadrant << STOCHASTIC_SLOT_QUADRANT_SHIFT) | STOCHASTIC_SLOT_USED
                            | (traced ? STOCHASTIC_SLOT_RAY : 0u),
                        StochasticPackRgb9e5(sampleDiffuse, rounding), StochasticPackRgb9e5(sampleSpecular, rounding));
                }
            }
            slots[slotBase + k] = slot;
#if defined(STOCHASTIC_DEBUG_PROBE) && (STOCHASTIC_DEBUG_PROBE == 1 || STOCHASTIC_DEBUG_PROBE == 3)
            // The ray probes run in the trace kernel (compiled with the same probe): one entry per sampled pixel with a
            // surface and no shadow rays.
            traced = k == 0u;
#endif
            // The wave's lanes still here (pixels with a surface) append their rays for this slot together.
            const uint waveRays = WaveActiveCountBits(traced);
            if (waveRays > 0u)
            {
                uint rayBase = 0u;
                if (WaveIsFirstLane())
                {
                    InterlockedAdd(rayCounter[0], waveRays, rayBase);
                }
                const uint index = WaveReadLaneFirst(rayBase) + WavePrefixCountBits(traced);
                if (traced && index < STOCHASTIC_RAY_CAPACITY)
                {
                    rays[index] = uint3(StochasticPackRayPixel(pixel, k), StochasticPackNormal(ray.Direction), asuint(ray.TMax));
                }
            }
        }
    }

    // ---- The pixel word: surface valid, and the visibility bits cleared for the trace to set.
    uint pixelWord = valid ? STOCHASTIC_PIXEL_VALID | StochasticEncodeBound(boundListed + boundHidden) : 0u;
#if defined(STOCHASTIC_DEBUG_PROBE) && STOCHASTIC_DEBUG_PROBE == 5
    pixelWord |= uint(debugListCount + 1.0f) << STOCHASTIC_PIXEL_LIST_SHIFT;
#elif defined(STOCHASTIC_DEBUG_PROBE) && (STOCHASTIC_DEBUG_PROBE == 1 || STOCHASTIC_DEBUG_PROBE == 3)
    pixelWord |= valid ? STOCHASTIC_PIXEL_DEBUG : 0u; // Shading leaves these pixels to the trace's output.
#endif

    if (!inBounds)
    {
        return;
    }


#if defined(STOCHASTIC_DEBUG_PROBE) && (STOCHASTIC_DEBUG_PROBE == 2 || STOCHASTIC_DEBUG_PROBE == 4)
    // Diagnostics written here, where their values are (pso.recompile on Stochastic.Select.PSO, denoiser off); shading leaves
    // the pixel alone.
    float3 debugDiffuse = 0.0f;
#if STOCHASTIC_DEBUG_PROBE == 2
    // The pixel's set shares (directional, unlisted local, 1 where reprojection held; -1 without a surface).
    debugDiffuse = debugShares;
#else
    // The pixel's light grid froxel: its opaque light count, the local candidates evaluated (listed and drawn), and (brute
    // force over every active light) the lights whose range reaches the surface. z / x is how tight the culling is; a froxel
    // count below the lights reaching means culling dropped lights.
    if (valid)
    {
        StructuredBuffer<uint> probeActive = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::ActiveLightIndices)];
        for (uint a = 0u; a < perFrame.numLights; ++a)
        {
            const LightInfo probeLight = lights[probeActive[a]];
            debugCluster.z += LightInRange(probeLight, EvaluateLightAtPoint(probeLight, surface.position)) ? 1.0f : 0.0f;
        }
    }
    debugDiffuse = debugCluster;
#endif
    pixelWord |= STOCHASTIC_PIXEL_DEBUG;
    StochasticWriteDebugOutput(id.xy, pixel, downsample, screen, debugDiffuse, 0.0f);
#endif
    RWStructuredBuffer<uint> pixelWords = ResourceDescriptorHeap[STOCHASTIC_PIXEL_WORDS_UAV];
    pixelWords[id.y * blocks.x + id.x] = pixelWord;
}
