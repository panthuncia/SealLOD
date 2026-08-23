#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/vertex.hlsli"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodPageJobRasterShared.hlsli"
#include "include/visibleClusterPacking.hlsli"

#define SW_CLUSTER_RASTER_THREADS 32
#define DW_SKIN_CACHE_INVALID 0xFFFFFFFFu
static const uint DW_SKIN_CACHE_ENTRY_STRIDE = 36u;
static const uint DW_SKIN_CACHE_MAX_PROBES = 8u;
static const uint DW_COUNTER_ELIGIBLE = 267u;
static const uint DW_COUNTER_UNIQUE = 268u;
static const uint DW_COUNTER_DUPLICATE = 269u;
static const uint DW_COUNTER_REQUESTED_VERTICES = 270u;
static const uint DW_COUNTER_SKINNED_VERTICES = 271u;
static const uint DW_COUNTER_FALLBACK_VERTICES = 272u;
static const uint DW_COUNTER_POSITION_BYTES_USED = 277u;
static const uint DW_COUNTER_METADATA_BYTES_USED = 278u;

void DWTelemetryAdd(uint counter, uint value)
{
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == 0xFFFFFFFFu) return;
    RWStructuredBuffer<uint> telemetry =
        ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetry[counter], value);
}

void DWTelemetryMax(uint counter, uint value)
{
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == 0xFFFFFFFFu) return;
    RWStructuredBuffer<uint> telemetry =
        ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedMax(telemetry[counter], value);
}

uint DWHash(uint drawRecordIndex, uint assemblyTransformIndex, uint slabDescriptor,
    uint slabOffset, uint localMeshletIndex, uint skinningSlot)
{
    uint hash = 0x811C9DC5u;
    hash = (hash ^ drawRecordIndex) * 0x01000193u;
    hash = (hash ^ assemblyTransformIndex) * 0x01000193u;
    hash = (hash ^ slabDescriptor) * 0x01000193u;
    hash = (hash ^ slabOffset) * 0x01000193u;
    hash = (hash ^ localMeshletIndex) * 0x01000193u;
    hash = (hash ^ skinningSlot) * 0x01000193u;
    hash ^= hash >> 16u;
    hash *= 0x7FEB352Du;
    hash ^= hash >> 15u;
    return hash;
}

bool DWKeyMatches(RWByteAddressBuffer hashTable, uint offset, uint drawRecordIndex,
    uint assemblyTransformIndex, uint slabDescriptor, uint slabOffset,
    uint localMeshletIndex, uint skinningSlot)
{
    return hashTable.Load(offset + 4u) == drawRecordIndex &&
        hashTable.Load(offset + 8u) == assemblyTransformIndex &&
        hashTable.Load(offset + 12u) == slabDescriptor &&
        hashTable.Load(offset + 16u) == slabOffset &&
        hashTable.Load(offset + 20u) == localMeshletIndex &&
        hashTable.Load(offset + 24u) == skinningSlot;
}

