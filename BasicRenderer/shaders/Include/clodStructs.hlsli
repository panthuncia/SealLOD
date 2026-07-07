#ifndef CLOD_STRUCTS_HLSLI
#define CLOD_STRUCTS_HLSLI

struct MeshInstanceClodOffsets
{
    uint clodMeshMetadataIndex;
};

MeshInstanceClodOffsets LoadCLodOffsetsForDrawRecord(InstanceDrawRecordBuffer record)
{
    StructuredBuffer<MeshInstanceClodOffsets> clodOffsets =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Offsets)];
    return clodOffsets[record.clodOffsetIndex];
}

MeshInstanceClodOffsets LoadCLodOffsetsForDraw(uint drawRecordIndex)
{
    return LoadCLodOffsetsForDrawRecord(LoadInstanceDrawRecord(drawRecordIndex));
}

struct CLodMeshMetadata
{
    uint groupsBase;
    uint segmentsBase;
    uint lodNodesBase;
    uint rootNode; // node index (relative to lodNodesBase) to start traversal from
    uint groupChunkTableBase;
    uint groupChunkTableCount;
    uint pageMapBase; // global offset into GroupPageMap buffer for this mesh
    uint lodLevelInfoBase;
    uint lodLevelCount;
    uint maxDepth;
    uint assemblyTransformBase;
    uint assemblyTransformCount;
    uint assemblyInstanceBase;
    uint assemblyInstanceCount;
};

struct CLodHierarchyLevelInfo
{
    uint rootNode;
    uint nodeRangeOffset;
    uint nodeRangeCount;
    uint pad0;
};

// GPU-visible page table entry - maps a virtual page ID to a slab + byte offset.
struct PageTableEntry
{
    uint slabIndex;      // Which slab ByteAddressBuffer this page lives in.
    uint slabByteOffset; // Byte offset of the page start within that slab.
};

static const uint CLOD_PAGE_ATTRIBUTE_NORMAL = 1u << 0;
static const uint CLOD_PAGE_ATTRIBUTE_JOINTS = 1u << 1;
static const uint CLOD_PAGE_ATTRIBUTE_WEIGHTS = 1u << 2;
static const uint CLOD_PAGE_ATTRIBUTE_COLOR = 1u << 3;
static const uint CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME = 1u << 4;

static const uint CLOD_POSITION_FORMAT_FLOAT3 = 1u;
static const uint CLOD_POSITION_FORMAT_FLOAT3_STRIDE_BYTES = 12u;

// Embedded at byte 0 of each page-tile. Simplified header.
// 16 x uint32 = 64 bytes.
struct CLodPageHeader
{
    uint meshletCount;                // [0]
    uint compressedPositionQuantExp;  // [1] CLOD_POSITION_FORMAT_* value
    uint attributeMask;               // [2] page-wide optional non-UV attribute mask
    uint uvSetCount;                  // [3] UV set count packed into this page

    uint descriptorOffset;            // [4] byte offset to CLodMeshletDescriptor array
    uint uvDescriptorOffset;          // [5] byte offset to CLodMeshletUvDescriptor table
    uint positionBitstreamOffset;     // [6] byte offset to native position stream
    uint normalArrayOffset;           // [7] byte offset to normal array
    uint colorArrayOffset;            // [8] byte offset to RGBA8_UNORM color array per vertex
    uint jointArrayOffset;            // [9] byte offset to two-uint4 joint array per vertex
    uint weightArrayOffset;           // [10] byte offset to two-float4 weight array per vertex
    uint uvBitstreamDirectoryOffset;  // [11] byte offset to UV bitstream offset table
    uint triangleStreamOffset;        // [12] byte offset to triangle byte stream
    uint boneIndexStreamOffset;       // [13] byte offset to page-local bone-index stream
    uint tangentFrameArrayOffset;     // [14] byte offset to tangent-frame angle/sign array
    uint reserved1;
};

