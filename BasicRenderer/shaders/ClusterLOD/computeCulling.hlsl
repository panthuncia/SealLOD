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
    if (index == 0u) {
        outCounter[0] = count;
    }
    if (index >= count) {
        return;
    }

    const uint byteOffset = index * CLOD_NODE_REPLAY_STRIDE_BYTES;
    const uint2 header = replayBuffer.Load2(byteOffset);

    TraverseNodeRecord record = (TraverseNodeRecord)0;
    record.instanceIndex = header.x;
    record.nodeIdPacked = header.y;
    record.viewId = replayBuffer.Load(byteOffset + 8u);
    outFrontier[index] = record;
}

[numthreads(32, 1, 1)]
void SeedPureComputeReplayClustersCS(const uint3 dispatchThreadID : SV_DispatchThreadID)
{
    RWByteAddressBuffer replayBuffer = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReplayBufferState> replayState = ResourceDescriptorHeap[CLOD_WG_OCCLUSION_REPLAY_STATE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<MeshletBucketRecord> outFrontier = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX];
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
    const uint2 tail = replayBuffer.Load2(byteOffset + 16u);

    MeshletBucketRecord record = (MeshletBucketRecord)0;
    record.instanceIndex = head.x;
    record.viewId = head.y;
    record.groupIdPacked = head.z;
    record.meshletIndexAndCount = head.w;
    record.pageSlabDescriptorIndex = tail.x;
    record.pageSlabByteOffset = tail.y;
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
    const PerObjectBuffer instanceTransform = LoadInstanceTransformForDrawRecord(drawRecord);
    const row_major matrix objectModelMatrix = instanceTransform.model;
    StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const MeshInstanceClodOffsets off = LoadCLodOffsetsForDrawRecord(drawRecord);
    const CLodMeshMetadata clodMeshMetadata = clodMeshMetadataBuffer[off.clodMeshMetadataIndex];
    const bool voxelRootCandidate = CLodMeshHasVoxelRootGroup(clodMeshMetadata);
    if (voxelRootCandidate)
    {
        WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_CANDIDATES, 1);
    }

    StructuredBuffer<Camera> cameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX];

    const float3 objectSpaceCenter = instanceData.boundingSphere.sphere.xyz;
    const float3 viewSpaceCenter = ToViewSpace(objectSpaceCenter, objectModelMatrix, camera.view);
    const float worldRadius = instanceData.boundingSphere.sphere.w * MaxAxisScale_RowVector(objectModelMatrix);

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
                WGTelemetryAddObjectCullPlaneReject(planeIndex);
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

    RWStructuredBuffer<TraverseNodeRecord> outFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> outCounter = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX];

    uint outputIndex = 0u;
    InterlockedAdd(outCounter[0], 1u, outputIndex);
    if (outputIndex >= CLOD_WG_VISIBLE_CLUSTERS_CAPACITY) {
        return;
    }

    outFrontier[outputIndex].viewId = CLOD_PC_OBJECT_CULL_VIEW_DATA_INDEX;
    outFrontier[outputIndex].instanceIndex = drawRecordIndex;
    outFrontier[outputIndex].nodeIdPacked = PackTraverseNodeId(CLodResolveTraversalRootNode(clodMeshMetadata), CLOD_RECORD_SOURCE_PASS1, 1u);

    WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_VISIBLE_THREADS, 1);
    WGTelemetryAdd(WG_COUNTER_OBJECT_CULL_TRAVERSE_RECORDS, 1);
    if (voxelRootCandidate)
    {
        WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_VISIBLE, 1);
        WGTelemetryAdd(WG_COUNTER_VOXEL_OBJECT_TRAVERSE_RECORDS, 1);
    }
}

