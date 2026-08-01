// Page-job VSM rasterization: three-node work-graph pipeline.
// Build -> Expand -> RasterPage.
//
// Extracted into its own compilation unit so that groupshared memory is
// NOT charged with the 16+ KB batch accumulators from workGraphCulling.hlsl.

#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/clodVirtualShadowDepth.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/vertex.hlsli"
#include "include/materialFlags.hlsli"
#include "PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodPageJobCommon.hlsli"
#include "include/clodPageJobRasterShared.hlsli"
#include "include/visibleClusterPacking.hlsli"

// Telemetry

static const uint PJ_COUNTER_BUILD_CLUSTERS_PROCESSED = 68;
static const uint PJ_COUNTER_BUILD_TILES_EMITTED      = 69;
static const uint PJ_COUNTER_EXPAND_PAGES_EMITTED     = 70;
static const uint PJ_COUNTER_RASTER_PIXELS_WRITTEN    = 72;
static const uint PJ_COUNTER_RASTER_FLAG_WRITES       = 73;
static const uint PJ_COUNTER_EXPAND_JOBS_LAUNCHED      = 89;

void PJTelemetryAdd(uint counterIndex, uint value)
{
    if ((CLOD_WG_FLAGS & CLOD_WG_FLAG_TELEMETRY_ENABLED) == 0u) return;
    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[CLOD_WG_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryCounters[counterIndex], value);
}

// Node 1: PageJobBuild: emits tile-level expand records
//
// Groupshared for Build only: screen positions + tile state scalars.
// Total: 128 * 8 bytes + ~40 bytes = ~1064 bytes.

groupshared float2  gs_pjBuildScreenPos[SW_RASTER_MAX_VERTS];
groupshared uint    gs_pjBuildMinPageX;
groupshared uint    gs_pjBuildMinPageY;
groupshared uint    gs_pjBuildMaxPageX;
groupshared uint    gs_pjBuildMaxPageY;
groupshared uint    gs_pjBuildMinTileX;
groupshared uint    gs_pjBuildMinTileY;
groupshared uint    gs_pjBuildTileCountX;
groupshared uint    gs_pjBuildTileCountY;
groupshared uint    gs_pjBuildTileJobCount;

