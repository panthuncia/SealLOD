// The layered path's composite: one thread per pixel blends the pixel's hair layers over the lit frame.
//
// A pixel holds up to three keys, nearest first (hairRaster.hlsl). For each the shader finds the hit again
// (HairReconstructHit), takes the share of the pixel that segment covers exactly as the rasterizer counted it, and the
// colour the shade pass gave the segment's two vertices, interpolated at the hit. The three are blended front to back.
//
// Hair beyond the third layer is in the coverage target only: the summed coverage of every segment over the pixel. What
// the three layers do not account for of that sum is the remainder. It covers exp(-remainder) less of what is left
// behind the three layers (thin occluders that overlap at random), and is lit as a weighted average of the three
// samples that favours the farther ones, with the weights of "Strand Hair in Indiana Jones and the Great Circle"
// (SIGGRAPH 2025): the layer's coverage times 0.25 to the power of its distance from the farthest layer. A pixel with
// three segments or fewer has no remainder, and its blend is exact.
//
// Where the hair's alpha is at least HairFrameGPU::depthAlpha the pixel takes hair's depth and motion vector, those of
// its nearest layer, in the forms the passes after this one read (the upscaler, motion vector dilation, the lens
// passes); elsewhere it keeps those of what is behind the hair. The reactive mask tells the upscaler where that one
// motion vector does not describe the whole pixel.

#include "HairShaders/hairCommon.hlsli"

// HairCompositePass::Record. The camera, the scene colour and the motion vectors are bound by name.
#define HAIR_VISIBILITY UintRootConstant0        // Texture2D<uint64_t>: the nearest layer
#define HAIR_VISIBLE_CLUSTERS UintRootConstant1  // StructuredBuffer<HairVisibleCluster>
#define HAIR_FRAME_STATE UintRootConstant2       // StructuredBuffer<HairFrameGPU>
#define HAIR_POOL_VERTICES UintRootConstant3     // StructuredBuffer<HairPoolVertex>
#define HAIR_LAYER1 UintRootConstant4            // Texture2D<uint64_t>
#define HAIR_LAYER2 UintRootConstant5            // Texture2D<uint64_t>
#define HAIR_COVERAGE UintRootConstant6          // Texture2D<uint>
#define HAIR_VISIBILITY_WIDTH UintRootConstant7
#define HAIR_VISIBILITY_HEIGHT UintRootConstant8
#define HAIR_COUNTERS UintRootConstant9          // RWStructuredBuffer<uint>
#define HAIR_POOL_COLORS UintRootConstant10      // StructuredBuffer<uint2>
#define HAIR_COMPOSITE_INFO UintRootConstant11   // RWTexture2D<float2>
#define HAIR_REACTIVE_MASK UintRootConstant12    // RWTexture2D<float>
#define HAIR_PROJECTED_DEPTH UintRootConstant13  // RWTexture2D<float>
#define HAIR_COVERAGE_FAR UintRootConstant14     // Texture2D<uint>: the culled rasterizer's coverage behind a pixel's split depth
#define HAIR_OCCLUSION UintRootConstant15        // Texture2D<uint64_t>: the culled rasterizer's hidden depths

#include "HairShaders/hairHit.hlsli"
#include "HairShaders/hairShading.hlsli"

// A coverage sum this far past the three layers' own is a remainder; below it, it is the rounding of the three adds.
static const float kHairRemainderEpsilon = 1.0e-3f;

