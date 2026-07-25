#define CLOD_COMPUTE_INCLUDE_ONLY 1
#include "ClusterLOD/workGraphCulling.hlsl"

uint PureComputeNormalizePhase2ExpansionFactor(uint value)
{
    value = min(max(value, 1u), 64u);
    uint normalized = 1u;
    [unroll]
    for (uint candidate = 2u; candidate <= 64u; candidate <<= 1u) {
        if (candidate <= value) {
            normalized = candidate;
        }
    }
    return normalized;
}

[numthreads(1, 1, 1)]
void BuildPureComputeDispatchArgsCS()
{
    StructuredBuffer<uint> counterBuffer = ResourceDescriptorHeap[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint3> dispatchArgs = ResourceDescriptorHeap[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX];
    const uint count = min(counterBuffer[0], max(1u, CLOD_PC_DISPATCH_COUNT_LIMIT));
    const uint threadsPerGroup = max(1u, CLOD_PC_DISPATCH_THREADS_PER_GROUP);
    const uint groups = (count == 0u) ? 1u : ((count + threadsPerGroup - 1u) / threadsPerGroup);
    dispatchArgs[0] = uint3(groups, 1u, 1u);
}

[numthreads(1, 1, 1)]
void BuildPureComputeDualDispatchArgsCS()
{
    StructuredBuffer<uint> firstCounterBuffer = ResourceDescriptorHeap[CLOD_PC_DISPATCH_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint3> firstDispatchArgs = ResourceDescriptorHeap[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> secondCounterBuffer = ResourceDescriptorHeap[CLOD_PC_SECOND_DISPATCH_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint3> secondDispatchArgs = ResourceDescriptorHeap[CLOD_PC_SECOND_DISPATCH_ARGS_DESCRIPTOR_INDEX];
    const uint countLimit = max(1u, CLOD_PC_DISPATCH_COUNT_LIMIT);
    const uint threadsPerGroup = max(1u, CLOD_PC_DISPATCH_THREADS_PER_GROUP);
    const uint firstCount = min(firstCounterBuffer[0], countLimit);
    const uint secondCount = min(secondCounterBuffer[0], countLimit);
    firstDispatchArgs[0] = uint3(
        firstCount == 0u ? 1u : ((firstCount + threadsPerGroup - 1u) / threadsPerGroup),
        1u,
        1u);
    secondDispatchArgs[0] = uint3(
        secondCount == 0u ? 1u : ((secondCount + threadsPerGroup - 1u) / threadsPerGroup),
        1u,
        1u);
}

[numthreads(1, 1, 1)]
void ClearPureComputeTraversalCountersCS()
{
    RWStructuredBuffer<uint> nodeCounter = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> leafCounter = ResourceDescriptorHeap[CLOD_PC_LEAF_OUTPUT_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> clusterCounter = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX];
    nodeCounter[0] = 0u;
    leafCounter[0] = 0u;
    clusterCounter[0] = 0u;
}

[numthreads(1, 1, 1)]
void BuildPureComputeReplayDispatchArgsCS()
{
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint3> dispatchArgs = ResourceDescriptorHeap[CLOD_PC_DISPATCH_ARGS_DESCRIPTOR_INDEX];
    const uint rawCount = (CLOD_PC_REPLAY_SOURCE_INDEX == 0u)
        ? replayState[0].nodeWriteCount
        : replayState[0].meshletWriteCount;
    const uint replayCapacity = (CLOD_PC_REPLAY_SOURCE_INDEX == 0u)
        ? CLOD_NODE_REPLAY_CAPACITY
        : CLOD_MESHLET_REPLAY_CAPACITY;
    const uint count = min(rawCount, min(CLOD_WG_VISIBLE_CLUSTERS_CAPACITY, replayCapacity));
    const uint threadsPerGroup = max(1u, CLOD_PC_DISPATCH_THREADS_PER_GROUP);
    const uint groups = (count == 0u) ? 1u : ((count + threadsPerGroup - 1u) / threadsPerGroup);
    dispatchArgs[0] = uint3(groups, 1u, 1u);
}

[numthreads(64, 1, 1)]
void SeedPureComputeReplayNodesCS(const uint3 dispatchThreadID : SV_DispatchThreadID)
{
    RWByteAddressBuffer replayBuffer = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<TraverseNodeRecord> outFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> outCounter = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX];

    const uint count = min(replayState[0].nodeWriteCount, min(CLOD_WG_VISIBLE_CLUSTERS_CAPACITY, CLOD_NODE_REPLAY_CAPACITY));
    const uint index = dispatchThreadID.x;
    if (index >= count) {
        return;
    }

    const uint byteOffset = index * CLOD_NODE_REPLAY_STRIDE_BYTES;
    const uint4 header = replayBuffer.Load4(byteOffset);

    TraverseNodeRecord record = (TraverseNodeRecord)0;
    record.instanceIndex = header.x;
    record.nodeIdPacked = header.y;
    record.viewId = header.z;
    record.assemblyTransformIndex = header.w;

    const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(record.instanceIndex);
    StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const MeshInstanceClodOffsets off = LoadCLodOffsetsForDrawRecord(drawRecord);
    const CLodMeshMetadata clodMeshMetadata =
        clodMeshMetadataBuffer[off.clodMeshMetadataIndex];
    if (CLodReplayRootOcclusionCandidate(record, clodMeshMetadata))
    {
        const PerMeshInstanceBuffer instanceData =
            LoadMeshTemplateForDrawRecord(drawRecord);
        const PerObjectBuffer instanceTransform =
            LoadInstanceTransformForDrawRecordWithAssemblyTransform(
                drawRecord,
                record.assemblyTransformIndex);
        StructuredBuffer<Camera> cameras =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
        const Camera cullCamera = cameras[record.viewId];
        if (CLodReplayRootOccluded(
            record,
            clodMeshMetadata,
            drawRecord,
            instanceData,
            instanceTransform,
            cullCamera))
        {
            return;
        }
    }

    uint outputIndex = 0u;
    InterlockedAdd(outCounter[0], 1u, outputIndex);
    if (outputIndex < CLOD_WG_VISIBLE_CLUSTERS_CAPACITY)
    {
        outFrontier[outputIndex] = record;
    }
}

[numthreads(32, 1, 1)]
void SeedPureComputeReplayClustersCS(const uint3 dispatchThreadID : SV_DispatchThreadID)
{
    RWByteAddressBuffer replayBuffer = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodClusterRunRecord> outFrontier = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> outCounter = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX];

    const uint count = min(replayState[0].meshletWriteCount, min(CLOD_WG_VISIBLE_CLUSTERS_CAPACITY, CLOD_MESHLET_REPLAY_CAPACITY));
    const uint index = dispatchThreadID.x;
    if (index == 0u) {
        outCounter[0] = count;
    }
    if (index >= count) {
        return;
    }

    const uint byteOffset = CLOD_REPLAY_MESHLET_REGION_OFFSET + index * CLOD_MESHLET_REPLAY_STRIDE_BYTES;
    const uint4 head = replayBuffer.Load4(byteOffset);
    const uint4 tail = replayBuffer.Load4(byteOffset + 16u);

    CLodClusterRunRecord record = (CLodClusterRunRecord)0;
    record.instanceIndex = head.x;
    record.viewId = head.y;
    record.groupIdPacked = head.z;
    record.clusterIndexAndCount = head.w;
    record.pageSlabDescriptorIndex = tail.x;
    record.pageSlabByteOffset = tail.y;
    record.assemblyTransformIndex = tail.z;
    outFrontier[index] = record;
}

[numthreads(64, 1, 1)]
void PureComputeObjectCullCS(const uint3 vDispatchThreadID : SV_DispatchThreadID)
{
    const uint drawIndex = vDispatchThreadID.x;
    const bool inRange = (drawIndex < CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_COUNT);

    WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_THREADS, 1);
    if (inRange) {
        WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_IN_RANGE_THREADS, 1);
    }

    if (!inRange) {
        return;
    }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    bool viewHasDirtyPages = true;
    if (WaveIsFirstLane())
    {
        viewHasDirtyPages =
            CLodVirtualShadowViewHasDirtyPages(CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX);
    }
    viewHasDirtyPages = WaveReadLaneFirst(viewHasDirtyPages);
    if (!viewHasDirtyPages)
    {
        return;
    }
#endif

    StructuredBuffer<uint2> activeDrawSetIndicesBuffer =
        ResourceDescriptorHeap[CLOD_PC_OBJECT_CULL_ACTIVE_DRAW_SET_SRV_INDEX];
    StructuredBuffer<uint> drawRecordVisibilityGenerations =
        ResourceDescriptorHeap[CLOD_PC_OBJECT_CULL_VISIBILITY_GENERATION_SRV_INDEX];
    const uint2 activeEntry = activeDrawSetIndicesBuffer[drawIndex];
    const uint drawRecordIndex = activeEntry.x;
    const uint activeGeneration = activeEntry.y;
    if (activeGeneration == 0u || drawRecordVisibilityGenerations[drawRecordIndex] != activeGeneration) {
        WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_STALE_GENERATION, 1);
        return;
    }
    const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(drawRecordIndex);
    const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDrawRecord(drawRecord);
    const PerObjectBuffer instanceTransform =
        LoadInstanceTransformForDrawRecordWithAssemblyTransform(drawRecord, CLOD_ASSEMBLY_TRANSFORM_SENTINEL);
    const row_major matrix objectModelMatrix = instanceTransform.model;
    StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const MeshInstanceClodOffsets off = LoadCLodOffsetsForDrawRecord(drawRecord);
    const CLodMeshMetadata clodMeshMetadata = clodMeshMetadataBuffer[off.clodMeshMetadataIndex];
    // Voxel-root classification only feeds debug telemetry. Avoid fetching the
    // first group for every object when telemetry is disabled.
    const bool voxelRootCandidate =
        CLodWorkGraphTelemetryEnabled() && CLodMeshHasVoxelRootGroup(clodMeshMetadata);
    if (voxelRootCandidate)
    {
        WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_CANDIDATES, 1);
    }

    StructuredBuffer<Camera> cameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX];

    float coarseBoundsScale = 1.0f;
    const BoundingSphere coarseBounds =
        LoadCoarseCullBoundsForDrawRecord(drawRecord, instanceData, coarseBoundsScale);
    const float3 objectSpaceCenter = coarseBounds.sphere.xyz;
    const float3 viewSpaceCenter = ToViewSpace(objectSpaceCenter, objectModelMatrix, camera.view);
    // Assembly material/subset draws share one fitted conservative sphere. This
    // is the only hierarchy-level visibility bound; final clusters use live
    // palette-deformed spheres.
    const float worldRadius = coarseBounds.sphere.w * coarseBoundsScale *
        MaxAxisScale_RowVector(objectModelMatrix);

    bool culled = false;
    if (any(isnan(viewSpaceCenter)) || any(isinf(viewSpaceCenter)) || isnan(worldRadius) || isinf(worldRadius)) {
        WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_INVALID_BOUNDS, 1);
        culled = true;
    }
    else if (CLodWorkGraphFrustumCullingEnabled()) {
        [unroll]
        for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
        {
            const float4 plane = camera.clippingPlanes[planeIndex].plane;
            const float distanceToPlane = dot(plane.xyz, viewSpaceCenter) + plane.w;
            if (distanceToPlane < -worldRadius)
            {
                WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_FRUSTUM, 1);
                if (voxelRootCandidate)
                {
                    WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_FRUSTUM_REJECTED, 1);
                }
                culled = true;
                break;
            }
        }
    }
    
    if (culled) {
        return;
    }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    // Gate traversal at object granularity.  The coarse draw bounds are
    // conservative, so a miss here guarantees that no descendant can touch
    // an admitted dirty page.
    const float3 worldCenter = mul(float4(objectSpaceCenter, 1.0f), objectModelMatrix).xyz;
    if (CLodWorkGraphShadowDirtyPageCullingEnabled() &&
        !CLodVirtualShadowBoundsTouchDirtyPages(
            worldCenter,
            worldRadius,
            CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX))
    {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES, 1u);
        return;
    }
