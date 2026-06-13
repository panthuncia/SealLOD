#define TERRAIN_RVT_GENERATION 1
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/materialFlags.hlsli"
#include "include/visUtilCommon.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/clodResolveCommon.hlsli"
#include "include/terrainCommon.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"

struct DispatchIndirectArgs
{
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

static const uint TERRAIN_RVT_MAX_DISPATCH_GROUPS_X = 65535u;

uint TerrainRvtInfoPageSize() { return UintRootConstant0; }
uint TerrainRvtInfoBorderTexels() { return UintRootConstant1; }
uint TerrainRvtInfoAtlasPagesWide() { return UintRootConstant2; }
uint TerrainRvtInfoAtlasPagesHigh() { return UintRootConstant3; }
uint TerrainRvtInfoMaxPageTableEntries() { return UintRootConstant4; }
uint TerrainRvtInfoMaxRequests() { return UintRootConstant5; }
uint TerrainRvtInfoMaxGenerationEntries() { return UintRootConstant6; }
uint TerrainRvtInfoMipCount() { return UintRootConstant7; }
uint TerrainRvtInfoMaxVirtualPagesPerAxis() { return UintRootConstant8; }
float TerrainRvtInfoBasePageWorldSize() { return asfloat(UintRootConstant9); }
uint TerrainRvtInfoAtlasPoolCount() { return UintRootConstant11; }
int TerrainRvtInfoPageTableOriginPageX() { return asint(UintRootConstant12); }
int TerrainRvtInfoPageTableOriginPageY() { return asint(UintRootConstant13); }

float TerrainRvtMaxAxisScale_RowVector(row_major matrix m)
{
    const float3 row0 = float3(m._11, m._12, m._13);
    const float3 row1 = float3(m._21, m._22, m._23);
    const float3 row2 = float3(m._31, m._32, m._33);
    return sqrt(max(dot(row0, row0), max(dot(row1, row1), dot(row2, row2))));
}

uint TerrainRvtMipForFootprintWorld(TerrainRvtInfo info, float footprintWorld)
{
    const float texelWorldSize0 = max(TerrainRvtBasePageWorldSize(info) / max((float)info.pageSize, 1.0f), 1.0e-4f);
    const float requestedMip = 0.5f + log2(max(footprintWorld / texelWorldSize0, 1.0e-4f));
    return min((uint)max(0.0f, floor(requestedMip)), max(info.mipCount, 1u) - 1u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtFrameResetCS(uint3 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    RWStructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    RWStructuredBuffer<uint> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    RWStructuredBuffer<uint> requestMasks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestMasks)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];

    TerrainRvtInfo info;
    info.pageSize = max(TerrainRvtInfoPageSize(), 1u);
    info.borderTexels = TerrainRvtInfoBorderTexels();
    info.physicalTileTexelSide = info.pageSize + info.borderTexels * 2u;
    info.physicalAtlasPagesWide = max(TerrainRvtInfoAtlasPagesWide(), 1u);
    info.physicalAtlasPagesHigh = max(TerrainRvtInfoAtlasPagesHigh(), 1u);
    info.physicalAtlasPoolCount = max(TerrainRvtInfoAtlasPoolCount(), 1u);
    info.maxPhysicalPages = min(info.physicalAtlasPagesWide * info.physicalAtlasPagesHigh * info.physicalAtlasPoolCount, 0x00FFFFFFu);
    info.maxVirtualPageTableEntries = TerrainRvtInfoMaxPageTableEntries();
    info.maxRequests = TerrainRvtInfoMaxRequests();
    info.maxGenerationEntries = TerrainRvtInfoMaxGenerationEntries();
    info.mipCount = max(TerrainRvtInfoMipCount(), 1u);
    info.maxVirtualPagesPerAxis = max(TerrainRvtInfoMaxVirtualPagesPerAxis(), 1u);
    info.flags = 0u;
    info.basePageWorldSize = max(TerrainRvtInfoBasePageWorldSize(), 0.125f);
    info.pageTableOriginPageX = TerrainRvtInfoPageTableOriginPageX();
    info.pageTableOriginPageY = TerrainRvtInfoPageTableOriginPageY();

    const uint linearThreadIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;

    if (linearThreadIndex == 0u)
    {
        infoBuffer[0] = info;
        counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT] = 0u;
        counters[TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT] = 0u;
        counters[TERRAIN_RVT_COUNTER_OVERFLOW_COUNT] = 0u;
        stats[0] = (TerrainRvtStats)0;
        stats[0].materialSampleRequestedPageMin = 0xffffffffu;
        stats[0].materialSampleResidentPageMin = 0xffffffffu;
        stats[0].materialSamplePhysicalPageMin = 0xffffffffu;
        stats[0].requestPageTableMin = 0xffffffffu;
        stats[0].generationPageTableMin = 0xffffffffu;
        stats[0].materialSampleAttemptedPageMin = 0xffffffffu;
        stats[0].materialSamplePageMissRequestedPageMin = 0xffffffffu;
    }