// Per-meshlet descriptor. Self-contained stream offsets, bounds, and LOD metadata.
// 16 x uint32 = 64 bytes = 4 x Load4.
struct CLodMeshletDescriptor
{
    uint positionBitOffset;           // [0] byte offset into page native position stream
    uint vertexAttributeOffset;       // [1] element offset into page vertex-attribute arrays
    uint triangleByteOffset;          // [2] byte offset into page triangle stream
    uint boneListOffset;              // [3] uint offset into page bone-index stream

    int  minQx;                       // [4] reserved for future compact position formats
    int  minQy;                       // [5] reserved for future compact position formats
    int  minQz;                       // [6] reserved for future compact position formats

    uint bitsAndVertexCount;          // [7] reserved:24 | vertexCount:8
    uint triangleCountAndRefinedGroup; // [8] triangleCount:16 | (refinedGroupId+1):16
    uint boneCount;                   // [9]
    uint sourceGroupLocalIndex;       // [10] temporary diagnostic source group tag
    float terrainRvtLocalSkyrimXYRadius; // [11] local terrain XY footprint radius for cluster request mips

    float4 bounds;                    // [12-15] bounding sphere {cx, cy, cz, radius}
};

// Per-(meshlet, uv-set) descriptor. 8 x uint32 = 32 bytes = 2 x Load4.
struct CLodMeshletUvDescriptor
{
    uint uvBitOffset;                 // [0] bit offset into this UV set's page-local bitstream
    float uvMinU;                     // [1]
    float uvMinV;                     // [2]
    float uvScaleU;                   // [3]
    float uvScaleV;                   // [4]
    uint uvBits;                      // [5] bitsU:8 | bitsV:8
    uint reserved0;                   // [6]
    uint reserved1;                   // [7]
};

// Helper functions to unpack CLodMeshletDescriptor fields
uint CLodDescBitsX(CLodMeshletDescriptor desc) { return desc.bitsAndVertexCount & 0xFFu; }
uint CLodDescBitsY(CLodMeshletDescriptor desc) { return (desc.bitsAndVertexCount >> 8u) & 0xFFu; }
uint CLodDescBitsZ(CLodMeshletDescriptor desc) { return (desc.bitsAndVertexCount >> 16u) & 0xFFu; }
uint CLodDescVertexCount(CLodMeshletDescriptor desc) { return (desc.bitsAndVertexCount >> 24u) & 0xFFu; }
uint CLodDescTriangleCount(CLodMeshletDescriptor desc) { return desc.triangleCountAndRefinedGroup & 0xFFFFu; }
int  CLodDescRefinedGroupId(CLodMeshletDescriptor desc) { return (int)(desc.triangleCountAndRefinedGroup >> 16u) - 1; }
uint CLodDescBoneCount(CLodMeshletDescriptor desc) { return desc.boneCount; }
uint CLodUvDescBitsU(CLodMeshletUvDescriptor desc) { return desc.uvBits & 0xFFu; }
uint CLodUvDescBitsV(CLodMeshletUvDescriptor desc) { return (desc.uvBits >> 8u) & 0xFFu; }

float3 CLodLoadPagePosition(
    ByteAddressBuffer slab,
    uint positionFormat,
    uint positionStreamBase,
    uint positionByteOffset,
    uint meshletLocalVertex)
{
    if (positionFormat != CLOD_POSITION_FORMAT_FLOAT3)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const uint addr = positionStreamBase + positionByteOffset + meshletLocalVertex * CLOD_POSITION_FORMAT_FLOAT3_STRIDE_BYTES;
    return asfloat(slab.Load3(addr));
}

// Runtime-filled entry: maps group-local page index to physical slab location.
struct GroupPageMapEntry
{
    uint slabDescriptorIndex; // Descriptor-heap index of the slab BAB
    uint slabByteOffset;      // Byte offset of page start in slab
};

struct CLodStreamingRequest
{
    uint groupGlobalIndex;
    uint meshInstanceIndex;
    uint meshBufferIndex;
    uint viewId; // low 16 bits: viewId, high 16 bits: quantized priority
};

