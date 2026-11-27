// Debug view of the hair visibility target: a full-screen pass over the presentation colour, after tonemapping, that
// colours every pixel holding a hair segment and leaves the rest of the frame as it is. It is how hair is seen before
// the resolve into the canonical surface exists.

#include "HairShaders/hairCommon.hlsli"
#include "fullscreenVS.hlsli"
#include "include/debugPayload.hlsli"

// HairDebugOverlayPass::Record.
#define HAIR_VISIBILITY UintRootConstant0        // Texture2D<uint64_t>
#define HAIR_VISIBLE_CLUSTERS UintRootConstant1  // StructuredBuffer<HairVisibleCluster>
#define HAIR_INSTANCES UintRootConstant2         // StructuredBuffer<HairInstanceGPU>
#define HAIR_CLUSTERS UintRootConstant3          // StructuredBuffer<HairClusterHeader>
#define HAIR_STRANDS UintRootConstant4           // StructuredBuffer<HairStrandRecord>
#define HAIR_DEBUG_VIEW UintRootConstant5        // HAIR_DEBUG_VIEW_*
#define HAIR_VISIBILITY_WIDTH UintRootConstant6
#define HAIR_VISIBILITY_HEIGHT UintRootConstant7
#define HAIR_OUTPUT_WIDTH UintRootConstant8
#define HAIR_OUTPUT_HEIGHT UintRootConstant9
#define HAIR_COVERAGE UintRootConstant10         // Texture2D<uint>: the coverage target

float4 HairDebugOverlayPS(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    // The target is at render resolution and the presentation colour at output resolution: nearest pixel.
    const float2 uv = input.position.xy / float2((float)HAIR_OUTPUT_WIDTH, (float)HAIR_OUTPUT_HEIGHT);
    const uint2 size = uint2(HAIR_VISIBILITY_WIDTH, HAIR_VISIBILITY_HEIGHT);
    const uint2 pixel = min((uint2)(uv * float2(size)), size - 1u);
    if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_COVERAGE)
    {
        // The whole frame, so that a capture measures it: black where no hair is, grey up to one pixel's worth of
        // coverage (the grey level is the coverage), then from yellow to red by the doublings above that.
        Texture2D<uint> hairCoverage = ResourceDescriptorHeap[HAIR_COVERAGE];
        const float coverage = (float)hairCoverage[pixel] * (1.0f / HAIR_COVERAGE_UNIT);
        const float3 over = lerp(float3(1.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), saturate(log2(max(coverage, 1.0f)) / 6.0f));
        return float4(coverage <= 1.0f ? coverage.xxx : over, 1.0f);
    }
    Texture2D<uint64_t> hairVisibility = ResourceDescriptorHeap[HAIR_VISIBILITY];
    const uint64_t key = hairVisibility[pixel];
    if (key == 0xFFFFFFFFFFFFFFFFull)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    uint visibleIndex;
    uint strandIndex;
    uint segmentIndex;
    HairUnpackSegmentId((uint)key, visibleIndex, strandIndex, segmentIndex);
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    const HairVisibleCluster visible = visibleClusters[visibleIndex];

    float3 color = float3(1.0f, 1.0f, 1.0f);
    if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_CLUSTER)
    {
        color = max(HashToColor(visible.instanceIndex * 0x9E3779B9u + visible.clusterIndex + 1u), 0.2f.xxx);
    }
    else if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_DETAIL)
    {
        color = float3(1.0f - visible.detailLevel, visible.detailLevel, 0.1f);
    }
    else if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_STRAND)
    {
        color = max(HashToColor((visible.instanceIndex * 0x9E3779B9u + visible.clusterIndex) * 64u + strandIndex + 1u),
            0.2f.xxx);
    }
    else if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_RANK)
    {
        StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
        StructuredBuffer<HairClusterHeader> clusters = ResourceDescriptorHeap[HAIR_CLUSTERS];
        StructuredBuffer<HairStrandRecord> strands = ResourceDescriptorHeap[HAIR_STRANDS];
        const HairInstanceGPU instance = instances[visible.instanceIndex];
        const HairClusterHeader cluster = clusters[instance.clusterBase + visible.clusterIndex];
        const uint rank = strands[instance.strandBase + cluster.strandOffset + strandIndex].rank;
        const float kept = 1.0f - (float)rank / (float)max(instance.strandCount, 1u);
        color = lerp(float3(0.15f, 0.1f, 0.6f), float3(1.0f, 1.0f, 1.0f), kept * kept);
    }
    else if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_VERTEX_LEVEL)
    {
        // Hue by drawn level; darker as the geomorph nears the level below. Odd segments are dimmed to show the vertices.
        color = max(HashToColor(visible.vertexLevel + 101u), 0.25f.xxx) * (1.0f - 0.5f * visible.geomorph);
        color *= (segmentIndex & 1u) != 0u ? 0.6f : 1.0f;
    }
    return float4(color, 1.0f);
}