    const uint maxEntries = info.maxVirtualPageTableEntries;
    if (linearThreadIndex < maxEntries)
    {
        pageTable[linearThreadIndex] = 0u;
    }
    if (linearThreadIndex < info.maxPhysicalPages)
    {
        physicalPageOwner[linearThreadIndex] = 0xffffffffu;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtClearFeedbackRequestsCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    TerrainRvtInfo info = infoBuffer[0];
    RWStructuredBuffer<uint> requestMasks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestMasks)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];

    const uint linearThreadIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;
    if (linearThreadIndex == 0u)
    {
        counters[TERRAIN_RVT_COUNTER_REQUEST_COUNT] = 0u;
        counters[TERRAIN_RVT_COUNTER_OVERFLOW_COUNT] = 0u;
    }
    if (linearThreadIndex < info.maxVirtualPageTableEntries)
    {
        requestMasks[linearThreadIndex] = 0u;
    }
}

uint TerrainRvtMaterialDataIndexFromVisibility(
    uint clusterIndex,
    ByteAddressBuffer visibleClusterBuffer,
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue)
{
    if (clusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE && VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        clusterIndex = diceQueue[clusterIndex - VISBUF_REYES_PATCH_INDEX_BASE].visibleClusterIndex;
    }

    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
    if (CLodVisibleClusterIsVoxel(packedCluster))
    {
        return 0xffffffffu;
    }

    const uint drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDraw(drawRecordIndex);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    PerMeshBuffer mesh = perMeshBuffer[instanceData.perMeshBufferIndex];
    return mesh.materialDataIndex;
}

float3 TerrainRvtReconstructWorldPosition(uint2 pixel, float linearDepth)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera cam = cameras[perFrame.mainCameraIndex];
    float2 winSize = float2(perFrame.screenResX, perFrame.screenResY);
    float2 pixelUv = (float2(pixel) + 0.5f) / winSize;
    float2 ndc = float2(pixelUv.x * 2.0f - 1.0f, 1.0f - pixelUv.y * 2.0f);
    float4 viewFar = mul(float4(ndc, 1.0f, 1.0f), cam.projectionInverse);
    viewFar.xyz /= max(abs(viewFar.w), 1.0e-6f);
    float viewScale = linearDepth / max(abs(viewFar.z), 1.0e-6f);
    float3 viewPosition = viewFar.xyz * viewScale;
    float4 worldPosition = mul(float4(viewPosition, 1.0f), cam.viewInverse);
    return worldPosition.xyz / max(abs(worldPosition.w), 1.0e-6f);
}