struct CLodStreamingRuntimeState
{
    uint activeGroupScanCount;
    uint unloadAfterFrames;
    uint activeGroupsBitsetWordCount;
    uint pad2;
};
struct ClodBounds
{
    float4 centerAndRadius; // xyz = center, w = radius
    float error; // simplification error in mesh space
};

struct ClusterLODGroupSegment
{
    int refinedGroup; // -1 => terminal meshlets bucket
    uint firstMeshletInPage; // page-local start meshlet index
    uint meshletCount;
    uint pageIndex; // mesh-local page-map index
};

struct ClusterLODGroup
{
    ClodBounds bounds; // center/radius/error
    
    uint firstMeshlet;
    uint meshletCount;
    int depth;

    uint firstGroupVertex;
    uint groupVertexCount;
    uint firstSegment;
    uint segmentCount;

    uint terminalSegmentCount;
    uint flags;
    uint pageMapBase; // first mesh-local page-map slot owned by this group
    uint pageCount;   // number of group-owned page-map slots / streamable pages
    int parentGroupId; // mesh-local group index of the parent group (-1 for root)
    float maxParentError; // max error of any parent group that refines into this group
    float representationError; // voxel payload quality/diagnostic error; builder uses it to derive cut boundaries
};

static const uint CLOD_NODE_INTERNAL = 0u;
static const uint CLOD_NODE_VOXEL_LEAF = 1u;
static const uint CLOD_NODE_SEGMENT_LEAF = 2u;
static const uint CLOD_NODE_INSTANCE_ROOT = 3u;
static const uint CLOD_ASSEMBLY_TRANSFORM_SENTINEL = 0xFFFFFFFFu;
static const uint CLOD_ASSEMBLY_MAX_STACK_DEPTH = 8u;

struct ClusterLODAssemblyTransform
{
    float4 row0;
    float4 row1;
    float4 row2;
};

struct ClusterLODAssemblyInstance
{
    uint targetRootNode;
    uint transformIndex;
    uint flags;
    uint stackDepth;
};

struct CLodRuntimeAssemblyTransform
{
    float4 modelRow0;
    float4 modelRow1;
    float4 modelRow2;
    float4 prevModelRow0;
    float4 prevModelRow1;
    float4 prevModelRow2;
};

static const uint CLOD_GROUP_FLAG_IS_VOXEL = 1u << 0;
static const uint CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY = 1u << 1;
static const uint CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL = 1u << 2;

static const uint CLOD_VOXEL_STATIC_BONE_INDEX = 0xFFFFFFFFu;
static const uint CLOD_VOXEL_MAX_CUBES_PER_CLUSTER = 128u;
static const uint CLOD_VOXEL_CLUSTER_FLAG_HAS_SKINNED_CUBES = 1u << 0;

struct CLodVoxelClusterRecord
{
    uint firstCube;
    uint cubeCount;
    int refinedGroup;
    uint flags;
    float4 bounds;
    float4 aabbMinAndVoxelWidth;
    uint resolution;
    uint reserved0;
    uint reserved1;
    uint reserved2;
};

struct CLodVoxelCubeRecord
{
    uint cubeCoord; // x:10 | y:10 | z:10 in 4x4x4-cell cube coordinates
    uint dominantBoneIndex;
    uint2 occupancyMask;
    float opacitySum;
    uint firstAttribute;
    int refinedGroup;
    uint activeBounds; // minX/Y/Z and maxX/Y/Z packed as 2-bit local cell coordinates
};

uint3 CLodVoxelDecodeCubeCoord(uint packedCoord)
{
    return uint3(
        packedCoord & 0x3FFu,
        (packedCoord >> 10u) & 0x3FFu,
        (packedCoord >> 20u) & 0x3FFu);
}

struct CLodVoxelAttributeSample
{
    float4 sggxAxisAndSigmas;
    float opacity;
    float2 uv;
};

