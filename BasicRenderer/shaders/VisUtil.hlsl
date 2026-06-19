#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/waveIntrinsicsHelpers.hlsli"
#include "include/visUtilCommon.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"
#include "include/visibilityPacking.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/materialFlags.hlsli"

static const uint TERRAIN_REGION_BIN_COUNT = 65536u;
static const uint TERRAIN_INVALID_REGION_BIN = 0xffffffffu;

uint GetMaterialIdFromCluster(uint clusterIndex,
                              ByteAddressBuffer visibleClusterBuffer,
                              StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue,
                              StructuredBuffer<PerMeshInstanceBuffer> perMeshInstance,
                              StructuredBuffer<PerMeshBuffer> perMeshBuffer)
{
    bool isReyesPatch = false;
    if (clusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE && VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        clusterIndex = diceQueue[clusterIndex - VISBUF_REYES_PATCH_INDEX_BASE].visibleClusterIndex;
        isReyesPatch = true;
    }

    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
    if (CLodVisibleClusterIsVoxel(packedCluster))
    {
        return VISBUF_VOXEL_MATERIAL_BIN_INDEX;
    }

    uint drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDraw(drawRecordIndex);
    PerMeshBuffer meshBuffer = perMeshBuffer[instanceData.perMeshBufferIndex];

    return isReyesPatch ? meshBuffer.materialReyesEvalCompileFlagsID : meshBuffer.materialEvalCompileFlagsID;
}

bool IsSARPGrassVisibilityCluster(uint clusterIndex)
{
    return clusterIndex >= VISBUF_SARP_GRASS_INDEX_BASE;
}

bool TryGetMaterialEvalDataIndexFromCluster(
    uint clusterIndex,
    ByteAddressBuffer visibleClusterBuffer,
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue,
    out uint materialDataIndex)
{
    materialDataIndex = 0xffffffffu;
    if (clusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE && VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        clusterIndex = diceQueue[clusterIndex - VISBUF_REYES_PATCH_INDEX_BASE].visibleClusterIndex;
    }

    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
    if (CLodVisibleClusterIsVoxel(packedCluster))
    {
        return false;
    }

    const uint meshletIndex = CLodVisibleClusterMeshletIndex(packedCluster);
    const uint drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDraw(drawRecordIndex);
    PerMeshBuffer mesh = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)][instanceData.perMeshBufferIndex];
    StructuredBuffer<InstanceDrawRecord> instanceDrawRecords = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::InstanceDrawRecordBuffer)];
    InstanceDrawRecord draw = instanceDrawRecords[drawRecordIndex];
    StructuredBuffer<CLodMeshMetadata> meshMetadata = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    CLodMeshMetadata metadata = meshMetadata[mesh.clodMeshMetadataIndex];
    StructuredBuffer<uint> groupChunks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::GroupChunks)];
    StructuredBuffer<CLodGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    uint groupChunkIndex = metadata.groupChunkBase + meshletIndex / CLOD_GROUP_CHUNK_SIZE;
    uint groupInChunk = meshletIndex % CLOD_GROUP_CHUNK_SIZE;
    uint groupIndex = groupChunks[groupChunkIndex] + groupInChunk;
    CLodGroup group = groups[groupIndex];
    materialDataIndex = draw.materialDataIndex + group.materialOffset;
    return true;
}

uint TerrainRegionBinFromPosition(uint terrainSetIndex, float3 positionWS)
{
    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    TerrainSetInfo terrain = terrainSets[terrainSetIndex];
    if (terrain.regionSizeWorld <= 0.0f || terrain.regionCountX == 0u || terrain.regionCountY == 0u)
    {
        return TERRAIN_INVALID_REGION_BIN;
    }

    float2 skyrimXY = float2(positionWS.x, -positionWS.z);
    int2 regionCoord = int2(floor(skyrimXY / terrain.regionSizeWorld));
    if (regionCoord.x < terrain.minRegionX || regionCoord.y < terrain.minRegionY)
    {
        return TERRAIN_INVALID_REGION_BIN;
    }

    uint2 localRegion;
    localRegion.x = (uint)(regionCoord.x - terrain.minRegionX);
    localRegion.y = (uint)(regionCoord.y - terrain.minRegionY);
    if (localRegion.x >= terrain.regionCountX || localRegion.y >= terrain.regionCountY)
    {
        return TERRAIN_INVALID_REGION_BIN;
    }

    uint regionIndex = terrain.regionBase + localRegion.y * terrain.regionCountX + localRegion.x;
    if (regionIndex >= terrain.regionBase + terrain.regionCount)
    {
        return TERRAIN_INVALID_REGION_BIN;
    }
    return regionIndex < TERRAIN_REGION_BIN_COUNT ? regionIndex : TERRAIN_INVALID_REGION_BIN;
}

