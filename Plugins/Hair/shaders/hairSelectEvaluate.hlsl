// Strand LOD selection and evaluation for the main view.
//
// HairSelectCS: one dispatch per groom instance, one thread per cluster. It frustum-tests the cluster, picks its detail
// level l, its drawn strand count and its drawn vertex level, appends it to the visible-cluster list and takes its range
// of the strand pool.
//
// HairEvaluateCS: one thread group per visible cluster, one thread per drawn strand. It writes the strand's drawn
// vertices into the pool in world space, for this frame and the previous one. Without simulation a strand follows its
// root rigidly. With it (HAIR_SIM_FLAG_ENABLED) a guide's vertices are its guide state, simulated or evaluated from its
// parents (hairGuideLevels.hlsl), and every other strand is skinned to its three guides.

#include "HairShaders/hairCommon.hlsli"

// HairSelectPass::Record and HairEvaluatePass::Record. The camera and the entities' rows are bound by name.
#define HAIR_INSTANCES UintRootConstant0        // StructuredBuffer<HairInstanceGPU>
#define HAIR_CLUSTERS UintRootConstant1         // StructuredBuffer<HairClusterHeader>
#define HAIR_AGGREGATES UintRootConstant2       // StructuredBuffer<HairClusterAggregate>, select only
#define HAIR_ROOT_BINDINGS UintRootConstant3    // StructuredBuffer<HairRootBinding>
#define HAIR_ROOT_FRAMES UintRootConstant4      // StructuredBuffer<HairRootFramePair>
#define HAIR_FRAME_STATE UintRootConstant5      // StructuredBuffer<HairFrameGPU>
#define HAIR_COUNTERS UintRootConstant6         // RWStructuredBuffer<uint> in select, StructuredBuffer<uint> in evaluate
#define HAIR_VISIBLE_CLUSTERS UintRootConstant7 // RWStructuredBuffer<HairVisibleCluster> in select, read in evaluate
#define HAIR_INSTANCE_INDEX UintRootConstant8   // select only
#define HAIR_STRANDS UintRootConstant9          // StructuredBuffer<HairStrandRecord>, evaluate only
#define HAIR_POINTS UintRootConstant10          // StructuredBuffer<HairPackedPoint>, evaluate only
#define HAIR_POOL_VERTICES UintRootConstant11   // RWStructuredBuffer<HairPoolVertex>, evaluate only
#define HAIR_POOL_SEGMENT_IDS UintRootConstant12 // RWStructuredBuffer<uint>, evaluate only
#define HAIR_GUIDE_POINTS UintRootConstant13       // StructuredBuffer<HairGuidePoint>, evaluate only
#define HAIR_GUIDE_POINT_RANGES UintRootConstant14 // StructuredBuffer<HairGuidePointRange>, evaluate only
#define HAIR_SIM_METRICS UintRootConstant15        // RWStructuredBuffer<uint>, evaluate only

#include "HairShaders/hairGuideSkin.hlsli"