#endif

    const uint rootNodeId = CLodResolveTraversalRootNode(clodMeshMetadata);
    bool occlusionCulled = false;
    if (CLodWorkGraphOcclusionEnabled() && !camera.isOrtho) {
        StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
            ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
        const uint depthMapDescriptorIndex =
            viewDepthSRVIndices[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX].linearDepthSRVIndex;
        if (depthMapDescriptorIndex != 0u) {
            const row_major matrix prevModelMatrix = instanceTransform.prevModel;
            const float3 prevViewSpaceCenter =
                ToViewSpace(objectSpaceCenter, prevModelMatrix, camera.prevView);
            const float prevWorldRadius = coarseBounds.sphere.w * coarseBoundsScale *
                MaxAxisScale_RowVector(prevModelMatrix);
            OcclusionCullingPerspectiveTexture2D(
                occlusionCulled,
                camera,
                prevViewSpaceCenter,
                -prevViewSpaceCenter.z,
                prevWorldRadius,
                depthMapDescriptorIndex,
                camera.prevUnjitteredProjection);
        }
    }
    if (occlusionCulled) {
        WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_REJECTED_OCCLUSION, 1u);
        ReplayTryAppendNode(
            drawRecordIndex,
            CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX,
            rootNodeId,
            CLOD_ASSEMBLY_TRANSFORM_SENTINEL);
        return;
    }

    RWStructuredBuffer<TraverseNodeRecord> outFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> outCounter = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX];

    uint outputIndex = 0u;
    InterlockedAdd(outCounter[0], 1u, outputIndex);
    if (outputIndex >= CLOD_WG_VISIBLE_CLUSTERS_CAPACITY) {
        return;
    }

    outFrontier[outputIndex].viewId = CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX;
    outFrontier[outputIndex].instanceIndex = drawRecordIndex;
    outFrontier[outputIndex].nodeIdPacked =
        PackTraverseNodeId(rootNodeId, CLOD_RECORD_SOURCE_PASS1, 1u, 0u);
    outFrontier[outputIndex].assemblyTransformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;

    WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_VISIBLE_THREADS, 1);
    WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_TRAVERSE_RECORDS, 1);
    if (voxelRootCandidate)
    {
        WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_VISIBLE, 1);
        WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_TRAVERSE_RECORDS, 1);
    }
}

