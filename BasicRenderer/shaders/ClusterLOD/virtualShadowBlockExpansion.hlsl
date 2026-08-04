#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodPageJobRasterShared.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "PerPassRootConstants/clodVirtualShadowBlockExpandRootConstants.h"
#include "PerPassRootConstants/clodVirtualShadowBuildArgsRootConstants.h"

struct RasterizeClustersCommand
{
    uint baseClusterOffset;
    uint xDim;
    uint rasterBucketID;
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

static const uint kVsmBlockTrackedCapacity = kCLodVirtualShadowMaxBlocksPerClipmap;

groupshared float2 gs_screenPos[SW_RASTER_MAX_VERTS];
groupshared float2 gs_coverageMin;
groupshared float2 gs_coverageMax;
groupshared uint gs_useCluster;
groupshared uint gs_minBlockX;
groupshared uint gs_minBlockY;
groupshared uint gs_blockCountX;
groupshared uint gs_blockCountY;
groupshared uint gs_totalBlockCount;
groupshared uint gs_activeBlockCount;
groupshared uint gs_outputBaseIndex;
groupshared uint gs_committedCount;
groupshared uint gs_emitPackedVirtualBlockOrigins[kVsmBlockTrackedCapacity];
groupshared uint gs_emitPackedActiveRects[kVsmBlockTrackedCapacity];

void VsmStoreTrackedBlockMeta(uint slot, CLodVirtualShadowBlockMeta blockMeta)
{
    gs_emitPackedVirtualBlockOrigins[slot] = blockMeta.packedVirtualBlockOrigin;
    gs_emitPackedActiveRects[slot] = blockMeta.packedActiveRectAndFlags;
}

bool VsmComputeBlockCoverage(
    float2 ssMin,
    float2 ssMax,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    out uint2 minBlockCoord,
    out uint2 blockCount)
{
    const float virtualResolution = (float)max(clipmapInfo.virtualResolution, 1u);
    if (ssMax.x < 0.0f || ssMax.y < 0.0f || ssMin.x >= virtualResolution || ssMin.y >= virtualResolution)
    {
        minBlockCoord = uint2(0u, 0u);
        blockCount = uint2(0u, 0u);
        return false;
    }

    const float2 clampedMin = clamp(ssMin, float2(0.0f, 0.0f), float2(virtualResolution - 1.0f, virtualResolution - 1.0f));
    const float2 clampedMax = clamp(ssMax, float2(0.0f, 0.0f), float2(virtualResolution - 1.0f, virtualResolution - 1.0f));
    const uint2 minPageCoord = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(clampedMin), clipmapInfo);
    const uint2 maxPageCoord = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(clampedMax), clipmapInfo);
    minBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(minPageCoord);
    const uint2 maxBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(maxPageCoord);
    blockCount = maxBlockCoord - minBlockCoord + 1u;
    return all(blockCount > uint2(0u, 0u));
}

bool VsmLoadActiveBlockMeta(
    uint2 blockCoord,
    uint shadowClipmapIndex,
    StructuredBuffer<uint> activeBlockMetadata,
    out CLodVirtualShadowBlockMeta blockMeta)
{
    blockMeta = (CLodVirtualShadowBlockMeta)0;
    const uint blockLinearIndex =
        CLodVirtualShadowBlockLinearIndex(blockCoord, shadowClipmapIndex);
    const uint packedActiveRect = activeBlockMetadata[blockLinearIndex];
    if (packedActiveRect == 0xFFFFFFFFu)
    {
        return false;
    }
    blockMeta.packedVirtualBlockOrigin =
        CLodVirtualShadowPackBlockPageCoords(
            CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord));
    blockMeta.packedActiveRectAndFlags = packedActiveRect;
    return true;
}