[shader("compute")]
[numthreads(64, 1, 1)]
void HairSelectCS(uint3 dispatchThreadId : SV_DispatchThreadID)
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
    StructuredBuffer<HairRootFramePair> rootFrames = ResourceDescriptorHeap[HAIR_ROOT_FRAMES];
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    StructuredBuffer<PerObjectBuffer> perObject = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    RWStructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];

    const uint clusterIndex = dispatchThreadId.x;
    const HairClusterHeader cluster = clusters[instance.clusterBase + clusterIndex];
    const HairFrameGPU frame = frameState[0];
    const Camera camera = HairLoadMainCamera();
    InterlockedAdd(counters[HAIR_COUNTER_CLUSTERS_TESTED], 1u);
    if (cluster.strandCount == 0u)
    {
        return;
    }

    // The cluster's bounds follow its first strand's root, as its strands follow theirs. That is exact while every root
    // of the instance moves with one matrix; with a skin binding per root it is what the margin below allows for.
    const float scale = HairModelScale(perObject[instance.perObjectIndex].model);
    const HairRootFollow follow = HairMakeRootFollow(rootBindings[instance.strandBase + cluster.strandOffset],
        rootFrames[instance.rootFrameBase + cluster.strandOffset].current, scale);
    const float3 center = HairFollowRoot(follow, cluster.boundsCenter);
    const float radius = cluster.boundsRadius * scale;

    // The frustum planes are in view space, as CLod's culling tests them.
    const float3 viewCenter = mul(float4(center, 1.0f), camera.view).xyz;
    const float cullRadius = radius * (1.0f + HAIR_CULL_RADIUS_MARGIN) + HAIR_CULL_WIDTH_MARGIN * scale;
    for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        const float4 plane = camera.clippingPlanes[planeIndex].plane;
        if (dot(plane.xyz, viewCenter) + plane.w < -cullRadius)
        {
            return;
        }
    }

    if (HAIR_INSTANCE_INDEX < HAIR_COUNTER_INSTANCE_MASK_WORDS * 32u)
    {
        InterlockedOr(counters[HAIR_COUNTER_INSTANCE_MASK_FIRST + (HAIR_INSTANCE_INDEX >> 5u)], 1u << (HAIR_INSTANCE_INDEX & 31u));
    }

    // Projection as CLod's ProjectedGeometricError: by view depth, not distance, at the near side of the bounds.
    const float depth = max(-viewCenter.z - radius, camera.zNear);
    const float pixelsPerMetre = HairPixelsPerMetreAtUnitDepth(camera, (float)perFrame.screenResY) / depth;

    // Strand spacing from the aggregate's density (strands per cross-section area); from the bounds when it has none.
    const HairClusterAggregate aggregate = aggregates[instance.clusterBase + clusterIndex];
    const float spacing = (aggregate.density > 0.0f
        ? rsqrt(aggregate.density)
        : 2.0f * cluster.boundsRadius * rsqrt((float)cluster.strandCount)) * scale;
    float detailLevel = HairLodDetailLevel(spacing * pixelsPerMetre, frame.lodErrorPixels, frame.minDetail);
    if (frame.forceDetail > 0.0f)
    {
        detailLevel = min(frame.forceDetail, 1.0f);
    }
    const uint strandCount = HairLodStrandCountShared(detailLevel, cluster.strandCount, cluster.randomOffset);

    // Vertex level from the length of the cluster's centreline on screen.
    float strandLength = 0.0f;
    for (uint controlPoint = 1u; controlPoint < HAIR_AGGREGATE_CONTROL_POINT_COUNT; ++controlPoint)
    {
        strandLength += distance(aggregate.controlPoints[controlPoint].position,
            aggregate.controlPoints[controlPoint - 1u].position);
    }
    const float fractionalLevel =
        HairLodVertexLevel(strandLength * scale * pixelsPerMetre, frame.segmentPixels, cluster.vertexLevel);
    const uint vertexLevel = HairLodDrawnVertexLevel(fractionalLevel);
    const uint segmentsPerStrand = 1u << vertexLevel;

    // Neither allocator gives anything back: once one is past its capacity, every later cluster is dropped too.
    const uint poolVertexCount = strandCount * (segmentsPerStrand + 1u);
    uint poolVertexBase;
    InterlockedAdd(counters[HAIR_COUNTER_POOL_VERTICES], poolVertexCount, poolVertexBase);
    if (poolVertexBase + poolVertexCount > frame.poolVertexCapacity)
    {
        InterlockedAdd(counters[HAIR_COUNTER_OVERFLOW_CLUSTERS], 1u);
        return;
    }
    uint visibleIndex;
    InterlockedAdd(counters[HAIR_COUNTER_CLUSTERS_VISIBLE], 1u, visibleIndex);
    if (visibleIndex >= frame.visibleClusterCapacity)
    {
        InterlockedAdd(counters[HAIR_COUNTER_OVERFLOW_CLUSTERS], 1u);
        return;
    }

    HairVisibleCluster visible;
    visible.instanceIndex = HAIR_INSTANCE_INDEX;
    visible.clusterIndex = clusterIndex;
    visible.strandCount = strandCount;
    visible.poolVertexBase = poolVertexBase;
    visible.detailLevel = detailLevel;
    visible.boundaryWeight = HairLodBoundaryWeight(detailLevel, cluster.strandCount, cluster.randomOffset);
    visible.vertexLevel = vertexLevel;
    visible.geomorph = HairLodGeomorph(fractionalLevel);
    visibleClusters[visibleIndex] = visible;
    InterlockedAdd(counters[HAIR_COUNTER_STRANDS_SELECTED], strandCount);
    InterlockedAdd(counters[HAIR_COUNTER_SEGMENTS_SELECTED], strandCount * segmentsPerStrand);
}

