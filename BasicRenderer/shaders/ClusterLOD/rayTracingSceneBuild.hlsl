// Builds the ray tracing scene from the RT view's cluster list:
//   CLodRtBuildClasArgsCS: argument records for groups whose CLAS were just requested
//   Count -> Allocate -> AllocateFinish -> Place: groups the unordered cluster list by instance, one CLAS address array per BLAS
//   CLodRtWriteInstancesCS: TLAS instances, once the BLAS addresses are known
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/materialFlags.hlsli"
#include "include/vertex.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/rhiRtasArgs.hlsli"
#include "include/clodRayTracingScene.hlsli"
#include "include/clodRayTracingAlpha.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"

[numthreads(256, 1, 1)]
void CLodRtClearCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLOD_RT_CLEAR_WORD_COUNT) {
        return;
    }
    RWByteAddressBuffer target = ResourceDescriptorHeap[CLOD_RT_CLEAR_UAV];
    target.Store(id.x * 4u, 0u);
}

// ---- CLAS arguments

struct CLodRtBuildSource
{
    uint64_t slabAddress;
    uint pageByteOffset;
    uint slabDescriptor;
    uint pageOffsetUnits;
    uint firstMeshlet;
    uint meshletCount;
    uint firstBuildIndex;
    uint firstClasSlot;
    uint groupLocalIndex;
    uint ommKey; // The identity of the material the source's micromaps were baked for. Zero without micromaps.
    uint ommIndexUnit; // Its first cluster's indices in the index pool, in CLOD_RT_OMM_INDEX_UNIT_BYTES.
    uint64_t ommArray; // Null when every triangle took a special index.
    uint2 padding;
};

[numthreads(16, 1, 1)]
void CLodRtBuildClasArgsCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLOD_RT_BUILD_SOURCE_COUNT) {
        return;
    }
    StructuredBuffer<CLodRtBuildSource> sources = ResourceDescriptorHeap[CLOD_RT_BUILD_SOURCES_SRV];
    RWStructuredBuffer<RtasClusterBuildTriangleInfo> args = ResourceDescriptorHeap[CLOD_RT_BUILD_ARGS_UAV];
    RWStructuredBuffer<uint64_t> destinations = ResourceDescriptorHeap[CLOD_RT_BUILD_DESTINATIONS_UAV];
    RWStructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_BUILD_CLAS_INFO_UAV];
    RWStructuredBuffer<uint64_t> clasAddresses = ResourceDescriptorHeap[CLOD_RT_BUILD_CLAS_ADDRESSES_UAV];

    const CLodRtBuildSource source = sources[CLOD_RT_BUILD_SOURCE_FIRST + id.x];
    const CLodPageHeader header = LoadPageHeader(source.slabDescriptor, source.pageByteOffset);
    const uint64_t pageAddress = source.slabAddress + source.pageByteOffset;

    for (uint i = 0u; i < source.meshletCount; ++i) {
        const uint localMeshlet = source.firstMeshlet + i;
        const uint buildIndex = source.firstBuildIndex + i;
        const uint slot = source.firstClasSlot + i;

        // Measuring sizes leaves these alone, since the slot may not be built for a few frames.
        if (CLOD_RT_BUILD_PUBLISH != 0u) {
            clasAddresses[slot] = destinations[buildIndex];
            RWStructuredBuffer<uint> slotOmmKeys = ResourceDescriptorHeap[CLOD_RT_BUILD_SLOT_OMM_KEYS_UAV];
            slotOmmKeys[slot] = source.ommKey;
            CLodRtClasInfo info;
            info.slabDescriptor = source.slabDescriptor;
            info.pageOffsetUnits = source.pageOffsetUnits;
            info.localMeshlet = localMeshlet;
            info.groupLocalIndex = source.groupLocalIndex;
            clasInfo[slot] = info;
        }

        // An empty cluster is a valid CLAS: the slot stays usable if this page isn't a triangle page.
        uint triangleCount = 0u;
        uint vertexCount = 0u;
        uint64_t vertexBuffer = 0ull;
        uint64_t indexBuffer = 0ull;
        if (header.formatAndKind == CLOD_TRIANGLE_PAGE_MAGIC
            && header.compressedPositionQuantExp == CLOD_POSITION_FORMAT_FLOAT3
            && localMeshlet < header.meshletCount) {
            const CLodMeshletDescriptor descriptor = LoadMeshletDescriptor(
                source.slabDescriptor, source.pageByteOffset, header.descriptorOffset, localMeshlet);
            triangleCount = descriptor.triangleCountAndRefinedGroup & 0xFFFFu;
            vertexCount = (descriptor.bitsAndVertexCount >> 24u) & 0xFFu;
            vertexBuffer = pageAddress + header.positionBitstreamOffset + descriptor.positionBitOffset;
            indexBuffer = pageAddress + header.triangleStreamOffset + descriptor.triangleByteOffset;
        }

        // Opacity is decided per instance with FORCE_OPAQUE: a page can be shared by meshes with different materials.
        // Micromaps belong to one material, so instances of any other disable them (CLodRtWriteInstancesCS).
        RtasClusterBuildTriangleInfo info = RtasMakeClusterBuildTriangleInfo(
            slot, triangleCount, vertexCount, RTAS_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT,
            CLOD_POSITION_FORMAT_FLOAT3_STRIDE_BYTES, 1u, vertexBuffer, indexBuffer);
        if (source.ommKey != 0u && triangleCount != 0u) {
            const uint64_t indexPool = uint64_t(CLOD_RT_BUILD_OMM_INDEX_POOL_LOW) | (uint64_t(CLOD_RT_BUILD_OMM_INDEX_POOL_HIGH) << 32u);
            const uint64_t ommIndices = indexPool + uint64_t(source.ommIndexUnit + i) * CLOD_RT_OMM_INDEX_UNIT_BYTES;
            RtasClusterBuildTriangleInfoSetOpacityMicromaps(info, source.ommArray, ommIndices, 4u, 0u, true);
        }
        args[CLOD_RT_BUILD_FIRST_ARG + buildIndex] = info;
    }
}

