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

static const uint kVsmBlockTrackedCapacity = kCLodVirtualShadowBlockMaxTrackedPerCluster;

groupshared float2 gs_screenPos[SW_RASTER_MAX_VERTS];
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
groupshared uint gs_emitPackedWrappedBlockOrigins[kVsmBlockTrackedCapacity];
groupshared uint gs_emitActiveMasks[kVsmBlockTrackedCapacity];
groupshared uint gs_emitPackedActiveRects[kVsmBlockTrackedCapacity];
groupshared uint gs_emitPackedPhysicalPageIndices[kVsmBlockTrackedCapacity * 8u];

uint VsmEmitPhysicalPageIndexArrayOffset(uint slot)
{
    return slot * 8u;
}

void VsmStoreTrackedBlockMeta(uint slot, CLodVirtualShadowBlockMeta blockMeta)
{
    gs_emitPackedVirtualBlockOrigins[slot] = blockMeta.packedVirtualBlockOrigin;
    gs_emitPackedWrappedBlockOrigins[slot] = blockMeta.packedWrappedBlockOrigin;
    gs_emitActiveMasks[slot] = blockMeta.activePageMask;
    gs_emitPackedActiveRects[slot] = blockMeta.packedActiveRectAndFlags;

    const uint baseIndex = VsmEmitPhysicalPageIndexArrayOffset(slot);
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        gs_emitPackedPhysicalPageIndices[baseIndex + i] = blockMeta.packedPhysicalPageIndices[i];
    }
}

CLodVirtualShadowBlockMeta VsmLoadTrackedBlockMeta(uint slot)
{
    CLodVirtualShadowBlockMeta blockMeta = (CLodVirtualShadowBlockMeta)0;
    blockMeta.packedVirtualBlockOrigin = gs_emitPackedVirtualBlockOrigins[slot];
    blockMeta.packedWrappedBlockOrigin = gs_emitPackedWrappedBlockOrigins[slot];
    blockMeta.activePageMask = gs_emitActiveMasks[slot];
    blockMeta.packedActiveRectAndFlags = gs_emitPackedActiveRects[slot];

    const uint baseIndex = VsmEmitPhysicalPageIndexArrayOffset(slot);
    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        blockMeta.packedPhysicalPageIndices[i] = gs_emitPackedPhysicalPageIndices[baseIndex + i];
    }
    return blockMeta;
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

bool VsmBuildBlockMeta(
    uint2 blockOriginPageCoord,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable,
    out CLodVirtualShadowBlockMeta blockMeta)
{
    blockMeta = (CLodVirtualShadowBlockMeta)0;

    [unroll]
    for (uint packedPairIndex = 0u; packedPairIndex < 8u; ++packedPairIndex)
    {
        blockMeta.packedPhysicalPageIndices[packedPairIndex] = 0xFFFFFFFFu;
    }

    uint activeMask = 0u;
    uint2 minLocalPageCoord = uint2(3u, 3u);
    uint2 maxLocalPageCoord = uint2(0u, 0u);

    [unroll]
    for (uint localPageY = 0u; localPageY < kCLodVirtualShadowBlockPagesPerAxis; ++localPageY)
    {
        [unroll]
        for (uint localPageX = 0u; localPageX < kCLodVirtualShadowBlockPagesPerAxis; ++localPageX)
        {
            const uint2 localPageCoord = uint2(localPageX, localPageY);
            const uint2 pageCoord = blockOriginPageCoord + localPageCoord;
            if (pageCoord.x >= clipmapInfo.pageTableResolution || pageCoord.y >= clipmapInfo.pageTableResolution)
            {
                continue;
            }

            const uint2 wrappedPageCoord = CLodVirtualShadowWrappedPageCoords(pageCoord, clipmapInfo);
            const uint pageEntry = pageTable[uint3(wrappedPageCoord, clipmapInfo.pageTableLayer)];
            if (!CLodVirtualShadowPageEntryCanRaster(pageEntry))
            {
                continue;
            }

            const uint localPageIndex = CLodVirtualShadowBlockLocalPageIndex(localPageCoord);
            const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
            activeMask |= (1u << localPageIndex);
            minLocalPageCoord = min(minLocalPageCoord, localPageCoord);
            maxLocalPageCoord = max(maxLocalPageCoord, localPageCoord);

            const uint packedPairIndex = localPageIndex >> 1u;
            const uint halfShift = (localPageIndex & 1u) * 16u;
            blockMeta.packedPhysicalPageIndices[packedPairIndex] =
                (blockMeta.packedPhysicalPageIndices[packedPairIndex] & ~(0xFFFFu << halfShift)) |
                ((physicalPageIndex & 0xFFFFu) << halfShift);
        }
    }

    if (activeMask == 0u)
    {
        return false;
    }

    blockMeta.packedVirtualBlockOrigin = CLodVirtualShadowPackBlockPageCoords(blockOriginPageCoord);
    blockMeta.packedWrappedBlockOrigin = CLodVirtualShadowPackBlockPageCoords(
        CLodVirtualShadowWrappedPageCoords(blockOriginPageCoord, clipmapInfo));
    blockMeta.activePageMask = activeMask;
    blockMeta.packedActiveRectAndFlags = CLodVirtualShadowPackBlockActiveRect(minLocalPageCoord, maxLocalPageCoord, false);
    return true;
}

