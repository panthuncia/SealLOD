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
uint TerrainRvtInfoPageTableResolution() { return UintRootConstant8; }
float TerrainRvtInfoBasePageWorldSize() { return asfloat(UintRootConstant9); }
uint TerrainRvtInfoMaxTerrainSets() { return UintRootConstant10; }
uint TerrainRvtInfoAtlasPoolCount() { return UintRootConstant11; }
uint TerrainRvtInfoMaxClipLevels() { return UintRootConstant12; }
uint TerrainRvtInfoMaxClipInfos() { return UintRootConstant13; }
uint TerrainRvtInfoMaxGeneratedPagesPerFrame() { return UintRootConstant14; }

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
    RWStructuredBuffer<TerrainRvtClipInfo> clipInfos = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtClipInfos)];
    RWStructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    RWStructuredBuffer<TerrainRvtPageTag> pageTags = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageKeys)];
    RWStructuredBuffer<uint4> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    RWStructuredBuffer<uint> requestMasks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestMasks)];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];

    const TerrainRvtInfo previousInfo = infoBuffer[0];
    const bool initialized = (previousInfo.flags & TERRAIN_RVT_INFO_INITIALIZED) != 0u;
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
    info.pageTableResolution = max(TerrainRvtInfoPageTableResolution(), 1u);
    info.flags = TERRAIN_RVT_INFO_INITIALIZED;
    info.basePageWorldSize = max(TerrainRvtInfoBasePageWorldSize(), 0.125f);
    info.physicalAtlasPoolCount = max(TerrainRvtInfoAtlasPoolCount(), 1u);
    info.maxTerrainSets = max(TerrainRvtInfoMaxTerrainSets(), 1u);
    info.maxClipLevels = max(TerrainRvtInfoMaxClipLevels(), 1u);
    info.maxGeneratedPagesPerFrame = max(TerrainRvtInfoMaxGeneratedPagesPerFrame(), 1u);
    info.mipCount = min(max(info.mipCount, 1u), info.maxClipLevels);

    const uint linearThreadIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;

    if (linearThreadIndex == 0u)
    {
        infoBuffer[0] = info;
        counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT] = 0u;
        if (!initialized)
        {
            counters[TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT] = 0u;
            counters[TERRAIN_RVT_COUNTER_REQUEST_COUNT] = 0u;
        }
        counters[TERRAIN_RVT_COUNTER_OVERFLOW_COUNT] = 0u;
        counters[TERRAIN_RVT_COUNTER_REUSE_CURSOR] = 0u;
        if (TERRAIN_RVT_TELEMETRY_ENABLED(perFrame))
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            stats[0] = (TerrainRvtStats)0;
            stats[0].materialSampleRequestedPageMin = 0xffffffffu;
            stats[0].materialSampleResidentPageMin = 0xffffffffu;
            stats[0].materialSamplePhysicalPageMin = 0xffffffffu;
            stats[0].requestPageTableMin = 0xffffffffu;
            stats[0].generationPageTableMin = 0xffffffffu;
            stats[0].materialSampleAttemptedPageMin = 0xffffffffu;
            stats[0].materialSamplePageMissRequestedPageMin = 0xffffffffu;
            stats[0].heightSampleAttemptedPageMin = 0xffffffffu;
            stats[0].heightSamplePageMissRequestedPageMin = 0xffffffffu;
        }
    }

    const uint maxClipInfos = TerrainRvtInfoMaxClipInfos();
    if (linearThreadIndex < maxClipInfos)
    {
        const uint clipLevel = linearThreadIndex % info.maxClipLevels;
        const uint terrainSetIndex = linearThreadIndex / info.maxClipLevels;
        TerrainRvtClipInfo clipInfo = (TerrainRvtClipInfo)0;
        clipInfo.terrainSetIndex = terrainSetIndex;
        clipInfo.clipLevel = clipLevel;
        clipInfo.tableResolution = info.pageTableResolution;
        clipInfo.tableBaseSlot = linearThreadIndex * info.pageTableResolution * info.pageTableResolution;
        clipInfo.pageWorldSize = TerrainRvtPageWorldSize(info, clipLevel);

        if (terrainSetIndex < info.maxTerrainSets)
        {
            const TerrainSetInfo terrain = terrainSets[terrainSetIndex];
            const uint terrainClipCount = TerrainRvtTerrainClipCount(info, terrain);
            if (clipLevel < terrainClipCount &&
                terrain.regionSizeWorld > 0.0f &&
                terrain.regionCountX > 0u &&
                terrain.regionCountY > 0u)
            {
                const uint2 terrainPageCount = TerrainRvtClipPageCount(info, terrain, clipLevel);
                const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
                const float2 cameraSkyrimXY = TerrainRvtSkyrimXYFromRendererPosition(cameras[perFrame.mainCameraIndex].positionWorldSpace.xyz);
                const float2 cameraLocal = cameraSkyrimXY - terrainOrigin;
                const int2 cameraPage = (int2)floor(cameraLocal / max(clipInfo.pageWorldSize, 0.125f));
                const int2 centeredOrigin = cameraPage - int2((int)info.pageTableResolution / 2, (int)info.pageTableResolution / 2);
                const int2 maxOrigin = max(int2(terrainPageCount) - int2(info.pageTableResolution, info.pageTableResolution), int2(0, 0));
                const uint2 originPage = (uint2)clamp(centeredOrigin, int2(0, 0), maxOrigin);

                TerrainRvtClipInfo previousClip = clipInfos[linearThreadIndex];
                clipInfo.originPage = originPage;
                clipInfo.terrainPageCount = terrainPageCount;
                clipInfo.valid = 1u;
                clipInfo.clearDelta = initialized && previousClip.valid != 0u ? int2(originPage) - int2(previousClip.originPage) : int2(0, 0);
            }
        }
        clipInfos[linearThreadIndex] = clipInfo;
    }

    if (!initialized && linearThreadIndex < info.maxPhysicalPages)
    {
        physicalPageOwner[linearThreadIndex] = uint4(TERRAIN_RVT_PHYSICAL_PAGE_OWNER_FREE, 0u, 0u, 0u);
    }
    else if (initialized && linearThreadIndex < info.maxPhysicalPages)
    {
        const uint4 owner = physicalPageOwner[linearThreadIndex];
        const bool ownerLocked = owner.x == TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED;
        const bool ownerHasPage = owner.x < info.maxVirtualPageTableEntries;
        const bool ownerResident = (owner.z & TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT) != 0u;
        if (ownerLocked || (ownerHasPage && !ownerResident))
        {
            if (ownerHasPage)
            {
                const uint oldEntry = pageTable[owner.x];
                if ((oldEntry & TERRAIN_RVT_PAGE_VALID) != 0u &&
                    ((oldEntry & TERRAIN_RVT_PAGE_PHYSICAL_MASK) == linearThreadIndex))
                {
                    pageTable[owner.x] = 0u;
                }
            }
            physicalPageOwner[linearThreadIndex] = uint4(TERRAIN_RVT_PHYSICAL_PAGE_OWNER_FREE, 0u, 0u, 0u);
        }
    }

    if (linearThreadIndex < info.maxVirtualPageTableEntries)
    {
        const uint resolution = info.pageTableResolution;
        const uint slotsPerClip = resolution * resolution;
        const uint clipInfoIndex = linearThreadIndex / slotsPerClip;
        const uint slotInClip = linearThreadIndex - clipInfoIndex * slotsPerClip;
        const uint2 wrappedPage = uint2(slotInClip % resolution, slotInClip / resolution);
        const uint clipLevel = clipInfoIndex % info.maxClipLevels;
        const uint terrainSetIndex = clipInfoIndex / info.maxClipLevels;
        TerrainRvtClipInfo clipInfo = (TerrainRvtClipInfo)0;
        clipInfo.terrainSetIndex = terrainSetIndex;
        clipInfo.clipLevel = clipLevel;
        clipInfo.tableResolution = resolution;
        clipInfo.tableBaseSlot = clipInfoIndex * slotsPerClip;
        clipInfo.pageWorldSize = TerrainRvtPageWorldSize(info, clipLevel);
        if (clipInfoIndex < maxClipInfos && terrainSetIndex < info.maxTerrainSets)
        {
            const TerrainSetInfo terrain = terrainSets[terrainSetIndex];
            const uint terrainClipCount = TerrainRvtTerrainClipCount(info, terrain);
            if (clipLevel < terrainClipCount &&
                terrain.regionSizeWorld > 0.0f &&
                terrain.regionCountX > 0u &&
                terrain.regionCountY > 0u)
            {
                const uint2 terrainPageCount = TerrainRvtClipPageCount(info, terrain, clipLevel);
                const float2 terrainOrigin = float2(terrain.minRegionX, terrain.minRegionY) * terrain.regionSizeWorld;
                const float2 cameraSkyrimXY = TerrainRvtSkyrimXYFromRendererPosition(cameras[perFrame.mainCameraIndex].positionWorldSpace.xyz);
                const float2 cameraLocal = cameraSkyrimXY - terrainOrigin;
                const int2 cameraPage = (int2)floor(cameraLocal / max(clipInfo.pageWorldSize, 0.125f));
                const int2 centeredOrigin = cameraPage - int2((int)resolution / 2, (int)resolution / 2);
                const int2 maxOrigin = max(int2(terrainPageCount) - int2(resolution, resolution), int2(0, 0));
                clipInfo.originPage = (uint2)clamp(centeredOrigin, int2(0, 0), maxOrigin);
                clipInfo.terrainPageCount = terrainPageCount;
                clipInfo.valid = 1u;
            }
        }

        const uint2 pageCoord = uint2(
            TerrainRvtUnwrapLocalPageCoord(wrappedPage.x, clipInfo.originPage.x, resolution),
            TerrainRvtUnwrapLocalPageCoord(wrappedPage.y, clipInfo.originPage.y, resolution));
        bool validSlot = clipInfo.valid != 0u &&
            pageCoord.x >= clipInfo.originPage.x &&
            pageCoord.y >= clipInfo.originPage.y &&
            pageCoord.x < clipInfo.originPage.x + resolution &&
            pageCoord.y < clipInfo.originPage.y + resolution;
        validSlot = validSlot && all(pageCoord < clipInfo.terrainPageCount);

        TerrainRvtPageTag tag = pageTags[linearThreadIndex];
        const bool tagMatches = validSlot &&
            TerrainRvtPageTagMatches(tag, clipInfo.terrainSetIndex, clipInfo.clipLevel, pageCoord);
        if (!tagMatches)
        {
            const uint oldEntry = pageTable[linearThreadIndex];
            if ((oldEntry & TERRAIN_RVT_PAGE_VALID) != 0u)
            {
                const uint oldPhysicalPage = oldEntry & TERRAIN_RVT_PAGE_PHYSICAL_MASK;
                if (oldPhysicalPage < info.maxPhysicalPages)
                {
                    const uint4 oldOwner = physicalPageOwner[oldPhysicalPage];
                    if (oldOwner.x == linearThreadIndex)
                    {
                        physicalPageOwner[oldPhysicalPage] = uint4(TERRAIN_RVT_PHYSICAL_PAGE_OWNER_FREE, 0u, 0u, 0u);
                    }
                }
            }
            pageTable[linearThreadIndex] = 0u;
            requestMasks[linearThreadIndex] = 0u;
        }

        if (validSlot)
        {
            TerrainRvtPageTag newTag;
            newTag.terrainSetIndex = clipInfo.terrainSetIndex;
            newTag.clipLevel = clipInfo.clipLevel;
            newTag.pageX = pageCoord.x;
            newTag.pageY = pageCoord.y;
            pageTags[linearThreadIndex] = newTag;
        }
        else
        {
            pageTags[linearThreadIndex] = (TerrainRvtPageTag)0;
        }
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
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];

    const uint linearThreadIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;
    if (linearThreadIndex == 0u)
    {
        counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT] = 0u;
        counters[TERRAIN_RVT_COUNTER_REUSE_CURSOR] = 0u;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtResolveRequestsCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    TerrainRvtInfo info = infoBuffer[0];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    StructuredBuffer<TerrainRvtPageRequest> requestList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestList)];
    StructuredBuffer<uint> requestMasks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtRequestMasks)];
    StructuredBuffer<TerrainRvtPageTag> pageTags = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageKeys)];
    RWStructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    RWStructuredBuffer<uint4> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    RWStructuredBuffer<TerrainRvtGenerationRequest> generationList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerationList)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const bool telemetryEnabled = TERRAIN_RVT_TELEMETRY_ENABLED(perFrame);

    const uint requestCount = min(counters[TERRAIN_RVT_COUNTER_REQUEST_COUNT], info.maxRequests);
    const uint requestIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;
    if (requestIndex < requestCount)
    {
        if (requestIndex >= info.maxGeneratedPagesPerFrame)
        {
            if (telemetryEnabled)
            {
                RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                InterlockedAdd(stats[0].requestOverflows, 1u);
            }
            return;
        }

        const TerrainRvtPageRequest request = requestList[requestIndex];
        const uint pageTableIndex = request.pageTableIndex;
        if (pageTableIndex >= info.maxVirtualPageTableEntries)
        {
            return;
        }
        const TerrainRvtPageTag tag = pageTags[pageTableIndex];
        if (!TerrainRvtPageTagMatches(tag, request.terrainSetIndex, request.clipLevel, uint2(request.pageX, request.pageY)))
        {
            return;
        }

        const uint requestedMask = requestMasks[pageTableIndex] & 0x3u;
        if (requestedMask == 0u)
        {
            return;
        }

        uint entry = pageTable[pageTableIndex];
        uint currentContentMask = (entry & TERRAIN_RVT_PAGE_CONTENT_MASK) >> TERRAIN_RVT_PAGE_CONTENT_SHIFT;
        if ((entry & TERRAIN_RVT_PAGE_VALID) != 0u)
        {
            const uint currentPhysicalPage = entry & TERRAIN_RVT_PAGE_PHYSICAL_MASK;
            const uint4 currentOwner = currentPhysicalPage < info.maxPhysicalPages
                ? physicalPageOwner[currentPhysicalPage]
                : uint4(TERRAIN_RVT_PHYSICAL_PAGE_OWNER_FREE, 0u, 0u, 0u);
            if (currentPhysicalPage >= info.maxPhysicalPages ||
                (currentOwner.z & TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT) == 0u ||
                currentOwner.x != pageTableIndex)
            {
                pageTable[pageTableIndex] = 0u;
                entry = 0u;
                currentContentMask = 0u;
            }
        }
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
                    if (info.maxPhysicalPages == 0u)
                    {
                        if (telemetryEnabled)
                        {
                            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                            InterlockedAdd(stats[0].allocationFailures, 1u);
                        }
                        return;
                    }

                    const uint observedAllocatedPageCount = counters[TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT];
                    if (!allocatedPage && observedAllocatedPageCount < info.maxPhysicalPages)
                    {
                        uint originalAllocatedPageCount = 0u;
                        InterlockedCompareExchange(
                            counters[TERRAIN_RVT_COUNTER_ALLOCATED_PHYSICAL_PAGE_COUNT],
                            observedAllocatedPageCount,
                            observedAllocatedPageCount + 1u,
                            originalAllocatedPageCount);
                        if (originalAllocatedPageCount == observedAllocatedPageCount)
                        {
                            physicalPageIndex = observedAllocatedPageCount;
                            uint originalOwnerX = 0u;
                            InterlockedCompareExchange(
                                physicalPageOwner[physicalPageIndex].x,
                                TERRAIN_RVT_PHYSICAL_PAGE_OWNER_FREE,
                                TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED,
                                originalOwnerX);
                            if (originalOwnerX == TERRAIN_RVT_PHYSICAL_PAGE_OWNER_FREE)
                            {
                                physicalPageOwner[physicalPageIndex].y = perFrame.frameIndex;
                                physicalPageOwner[physicalPageIndex].z = 0u;
                                physicalPageOwner[physicalPageIndex].w = 0u;
                                uint publishOwnerX = 0u;
                                InterlockedCompareExchange(
                                    physicalPageOwner[physicalPageIndex].x,
                                    TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED,
                                    pageTableIndex,
                                    publishOwnerX);
                                allocatedPage = publishOwnerX == TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED;
                            }
                        }
                    }

                    if (!allocatedPage && info.maxPhysicalPages > 0u && observedAllocatedPageCount >= info.maxPhysicalPages)
                    {
                        uint reuseTicket = 0u;
                        InterlockedAdd(counters[TERRAIN_RVT_COUNTER_REUSE_CURSOR], 1u, reuseTicket);
                        [loop]
                        for (uint attempt = 0u; attempt < info.maxPhysicalPages; ++attempt)
                        {
                            const uint candidatePhysicalPage = (reuseTicket + attempt) % info.maxPhysicalPages;
                            const uint4 oldOwner = physicalPageOwner[candidatePhysicalPage];
                            const bool ownerLocked = oldOwner.x == TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED;
                            const bool ownerHasPage = oldOwner.x < info.maxVirtualPageTableEntries;
                            const bool ownerResident = (oldOwner.z & TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT) != 0u;
                            const uint oldOwnerEntry = ownerHasPage ? pageTable[oldOwner.x] : 0u;
                            const bool ownerBackReferenceValid = ownerResident &&
                                ownerHasPage &&
                                (oldOwnerEntry & TERRAIN_RVT_PAGE_VALID) != 0u &&
                                ((oldOwnerEntry & TERRAIN_RVT_PAGE_PHYSICAL_MASK) == candidatePhysicalPage);
                            const bool ownerInProgress = ownerHasPage && !ownerResident;
                            // Content bits are the feedback signal for pages sampled/requested by the last material pass.
                            const bool ownerUsedByFeedback = ownerBackReferenceValid &&
                                ((requestMasks[oldOwner.x] & TERRAIN_RVT_REQUEST_MASK_CONTENT_MASK) != 0u);
                            const bool ownerFreeOrStale = !ownerLocked && !ownerInProgress && !ownerBackReferenceValid;
                            const bool ownerEvictable = ownerBackReferenceValid && !ownerUsedByFeedback;
                            if (!ownerFreeOrStale && !ownerEvictable)
                            {
                                continue;
                            }

                            uint originalOwnerX = 0u;
                            InterlockedCompareExchange(
                                physicalPageOwner[candidatePhysicalPage].x,
                                oldOwner.x,
                                TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED,
                                originalOwnerX);
                            if (originalOwnerX == oldOwner.x)
                            {
                                physicalPageIndex = candidatePhysicalPage;
                                if (ownerBackReferenceValid && oldOwner.x != pageTableIndex)
                                {
                                    pageTable[oldOwner.x] = 0u;
                                }
                                physicalPageOwner[candidatePhysicalPage].y = perFrame.frameIndex;
                                physicalPageOwner[candidatePhysicalPage].z = 0u;
                                physicalPageOwner[candidatePhysicalPage].w = 0u;
                                uint publishOwnerX = 0u;
                                InterlockedCompareExchange(
                                    physicalPageOwner[candidatePhysicalPage].x,
                                    TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED,
                                    pageTableIndex,
                                    publishOwnerX);
                                if (publishOwnerX == TERRAIN_RVT_PHYSICAL_PAGE_OWNER_LOCKED)
                                {
                                    allocatedPage = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (!allocatedPage && observedAllocatedPageCount >= info.maxPhysicalPages)
                    {
                        if (telemetryEnabled)
                        {
                            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
                            InterlockedAdd(stats[0].allocationFailures, 1u);
                        }
                        return;
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
        generation.terrainSetIndex = request.terrainSetIndex;
        generation.clipLevel = request.clipLevel;
        generation.pageX = request.pageX;
        generation.pageY = request.pageY;
        generation.pad0 = 0u;
        generationList[generationIndex] = generation;
        if (telemetryEnabled)
        {
            RWStructuredBuffer<TerrainRvtStats> stats = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtStats)];
            InterlockedAdd(stats[0].generatedPages, 1u);
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
            InterlockedAdd(stats[0].generationMipHistogram[TerrainRvtTelemetryMipBin(request.clipLevel)], 1u);
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtFinalizeGeneratedPagesCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    TerrainRvtInfo info = infoBuffer[0];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    StructuredBuffer<TerrainRvtGenerationRequest> generationList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerationList)];
    StructuredBuffer<TerrainRvtPageTag> pageTags = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageKeys)];
    RWStructuredBuffer<uint> pageTable = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPageTable)];
    RWStructuredBuffer<uint4> physicalPageOwner = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtPhysicalPageOwner)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    const uint generationCount = min(
        min(counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT], info.maxGenerationEntries),
        info.maxGeneratedPagesPerFrame);
    const uint generationIndex = tid.x + tid.y * TERRAIN_RVT_MAX_DISPATCH_GROUPS_X * 64u;
    if (generationIndex >= generationCount)
    {
        return;
    }

    const TerrainRvtGenerationRequest generation = generationList[generationIndex];
    const uint pageTableIndex = generation.pageTableIndex;
    const uint physicalPageIndex = generation.physicalPageIndex;
    if (pageTableIndex >= info.maxVirtualPageTableEntries || physicalPageIndex >= info.maxPhysicalPages)
    {
        return;
    }

    const TerrainRvtPageTag tag = pageTags[pageTableIndex];
    if (!TerrainRvtPageTagMatches(tag, generation.terrainSetIndex, generation.clipLevel, uint2(generation.pageX, generation.pageY)))
    {
        return;
    }

    const uint4 previousOwner = physicalPageOwner[physicalPageIndex];
    if (previousOwner.x != pageTableIndex)
    {
        return;
    }

    const uint existingEntry = pageTable[pageTableIndex];
    const uint existingContentMask = (existingEntry & TERRAIN_RVT_PAGE_VALID) != 0u
        ? (existingEntry & TERRAIN_RVT_PAGE_CONTENT_MASK) >> TERRAIN_RVT_PAGE_CONTENT_SHIFT
        : 0u;
    pageTable[pageTableIndex] = TerrainRvtPackPageTableEntry(physicalPageIndex, existingContentMask | generation.contentMask);
    physicalPageOwner[physicalPageIndex] = uint4(
        pageTableIndex,
        perFrame.frameIndex,
        TERRAIN_RVT_PHYSICAL_PAGE_RESIDENT,
        0u);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void TerrainRvtBuildGenerateDispatchArgsCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    RWStructuredBuffer<DispatchIndirectArgs> argsOut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerateDispatchArgs)];
    TerrainRvtInfo info = infoBuffer[0];
    const uint generationCount = min(
        min(counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT], info.maxGenerationEntries),
        info.maxGeneratedPagesPerFrame);
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

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRvtGeneratePagesCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<TerrainRvtInfo> infoBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtInfo)];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtCounters)];
    StructuredBuffer<TerrainRvtGenerationRequest> generationList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::RvtGenerationList)];
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    TerrainRvtInfo info = infoBuffer[0];
    const uint generationCount = min(
        min(counters[TERRAIN_RVT_COUNTER_GENERATION_COUNT], info.maxGenerationEntries),
        info.maxGeneratedPagesPerFrame);
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
    const uint mip = generation.clipLevel;
    const uint2 pageCoord = uint2(generation.pageX, generation.pageY);
    const uint terrainSetIndex = generation.terrainSetIndex;
    if (terrainSetIndex >= info.maxTerrainSets || mip >= info.maxClipLevels)
    {
        return;
    }
    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    if (TERRAIN_RVT_TELEMETRY_ENABLED(perFrame) && texelInPage == 0u)
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