// ---- Group the cluster list by instance

uint CLodRtClusterCount()
{
    StructuredBuffer<uint> counter = ResourceDescriptorHeap[CLOD_RT_SCENE_CLUSTER_COUNTER_SRV];
    return min(counter[0], CLOD_RT_SCENE_CLUSTER_CAPACITY);
}

uint4 CLodRtLoadCluster(uint index)
{
    ByteAddressBuffer clusters = ResourceDescriptorHeap[CLOD_RT_SCENE_CLUSTERS_SRV];
    return clusters.Load4(index * 16u);
}

// Draw table, per draw record: a 64-bit [cluster count : 20, signature : 44] updated with one atomic per cluster, then the
// [end, cursor] of the range this draw fills in, which is empty for a draw that shares another's BLAS.
#define CLOD_RT_DRAW_ENTRY_BYTES 16u
#define CLOD_RT_DRAW_COUNT_BITS 20u

uint CLodRtMix(uint value)
{
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    return value ^ (value >> 16u);
}

// Puts last frame's touched entries back to zero, which is far cheaper than clearing a table this size.
[numthreads(64, 1, 1)]
void CLodRtResetDrawsCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    if (id.x >= min(counters.Load(CLOD_RT_COUNTER_INSTANCES * 4u), CLOD_RT_SCENE_INSTANCE_CAPACITY)) {
        return;
    }
    RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    drawTable.Store4(instanceList[id.x] * CLOD_RT_DRAW_ENTRY_BYTES, uint4(0u, 0u, 0u, 0u));
}