#ifndef CLOD_PC_LEAF_ONLY
#define CLOD_PC_LEAF_ONLY 0
#endif

[numthreads(64, 1, 1)]
void PureComputeTraverseFrontierCS(const uint3 dispatchThreadID : SV_DispatchThreadID)
{
    StructuredBuffer<TraverseNodeRecord> inputFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> inputCountBuffer = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX];
#if !CLOD_PC_LEAF_ONLY
    RWStructuredBuffer<TraverseNodeRecord> nextFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> nextCounter = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<TraverseNodeRecord> nextLeafFrontier = ResourceDescriptorHeap[CLOD_PC_LEAF_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> nextLeafCounter = ResourceDescriptorHeap[CLOD_PC_LEAF_OUTPUT_COUNT_DESCRIPTOR_INDEX];
#endif
    RWStructuredBuffer<CLodClusterRunRecord> clusterFrontier = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> clusterCounter = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX];

    const uint inputCount = inputCountBuffer[0];
    const uint index = dispatchThreadID.x;
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_THREADS, 1);
    CLodTelemetryTraverseWaveLaunch(index < inputCount);
    if (index >= inputCount) {
        return;
    }

    const TraverseNodeRecord rec = inputFrontier[index];
    const bool parentAllowsRefine = (UnpackAllowRefine(rec.nodeIdPacked) != 0u);
    if (UnpackSourceTag(rec.nodeIdPacked) == CLOD_RECORD_SOURCE_REPLAY) {
        WGTelemetryAdd(WG_COUNTER_PHASE2_REPLAY_TRAVERSE_RECORDS_CONSUMED, 1);
    }
    const bool replaySource = (UnpackSourceTag(rec.nodeIdPacked) == CLOD_RECORD_SOURCE_REPLAY);

    StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(rec.instanceIndex);
    const MeshInstanceClodOffsets off = LoadCLodOffsetsForDrawRecord(drawRecord);
    const CLodMeshMetadata clodMeshMetadata = clodMeshMetadataBuffer[off.clodMeshMetadataIndex];
    const bool forceLodDecision = CLodForcedTraversalDepthRootEnabled(clodMeshMetadata);
    const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDrawRecord(drawRecord);
    const PerObjectBuffer instanceTransform =
        LoadInstanceTransformForDrawRecordWithAssemblyTransform(drawRecord, rec.assemblyTransformIndex);
    // Mesh upload creates one node-skinning sidecar entry per CLOD node for
    // skinned meshes and none for rigid meshes. Keep the original source for
    // telemetry so live PSO replacement retains the same resource interface.
    bool isSkinned = clodMeshMetadata.nodeSkinningInfoCount != 0u;
    if (CLodWorkGraphTelemetryEnabled())
    {
        StructuredBuffer<PerMeshBuffer> perMeshBuffer =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshBuffer perMesh = perMeshBuffer[instanceData.perMeshBufferIndex];
        isSkinned = (perMesh.vertexFlags & VERTEX_SKINNED) != 0u;
    }
    const row_major matrix objectModelMatrix = instanceTransform.model;
    StructuredBuffer<Camera> cameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const uint cullViewId = rec.viewId;
    const uint lodViewId = CLodResolveLodViewId(cullViewId);
    const Camera cullCamera = cameras[cullViewId];
    StructuredBuffer<CullingCameraInfo> cameraInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    const CullingCameraInfo lodCam = cameraInfos[lodViewId];
    const bool lodCameraIsOrtho = lodCam.isOrtho != 0u;
    StructuredBuffer<ClusterLODNode> lodNodes =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Nodes)];

    const uint nodeLocalId = UnpackNodeId(rec.nodeIdPacked);
    const ClusterLODNode node = lodNodes[clodMeshMetadata.lodNodesBase + nodeLocalId];
    CLodTelemetryTraverseWaveClassification(
        node.range.isLeaf == CLOD_NODE_INTERNAL,
        isSkinned);