float TerrainRvtLoadVisibilityDepthOrDefault(Texture2D<uint64_t> visibility, uint2 pixel, float defaultDepth)
{
    const uint64_t vis = visibility[pixel];
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        return defaultDepth;
    }

    float depth;
    uint clusterIndex;
    uint primID;
    UnpackVisKey(vis, depth, clusterIndex, primID);
    return depth;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void TerrainRvtMarkVisibilityMaterialPagesCS(uint3 dtid : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (perFrame.terrainRvtEnabled == 0u)
    {
        return;
    }

    if (dtid.x >= perFrame.screenResX || dtid.y >= perFrame.screenResY)
    {
        return;
    }

    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    const uint64_t vis = visibility[dtid.xy];
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        return;
    }

    float depth;
    uint clusterIndex;
    uint primID;
    UnpackVisKey(vis, depth, clusterIndex, primID);

    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
    const uint materialDataIndex = TerrainRvtMaterialDataIndexFromVisibility(clusterIndex, visibleClusterBuffer, diceQueue);
    if (materialDataIndex == 0xffffffffu)
    {
        return;
    }

    StructuredBuffer<MaterialEvalInfo> materialData = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialEvalDataBuffer)];
    MaterialEvalInfo materialInfo = materialData[materialDataIndex];
    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) == 0u)
    {
        return;
    }

    const uint2 pixel = dtid.xy;
    const uint2 pixelX = uint2(min(dtid.x + 1u, perFrame.screenResX - 1u), dtid.y);
    const uint2 pixelY = uint2(dtid.x, min(dtid.y + 1u, perFrame.screenResY - 1u));
    const float depthX = TerrainRvtLoadVisibilityDepthOrDefault(visibility, pixelX, depth);
    const float depthY = TerrainRvtLoadVisibilityDepthOrDefault(visibility, pixelY, depth);
    const float3 positionWS = TerrainRvtReconstructWorldPosition(pixel, depth);
    const float3 positionWSX = TerrainRvtReconstructWorldPosition(pixelX, depthX);
    const float3 positionWSY = TerrainRvtReconstructWorldPosition(pixelY, depthY);
    TerrainRvtMarkPosition(
        materialInfo.terrainSetIndex,
        positionWS,
        positionWSX - positionWS,
        positionWSY - positionWS,
        TERRAIN_RVT_CONTENT_MATERIAL | TERRAIN_RVT_CONTENT_HEIGHT);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtMarkVisibleClusterPagesCS(uint3 dtid : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (perFrame.terrainRvtEnabled == 0u ||
        VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX == 0xFFFFFFFFu ||
        VISBUF_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return;
    }

    StructuredBuffer<uint> visibleClusterCounter = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_COUNTER_DESCRIPTOR_INDEX];
    const uint visibleClusterCount = visibleClusterCounter[0];
    const uint visibleClusterIndex = dtid.x;
    if (visibleClusterIndex >= visibleClusterCount)
    {
        return;
    }

    ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, visibleClusterIndex);
    if (CLodVisibleClusterIsVoxel(packedCluster))
    {
        return;
    }
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader pageHeader = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    if (localMeshletIndex >= pageHeader.meshletCount || pageHeader.descriptorOffset == 0u)
    {
        return;
    }
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(
        pageSlabDescriptorIndex,
        pageSlabByteOffset,
        pageHeader.descriptorOffset,
        localMeshletIndex);

    const uint drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(drawRecordIndex);
    StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const PerMeshBuffer mesh = perMeshes[meshInstance.perMeshBufferIndex];
    StructuredBuffer<MaterialEvalInfo> materialData = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialEvalDataBuffer)];
    const MaterialEvalInfo materialInfo = materialData[mesh.materialDataIndex];
    if ((materialInfo.materialFlags & MATERIAL_TERRAIN) == 0u)
    {
        return;
    }

    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(drawRecordIndex);
    const float scale = TerrainRvtMaxAxisScale_RowVector(objectData.model);
    const float displacementMagnitude = max(abs(materialInfo.geometricDisplacementMin), abs(materialInfo.geometricDisplacementMax)) * scale;

    const float3 clusterCenterWS = mul(float4(meshletDesc.bounds.xyz, 1.0f), objectData.model).xyz;
    const float2 clusterCenterXY = TerrainRvtSkyrimXYFromRendererPosition(clusterCenterWS);
    const float clusterTerrainRadius = max(meshletDesc.terrainRvtLocalSkyrimXYRadius * scale, 1.0e-3f) + displacementMagnitude;
    const float2 rvtMinXY = clusterCenterXY - clusterTerrainRadius.xx;
    const float2 rvtMaxXY = clusterCenterXY + clusterTerrainRadius.xx;

    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    const TerrainRvtInfo terrainRvtInfo = infoBuffer[0];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[CLodVisibleClusterViewID(packedCluster)];
    const float radiusWS = max(clusterTerrainRadius, 1.0e-3f);
    const float3 centerVS = mul(float4(clusterCenterWS, 1.0f), camera.view).xyz;
    const float depth = max(abs(centerVS.z), radiusWS + 1.0e-3f);
    const float projectedRadiusNdc = radiusWS * max(abs(camera.projection._11), abs(camera.projection._22)) / depth;
    const float projectedDiameterPixels = max(projectedRadiusNdc * max((float)perFrame.screenResX, (float)perFrame.screenResY), 1.0f);
    const float terrainExtentWorld = max(rvtMaxXY.x - rvtMinXY.x, rvtMaxXY.y - rvtMinXY.y);
    const uint mip = TerrainRvtMipForFootprintWorld(terrainRvtInfo, terrainExtentWorld / projectedDiameterPixels);

    TerrainRvtMarkWorldRect(
        materialInfo.terrainSetIndex,
        rvtMinXY,
        rvtMaxXY,
        mip,
        TERRAIN_RVT_CONTENT_MATERIAL | TERRAIN_RVT_CONTENT_HEIGHT);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtClearGenerationCounterCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x == 0u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
        counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT] = 0u;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtResolveRequestsCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    TerrainRvtInfo info = infoBuffer[0];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    StructuredBuffer<uint> requestList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestList)];
    StructuredBuffer<uint> requestMasks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestMasks)];
    RWStructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    RWStructuredBuffer<TerrainRvtGenerationRequest> generationList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerationList)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = perFrame.terrainRvtTelemetryEnabled != 0u;

    const uint requestCount = min(counters[TERRAIN_RVT_COUNTER_REQUEST_COUNT], info.maxRequests);
    const uint requestIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;
    if (requestIndex < requestCount)
    {
        const uint pageTableIndex = requestList[requestIndex];
        if (pageTableIndex >= info.maxVirtualPageTableEntries)
        {
            return;
        }

        const uint requestedMask = requestMasks[pageTableIndex] & 0x3u;
        if (requestedMask == 0u)
        {
            return;
        }

        uint entry = pageTable[pageTableIndex];
        const uint currentContentMask = (entry & TERRAIN_RVT_PAGE_CONTENT_MASK) >> TERRAIN_RVT_PAGE_CONTENT_SHIFT;
        if ((entry & TERRAIN_RVT_PAGE_VALID) != 0u && (currentContentMask & requestedMask) == requestedMask)
        {
            if (telemetryEnabled)
            {
                RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                InterlockedAdd(stats[0].resolveResidentPages, 1u);
            }
            return;
        }

        uint physicalPageIndex = entry & TERRAIN_RVT_PAGE_PHYSICAL_MASK;
        if ((entry & TERRAIN_RVT_PAGE_VALID) == 0u)
        {
            if (perFrame.terrainRvtDebugView == 4u)
            {
                physicalPageIndex = pageTableIndex % max(info.maxPhysicalPages, 1u);
                InterlockedMax(counters[TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT], physicalPageIndex + 1u);
            }
            else
            {
                bool allocatedPage = false;
                [loop]
                while (!allocatedPage)
                {
                    const uint observedAllocatedPageCount = counters[TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT];
                    if (observedAllocatedPageCount >= info.maxPhysicalPages)
                    {
                        if (telemetryEnabled)
                        {
                            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                            InterlockedAdd(stats[0].allocationFailures, 1u);
                        }
                        return;
                    }

                    uint originalAllocatedPageCount = 0u;
                    InterlockedCompareExchange(
                        counters[TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT],
                        observedAllocatedPageCount,
                        observedAllocatedPageCount + 1u,
                        originalAllocatedPageCount);
                    if (originalAllocatedPageCount == observedAllocatedPageCount)
                    {
                        physicalPageIndex = observedAllocatedPageCount;
                        allocatedPage = true;
                    }
                }
            }
        }

        uint generationIndex = 0u;
        InterlockedAdd(counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT], 1u, generationIndex);
        if (generationIndex >= info.maxGenerationEntries)
        {
            if (telemetryEnabled)
            {
                RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                InterlockedAdd(stats[0].requestOverflows, 1u);
            }
            return;
        }

        TerrainRvtGenerationRequest generation;
        generation.pageTableIndex = pageTableIndex;
        generation.physicalPageIndex = physicalPageIndex;
        generation.contentMask = requestedMask;
        generation.pad0 = 0u;
        generationList[generationIndex] = generation;
        pageTable[pageTableIndex] = TerrainRvtPackPageTableEntry(physicalPageIndex, currentContentMask | requestedMask);
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].generatedPages, 1u);
            const uint mip = TerrainRvtMipFromPageTableIndex(info, pageTableIndex);
            InterlockedAdd(stats[0].generationMipHistogram[TerrainRvtTelemetryMipBin(mip)], 1u);
            if ((requestedMask & TERRAIN_RVT_CONTENT_HEIGHT) != 0u)
            {
                InterlockedAdd(stats[0].generationHeightPages, 1u);
            }
            if ((requestedMask & TERRAIN_RVT_CONTENT_MATERIAL) != 0u)
            {
                InterlockedAdd(stats[0].generationMaterialPages, 1u);
            }
            if ((requestedMask & (TERRAIN_RVT_CONTENT_HEIGHT | TERRAIN_RVT_CONTENT_MATERIAL)) ==
                (TERRAIN_RVT_CONTENT_HEIGHT | TERRAIN_RVT_CONTENT_MATERIAL))
            {
                InterlockedAdd(stats[0].generationCombinedPages, 1u);
            }
            InterlockedXor(stats[0].generationPageTableXor, pageTableIndex);
            InterlockedXor(stats[0].generationPhysicalPageXor, physicalPageIndex);