[numthreads(256, 1, 1)]
void CLodRtCountCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLodRtClusterCount()) {
        return;
    }
    const uint4 packed = CLodRtLoadCluster(id.x);
    const uint draw = CLodVisibleClusterInstanceID(packed);
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    if (draw >= CLOD_RT_SCENE_DRAW_TABLE_ENTRIES) {
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_CLUSTERS * 4u, 1u);
        return;
    }
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];

    // A signature of which clusters this draw selected, independent of the order they were appended in. It is built
    // from the page location, so two draws agree exactly when they reference the same CLAS.
    const uint clusterKey = CLodVisibleClusterLocalMeshletIndex(packed)
        | (CLodVisibleClusterPageSlabDescriptorIndex(packed) << 14u);
    const uint pageKey = CLodVisibleClusterPageSlabByteOffset(packed) >> 14u;
    const uint mixed = CLodRtMix(clusterKey ^ CLodRtMix(pageKey));
    const uint64_t signature = (uint64_t(CLodRtMix(mixed + 0x9E3779B9u) & 0xFFFu) << 32u) | uint64_t(mixed);
    uint64_t previous = 0ull;
    drawTable.InterlockedAdd64(draw * CLOD_RT_DRAW_ENTRY_BYTES, (signature << CLOD_RT_DRAW_COUNT_BITS) + 1ull, previous);
    if (previous == 0ull) {
        uint instance = 0u;
        counters.InterlockedAdd(CLOD_RT_COUNTER_INSTANCES * 4u, 1u, instance);
        if (instance < CLOD_RT_SCENE_INSTANCE_CAPACITY) {
            RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
            instanceList[instance] = draw;
        }
        else {
            counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_INSTANCES * 4u, 1u);
        }
    }
}

// One thread per instance. The share table key is the draw's whole [signature : 44, count : 20], claimed with one atomic,
// so the slot identifies the BLAS before its index is known. Consumers record the slot and resolve it when writing instances.
[numthreads(64, 1, 1)]
void CLodRtAllocateCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    if (id.x >= min(counters.Load(CLOD_RT_COUNTER_INSTANCES * 4u), CLOD_RT_SCENE_INSTANCE_CAPACITY)) {
        return;
    }
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
    RWStructuredBuffer<RtasClusterBuildBottomLevelInfo> blasArgs = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_ARGS_UAV];
    RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
    RWByteAddressBuffer shareTable = ResourceDescriptorHeap[CLOD_RT_SCENE_SHARE_TABLE_UAV];
    const uint64_t addressArray = uint64_t(CLOD_RT_SCENE_ADDRESS_ARRAY_LOW) | (uint64_t(CLOD_RT_SCENE_ADDRESS_ARRAY_HIGH) << 32u);

    const uint draws = instanceList[id.x] * CLOD_RT_DRAW_ENTRY_BYTES;
    const uint64_t key = drawTable.Load<uint64_t>(draws);
    const uint wanted = uint(key & ((1ull << CLOD_RT_DRAW_COUNT_BITS) - 1ull));
    drawTable.Store(draws, id.x + 1u); // From here on, what instances that skipped traversal need: this draw's row.

    const uint mask = CLOD_RT_SCENE_SHARE_TABLE_ENTRIES - 1u;
    uint slot = CLodRtMix(uint(key) ^ uint(key >> 32u)) & mask;
    bool claimed = false;
    for (uint probe = 0u; probe < 64u; ++probe) {
        uint64_t original = 0ull;
        shareTable.InterlockedCompareExchange64(slot * 16u, 0ull, key, original);
        if (original == 0ull) {
            claimed = true;
            break;
        }
        if (original == key) {
            instanceBlas[id.x] = CLOD_RT_INSTANCE_BLAS_SHARED | slot;
            return; // Its range stays empty: nothing for this draw to place.
        }
        slot = (slot + 1u) & mask;
    }

    // Both counters can overshoot their capacity here, and CLodRtAllocateFinishCS clamps them.
    uint blasIndex = 0u;
    counters.InterlockedAdd(CLOD_RT_COUNTER_BLAS_ARGS * 4u, 1u, blasIndex);
    if (blasIndex >= CLOD_RT_SCENE_BLAS_CAPACITY) {
        instanceBlas[id.x] = 0xFFFFFFFFu; // Inactive this frame, and so is anything sharing this slot.
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_BLASES * 4u, 1u);
        return;
    }
    const uint capped = min(wanted, CLOD_RT_SCENE_MAX_CLUSTERS_PER_INSTANCE);
    uint base = 0u;
    counters.InterlockedAdd(CLOD_RT_COUNTER_CLUSTERS * 4u, capped, base);
    const uint available = base < CLOD_RT_SCENE_REFERENCE_CAPACITY ? CLOD_RT_SCENE_REFERENCE_CAPACITY - base : 0u;
    const uint count = min(capped, available);
    if (count != wanted) {
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_BLASES * 4u, 1u);
    }
    // Every index below the final count must hold valid arguments, so this one is written even if it ended up empty.
    RtasClusterBuildBottomLevelInfo args;
    args.clusterReferencesCount = count;
    args.clusterReferencesStride = 8u;
    args.clusterReferences = addressArray + uint64_t(base) * 8ull;
    blasArgs[blasIndex] = args;
    instanceBlas[id.x] = count != 0u ? blasIndex : 0xFFFFFFFFu;
    // Only a whole selection can be shared: a truncated one differs from what its key says.
    if (claimed && count == wanted) {
        shareTable.Store((slot * 16u) + 12u, blasIndex + 1u);
    }
    if (count != 0u) {
        drawTable.Store2(draws + 8u, uint2(base + count, base));
    }
}