bool DWLoadCluster(uint sortedClusterIndex, out uint4 packedCluster,
    out uint assemblyTransformIndex, out uint drawRecordIndex, out uint localMeshletIndex,
    out uint slabDescriptor, out uint slabOffset, out uint skinningSlot,
    out CLodPageHeader pageHeader, out CLodMeshletDescriptor meshletDescriptor,
    out CLodMeshMetadata metadata)
{
    packedCluster = 0u.xxxx;
    assemblyTransformIndex = DW_SKIN_CACHE_INVALID;
    drawRecordIndex = DW_SKIN_CACHE_INVALID;
    localMeshletIndex = 0u;
    slabDescriptor = 0u;
    slabOffset = 0u;
    skinningSlot = DW_SKIN_CACHE_INVALID;
    pageHeader = (CLodPageHeader)0;
    meshletDescriptor = (CLodMeshletDescriptor)0;
    metadata = (CLodMeshMetadata)0;
    ByteAddressBuffer clusters =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> assemblyTransforms =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    packedCluster = CLodLoadVisibleClusterPacked(clusters, sortedClusterIndex);
    assemblyTransformIndex = assemblyTransforms[sortedClusterIndex];
    drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    slabDescriptor = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    slabOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);

    const InstanceDrawRecordBuffer instanceDrawRecord = LoadInstanceDrawRecord(drawRecordIndex);
    const PerMeshInstanceBuffer instanceData = LoadMeshTemplateForDrawRecord(instanceDrawRecord);
    StructuredBuffer<PerMeshBuffer> meshes =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const PerMeshBuffer mesh = meshes[instanceData.perMeshBufferIndex];
    if ((mesh.vertexFlags & VERTEX_SKINNED) == 0u ||
        !IsValidSkinningInstanceSlot(instanceData.skinningInstanceSlot)) return false;
    const SkinningInstanceGPUInfo sourceInfo = LoadSkinningInstanceInfo(instanceData.skinningInstanceSlot);
    if ((sourceInfo.flags & 2u) == 0u) return false;
    StructuredBuffer<uint> dynamicWindVisibleMembership =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_VISIBLE_MEMBERSHIP_DESCRIPTOR_INDEX];
    if (dynamicWindVisibleMembership[instanceDrawRecord.instanceTransformIndex] == 0u) return false;
    skinningSlot = ResolveAssemblyProceduralWindSkinningSlot(
        drawRecordIndex, instanceData.skinningInstanceSlot, assemblyTransformIndex);
    if (!IsValidSkinningInstanceSlot(skinningSlot) ||
        LoadSkinningInstanceInfo(skinningSlot).boneCount == 0u) return false;

    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(drawRecordIndex);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    pageHeader = LoadPageHeader(slabDescriptor, slabOffset);
    meshletDescriptor = LoadMeshletDescriptor(
        slabDescriptor, slabOffset, pageHeader.descriptorOffset, localMeshletIndex);
    return true;
}

bool DWFindEntry(RWByteAddressBuffer hashTable, uint hash, uint drawRecordIndex,
    uint assemblyTransformIndex, uint slabDescriptor, uint slabOffset,
    uint localMeshletIndex, uint skinningSlot, out uint entryOffset)
{
    const uint entryCount = CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT;
    const uint readyState = (CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION << 1u) | 1u;
    [unroll] for (uint probe = 0u; probe < DW_SKIN_CACHE_MAX_PROBES; ++probe) {
        entryOffset = ((hash + probe) % entryCount) * DW_SKIN_CACHE_ENTRY_STRIDE;
        if (hashTable.Load(entryOffset) == readyState &&
            DWKeyMatches(hashTable, entryOffset, drawRecordIndex, assemblyTransformIndex,
                slabDescriptor, slabOffset, localMeshletIndex, skinningSlot)) return true;
    }
    entryOffset = 0u;
    return false;
}

bool DWResolveDispatchCluster(uint3 groupId, out uint sortedClusterIndex)
{
    StructuredBuffer<uint> histogram =
        ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketId = IndirectCommandSignatureRootConstant2;
    const uint linearizedGroup = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    const uint clusterIndex = linearizedGroup / SW_RASTER_GROUPS_PER_CLUSTER;
    if ((linearizedGroup % SW_RASTER_GROUPS_PER_CLUSTER) != 0u || clusterIndex >= histogram[bucketId]) {
        sortedClusterIndex = 0u;
        return false;
    }
    sortedClusterIndex = IndirectCommandSignatureRootConstant0 + clusterIndex;
    return true;
}