struct CLodVoxelRasterQueueDescriptors
{
    uint rigidWorkRecordsUAVDescriptorIndex;
    uint rigidWorkRecordCounterUAVDescriptorIndex;
    uint skinnedWorkRecordsUAVDescriptorIndex;
    uint skinnedWorkRecordCounterUAVDescriptorIndex;
    uint workRecordCapacity;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct CLodVoxelRasterWorkRecord
{
    uint visibleClusterIndex;
    uint instanceIndex;
    uint viewId;
    uint localGroupId;
    uint localPageIndex;
    uint pageLocalClusterIndex;
};

struct CLodVoxelRasterDispatchCommand
{
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
};

static const uint CLOD_VOXEL_PAGE_MAGIC = 0x4C435856u;
static const uint CLOD_VOXEL_CLUSTER_RECORD_STRIDE = 64u;
static const uint CLOD_VOXEL_CUBE_RECORD_STRIDE = 32u;
static const uint CLOD_VOXEL_ATTRIBUTE_SAMPLE_STRIDE = 28u;
static const uint CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT = 0u;

struct CLodVoxelPageHeader
{
    uint magic;
    uint firstCluster;
    uint clusterCount;
    uint firstCube;
    uint cubeCount;
    uint reservedPage0;
    uint reserved0;
    uint reserved1;
    uint clusterRecordsOffset;
    uint cubeRecordsOffset;
    uint attributeSamplesOffset;
    uint attributeSamplesPerCube;
    uint clusterRecordStride;
    uint cubeRecordStride;
    uint attributeSampleStride;
    uint reserved2;
};

GroupPageMapEntry CLodLoadVoxelPageMapEntry(CLodMeshMetadata metadata, ClusterLODGroup group, uint pageIndex)
{
    StructuredBuffer<GroupPageMapEntry> pageMap = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::GroupPageMap)];
    return pageMap[metadata.pageMapBase + pageIndex];
}

CLodVoxelPageHeader CLodLoadVoxelPageHeader(uint slabDescriptorIndex, uint pageByteOffset)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    uint4 d0 = slab.Load4(pageByteOffset + 0u);
    uint4 d1 = slab.Load4(pageByteOffset + 16u);
    uint4 d2 = slab.Load4(pageByteOffset + 32u);
    uint4 d3 = slab.Load4(pageByteOffset + 48u);
    CLodVoxelPageHeader header;
    header.magic = d0.x;
    header.firstCluster = d0.y;
    header.clusterCount = d0.z;
    header.firstCube = d0.w;
    header.cubeCount = d1.x;
    header.reservedPage0 = d1.y;
    header.reserved0 = d1.z;
    header.reserved1 = d1.w;
    header.clusterRecordsOffset = d2.x;
    header.cubeRecordsOffset = d2.y;
    header.attributeSamplesOffset = d2.z;
    header.attributeSamplesPerCube = d2.w;
    header.clusterRecordStride = d3.x;
    header.cubeRecordStride = d3.y;
    header.attributeSampleStride = d3.z;
    header.reserved2 = d3.w;
    return header;
}

CLodVoxelClusterRecord CLodLoadVoxelClusterFromPage(uint slabDescriptorIndex, uint pageByteOffset, uint clusterRecordsOffset, uint pageLocalClusterIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    uint addr = pageByteOffset + clusterRecordsOffset + pageLocalClusterIndex * CLOD_VOXEL_CLUSTER_RECORD_STRIDE;
    uint4 d0 = slab.Load4(addr + 0u);
    uint4 d1 = slab.Load4(addr + 16u);
    uint4 d2 = slab.Load4(addr + 32u);
    uint4 d3 = slab.Load4(addr + 48u);
    CLodVoxelClusterRecord cluster;
    cluster.firstCube = d0.x;
    cluster.cubeCount = d0.y;
    cluster.refinedGroup = asint(d0.z);
    cluster.flags = d0.w;
    cluster.bounds = asfloat(d1);
    cluster.aabbMinAndVoxelWidth = asfloat(d2);
    cluster.resolution = d3.x;
    cluster.reserved0 = d3.y;
    cluster.reserved1 = d3.z;
    cluster.reserved2 = d3.w;
    return cluster;
}