// Instances that took the coarsest level from another draw of the mesh without being traversed.
[numthreads(64, 1, 1)]
void CLodRtAddCoarseConsumersCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer coarse = ResourceDescriptorHeap[CLOD_RT_SCENE_COARSE_CUT_UAV];
    if (id.x >= min(coarse.Load(0u), CLOD_RT_COARSE_CONSUMER_CAPACITY)) {
        return;
    }
    const uint2 pair = coarse.Load2(16u + id.x * 8u);
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    const uint providerRow = drawTable.Load(pair.y * CLOD_RT_DRAW_ENTRY_BYTES);
    if (providerRow == 0u) {
        return; // Nothing of this mesh was selected, which means nothing it needs has a CLAS yet.
    }
    uint row = 0u;
    counters.InterlockedAdd(CLOD_RT_COUNTER_INSTANCES * 4u, 1u, row);
    if (row >= CLOD_RT_SCENE_INSTANCE_CAPACITY) {
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_INSTANCES * 4u, 1u);
        return;
    }
    RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
    RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
    instanceList[row] = pair.x;
    instanceBlas[row] = CLOD_RT_INSTANCE_BLAS_VIA_ROW | (providerRow - 1u);
    counters.InterlockedAdd(CLOD_RT_COUNTER_COARSE_CONSUMERS * 4u, 1u);
}

[numthreads(1, 1, 1)]
void CLodRtAllocateFinishCS()
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    counters.Store(CLOD_RT_COUNTER_BLAS_ARGS * 4u, min(counters.Load(CLOD_RT_COUNTER_BLAS_ARGS * 4u), CLOD_RT_SCENE_BLAS_CAPACITY));
    counters.Store(CLOD_RT_COUNTER_CLUSTERS * 4u, min(counters.Load(CLOD_RT_COUNTER_CLUSTERS * 4u), CLOD_RT_SCENE_REFERENCE_CAPACITY));
    counters.Store(CLOD_RT_COUNTER_TLAS_INSTANCES * 4u, min(counters.Load(CLOD_RT_COUNTER_INSTANCES * 4u), CLOD_RT_SCENE_INSTANCE_CAPACITY));
    counters.Store(CLOD_RT_COUNTER_LIST_CLUSTERS * 4u, CLodRtClusterCount());
}