#if !CLOD_PC_LEAF_ONLY
    const bool skinnedAssemblyPortal =
        node.range.isLeaf == CLOD_NODE_INSTANCE_ROOT &&
        isSkinned &&
        clodMeshMetadata.assemblyInstanceCount != 0u;
#endif
    const bool assemblyPortalTraversal = rec.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
    if (assemblyPortalTraversal)
    {
        WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_TRAVERSAL_RECORDS, 1);
    }
    // Voxel-root classification exists only for debug telemetry. Avoid the
    // group-buffer read on every traversal record in normal rendering.
    if (CLodWorkGraphTelemetryEnabled() &&
        CLodMeshHasVoxelRootGroup(clodMeshMetadata) &&
        nodeLocalId == CLodResolveTraversalRootNode(clodMeshMetadata))
    {
        WGTelemetryAdd(
            node.range.isLeaf == CLOD_NODE_INTERNAL
                ? WG_COUNTER_VOXEL_ROOT_INTERNAL_RECORDS
                : WG_COUNTER_VOXEL_ROOT_LEAF_RECORDS,
            1);
    }

#if CLOD_PC_LEAF_ONLY
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_LEAF_NODE_RECORDS, 1);
#else
    if (node.range.isLeaf == CLOD_NODE_INTERNAL) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_INTERNAL_NODE_RECORDS, 1);
    }
    else {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_LEAF_NODE_RECORDS, 1);
    }
#endif

    const float objectUniformScale = MaxAxisScale_RowVector(objectModelMatrix);
    const float cullUniformScale = objectUniformScale;
    const float lodUniformScale = objectUniformScale;
    BoundingSphere nodeCullBounds = { node.metric.cullCenterAndRadius };
    const uint nodeCullClassification = isSkinned
        ? CLodResolveAnimatedNodeCullSphere(
            nodeLocalId, node.metric.cullCenterAndRadius, clodMeshMetadata,
            rec.instanceIndex, instanceData.skinningInstanceSlot,
            rec.assemblyTransformIndex, nodeCullBounds)
        : CLOD_NODE_CULL_STATIC_BIND_POSE;
    CLodTelemetryNodeCullClassification(nodeCullClassification);
    const float3 nodeCullCenterObjectSpace = nodeCullBounds.sphere.xyz;
    const float nodeCullRadiusObjectSpace = nodeCullBounds.sphere.w;
    const float3 nodeLodCenterObjectSpace = node.metric.lodCenterAndRadius.xyz;
    const float nodeLodRadiusObjectSpace = node.metric.lodCenterAndRadius.w;
    const float3 nodeCenterViewSpace = ToViewSpace(nodeCullCenterObjectSpace, objectModelMatrix, cullCamera.view);
    const float nodeRadiusWorld = nodeCullRadiusObjectSpace * cullUniformScale;
    const bool nodeCulled =
        CLodWorkGraphFrustumCullingEnabled() &&
        !replaySource &&
        UnpackBoundsTested(rec.nodeIdPacked) == 0u &&
        CLodNodeBoundsOutsideFrustum(
            nodeCullClassification,
            node.metric.cullCenterAndRadius,
            nodeCullBounds,
            objectModelMatrix,
            cullUniformScale,
            cullCamera);

    bool dirtyPageCullingEnabled = false;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    const bool objectInvalidatedThisFrame = CLodVirtualShadowInstanceInvalidatedThisFrame(rec.instanceIndex);
    dirtyPageCullingEnabled =
        !isSkinned && CLodWorkGraphShadowDirtyPageCullingEnabled() && !objectInvalidatedThisFrame;