#if TERRAIN_RVT_ENABLE_PAGE_STAMP_DEBUG
            InterlockedXor(stats[0].generationPairHashXor, TerrainRvtDebugHash(pageTableIndex ^ (physicalPageIndex * 0x9e3779b9u)));
#endif
            InterlockedMin(stats[0].generationPageTableMin, pageTableIndex);
            InterlockedMax(stats[0].generationPageTableMax, pageTableIndex);
        }
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void TerrainRvtBuildGenerateDispatchArgsCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    RWStructuredBuffer<DispatchIndirectArgs> argsOut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerateDispatchArgs)];
    TerrainRvtInfo info = infoBuffer[0];
    const uint generationCount = min(counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT], info.maxGenerationEntries);
    const uint texelsPerPage = info.physicalTileTexelSide * info.physicalTileTexelSide;
    const uint totalThreadCount = generationCount * texelsPerPage;
    const uint groupCount = (totalThreadCount + 63u) / 64u;
    const uint dispatchX = min(max(groupCount, 1u), TERRAIN_RVT_MAX_DISPATCH_GROUPS_X);
    DispatchIndirectArgs args;
    args.dispatchX = dispatchX;
    args.dispatchY = max(1u, (groupCount + dispatchX - 1u) / dispatchX);
    args.dispatchZ = 1u;
    argsOut[0] = args;
}