CLodVoxelCubeRecord CLodLoadVoxelCubeFromPage(uint slabDescriptorIndex, uint pageByteOffset, uint cubeRecordsOffset, uint pageLocalCubeIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    uint addr = pageByteOffset + cubeRecordsOffset + pageLocalCubeIndex * CLOD_VOXEL_CUBE_RECORD_STRIDE;
    uint4 d0 = slab.Load4(addr + 0u);
    uint4 d1 = slab.Load4(addr + 16u);
    CLodVoxelCubeRecord cube;
    cube.cubeCoord = d0.x;
    cube.dominantBoneIndex = d0.y;
    cube.occupancyMask = d0.zw;
    cube.opacitySum = asfloat(d1.x);
    cube.firstAttribute = d1.y;
    cube.refinedGroup = asint(d1.z);
    cube.activeBounds = d1.w;
    return cube;
}

bool CLodVoxelClusterHasSkinnedCubes(CLodVoxelClusterRecord cluster)
{
    return (cluster.flags & CLOD_VOXEL_CLUSTER_FLAG_HAS_SKINNED_CUBES) != 0u;
}

bool CLodTryLoadVoxelPageForSegment(
    CLodMeshMetadata metadata,
    ClusterLODGroup group,
    ClusterLODGroupSegment segment,
    out GroupPageMapEntry pageEntry,
    out CLodVoxelPageHeader pageHeader)
{
    pageEntry = (GroupPageMapEntry)0;
    pageHeader = (CLodVoxelPageHeader)0;
    if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
        group.pageCount == 0u ||
        segment.meshletCount == 0u ||
        segment.pageIndex < group.pageMapBase ||
        segment.pageIndex >= group.pageMapBase + group.pageCount)
    {
        return false;
    }

    pageEntry = CLodLoadVoxelPageMapEntry(metadata, group, segment.pageIndex);
    pageHeader = CLodLoadVoxelPageHeader(pageEntry.slabDescriptorIndex, pageEntry.slabByteOffset);
    if (pageHeader.magic != CLOD_VOXEL_PAGE_MAGIC)
    {
        return false;
    }

    return segment.firstMeshletInPage + segment.meshletCount <= pageHeader.clusterCount;
}

CLodVoxelAttributeSample CLodLoadVoxelAttributeSampleFromPage(GroupPageMapEntry pageEntry, CLodVoxelPageHeader pageHeader, CLodVoxelCubeRecord cube, uint localCellIndex)
{
    CLodVoxelAttributeSample sample;
    sample.sggxAxisAndSigmas = float4(0.0f, 0.0f, 1.0e-4f, 0.5f);
    sample.opacity = 0.0f;
    sample.uv = float2(0.0f, 0.0f);

    if (localCellIndex >= 64u)
    {
        return sample;
    }

    const uint localCellBit = localCellIndex < 32u
        ? (1u << localCellIndex)
        : (1u << (localCellIndex - 32u));
    const bool occupied = localCellIndex < 32u
        ? ((cube.occupancyMask.x & localCellBit) != 0u)
        : ((cube.occupancyMask.y & localCellBit) != 0u);
    if (!occupied)
    {
        return sample;
    }

    uint localAttributeIndex = localCellIndex;
    if (pageHeader.attributeSamplesPerCube == CLOD_VOXEL_ATTRIBUTE_SAMPLES_COMPACT)
    {
        const uint lowerMask = localCellIndex < 32u
            ? (localCellIndex == 0u ? 0u : ((1u << localCellIndex) - 1u))
            : 0xFFFFFFFFu;
        const uint upperMask = localCellIndex < 32u
            ? 0u
            : ((localCellIndex - 32u) == 0u ? 0u : ((1u << (localCellIndex - 32u)) - 1u));
        localAttributeIndex = countbits(cube.occupancyMask.x & lowerMask) +
            countbits(cube.occupancyMask.y & upperMask);
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageEntry.slabDescriptorIndex)];
    const uint attributeIndex = cube.firstAttribute + localAttributeIndex;
    const uint attributeStride = pageHeader.attributeSampleStride != 0u ? pageHeader.attributeSampleStride : CLOD_VOXEL_ATTRIBUTE_SAMPLE_STRIDE;
    const uint addr = pageEntry.slabByteOffset + pageHeader.attributeSamplesOffset + attributeIndex * attributeStride;
    sample.sggxAxisAndSigmas = asfloat(slab.Load4(addr));
    sample.opacity = asfloat(slab.Load(addr + 16u));
    sample.uv = attributeStride >= 28u ? asfloat(slab.Load2(addr + 20u)) : float2(0.0f, 0.0f);
    return sample;
}