#endif

    if (nodeCulled) {
        if (nodeCullClassification == CLOD_NODE_CULL_EXPLICIT_LIVE_BOUNDS)
            WGTelemetryAdd(WG_COUNTER_NODE_BOUNDS_EXPLICIT_FRUSTUM_REJECTED, 1u);
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_CULLED_NODE_RECORDS, 1);
        return;
    }

#if !CLOD_PC_LEAF_ONLY
    if (node.range.isLeaf == CLOD_NODE_INSTANCE_ROOT) {
        WGTelemetryAdd(WG_COUNTER_ASSEMBLY_INSTANCE_ROOT_RECORDS, 1);
        if (rec.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_INSTANCE_ROOT_RECORDS, 1);
        }
        // Animated part portals use bind-pose proxy bounds, so their LOD test can
        // hide currently visible deformed geometry. Keep them open and defer the
        // precise visibility decision to animated meshlet bounds.
        const bool portalWantsTraversal = skinnedAssemblyPortal || CLodInstanceRootWantsTraversal(
            clodMeshMetadata,
            node,
            parentAllowsRefine,
            objectModelMatrix,
            lodUniformScale,
            lodCam,
            lodCameraIsOrtho,
            forceLodDecision,
            rec.instanceIndex,
            instanceData.perMeshBufferIndex,
            rec.viewId);
        if (!portalWantsTraversal)
        {
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_REJECTED_BY_ERROR_RECORDS, 1);
        }
        else
        {
            StructuredBuffer<ClusterLODAssemblyInstance> assemblyInstances =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyInstances)];
            if (node.range.indexOrOffset < clodMeshMetadata.assemblyInstanceCount) {
                const ClusterLODAssemblyInstance assemblyInstance =
                    assemblyInstances[clodMeshMetadata.assemblyInstanceBase + node.range.indexOrOffset];
                if (assemblyInstance.stackDepth <= CLOD_ASSEMBLY_MAX_STACK_DEPTH) {
                    uint outputIndex = 0u;
                    InterlockedAdd(nextCounter[0], 1u, outputIndex);
                    if (outputIndex < CLOD_WG_VISIBLE_CLUSTERS_CAPACITY) {
                        TraverseNodeRecord childRecord = (TraverseNodeRecord)0;
                        childRecord.instanceIndex = rec.instanceIndex;
                        childRecord.viewId = rec.viewId;
                        childRecord.nodeIdPacked = PackTraverseNodeId(
                            assemblyInstance.targetRootNode,
                            UnpackSourceTag(rec.nodeIdPacked),
                            1u,
                            0u);
                        childRecord.assemblyTransformIndex =
                            assemblyInstance.transformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL
                                ? rec.assemblyTransformIndex
                                : clodMeshMetadata.assemblyTransformBase + assemblyInstance.transformIndex;
                        nextFrontier[outputIndex] = childRecord;
                        WGTelemetryAdd(WG_COUNTER_TRAVERSE_TRAVERSE_RECORDS, 1);
                    }
                }
            }
        }
        return;
    }
#endif

