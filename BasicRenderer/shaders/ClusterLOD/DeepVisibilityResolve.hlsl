#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/lighting.hlsli"
#include "include/outputTypes.hlsli"
#include "include/debugPayload.hlsli"
#include "include/clodResolveCommon.hlsli"
#include "include/waveIntrinsicsHelpers.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"
#include "PerPassRootConstants/clodDeepVisibilityResolveRootConstants.h"

#define CLOD_DEEP_VISIBILITY_LIST_NULL 0xffffffffu
#define CLOD_DEEP_VISIBILITY_MAX_SAMPLES 64u
#define CLOD_DEEP_VISIBILITY_EARLY_ALPHA 0.995f

struct DeepVisibilityResolveSample
{
    uint64_t visKey;
    uint flags;
    float depth;
};

bool IsTransparentDebugOutput(uint outputType)
{
    return outputType == OUTPUT_TRANSPARENT_NODE_COUNT ||
        outputType == OUTPUT_TRANSPARENT_DEPTH_COMPLEXITY ||
        outputType == OUTPUT_TRANSPARENT_RESOLVED_SAMPLE_COUNT;
}

float3 TransparentDepthComplexityHeatmap(uint count)
{
    float t = saturate((float)count / 16.0f);
    return saturate(float3(
        smoothstep(0.0f, 0.35f, t) + smoothstep(0.55f, 0.9f, t),
        smoothstep(0.1f, 0.6f, t),
        1.0f - smoothstep(0.25f, 0.8f, t)));
}

void SortDeepVisibilitySamples(inout DeepVisibilityResolveSample samples[CLOD_DEEP_VISIBILITY_MAX_SAMPLES], uint count)
{
    [loop]
    for (uint i = 1u; i < count; ++i)
    {
        DeepVisibilityResolveSample key = samples[i];
        uint j = i;
        while (j > 0u && samples[j - 1u].depth > key.depth)
        {
            samples[j] = samples[j - 1u];
            --j;
        }
        samples[j] = key;
    }
}

uint2 MakeTransparentDebugPayload(uint outputType, uint rawNodeCount, uint resolvedSampleCount)
{
    switch (outputType)
    {
    case OUTPUT_TRANSPARENT_NODE_COUNT:
        return PackDebugUint(rawNodeCount);
    case OUTPUT_TRANSPARENT_DEPTH_COMPLEXITY:
        return PackDebugFloat3(TransparentDepthComplexityHeatmap(rawNodeCount));
    case OUTPUT_TRANSPARENT_RESOLVED_SAMPLE_COUNT:
        return PackDebugUint(resolvedSampleCount);
    default:
        return uint2(DEBUG_SENTINEL, DEBUG_SENTINEL);
    }
}