[numthreads(256, 1, 1)]
void CLodRtPlaceCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLodRtClusterCount()) {
        return;
    }
    const uint4 packed = CLodRtLoadCluster(id.x);
    const uint draw = CLodVisibleClusterInstanceID(packed);
    if (draw >= CLOD_RT_SCENE_DRAW_TABLE_ENTRIES) {
        return;
    }
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    const uint draws = draw * CLOD_RT_DRAW_ENTRY_BYTES;
    const uint end = drawTable.Load(draws + 8u);
    if (end == 0u) {
        return; // Shares another draw's BLAS, which is most of them.
    }
    uint element = 0u;
    drawTable.InterlockedAdd(draws + 12u, 1u, element);
    if (element >= end) {
        return;
    }

    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDrawRecord(LoadInstanceDrawRecord(draw));
    StructuredBuffer<CLodMeshMetadata> metadata = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const uint groupGlobal = metadata[offsets.clodMeshMetadataIndex].groupsBase + CLodVisibleClusterGroupID(packed);
    const uint slot = CLodRtFindClasSlot(
        CLOD_RT_SCENE_GROUP_TABLE_SRV,
        CLOD_RT_SCENE_SEGMENT_TABLE_SRV,
        groupGlobal,
        CLodVisibleClusterPageSlabDescriptorIndex(packed),
        CLodVisibleClusterPageSlabByteOffset(packed) >> 14u,
        CLodVisibleClusterLocalMeshletIndex(packed));

    // A null reference is ignored by the build: this happens if a CLAS was freed after culling selected it.
    uint64_t address = 0ull;
    if (slot != CLOD_RT_INVALID_SLOT) {
        RWStructuredBuffer<uint64_t> clasAddresses = ResourceDescriptorHeap[CLOD_RT_SCENE_CLAS_ADDRESSES_UAV];
        address = clasAddresses[slot];
        // Which material the BLAS's micromaps were baked for, so instances of another can disable them.
        RWStructuredBuffer<uint> slotOmmKeys = ResourceDescriptorHeap[CLOD_RT_SCENE_SLOT_OMM_KEYS_UAV];
        const uint ommKey = slotOmmKeys[slot];
        if (ommKey != 0u) {
            RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
            const uint blas = instanceBlas[drawTable.Load(draws) - 1u];
            if (blas < CLOD_RT_SCENE_BLAS_CAPACITY) {
                RWStructuredBuffer<uint> blasOmmKeys = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_OMM_KEYS_UAV];
                uint previous = 0u;
                InterlockedCompareExchange(blasOmmKeys[blas], 0u, ommKey, previous);
                if (previous != 0u && previous != ommKey) {
                    InterlockedOr(blasOmmKeys[blas], CLOD_RT_OMM_KEY_CONFLICT);
                }
            }
        }
    }
    RWStructuredBuffer<uint64_t> addressArray = ResourceDescriptorHeap[CLOD_RT_SCENE_ADDRESS_ARRAY_UAV];
    addressArray[element] = address;
}

// ---- TLAS instances