// Replay buffer: single physical buffer split into four 50 MB regions.
// Node/meshlet replay are always present; Reyes split/dice replay are consumed by the WG Reyes experiment.
static const uint CLOD_REPLAY_BUFFER_SIZE_BYTES          = 200u * 1024u * 1024u;
static const uint CLOD_REPLAY_NODE_REGION_SIZE_BYTES     = 50u * 1024u * 1024u;
static const uint CLOD_REPLAY_MESHLET_REGION_OFFSET      = CLOD_REPLAY_NODE_REGION_SIZE_BYTES;
static const uint CLOD_REPLAY_REYES_SPLIT_REGION_OFFSET  = 2u * CLOD_REPLAY_NODE_REGION_SIZE_BYTES;
static const uint CLOD_REPLAY_REYES_DICE_REGION_OFFSET   = 3u * CLOD_REPLAY_NODE_REGION_SIZE_BYTES;
static const uint CLOD_REPLAY_MESHLET_REGION_SIZE_BYTES  = CLOD_REPLAY_NODE_REGION_SIZE_BYTES;
static const uint CLOD_REPLAY_REYES_SPLIT_REGION_SIZE_BYTES = CLOD_REPLAY_NODE_REGION_SIZE_BYTES;
static const uint CLOD_REPLAY_REYES_DICE_REGION_SIZE_BYTES  = CLOD_REPLAY_NODE_REGION_SIZE_BYTES;

static const uint CLOD_NODE_REPLAY_STRIDE_BYTES    = 16u;  // 4 uints (TraverseNodeRecord)
static const uint CLOD_MESHLET_REPLAY_STRIDE_BYTES = 32u;  // 8 uints (MeshletBucketRecord, aligned)
static const uint CLOD_REYES_SPLIT_REPLAY_STRIDE_BYTES = 60u; // sizeof(CLodReyesSplitQueueEntry)
static const uint CLOD_REYES_DICE_REPLAY_STRIDE_BYTES  = 68u; // sizeof(CLodReyesDiceQueueEntry)

static const uint CLOD_NODE_REPLAY_CAPACITY    = CLOD_REPLAY_NODE_REGION_SIZE_BYTES / CLOD_NODE_REPLAY_STRIDE_BYTES;
static const uint CLOD_MESHLET_REPLAY_CAPACITY = CLOD_REPLAY_MESHLET_REGION_SIZE_BYTES / CLOD_MESHLET_REPLAY_STRIDE_BYTES;
static const uint CLOD_REYES_SPLIT_REPLAY_CAPACITY = CLOD_REPLAY_REYES_SPLIT_REGION_SIZE_BYTES / CLOD_REYES_SPLIT_REPLAY_STRIDE_BYTES;
static const uint CLOD_REYES_DICE_REPLAY_CAPACITY  = CLOD_REPLAY_REYES_DICE_REGION_SIZE_BYTES / CLOD_REYES_DICE_REPLAY_STRIDE_BYTES;

struct CLodReplayBufferState
{
    uint nodeWriteCount;
    uint meshletWriteCount;
    uint nodeDropped;
    uint meshletDropped;
    uint visibleClusterCombinedCount;
    uint reyesSplitWriteCount;
    uint reyesDiceWriteCount;
    uint reyesSplitDropped;
    uint reyesDiceDropped;
};