// What evaluating a strand under simulation needs.
struct HairSimulatedStrand
{
    HairClusterHeader cluster;
    HairRootFollow follow;
    HairRootFollow previousFollow;
    uint pointBase;     // the strand's first HairPackedPoint
    uint segmentCount;  // stored: 2^k
    bool isGuide;       // a guide: its vertices are its own guide state
    uint guideBases[3]; // first HairGuidePoint of each guide; for a guide, slot 0 is its own
    uint guideSegmentCounts[3];
    float guideWeights[3]; // 0 for an unused slot
};

// Stored point `pointIndex` of a strand under simulation.
HairEvaluatedPoint HairEvaluateSimulatedPoint(HairSimulatedStrand strand, uint pointIndex)
{
    HairEvaluatedPoint evaluated;
    if (strand.isGuide)
    {
        StructuredBuffer<HairGuidePoint> guidePoints = ResourceDescriptorHeap[HAIR_GUIDE_POINTS];
        const HairGuidePoint state = guidePoints[strand.guideBases[0] + pointIndex];
        evaluated.position = state.position;
        evaluated.previousPosition = state.previousPosition;
        return evaluated;
    }
    StructuredBuffer<HairPackedPoint> points = ResourceDescriptorHeap[HAIR_POINTS];
    const float3 rest = HairDecodePointPosition(strand.cluster, points[strand.pointBase + pointIndex]);
    const float3 rigid = HairFollowRoot(strand.follow, rest);
    const float3 previousRigid = HairFollowRoot(strand.previousFollow, rest);
    const float parameter = (float)pointIndex / (float)strand.segmentCount;
    evaluated.position = float3(0.0f, 0.0f, 0.0f);
    evaluated.previousPosition = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;
    for (uint guide = 0u; guide < 3u; ++guide)
    {
        const float weight = strand.guideWeights[guide];
        if (weight <= 0.0f)
        {
            continue;
        }
        const HairEvaluatedPoint skinned =
            HairSkinToGuide(strand.guideBases[guide], strand.guideSegmentCounts[guide], parameter, rigid, previousRigid);
        evaluated.position += weight * skinned.position;
        evaluated.previousPosition += weight * skinned.previousPosition;
        weightSum += weight;
    }
    if (weightSum <= 0.0f)
    {
        // No guide: the strand keeps following its root.
        evaluated.position = rigid;
        evaluated.previousPosition = previousRigid;
        return evaluated;
    }
    evaluated.position /= weightSum;
    evaluated.previousPosition /= weightSum;
    return evaluated;
}