[numthreads(64, 1, 1)]
void CLodRtWriteInstancesCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLOD_RT_SCENE_TLAS_INSTANCE_COUNT) {
        return;
    }
    RWStructuredBuffer<RtasInstanceDesc> instances = ResourceDescriptorHeap[CLOD_RT_SCENE_TLAS_INSTANCES_UAV];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];

    RtasInstanceDesc desc = (RtasInstanceDesc)0; // A null acceleration structure makes the row inactive.
    if (id.x < counters.Load(CLOD_RT_COUNTER_TLAS_INSTANCES * 4u)) {
        StructuredBuffer<uint64_t> blasAddresses = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_ADDRESSES_SRV];
        RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
        RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
        const uint draw = instanceList[id.x];
        const InstanceDrawRecordBuffer record = LoadInstanceDrawRecord(draw);
        const PerObjectBuffer transform = LoadInstanceTransformForDrawRecordWithAssemblyTransform(record, CLOD_ASSEMBLY_TRANSFORM_SENTINEL);
        const row_major matrix model = transform.model;

        // Row-vector model matrix to a 3x4 of column-vector rows.
        for (uint row = 0u; row < 3u; ++row) {
            desc.transformRows[row] = float4(model[0][row], model[1][row], model[2][row], model[3][row]);
        }

        StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshBuffer perMesh = perMeshBuffer[LoadMeshTemplateForDrawRecord(record).perMeshBufferIndex];
        StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
        const uint materialFlags = materials[perMesh.materialDataIndex].materialFlags;

        // Raster's counter-clockwise front faces are clockwise here, because its projection flips Y. Mirrored objects swap.
        uint flags = (transform.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u ? RTAS_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE : 0u;
        if ((materialFlags & MATERIAL_DOUBLE_SIDED) != 0u) {
            flags |= RTAS_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        }
        if ((materialFlags & MATERIAL_ALPHA_TEST) == 0u) {
            flags |= RTAS_INSTANCE_FLAG_FORCE_OPAQUE;
        }
        desc.instanceIDAndMask = (draw & 0x00FFFFFFu) | (0xFFu << 24u);
        // Selects the hit groups of pipelines that trace this BVH. See pathTrace.lib.hlsl. Ray queries ignore it.
        const uint hitGroupContribution = (materialFlags & MATERIAL_TERRAIN) != 0u ? 2u : 0u;
        desc.contributionAndFlags = (flags << 24u) | hitGroupContribution;
        uint blas = instanceBlas[id.x];
        if (blas != CLOD_RT_INSTANCE_BLAS_NONE && (blas & CLOD_RT_INSTANCE_BLAS_VIA_ROW) != 0u) {
            blas = instanceBlas[blas & ~CLOD_RT_INSTANCE_BLAS_VIA_ROW];
        }
        if (blas != CLOD_RT_INSTANCE_BLAS_NONE && (blas & CLOD_RT_INSTANCE_BLAS_SHARED) != 0u) {
            RWByteAddressBuffer shareTable = ResourceDescriptorHeap[CLOD_RT_SCENE_SHARE_TABLE_UAV];
            const uint owner = shareTable.Load(((blas & ~CLOD_RT_INSTANCE_BLAS_SHARED) * 16u) + 12u);
            blas = owner != 0u ? owner - 1u : 0xFFFFFFFFu;
        }
        desc.accelerationStructure = blas != 0xFFFFFFFFu ? blasAddresses[blas] : 0ull;

        // Every alpha-tested instance in the BVH asks for its test mip. Without that, foliage only secondary rays reach would
        // never get there, and baked foliage would drift back down, since rays that micromaps answer run no any-hit to ask.
        if (CLOD_RT_SCENE_OMM_ACTIVE != 0u && blas != 0xFFFFFFFFu && (materialFlags & MATERIAL_ALPHA_TEST) != 0u) {
            counters.InterlockedAdd(CLOD_RT_COUNTER_ALPHA_INSTANCES * 4u, 1u);
            const MaterialInfo alphaMaterial = materials[perMesh.materialDataIndex];
            if ((materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u && alphaMaterial.baseColorStreamingTextureID != 0u) {
                const CLodRtAlphaImage image = CLodRtResolveAlphaImage(alphaMaterial.baseColorTextureIndex, alphaMaterial.baseColorStreamingTextureID);
                RecordTextureStreamingFeedbackRequestedTopMip(LoadTextureStreamingInfo(alphaMaterial.baseColorStreamingTextureID), alphaMaterial.baseColorStreamingTextureID, image.absoluteMip);
            }
            if ((materialFlags & MATERIAL_OPACITY_TEXTURE) != 0u && alphaMaterial.opacityStreamingTextureID != 0u) {
                const CLodRtAlphaImage image = CLodRtResolveAlphaImage(alphaMaterial.opacityTextureIndex, alphaMaterial.opacityStreamingTextureID);
                RecordTextureStreamingFeedbackRequestedTopMip(LoadTextureStreamingInfo(alphaMaterial.opacityStreamingTextureID), alphaMaterial.opacityStreamingTextureID, image.absoluteMip);
            }
        }

        // Micromaps were baked offline against one material's alpha test at its test mip (the material's identity). An
        // instance drawn with any other material, or by BLAS sharing with clusters of several, tests alpha in any-hit
        // instead, which gives the same answer. Residency doesn't matter: a known micro-triangle's state is the test
        // mip's, and any-hit decides only the unknown ones, at whatever mip has streamed in.
        if (blas != 0xFFFFFFFFu) {
            RWStructuredBuffer<uint> blasOmmKeys = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_OMM_KEYS_UAV];
            const uint ommKey = blasOmmKeys[blas];
            if (ommKey != 0u) {
                const bool usable = CLOD_RT_SCENE_OMM_ACTIVE != 0u && (materialFlags & MATERIAL_ALPHA_TEST) != 0u
                    && ommKey == materials[perMesh.materialDataIndex].ommIdentity;
                if (!usable) {
                    desc.contributionAndFlags |= RTAS_INSTANCE_FLAG_DISABLE_OMMS << 24u;
                    counters.InterlockedAdd(CLOD_RT_COUNTER_OMM_DISABLED_INSTANCES * 4u, 1u);
                }
                else {
                    counters.InterlockedAdd(CLOD_RT_COUNTER_OMM_INSTANCES * 4u, 1u);
                }
            }
        }
    }
    instances[id.x] = desc;
}