void VsmLoadClusterScreenCoverage(
    uint4 packedCluster,
    uint assemblyTransformIndex,
    uint GI,
    out bool outHasClipmapInfo,
    out CLodVirtualShadowClipmapInfo outClipmapInfo)
{
    const uint viewID = CLodVisibleClusterViewID(packedCluster);
    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const uint shadowClipmapIndex = CLodVisibleClusterShadowClipmapIndex(packedCluster);

    const PerObjectBuffer objData =
        LoadInstanceTransformForDrawWithAssemblyTransform(instanceID, assemblyTransformIndex);
#if defined(PSO_SKINNED)
    PerMeshInstanceBuffer meshInst = LoadMeshTemplateForDraw(instanceID);
    // Keep virtual-shadow deformation on the same transient wind palette as visibility rasterization.
    meshInst.skinningInstanceSlot = CLodVisibleClusterUsesDynamicShadowLayer(packedCluster)
        ? ResolveProceduralWindSkinningSlot(instanceID, meshInst.skinningInstanceSlot)
        : 0xFFFFFFFFu;
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceID);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
#endif

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    outHasClipmapInfo = false;
    outClipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    if (shadowClipmapIndex < kCLodVirtualShadowClipmapCount)
    {
        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[shadowClipmapIndex];
        if (CLodVirtualShadowClipmapIsValid(clipmapInfo) && clipmapInfo.shadowCameraBufferIndex == viewID)
        {
            outHasClipmapInfo = true;
            outClipmapInfo = clipmapInfo;
        }
    }

    if (!outHasClipmapInfo)
    {
        return;
    }

    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDescriptorIndex,
        pageSlabByteOffset,
        hdr.descriptorOffset,
        localMeshletIndex);
    const uint vertCount = CLodDescVertexCount(desc);

    StructuredBuffer<CullingCameraInfo> cullingCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    const CullingCameraInfo cam = cullingCameras[viewID];
    const row_major matrix modelViewProjection = mul(objData.model, cam.viewProjection);
#if !defined(PSO_SKINNED)
    if (GI == 0u)
    {
        const float4 clipCenter = mul(float4(desc.bounds.xyz, 1.0f), modelViewProjection);
        const float inverseW = rcp(max(abs(clipCenter.w), 1.0e-8f));
        const float2 ndcCenter = clipCenter.xy * inverseW;
        const float2 ndcRadius = desc.bounds.w * float2(
            length(float3(modelViewProjection[0][0], modelViewProjection[1][0], modelViewProjection[2][0])),
            length(float3(modelViewProjection[0][1], modelViewProjection[1][1], modelViewProjection[2][1]))) * inverseW;
        const float virtualResolution = (float)max(outClipmapInfo.virtualResolution, 1u);
        const float2 screenCenter = float2(
            (ndcCenter.x + 1.0f) * 0.5f * virtualResolution,
            (1.0f - ndcCenter.y) * 0.5f * virtualResolution);
        const float2 screenRadius = ndcRadius * (0.5f * virtualResolution);
        gs_coverageMin = screenCenter - screenRadius;
        gs_coverageMax = screenCenter + screenRadius;
    }
    return;
#endif
    const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;

    for (uint v = GI; v < vertCount; v += SW_RASTER_THREADS)
    {
        float3 localPos = PJ_DecodeCompressedPosition(
            v,
            positionBitstreamBase,
            desc.positionBitOffset,
            CLodDescBitsX(desc), CLodDescBitsY(desc), CLodDescBitsZ(desc),
            hdr.compressedPositionQuantExp,
            int3(desc.minQx, desc.minQy, desc.minQz),
            pageSlabDescriptorIndex);
#if defined(PSO_SKINNED)
        SkinningInfluences skinning = PJ_DecodePackedJoints(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex);
        skinning = PJ_DecodePackedWeights(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex, skinning);
        skinning = ResolveAssemblySkinningInfluences(skinning, metadata, assemblyTransformIndex);
        localPos = mul(float4(localPos, 1.0f), BuildAssemblyLocalSkinMatrix(
            meshInst.skinningInstanceSlot, skinning, assemblyTransformIndex)).xyz;
#endif

        const float4 clipPos = mul(float4(localPos, 1.0f), modelViewProjection);
        const float invW = 1.0f / clipPos.w;
        const float2 ndc = clipPos.xy * invW;
        const float virtualResolution = (float)max(outClipmapInfo.virtualResolution, 1u);
        gs_screenPos[v] = float2(
            (ndc.x + 1.0f) * 0.5f * virtualResolution,
            (1.0f - ndc.y) * 0.5f * virtualResolution);
    }
}

