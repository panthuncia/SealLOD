// Hair in the sun's virtual shadow map: the passes HairExtension gives the shadow map as a caster provider
// (Extensions/VirtualShadowCasterProvider.h). They run after the shadow map has decided which pages take depth this
// frame, and before anything looks a shadow up.
//
// HairShadowSelectCS: one dispatch per groom instance, one thread per cluster, like the main view's select pass. A
// cluster is kept when its bounds, seen from the sun, lie over a page some clipmap draws this frame: that is what "its
// shadow can reach the view" means, since the shadow map only has pages that receivers in the main view asked for. A
// kept cluster takes a detail level from the size of a shadow texel there, a row of the shadow cluster list and a range
// of the shadow pool. Whether the main view shows the cluster plays no part.
//
// The shadow pool is then filled by the evaluate pass's own shader (HairEvaluateCS, hairSelectEvaluate.hlsl) run over
// the shadow list: the strands are where the main view's are, simulated when their instance is.
//
// HairShadowRasterCS: one thread per row of the shadow pool. A row that starts a segment draws it into every clipmap its
// cluster was kept for, as a strip at least one texel wide, with the depth encoding and the atomic write of
// virtualShadowCaster.hlsli. A page holds one depth a texel and hair is not opaque to light: a texel of the strip is
// written with the probability of the strand's coverage of it (its width over the strip's), so that a layer of hair
// leaves the share of texels open that it lets light through, and the shadow lookup's filter and the upscaler average
// that into a partial shadow. Widths are the pool's, which carry the level of detail's width scale: a cluster drawn
// with fewer strands covers the same share of texels.
//
// Hair is a deformable caster: in the clipmaps the shadow map redraws every frame for such casters it writes the
// dynamic layer, which starts each frame from the cached depth of everything else; in the others it writes the cached
// pages themselves, when they are drawn, and HairRuntime invalidates the pages under a groom that moved.

#include "HairShaders/hairCommon.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/clodVirtualShadowDepth.hlsli"
#include "include/virtualShadowCaster.hlsli"
#include "HairShaders/hairRootFrame.hlsli"

// HairShadowSelectPass::Record and HairShadowRasterPass::Record. The entities' rows are bound by name.
#define HAIR_FRAME_STATE UintRootConstant0       // StructuredBuffer<HairFrameGPU>
#define HAIR_COUNTERS UintRootConstant1          // RWStructuredBuffer<uint>
#define HAIR_SHADOW_CLUSTERS UintRootConstant2   // (RW)StructuredBuffer<HairVisibleCluster>: the shadow cluster list
#define HAIR_SHADOW_MASKS UintRootConstant3      // (RW)StructuredBuffer<uint>, parallel to it: the clipmaps a cluster is drawn into
#define HAIR_SHADOW_CLIPMAPS UintRootConstant4   // StructuredBuffer<CLodVirtualShadowClipmapInfo>
#define HAIR_SHADOW_CAMERAS UintRootConstant5    // StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo>
// Select.
#define HAIR_INSTANCES UintRootConstant6         // StructuredBuffer<HairInstanceGPU>
#define HAIR_CLUSTERS UintRootConstant7          // StructuredBuffer<HairClusterHeader>
#define HAIR_AGGREGATES UintRootConstant8        // StructuredBuffer<HairClusterAggregate>
#define HAIR_ROOT_BINDINGS UintRootConstant9     // StructuredBuffer<HairRootBinding>
#define HAIR_INSTANCE_INDEX UintRootConstant10
#define HAIR_SHADOW_STATIC_BLOCKS UintRootConstant11  // StructuredBuffer<uint>: the blocks of pages drawn this frame, by layer
#define HAIR_SHADOW_DYNAMIC_BLOCKS UintRootConstant12
// Raster.
#define HAIR_SHADOW_POOL_VERTICES UintRootConstant6    // StructuredBuffer<HairPoolVertex>
#define HAIR_SHADOW_POOL_SEGMENT_IDS UintRootConstant7 // StructuredBuffer<uint>
#define HAIR_SHADOW_PAGE_TABLE UintRootConstant8       // RWTexture2DArray<uint>
#define HAIR_SHADOW_STATIC_PAGES UintRootConstant9     // RWTexture2D<uint>
#define HAIR_SHADOW_DYNAMIC_PAGES UintRootConstant10   // RWTexture2D<uint>
#define HAIR_GROUPS_PER_ROW UintRootConstant11         // the dispatch is this many groups wide
#define HAIR_SHADOW_STATISTICS UintRootConstant12      // StructuredBuffer<CLodVirtualShadowStats>: the shadow map's own counts