// UintRootConstant0 = NumMaterials
[shader("compute")]
[numthreads(64, 1, 1)]
void ClearMaterialCountersCS(uint3 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> materialPixelCount = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialPixelCountBuffer)];
    RWStructuredBuffer<uint> materialWriteCursor = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialWriteCursorBuffer)];
    
    uint numMaterials = UintRootConstant0;
    uint i = tid.x;
    if (i < numMaterials)
    {
        materialPixelCount[i] = 0;
        materialWriteCursor[i] = 0;
    }
}

// Histogram: one thread per pixel, atomic into count[m].
[shader("compute")]
[numthreads(8, 8, 1)]
void MaterialHistogramCS(uint3 dtid : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    uint screenW = perFrame.screenResX;
    uint screenH = perFrame.screenResY;
    if (dtid.x >= screenW || dtid.y >= screenH)
        return;

    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshInstanceBuffer> perMeshInstance = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshInstanceBuffer)];
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];

    RWStructuredBuffer<uint> materialPixelCount = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialPixelCountBuffer)];

    uint2 pixel = dtid.xy;
    uint64_t vis = visibility[pixel];
    if (vis == 0xFFFFFFFFFFFFFFFF) { // No cluster visible
        return;
    }
    
    float depth;
    uint clusterIndex;
    uint primID;

    UnpackVisKey(vis, depth, clusterIndex, primID);
    if (IsSARPGrassVisibilityCluster(clusterIndex))
    {
        return;
    }

    // Derive material ID
    uint matId = GetMaterialIdFromCluster(clusterIndex, visibleClusterBuffer, diceQueue, perMeshInstance, perMeshBuffer);
    
    // Group threads in the wave by matId
    uint4 mask = WaveMatch(matId);

    // TODO: Can we optimize other cases?
    // General case: one atomic per unique matId in the wave
    if (IsWaveGroupLeader(mask))
    {
        uint groupSize = CountBits128(mask);
        InterlockedAdd(materialPixelCount[matId], groupSize);
    }
}

// Build grouped pixel list: use offsets[] as base and a per-material write cursor
[shader("compute")]
[numthreads(8, 8, 1)]
void BuildPixelListCS(uint3 dtid : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    uint screenW = perFrame.screenResX;
    uint screenH = perFrame.screenResY;
    if (dtid.x >= screenW || dtid.y >= screenH)
    {
        return;
    }

    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshInstanceBuffer> perMeshInstance = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshInstanceBuffer)];
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];

    StructuredBuffer<uint> materialOffset = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialOffsetBuffer)];
    RWStructuredBuffer<uint> materialWriteCursor = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialWriteCursorBuffer)];

    RWStructuredBuffer<PixelRef> pixelList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::PixelListBuffer)];

    
    uint2 pixel = dtid.xy;
    uint64_t vis = visibility[pixel];

    if (vis == 0xFFFFFFFFFFFFFFFF) { // No cluster visible
        return;
    }

    float depth;
    uint clusterIndex;
    uint primID;
    UnpackVisKey(vis, depth, clusterIndex, primID);
    if (IsSARPGrassVisibilityCluster(clusterIndex))
    {
        return;
    }
    
    uint matId = GetMaterialIdFromCluster(clusterIndex, visibleClusterBuffer, diceQueue, perMeshInstance, perMeshBuffer);

    // Group threads in this wave by matId
    uint4 groupMask = WaveMatch(matId);
    uint groupSize = CountBits128(groupMask);
    uint lane = WaveGetLaneIndex();
    uint leaderLane = GetWaveGroupLeaderLane(groupMask);

    // One atomic per (wave, matId) group
    uint groupBase = 0;
    if (lane == leaderLane)
    {
        InterlockedAdd(materialWriteCursor[matId], groupSize, groupBase);
    }

    // Broadcast base index from leader to all lanes in the group
    groupBase = WaveReadLaneAt(groupBase, leaderLane);

    // Compute our rank within this matId group
    uint rankInGroup = GetLaneRankInGroup(groupMask, lane);

    // Final index into pixel list
    uint base = materialOffset[matId];
    uint dst = base + groupBase + rankInGroup;

    PixelRef ref; //{ pixel.x, pixel.y, 0, 0 }; // pack xy
    ref.pixelXY = (pixel.x & 0xFFFFu) | ((pixel.y & 0xFFFFu) << 16);
    pixelList[dst] = ref;
}