[shader("compute")]
[numthreads(8, 8, 1)]
void HairCompositeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= HAIR_VISIBILITY_WIDTH || pixel.y >= HAIR_VISIBILITY_HEIGHT)
    {
        return;
    }
    Texture2D<uint64_t> layer0 = ResourceDescriptorHeap[HAIR_VISIBILITY];
    Texture2D<uint64_t> layer1 = ResourceDescriptorHeap[HAIR_LAYER1];
    Texture2D<uint64_t> layer2 = ResourceDescriptorHeap[HAIR_LAYER2];
    RWTexture2D<float2> compositeInfo = ResourceDescriptorHeap[HAIR_COMPOSITE_INFO];
    RWTexture2D<float> reactiveMask = ResourceDescriptorHeap[HAIR_REACTIVE_MASK];
    const uint64_t keys[HAIR_LAYER_COUNT] = { layer0[pixel], layer1[pixel], layer2[pixel] };
    if (keys[0] == 0xFFFFFFFFFFFFFFFFull)
    {
        compositeInfo[pixel] = 0.0f.xx;
        reactiveMask[pixel] = 0.0f;
        return;
    }

    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    StructuredBuffer<uint2> poolColors = ResourceDescriptorHeap[HAIR_POOL_COLORS];
    const HairFrameGPU frame = frameState[0];
    const Camera camera = HairLoadMainCamera();

    // The layer's coverage times 0.25 ^ (2 - layer): the remainder is lit mostly as the farthest layer is.
    static const float kRemainderWeights[HAIR_LAYER_COUNT] = { 0.0625f, 0.25f, 1.0f };
    float3 radiance = 0.0f.xxx; // premultiplied
    float transmittance = 1.0f;
    float layerCoverage = 0.0f;
    float3 weightedColor = 0.0f.xxx;
    float weight = 0.0f;
    uint layers = 0u;
    float3 nearestPosition = 0.0f.xxx;
    float3 nearestPreviousPosition = 0.0f.xxx;
    for (uint layer = 0u; layer < HAIR_LAYER_COUNT && keys[layer] != 0xFFFFFFFFFFFFFFFFull; ++layer)
    {
        // Behind a layer that covers the pixel whole nothing shows: the layer is counted and not looked up.
        if (!(transmittance > 0.0f))
        {
            ++layers;
            continue;
        }
        const HairHit hit = HairReconstructHit(pixel, keys[layer], camera);
        const float3 color =
            lerp(HairUnpackColor(poolColors[hit.poolRow]), HairUnpackColor(poolColors[hit.poolRow + 1u]), hit.parameter);
        radiance += transmittance * hit.coverage * color;
        transmittance *= 1.0f - hit.coverage;
        layerCoverage += hit.coverage;
        weightedColor += hit.coverage * kRemainderWeights[layer] * color;
        weight += hit.coverage * kRemainderWeights[layer];
        if (layer == 0u)
        {
            nearestPosition = hit.position;
            nearestPreviousPosition = hit.previousPosition;
        }
        ++layers;
    }

    float remainderAlpha = 0.0f;
    bool hasRemainder = false;
    if (layers == HAIR_LAYER_COUNT)
    {
        Texture2D<uint> hairCoverage = ResourceDescriptorHeap[HAIR_COVERAGE];
        // The culled rasterizer sums coverage apart in front of and behind a pixel's split depth (hairRasterCulled.hlsl);
        // the direct one leaves the second sum zero. A strand that covers the pixel whole counts HAIR_COVERAGE_OPAQUE
        // there: in a layer it has already made the three opaque, and behind them it makes the remainder so.
        Texture2D<uint> hairCoverageFar = ResourceDescriptorHeap[HAIR_COVERAGE_FAR];
        float remainder = ((float)hairCoverage[pixel] + (float)hairCoverageFar[pixel]) * (1.0f / HAIR_COVERAGE_UNIT) - layerCoverage;
        // A pixel whose hair lowered its hidden depth is opaque from that depth on (a strand that covers it whole, or
        // saturated coverage): what was skipped behind it is not in the sums, and does not need to be.
        if ((frame.rasterFlags & HAIR_RASTER_FLAG_CULLED) != 0u)
        {
            Texture2D<uint64_t> occlusion = ResourceDescriptorHeap[HAIR_OCCLUSION];
            if (((uint)occlusion[pixel] & HAIR_OCCLUSION_SATURATED) != 0u)
            {
                remainder = max(remainder, HAIR_COVERAGE_OPAQUE);
            }
        }
        if (remainder > kHairRemainderEpsilon && weight > 0.0f)
        {
            hasRemainder = true;
            remainderAlpha = transmittance * (1.0f - exp(-remainder));
            radiance += remainderAlpha * weightedColor / weight;
            transmittance -= remainderAlpha;
        }
    }
    const float alpha = saturate(1.0f - transmittance);

    // Premultiplied over the scene colour.
    RWTexture2D<float4> hdrTarget = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Color::HDRColorTarget)];
    const float4 behind = hdrTarget[pixel];
    hdrTarget[pixel] = float4(radiance + behind.rgb * (1.0f - alpha), alpha + behind.a * (1.0f - alpha));

    // Hair's motion vector, the nearest layer's: current minus previous position in NDC, both without jitter, as the
    // renderer's own surfaces write it (ComputeClodMotionVectorFromPrevWorld).
    RWTexture2D<float2> motion = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Surface::Motion)];
    const float4 clip = mul(mul(float4(nearestPosition, 1.0f), camera.view), camera.unjitteredProjection);
    const float4 previousClip = mul(mul(float4(nearestPreviousPosition, 1.0f), camera.prevView), camera.prevUnjitteredProjection);
    const float2 hairMotion = clip.xy / clip.w - previousClip.xy / previousClip.w;
    const float2 behindMotion = motion[pixel];
    const bool takesDepth = alpha >= frame.depthAlpha;
    if (takesDepth)
    {
        // Device depth for the unjittered projection, from the nearest key's depth (PerViewPrimaryDepthCopyCS).
        RWTexture2D<float> projectedDepthTexture = ResourceDescriptorHeap[HAIR_PROJECTED_DEPTH];
        const float depth = asfloat(HairVisKeyDepthBits(keys[0]) << 1u);
        motion[pixel] = hairMotion;
        projectedDepthTexture[pixel] = -camera.unjitteredProjection[2][2] + camera.unjitteredProjection[3][2] / depth;
    }

    // Reactive where part of the pixel does not move as its motion vector says: the share of the pixel that is not what
    // the vector belongs to, scaled by how far apart hair and what is behind it move, in pixels.
    const float2 motionDifference = (hairMotion - behindMotion) * 0.5f * float2((float)HAIR_VISIBILITY_WIDTH, (float)HAIR_VISIBILITY_HEIGHT);
    const float unfollowed = takesDepth ? 1.0f - alpha : alpha;
    reactiveMask[pixel] = saturate(frame.reactiveStrength * unfollowed * saturate(length(motionDifference) / frame.reactivePixels));
    const uint infoBits = layers | (hasRemainder ? HAIR_COMPOSITE_INFO_REMAINDER : 0u) | (takesDepth ? HAIR_COMPOSITE_INFO_DEPTH : 0u);
    compositeInfo[pixel] = float2(alpha, (float)infoBits * (1.0f / 255.0f));

    // The frame's counts, one atomic per wave each.
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const uint oneLayer = WaveActiveCountBits(layers == 1u);
    const uint twoLayers = WaveActiveCountBits(layers == 2u);
    const uint threeLayers = WaveActiveCountBits(layers == 3u);
    const uint remainderPixels = WaveActiveCountBits(hasRemainder);
    const uint depthPixels = WaveActiveCountBits(takesDepth);
    const float remainderSum = WaveActiveSum(remainderAlpha);
    const float alphaSum = WaveActiveSum(alpha);
    const uint hairPixels = WaveActiveCountBits(true);
    if (WaveIsFirstLane())
    {
        // On this path the pixels the composite blended are the pixels hair was resolved in.
        InterlockedAdd(counters[HAIR_COUNTER_PIXELS_RESOLVED], hairPixels);
        if (oneLayer != 0u) InterlockedAdd(counters[HAIR_COUNTER_LAYER_PIXELS], oneLayer);
        if (twoLayers != 0u) InterlockedAdd(counters[HAIR_COUNTER_LAYER_PIXELS + 1u], twoLayers);
        if (threeLayers != 0u) InterlockedAdd(counters[HAIR_COUNTER_LAYER_PIXELS + 2u], threeLayers);
        if (remainderPixels != 0u) InterlockedAdd(counters[HAIR_COUNTER_REMAINDER_PIXELS], remainderPixels);
        if (depthPixels != 0u) InterlockedAdd(counters[HAIR_COUNTER_DEPTH_PIXELS], depthPixels);
        InterlockedAdd(counters[HAIR_COUNTER_REMAINDER_ALPHA], (uint)(remainderSum * HAIR_ALPHA_COUNTER_UNIT + 0.5f));
        InterlockedAdd(counters[HAIR_COUNTER_ALPHA_SUM], (uint)(alphaSum * HAIR_ALPHA_COUNTER_UNIT + 0.5f));
    }
}