#if !CLOD_PC_LEAF_ONLY
    if (node.range.isLeaf != CLOD_NODE_INTERNAL) {
#endif
        bool nodeTouchesDirtyPages = true;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (dirtyPageCullingEnabled) {
            const float3 nodeCullCenterWorld = mul(float4(nodeCullCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
            nodeTouchesDirtyPages = CLodVirtualShadowBoundsTouchDirtyPages(nodeCullCenterWorld, nodeRadiusWorld, rec.viewId);
        }
#endif

        CLodRenderableLeaf leaf;
        if (!CLodPrepareRenderableLeaf(
            clodMeshMetadata,
            node,
            parentAllowsRefine,
            objectModelMatrix,
            lodUniformScale,
            lodCam,
            lodCameraIsOrtho,
            nodeTouchesDirtyPages,
            forceLodDecision,
            rec.instanceIndex,
            instanceData.perMeshBufferIndex,
            rec.viewId,
            leaf))
        {
            return;
        }
        const bool assemblyPartVoxelLeaf =
            assemblyPortalTraversal &&
            leaf.isVoxel &&
            ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) == 0u);
        if (assemblyPartVoxelLeaf)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_VOXEL_LEAF_RECORDS, 1);
        }

        if (!forceLodDecision && CLodRefinedChildSuppressesParent(
            clodMeshMetadata.groupsBase,
            node.range.countMinusOne - 1u,
            node.range.countMinusOne != 0u,
            objectModelMatrix,
            lodUniformScale,
            lodCam,
            lodCameraIsOrtho,
            rec.instanceIndex,
            instanceData.perMeshBufferIndex,
            rec.viewId,
            leaf.errorOverDistance,
            0.0f.xxx,
            -1.0f,
            true,
            false))
        {
            WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CONDITION2, 1);
            if ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
            {
                WGTelemetryAdd(WG_COUNTER_ASSEMBLY_VOXEL_SUPPRESSED_BY_CHILD_RECORDS, 1);
            }
            return;
        }

        if (!leaf.canRender)
        {
            if ((leaf.group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
            {
                WGTelemetryAdd(WG_COUNTER_ASSEMBLY_VOXEL_NONRESIDENT_RECORDS, 1);
            }
            return;
        }

        if (leaf.isVoxel)
        {
#if CLOD_WG_ENABLE_VOXEL_OUTPUT
            if (assemblyPartVoxelLeaf)
            {
                WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_VOXEL_RASTER_WORK_RECORDS, 1);
            }
            CLodAppendVoxelRasterWorkForLeaf(
                clodMeshMetadata,
                rec.instanceIndex,
                rec.assemblyTransformIndex,
                rec.viewId,
                node,
                leaf.group,
                objectModelMatrix,
                lodUniformScale,
                cullCamera,
                dirtyPageCullingEnabled);
#endif
            return;
        }

        StructuredBuffer<ClusterLODGroupSegment> segments =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
        const uint segGlobalIndex = clodMeshMetadata.segmentsBase + node.range.indexOrOffset;
        const ClusterLODGroupSegment seg = segments[segGlobalIndex];

        if (seg.meshletCount == 0u) {
            return;
        }

        WGTelemetryAdd(WG_COUNTER_SEGMENT_EVALUATE_EMIT_BUCKET_THREADS, 1);
        if (assemblyPortalTraversal)
        {
            WGTelemetryAdd(WG_COUNTER_ASSEMBLY_PART_TRIANGLE_BUCKET_RECORDS, 1);
        }
        const GroupPageMapEntry pageEntry = LoadGroupPageMapEntry(clodMeshMetadata.pageMapBase, seg.pageIndex);
        if (pageEntry.slabDescriptorIndex == 0u)
        {
            WGTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB, 1);
            return;
        }
        const uint sourceTag = UnpackSourceTag(rec.nodeIdPacked);

        const uint phase2ExpansionFactor =
            PureComputeNormalizePhase2ExpansionFactor(CLOD_PC_PHASE2_EXPANSION_FACTOR);
        uint meshletBase = seg.firstMeshletInPage;
        uint remainingMeshlets = seg.meshletCount;
        const uint outputRecordCount =
            (remainingMeshlets + phase2ExpansionFactor - 1u) / phase2ExpansionFactor;
        const uint waveOutputOffset = WavePrefixSum(outputRecordCount);
        const uint waveOutputCount = WaveActiveSum(outputRecordCount);
        uint outputBase = 0u;
        if (WaveIsFirstLane()) {
            InterlockedAdd(clusterCounter[0], waveOutputCount, outputBase);
        }
        outputBase = WaveReadLaneFirst(outputBase) + waveOutputOffset;
        uint outputOffset = 0u;
        [loop]
        while (remainingMeshlets > 0u) {
            const uint chunkCount = min(phase2ExpansionFactor, remainingMeshlets);
            const uint outputIndex = outputBase + outputOffset;
            if (outputIndex < CLOD_WG_VISIBLE_CLUSTERS_CAPACITY) {
                CLodClusterRunRecord outRecord = (CLodClusterRunRecord)0;
                outRecord.instanceIndex = rec.instanceIndex;
                outRecord.viewId = rec.viewId;
                outRecord.groupIdPacked = PackGroupId(node.range.ownerGroupId, sourceTag);
                outRecord.clusterIndexAndCount = PackClusterIndexAndCount(meshletBase, chunkCount);
                outRecord.pageSlabDescriptorIndex = pageEntry.slabDescriptorIndex;
                outRecord.pageSlabByteOffset = pageEntry.slabByteOffset;
                outRecord.assemblyTransformIndex = rec.assemblyTransformIndex;
                clusterFrontier[outputIndex] = outRecord;
            }
            meshletBase += chunkCount;
            remainingMeshlets -= chunkCount;
            outputOffset++;
        }
        return;