[numthreads(SW_CLUSTER_RASTER_THREADS, 1, 1)]
void DynamicWindSkinCacheBuildCS(uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    uint sortedClusterIndex = 0u;
    if (!DWResolveDispatchCluster(groupId, sortedClusterIndex)) return;
    RWStructuredBuffer<uint> mapping =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX];
    if (GI == 0u) mapping[sortedClusterIndex] = DW_SKIN_CACHE_INVALID;
    if (GI != 0u) return;

    uint4 packedCluster;
    uint assemblyTransformIndex, drawRecordIndex, localMeshletIndex, slabDescriptor, slabOffset, skinningSlot;
    CLodPageHeader pageHeader;
    CLodMeshletDescriptor meshletDescriptor;
    CLodMeshMetadata metadata;
    if (!DWLoadCluster(sortedClusterIndex, packedCluster, assemblyTransformIndex,
        drawRecordIndex, localMeshletIndex, slabDescriptor, slabOffset, skinningSlot,
        pageHeader, meshletDescriptor, metadata)) return;
    DWTelemetryAdd(DW_COUNTER_ELIGIBLE, 1u);
    const uint vertexCount = CLodDescVertexCount(meshletDescriptor);
    DWTelemetryAdd(DW_COUNTER_REQUESTED_VERTICES, vertexCount);
    RWByteAddressBuffer hashTable =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX];
    const uint hash = DWHash(drawRecordIndex, assemblyTransformIndex, slabDescriptor,
        slabOffset, localMeshletIndex, skinningSlot);
    uint entryOffset = 0u;
    if (DWFindEntry(hashTable, hash, drawRecordIndex, assemblyTransformIndex,
        slabDescriptor, slabOffset, localMeshletIndex, skinningSlot, entryOffset)) {
        DWTelemetryAdd(DW_COUNTER_DUPLICATE, 1u);
        return;
    }

    const uint writingState = CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION << 1u;
    const uint readyState = writingState | 1u;
    const uint entryCount = CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT;
    [unroll] for (uint probe = 0u; probe < DW_SKIN_CACHE_MAX_PROBES; ++probe) {
        entryOffset = ((hash + probe) % entryCount) * DW_SKIN_CACHE_ENTRY_STRIDE;
        const uint oldState = hashTable.Load(entryOffset);
        if ((oldState >> 1u) == CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION) {
            // A ready exact match was handled above.  A writing entry cannot be
            // inspected safely, so never wait: let the later resolve pass find
            // it, or leave this cluster on the inline fallback path.
            return;
        }
        uint observed = 0u;
        hashTable.InterlockedCompareExchange(entryOffset, oldState, writingState, observed);
        if (observed != oldState) continue;
        RWStructuredBuffer<uint> allocator =
            ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX];
        uint positionBase = 0u;
        InterlockedAdd(allocator[0], vertexCount, positionBase);
        if (positionBase > CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY ||
            vertexCount > CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY - positionBase) {
            positionBase = DW_SKIN_CACHE_INVALID;
            DWTelemetryAdd(DW_COUNTER_FALLBACK_VERTICES, vertexCount);
        }
        hashTable.Store(entryOffset + 4u, drawRecordIndex);
        hashTable.Store(entryOffset + 8u, assemblyTransformIndex);
        hashTable.Store(entryOffset + 12u, slabDescriptor);
        hashTable.Store(entryOffset + 16u, slabOffset);
        hashTable.Store(entryOffset + 20u, localMeshletIndex);
        hashTable.Store(entryOffset + 24u, skinningSlot);
        hashTable.Store(entryOffset + 28u, positionBase);
        hashTable.Store(entryOffset + 32u, sortedClusterIndex);
        DeviceMemoryBarrier();
        uint replaced = 0u;
        hashTable.InterlockedExchange(entryOffset, readyState, replaced);
        if (positionBase != DW_SKIN_CACHE_INVALID) {
            uint workIndex = 0u;
            InterlockedAdd(allocator[1], 1u, workIndex);
            if (workIndex < entryCount) {
                RWStructuredBuffer<uint> workRecords =
                    ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX];
                workRecords[workIndex] = entryOffset;
            }
        }
        DWTelemetryAdd(DW_COUNTER_UNIQUE, 1u);
        return;
    }
    DWTelemetryAdd(DW_COUNTER_FALLBACK_VERTICES, vertexCount);
}

[numthreads(1, 1, 1)]
void DynamicWindSkinCacheFinalizeCS()
{
    RWStructuredBuffer<uint> allocator =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX];
    RWByteAddressBuffer indirectArgs =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_INDIRECT_ARGS_DESCRIPTOR_INDEX];
    indirectArgs.Store(0u, min(allocator[1], CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT));
    indirectArgs.Store(4u, 1u);
    indirectArgs.Store(8u, 1u);
    DWTelemetryMax(
        DW_COUNTER_POSITION_BYTES_USED,
        min(allocator[0], CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITION_CAPACITY) * 12u);
    DWTelemetryMax(
        DW_COUNTER_METADATA_BYTES_USED,
        min(allocator[1], CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT) * 40u);
}