// Indirect command record layout: 4 root constants + 3 dispatch args.
struct MaterialEvaluationIndirectArgs {
    // Root constants (all uints):
    uint materialId; // UintRootConstant0
    uint baseOffset; // UintRootConstant1
    uint count; // UintRootConstant2
    uint dispatchXDimension; // UintRootConstant3

    // Dispatch arguments:
    uint dispatchX; // D3D12_DISPATCH_ARGUMENTS.x
    uint dispatchY; // D3D12_DISPATCH_ARGUMENTS.y
    uint dispatchZ; // D3D12_DISPATCH_ARGUMENTS.z
};

struct TerrainRegionMaterialEvaluationIndirectArgs {
    uint terrainSetIndex;
    uint regionIndex;
    uint baseOffset;
    uint count;
    uint dispatchXDimension;
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

struct DispatchIndirectArgs {
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

float3 ReconstructWorldPositionFromVisibilityDepth(uint2 pixel, float linearDepth)
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

[shader("compute")]
[numthreads(1, 1, 1)]
void BuildTerrainRegionCommandBuildDispatchArgsCS(uint3 tid : SV_DispatchThreadID)
{
    StructuredBuffer<uint> activeCountBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionActiveCountBuffer)];
    RWStructuredBuffer<DispatchIndirectArgs> outArgs = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuildDispatchArgsBuffer)];

    uint activeRegionCount = activeCountBuffer[0];
    DispatchIndirectArgs args;
    args.dispatchX = max(1u, (activeRegionCount + 63u) / 64u);
    args.dispatchY = 1u;
    args.dispatchZ = 1u;
    outArgs[0] = args;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ClearTerrainRegionCountersCS(uint3 tid : SV_DispatchThreadID)
{
    uint regionCount = UintRootConstant0;
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionPixelCountBuffer)];
    RWStructuredBuffer<uint> cursors = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionWriteCursorBuffer)];
    RWStructuredBuffer<uint> activeCount = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionActiveCountBuffer)];

    if (tid.x < regionCount)
    {
        counts[tid.x] = 0u;
        cursors[tid.x] = 0u;
    }
    if (tid.x == 0u)
    {
        activeCount[0] = 0u;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRegionHistogramFromMaterialRangeCS(uint3 tid : SV_DispatchThreadID)
{
    uint baseOffset = IndirectCommandSignatureRootConstant1;
    uint count = IndirectCommandSignatureRootConstant2;
    uint dispatchXDimension = IndirectCommandSignatureRootConstant3;
    uint terrainSetIndex = UintRootConstant0;
    uint idx = tid.y * dispatchXDimension + tid.x;
    if (idx >= count)
    {
        return;
    }

    StructuredBuffer<PixelRef> pixelList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::PixelListBuffer)];
    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    RWStructuredBuffer<uint> regionCounts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionPixelCountBuffer)];
    RWStructuredBuffer<uint> activeRegions = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionActiveListBuffer)];
    RWStructuredBuffer<uint> activeCount = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionActiveCountBuffer)];

    PixelRef ref = pixelList[baseOffset + idx];
    uint2 pixel = uint2(ref.pixelXY & 0xFFFFu, ref.pixelXY >> 16);
    uint64_t vis = visibility[pixel];
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        return;
    }

    float depth;
    uint clusterIndex;
    uint primID;
    UnpackVisKey(vis, depth, clusterIndex, primID);
    uint regionIndex = TerrainRegionBinFromPosition(terrainSetIndex, ReconstructWorldPositionFromVisibilityDepth(pixel, depth));
    if (regionIndex == TERRAIN_INVALID_REGION_BIN)
    {
        return;
    }

    uint4 groupMask = WaveMatch(regionIndex);
    uint groupSize = CountBits128(groupMask);
    uint lane = WaveGetLaneIndex();
    uint leaderLane = GetWaveGroupLeaderLane(groupMask);

    uint oldCount = 0u;
    if (lane == leaderLane)
    {
        InterlockedAdd(regionCounts[regionIndex], groupSize, oldCount);
        if (oldCount == 0u)
        {
            uint activeSlot = 0u;
            InterlockedAdd(activeCount[0], 1u, activeSlot);
            if (activeSlot < TERRAIN_REGION_BIN_COUNT)
            {
                activeRegions[activeSlot] = regionIndex;
            }
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void TerrainRegionListFromMaterialRangeCS(uint3 tid : SV_DispatchThreadID)
{
    uint baseOffset = IndirectCommandSignatureRootConstant1;
    uint count = IndirectCommandSignatureRootConstant2;
    uint dispatchXDimension = IndirectCommandSignatureRootConstant3;
    uint terrainSetIndex = UintRootConstant0;
    uint idx = tid.y * dispatchXDimension + tid.x;
    if (idx >= count)
    {
        return;
    }

    StructuredBuffer<PixelRef> pixelList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::PixelListBuffer)];
    Texture2D<uint64_t> visibility = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PrimaryCamera::VisibilityTexture)];
    StructuredBuffer<uint> regionOffsets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionOffsetBuffer)];
    RWStructuredBuffer<uint> regionCursors = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionWriteCursorBuffer)];
    RWStructuredBuffer<PixelRef> terrainPixelList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionPixelListBuffer)];

    PixelRef ref = pixelList[baseOffset + idx];
    uint2 pixel = uint2(ref.pixelXY & 0xFFFFu, ref.pixelXY >> 16);
    uint64_t vis = visibility[pixel];
    if (vis == 0xFFFFFFFFFFFFFFFF)
    {
        return;
    }

    float depth;
    uint clusterIndex;
    uint primID;
    UnpackVisKey(vis, depth, clusterIndex, primID);
    uint regionIndex = TerrainRegionBinFromPosition(terrainSetIndex, ReconstructWorldPositionFromVisibilityDepth(pixel, depth));
    if (regionIndex == TERRAIN_INVALID_REGION_BIN)
    {
        return;
    }

    uint4 groupMask = WaveMatch(regionIndex);
    uint groupSize = CountBits128(groupMask);
    uint lane = WaveGetLaneIndex();
    uint leaderLane = GetWaveGroupLeaderLane(groupMask);

    uint groupBase = 0u;
    if (lane == leaderLane)
    {
        InterlockedAdd(regionCursors[regionIndex], groupSize, groupBase);
    }
    groupBase = WaveReadLaneAt(groupBase, leaderLane);

    uint rankInGroup = GetLaneRankInGroup(groupMask, lane);
    terrainPixelList[regionOffsets[regionIndex] + groupBase + rankInGroup] = ref;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void BuildTerrainRegionEvaluateIndirectArgsCS(uint3 tid : SV_DispatchThreadID)
{
    uint maxRegionCommands = UintRootConstant0;
    uint terrainSetIndex = UintRootConstant1;
    if (tid.x >= maxRegionCommands)
    {
        return;
    }

    StructuredBuffer<uint> activeCountBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionActiveCountBuffer)];
    StructuredBuffer<uint> activeRegions = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionActiveListBuffer)];
    StructuredBuffer<uint> counts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionPixelCountBuffer)];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionOffsetBuffer)];
    RWStructuredBuffer<TerrainRegionMaterialEvaluationIndirectArgs> outArgs = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::IndirectCommandBuffers::TerrainRegionMaterialEvaluationCommandBuffer)];

    uint activeRegionCount = activeCountBuffer[0];
    if (tid.x >= activeRegionCount)
    {
        outArgs[tid.x] = (TerrainRegionMaterialEvaluationIndirectArgs)0;
        return;
    }

    uint regionIndex = activeRegions[tid.x];
    uint count = counts[regionIndex];
    TerrainRegionMaterialEvaluationIndirectArgs args = (TerrainRegionMaterialEvaluationIndirectArgs)0;
    if (count != 0u)
    {
        uint groupsNeeded = (count + MATERIAL_EXECUTION_GROUP_SIZE - 1u) / MATERIAL_EXECUTION_GROUP_SIZE;
        const uint kMaxDim = 65535u;
        uint dispatchX = min((uint)ceil(sqrt((float)groupsNeeded)), kMaxDim);
        dispatchX = max(dispatchX, 1u);
        uint dispatchY = min((groupsNeeded + dispatchX - 1u) / dispatchX, kMaxDim);
        args.terrainSetIndex = terrainSetIndex;
        args.regionIndex = regionIndex;
        args.baseOffset = offsets[regionIndex];
        args.count = count;
        args.dispatchXDimension = dispatchX * MATERIAL_EXECUTION_GROUP_SIZE;
        args.dispatchX = dispatchX;
        args.dispatchY = dispatchY;
        args.dispatchZ = 1u;
    }
    outArgs[tid.x] = args;
}