[numthreads(64, 1, 1)]
void PureComputeTraverseFrontierCS(const uint3 dispatchThreadID : SV_DispatchThreadID)
{
    StructuredBuffer<TraverseNodeRecord> inputFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> inputCountBuffer = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<TraverseNodeRecord> nextFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> nextCounter = ResourceDescriptorHeap[CLOD_PC_FRONTIER_OUTPUT_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<MeshletBucketRecord> clusterFrontier = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> clusterCounter = ResourceDescriptorHeap[CLOD_PC_CLUSTER_OUTPUT_COUNT_DESCRIPTOR_INDEX];

    const uint inputCount = inputCountBuffer[0];
    const uint index = dispatchThreadID.x;
    WGTelemetryAdd(WG_COUNTER_TRAVERSE_THREADS, 1);
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
    const PerObjectBuffer instanceTransform = LoadInstanceTransformForDrawRecord(drawRecord);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const PerMeshBuffer perMesh = perMeshBuffer[instanceData.perMeshBufferIndex];
    const bool isSkinned = (perMesh.vertexFlags & VERTEX_SKINNED) != 0u;
    const row_major matrix objectModelMatrix = instanceTransform.model;
    StructuredBuffer<Camera> cameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const uint cullViewId = rec.viewId;
    const uint lodViewId = CLodResolveLodViewId(cullViewId);
    const Camera cullCamera = cameras[cullViewId];
    const Camera lodCamera = cameras[lodViewId];
    StructuredBuffer<CullingCameraInfo> cameraInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    const CullingCameraInfo lodCam = cameraInfos[lodViewId];
    StructuredBuffer<ClusterLODNode> lodNodes =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Nodes)];

    const uint nodeLocalId = UnpackNodeId(rec.nodeIdPacked);
    const ClusterLODNode node = lodNodes[clodMeshMetadata.lodNodesBase + nodeLocalId];
    const bool voxelRootCandidate = CLodMeshHasVoxelRootGroup(clodMeshMetadata);
    if (voxelRootCandidate && nodeLocalId == CLodResolveTraversalRootNode(clodMeshMetadata))
    {
        WGTelemetryAdd(
            node.range.isLeaf == CLOD_NODE_INTERNAL
                ? WG_COUNTER_VOXEL_ROOT_INTERNAL_RECORDS
                : WG_COUNTER_VOXEL_ROOT_LEAF_RECORDS,
            1);
    }

    if (node.range.isLeaf == CLOD_NODE_INTERNAL) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_INTERNAL_NODE_RECORDS, 1);
    }
    else {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_LEAF_NODE_RECORDS, 1);
    }

    const float objectUniformScale = MaxAxisScale_RowVector(objectModelMatrix);
    const float cullUniformScale = objectUniformScale;
    const float lodUniformScale = objectUniformScale;
    const float3 nodeCullCenterObjectSpace = isSkinned ? instanceData.boundingSphere.sphere.xyz : node.metric.cullCenterAndRadius.xyz;
    const float nodeCullRadiusObjectSpace = isSkinned ? instanceData.boundingSphere.sphere.w : node.metric.cullCenterAndRadius.w;
    const float3 nodeLodCenterObjectSpace = node.metric.lodCenterAndRadius.xyz;
    const float nodeLodRadiusObjectSpace = node.metric.lodCenterAndRadius.w;
    const float3 nodeCenterViewSpace = ToViewSpace(nodeCullCenterObjectSpace, objectModelMatrix, cullCamera.view);
    const float nodeRadiusWorld = nodeCullRadiusObjectSpace * cullUniformScale;
    const bool nodeCulled =
        CLodWorkGraphFrustumCullingEnabled() &&
        !replaySource &&
        SphereOutsideFrustumViewSpace(nodeCenterViewSpace, nodeRadiusWorld, cullCamera);

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    const bool objectInvalidatedThisFrame = CLodVirtualShadowInstanceInvalidatedThisFrame(rec.instanceIndex);
    const bool dirtyPageCullingEnabled = CLodWorkGraphShadowDirtyPageCullingEnabled() && !objectInvalidatedThisFrame;