[numthreads(SW_CLUSTER_RASTER_THREADS, 1, 1)]
void DynamicWindSkinCacheSkinWorkCS(uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    const uint workIndex = groupId.x;
    RWStructuredBuffer<uint> allocator =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_ALLOCATOR_DESCRIPTOR_INDEX];
    if (workIndex >= min(allocator[1], CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_ENTRY_COUNT)) return;
    StructuredBuffer<uint> workRecords =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_WORK_RECORDS_DESCRIPTOR_INDEX];
    const uint entryOffset = workRecords[workIndex];
    RWByteAddressBuffer hashTable =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX];
    const uint readyState = (CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_GENERATION << 1u) | 1u;
    if (hashTable.Load(entryOffset) != readyState) return;
    const uint sortedClusterIndex = hashTable.Load(entryOffset + 32u);
    uint4 packedCluster;
    uint assemblyTransformIndex, drawRecordIndex, localMeshletIndex, slabDescriptor, slabOffset, skinningSlot;
    CLodPageHeader pageHeader;
    CLodMeshletDescriptor meshletDescriptor;
    CLodMeshMetadata metadata;
    if (!DWLoadCluster(sortedClusterIndex, packedCluster, assemblyTransformIndex,
        drawRecordIndex, localMeshletIndex, slabDescriptor, slabOffset, skinningSlot,
        pageHeader, meshletDescriptor, metadata)) return;

    const uint positionBase = hashTable.Load(entryOffset + 28u);
    if (positionBase == DW_SKIN_CACHE_INVALID) return;

    const uint vertexCount = CLodDescVertexCount(meshletDescriptor);
    RWStructuredBuffer<float3> positions =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX];
    const uint positionBitstreamBase = slabOffset + pageHeader.positionBitstreamOffset;
    for (uint vertex = GI; vertex < vertexCount; vertex += SW_CLUSTER_RASTER_THREADS) {
        float3 localPosition = PJ_DecodeCompressedPosition(
            vertex, positionBitstreamBase, meshletDescriptor.positionBitOffset,
            CLodDescBitsX(meshletDescriptor), CLodDescBitsY(meshletDescriptor),
            CLodDescBitsZ(meshletDescriptor), pageHeader.compressedPositionQuantExp,
            int3(meshletDescriptor.minQx, meshletDescriptor.minQy, meshletDescriptor.minQz),
            slabDescriptor);
        SkinningInfluences skinning = PJ_DecodePackedJoints(
            vertex, pageHeader, meshletDescriptor, slabOffset, slabDescriptor);
        skinning = PJ_DecodePackedWeights(
            vertex, pageHeader, meshletDescriptor, slabOffset, slabDescriptor, skinning);
        skinning = ResolveAssemblySkinningInfluences(skinning, metadata, assemblyTransformIndex);
        positions[positionBase + vertex] = ApplyAssemblySkinningToPosition(
            skinningSlot, skinning, assemblyTransformIndex, localPosition);
    }
    if (GI == 0u) DWTelemetryAdd(DW_COUNTER_SKINNED_VERTICES, vertexCount);
}

[numthreads(SW_CLUSTER_RASTER_THREADS, 1, 1)]
void DynamicWindSkinCacheResolveCS(uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    uint sortedClusterIndex = 0u;
    if (!DWResolveDispatchCluster(groupId, sortedClusterIndex) || GI != 0u) return;
    uint4 packedCluster;
    uint assemblyTransformIndex, drawRecordIndex, localMeshletIndex, slabDescriptor, slabOffset, skinningSlot;
    CLodPageHeader pageHeader;
    CLodMeshletDescriptor meshletDescriptor;
    CLodMeshMetadata metadata;
    if (!DWLoadCluster(sortedClusterIndex, packedCluster, assemblyTransformIndex,
        drawRecordIndex, localMeshletIndex, slabDescriptor, slabOffset, skinningSlot,
        pageHeader, meshletDescriptor, metadata)) return;
    RWByteAddressBuffer hashTable =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_HASH_DESCRIPTOR_INDEX];
    const uint hash = DWHash(drawRecordIndex, assemblyTransformIndex, slabDescriptor,
        slabOffset, localMeshletIndex, skinningSlot);
    uint entryOffset = 0u;
    if (!DWFindEntry(hashTable, hash, drawRecordIndex, assemblyTransformIndex,
        slabDescriptor, slabOffset, localMeshletIndex, skinningSlot, entryOffset)) return;
    RWStructuredBuffer<uint> mapping =
        ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX];
    mapping[sortedClusterIndex] = hashTable.Load(entryOffset + 28u);
}