void CommitDeepVisibilityStatsWave(
    RWStructuredBuffer<CLodDeepVisibilityStats> statsBuffer,
    uint truncatedPixelContribution,
    uint truncatedNodeContribution,
    uint resolvedSampleContribution,
    uint rawNodeCount,
    uint resolvedSampleCount)
{
    uint waveTruncatedPixels = WaveActiveSum(truncatedPixelContribution);
    uint waveTruncatedNodes = WaveActiveSum(truncatedNodeContribution);
    uint waveResolvedSamples = WaveActiveSum(resolvedSampleContribution);
    uint waveMaxRawNodeCount = WaveActiveMax(rawNodeCount);
    uint waveMaxResolvedSamples = WaveActiveMax(resolvedSampleCount);

    if (!WaveIsFirstLane())
    {
        return;
    }

    if (waveTruncatedPixels > 0u)
    {
        InterlockedAdd(statsBuffer[0].truncatedPixelCount, waveTruncatedPixels);
    }
    if (waveTruncatedNodes > 0u)
    {
        InterlockedAdd(statsBuffer[0].truncatedNodeCount, waveTruncatedNodes);
    }
    if (waveResolvedSamples > 0u)
    {
        InterlockedAdd(statsBuffer[0].totalResolvedSamples, waveResolvedSamples);
    }
    if (waveMaxRawNodeCount > 0u)
    {
        InterlockedMax(statsBuffer[0].maxRawNodeCount, waveMaxRawNodeCount);
    }
    if (waveMaxResolvedSamples > 0u)
    {
        InterlockedMax(statsBuffer[0].maxResolvedSamples, waveMaxResolvedSamples);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CLodDeepVisibilityResolveCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    Texture2D<uint> headPointers = ResourceDescriptorHeap[CLOD_DEEP_VISIBILITY_RESOLVE_HEAD_POINTER_DESCRIPTOR_INDEX];
    RWTexture2D<float4> hdrTarget = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Color::HDRColorTarget)];
    uint width, height;
    hdrTarget.GetDimensions(width, height);

    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
    {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    uint outputType = perFrameBuffer.outputType;
    bool transparentDebugOutput = IsTransparentDebugOutput(outputType);
    uint head = headPointers[pixel];
    if (head == CLOD_DEEP_VISIBILITY_LIST_NULL)
    {
        if (transparentDebugOutput)
        {
            RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
            WriteDebugPixel(debugVisTex, pixel, MakeTransparentDebugPayload(outputType, 0u, 0u));
        }
        return;
    }

    StructuredBuffer<CLodDeepVisibilityNode> nodeBuffer = ResourceDescriptorHeap[CLOD_DEEP_VISIBILITY_RESOLVE_NODE_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodDeepVisibilityStats> statsBuffer = ResourceDescriptorHeap[CLOD_DEEP_VISIBILITY_RESOLVE_STATS_DESCRIPTOR_INDEX];
    uint rawNodeCount = 0u;
    uint gatheredCount = 0u;
    DeepVisibilityResolveSample gathered[CLOD_DEEP_VISIBILITY_MAX_SAMPLES];

    uint current = head;
    [loop]
    while (current != CLOD_DEEP_VISIBILITY_LIST_NULL)
    {
        CLodDeepVisibilityNode node = nodeBuffer[current];
        ++rawNodeCount;

        if (gatheredCount < CLOD_DEEP_VISIBILITY_MAX_SAMPLES)
        {
            float depth;
            uint clusterIndex;
            uint primitiveID;
            UnpackVisKey(node.visKey, depth, clusterIndex, primitiveID);
            gathered[gatheredCount].visKey = node.visKey;
            gathered[gatheredCount].flags = node.flags;
            gathered[gatheredCount].depth = depth;
            ++gatheredCount;
        }

        current = node.next;
    }

    uint truncatedPixelContribution = rawNodeCount > CLOD_DEEP_VISIBILITY_MAX_SAMPLES ? 1u : 0u;
    uint truncatedNodeContribution = rawNodeCount > CLOD_DEEP_VISIBILITY_MAX_SAMPLES ? (rawNodeCount - CLOD_DEEP_VISIBILITY_MAX_SAMPLES) : 0u;

    uint resolvedSampleCount = 0u;
    float3 accumulatedPremultiplied = 0.0f.xxx;
    float accumulatedAlpha = 0.0f;

    if (gatheredCount > 0u)
    {
        StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];

        SortDeepVisibilitySamples(gathered, gatheredCount);

        [loop]
        for (uint i = 0u; i < gatheredCount; ++i)
        {
            bool isBackface = (gathered[i].flags & 0x1u) != 0u;

            ClodShadingSample sample;
            if (!ResolveClodShadingSampleFromVisKeyWithFace(gathered[i].visKey, pixel, isBackface, sample))
            {
                continue;
            }

            FragmentInfo fragmentInfo = (FragmentInfo)0;
            fragmentInfo.pixelCoords = float2(pixel);
            fragmentInfo.fragPosWorldSpace = sample.positionWS;
            fragmentInfo.fragPosViewSpace = sample.positionVS;

            float3 viewWS = normalize(mainCamera.positionWorldSpace.xyz - sample.positionWS);
            FillFragmentInfoDirect(
                fragmentInfo,
                sample.materialInputs,
                viewWS,
                float2(pixel),
                false,
                true,
                !isBackface,
                sample.materialFlags);

            const float3 lighting = lightFragmentColor(
                fragmentInfo,
                mainCamera,
                perFrameBuffer.activeEnvironmentIndex,
                ResourceDescriptorIndex(Builtin::Environment::InfoBuffer),
                DirectLightingOptionsFromRoot());

            float alpha = saturate(fragmentInfo.alpha);
            float remaining = 1.0f - accumulatedAlpha;
            accumulatedPremultiplied += lighting * (alpha * remaining);
            accumulatedAlpha += alpha * remaining;

            ++resolvedSampleCount;
            if (accumulatedAlpha >= CLOD_DEEP_VISIBILITY_EARLY_ALPHA)
            {
                break;
            }
        }
    }

    CommitDeepVisibilityStatsWave(
        statsBuffer,
        truncatedPixelContribution,
        truncatedNodeContribution,
        resolvedSampleCount,
        rawNodeCount,
        resolvedSampleCount);

    if (transparentDebugOutput)
    {
        RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
        WriteDebugPixel(debugVisTex, pixel, MakeTransparentDebugPayload(outputType, rawNodeCount, resolvedSampleCount));
    }

    if (accumulatedAlpha <= 0.0f)
    {
        return;
    }

    float4 existingHdr = hdrTarget[pixel];
    float oneMinusAlpha = 1.0f - accumulatedAlpha;
    float3 compositeRgb = accumulatedPremultiplied + existingHdr.rgb * oneMinusAlpha;
    float compositeAlpha = accumulatedAlpha + existingHdr.a * oneMinusAlpha;
    hdrTarget[pixel] = float4(compositeRgb, compositeAlpha);
}