#endif

    if (nodeCulled) {
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_CULLED_NODE_RECORDS, 1);
        return;
    }

    if (node.range.isLeaf != CLOD_NODE_INTERNAL) {
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
            lodCamera.isOrtho,
            nodeTouchesDirtyPages,
            forceLodDecision,
            rec.instanceIndex,
            instanceData.perMeshBufferIndex,
            rec.viewId,
            leaf))
        {
            return;
        }

        if (!forceLodDecision && CLodRefinedChildSuppressesParent(
            clodMeshMetadata.groupsBase,
            node.range.countMinusOne - 1u,
            node.range.countMinusOne != 0u,
            objectModelMatrix,
            lodUniformScale,
            lodCam,
            lodCamera.isOrtho,
            rec.instanceIndex,
            instanceData.perMeshBufferIndex,
            rec.viewId,
            leaf.errorOverDistance,
            0.0f.xxx,
            -1.0f,
            false))
        {
            WGTelemetryAdd(WG_COUNTER_CLUSTER_CULL_REJECTED_CONDITION2, 1);
            return;
        }

        if (!leaf.canRender)
        {
            return;
        }

        if (leaf.isVoxel)
        {
            StructuredBuffer<ClusterLODGroupSegment> segments =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
            const uint segGlobalIndex = clodMeshMetadata.segmentsBase + node.range.indexOrOffset;
            const ClusterLODGroupSegment seg = segments[segGlobalIndex];
            WGTelemetryAdd(WG_COUNTER_TRAVERSE_VOXEL_SEGMENT_PAGE_HITS, 1);
            CLodAppendVoxelRasterClusterWork(
                clodMeshMetadata,
                rec.instanceIndex,
                rec.viewId,
                node.range.ownerGroupId,
                leaf.group,
                seg,
                objectModelMatrix,
                lodUniformScale,
                cullCamera,
                lodCam,
                lodCamera.isOrtho,
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                dirtyPageCullingEnabled,
#else
                false,
#endif
                instanceData.perMeshBufferIndex,
                leaf.errorOverDistance);
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
        [loop]
        while (remainingMeshlets > 0u) {
            const uint chunkCount = min(phase2ExpansionFactor, remainingMeshlets);
            uint outputIndex = 0u;
            InterlockedAdd(clusterCounter[0], 1u, outputIndex);
            if (outputIndex < CLOD_WG_VISIBLE_CLUSTERS_CAPACITY) {
                MeshletBucketRecord outRecord = (MeshletBucketRecord)0;
                outRecord.instanceIndex = rec.instanceIndex;
                outRecord.viewId = rec.viewId;
                outRecord.groupIdPacked = PackGroupId(node.range.ownerGroupId, sourceTag);
                outRecord.meshletIndexAndCount = PackMeshletIndexAndCount(meshletBase, chunkCount);
                outRecord.pageSlabDescriptorIndex = pageEntry.slabDescriptorIndex;
                outRecord.pageSlabByteOffset = pageEntry.slabByteOffset;
                clusterFrontier[outputIndex] = outRecord;
            }
            meshletBase += chunkCount;
            remainingMeshlets -= chunkCount;
        }
        return;
    }

    const float3 lodCheckWorldCenter = mul(float4(nodeLodCenterObjectSpace, 1.0f), objectModelMatrix).xyz;
    const float lodCheckWorldRadius = nodeLodRadiusObjectSpace * lodUniformScale;
    const float nodeErrorOverDistance = ProjectedGeometricError(
        lodCheckWorldCenter,
        lodCheckWorldRadius,
        node.metric.maxQuadricError,
        lodUniformScale,
        lodCam.positionWorldSpace.xyz,
        lodCam.zNear,
        lodCamera.isOrtho);
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
    if (CLodWorkGraphOcclusionEnabled() && (!cullCamera.isOrtho || CLOD_VSM_OCCLUSION_CULLING)) {
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
            ReplayTryAppendNode(rec.instanceIndex, rec.viewId, UnpackNodeId(rec.nodeIdPacked));
        }
        return;
    }

    const uint childCount = min(node.range.countMinusOne + 1u, BVH_MAX_CHILDREN);
    const uint sourceTag = UnpackSourceTag(rec.nodeIdPacked);

    [loop]
    for (uint childIndex = 0; childIndex < childCount; ++childIndex) {
        const uint childNodeId = node.range.indexOrOffset + childIndex;
        const ClusterLODNode child = lodNodes[clodMeshMetadata.lodNodesBase + childNodeId];

        const float3 childCullCenterOS = isSkinned ? instanceData.boundingSphere.sphere.xyz : child.metric.cullCenterAndRadius.xyz;
        const float childCullRadiusOS = isSkinned ? instanceData.boundingSphere.sphere.w : child.metric.cullCenterAndRadius.w;
        const float3 childCenterVS = ToViewSpace(childCullCenterOS, objectModelMatrix, cullCamera.view);
        const float childRadiusWorld = childCullRadiusOS * cullUniformScale;
        if (CLodWorkGraphFrustumCullingEnabled() &&
            !replaySource &&
            SphereOutsideFrustumViewSpace(childCenterVS, childRadiusWorld, cullCamera)) {
            WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_FRUSTUM_CULLED, 1);
            continue;
        }

        if (!forceLodDecision && child.range.isLeaf == CLOD_NODE_INTERNAL) {
            const float3 childWorldCenter = mul(float4(child.metric.lodCenterAndRadius.xyz, 1.0f), objectModelMatrix).xyz;
            const float childLodRadiusWorld = child.metric.lodCenterAndRadius.w * lodUniformScale;
            const float childEOD = ProjectedGeometricError(
                childWorldCenter, childLodRadiusWorld,
                child.metric.maxQuadricError, lodUniformScale,
                lodCam.positionWorldSpace.xyz, lodCam.zNear,
                lodCamera.isOrtho);
            if (childEOD < lodCam.errorOverDistanceThreshold) {
                WGTelemetryAdd(WG_COUNTER_CHILD_PREFILTER_LOD_REJECTED, 1);
                continue;
            }
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

        uint outputIndex = 0u;
        InterlockedAdd(nextCounter[0], 1u, outputIndex);
        if (outputIndex >= CLOD_WG_VISIBLE_CLUSTERS_CAPACITY) {
            continue;
        }

        TraverseNodeRecord childRecord = (TraverseNodeRecord)0;
        childRecord.instanceIndex = rec.instanceIndex;
        childRecord.viewId = rec.viewId;
        childRecord.nodeIdPacked = PackTraverseNodeId(childNodeId, sourceTag, 1u);
        nextFrontier[outputIndex] = childRecord;
        WGTelemetryAdd(WG_COUNTER_TRAVERSE_TRAVERSE_RECORDS, 1);
    }
}

