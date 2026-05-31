#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/waveIntrinsicsHelpers.hlsli"
#include "include/visUtilCommon.hlsli"
#include "PerPassRootConstants/visUtilRootConstants.h"
#include "include/visibilityPacking.hlsli"
#include "include/visibleClusterPacking.hlsli"

uint GetMaterialIdFromCluster(uint clusterIndex,
                              ByteAddressBuffer visibleClusterBuffer,
                              StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue,
                              StructuredBuffer<PerMeshInstanceBuffer> perMeshInstance,
                              StructuredBuffer<PerMeshBuffer> perMeshBuffer)
{
    if (clusterIndex >= VISBUF_REYES_PATCH_INDEX_BASE && VISBUF_REYES_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        clusterIndex = diceQueue[clusterIndex - VISBUF_REYES_PATCH_INDEX_BASE].visibleClusterIndex;
    }

    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
    if (CLodVisibleClusterIsVoxel(packedCluster))
    {
        return VISBUF_VOXEL_MATERIAL_BIN_INDEX;
    }

    uint drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDraw(drawRecordIndex);
    PerMeshBuffer meshBuffer = perMeshBuffer[instanceData.perMeshBufferIndex];

    StructuredBuffer<MaterialInfo> materialDataBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[meshBuffer.materialDataIndex];

    return materialInfo.compileFlagsID;
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