[shader("compute")]
[numthreads(SW_RASTER_THREADS, 1, 1)]
void CLodVirtualShadowBlockHistogramCSMain(uint3 dtid : SV_DispatchThreadID, uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> sourceHistogram = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_SOURCE_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketID = IndirectCommandSignatureRootConstant2;
    const uint clusterCount = sourceHistogram[bucketID];
    const uint linearizedGroupID = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    if (linearizedGroupID >= clusterCount)
    {
        return;
    }

    const uint sortedClusterIndex = IndirectCommandSignatureRootConstant0 + linearizedGroupID;
    ByteAddressBuffer sourceClusters = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> sourceClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(sourceClusters, sortedClusterIndex);
    const uint sourceClusterTransformIndex = sourceClusterTransformIndices[sortedClusterIndex];
    RWStructuredBuffer<uint> clusterCoverage =
        ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_CLUSTER_COVERAGE_DESCRIPTOR_INDEX];

    if (GI == 0u)
    {
        gs_useCluster = 0u;
        gs_activeBlockCount = 0u;
        gs_totalBlockCount = 0u;
        clusterCoverage[sortedClusterIndex] = 0xFFFFFFFFu;
    }

    bool hasClipmapInfo = false;
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    VsmLoadClusterScreenCoverage(packedCluster, sourceClusterTransformIndex, GI, hasClipmapInfo, clipmapInfo);
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u && hasClipmapInfo)
    {
        float2 ssMin;
        float2 ssMax;
#if defined(PSO_SKINNED)
        const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
        const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
        const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
        const CLodMeshletDescriptor desc = LoadMeshletDescriptor(
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            hdr.descriptorOffset,
            CLodVisibleClusterLocalMeshletIndex(packedCluster));
        PJ_ComputeScreenBounds(gs_screenPos, CLodDescVertexCount(desc), ssMin, ssMax);
#else
        ssMin = gs_coverageMin;
        ssMax = gs_coverageMax;
#endif

        uint2 minBlockCoord;
        uint2 blockCount;
        if (VsmComputeBlockCoverage(ssMin, ssMax, clipmapInfo, minBlockCoord, blockCount))
        {
            gs_minBlockX = minBlockCoord.x;
            gs_minBlockY = minBlockCoord.y;
            gs_blockCountX = blockCount.x;
            gs_blockCountY = blockCount.y;
            gs_totalBlockCount = blockCount.x * blockCount.y;
            gs_useCluster = 1u;
            clusterCoverage[sortedClusterIndex] =
                minBlockCoord.x |
                (minBlockCoord.y << 8u) |
                ((blockCount.x - 1u) << 16u) |
                ((blockCount.y - 1u) << 24u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gs_useCluster == 0u)
    {
        return;
    }

    const uint shadowClipmapIndex =
        CLodVisibleClusterShadowClipmapIndex(packedCluster);
    StructuredBuffer<uint> activeBlockMetadata =
        ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX];
    [loop]
    for (uint blockLinearIndex = GI; blockLinearIndex < gs_totalBlockCount; blockLinearIndex += SW_RASTER_THREADS)
    {
        const uint2 blockCoord = uint2(blockLinearIndex % gs_blockCountX, blockLinearIndex / gs_blockCountX) + uint2(gs_minBlockX, gs_minBlockY);
        CLodVirtualShadowBlockMeta blockMeta;
        if (VsmLoadActiveBlockMeta(
                blockCoord,
                shadowClipmapIndex,
                activeBlockMetadata,
                blockMeta))
        {
            InterlockedAdd(gs_activeBlockCount, 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u && gs_activeBlockCount != 0u)
    {
        // Never fall back to one unscoped cluster when block coverage exceeds
        // the tracking cap. That cluster can rasterize every admitted page its
        // triangles touch, defeating both page-local ownership and the work
        // budget. Emit the bounded tracked subset; deferred pages remain dirty
        // and will be covered by later frames.
        const uint emittedCount = gs_activeBlockCount;
        RWStructuredBuffer<uint> expandedHistogram = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_HISTOGRAM_DESCRIPTOR_INDEX];
        InterlockedAdd(expandedHistogram[bucketID], emittedCount);
        RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
            ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_STATS_DESCRIPTOR_INDEX];
        InterlockedAdd(statsBuffer[0].blockExpandedRequestedRecordCount, emittedCount);
    }
}