// Build per-material indirect compute args.
// Inputs:
//  - counts[m] = number of pixels for material m
//  - offsets[m] = base offset into PixelListBuffer where this material's pixels start
// Root constants:
//  - UintRootConstant0 = NumMaterials
//  - UintRootConstant1 = PixelList SRV descriptor index
// Output:
//  - one ComputeIndirectArgs per material, written at index=materialId
[shader("compute")]
[numthreads(64, 1, 1)]
void BuildEvaluateIndirectArgsCS(uint3 dtid : SV_DispatchThreadID)
{
    uint numMaterials = UintRootConstant0;
    if (dtid.x >= numMaterials)
        return;

    StructuredBuffer<uint> counts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialPixelCountBuffer)];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialOffsetBuffer)];
    RWStructuredBuffer<MaterialEvaluationIndirectArgs> outArgs = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::IndirectCommandBuffers::MaterialEvaluationCommandBuffer)];

    uint materialId = dtid.x;
    uint count = counts[materialId];

    // If material has no pixels, write a zero-dispatch
    if (count == 0)
    {
        MaterialEvaluationIndirectArgs zero = (MaterialEvaluationIndirectArgs) 0;
        outArgs[materialId] = zero;
        return;
    }

    uint baseOffset = offsets[materialId];
    uint pixelListSrvIndex = UintRootConstant1;

    // We index the pixel list linearly; pick 2D dispatch that minimizes wasted threads.

    // Number of thread groups required overall (linear space):
    uint groupsNeeded = (count + MATERIAL_EXECUTION_GROUP_SIZE - 1u) / MATERIAL_EXECUTION_GROUP_SIZE;

    // Pick 2D dispatch that fits DX12 limits and keeps near-square shape
    const uint kMaxDim = 65535u;
    // Start from sqrt(groupsNeeded) for near-square tiling
    uint dispatchX = (uint) ceil(sqrt((float) groupsNeeded));
    if (dispatchX > kMaxDim) {
        dispatchX = kMaxDim;
    }
    uint dispatchY = (groupsNeeded + dispatchX - 1u) / dispatchX;
    if (dispatchY > kMaxDim) {
        dispatchY = kMaxDim; // With screen sizes in practice, this won't clamp
    }
    
    MaterialEvaluationIndirectArgs args;
    args.materialId = materialId;
    args.baseOffset = baseOffset;
    args.count = count;
    args.dispatchXDimension = dispatchX * MATERIAL_EXECUTION_GROUP_SIZE; // total threads in X

    args.dispatchX = dispatchX;
    args.dispatchY = dispatchY;
    args.dispatchZ = 1;

    outArgs[materialId] = args;
}