// PCG's output permutation (Jarzynski and Olano 2020).
uint HairShadowHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The pages a sphere covers in a clipmap. A clipmap is an orthographic view, so the sphere's extent there is its radius
// times the lengths of the view-projection's x and y columns.
bool HairShadowSpherePages(float3 center, float radius, CLodVirtualShadowClipmapInfo clipmap,
    CLodVirtualShadowCompactShadowCameraInfo camera, out uint2 minPage, out uint2 maxPage)
{
    minPage = 0u.xx;
    maxPage = 0u.xx;
    if (clipmap.pageTableResolution == 0u || clipmap.pageTableResolution > kCLodVirtualShadowMaxPageTableResolution)
    {
        return false;
    }
    const float4 clipCenter = mul(float4(center, 1.0f), camera.viewProjection);
    if (!(abs(clipCenter.w) > 1.0e-6f))
    {
        return false;
    }
    const float2 uvCenter = clipCenter.xy / clipCenter.w * float2(0.5f, -0.5f) + 0.5f;
    const float2 uvRadius = 0.5f * radius / abs(clipCenter.w) * float2(
        length(float3(camera.viewProjection[0][0], camera.viewProjection[1][0], camera.viewProjection[2][0])),
        length(float3(camera.viewProjection[0][1], camera.viewProjection[1][1], camera.viewProjection[2][1])));
    const float2 uvMin = uvCenter - uvRadius;
    const float2 uvMax = uvCenter + uvRadius;
    // Written so a position that is not a number is rejected too.
    if (!(uvMax.x >= 0.0f && uvMax.y >= 0.0f && uvMin.x <= 1.0f && uvMin.y <= 1.0f))
    {
        return false;
    }
    minPage = CLodVirtualShadowVirtualPageCoordsFromUv(saturate(uvMin), clipmap);
    maxPage = CLodVirtualShadowVirtualPageCoordsFromUv(saturate(uvMax), clipmap);
    return true;
}