struct CLodViewDepthSRVIndex
{
    uint cameraBufferIndex;
    uint linearDepthSRVIndex;
    uint pad0;
    uint pad1;
};

struct CLodNodeGpuInput
{
    uint entrypointIndex;
    uint numRecords;
    uint64_t recordsAddress;
    uint64_t recordStride;
};

struct CLodMultiNodeGpuInput
{
    uint numNodeInputs;
    uint pad0;
    uint64_t nodeInputsAddress;
    uint64_t nodeInputStride;
};

// Shared software-raster launch constants.
// Both compute and work-graph paths use one 128-thread group per cluster.
#define SW_RASTER_THREADS            128
#define SW_RASTER_GROUPS_PER_CLUSTER 1
#define SW_RASTER_MAX_VERTS          128

// Batched work graph record for software rasterization of small clusters.
// Broadcasting node: ClusterCull accumulates up to SW_BATCH_MAX_CLUSTERS
// cluster indices per record. SWRaster reads full VisibleCluster data from
// the visible clusters buffer via indirection.
#define SW_BATCH_MAX_CLUSTERS 8

struct SWRasterBatchRecord
{
    uint3 dispatchGrid : SV_DispatchGrid; // (numClusters, 1, 1)
    uint numClusters;                       // 1..SW_BATCH_MAX_CLUSTERS
    uint clusterIndices[SW_BATCH_MAX_CLUSTERS]; // unsorted visible cluster buffer indices
};

// ---------------------------------------------------------------------------
// Page-job VSM software rasterization records.
// Three-node pipeline: ClusterCull → PageJobBuild → PageJobExpand → PageJobRasterPage.
// ---------------------------------------------------------------------------
#define PAGEJOB_BUILD_THREADS            128
#define PAGEJOB_BUILD_MAX_CLUSTERS       8
#define PAGEJOB_EXPAND_THREADS           64
#define PAGEJOB_RASTER_PAGE_THREADS      128
#define PAGEJOB_TILE_PAGES_X             8
#define PAGEJOB_TILE_PAGES_Y             8
#define PAGEJOB_MAX_TILE_JOBS_PER_CLUSTER 256
#define PAGEJOB_MAX_PAGES_PER_TILE       (PAGEJOB_TILE_PAGES_X * PAGEJOB_TILE_PAGES_Y)

// Build-stage input: batch of cluster indices (same shape as SWRasterBatchRecord).
struct PageJobBuildBatchRecord
{
    uint3 dispatchGrid : SV_DispatchGrid; // (1, 1, 1) — single group per batch
    uint numClusters;                       // 1..PAGEJOB_BUILD_MAX_CLUSTERS
    uint clusterIndices[PAGEJOB_BUILD_MAX_CLUSTERS];
};

// Expand-stage input: one (cluster, page-tile) pair. Build emits these.
struct PageJobExpandRecord
{
    uint3 dispatchGrid : SV_DispatchGrid; // (1, 1, 1)
    uint clusterIndex;                      // visible-cluster-buffer index
    uint packedTileAndClipmap;              // tileMinPageX:8 | tileMinPageY:8 | clipmapLayer:5
    uint packedPageBounds;                  // minPageX:8 | minPageY:8 | maxPageX:8 | maxPageY:8
};

// Raster-page-stage input: one (cluster, physical-page) pair. Expand emits these.
struct PageJobRasterPageRecord
{
    uint3 dispatchGrid : SV_DispatchGrid; // (1, 1, 1)
    uint clusterIndex;                      // visible-cluster-buffer index
    uint physicalPageIndex;                 // physical atlas page index
    uint packedPagePixelOriginAndClipmap;   // pagePixelMinX:16 | pagePixelMinY:16 (absolute virtual pixels)
    uint packedAtlasOriginAndClipmap;       // atlasBaseX:16 | atlasBaseY:16
    uint clipmapLayer;                      // clipmap layer for content-valid write-back
    uint2 wrappedPageCoords;                // for page-table content-valid write-back
};

#endif // CLOD_STRUCTS_HLSLI