#if !CLOD_PC_LEAF_ONLY
    }

    const float3 lodCheckWorldCenter = mul(float4(nodeLodCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
    const float lodCheckWorldRadius = nodeLodRadiusObjectSpace * lodUniformScale;
    const float nodeErrorOverDistance = ProjectedGeometricError(
        lodCheckWorldCenter,
        lodCheckWorldRadius,
        node.metric.maxQuadricError,
        lodUniformScale,
        lodCam.viewZ,
        lodCam.zNear,
        lodCameraIsOrtho);
    const bool nodeWantsTraversal =
        forceLodDecision ||
        (parentAllowsRefine && (nodeErrorOverDistance >= lodCam.errorOverDistanceThreshold));

    if (!nodeWantsTraversal) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_REJECTED_BY_ERROR_RECORDS, 1);
        return;
    }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    if (dirtyPageCullingEnabled) {
        const float3 nodeCullCenterWorld = mul(float4(nodeCullCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
        if (!CLodVirtualShadowBoundsTouchDirtyPages(nodeCullCenterWorld, nodeRadiusWorld, rec.viewId)) {
            WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES, 1);
            return;
        }
    }
#endif

    bool occlusionCulled = false;
    if (!isSkinned && CLodWorkGraphOcclusionEnabled() && (!cullCamera.isOrtho || CLOD_VSM_OCCLUSION_CULLING)) {
        StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
            ResourceDescriptorHeap[CLOD_WG_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
        const uint depthMapDescriptorIndex = viewDepthSRVIndices[cullViewId].linearDepthSRVIndex;
        if (depthMapDescriptorIndex != 0) {
            if (replaySource) {
                OcclusionCullingPerspectiveTexture2D(
                    occlusionCulled,
                    cullCamera,
                    nodeCenterViewSpace,
                    -nodeCenterViewSpace.z,
                    nodeRadiusWorld,
                    depthMapDescriptorIndex);
            } else {
                const row_major matrix prevModelMatrix = instanceTransform.prevModel;
                const float prevNodeCullScale = MaxAxisScale_RowVector(prevModelMatrix);
                const float3 prevNodeCenterViewSpace = ToViewSpace(nodeCullCenterObjectSpace, prevModelMatrix, cullCamera.prevView);
                const float prevNodeRadiusWorld = nodeCullRadiusObjectSpace * prevNodeCullScale;
                OcclusionCullingPerspectiveTexture2D(
                    occlusionCulled,
                    cullCamera,
                    prevNodeCenterViewSpace,
                    -prevNodeCenterViewSpace.z,
                    prevNodeRadiusWorld,
                    depthMapDescriptorIndex,
                    cullCamera.prevUnjitteredProjection);
            }
        }
    }

    if (occlusionCulled) {
        if (!replaySource) {
            ReplayTryAppendNode(rec.instanceIndex, rec.viewId, UnpackNodeId(rec.nodeIdPacked), rec.assemblyTransformIndex);
        }
        return;
    }

    const uint childCount = min(node.range.countMinusOne + 1u, BVH_MAX_CHILDREN);
    const uint sourceTag = UnpackSourceTag(rec.nodeIdPacked);
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_CHILD_LOOP_NODES, 1u);
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_CHILD_LOOP_SLOTS, childCount);
    uint emittedChildCount = 0u;

    [loop]
    for (uint childIndex = 0; childIndex < childCount; ++childIndex) {
        const uint childNodeId = node.range.indexOrOffset + childIndex;
        const ClusterLODNode child = lodNodes[clodMeshMetadata.lodNodesBase + childNodeId];

        // Reject internal children by their cheap hierarchy LOD bound before
        // doing any visibility-bound work.
        if (!forceLodDecision && child.range.isLeaf == CLOD_NODE_INTERNAL) {
            const float3 childWorldCenter = mul(float4(child.metric.lodCenterAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
            const float childLodRadiusWorld = child.metric.lodCenterAndRadius.w * lodUniformScale;
            const float childEOD = ProjectedGeometricError(
                childWorldCenter, childLodRadiusWorld,
                child.metric.maxQuadricError, lodUniformScale,
                lodCam.viewZ, lodCam.zNear,
                lodCameraIsOrtho);
            if (childEOD < lodCam.errorOverDistanceThreshold) {
                WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_LOD_REJECTED, 1);
                continue;
            }
        }

        // A skinned child resolves this same live sphere when it consumes the
        // emitted frontier record. Defer its frustum test to that thread rather
        // than evaluating every animated child twice. Static bind bounds remain
        // cheap enough to prefilter here.
        if (!isSkinned) {
            BoundingSphere childCullBounds = { child.metric.cullCenterAndRadius };
            const uint childCullClassification = CLOD_NODE_CULL_STATIC_BIND_POSE;
            CLodTelemetryNodeCullClassification(childCullClassification);
            const float3 childCullCenterOS = childCullBounds.sphere.xyz;
            const float childCullRadiusOS = childCullBounds.sphere.w;
            const float childRadiusWorld = childCullRadiusOS * cullUniformScale;
            if (CLodWorkGraphFrustumCullingEnabled() &&
                !replaySource &&
                CLodNodeBoundsOutsideFrustum(
                    childCullClassification,
                    child.metric.cullCenterAndRadius,
                    childCullBounds,
                    objectModelMatrix,
                    cullUniformScale,
                    cullCamera)) {
                WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_FRUSTUM_CULLED, 1);
                continue;
            }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
            if (dirtyPageCullingEnabled) {
                const float3 childCullCenterWorld = mul(float4(childCullCenterOS, 1.0f), objectModelMatrix).xyz;
                if (!CLodVirtualShadowBoundsTouchDirtyPages(childCullCenterWorld, childRadiusWorld, rec.viewId)) {
                    WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CLEAN_PAGES, 1);
                    continue;
                }
            }
#endif
        }

        const bool childIsLeaf =
            child.range.isLeaf != CLOD_NODE_INTERNAL &&
            child.range.isLeaf != CLOD_NODE_INSTANCE_ROOT;
        uint outputIndex = 0u;
        if (childIsLeaf) {
            InterlockedAdd(nextLeafCounter[0], 1u, outputIndex);
        }
        else {
            InterlockedAdd(nextCounter[0], 1u, outputIndex);
        }
        if (outputIndex >= CLOD_WG_VISIBLE_CLUSTERS_CAPACITY) {
            continue;
        }

        TraverseNodeRecord childRecord = (TraverseNodeRecord)0;
        childRecord.instanceIndex = rec.instanceIndex;
        childRecord.viewId = rec.viewId;
        childRecord.nodeIdPacked =
            PackTraverseNodeId(childNodeId, sourceTag, 1u, isSkinned ? 0u : 1u);
        childRecord.assemblyTransformIndex = rec.assemblyTransformIndex;
        if (childIsLeaf) {
            nextLeafFrontier[outputIndex] = childRecord;
        }
        else {
            nextFrontier[outputIndex] = childRecord;
        }
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_TRAVERSE_RECORDS, 1);
        emittedChildCount++;
    }
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_ACTIVE_CHILD_THREADS, emittedChildCount);
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_CHILD_RECORDS_EMITTED, emittedChildCount);
#endif
}