// Whether any of those pages is one the clipmap draws this frame, in the layer hair writes there.
bool HairShadowAnyActivePage(uint2 minPage, uint2 maxPage, uint clipmapIndex, StructuredBuffer<uint> activeBlocks)
{
    const uint2 minBlock = CLodVirtualShadowBlockCoordFromPageCoord(minPage);
    if (any(minBlock >= kCLodVirtualShadowMaxBlocksPerAxis.xx))
    {
        return false;
    }
    const uint2 maxBlock = min(CLodVirtualShadowBlockCoordFromPageCoord(maxPage), (kCLodVirtualShadowMaxBlocksPerAxis - 1u).xx);
    for (uint blockY = minBlock.y; blockY <= maxBlock.y; ++blockY)
    {
        for (uint blockX = minBlock.x; blockX <= maxBlock.x; ++blockX)
        {
            const uint2 block = uint2(blockX, blockY);
            const uint activeRect = activeBlocks[CLodVirtualShadowBlockLinearIndex(block, clipmapIndex)];
            if (activeRect == 0xFFFFFFFFu)
            {
                continue;
            }
            const uint2 origin = CLodVirtualShadowBlockOriginFromBlockCoord(block);
            const uint2 localMin = max(minPage, origin) - origin;
            const uint2 localMax = min(maxPage, origin + kCLodVirtualShadowBlockPagesPerAxis - 1u) - origin;
            if (!any(max(localMin, CLodVirtualShadowUnpackBlockActiveRectMin(activeRect)) >
                min(localMax, CLodVirtualShadowUnpackBlockActiveRectMax(activeRect))))
            {
                return true;
            }
        }
    }
    return false;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairShadowSelectCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
    const HairInstanceGPU instance = instances[HAIR_INSTANCE_INDEX];
    if (dispatchThreadId.x >= instance.clusterCount)
    {
        return;
    }
    StructuredBuffer<HairClusterHeader> clusters = ResourceDescriptorHeap[HAIR_CLUSTERS];
    StructuredBuffer<HairClusterAggregate> aggregates = ResourceDescriptorHeap[HAIR_AGGREGATES];
    StructuredBuffer<HairRootBinding> rootBindings = ResourceDescriptorHeap[HAIR_ROOT_BINDINGS];
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    StructuredBuffer<PerObjectBuffer> perObject = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmaps = ResourceDescriptorHeap[HAIR_SHADOW_CLIPMAPS];
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> shadowCameras = ResourceDescriptorHeap[HAIR_SHADOW_CAMERAS];

    const uint clusterIndex = dispatchThreadId.x;
    const HairClusterHeader cluster = clusters[instance.clusterBase + clusterIndex];
    const HairFrameGPU frame = frameState[0];
    InterlockedAdd(counters[HAIR_COUNTER_SHADOW_CLUSTERS_TESTED], 1u);
    if (cluster.strandCount == 0u)
    {
        return;
    }

    // The cluster's bounds, as the main view's select pass places them.
    const PerObjectBuffer object = perObject[instance.perObjectIndex];
    const float scale = HairModelScale(object.model);
    const HairRootBinding firstRoot = rootBindings[instance.strandBase + cluster.strandOffset];
    float4x4 skin;
    float4x4 previousSkin;
    HairLoadRootSkin(instance, firstRoot, skin, previousSkin);
    const HairRootFollow follow = HairMakeRootFollow(firstRoot, HairMakeRootFrame(firstRoot, skin, object.model, 0u), scale);
    const float3 center = HairFollowRoot(follow, cluster.boundsCenter);
    const float radius = cluster.boundsRadius * scale;
    const float cullRadius = radius * (1.0f + HAIR_CULL_RADIUS_MARGIN) + HAIR_CULL_WIDTH_MARGIN * scale;

    // The clipmaps that draw a page under it this frame, and the finest texel among them.
    uint clipmapMask = 0u;
    float texel = 3.0e38f;
    for (uint clipmapIndex = 0u; clipmapIndex < kCLodVirtualShadowClipmapCount; ++clipmapIndex)
    {
        const CLodVirtualShadowClipmapInfo clipmap = clipmaps[clipmapIndex];
        if (!CLodVirtualShadowClipmapIsValid(clipmap) || CLodVirtualShadowClipmapIsLocal(clipmap))
        {
            continue;
        }
        const bool dynamicLayer = VirtualShadowCasterUsesDynamicLayer(kVirtualShadowCasterMobilitySkinnedOrDeformable, clipmap);
        StructuredBuffer<uint> activeBlocks =
            ResourceDescriptorHeap[dynamicLayer ? HAIR_SHADOW_DYNAMIC_BLOCKS : HAIR_SHADOW_STATIC_BLOCKS];
        uint2 minPage;
        uint2 maxPage;
        if (HairShadowSpherePages(center, cullRadius, clipmap, shadowCameras[clipmapIndex], minPage, maxPage) &&
            HairShadowAnyActivePage(minPage, maxPage, clipmapIndex, activeBlocks))
        {
            clipmapMask |= 1u << clipmapIndex;
            texel = min(texel, clipmap.texelWorldSize);
        }
    }
    if (clipmapMask == 0u)
    {
        return;
    }

    // The main view's rule with the shadow texel in the pixel's place: every strand is drawn while the strands are at
    // least lodErrorPixels shadow texels apart, a texel counted as shadowDetailScale of them.
    const float texelsPerMetre = 1.0f / (max(texel, 1.0e-6f) * frame.shadowDetailScale);
    const HairClusterAggregate aggregate = aggregates[instance.clusterBase + clusterIndex];
    const float spacing = (aggregate.density > 0.0f
        ? rsqrt(aggregate.density)
        : 2.0f * cluster.boundsRadius * rsqrt((float)cluster.strandCount)) * scale;
    float detailLevel = HairLodDetailLevelPower(spacing * texelsPerMetre, frame.lodErrorPixels, frame.minDetail, frame.detailExponent);
    if (frame.shadowForceDetail > 0.0f)
    {
        detailLevel = min(frame.shadowForceDetail, 1.0f);
    }
    const uint strandCount = HairLodStrandCountShared(detailLevel, cluster.strandCount, cluster.randomOffset);
    float strandLength = 0.0f;
    for (uint controlPoint = 1u; controlPoint < HAIR_AGGREGATE_CONTROL_POINT_COUNT; ++controlPoint)
    {
        strandLength += distance(aggregate.controlPoints[controlPoint].position, aggregate.controlPoints[controlPoint - 1u].position);
    }
    // A drawn segment aims for segmentPixels shadow texels, the texel at its own size: with fewer vertices a strand that
    // bends is drawn along its chords, inside the shape it has, and its shadow is smaller (measured: a fifth less shadow
    // on the floor under a drooping groom at half the vertices). Fewer strands cost nothing of the kind.
    const float fractionalLevel =
        HairLodVertexLevel(strandLength * scale / max(texel, 1.0e-6f), frame.segmentPixels, cluster.vertexLevel);
    const uint vertexLevel = HairLodDrawnVertexLevel(fractionalLevel);
    const uint segmentsPerStrand = 1u << vertexLevel;

    const uint poolVertexCount = strandCount * (segmentsPerStrand + 1u);
    uint poolVertexBase;
    InterlockedAdd(counters[HAIR_COUNTER_SHADOW_POOL_VERTICES], poolVertexCount, poolVertexBase);
    if (poolVertexBase + poolVertexCount > frame.shadowPoolCapacity)
    {
        InterlockedAdd(counters[HAIR_COUNTER_SHADOW_OVERFLOW], 1u);
        return;
    }
    uint listIndex;
    InterlockedAdd(counters[HAIR_COUNTER_SHADOW_CLUSTERS], 1u, listIndex);
    if (listIndex >= frame.shadowClusterCapacity)
    {
        InterlockedAdd(counters[HAIR_COUNTER_SHADOW_OVERFLOW], 1u);
        return;
    }

    RWStructuredBuffer<HairVisibleCluster> shadowClusters = ResourceDescriptorHeap[HAIR_SHADOW_CLUSTERS];
    RWStructuredBuffer<uint> shadowMasks = ResourceDescriptorHeap[HAIR_SHADOW_MASKS];
    HairVisibleCluster kept;
    kept.instanceIndex = HAIR_INSTANCE_INDEX;
    kept.clusterIndex = clusterIndex;
    kept.strandCount = strandCount;
    kept.poolVertexBase = poolVertexBase;
    kept.detailLevel = detailLevel;
    kept.boundaryWeight = HairLodBoundaryWeight(detailLevel, cluster.strandCount, cluster.randomOffset);
    kept.vertexLevel = vertexLevel;
    kept.geomorph = HairLodGeomorph(fractionalLevel);
    kept.seenSlot = HAIR_INVALID_INDEX; // the stamps are the main view's
    kept.flags = HAIR_VISIBLE_FLAG_SEEN;
    kept.widthDetail = kept.detailLevel;
    kept.densityScale = 1.0f;
    shadowClusters[listIndex] = kept;
    shadowMasks[listIndex] = clipmapMask;
    InterlockedAdd(counters[HAIR_COUNTER_SHADOW_STRANDS], strandCount);
    InterlockedAdd(counters[HAIR_COUNTER_SHADOW_SEGMENTS], strandCount * segmentsPerStrand);
    InterlockedOr(counters[HAIR_COUNTER_SHADOW_CLIPMAP_MASK], clipmapMask);
}