[shader("compute")]
[numthreads(64, 1, 1)] // HAIR_MAX_STRANDS_PER_CLUSTER: one thread per strand of a cluster
void HairEvaluateCS(uint3 groupId : SV_GroupID, uint strandIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint visibleIndex = groupId.x;
    if (visibleIndex >= min(counters[HAIR_COUNTER_CLUSTERS_VISIBLE], frame.visibleClusterCapacity))
    {
        return;
    }
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    const HairVisibleCluster visible = visibleClusters[visibleIndex];
    if (strandIndex >= visible.strandCount)
    {
        return;
    }

    StructuredBuffer<HairInstanceGPU> instances = ResourceDescriptorHeap[HAIR_INSTANCES];
    StructuredBuffer<HairClusterHeader> clusters = ResourceDescriptorHeap[HAIR_CLUSTERS];
    StructuredBuffer<HairStrandRecord> strands = ResourceDescriptorHeap[HAIR_STRANDS];
    StructuredBuffer<HairRootBinding> rootBindings = ResourceDescriptorHeap[HAIR_ROOT_BINDINGS];
    StructuredBuffer<HairRootFramePair> rootFrames = ResourceDescriptorHeap[HAIR_ROOT_FRAMES];
    StructuredBuffer<HairPackedPoint> points = ResourceDescriptorHeap[HAIR_POINTS];
    StructuredBuffer<PerObjectBuffer> perObject = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    RWStructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_POOL_VERTICES];
    RWStructuredBuffer<uint> poolSegmentIds = ResourceDescriptorHeap[HAIR_POOL_SEGMENT_IDS];

    const HairInstanceGPU instance = instances[visible.instanceIndex];
    const HairClusterHeader cluster = clusters[instance.clusterBase + visible.clusterIndex];
    const uint strandRow = cluster.strandOffset + strandIndex;
    const HairStrandRecord strand = strands[instance.strandBase + strandRow];
    const HairRootBinding rootBinding = rootBindings[instance.strandBase + strandRow];
    const HairRootFramePair rootFrame = rootFrames[instance.rootFrameBase + strandRow];
    const PerObjectBuffer object = perObject[instance.perObjectIndex];
    const HairRootFollow follow = HairMakeRootFollow(rootBinding, rootFrame.current, HairModelScale(object.model));
    const HairRootFollow previousFollow =
        HairMakeRootFollow(rootBinding, rootFrame.previous, HairModelScale(object.prevModel));

    // The strand is widened to make up for the dropped ones; the cluster's last drawn strand fades in by width.
    float width = strand.width * HairLodWidthScale(visible.detailLevel, frame.maxWidthScale);
    if (strandIndex + 1u == visible.strandCount)
    {
        width *= visible.boundaryWeight;
    }

    // The drawn level's vertex j is the stored point j << levelShift. Its odd vertices are the ones the level below
    // drops: they move to the midpoint of their neighbours as the geomorph goes to 1.
    const uint levelShift = cluster.vertexLevel - visible.vertexLevel;
    const uint segmentCount = 1u << visible.vertexLevel;
    const uint pointBase = instance.pointBase + strand.pointOffset;
    const uint poolBase = visible.poolVertexBase + strandIndex * (segmentCount + 1u);
    uint vertex;

    if ((instance.simFlags & HAIR_SIM_FLAG_ENABLED) != 0u)
    {
        StructuredBuffer<HairGuidePointRange> guidePointRanges = ResourceDescriptorHeap[HAIR_GUIDE_POINT_RANGES];
        RWStructuredBuffer<uint> metrics = ResourceDescriptorHeap[HAIR_SIM_METRICS];
        HairSimulatedStrand simulated;
        simulated.cluster = cluster;
        simulated.follow = follow;
        simulated.previousFollow = previousFollow;
        simulated.pointBase = pointBase;
        simulated.segmentCount = 1u << cluster.vertexLevel;
        // A guide-capable strand binds to itself. Every guide of a simulated instance has live state, whether the solver
        // or the guide-level pass wrote it.
        simulated.isGuide = strand.rank < instance.guideCount;
        for (uint guide = 0u; guide < 3u; ++guide)
        {
            const uint guideRank = strand.guideIndices[guide];
            const bool used = guideRank != HAIR_INVALID_INDEX && guideRank < instance.guideCount;
            HairGuidePointRange range;
            range.offset = 0u;
            range.count = 2u;
            if (used)
            {
                range = guidePointRanges[instance.guideBase + guideRank];
            }
            simulated.guideBases[guide] = instance.guidePointBase + range.offset;
            simulated.guideSegmentCounts[guide] = range.count - 1u;
            simulated.guideWeights[guide] = used ? strand.guideWeights[guide] : 0.0f;
        }

        for (vertex = 0u; vertex <= segmentCount; ++vertex)
        {
            const uint pointIndex = vertex << levelShift;
            HairEvaluatedPoint evaluated = HairEvaluateSimulatedPoint(simulated, pointIndex);
            float widthFraction = HairDecodePointWidthFraction(points[pointBase + pointIndex]);
            if (vertex == 0u)
            {
                // How far the evaluation left the root from its root frame; then the root goes exactly onto it. The
                // previous position stays the evaluated one: where the guides say the root was a frame ago, which is
                // not the previous root frame when the entity's frames did not follow one another (hairSimulate.hlsl).
                const uint metricBase = visible.instanceIndex * HAIR_SIM_METRIC_COUNT;
                const float rootError = distance(evaluated.position, follow.position);
                InterlockedAdd(metrics[metricBase + (simulated.isGuide ? HAIR_SIM_METRIC_GUIDE_ROOTS : HAIR_SIM_METRIC_STRAND_ROOTS)], 1u);
                InterlockedMax(metrics[metricBase + (simulated.isGuide ? HAIR_SIM_METRIC_GUIDE_ROOT_ERROR : HAIR_SIM_METRIC_STRAND_ROOT_ERROR)],
                    asuint(rootError));
                evaluated.position = follow.position;
            }
            else if ((vertex & 1u) != 0u && visible.geomorph > 0.0f)
            {
                // The geomorph, on evaluated positions: at 1 the vertex is on the segment the level below draws.
                const HairEvaluatedPoint before = HairEvaluateSimulatedPoint(simulated, (vertex - 1u) << levelShift);
                const HairEvaluatedPoint after = HairEvaluateSimulatedPoint(simulated, (vertex + 1u) << levelShift);
                evaluated.position = lerp(evaluated.position, 0.5f * (before.position + after.position), visible.geomorph);
                evaluated.previousPosition =
                    lerp(evaluated.previousPosition, 0.5f * (before.previousPosition + after.previousPosition), visible.geomorph);
                const float midpointWidth = 0.5f * (HairDecodePointWidthFraction(points[pointBase + ((vertex - 1u) << levelShift)]) +
                    HairDecodePointWidthFraction(points[pointBase + ((vertex + 1u) << levelShift)]));
                widthFraction = lerp(widthFraction, midpointWidth, visible.geomorph);
            }

            HairPoolVertex poolVertex;
            poolVertex.position = evaluated.position;
            poolVertex.width = width * widthFraction * follow.scale;
            poolVertex.previousPosition = evaluated.previousPosition;
            poolVertex.previousWidth = width * widthFraction * previousFollow.scale;
            poolVertices[poolBase + vertex] = poolVertex;
            poolSegmentIds[poolBase + vertex] =
                vertex < segmentCount ? HairPackSegmentId(visibleIndex, strandIndex, vertex) : HAIR_INVALID_INDEX;
        }
        return;
    }

    for (vertex = 0u; vertex <= segmentCount; ++vertex)
    {
        const HairPackedPoint packed = points[pointBase + (vertex << levelShift)];
        float3 restPosition = HairDecodePointPosition(cluster, packed);
        float widthFraction = HairDecodePointWidthFraction(packed);
        if ((vertex & 1u) != 0u && visible.geomorph > 0.0f)
        {
            const HairPackedPoint before = points[pointBase + ((vertex - 1u) << levelShift)];
            const HairPackedPoint after = points[pointBase + ((vertex + 1u) << levelShift)];
            const float3 midpoint =
                0.5f * (HairDecodePointPosition(cluster, before) + HairDecodePointPosition(cluster, after));
            const float midpointWidth =
                0.5f * (HairDecodePointWidthFraction(before) + HairDecodePointWidthFraction(after));
            restPosition = lerp(restPosition, midpoint, visible.geomorph);
            widthFraction = lerp(widthFraction, midpointWidth, visible.geomorph);
        }

        HairPoolVertex evaluated;
        evaluated.position = HairFollowRoot(follow, restPosition);
        evaluated.width = width * widthFraction * follow.scale;
        evaluated.previousPosition = HairFollowRoot(previousFollow, restPosition);
        evaluated.previousWidth = width * widthFraction * previousFollow.scale;
        poolVertices[poolBase + vertex] = evaluated;
        poolSegmentIds[poolBase + vertex] =
            vertex < segmentCount ? HairPackSegmentId(visibleIndex, strandIndex, vertex) : HAIR_INVALID_INDEX;
    }
}
