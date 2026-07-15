#pragma once

// Header containing CLod-related types originally defined in
// ShaderBuffers.h.  These types only depend on DirectXMath and the
// clusterlod.h header, so they can be consumed by headless code.

#include <cstdint>
#include <DirectXMath.h>
#include "ThirdParty/meshoptimizer/clusterlod.h"

struct BoundingSphere {
	DirectX::XMFLOAT4 sphere;
};

static constexpr uint32_t CLOD_PAGE_ATTRIBUTE_NORMAL = 1u << 0;
static constexpr uint32_t CLOD_PAGE_ATTRIBUTE_JOINTS = 1u << 1;
static constexpr uint32_t CLOD_PAGE_ATTRIBUTE_WEIGHTS = 1u << 2;
static constexpr uint32_t CLOD_PAGE_ATTRIBUTE_COLOR = 1u << 3;
static constexpr uint32_t CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME = 1u << 4;

static constexpr uint32_t CLOD_POSITION_FORMAT_FLOAT3 = 1u;
static constexpr uint32_t CLOD_POSITION_FORMAT_FLOAT3_STRIDE_BYTES = sizeof(float) * 3u;

static constexpr uint32_t CLOD_CLUSTER_KIND_TRIANGLE = 0u;
static constexpr uint32_t CLOD_CLUSTER_KIND_VOXEL = 1u;
static constexpr uint32_t CLOD_CLUSTER_CULL_FLAG_ANIMATED = 1u << 0;
static constexpr uint32_t CLOD_CLUSTER_CULL_FLAG_BONE_OVERFLOW = 1u << 1;
static constexpr uint32_t CLOD_CLUSTER_CULL_FLAG_RIGID_COMPONENT = 1u << 2;
static constexpr uint32_t CLOD_CLUSTER_CULL_KIND_MASK = 0xFFu;
static constexpr uint32_t CLOD_CLUSTER_CULL_FLAGS_SHIFT = 8u;
static constexpr uint32_t CLOD_CLUSTER_CULL_BONE_COUNT_SHIFT = 16u;
constexpr uint32_t CLodPackClusterCullMetadata(uint32_t kind, uint32_t flags, uint32_t boneCount)
{
	return (kind & CLOD_CLUSTER_CULL_KIND_MASK) |
		((flags & 0xFFu) << CLOD_CLUSTER_CULL_FLAGS_SHIFT) |
		((boneCount & 0xFFFFu) << CLOD_CLUSTER_CULL_BONE_COUNT_SHIFT);
}
constexpr uint32_t CLodClusterCullMetadataBoneCount(uint32_t metadata)
{
	return metadata >> CLOD_CLUSTER_CULL_BONE_COUNT_SHIFT;
}
static constexpr uint32_t CLOD_TRIANGLE_PAGE_MAGIC = 0x4C435254u; // TRCL
static constexpr uint32_t CLOD_VOXEL_PAGE_MAGIC = 0x4C435856u; // VXCL

// Representation-neutral, resident culling prefix.  Page encoders keep this
// contract even when their payload tails use different stream layouts.
struct CLodClusterCullHeader
{
	DirectX::XMFLOAT4 bounds = {};
	uint32_t payloadBase = 0;
	uint32_t primitiveCountAndRefinedGroup = 0;
	uint32_t boneListOffset = 0;
	uint32_t kindFlagsAndBoneCount = 0;
};
static_assert(sizeof(CLodClusterCullHeader) == 32, "CLodClusterCullHeader must be 32 bytes");

struct CLodClusterPagePrefix
{
	uint32_t formatAndKind = 0;
	uint32_t clusterCount = 0;
	uint32_t descriptorOffset = 0;
	uint32_t boneIndexStreamOffset = 0;
};
static_assert(sizeof(CLodClusterPagePrefix) == 16, "CLodClusterPagePrefix must be 16 bytes");

