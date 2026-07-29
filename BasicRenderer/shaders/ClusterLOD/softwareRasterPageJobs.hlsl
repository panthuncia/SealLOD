#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/clodVirtualShadowDepth.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/vertex.hlsli"
#include "PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodPageJobCommon.hlsli"
#include "include/clodPageJobRasterShared.hlsli"
#include "include/visibleClusterPacking.hlsli"

struct CLodSoftwareRasterPageJobRecord
{
    uint sortedClusterIndex;
    // physical page [0..13], page X [14..20], page Y [21..27]
    uint physicalAndPageCoords;
    // wrapped X [0..6], wrapped Y [7..13], clipmap [14..18], flags [19..]
    uint wrappedCoordsLayerAndFlags;
};

static const uint kCLodSoftwareRasterPageJobDoubleSidedFlag = 1u;

struct RasterizeClustersCommand
{
    uint baseClusterOffset;
    uint xDim;
    uint rasterBucketID;
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

groupshared float2 gs_expandScreenPos[SW_RASTER_MAX_VERTS];
groupshared uint gs_expandUse;
groupshared uint gs_expandMinPageX;
groupshared uint gs_expandMinPageY;
groupshared uint gs_expandMaxPageX;
groupshared uint gs_expandMaxPageY;
groupshared uint gs_expandMinTileX;
groupshared uint gs_expandMinTileY;
groupshared uint gs_expandTileCountX;
groupshared uint gs_expandTileCountY;
groupshared uint gs_expandTileJobCount;
groupshared uint gs_expandDirtyCount;
groupshared uint gs_expandBaseIndex;
groupshared uint gs_expandCommittedCount;
groupshared uint gs_expandPackedPageCoords[PAGEJOB_MAX_PAGES_PER_TILE];
groupshared uint gs_expandPhysicalPageIndices[PAGEJOB_MAX_PAGES_PER_TILE];

groupshared float2 gs_pageRasterScreenPos[SW_RASTER_MAX_VERTS];
groupshared float gs_pageRasterLinearDepth[SW_RASTER_MAX_VERTS];
groupshared uint gs_pageRasterTriangleCount;
groupshared uint gs_pageRasterDepthRejectedTriangleCount;
groupshared uint gs_pageRasterBackfaceRejectedTriangleCount;
groupshared uint gs_pageRasterCoveredPixelCount;
groupshared uint gs_pageRasterAnyWrite;
groupshared uint gs_pageRasterClusterBoundsOverlap;
groupshared uint gs_pageRasterBboxRejectedTriangleCount;

[shader("compute")]
[numthreads(SW_RASTER_THREADS, 1, 1)]
void SWPageJobExpandCSMain(uint3 dtid : SV_DispatchThreadID, uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketID = IndirectCommandSignatureRootConstant2;
    const uint clusterCount = histogram[bucketID];

    const uint linearizedGroupID = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    if (linearizedGroupID >= clusterCount) {
        return;
    }

    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const uint sortedClusterIndex = IndirectCommandSignatureRootConstant0 + linearizedGroupID;
    ByteAddressBuffer compactedVisibleClusters = ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> compactedVisibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(compactedVisibleClusters, sortedClusterIndex);
    const uint assemblyTransformIndex = compactedVisibleClusterTransformIndices[sortedClusterIndex];

    const uint viewID = CLodVisibleClusterViewID(packedCluster);
    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const uint shadowClipmapIndex = CLodVisibleClusterShadowClipmapIndex(packedCluster);

    PerMeshInstanceBuffer meshInst = LoadMeshTemplateForDraw(instanceID);
    // Wind palettes are draw-transient; the persistent mesh template retains its bind-pose source slot.
    meshInst.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(instanceID, meshInst.skinningInstanceSlot);
    PerObjectBuffer objData = LoadInstanceTransformForDrawWithAssemblyTransform(instanceID, assemblyTransformIndex);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceID);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    const bool hasClipmapInfo =
        shadowClipmapIndex < kCLodVirtualShadowClipmapCount &&
        CLodVirtualShadowClipmapIsValid(clipmapInfos[shadowClipmapIndex]) &&
        clipmapInfos[shadowClipmapIndex].shadowCameraBufferIndex == viewID;
    if (hasClipmapInfo) {
        clipmapInfo = clipmapInfos[shadowClipmapIndex];
    }

    CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDescriptorIndex,
        pageSlabByteOffset,
        hdr.descriptorOffset,
        localMeshletIndex);
    const uint vertCount = CLodDescVertexCount(desc);

    StructuredBuffer<CullingCameraInfo> cullingCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    CullingCameraInfo cam = cullingCameras[viewID];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuf =
        ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    ClodViewRasterInfo rasterInfo = viewRasterInfoBuf[viewID];

    const float visWidth = float(max(clipmapInfo.virtualResolution, 1u));
    const float visHeight = visWidth;
    const float scissorMinXf = 0.0f;
    const float scissorMinYf = 0.0f;
    const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
    row_major matrix modelViewProjection = mul(objData.model, cam.viewProjection);

    if (GI == 0) {
        gs_expandUse = 0u;
        gs_expandDirtyCount = 0u;
        gs_expandBaseIndex = 0u;
        gs_expandCommittedCount = 0u;
        gs_expandTileJobCount = 0u;
    }

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

        float4 clipPos = mul(float4(localPos, 1.0f), modelViewProjection);
        float invW = 1.0f / clipPos.w;
        float2 ndc = clipPos.xy * invW;
        float2 screen;
        screen.x = (ndc.x + 1.0f) * 0.5f * visWidth + scissorMinXf;
        screen.y = (1.0f - ndc.y) * 0.5f * visHeight + scissorMinYf;
        gs_expandScreenPos[v] = screen;
    }

    GroupMemoryBarrierWithGroupSync();

    if (GI == 0)
    {
        if (hasClipmapInfo)
        {
            float2 ssMin;
            float2 ssMax;
            PJ_ComputeScreenBounds(gs_expandScreenPos, vertCount, ssMin, ssMax);

            uint2 minPageCoord;
            uint2 maxPageCoord;
            uint2 minTileCoord;
            uint2 tileCount;
            uint tileJobCount;
            PJ_ComputeTileCoverage(
                ssMin,
                ssMax,
                clipmapInfo,
                minPageCoord,
                maxPageCoord,
                minTileCoord,
                tileCount,
                tileJobCount);

            gs_expandMinPageX = minPageCoord.x;
            gs_expandMinPageY = minPageCoord.y;
            gs_expandMaxPageX = maxPageCoord.x;
            gs_expandMaxPageY = maxPageCoord.y;
            gs_expandMinTileX = minTileCoord.x;
            gs_expandMinTileY = minTileCoord.y;
            gs_expandTileCountX = tileCount.x;
            gs_expandTileCountY = tileCount.y;
            gs_expandTileJobCount = tileJobCount;
            gs_expandUse = tileJobCount != 0u ? 1u : 0u;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (gs_expandUse == 0u)
    {
        return;
    }

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> pageJobCount = ResourceDescriptorHeap[CLOD_RASTER_PAGE_JOB_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> pageJobClusterTags = ResourceDescriptorHeap[CLOD_RASTER_PAGE_JOB_CLUSTER_TAGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodSoftwareRasterPageJobRecord> pageJobRecords = ResourceDescriptorHeap[CLOD_RASTER_PAGE_JOB_RECORDS_DESCRIPTOR_INDEX];
    const uint recordCapacity = CLOD_RASTER_PAGE_JOB_RECORD_CAPACITY;
    const uint2 minPageCoord = uint2(gs_expandMinPageX, gs_expandMinPageY);
    const uint2 maxPageCoord = uint2(gs_expandMaxPageX, gs_expandMaxPageY);
    const uint2 minTileCoord = uint2(gs_expandMinTileX, gs_expandMinTileY);
    const uint2 tileCount = uint2(gs_expandTileCountX, gs_expandTileCountY);
    const uint tileJobCount = gs_expandTileJobCount;
    const uint physicalAtlasPagesWide = max(clipmapInfo.physicalAtlasPagesWide, 1u);

    for (uint tileJobIndex = 0u; tileJobIndex < tileJobCount; ++tileJobIndex)
    {
        if (GI == 0u)
        {
            gs_expandDirtyCount = 0u;
            gs_expandBaseIndex = 0u;
            gs_expandCommittedCount = 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        uint2 tileMinPageCoord;
        uint2 scanMinPageCoord;
        uint2 tileMaxPageCoord;
        PJ_ComputeTilePageBounds(
            tileJobIndex,
            minPageCoord,
            maxPageCoord,
            minTileCoord,
            tileCount,
            tileMinPageCoord,
            scanMinPageCoord,
            tileMaxPageCoord);

        const uint pageRangeWidth = tileMaxPageCoord.x - scanMinPageCoord.x + 1u;
        const uint tilePageCount = pageRangeWidth * (tileMaxPageCoord.y - scanMinPageCoord.y + 1u);

        if (GI < tilePageCount)
        {
            const uint pageX = scanMinPageCoord.x + (GI % pageRangeWidth);
            const uint pageY = scanMinPageCoord.y + (GI / pageRangeWidth);
            const uint2 wrappedCoords = CLodVirtualShadowWrappedPageCoords(uint2(pageX, pageY), clipmapInfo);
            const uint pageEntry = pageTable[uint3(wrappedCoords, clipmapInfo.pageTableLayer)];
#if defined(PSO_SKINNED)
            const bool pageRasterable =
                CLodVirtualShadowPageEntryIsDynamicActive(pageEntry);
#else
            const bool pageRasterable =
                CLodVirtualShadowPageEntryCanRaster(pageEntry);
#endif
            if (pageRasterable)
            {
                uint slot = 0u;
                InterlockedAdd(gs_expandDirtyCount, 1u, slot);
                gs_expandPackedPageCoords[slot] = (pageX & 0xFFFFu) | ((pageY & 0xFFFFu) << 16u);
                gs_expandPhysicalPageIndices[slot] = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
            }
        }

        GroupMemoryBarrierWithGroupSync();

        const uint dirtyCount = gs_expandDirtyCount;
        if (GI == 0u && dirtyCount != 0u)
        {
            InterlockedAdd(pageJobCount[0], dirtyCount, gs_expandBaseIndex);
            gs_expandCommittedCount =
                (gs_expandBaseIndex < recordCapacity)
                    ? min(dirtyCount, recordCapacity - gs_expandBaseIndex)
                    : 0u;
            RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
                ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_STATS_DESCRIPTOR_INDEX];
            InterlockedAdd(statsBuffer[0].pageJobRequestedRecordCount, dirtyCount);
            InterlockedAdd(statsBuffer[0].pageJobCommittedRecordCount, gs_expandCommittedCount);
            InterlockedAdd(
                statsBuffer[0].pageJobDroppedRecordCount,
                dirtyCount - gs_expandCommittedCount);
#if defined(PSO_DOUBLE_SIDED)
            InterlockedAdd(
                statsBuffer[0].pageJobDoubleSidedRecordCount,
                gs_expandCommittedCount);
#endif

            if (gs_expandCommittedCount > 0u)
            {
                pageJobClusterTags[sortedClusterIndex] = perFrameBuffer.frameIndex;
            }

            if (gs_expandBaseIndex + dirtyCount > recordCapacity)
            {
                InterlockedMin(pageJobCount[0], recordCapacity);
            }
        }

        GroupMemoryBarrierWithGroupSync();

        const uint committedCount = gs_expandCommittedCount;
        if (GI >= committedCount)
        {
            continue;
        }

        const uint packedPageCoords = gs_expandPackedPageCoords[GI];
        const uint pageX = packedPageCoords & 0xFFFFu;
        const uint pageY = (packedPageCoords >> 16u) & 0xFFFFu;
        const uint physicalPageIndex = gs_expandPhysicalPageIndices[GI];

        CLodSoftwareRasterPageJobRecord rec = (CLodSoftwareRasterPageJobRecord)0;
        rec.sortedClusterIndex = sortedClusterIndex;
        rec.physicalAndPageCoords =
            (physicalPageIndex & 0x3fffu) |
            ((pageX & 0x7fu) << 14u) |
            ((pageY & 0x7fu) << 21u);
        const uint2 wrappedCoords = CLodVirtualShadowWrappedPageCoords(uint2(pageX, pageY), clipmapInfo);
        rec.wrappedCoordsLayerAndFlags =
            (wrappedCoords.x & 0x7fu) |
            ((wrappedCoords.y & 0x7fu) << 7u) |
            ((clipmapInfo.pageTableLayer & 0x1fu) << 14u);
#if defined(PSO_DOUBLE_SIDED)
        rec.wrappedCoordsLayerAndFlags |=
            kCLodSoftwareRasterPageJobDoubleSidedFlag << 19u;
#endif
        pageJobRecords[gs_expandBaseIndex + GI] = rec;
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void SWPageJobBuildIndirectArgsCSMain(uint3 dtid : SV_DispatchThreadID)
{
    StructuredBuffer<uint> pageJobCount = ResourceDescriptorHeap[CLOD_RASTER_PAGE_JOB_COUNT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<RasterizeClustersCommand> outArgs = ResourceDescriptorHeap[CLOD_RASTER_PAGE_JOB_INDIRECT_ARGS_DESCRIPTOR_INDEX];

    RasterizeClustersCommand cmd = (RasterizeClustersCommand)0;
    const uint count = pageJobCount[0];
    if (count > 0u)
    {
        const uint kMaxDim = 65535u;
        uint dispatchX = (uint)ceil(sqrt((float)count));
        dispatchX = min(dispatchX, kMaxDim);
        uint dispatchY = (count + dispatchX - 1u) / dispatchX;
        dispatchY = min(dispatchY, kMaxDim);

        cmd.baseClusterOffset = 0u;
        cmd.xDim = dispatchX;
        cmd.rasterBucketID = 0u;
        cmd.dispatchX = dispatchX;
        cmd.dispatchY = dispatchY;
        cmd.dispatchZ = 1u;
    }
    outArgs[0] = cmd;
}

[shader("compute")]
[numthreads(SW_RASTER_THREADS, 1, 1)]
void SWPageJobRasterPageCSMain(uint3 dtid : SV_DispatchThreadID, uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    const uint linearizedGroupID = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    const uint pageJobIndex = IndirectCommandSignatureRootConstant0 + linearizedGroupID;

    StructuredBuffer<uint> pageJobCount = ResourceDescriptorHeap[CLOD_RASTER_PAGE_JOB_COUNT_DESCRIPTOR_INDEX];
    if (pageJobIndex >= pageJobCount[0])
    {
        return;
    }

    StructuredBuffer<CLodSoftwareRasterPageJobRecord> pageJobRecords = ResourceDescriptorHeap[CLOD_RASTER_PAGE_JOB_RECORDS_DESCRIPTOR_INDEX];
    const CLodSoftwareRasterPageJobRecord rec = pageJobRecords[pageJobIndex];
    const uint sortedClusterIndex = rec.sortedClusterIndex;
    const uint physicalPageIndex = rec.physicalAndPageCoords & 0x3fffu;
    const uint pageX = (rec.physicalAndPageCoords >> 14u) & 0x7fu;
    const uint pageY = (rec.physicalAndPageCoords >> 21u) & 0x7fu;
    const uint wrappedPageX = rec.wrappedCoordsLayerAndFlags & 0x7fu;
    const uint wrappedPageY = (rec.wrappedCoordsLayerAndFlags >> 7u) & 0x7fu;
    const uint clipmapLayer = (rec.wrappedCoordsLayerAndFlags >> 14u) & 0x1fu;
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapLayer];
    const uint pagePixelMinX = pageX * kCLodVirtualShadowPhysicalPageSize;
    const uint pagePixelMinY = pageY * kCLodVirtualShadowPhysicalPageSize;
    const uint atlasBaseX =
        (physicalPageIndex % physicalAtlasPagesWide) * kCLodVirtualShadowPhysicalPageSize;
    const uint atlasBaseY =
        (physicalPageIndex / physicalAtlasPagesWide) * kCLodVirtualShadowPhysicalPageSize;
#if defined(PSO_SKINNED)
    const bool dynamicLayer = true;
#else
    const bool dynamicLayer = false;
#endif
    const bool doubleSided =
        ((rec.wrappedCoordsLayerAndFlags >> 19u) &
            kCLodSoftwareRasterPageJobDoubleSidedFlag) != 0u;
    const bool telemetryEnabled =
        CLOD_RASTER_VIRTUAL_SHADOW_TELEMETRY_ENABLED != 0u;
    ByteAddressBuffer compactedVisibleClusters = ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> compactedVisibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(compactedVisibleClusters, sortedClusterIndex);
    const uint assemblyTransformIndex = compactedVisibleClusterTransformIndices[sortedClusterIndex];

    const uint viewID = CLodVisibleClusterViewID(packedCluster);
    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);

    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDescriptorIndex,
        pageSlabByteOffset,
        hdr.descriptorOffset,
        localMeshletIndex);
    const uint vertCount = CLodDescVertexCount(desc);
    const uint triCount = CLodDescTriangleCount(desc);
    const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
    const uint triangleStreamBase = pageSlabByteOffset + hdr.triangleStreamOffset;
    const uint positionBitOffset = desc.positionBitOffset;
    const uint triangleByteOffset = desc.triangleByteOffset;
    const uint bitsX = CLodDescBitsX(desc);
    const uint bitsY = CLodDescBitsY(desc);
    const uint bitsZ = CLodDescBitsZ(desc);
    const uint positionQuantExp = hdr.compressedPositionQuantExp;
    const uint attributeMask = hdr.attributeMask;
    const uint vertexAttributeOffset = desc.vertexAttributeOffset;
    const uint jointArrayOffset = hdr.jointArrayOffset;
    const uint weightArrayOffset = hdr.weightArrayOffset;
    const int3 minQ = int3(desc.minQx, desc.minQy, desc.minQz);

    PerMeshInstanceBuffer meshInst = LoadMeshTemplateForDraw(instanceID);
    meshInst.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(instanceID, meshInst.skinningInstanceSlot);
    const uint perMeshBufferIndex = meshInst.perMeshBufferIndex;
    const uint skinningInstanceSlot = meshInst.skinningInstanceSlot;
    StructuredBuffer<CullingCameraInfo> cullingCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuf =
        ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    row_major matrix modelViewProjection;
    float4 modelViewZ;
    bool reverseWinding = false;
    {
        const PerObjectBuffer objData = LoadInstanceTransformForDrawWithAssemblyTransform(instanceID, assemblyTransformIndex);
        const CullingCameraInfo cam = cullingCameras[viewID];
        reverseWinding = (objData.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u;
        modelViewProjection = mul(objData.model, cam.viewProjection);
        modelViewZ = mul(objData.model, cam.viewZ);
    }

    const ClodViewRasterInfo rasterInfo = viewRasterInfoBuf[viewID];
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceID);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    const float visWidth = float(max(clipmapInfo.virtualResolution, 1u));
    const float visHeight = visWidth;
    const float screenScaleX = 0.5f * visWidth;
    const float screenScaleY = -0.5f * visHeight;
    const float screenBiasX = screenScaleX;
    const float screenBiasY = -screenScaleY;

    for (uint v = GI; v < vertCount; v += SW_RASTER_THREADS)
    {
        float3 localPos = PJ_DecodeCompressedPosition(
            v,
            positionBitstreamBase,
            positionBitOffset,
            bitsX, bitsY, bitsZ,
            positionQuantExp,
            minQ,
            pageSlabDescriptorIndex);
#if defined(PSO_SKINNED)
        SkinningInfluences skinning = PJ_DecodePackedJointsScalar(
            v,
            attributeMask,
            vertexAttributeOffset,
            jointArrayOffset,
            pageSlabByteOffset,
            pageSlabDescriptorIndex);
        skinning = PJ_DecodePackedWeightsScalar(
            v,
            attributeMask,
            vertexAttributeOffset,
            weightArrayOffset,
            pageSlabByteOffset,
            pageSlabDescriptorIndex,
            skinning);
        skinning = ResolveAssemblySkinningInfluences(skinning, metadata, assemblyTransformIndex);
        localPos = mul(float4(localPos, 1.0f), BuildAssemblyLocalSkinMatrix(
            skinningInstanceSlot, skinning, assemblyTransformIndex)).xyz;
#endif

        const float4 clipPos = mul(float4(localPos, 1.0f), modelViewProjection);
        const float invW = rcp(clipPos.w);
        const float viewZ = dot(float4(localPos, 1.0f), modelViewZ);
        gs_pageRasterScreenPos[v] = float2(
            clipPos.x * invW * screenScaleX + screenBiasX,
            clipPos.y * invW * screenScaleY + screenBiasY);
        gs_pageRasterLinearDepth[v] = -viewZ;
    }

    GroupMemoryBarrierWithGroupSync();

    RWTexture2D<uint> physicalPages = ResourceDescriptorHeap[
#if defined(PSO_SKINNED)
        CLOD_RASTER_VIRTUAL_SHADOW_DYNAMIC_PAGES_DESCRIPTOR_INDEX
#else
        CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX
#endif
    ];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
    ByteAddressBuffer slab = ResourceDescriptorHeap[pageSlabDescriptorIndex];
    const uint pagePixelMaxX = pagePixelMinX + kCLodVirtualShadowPhysicalPageSize - 1u;
    const uint pagePixelMaxY = pagePixelMinY + kCLodVirtualShadowPhysicalPageSize - 1u;

    if (GI == 0u)
    {
        gs_pageRasterTriangleCount = 0u;
        gs_pageRasterDepthRejectedTriangleCount = 0u;
        gs_pageRasterBackfaceRejectedTriangleCount = 0u;
        gs_pageRasterCoveredPixelCount = 0u;
        gs_pageRasterAnyWrite = 0u;
        gs_pageRasterBboxRejectedTriangleCount = 0u;
        float2 clusterMin;
        float2 clusterMax;
        PJ_ComputeScreenBounds(gs_pageRasterScreenPos, vertCount, clusterMin, clusterMax);
        gs_pageRasterClusterBoundsOverlap =
            clusterMax.x >= float(pagePixelMinX) &&
            clusterMax.y >= float(pagePixelMinY) &&
            clusterMin.x <= float(pagePixelMaxX) &&
            clusterMin.y <= float(pagePixelMaxY)
                ? 1u
                : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint localTriangleCount = 0u;
    uint localDepthRejectedTriangleCount = 0u;
    uint localBackfaceRejectedTriangleCount = 0u;
    uint localCoveredPixelCount = 0u;
    uint localBboxRejectedTriangleCount = 0u;
    bool localAnyPixelWritten = false;

    for (uint t = GI; t < triCount; t += SW_RASTER_THREADS)
    {
        if (telemetryEnabled) localTriangleCount++;
        uint3 tri = PJ_DecodeTriangle(slab, triangleStreamBase, triangleByteOffset, t);
        if (any(tri >= vertCount))
        {
            if (telemetryEnabled) localBackfaceRejectedTriangleCount++;
            continue;
        }
        if (reverseWinding) { uint tmp = tri.y; tri.y = tri.z; tri.z = tmp; }

        float2 s0 = gs_pageRasterScreenPos[tri.x];
        float2 s1 = gs_pageRasterScreenPos[tri.y];
        float2 s2 = gs_pageRasterScreenPos[tri.z];
        float depth0 = gs_pageRasterLinearDepth[tri.x];
        float depth1 = gs_pageRasterLinearDepth[tri.y];
        float depth2 = gs_pageRasterLinearDepth[tri.z];
        if (depth0 <= 0.0f || depth1 <= 0.0f || depth2 <= 0.0f)
        {
            if (telemetryEnabled) localDepthRejectedTriangleCount++;
            continue;
        }

        float twiceArea =
            (s1.x - s0.x) * (s2.y - s0.y) -
            (s1.y - s0.y) * (s2.x - s0.x);
        if (abs(twiceArea) <= 1.0e-8f)
        {
            continue;
        }
        if (doubleSided)
        {
            if (twiceArea > 0.0f)
            {
                float2 tmpPos = s1;
                s1 = s2;
                s2 = tmpPos;
                float tmpDepth = depth1;
                depth1 = depth2;
                depth2 = tmpDepth;
                twiceArea = -twiceArea;
            }
        }
        else if (twiceArea >= 0.0f)
        {
            if (telemetryEnabled) localBackfaceRejectedTriangleCount++;
            continue;
        }
        const float invTwiceArea = -rcp(twiceArea);
        const float2 bbMinF = min(min(s0, s1), s2);
        const float2 bbMaxF = max(max(s0, s1), s2);
        int2 minPx = max(int2(floor(bbMinF)), int2(pagePixelMinX, pagePixelMinY));
        int2 maxPx = min(int2(floor(bbMaxF)), int2(pagePixelMaxX, pagePixelMaxY));
        if (minPx.x > maxPx.x || minPx.y > maxPx.y)
        {
            if (telemetryEnabled) localBboxRejectedTriangleCount++;
            continue;
        }
        const float2 origin = float2(float(minPx.x) + 0.5f, float(minPx.y) + 0.5f);
        const float2 e12 = s2 - s1;
        const float2 e20 = s0 - s2;
        const float dx_b0 = e12.y * invTwiceArea;
        const float dx_b1 = e20.y * invTwiceArea;
        float scanline_b0 =
            ((origin.x - s1.x) * e12.y - (origin.y - s1.y) * e12.x) *
            invTwiceArea;
        float scanline_b1 =
            ((origin.x - s2.x) * e20.y - (origin.y - s2.y) * e20.x) *
            invTwiceArea;

        for (int py = minPx.y; py <= maxPx.y; ++py)
        {
            float b0 = scanline_b0;
            float b1 = scanline_b1;
            for (int px = minPx.x; px <= maxPx.x; ++px)
            {
                const float b2 = 1.0f - b0 - b1;
                if (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f)
                {
                    const float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                    const float pageSpaceDepth = dynamicLayer
                        ? CLodVirtualShadowDepthToCachedPageSpace(
                            depth,
                            physicalPageIndex,
                            clipmapInfo.shadowCameraBufferIndex)
                        : depth;
                    InterlockedMin(
                        physicalPages[uint2(
                            atlasBaseX + uint(px - int(pagePixelMinX)),
                            atlasBaseY + uint(py - int(pagePixelMinY)))],
                        asuint(pageSpaceDepth));
                    localAnyPixelWritten = true;
                    if (telemetryEnabled) localCoveredPixelCount++;
                }
                b0 += dx_b0;
                b1 += dx_b1;
            }
            scanline_b0 -= e12.x * invTwiceArea;
            scanline_b1 -= e20.x * invTwiceArea;
        }
    }

    if (telemetryEnabled && localTriangleCount != 0u)
        InterlockedAdd(gs_pageRasterTriangleCount, localTriangleCount);
    if (telemetryEnabled && localDepthRejectedTriangleCount != 0u)
        InterlockedAdd(gs_pageRasterDepthRejectedTriangleCount, localDepthRejectedTriangleCount);
    if (telemetryEnabled && localBackfaceRejectedTriangleCount != 0u)
        InterlockedAdd(gs_pageRasterBackfaceRejectedTriangleCount, localBackfaceRejectedTriangleCount);
    if (telemetryEnabled && localCoveredPixelCount != 0u)
    {
        InterlockedAdd(gs_pageRasterCoveredPixelCount, localCoveredPixelCount);
    }
    if (localAnyPixelWritten)
        InterlockedOr(gs_pageRasterAnyWrite, 1u);
    if (telemetryEnabled && localBboxRejectedTriangleCount != 0u)
        InterlockedAdd(gs_pageRasterBboxRejectedTriangleCount, localBboxRejectedTriangleCount);
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u)
    {
        RWStructuredBuffer<CLodVirtualShadowStats> statsBuffer =
            ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_STATS_DESCRIPTOR_INDEX];
        if (telemetryEnabled)
        {
            InterlockedAdd(statsBuffer[0].pageJobRasterJobCount, 1u);
            InterlockedAdd(statsBuffer[0].pageJobRasterTriangleCount, gs_pageRasterTriangleCount);
            InterlockedAdd(
                statsBuffer[0].pageJobRasterDepthRejectedTriangleCount,
                gs_pageRasterDepthRejectedTriangleCount);
            InterlockedAdd(
                statsBuffer[0].pageJobRasterBackfaceRejectedTriangleCount,
                gs_pageRasterBackfaceRejectedTriangleCount);
            InterlockedAdd(
                statsBuffer[0].pageJobRasterCoveredPixelCount,
                gs_pageRasterCoveredPixelCount);
            InterlockedAdd(
                statsBuffer[0].pageJobRasterClusterBoundsOverlapCount,
                gs_pageRasterClusterBoundsOverlap);
            InterlockedAdd(
                statsBuffer[0].pageJobRasterBboxRejectedTriangleCount,
                gs_pageRasterBboxRejectedTriangleCount);
        }
        if (gs_pageRasterAnyWrite != 0u)
        {
#if !defined(PSO_SKINNED)
            uint ignored = 0u;
            InterlockedOr(
                pageTable[uint3(uint2(wrappedPageX, wrappedPageY), clipmapLayer)],
                kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask,
                ignored);
#endif
            if (telemetryEnabled)
                InterlockedAdd(statsBuffer[0].pageJobRasterPageWriteCount, 1u);
        }
    }
}