[shader("compute")]
[numthreads(SW_RASTER_THREADS, 1, 1)]
void CLodVirtualShadowBlockEmitCSMain(uint3 dtid : SV_DispatchThreadID, uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> sourceHistogram = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_SOURCE_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketID = IndirectCommandSignatureRootConstant2;
    const uint clusterCount = sourceHistogram[bucketID];
    const uint linearizedGroupID = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    if (linearizedGroupID >= clusterCount)
    {
        return;
    }

    const uint sortedClusterIndex = IndirectCommandSignatureRootConstant0 + linearizedGroupID;
    ByteAddressBuffer sourceClusters = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(sourceClusters, sortedClusterIndex);
    StructuredBuffer<uint> sourceClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_SOURCE_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint sourceClusterTransformIndex = sourceClusterTransformIndices[sortedClusterIndex];

    if (GI == 0u)
    {
        gs_useCluster = 0u;
        gs_activeBlockCount = 0u;
        gs_totalBlockCount = 0u;
        gs_outputBaseIndex = 0u;
        gs_committedCount = 0u;
    }

    if (GI == 0u)
    {
        StructuredBuffer<uint> clusterCoverage =
            ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_CLUSTER_COVERAGE_DESCRIPTOR_INDEX];
        const uint packedCoverage = clusterCoverage[sortedClusterIndex];
        if (packedCoverage != 0xFFFFFFFFu)
        {
            gs_minBlockX = packedCoverage & 0xFFu;
            gs_minBlockY = (packedCoverage >> 8u) & 0xFFu;
            gs_blockCountX = ((packedCoverage >> 16u) & 0xFFu) + 1u;
            gs_blockCountY = ((packedCoverage >> 24u) & 0xFFu) + 1u;
            gs_totalBlockCount = gs_blockCountX * gs_blockCountY;
            gs_useCluster = 1u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gs_useCluster == 0u)
    {
        return;
    }

    const uint shadowClipmapIndex =
        CLodVisibleClusterShadowClipmapIndex(packedCluster);
    StructuredBuffer<uint> activeBlockMetadata =
        ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_ACTIVE_BLOCK_METADATA_DESCRIPTOR_INDEX];
    [loop]
    for (uint blockLinearIndex = GI; blockLinearIndex < gs_totalBlockCount; blockLinearIndex += SW_RASTER_THREADS)
    {
        const uint2 blockCoord = uint2(blockLinearIndex % gs_blockCountX, blockLinearIndex / gs_blockCountX) + uint2(gs_minBlockX, gs_minBlockY);
        CLodVirtualShadowBlockMeta blockMeta;
        if (!VsmLoadActiveBlockMeta(
                blockCoord,
                shadowClipmapIndex,
                activeBlockMetadata,
                blockMeta))
        {
            continue;
        }

        uint slot = 0u;
        InterlockedAdd(gs_activeBlockCount, 1u, slot);
        if (slot < kVsmBlockTrackedCapacity)
        {
            VsmStoreTrackedBlockMeta(slot, blockMeta);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u)
    {
        const uint requestedCount = gs_activeBlockCount;
        if (requestedCount != 0u)
        {
            StructuredBuffer<uint> expandedOffsets = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_OFFSETS_DESCRIPTOR_INDEX];
            RWStructuredBuffer<uint> expandedWriteCursor = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_WRITE_CURSOR_DESCRIPTOR_INDEX];
            uint localBaseIndex = 0u;
            InterlockedAdd(expandedWriteCursor[bucketID], requestedCount, localBaseIndex);
            gs_outputBaseIndex = expandedOffsets[bucketID] + localBaseIndex;

            const uint recordCapacity = CLOD_VSM_BLOCK_EXPAND_RECORD_CAPACITY;
            gs_committedCount =
                (gs_outputBaseIndex < recordCapacity)
                    ? min(requestedCount, recordCapacity - gs_outputBaseIndex)
                    : 0u;

            RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
                ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_STATS_DESCRIPTOR_INDEX];
            InterlockedAdd(
                statsBuffer[0].blockExpandedCommittedRecordCount,
                gs_committedCount);
            const uint droppedCount = requestedCount - gs_committedCount;
            if (droppedCount != 0u)
            {
                // The prefix offsets intentionally retain their original
                // allocation, but raster must consume only records that were
                // actually written. Otherwise it reads stale records beyond
                // the capacity clamp.
                RWStructuredBuffer<uint> expandedHistogram =
                    ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_HISTOGRAM_DESCRIPTOR_INDEX];
                InterlockedAdd(expandedHistogram[bucketID], 0u - droppedCount);
                InterlockedAdd(
                    statsBuffer[0].blockExpandedDroppedRecordCount,
                    droppedCount);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gs_committedCount == 0u)
    {
        return;
    }

    RWByteAddressBuffer expandedClusters = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> expandedClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];

    for (uint outputIndex = GI;
         outputIndex < gs_committedCount;
         outputIndex += SW_RASTER_THREADS)
    {
        const uint2 virtualBlockOrigin =
            CLodVirtualShadowUnpackBlockPageCoords(
                gs_emitPackedVirtualBlockOrigins[outputIndex]);
        const uint2 blockCoord =
            CLodVirtualShadowBlockCoordFromPageCoord(
                virtualBlockOrigin);
        const uint2 minLocalPageCoord =
            CLodVirtualShadowUnpackBlockActiveRectMin(
                gs_emitPackedActiveRects[outputIndex]);
        const uint2 maxLocalPageCoord =
            CLodVirtualShadowUnpackBlockActiveRectMax(
                gs_emitPackedActiveRects[outputIndex]);
        uint blockPayload =
            CLodPackVisibleClusterVsmPayloadForBlock(
                CLodVisibleClusterShadowClipmapIndex(packedCluster),
                blockCoord,
                minLocalPageCoord,
                maxLocalPageCoord,
                false);
        if (CLodVisibleClusterIsVoxel(packedCluster))
        {
            blockPayload =
                CLodVisibleClusterMarkVoxelPayload(blockPayload);
        }
        uint4 expandedCluster = packedCluster;
        expandedCluster.w =
            (packedCluster.w &
                CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_HIGH_WORD_MASK) |
            blockPayload;
        CLodStoreVisibleClusterPackedWordsRW(
            expandedClusters,
            gs_outputBaseIndex + outputIndex,
            expandedCluster);
        expandedClusterTransformIndices[gs_outputBaseIndex + outputIndex] =
            sourceClusterTransformIndex;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CLodVirtualShadowBuildRasterArgsCSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint bucketIndex = dtid.x;
    if (bucketIndex >= CLOD_VSM_BUILD_ARGS_NUM_BUCKETS)
    {
        return;
    }

    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_VSM_BUILD_ARGS_HISTOGRAM_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[CLOD_VSM_BUILD_ARGS_OFFSETS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<RasterizeClustersCommand> outArgs = ResourceDescriptorHeap[CLOD_VSM_BUILD_ARGS_INDIRECT_ARGS_DESCRIPTOR_INDEX];

    RasterizeClustersCommand cmd = (RasterizeClustersCommand)0;
    const uint count = histogram[bucketIndex];
    if (count > 0u)
    {
        const uint kMaxDim = 65535u;
        uint dispatchX = (uint)ceil(sqrt((float)count));
        dispatchX = min(dispatchX, kMaxDim);
        uint dispatchY = (count + dispatchX - 1u) / dispatchX;
        dispatchY = min(dispatchY, kMaxDim);

        cmd.baseClusterOffset = offsets[bucketIndex];
        cmd.xDim = dispatchX;
        cmd.rasterBucketID = bucketIndex;
        cmd.dispatchX = dispatchX;
        cmd.dispatchY = dispatchY;
        cmd.dispatchZ = 1u;
    }

    outArgs[bucketIndex] = cmd;
}