// A number in [0, 1) for a shadow texel and a strand, and for the frame when the dither is temporal.
float HairShadowCoverageRandom(uint2 texel, uint strandKey, uint frameSeed)
{
    const uint hashed = HairShadowHash(texel.x + HairShadowHash(texel.y + HairShadowHash(strandKey + HairShadowHash(frameSeed))));
    return (float)(hashed >> 8u) * (1.0f / 16777216.0f);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairShadowRasterCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint row = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    if (row == 0u)
    {
        // What the shadow map itself does this frame, for hair.stats: the pages it draws, and the page table entries
        // bounds invalidated (hair's and any other provider's).
        StructuredBuffer<CLodVirtualShadowStats> statistics = ResourceDescriptorHeap[HAIR_SHADOW_STATISTICS];
        const CLodVirtualShadowStats shadowStats = statistics[0];
        uint invalidated = 0u;
        for (uint clipmapIndex = 0u; clipmapIndex < kCLodVirtualShadowClipmapCount; ++clipmapIndex)
        {
            invalidated += shadowStats.invalidatedCurrentBoundsPageTableEntries[clipmapIndex];
        }
        InterlockedMax(counters[HAIR_COUNTER_SHADOW_PAGES_ADMITTED], shadowStats.admittedPageCount);
        InterlockedMax(counters[HAIR_COUNTER_SHADOW_PAGES_INVALIDATED], invalidated);
    }
    const uint poolVertexCount = min(counters[HAIR_COUNTER_SHADOW_POOL_VERTICES], frame.shadowPoolCapacity);
    if (row + 1u >= poolVertexCount)
    {
        return;
    }
    StructuredBuffer<uint> poolSegmentIds = ResourceDescriptorHeap[HAIR_SHADOW_POOL_SEGMENT_IDS];
    const uint segmentId = poolSegmentIds[row];
    if (segmentId == HAIR_INVALID_INDEX)
    {
        return;
    }
    // As in the main rasterizer: a row is this frame's when the cluster its id names owns it.
    StructuredBuffer<HairVisibleCluster> shadowClusters = ResourceDescriptorHeap[HAIR_SHADOW_CLUSTERS];
    const uint listIndex = segmentId >> HAIR_VIS_CLUSTER_SHIFT;
    if (listIndex >= min(counters[HAIR_COUNTER_SHADOW_CLUSTERS], frame.shadowClusterCapacity))
    {
        return;
    }
    const HairVisibleCluster kept = shadowClusters[listIndex];
    if (row < kept.poolVertexBase || row >= kept.poolVertexBase + kept.strandCount * ((1u << kept.vertexLevel) + 1u))
    {
        return;
    }
    StructuredBuffer<uint> shadowMasks = ResourceDescriptorHeap[HAIR_SHADOW_MASKS];
    uint clipmapMask = shadowMasks[listIndex];

    StructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_SHADOW_POOL_VERTICES];
    const HairPoolVertex start = poolVertices[row];
    const HairPoolVertex end = poolVertices[row + 1u];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmaps = ResourceDescriptorHeap[HAIR_SHADOW_CLIPMAPS];
    StructuredBuffer<CLodVirtualShadowCompactShadowCameraInfo> shadowCameras = ResourceDescriptorHeap[HAIR_SHADOW_CAMERAS];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[HAIR_SHADOW_PAGE_TABLE];

    // A strand's segments meet end to end, as in the main view with coverage: only its own two ends are round.
    const bool roundStart = (segmentId & ((1u << HAIR_VIS_SEGMENT_BITS) - 1u)) == 0u;
    const bool roundEnd = poolSegmentIds[row + 1u] == HAIR_INVALID_INDEX;
    const uint strandKey = HairShadowHash(kept.instanceIndex * 0x9E3779B9u + kept.clusterIndex * HAIR_MAX_STRANDS_PER_CLUSTER +
        ((segmentId >> HAIR_VIS_SEGMENT_BITS) & ((1u << HAIR_VIS_STRAND_BITS) - 1u)));
    const bool opaque = (frame.shadowFlags & HAIR_SHADOW_FLAG_OPAQUE) != 0u;
    const uint frameSeed = (frame.shadowFlags & HAIR_SHADOW_FLAG_TEMPORAL) != 0u ? frame.frameNumber : 0u;

    uint drawn = 0u;
    uint covered = 0u;
    uint written = 0u;
    bool capped = false;
    while (clipmapMask != 0u)
    {
        const uint clipmapIndex = firstbitlow(clipmapMask);
        clipmapMask &= clipmapMask - 1u;
        const CLodVirtualShadowClipmapInfo clipmap = clipmaps[clipmapIndex];
        if (!CLodVirtualShadowClipmapIsValid(clipmap))
        {
            continue;
        }
        const CLodVirtualShadowCompactShadowCameraInfo camera = shadowCameras[clipmapIndex];
        const bool dynamicLayer = VirtualShadowCasterUsesDynamicLayer(kVirtualShadowCasterMobilitySkinnedOrDeformable, clipmap);

        // Both ends in the clipmap's texels, with their depth along the light.
        const float resolution = (float)clipmap.virtualResolution;
        const float4 clip0 = mul(float4(start.position, 1.0f), camera.viewProjection);
        const float4 clip1 = mul(float4(end.position, 1.0f), camera.viewProjection);
        const float2 texel0 = float2(clip0.x / clip0.w * 0.5f + 0.5f, 0.5f - clip0.y / clip0.w * 0.5f) * resolution;
        const float2 texel1 = float2(clip1.x / clip1.w * 0.5f + 0.5f, 0.5f - clip1.y / clip1.w * 0.5f) * resolution;
        const float depth0 = -mul(float4(start.position, 1.0f), camera.view).z;
        const float depth1 = -mul(float4(end.position, 1.0f), camera.view).z;
        const float halfTexelsPerMetre = 0.5f / max(clipmap.texelWorldSize, 1.0e-6f);
        const float trueHalfWidth0 = start.width * halfTexelsPerMetre;
        const float trueHalfWidth1 = end.width * halfTexelsPerMetre;
        const float halfWidth0 = max(trueHalfWidth0, 0.5f);
        const float halfWidth1 = max(trueHalfWidth1, 0.5f);
        const float maxHalfWidth = max(halfWidth0, halfWidth1);
        const float2 boundsMin = min(texel0, texel1) - maxHalfWidth;
        const float2 boundsMax = max(texel0, texel1) + maxHalfWidth;
        if (!(boundsMax.x >= 0.0f && boundsMax.y >= 0.0f && boundsMin.x < resolution && boundsMin.y < resolution))
        {
            continue;
        }
        // Nothing to do unless a page under the segment takes depth this frame.
        const float last = resolution - 1.0f;
        if (!CLodVirtualShadowAnyRenderablePageInPixelRect((uint2)clamp(floor(boundsMin), 0.0f, last),
            (uint2)clamp(floor(boundsMax), 0.0f, last), clipmap, dynamicLayer, pageTable))
        {
            continue;
        }

        // The main rasterizer's walk, in shadow texels.
        const float2 delta = texel1 - texel0;
        const bool steep = abs(delta.y) > abs(delta.x);
        const float major0 = steep ? texel0.y : texel0.x;
        const float major1 = steep ? texel1.y : texel1.x;
        const float minor0 = steep ? texel0.x : texel0.y;
        const float minor1 = steep ? texel1.x : texel1.y;
        const float majorDelta = major1 - major0;
        const float inverseMajorDelta = abs(majorDelta) > 1.0e-6f ? 1.0f / majorDelta : 0.0f;
        const float lengthSquared = dot(delta, delta);
        const float inverseLengthSquared = lengthSquared > 1.0e-12f ? 1.0f / lengthSquared : 0.0f;
        const float minorReach = maxHalfWidth * 1.4143f + 0.5f;
        const int majorFirst = (int)clamp(floor(min(major0, major1) - maxHalfWidth), 0.0f, last);
        const int majorEnd = (int)clamp(floor(max(major0, major1) + maxHalfWidth), 0.0f, last);
        uint tested = 0u;
        uint writtenHere = 0u;
        for (int major = majorFirst; major <= majorEnd && !capped; ++major)
        {
            const float majorCenter = (float)major + 0.5f;
            const float minorCenter = lerp(minor0, minor1, saturate((majorCenter - major0) * inverseMajorDelta));
            const int minorFirst = (int)clamp(floor(minorCenter - minorReach), 0.0f, last);
            const int minorEnd = (int)clamp(floor(minorCenter + minorReach), 0.0f, last);
            for (int minor = minorFirst; minor <= minorEnd; ++minor)
            {
                if (tested >= HAIR_SHADOW_MAX_TEXELS_PER_SEGMENT)
                {
                    capped = true;
                    break;
                }
                ++tested;
                const uint2 texel = (uint2)(steep ? int2(minor, major) : int2(major, minor));
                const float2 texelCenter = float2(texel) + 0.5f;
                const float along = dot(texelCenter - texel0, delta) * inverseLengthSquared;
                const float t = saturate(along);
                const float2 offset = texelCenter - (texel0 + delta * t);
                const float halfWidth = lerp(halfWidth0, halfWidth1, t);
                if (dot(offset, offset) > halfWidth * halfWidth || !((along >= 0.0f || roundStart) && (along < 1.0f || roundEnd)))
                {
                    continue;
                }
                const float coverage = opaque ? 1.0f : saturate(lerp(trueHalfWidth0, trueHalfWidth1, t) / halfWidth);
                const bool accepted = HairShadowCoverageRandom(texel, strandKey + clipmapIndex, frameSeed) < coverage;
                const uint2 page = CLodVirtualShadowVirtualPageCoordsFromPixel(texel, clipmap);
                if (!accepted)
                {
                    // Counted only where the page takes depth, as a written texel is.
                    if (CLodVirtualShadowPageEntryCanRasterLayer(
                        pageTable[uint3(CLodVirtualShadowWrappedPageCoords(page, clipmap), clipmap.pageTableLayer)], dynamicLayer))
                    {
                        ++covered;
                    }
                    continue;
                }
                if (VirtualShadowCasterWriteDepthAtVirtualTexel(page, texel, lerp(depth0, depth1, t), clipmap, dynamicLayer,
                    HAIR_SHADOW_PAGE_TABLE, HAIR_SHADOW_STATIC_PAGES, HAIR_SHADOW_DYNAMIC_PAGES))
                {
                    ++covered;
                    ++writtenHere;
                }
            }
        }
        written += writtenHere;
        drawn += writtenHere != 0u ? 1u : 0u;
    }

    if (drawn != 0u)
    {
        InterlockedAdd(counters[HAIR_COUNTER_SHADOW_SEGMENTS_DRAWN], drawn);
        InterlockedAdd(counters[HAIR_COUNTER_SHADOW_TEXELS_WRITTEN], written);
    }
    if (covered != 0u)
    {
        InterlockedAdd(counters[HAIR_COUNTER_SHADOW_TEXELS_COVERED], covered);
    }
    if (capped)
    {
        InterlockedAdd(counters[HAIR_COUNTER_SHADOW_SEGMENTS_CAPPED], 1u);
    }
}
