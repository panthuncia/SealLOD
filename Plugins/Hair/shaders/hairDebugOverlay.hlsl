// Debug view of the hair visibility target: a full-screen pass over the presentation colour, after tonemapping, that
// colours every pixel holding a hair segment and leaves the rest of the frame as it is. It is how hair is seen before
// the resolve into the canonical surface exists.

#include "HairShaders/hairCommon.hlsli"
#include "HairShaders/hairVolume.hlsli"
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
#define HAIR_COVERAGE_FAR UintRootConstant16     // Texture2D<uint>: the culled rasterizer's second coverage sum
#define HAIR_COMPOSITE_INFO UintRootConstant11   // Texture2D<float2>: the layered path's; HAIR_INVALID_INDEX without it
#define HAIR_REACTIVE_MASK UintRootConstant12    // Texture2D<float>: the layered path's
#define HAIR_VOLUMES UintRootConstant13          // StructuredBuffer<HairVolumeGPU>: the density volumes
#define HAIR_VOXELS UintRootConstant14           // StructuredBuffer<uint>: their voxels
#define HAIR_VOLUME_COUNT UintRootConstant15     // rows of the volume buffer in use; 0: no volumes this frame

// The density view looks through at most this many volumes, and this many voxels of each.
#define HAIR_DENSITY_VIEW_VOLUMES 128u
#define HAIR_DENSITY_VIEW_STEPS 192u

// The density volumes along a pixel's ray, as a maximum projection: the densest voxel of each volume the ray crosses,
// as the fibres a ray crosses in one voxel's length there (white at 2), over a dim grey that shows the volume's box.
// Blue where the ray meets the scalp shell.
float4 HairDensityView(float2 uv)
{
    const Camera camera = HairLoadMainCamera();
    float4 viewPoint = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.5f, 1.0f), camera.projectionInverse);
    viewPoint.xyz /= viewPoint.w;
    const float3 direction = normalize(mul(float4(viewPoint.xyz, 0.0f), camera.viewInverse).xyz);
    const float3 origin = camera.positionWorldSpace.xyz;
    StructuredBuffer<HairVolumeGPU> volumes = ResourceDescriptorHeap[HAIR_VOLUMES];
    StructuredBuffer<uint> voxels = ResourceDescriptorHeap[HAIR_VOXELS];
    float densest = 0.0f;
    bool inBox = false;
    bool scalp = false;
    for (uint index = 0u; index < min(HAIR_VOLUME_COUNT, HAIR_DENSITY_VIEW_VOLUMES); ++index)
    {
        const HairVolumeGPU volume = volumes[index];
        if (!HairVolumeExists(volume))
        {
            continue;
        }
        const float3 gridOrigin = HairVolumeGridPosition(volume, origin);
        const float3 gridDirection = HairVolumeGridDirection(volume, direction);
        float enter;
        float exit;
        HairVolumeRayRange(volume, gridOrigin, gridDirection, enter, exit);
        enter = max(enter, 0.0f);
        if (!(exit > enter))
        {
            continue;
        }
        inBox = true;
        const uint steps = min((uint)ceil((exit - enter) / (0.5f * volume.voxelSize)), HAIR_DENSITY_VIEW_STEPS);
        const int3 last = (int3)volume.resolution - 1;
        for (uint step = 0u; step < steps; ++step)
        {
            const float distance = enter + (exit - enter) * ((float)step + 0.5f) / (float)steps;
            const int3 voxel = clamp((int3)floor(gridOrigin + gridDirection * distance), 0, last);
            const uint value = voxels[HairVolumeVoxelIndex(volume, (uint3)voxel)];
            scalp = scalp || (value & HAIR_VOLUME_SCALP_BIT) != 0u;
            densest = max(densest, HairVolumeFibres(value, volume.voxelSize));
        }
    }
    if (!inBox)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float level = 0.08f + 0.92f * saturate(0.5f * densest);
    return float4(scalp ? float3(0.3f * level, 0.3f * level, max(level, 0.5f)) : level.xxx, 1.0f);
}

float4 HairDebugOverlayPS(FULLSCREEN_VS_OUTPUT input) : SV_Target
{
    // The target is at render resolution and the presentation colour at output resolution: nearest pixel.
    const float2 uv = input.position.xy / float2((float)HAIR_OUTPUT_WIDTH, (float)HAIR_OUTPUT_HEIGHT);
    const uint2 size = uint2(HAIR_VISIBILITY_WIDTH, HAIR_VISIBILITY_HEIGHT);
    const uint2 pixel = min((uint2)(uv * float2(size)), size - 1u);
    if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_DENSITY)
    {
        return HAIR_VOLUME_COUNT != 0u ? HairDensityView(uv) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_COVERAGE)
    {
        // The whole frame, so that a capture measures it: black where no hair is, grey up to one pixel's worth of
        // coverage (the grey level is the coverage), then from yellow to red by the doublings above that.
        Texture2D<uint> hairCoverage = ResourceDescriptorHeap[HAIR_COVERAGE];
        float coverage = (float)hairCoverage[pixel] * (1.0f / HAIR_COVERAGE_UNIT);
        if (HAIR_COVERAGE_FAR != HAIR_INVALID_INDEX)
        {
            Texture2D<uint> hairCoverageFar = ResourceDescriptorHeap[HAIR_COVERAGE_FAR];
            coverage += (float)hairCoverageFar[pixel] * (1.0f / HAIR_COVERAGE_UNIT);
        }
        const float3 over = lerp(float3(1.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), saturate(log2(max(coverage, 1.0f)) / 6.0f));
        return float4(coverage <= 1.0f ? coverage.xxx : over, 1.0f);
    }
    if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_LAYERS || HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_ALPHA || HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_REACTIVE)
    {
        // The layered path's, over the whole frame: what the composite wrote. Black on the surface path.
        if (HAIR_COMPOSITE_INFO == HAIR_INVALID_INDEX)
        {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        Texture2D<float2> compositeInfo = ResourceDescriptorHeap[HAIR_COMPOSITE_INFO];
        const float2 info = compositeInfo[pixel];
        if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_ALPHA)
        {
            return float4(info.xxx, 1.0f);
        }
        if (HAIR_DEBUG_VIEW == HAIR_DEBUG_VIEW_REACTIVE)
        {
            Texture2D<float> reactiveMask = ResourceDescriptorHeap[HAIR_REACTIVE_MASK];
            return float4(reactiveMask[pixel].xxx, 1.0f);
        }
        // One layer blue, two green, three yellow, three with coverage beyond them red.
        const uint bits = (uint)(info.y * 255.0f + 0.5f);
        const uint layers = bits & HAIR_COMPOSITE_INFO_LAYER_MASK;
        const float3 layerColors[4] = { float3(0.0f, 0.0f, 0.0f), float3(0.1f, 0.3f, 1.0f), float3(0.1f, 0.9f, 0.2f), float3(1.0f, 0.9f, 0.1f) };
        return float4((bits & HAIR_COMPOSITE_INFO_REMAINDER) != 0u ? float3(1.0f, 0.1f, 0.1f) : layerColors[layers], 1.0f);
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