struct CLodVoxelPageHeader
{
	uint32_t formatAndKind = CLOD_VOXEL_PAGE_MAGIC;
	uint32_t clusterCount = 0;
	uint32_t descriptorOffset = 0;
	uint32_t boneIndexStreamOffset = 0;
	uint32_t cubeCount = 0;
	uint32_t cubeRecordsOffset = 0;
	uint32_t attributeSamplesOffset = 0;
	uint32_t attributeSamplesPerCube = 0;
	uint32_t clusterRecordStride = 0;
	uint32_t cubeRecordStride = 0;
	uint32_t attributeSampleStride = 0;
	uint32_t firstCluster = 0;
	uint32_t firstCube = 0;
	uint32_t reserved0 = 0;
	uint32_t reserved1 = 0;
	uint32_t reserved2 = 0;
};
static_assert(sizeof(CLodVoxelPageHeader) == 64, "CLodVoxelPageHeader must be 64 bytes");

// Embedded at byte 0 of each page-tile in the page pool.
// Compression params moved to per-meshlet descriptors.
// 16 x uint32 = 64 bytes.
struct CLodPageHeader
{
	uint32_t formatAndKind = CLOD_TRIANGLE_PAGE_MAGIC; // [0] common page magic/kind
	uint32_t meshletCount = 0;            // [1] common cluster count
	uint32_t descriptorOffset = 0;        // [2] common descriptor-stream byte offset
	uint32_t boneIndexStreamOffset = 0;   // [3] common page-local bone stream byte offset

	uint32_t compressedPositionQuantExp = 0; // [4] CLOD_POSITION_FORMAT_* value
	uint32_t attributeMask = 0;           // [5] page-wide optional non-UV attribute mask
	uint32_t uvSetCount = 0;              // [6] UV set count packed into this page
	uint32_t uvDescriptorOffset = 0;      // [7] byte offset to CLodMeshletUvDescriptor table
	uint32_t positionBitstreamOffset = 0; // [8] byte offset to native position stream
	uint32_t normalArrayOffset = 0;       // [9] byte offset to normal array
	uint32_t colorArrayOffset = 0;        // [10] byte offset to RGBA8_UNORM color array
	uint32_t jointArrayOffset = 0;        // [11] byte offset to joint array
	uint32_t weightArrayOffset = 0;       // [12] byte offset to weight array
	uint32_t uvBitstreamDirectoryOffset = 0; // [13] byte offset to UV stream directory
	uint32_t triangleStreamOffset = 0;    // [14] byte offset to triangle byte stream
	uint32_t tangentFrameArrayOffset = 0; // [15] byte offset to tangent-frame array
};
static_assert(sizeof(CLodPageHeader) == 64, "CLodPageHeader must be 64 bytes");

// Per-meshlet descriptor in an SoA page format.
// Self-contained: each meshlet carries its own non-UV compression params, bounds, and LOD metadata.
struct CLodMeshletDescriptor
{
	DirectX::XMFLOAT4 bounds = {};        // [0-3] common bind-pose culling sphere
	uint32_t positionBitOffset = 0;       // [4] common payload base
	uint32_t triangleCountAndRefinedGroup = 0; // [5] common primitive count/refined group
	uint32_t boneListOffset = 0;          // [6] common page-local bone-list offset
	uint32_t boneCount = 0;               // [7] common bounded bone count

	uint32_t vertexAttributeOffset = 0;   // [8] triangle payload tail
	uint32_t triangleByteOffset = 0;      // [9]
	int32_t  minQx = 0;                   // [10]
	int32_t  minQy = 0;                   // [11]
	int32_t  minQz = 0;                   // [12]
	uint32_t bitsAndVertexCount = 0;      // [13]
	uint32_t sourceGroupLocalIndex = 0xFFFFFFFFu; // [14]
	float terrainRvtLocalSkyrimXYRadius = 0.0f; // [15]
};
static_assert(sizeof(CLodMeshletDescriptor) == 64, "CLodMeshletDescriptor must be 64 bytes");