[numthreads(32, 1, 1)]
void PureComputeClusterFrontierCS(const uint3 dispatchThreadID : SV_DispatchThreadID, const uint GI : SV_GroupIndex)
{
    StructuredBuffer<MeshletBucketRecord> inputFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> inputCountBuffer = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_COUNT_DESCRIPTOR_INDEX];

    const uint inputCount = inputCountBuffer[0];
    const uint index = dispatchThreadID.x;
    const uint groupBase = dispatchThreadID.x - GI;
    const uint activeCount = (groupBase < inputCount) ? min(32u, inputCount - groupBase) : 0u;
    const bool hasBucket = (index < inputCount);
    MeshletBucketRecord bucket = (MeshletBucketRecord)0;
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

groupshared MeshletBucketRecord gs_phase2Records[64];
groupshared uint gs_phase2ActiveClusterCount;

[numthreads(64, 1, 1)]
void PureComputeDenseClusterWorkCS(
    const uint3 groupID : SV_GroupID,
    const uint GI : SV_GroupIndex)
{
    StructuredBuffer<MeshletBucketRecord> inputFrontier = ResourceDescriptorHeap[CLOD_PC_FRONTIER_INPUT_DESCRIPTOR_INDEX];
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
            gs_phase2Records[GI] = (MeshletBucketRecord)0;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u) {
        uint activeClusterCount = 0u;
        [loop]
        for (uint recordIndex = 0u; recordIndex < recordsThisGroup; ++recordIndex) {
            activeClusterCount += UnpackMeshletCount(gs_phase2Records[recordIndex].meshletIndexAndCount);
        }
        gs_phase2ActiveClusterCount = activeClusterCount;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint recordSlotInGroup = GI / phase2ExpansionFactor;
    const uint laneInRecord = GI - recordSlotInGroup * phase2ExpansionFactor;
    if (recordSlotInGroup >= recordsThisGroup) {
        return;
    }

    const MeshletBucketRecord inputBucket = gs_phase2Records[recordSlotInGroup];
    const uint meshletCount = UnpackMeshletCount(inputBucket.meshletIndexAndCount);
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

    MeshletBucketRecord bucket = inputBucket;
    bucket.meshletIndexAndCount =
        PackMeshletIndexAndCount(UnpackMeshletFirstIndex(inputBucket.meshletIndexAndCount) + laneInRecord, 1u);

    uint swPending = 0u;
    uint pageJobPending = 0u;
    uint reyesPending = 0u;
    ClusterCullBody(bucket, true, false, GI, gs_phase2ActiveClusterCount, 1u, swPending, pageJobPending, reyesPending);
}