[Shader("node")]
[NodeID("PageJobBuild")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(PAGEJOB_BUILD_MAX_CLUSTERS, 1, 1)]
[NumThreads(PAGEJOB_BUILD_THREADS, 1, 1)]
void WG_PageJobBuild(
    DispatchNodeInputRecord<PageJobBuildBatchRecord> inputRecord,
    [NodeID("PageJobExpand")] [MaxRecords(PAGEJOB_MAX_TILE_JOBS_PER_CLUSTER)] NodeOutput<PageJobExpandRecord> expandOutput,
    uint GI : SV_GroupIndex,
    uint3 groupId : SV_GroupID)
{
    PageJobBuildBatchRecord batch = inputRecord.Get();
    const uint clusterSlot = groupId.x;
    if (clusterSlot >= batch.numClusters) return;

    const uint unsortedClusterIndex = batch.clusterIndices[clusterSlot];
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, unsortedClusterIndex);
    RWStructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint assemblyTransformIndex = visibleClusterTransformIndices[unsortedClusterIndex];

    const uint viewID = CLodVisibleClusterViewID(packedCluster);
    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const uint shadowClipmapIndex = CLodVisibleClusterShadowClipmapIndex(packedCluster);

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodClipmapInfo)];
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    const bool hasClipmapInfo =
        shadowClipmapIndex < kCLodVirtualShadowClipmapCount &&
        CLodVirtualShadowClipmapIsValid(clipmapInfos[shadowClipmapIndex]) &&
        clipmapInfos[shadowClipmapIndex].shadowCameraBufferIndex == viewID;
    if (hasClipmapInfo) {
        clipmapInfo = clipmapInfos[shadowClipmapIndex];
    }
    if (!hasClipmapInfo) {
        GroupNodeOutputRecords<PageJobExpandRecord> emptyOut = expandOutput.GetGroupNodeOutputRecords(0);
        emptyOut.OutputComplete();
        return;
    }

    CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDescriptorIndex, pageSlabByteOffset,
        hdr.descriptorOffset, localMeshletIndex);
    const uint vertCount = CLodDescVertexCount(desc);

    PerMeshInstanceBuffer meshInst = LoadMeshTemplateForDraw(instanceID);
    // Wind palettes are draw-transient; the persistent mesh template retains its bind-pose source slot.
    meshInst.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(instanceID, meshInst.skinningInstanceSlot);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    PerObjectBuffer objData = LoadInstanceTransformForDrawWithAssemblyTransform(instanceID, assemblyTransformIndex);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceID);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuf =
        ResourceDescriptorHeap[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    ClodViewRasterInfo rasterInfo = viewRasterInfoBuf[viewID];

    const float visWidth  = float(max(clipmapInfo.virtualResolution, 1u));
    const float visHeight = visWidth;
    const float scissorMinXf = 0.0f;
    const float scissorMinYf = 0.0f;

    StructuredBuffer<CullingCameraInfo> cullingCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    CullingCameraInfo cam = cullingCameras[viewID];

    const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
    row_major matrix modelViewProjection = mul(objData.model, cam.viewProjection);

    // Cooperative vertex position decode → gs_pjBuildScreenPos.
    for (uint v = GI; v < vertCount; v += PAGEJOB_BUILD_THREADS) {
        float3 localPos = PJ_DecodeCompressedPosition(
            v, positionBitstreamBase, desc.positionBitOffset,
            CLodDescBitsX(desc), CLodDescBitsY(desc), CLodDescBitsZ(desc),
            hdr.compressedPositionQuantExp,
            int3(desc.minQx, desc.minQy, desc.minQz),
            pageSlabDescriptorIndex);
        if ((perMeshBuffer[meshInst.perMeshBufferIndex].vertexFlags & VERTEX_SKINNED) != 0u) {
            SkinningInfluences skinning = PJ_DecodePackedJoints(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex);
            skinning = PJ_DecodePackedWeights(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex, skinning);
            skinning = ResolveAssemblySkinningInfluences(skinning, metadata, assemblyTransformIndex);
            localPos = ApplyAssemblySkinningToPosition(
                meshInst.skinningInstanceSlot, skinning, assemblyTransformIndex, localPos);
        }
        float4 clipPos = mul(float4(localPos, 1.0f), modelViewProjection);
        float invW = 1.0f / clipPos.w;
        float2 ndc = clipPos.xy * invW;
        float2 screen;
        screen.x = (ndc.x + 1.0f) * 0.5f * visWidth  + scissorMinXf;
        screen.y = (1.0f - ndc.y) * 0.5f * visHeight + scissorMinYf;
        gs_pjBuildScreenPos[v] = screen;
    }
    GroupMemoryBarrierWithGroupSync();

    // Thread 0: screen-space AABB → page footprint → tile job count.
    if (GI == 0) {
        float2 ssMin;
        float2 ssMax;
        PJ_ComputeScreenBounds(gs_pjBuildScreenPos, vertCount, ssMin, ssMax);

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

        gs_pjBuildMinPageX  = minPageCoord.x;
        gs_pjBuildMinPageY  = minPageCoord.y;
        gs_pjBuildMaxPageX  = maxPageCoord.x;
        gs_pjBuildMaxPageY  = maxPageCoord.y;
        gs_pjBuildMinTileX  = minTileCoord.x;
        gs_pjBuildMinTileY  = minTileCoord.y;
        gs_pjBuildTileCountX = tileCount.x;
        gs_pjBuildTileCountY = tileCount.y;
        gs_pjBuildTileJobCount = tileJobCount;

        PJTelemetryAdd(PJ_COUNTER_BUILD_CLUSTERS_PROCESSED, 1);
        PJTelemetryAdd(PJ_COUNTER_BUILD_TILES_EMITTED, gs_pjBuildTileJobCount);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint tileJobCount = gs_pjBuildTileJobCount;
    GroupNodeOutputRecords<PageJobExpandRecord> pjOut =
        expandOutput.GetGroupNodeOutputRecords(tileJobCount);
    if (GI == 0) {
        const uint2 minPageCoord = uint2(gs_pjBuildMinPageX, gs_pjBuildMinPageY);
        const uint2 maxPageCoord = uint2(gs_pjBuildMaxPageX, gs_pjBuildMaxPageY);
        const uint2 minTileCoord = uint2(gs_pjBuildMinTileX, gs_pjBuildMinTileY);
        const uint2 tileCount = uint2(gs_pjBuildTileCountX, gs_pjBuildTileCountY);

        for (uint tileJobIndex = 0; tileJobIndex < tileJobCount; ++tileJobIndex) {
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

            pjOut[tileJobIndex].dispatchGrid = uint3(1, 1, 1);
            pjOut[tileJobIndex].clusterIndex = unsortedClusterIndex;
            pjOut[tileJobIndex].packedTileAndClipmap =
                PackPageJobTileAndClipmap(tileMinPageCoord, clipmapInfo.pageTableLayer);
            pjOut[tileJobIndex].packedPageBounds =
                PackPageJobPageBounds(scanMinPageCoord, tileMaxPageCoord);
        }
    }
    pjOut.OutputComplete();
}

// Node 2: PageJobExpand: scans tile for dirty pages, emits per-page records
//
// Groupshared: atomic dirty-page counter + slot array.
// Total: 4 + 64*4 = 260 bytes (trivial).

groupshared uint gs_pjExpandDirtyCount;
groupshared uint gs_pjExpandSlots[PAGEJOB_MAX_PAGES_PER_TILE];

[Shader("node")]
[NodeID("PageJobExpand")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(1, 1, 1)]
[NumThreads(PAGEJOB_EXPAND_THREADS, 1, 1)]
void WG_PageJobExpand(
    DispatchNodeInputRecord<PageJobExpandRecord> inputRecord,
    [NodeID("PageJobRasterPage")] [MaxRecords(PAGEJOB_MAX_PAGES_PER_TILE)] NodeOutput<PageJobRasterPageRecord> rasterPageOutput,
    uint GI : SV_GroupIndex)
{
    PageJobExpandRecord rec = inputRecord.Get();

    const uint clusterIndex = rec.clusterIndex;
    const uint2 tileMinPageCoord = UnpackPageJobTileMinPageCoord(rec.packedTileAndClipmap);
    const uint clipmapLayer = UnpackPageJobClipmapLayer(rec.packedTileAndClipmap);
    const uint2 minPageCoord = UnpackPageJobMinPageCoord(rec.packedPageBounds);
    const uint2 maxPageCoord = UnpackPageJobMaxPageCoord(rec.packedPageBounds);

    if (GI == 0) {
        gs_pjExpandDirtyCount = 0;
        PJTelemetryAdd(PJ_COUNTER_EXPAND_JOBS_LAUNCHED, 1);
    }
    GroupMemoryBarrierWithGroupSync();

    // Each thread checks one page in the tile. Max 64 threads = 8x8 pages.
    const uint tilePageCount = (maxPageCoord.x - minPageCoord.x + 1u) * (maxPageCoord.y - minPageCoord.y + 1u);

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodClipmapInfo)];
    CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapLayer];
    const uint physicalAtlasPagesWide = max(clipmapInfo.physicalAtlasPagesWide, 1u);

    RWTexture2DArray<uint> pageTableUAV =
        ResourceDescriptorHeap[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX];
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster =
        CLodLoadVisibleClusterPackedGloballyCoherent(
            visibleClusters, clusterIndex);
    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
    const PerMeshInstanceBuffer meshInst =
        LoadMeshTemplateForDraw(instanceID);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[
            ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const bool dynamicLayer =
        (perMeshBuffer[meshInst.perMeshBufferIndex].vertexFlags &
            VERTEX_SKINNED) != 0u;

    const uint pageRangeWidth = maxPageCoord.x - minPageCoord.x + 1u;

    // Each thread takes one page index.
    uint mySlot = 0xFFFFFFFFu;
    uint myPhysicalPageIndex = 0;
    uint myPageX = 0;
    uint myPageY = 0;
    uint2 myWrappedCoords = uint2(0, 0);

    if (GI < tilePageCount) {
        myPageX = minPageCoord.x + (GI % pageRangeWidth);
        myPageY = minPageCoord.y + (GI / pageRangeWidth);
        myWrappedCoords = CLodVirtualShadowWrappedPageCoords(uint2(myPageX, myPageY), clipmapInfo);
        const uint pageEntry = pageTableUAV[uint3(myWrappedCoords, clipmapInfo.pageTableLayer)];

        if (CLodVirtualShadowPageEntryCanRasterLayer(
                pageEntry, dynamicLayer)) {
            myPhysicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
            InterlockedAdd(gs_pjExpandDirtyCount, 1u, mySlot);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint dirtyCount = gs_pjExpandDirtyCount;
    PJTelemetryAdd(PJ_COUNTER_EXPAND_PAGES_EMITTED, dirtyCount);

    GroupNodeOutputRecords<PageJobRasterPageRecord> rasterOut =
        rasterPageOutput.GetGroupNodeOutputRecords(dirtyCount);

    if (mySlot != 0xFFFFFFFFu && mySlot < dirtyCount) {
        const uint pagePixelMinX = myPageX * kCLodVirtualShadowPhysicalPageSize;
        const uint pagePixelMinY = myPageY * kCLodVirtualShadowPhysicalPageSize;
        const uint atlasBaseX = (myPhysicalPageIndex % physicalAtlasPagesWide) * kCLodVirtualShadowPhysicalPageSize;
        const uint atlasBaseY = (myPhysicalPageIndex / physicalAtlasPagesWide) * kCLodVirtualShadowPhysicalPageSize;

        rasterOut[mySlot].dispatchGrid = uint3(1, 1, 1);
        rasterOut[mySlot].clusterIndex = clusterIndex;
        rasterOut[mySlot].physicalPageIndex = myPhysicalPageIndex;
        rasterOut[mySlot].packedPagePixelOriginAndClipmap = (pagePixelMinX & 0xFFFFu) | ((pagePixelMinY & 0xFFFFu) << 16u);
        rasterOut[mySlot].packedAtlasOriginAndClipmap = (atlasBaseX & 0xFFFFu) | ((atlasBaseY & 0xFFFFu) << 16u);
        rasterOut[mySlot].clipmapLayer = clipmapLayer;
        rasterOut[mySlot].wrappedPageCoords = myWrappedCoords;
    }

    rasterOut.OutputComplete();
}

// Node 3: PageJobRasterPage: rasterizes one cluster onto one physical page
//
// Groupshared: screen positions (1 KB) + linear depths (0.5 KB) = 1.5 KB.
// Only 1 barrier (vertex sync). No per-page loop. No pageHadPixels tracking.

groupshared float2 gs_pjRasterScreenPos[SW_RASTER_MAX_VERTS];
groupshared float  gs_pjRasterLinearDepth[SW_RASTER_MAX_VERTS];

[Shader("node")]
[NodeID("PageJobRasterPage")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(1, 1, 1)]
[NumThreads(PAGEJOB_RASTER_PAGE_THREADS, 1, 1)]
void WG_PageJobRasterPage(
    DispatchNodeInputRecord<PageJobRasterPageRecord> inputRecord,
    uint GI : SV_GroupIndex)
{
    PageJobRasterPageRecord rec = inputRecord.Get();

    const uint unsortedClusterIndex = rec.clusterIndex;
    const uint physicalPageIndex = rec.physicalPageIndex;
    const uint pagePixelMinX = rec.packedPagePixelOriginAndClipmap & 0xFFFFu;
    const uint pagePixelMinY = (rec.packedPagePixelOriginAndClipmap >> 16u) & 0xFFFFu;
    const uint atlasBaseX = rec.packedAtlasOriginAndClipmap & 0xFFFFu;
    const uint atlasBaseY = (rec.packedAtlasOriginAndClipmap >> 16u) & 0xFFFFu;
    const uint clipmapLayer = rec.clipmapLayer;
    const uint2 wrappedPageCoords = rec.wrappedPageCoords;
    const uint pagePixelMaxX = pagePixelMinX + kCLodVirtualShadowPhysicalPageSize - 1u;
    const uint pagePixelMaxY = pagePixelMinY + kCLodVirtualShadowPhysicalPageSize - 1u;

    // Load cluster data.
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, unsortedClusterIndex);
    RWStructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint assemblyTransformIndex = visibleClusterTransformIndices[unsortedClusterIndex];

    const uint viewID = CLodVisibleClusterViewID(packedCluster);
    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);

    CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDescriptorIndex, pageSlabByteOffset,
        hdr.descriptorOffset, localMeshletIndex);
    const uint vertCount = CLodDescVertexCount(desc);
    const uint triCount = CLodDescTriangleCount(desc);

    PerMeshInstanceBuffer meshInst = LoadMeshTemplateForDraw(instanceID);
    meshInst.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(instanceID, meshInst.skinningInstanceSlot);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<MaterialInfo> materialDataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const MaterialInfo materialInfo =
        materialDataBuffer[perMeshBuffer[meshInst.perMeshBufferIndex].materialDataIndex];
    const bool doubleSided =
        (materialInfo.materialFlags & MATERIAL_DOUBLE_SIDED) != 0u;
    PerObjectBuffer objData = LoadInstanceTransformForDrawWithAssemblyTransform(instanceID, assemblyTransformIndex);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceID);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    StructuredBuffer<CullingCameraInfo> cullingCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    CullingCameraInfo cam = cullingCameras[viewID];

    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuf =
        ResourceDescriptorHeap[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    ClodViewRasterInfo rasterInfo = viewRasterInfoBuf[viewID];

    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Shadows::CLodClipmapInfo)];
    const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapLayer];
    const float visWidth  = float(max(clipmapInfo.virtualResolution, 1u));
    const float visHeight = visWidth;
    const float scissorMinXf = 0.0f;
    const float scissorMinYf = 0.0f;

    const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
    row_major matrix modelViewProjection = mul(objData.model, cam.viewProjection);
    float4 modelViewZ = mul(objData.model, cam.viewZ);

    // Cooperative vertex decode → groupshared.
    for (uint v = GI; v < vertCount; v += PAGEJOB_RASTER_PAGE_THREADS) {
        float3 localPos = PJ_DecodeCompressedPosition(
            v, positionBitstreamBase, desc.positionBitOffset,
            CLodDescBitsX(desc), CLodDescBitsY(desc), CLodDescBitsZ(desc),
            hdr.compressedPositionQuantExp,
            int3(desc.minQx, desc.minQy, desc.minQz),
            pageSlabDescriptorIndex);
        if ((perMeshBuffer[meshInst.perMeshBufferIndex].vertexFlags & VERTEX_SKINNED) != 0u) {
            SkinningInfluences skinning = PJ_DecodePackedJoints(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex);
            skinning = PJ_DecodePackedWeights(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex, skinning);
            skinning = ResolveAssemblySkinningInfluences(skinning, metadata, assemblyTransformIndex);
            localPos = ApplyAssemblySkinningToPosition(
                meshInst.skinningInstanceSlot, skinning, assemblyTransformIndex, localPos);
        }
        float4 localPos4 = float4(localPos, 1.0f);
        float4 clipPos = mul(localPos4, modelViewProjection);
        float viewZ = dot(localPos4, modelViewZ);
        float invW = 1.0f / clipPos.w;
        float2 ndc = clipPos.xy * invW;
        float2 screen;
        screen.x = (ndc.x + 1.0f) * 0.5f * visWidth  + scissorMinXf;
        screen.y = (1.0f - ndc.y) * 0.5f * visHeight + scissorMinYf;
        gs_pjRasterScreenPos[v] = screen;
        gs_pjRasterLinearDepth[v] = -viewZ;
    }
    GroupMemoryBarrierWithGroupSync();

    // Rasterize all triangles clipped to this single page.
    const bool dynamicLayer =
        (perMeshBuffer[meshInst.perMeshBufferIndex].vertexFlags &
            VERTEX_SKINNED) != 0u;
    RWTexture2D<uint> physicalPages =
        ResourceDescriptorHeap[
            dynamicLayer
                ? CLOD_WG_VIRTUAL_SHADOW_DYNAMIC_PAGES_UAV_DESCRIPTOR_INDEX
                : CLOD_WG_VIRTUAL_SHADOW_PHYSICAL_PAGES_UAV_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTableUAV =
        ResourceDescriptorHeap[CLOD_WG_VIRTUAL_SHADOW_PAGE_TABLE_UAV_DESCRIPTOR_INDEX];

    const bool reverseWinding = (objData.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u;
    ByteAddressBuffer slab = ResourceDescriptorHeap[pageSlabDescriptorIndex];

    bool anyPixelWritten = false;

    for (uint t = GI; t < triCount; t += PAGEJOB_RASTER_PAGE_THREADS) {
        uint3 tri = PJ_DecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, desc.triangleByteOffset, t);
        if (reverseWinding) { uint tmp = tri.y; tri.y = tri.z; tri.z = tmp; }

        float2 s0 = gs_pjRasterScreenPos[tri.x];
        float2 s1 = gs_pjRasterScreenPos[tri.y];
        float2 s2 = gs_pjRasterScreenPos[tri.z];

        float depth0 = gs_pjRasterLinearDepth[tri.x];
        float depth1 = gs_pjRasterLinearDepth[tri.y];
        float depth2 = gs_pjRasterLinearDepth[tri.z];

        if (depth0 <= 0.0f || depth1 <= 0.0f || depth2 <= 0.0f) continue;

        float2 e01 = s1 - s0;
        float2 e02 = s2 - s0;
        float twiceArea = e01.x * e02.y - e01.y * e02.x;
        if (doubleSided)
        {
            if (abs(twiceArea) <= 1.0e-8f) continue;
            if (twiceArea > 0.0f)
            {
                float2 tmpPos = s1;
                s1 = s2;
                s2 = tmpPos;
                float tmpDepth = depth1;
                depth1 = depth2;
                depth2 = tmpDepth;
                e01 = s1 - s0;
                e02 = s2 - s0;
                twiceArea = e01.x * e02.y - e01.y * e02.x;
            }
        }
        else if (twiceArea >= 0.0f) continue;

        float invTwiceArea = -1.0f / twiceArea;
        float2 bbMinF = min(min(s0, s1), s2);
        float2 bbMaxF = max(max(s0, s1), s2);

        int2 minPx = max(int2(floor(bbMinF)), int2(pagePixelMinX, pagePixelMinY));
        int2 maxPx = min(int2(floor(bbMaxF)), int2(pagePixelMaxX, pagePixelMaxY));
        if (minPx.x > maxPx.x || minPx.y > maxPx.y) continue;

        float2 origin = float2(float(minPx.x) + 0.5f, float(minPx.y) + 0.5f);
        float2 e12 = s2 - s1;
        float2 e20 = s0 - s2;
        float row_b0 = ((origin.x - s1.x) * e12.y - (origin.y - s1.y) * e12.x) * invTwiceArea;
        float row_b1 = ((origin.x - s2.x) * e20.y - (origin.y - s2.y) * e20.x) * invTwiceArea;
        float dx_b0 =  e12.y * invTwiceArea;
        float dx_b1 =  e20.y * invTwiceArea;
        float dy_b0 = -e12.x * invTwiceArea;
        float dy_b1 = -e20.x * invTwiceArea;
        float scanline_b0 = row_b0;
        float scanline_b1 = row_b1;

        for (int py = minPx.y; py <= maxPx.y; py++) {
            float b0 = scanline_b0;
            float b1 = scanline_b1;
            for (int px = minPx.x; px <= maxPx.x; px++) {
                float b2 = 1.0f - b0 - b1;
                if (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f) {
                    float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                    uint2 localPixel = uint2(px - pagePixelMinX, py - pagePixelMinY);
                    uint2 atlasPixel = uint2(atlasBaseX + localPixel.x, atlasBaseY + localPixel.y);
                    const float pageSpaceDepth = dynamicLayer
                        ? CLodVirtualShadowDepthToCachedPageSpace(
                            depth,
                            physicalPageIndex,
                            clipmapInfo.shadowCameraBufferIndex)
                        : depth;
                    InterlockedMin(
                        physicalPages[atlasPixel],
                        CLodVirtualShadowEncodeDepth(
                            pageSpaceDepth,
                            clipmapInfo));
                    anyPixelWritten = true;
                }
                b0 += dx_b0;
                b1 += dx_b1;
            }
            scanline_b0 += dy_b0;
            scanline_b1 += dy_b1;
        }
    }

    // Only static writes establish persistent cache validity. No barrier is
    // needed because every contributing thread writes the same idempotent OR.
    if (anyPixelWritten && !dynamicLayer) {
        uint ignored = 0u;
        InterlockedOr(
            pageTableUAV[uint3(wrappedPageCoords, clipmapLayer)],
            kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask,
            ignored);
    }
}