// Per-(meshlet, uv-set) descriptor in an SoA page format.
// 8 x uint32 = 32 bytes = 2 x Load4 on GPU.
struct CLodMeshletUvDescriptor
{
	uint32_t uvBitOffset = 0;             // [0] bit offset into this UV set's page-local bitstream
	float    uvMinU = 0.0f;               // [1]
	float    uvMinV = 0.0f;               // [2]
	float    uvScaleU = 0.0f;             // [3]
	float    uvScaleV = 0.0f;             // [4]
	uint32_t uvBits = 0;                  // [5] bitsU:8 | bitsV:8
	uint32_t reserved0 = 0;               // [6]
	uint32_t reserved1 = 0;               // [7]
};
static_assert(sizeof(CLodMeshletUvDescriptor) == 32, "CLodMeshletUvDescriptor must be 32 bytes");

// Runtime-filled entry mapping a group-local page index to its physical slab location.
struct GroupPageMapEntry
{
	uint32_t slabDescriptorIndex = 0; // Descriptor-heap index of the slab BAB
	uint32_t slabByteOffset = 0;      // Byte offset of page start in slab
};

// Retained for CPU-side streaming state tracking.
struct ClusterLODGroupChunk
{
	uint32_t groupVertexCount = 0;
	uint32_t meshletCount = 0;
	uint32_t meshletTrianglesByteCount = 0;
	uint32_t compressedPositionQuantExp = CLOD_POSITION_FORMAT_FLOAT3;
	uint32_t compressedFlags = 0;
};

// Cluster LOD data
// One entry per (group -> refinedGroup) edge.
// refinedGroup == -1 means "terminal meshlets" (original geometry)
// A segment references a contiguous run of meshlets within a single page.
struct ClusterLODGroupSegment
{
	int32_t  refinedGroup;              // group id to refine into, or -1
	uint32_t firstMeshletInPage;         // page-local start meshlet index
	uint32_t meshletCount;               // number of meshlets in this segment
	uint32_t pageIndex = 0;              // mesh-local page-map index
};

struct ClusterLODGroup
{
	clodBounds bounds; // 5 floats
	uint32_t firstMeshlet = 0;
	uint32_t meshletCount = 0;
	int32_t depth = 0;

	uint32_t firstGroupVertex = 0;
	uint32_t groupVertexCount = 0;
	uint32_t firstSegment = 0;    // offset into m_clodSegments
	uint32_t segmentCount = 0;    // number of ClusterLODGroupSegment entries for this group

	uint32_t terminalSegmentCount = 0;
	uint32_t flags = 0;         // Bit 0: IS_VOXEL_GROUP
	uint32_t pageMapBase = 0;   // first mesh-local page-map slot owned by this group
	uint32_t pageCount = 0;     // number of group-owned page-map slots / streamable pages
	int32_t  parentGroupId = -1; // mesh-local group index of the parent group (-1 for root)
	float maxParentError = 0.0f; // max error of any parent group that refines into this group
	float representationError = 0.0f; // voxel payload quality/diagnostic error; builder uses it to derive cut boundaries
};

static constexpr uint32_t CLOD_GROUP_FLAG_IS_VOXEL = 1u << 0;
static constexpr uint32_t CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY = 1u << 1;
static constexpr uint32_t CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL = 1u << 2;

static constexpr uint32_t CLOD_NODE_INTERNAL = 0u;
static constexpr uint32_t CLOD_NODE_VOXEL_LEAF = 1u;
static constexpr uint32_t CLOD_NODE_SEGMENT_LEAF = 2u;
static constexpr uint32_t CLOD_NODE_INSTANCE_ROOT = 3u;
static constexpr uint32_t CLOD_ASSEMBLY_TRANSFORM_SENTINEL = 0xFFFFFFFFu;
static constexpr uint32_t CLOD_ASSEMBLY_BONE_REMAP_SENTINEL = 0xFFFFFFFFu;
static constexpr uint32_t CLOD_ASSEMBLY_MAX_STACK_DEPTH = 8u;