void TerrainRvtDecodePageTableIndex(TerrainRvtInfo info, TerrainSetInfo terrain, uint pageTableIndex, out uint terrainSetIndex, out uint mip, out uint2 pageCoord)
{
    const uint entriesPerTerrainSet = max(TerrainRvtPageTableEntriesPerTerrainSet(info), 1u);
    terrainSetIndex = pageTableIndex / entriesPerTerrainSet;
    mip = 0u;
    pageCoord = 0u.xx;
    uint remaining = pageTableIndex - terrainSetIndex * entriesPerTerrainSet;
    [loop]
    for (uint i = 0u; i < info.mipCount; ++i)
    {
        const uint axis = TerrainRvtMipAxis(info, i);
        const uint mipEntries = axis * axis;
        if (remaining < mipEntries)
        {
            mip = i;
            const int2 tablePageCoord = int2(remaining % axis, remaining / axis);
            const int2 worldPageCoord = tablePageCoord + TerrainRvtPageTableOriginForMip(info, mip);
            pageCoord = (uint2)(worldPageCoord - TerrainRvtTerrainOriginPageForMip(info, terrain, mip));
            return;
        }
        remaining -= mipEntries;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtGeneratePagesCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    StructuredBuffer<TerrainRvtGenerationRequest> generationList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerationList)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];

    TerrainRvtInfo info = infoBuffer[0];
    const uint generationCount = min(counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT], info.maxGenerationEntries);
    const uint tileSide = max(info.physicalTileTexelSide, 1u);
    const uint texelsPerPage = tileSide * tileSide;
    const uint linearIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;
    const uint generationIndex = linearIndex / texelsPerPage;
    if (generationIndex >= generationCount)
    {
        return;
    }

    const uint texelInPage = linearIndex - generationIndex * texelsPerPage;
    const uint2 tileTexel = uint2(texelInPage % tileSide, texelInPage / tileSide);
    const TerrainRvtGenerationRequest generation = generationList[generationIndex];
    const uint physicalPageIndex = generation.physicalPageIndex;
    if (physicalPageIndex >= info.maxPhysicalPages)
    {
        return;
    }
    if (texelInPage == 0u)
    {
        RWStructuredBuffer<uint> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
        uint previousOwner = 0xffffffffu;
        InterlockedCompareExchange(
            physicalPageOwner[physicalPageIndex],
            0xffffffffu,
            generation.pageTableIndex,
            previousOwner);
        if (previousOwner != 0xffffffffu && previousOwner != generation.pageTableIndex)
        {
            ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
            if (perFrame.terrainRvtTelemetryEnabled != 0u)
            {
                RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                InterlockedAdd(stats[0].physicalPageOwnerCollisions, 1u);
            }
        }
    }

    uint mip;
    uint2 pageCoord;
    uint terrainSetIndex;
    const uint entriesPerTerrainSet = max(TerrainRvtPageTableEntriesPerTerrainSet(info), 1u);
    terrainSetIndex = generation.pageTableIndex / entriesPerTerrainSet;
    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    TerrainRvtDecodePageTableIndex(info, terrain, generation.pageTableIndex, terrainSetIndex, mip, pageCoord);
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    if (perFrame.terrainRvtTelemetryEnabled != 0u && texelInPage == 0u)
    {
        RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
        InterlockedAdd(stats[0].generationTexels, texelsPerPage);
    }

    const float pageWorldSize = TerrainRvtPageWorldSize(info, mip);
    const float texelWorldSize = pageWorldSize / (float)max(info.pageSize, 1u);
    const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
    const float2 unclampedPageTexel = (float2)tileTexel - (float)info.borderTexels + 0.5f.xx;
    const float2 terrainSize = float2(terrain.regionCountX, terrain.regionCountY) * terrain.regionSizeWorld;
    const float2 terrainLocal = clamp(
        ((float2)pageCoord * pageWorldSize) + unclampedPageTexel * texelWorldSize,
        0.5f.xx * texelWorldSize,
        terrainSize - 0.5f.xx * texelWorldSize);
    const float2 skyrimXY = terrainOrigin + terrainLocal;
    const float3 positionWS = float3(skyrimXY.x, 0.0f, -skyrimXY.y);
    const float3 dpdxWS = float3(texelWorldSize, 0.0f, 0.0f);
    const float3 dpdyWS = float3(0.0f, 0.0f, -texelWorldSize);

    const uint pagesPerPool = max(info.physicalAtlasPagesWide * info.physicalAtlasPagesHigh, 1u);
    const uint atlasPoolIndex = min(physicalPageIndex / pagesPerPool, max(info.physicalAtlasPoolCount, 1u) - 1u);
    const uint localPhysicalPageIndex = physicalPageIndex - atlasPoolIndex * pagesPerPool;
    const uint2 physicalPage = uint2(
        localPhysicalPageIndex % max(info.physicalAtlasPagesWide, 1u),
        localPhysicalPageIndex / max(info.physicalAtlasPagesWide, 1u));
    const uint3 atlasTexel = uint3(physicalPage * tileSide + tileTexel, atlasPoolIndex);

    if ((generation.contentMask & TERRAIN_RVT_CONTENT_HEIGHT) != 0u)
    {
        RWTexture2DArray<float> heightAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtHeightAtlas)];
        heightAtlas[atlasTexel] = TerrainSampleGeometricHeight(terrainSetIndex, positionWS);
    }

    if ((generation.contentMask & TERRAIN_RVT_CONTENT_MATERIAL) != 0u)
    {
        MaterialInputs inputs = (MaterialInputs)0;
        ApplyTerrainMaterialInternal(
            MATERIAL_TERRAIN | MATERIAL_GEOMETRIC_DISPLACEMENT,
            terrainSetIndex,
            positionWS,
            dpdxWS,
            dpdyWS,
            float3(0.0f, 1.0f, 0.0f),
            1.0f.xxx,
            inputs);

        RWTexture2DArray<float4> albedoAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtAlbedoAtlas)];
        RWTexture2DArray<float4> normalAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtNormalAtlas)];
        RWTexture2DArray<float4> materialAtlas = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtMaterialAtlas)];
        const float3 normalTS = TerrainRvtWorldToTangentNormal(inputs.normalWS, float3(0.0f, 1.0f, 0.0f));
        albedoAtlas[atlasTexel] = float4(saturate(inputs.albedo), 1.0f);
        normalAtlas[atlasTexel] = float4(saturate(normalTS * 0.5f + 0.5f), 1.0f);
        float pageStamp = 1.0f;
#if TERRAIN_RVT_ENABLE_PAGE_STAMP_DEBUG
        pageStamp = TerrainRvtDebugPageStampValue(generation.pageTableIndex);
#endif
        materialAtlas[atlasTexel] = float4(
            saturate(inputs.roughness),
            saturate(inputs.metallic),
            saturate(inputs.ambientOcclusion),
            pageStamp);
    }
}