[numthreads(32, 1, 1)]
void PureComputeClusterFrontierCS(const uint3 dispatchThreadID : SV_DispatchThreadID, const uint GI : SV_GroupIndex)
{
    StructuredBuffer<CLodClusterRunRecord> inputFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> inputCountBuffer = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX];

    const uint inputCount = inputCountBuffer[0];
    const uint index = dispatchThreadID.x;
    const uint groupBase = dispatchThreadID.x - GI;
    const uint activeCount = (groupBase < inputCount) ? min(32u, inputCount - groupBase) : 0u;
    const bool hasBucket = (index < inputCount);
    CLodClusterRunRecord bucket = (CLodClusterRunRecord)0;
    if (hasBucket) {
        bucket = inputFrontier[index];
    }

    uint swPending = 0u;
    uint pageJobPending = 0u;
    uint reyesPending = 0u;
    // ClusterCullBody uses wave ops inside its meshlet loop, so all lanes in the wave
    // must execute the same iteration count even when bucket sizes differ.
    ClusterCullBody(bucket, hasBucket, true, GI, activeCount, 64u, swPending, pageJobPending, reyesPending);
}

groupshared CLodClusterRunRecord gs_phase2Records[64];
groupshared uint gs_phase2ActiveClusterCount;

[numthreads(64, 1, 1)]
void PureComputeDenseClusterWorkCS(
    const uint3 groupID : SV_GroupID,
    const uint GI : SV_GroupIndex)
{
    StructuredBuffer<CLodClusterRunRecord> inputFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> inputCountBuffer = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX];

    const uint inputCount = inputCountBuffer[0];
    const uint phase2ExpansionFactor =
        PureComputeNormalizePhase2ExpansionFactor(CLOD_PC_PHASE2_EXPANSION_FACTOR);
    const uint recordsPerGroup = 64u / phase2ExpansionFactor;
    const uint groupRecordBase = groupID.x * recordsPerGroup;
    const uint recordsThisGroup = (groupRecordBase < inputCount)
        ? min(recordsPerGroup, inputCount - groupRecordBase)
        : 0u;

    if (GI < recordsPerGroup) {
        const uint recordIndex = groupRecordBase + GI;
        if (recordIndex < inputCount) {
            gs_phase2Records[GI] = inputFrontier[recordIndex];
        } else {
            gs_phase2Records[GI] = (CLodClusterRunRecord)0;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const bool telemetryEnabled = CLodWorkGraphTelemetryEnabled();
    if (telemetryEnabled) {
        if (GI == 0u) {
            uint activeClusterCount = 0u;
            [loop]
            for (uint recordIndex = 0u; recordIndex < recordsThisGroup; ++recordIndex) {
                activeClusterCount += UnpackClusterCount(gs_phase2Records[recordIndex].clusterIndexAndCount);
            }
            gs_phase2ActiveClusterCount = activeClusterCount;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    const uint recordSlotInGroup = GI / phase2ExpansionFactor;
    const uint laneInRecord = GI - recordSlotInGroup * phase2ExpansionFactor;
    if (recordSlotInGroup >= recordsThisGroup) {
        return;
    }

    const CLodClusterRunRecord inputBucket = gs_phase2Records[recordSlotInGroup];
    const uint meshletCount = UnpackClusterCount(inputBucket.clusterIndexAndCount);
    if (laneInRecord == 0u && meshletCount > 0u) {
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_DENSE_EXPANSION_BUCKETS, 1u);
        WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_DENSE_CLUSTERS_DISPATCHED, meshletCount);
        if (UnpackGroupSourceTag(inputBucket.groupIdPacked) == CLOD_RECORD_SOURCE_REPLAY) {
            WGTelemetryAdd(WG_COUNTER_PHASE2_REPLAY_CLUSTER_BUCKET_RECORDS_CONSUMED, 1u);
        }
    }

    if (laneInRecord >= meshletCount) {
        return;
    }

    CLodClusterRunRecord bucket = inputBucket;
    bucket.clusterIndexAndCount =
        PackClusterIndexAndCount(UnpackClusterFirstIndex(inputBucket.clusterIndexAndCount) + laneInRecord, 1u);

    uint swPending = 0u;
    uint pageJobPending = 0u;
    uint reyesPending = 0u;
    const uint telemetryActiveClusterCount = telemetryEnabled ? gs_phase2ActiveClusterCount : 0u;
    ClusterCullBody(bucket, true, false, GI, telemetryActiveClusterCount, 1u, swPending, pageJobPending, reyesPending);
}