struct ClusterLODAssemblyTransform
{
	DirectX::XMFLOAT4 row0 = { 1.0f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 row1 = { 0.0f, 1.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 row2 = { 0.0f, 0.0f, 1.0f, 0.0f };
};
static_assert(sizeof(ClusterLODAssemblyTransform) == 48, "ClusterLODAssemblyTransform must be 48 bytes");

struct ClusterLODAssemblyInstance
{
	uint32_t targetRootNode = 0;
	uint32_t transformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
	uint32_t flags = 0;
	uint32_t stackDepth = 0;
};
static_assert(sizeof(ClusterLODAssemblyInstance) == 16, "ClusterLODAssemblyInstance must be 16 bytes");

struct ClusterLODAssemblyBoneRemap
{
	uint32_t remapIndexBase = CLOD_ASSEMBLY_BONE_REMAP_SENTINEL;
	uint32_t remapIndexCount = 0;
	uint32_t flags = 0;
	uint32_t reserved = 0;
};
static_assert(sizeof(ClusterLODAssemblyBoneRemap) == 16, "ClusterLODAssemblyBoneRemap must be 16 bytes");

static constexpr uint32_t CLOD_VOXEL_STATIC_BONE_INDEX = 0xFFFFFFFFu;
static constexpr uint32_t CLOD_VOXEL_MAX_CUBES_PER_CLUSTER = 128u;
static constexpr uint32_t CLOD_VOXEL_CLUSTER_FLAG_HAS_SKINNED_CUBES =
	CLOD_CLUSTER_CULL_FLAG_ANIMATED << CLOD_CLUSTER_CULL_FLAGS_SHIFT;

struct CLodVoxelClusterRecord
{
	DirectX::XMFLOAT4 bounds = {};        // [0-3] common bind-pose culling sphere
	uint32_t firstCube = 0;               // [4] common payload base
	uint32_t cubeCount = 0;               // [5] common primitive count
	uint32_t reserved2 = 0;               // [6] common page-local bone-list offset
	uint32_t flags = 0;                   // [7] common kind/flags/bone-count metadata
	DirectX::XMFLOAT4 aabbMinAndVoxelWidth = {}; // xyz=min, w=voxel width
	uint32_t resolution = 0;
	DirectX::XMFLOAT2 uvDensity = { 0.0f, 0.0f };
	int32_t refinedGroup = -1;
};
static_assert(sizeof(CLodVoxelClusterRecord) == 64, "CLodVoxelClusterRecord must be 64 bytes");

struct CLodVoxelCubeRecord
{
	uint32_t cubeCoord = 0; // x:10 | y:10 | z:10 in 4x4x4-cell cube coordinates
	uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	uint64_t occupancyMask = 0;
	float opacitySum = 0.0f;
	uint32_t firstAttribute = 0; // first compact active-cell CLodVoxelAttributeSample for this cube
	int32_t refinedGroup = -1;
	uint32_t activeBounds = 0; // minX/Y/Z and maxX/Y/Z packed as 2-bit local cell coordinates
};
static_assert(sizeof(CLodVoxelCubeRecord) == 32, "CLodVoxelCubeRecord must be 32 bytes");

struct CLodVoxelAttributeSample
{
	// xy = oct-encoded object-space symmetry axis, z/w = sigmaPerp/sigmaParallel.
	DirectX::XMFLOAT4 sggxAxisAndSigmas = { 0.0f, 0.0f, 1.0e-4f, 0.5f };
	float opacity = 0.0f;
	DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
};
static_assert(sizeof(CLodVoxelAttributeSample) == 28, "CLodVoxelAttributeSample must be 28 bytes");