void VsmLoadClusterScreenCoverage(
    uint4 packedCluster,
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

    const PerMeshInstanceBuffer meshInst = LoadMeshTemplateForDraw(instanceID);
    const PerObjectBuffer objData = LoadInstanceTransformForDraw(instanceID);

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
        localPos = mul(float4(localPos, 1.0f), BuildSkinMatrix(meshInst.skinningInstanceSlot, skinning)).xyz;
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
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(sourceClusters, sortedClusterIndex);

    if (GI == 0u)
    {
        gs_useCluster = 0u;
        gs_activeBlockCount = 0u;
        gs_totalBlockCount = 0u;
    }

    bool hasClipmapInfo = false;
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    VsmLoadClusterScreenCoverage(packedCluster, GI, hasClipmapInfo, clipmapInfo);
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u && hasClipmapInfo)
    {
        float2 ssMin;
        float2 ssMax;
        const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
        const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
        const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
        const CLodMeshletDescriptor desc = LoadMeshletDescriptor(
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            hdr.descriptorOffset,
            CLodVisibleClusterLocalMeshletIndex(packedCluster));
        PJ_ComputeScreenBounds(gs_screenPos, CLodDescVertexCount(desc), ssMin, ssMax);

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
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gs_useCluster == 0u)
    {
        return;
    }

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
    [loop]
    for (uint blockLinearIndex = GI; blockLinearIndex < gs_totalBlockCount; blockLinearIndex += SW_RASTER_THREADS)
    {
        const uint2 blockCoord = uint2(blockLinearIndex % gs_blockCountX, blockLinearIndex / gs_blockCountX) + uint2(gs_minBlockX, gs_minBlockY);
        CLodVirtualShadowBlockMeta blockMeta;
        if (VsmBuildBlockMeta(CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord), clipmapInfo, pageTable, blockMeta))
        {
            InterlockedAdd(gs_activeBlockCount, 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u && gs_activeBlockCount != 0u)
    {
        const uint emittedCount = gs_activeBlockCount > CLOD_VSM_BLOCK_EXPAND_BLOCK_SOFT_CAP ? 1u : gs_activeBlockCount;
        RWStructuredBuffer<uint> expandedHistogram = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_HISTOGRAM_DESCRIPTOR_INDEX];
        InterlockedAdd(expandedHistogram[bucketID], emittedCount);
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

    if (GI == 0u)
    {
        gs_useCluster = 0u;
        gs_activeBlockCount = 0u;
        gs_totalBlockCount = 0u;
        gs_outputBaseIndex = 0u;
        gs_committedCount = 0u;
    }

    bool hasClipmapInfo = false;
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    VsmLoadClusterScreenCoverage(packedCluster, GI, hasClipmapInfo, clipmapInfo);
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u && hasClipmapInfo)
    {
        float2 ssMin;
        float2 ssMax;
        const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
        const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
        const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
        const CLodMeshletDescriptor desc = LoadMeshletDescriptor(
            pageSlabDescriptorIndex,
            pageSlabByteOffset,
            hdr.descriptorOffset,
            CLodVisibleClusterLocalMeshletIndex(packedCluster));
        PJ_ComputeScreenBounds(gs_screenPos, CLodDescVertexCount(desc), ssMin, ssMax);

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
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gs_useCluster == 0u)
    {
        return;
    }

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
    [loop]
    for (uint blockLinearIndex = GI; blockLinearIndex < gs_totalBlockCount; blockLinearIndex += SW_RASTER_THREADS)
    {
        const uint2 blockCoord = uint2(blockLinearIndex % gs_blockCountX, blockLinearIndex / gs_blockCountX) + uint2(gs_minBlockX, gs_minBlockY);
        CLodVirtualShadowBlockMeta blockMeta;
        if (!VsmBuildBlockMeta(CLodVirtualShadowBlockOriginFromBlockCoord(blockCoord), clipmapInfo, pageTable, blockMeta))
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

    const bool overflowedBlockTracking = gs_activeBlockCount > CLOD_VSM_BLOCK_EXPAND_BLOCK_SOFT_CAP;
    if (GI == 0u)
    {
        const uint requestedCount = overflowedBlockTracking ? 1u : min(gs_activeBlockCount, kVsmBlockTrackedCapacity);
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
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gs_committedCount == 0u)
    {
        return;
    }

    RWByteAddressBuffer expandedClusters = ResourceDescriptorHeap[CLOD_VSM_BLOCK_EXPAND_EXPANDED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];

    if (overflowedBlockTracking)
    {
        if (GI == 0u)
        {
            CLodStoreVisibleClusterPackedWordsRW(expandedClusters, gs_outputBaseIndex, packedCluster);
        }
        return;
    }

    if (GI >= gs_committedCount)
    {
        return;
    }

    CLodStoreVisibleClusterPackedWordsRW(expandedClusters, gs_outputBaseIndex + GI, packedCluster);
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
