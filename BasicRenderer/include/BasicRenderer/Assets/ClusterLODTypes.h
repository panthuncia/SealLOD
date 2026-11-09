#pragma once

// Standalone header containing all ClusterLOD data types and the
// MeshIngestBuilder class.  No GPU / RHI / DeletionManager / BufferView
// dependencies - only standard library, DirectXMath, meshoptimizer, and the
// CLod shader types header.

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <directxmath.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <meshoptimizer.h>

#include "BasicRenderer/Assets/ClusterLODShaderTypes.h"
#include <BasicRenderer/Scene/VertexFlags.h>
#include <BasicRenderer/Assets/MeshData.h>
#include "BasicRenderer/Scene/Animation/DynamicWindMetadata.h"
#include "BasicRenderer/Scene/Animation/SkeletonArtifact.h"

// Forward declarations for GPU-side types only needed by MeshIngestBuilder::Build()
class Material;
class Mesh;

enum class MeshCpuDataPolicy {
	Retain,
	ReleaseAfterUpload,
};

struct VoxelCoverageHit
{
	uint32_t triangleIndex = 0;
	DirectX::XMFLOAT3 position{};
	DirectX::XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
	DirectX::XMFLOAT2 uv{};
	DirectX::XMFLOAT3 trianglePositions[3]{};
	DirectX::XMFLOAT2 triangleUvs[3]{};
	float barycentrics[3]{};
};

struct VoxelCoverageMaterialSample
{
	bool accepted = true;
	bool overrideNormal = false;
	DirectX::XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
	float weight = 1.0f;
};

using VoxelCoverageMaterialSampler = std::function<VoxelCoverageMaterialSample(const VoxelCoverageHit&)>;

// Traversal types

struct ClusterLODTraversalMetric
{
	DirectX::XMFLOAT4 cullingSphere = {};
	DirectX::XMFLOAT4 lodBoundingSphere = {};
	float maxQuadricError = 0;
	float padding[3] = { 0,0,0 };
};

struct ClusterLODNodeRange
{
	uint32_t isGroup = 0;        // 0=internal, 1=voxel-group-leaf, 2=segment-leaf
	uint32_t indexOrOffset = 0;  // segment-leaf: mesh-local segment index; internal: child offset
	uint32_t countMinusOne = 0;  // internal: childCount-1; leaf: unused
	uint32_t ownerGroupId = 0;   // segment-leaf: mesh-local group index (for page resolution + streaming)
};

struct ClusterLODNode
{
	ClusterLODNodeRange        range{};
	ClusterLODTraversalMetric  traversalMetric{};
};

struct ClusterLODNodeSkinningInfo
{
	uint32_t boneListOffset = 0;
	uint16_t boneCount = 0;
	uint16_t flags = 0;
};
static_assert(sizeof(ClusterLODNodeSkinningInfo) == 8, "ClusterLODNodeSkinningInfo must be 8 bytes");

static constexpr uint16_t CLOD_NODE_SKINNING_FLAG_OVERFLOW = 1u << 0;
static constexpr uint16_t CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK = 1u << 1;
static constexpr uint32_t CLOD_NODE_BONE_LIMIT_DEFAULT = 16u;
static constexpr uint32_t CLOD_NODE_BONE_LIMIT_HARD_MAX = 64u;

struct ClusterLODNodeRangeAlloc
{
	uint32_t offset = 0;
	uint32_t count = 0;
};

// Disk / Cache types

struct ClusterLODDiskChunkSpan
{
	uint64_t offset = 0;
	uint64_t sizeBytes = 0;
};

struct ClusterLODGroupDiskLocator
{
	uint64_t blobOffset = 0;
	uint32_t blobSizeBytes = 0;
	uint32_t reserved = 0;
};

struct ClusterLODCacheSource
{
	std::string sourceIdentifier;
	std::string primPath;
	std::string subsetName;
	uint64_t buildConfigHash = 0;
	std::wstring containerFileName;
};

struct ClusterLODAssemblySkeletonData
{
	std::vector<std::string> jointNames;
	std::vector<int32_t> parentIndices;
	std::vector<DirectX::XMFLOAT4X4> inverseBindMatrices;
	std::vector<DirectX::XMFLOAT4X4> restLocalMatrices;
	std::vector<DirectX::XMFLOAT4X4> bindGlobalMatrices;
	std::vector<uint32_t> windSimulationGroupIndices;
	std::string windProfileIdentity;
	DynamicWindMetadata dynamicWindMetadata;

	bool Empty() const
	{
		return jointNames.empty();
	}

	void Clear()
	{
		jointNames.clear();
		parentIndices.clear();
		inverseBindMatrices.clear();
		restLocalMatrices.clear();
		bindGlobalMatrices.clear();
		windSimulationGroupIndices.clear();
		windProfileIdentity.clear();
		dynamicWindMetadata = {};
	}
};

// Voxel group data model

struct VoxelCell
{
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t z = 0;
	float    opacity = 0.0f;
	DirectX::XMFLOAT4 sggxAxisAndSigmas = { 0.0f, 0.0f, 1.0e-4f, 0.5f };
	DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
	uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	int32_t refinedGroup = -1;
};

struct VoxelGroupPayload
{
	uint32_t resolution = 0;
	DirectX::XMFLOAT3 aabbMin{};
	DirectX::XMFLOAT3 aabbMax{};
	float voxelWidth = 0.0f;
	DirectX::XMFLOAT2 uvDensity = { 0.0f, 0.0f };
	std::vector<VoxelCell> activeCells;
};

struct VoxelGroupPackedMetadata
{
	DirectX::XMFLOAT4 aabbMinAndVoxelWidth{}; // xyz=min, w=voxel width
	DirectX::XMFLOAT4 aabbMaxAndError{};      // xyz=max, w=accepted voxel error
	uint32_t firstCluster = 0;
	uint32_t clusterCount = 0;
	uint32_t firstCube = 0;
	uint32_t cubeCount = 0;
	uint32_t resolution = 0;
	uint32_t flags = 0;
	DirectX::XMFLOAT2 uvDensity = { 0.0f, 0.0f };
};

struct VoxelGroupMapping
{
	std::vector<int32_t> groupToPayloadIndex;
	std::vector<VoxelGroupPayload> payloads;
	std::vector<int32_t> groupToPackedMetadataIndex;
	std::vector<VoxelGroupPackedMetadata> packedGroupMetadata;
	std::vector<CLodVoxelClusterRecord> packedClusterRecords;
	std::vector<CLodVoxelCubeRecord> packedCubeRecords;
	std::vector<CLodVoxelAttributeSample> packedAttributeSamples;
};

// Prebuilt / Cache payload types

struct ClusterLODPartRecord
{
	uint32_t groupBase = 0;
	uint32_t groupCount = 0;
	uint32_t nodeBase = 0;
	uint32_t nodeCount = 0;
	uint32_t transformBase = 0;
	uint32_t transformCount = 0;
	uint32_t instanceBase = 0;
	uint32_t instanceCount = 0;
	uint32_t rootNode = 0;
	uint32_t flags = 0;
};

static constexpr uint32_t CLOD_PART_RECORD_FLAG_ROOT = 1u << 0;

// Reserved: a part that has external parts of its own, opened through a GPU-composed nested portal. Merging flattens
// such parts instead (docs/merge_hierarchies.md), so registration rejects the flag for now.
static constexpr uint32_t CLOD_EXTERNAL_PART_FLAG_NESTED = 1u << 0;

// A part another CLod mesh holds, which this mesh's part portals open (CLOD_ASSEMBLY_INSTANCE_FLAG_PART_PORTAL, whose
// stackDepth is the part's index in ClusterLODPrebuiltData::externalParts). Parts are shared between the meshes that
// open them, so they stay in their own caches. The host address is what the static asset source resolves (asset key,
// material variant, part and mesh of the asset); the cache identity is what the portal's node numbering was built
// against, so a rebuilt part that no longer matches is caught.
struct ClusterLODExternalPart
{
	std::string sourceIdentifier;
	std::string primPath;
	std::string subsetName;
	uint64_t buildConfigHash = 0;
	std::string assetKey;
	uint64_t variantHash = 0;
	std::vector<std::byte> variant; // the host's encoding of the material variant; empty for the asset's own materials
	uint32_t partIndex = 0;
	uint32_t meshIndex = 0;
	uint32_t nodeCount = 0;
	uint32_t flags = 0;

	bool operator==(const ClusterLODExternalPart&) const = default;
};

struct ClusterLODPrebuiltData
{
	std::vector<ClusterLODGroup> groups;
	std::vector<ClusterLODGroupSegment> segments;
	std::vector<BoundingSphere> segmentBounds;
	BoundingSphere objectBoundingSphere{};
	std::vector<ClusterLODGroupChunk> groupChunks;
	std::vector<ClusterLODGroupDiskLocator> groupDiskLocators;
	std::vector<ClusterLODGroupDiskLocator> pageDiskLocators;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0;
	uint32_t voxelPageBase = 0;
	uint32_t voxelPageCount = 0;
	ClusterLODCacheSource cacheSource;
	std::vector<ClusterLODNode> nodes;
	std::vector<ClusterLODNodeSkinningInfo> nodeSkinningInfos;
	std::vector<uint32_t> nodeBoneIndices;
	uint32_t nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
	std::vector<ClusterLODNodeRangeAlloc> lodNodeRanges;
	std::vector<uint32_t> lodLevelRoots;
	std::vector<ClusterLODAssemblyTransform> assemblyTransforms;
	std::vector<ClusterLODAssemblyInstance> assemblyInstances;
	std::vector<ClusterLODAssemblyBoneRemap> assemblyBoneRemaps;
	std::vector<uint32_t> assemblyBoneRemapIndices;
	SkeletonArtifactReference assemblySkeletonArtifact;
	std::vector<ClusterLODPartRecord> partRecords;
	uint32_t rootPartIndex = 0;
	uint32_t maxDepth = 0;
	uint32_t maxTraversalDepth = 0;
	// Material slots (docs/merge_hierarchies.md). Each cluster names a part-local slot in its page descriptor (word 15,
	// CLodMeshletDescriptorMaterialSlot); a hierarchy root's palette supplies the material for slot + the material
	// base of the portal that reached the part, which is per assembly transform.
	uint32_t materialSlotCount = 1;
	std::vector<uint32_t> assemblyTransformMaterialBases;
	// A merged mesh's references (docs/merge_hierarchies.md): the parts its portals open, and for each palette slot the
	// external part whose material it takes. Empty for every other mesh.
	std::vector<ClusterLODExternalPart> externalParts;
	std::vector<uint32_t> paletteParts;
};

struct ClusterLODCacheBuildPayload
{
	const std::vector<std::vector<std::vector<std::byte>>>* groupPageBlobs = nullptr;
	const std::vector<std::vector<std::byte>>* meshPageBlobs = nullptr;
};

struct ClusterLODCacheBuildOwnedData
{
	std::vector<std::vector<std::vector<std::byte>>> groupPageBlobs;
	std::vector<std::vector<std::byte>> meshPageBlobs;
	VoxelGroupMapping voxelGroupMapping;

	ClusterLODCacheBuildPayload AsPayload() const {
		ClusterLODCacheBuildPayload payload{};
		payload.groupPageBlobs = &groupPageBlobs;
		payload.meshPageBlobs = &meshPageBlobs;
		return payload;
	}
};

struct ClusterLODPrebuildArtifacts
{
	ClusterLODPrebuiltData prebuiltData;
	ClusterLODCacheBuildOwnedData cacheBuildData;
};

struct ClusterLODVoxelGridOverride
{
	DirectX::XMFLOAT3 aabbMin{};
	DirectX::XMFLOAT3 aabbMax{};
	float voxelWidth = 0.0f;
	uint32_t resolution = 0u;
};

// Builder settings

enum class ClusterLODCoveragePreservationMode : uint8_t
{
	None,
	PrioritizeEdges,
	Voxel, // voxel groups replace the triangle groups above the switch: no triangle fallback (grass, voxel-only builds)
};

// Whether a triangle build (PrioritizeEdges or None) also gets a voxel branch above the levels that voxelize well, so
// that raster views with voxel output take voxels there while every other view keeps triangles (CLOD_GROUP_FLAG_BRANCH_*).
enum class ClusterLODVoxelForkMode : uint8_t
{
	Off,
	Auto,   // fork where voxels are shown to win: the seed test, a stalled triangle reduction and the surface test
	Forced, // fork wherever the seed test passes
};

struct ClusterLODBuilderSettings
{
	bool disableSloppyFallback = false;
	float sloppyFallbackErrorFactor = 2.0f;
	float lodErrorMergePrevious = 1.5f;
	float lodErrorMergeAdditive = 0.0f;
	uint32_t partitionSizeFloor = 8u;
	uint32_t nodeBoneLimit = CLOD_NODE_BONE_LIMIT_DEFAULT;
	bool preserveImportedNormals = true;
	bool enableNormalAttributeSimplification = true;
	float normalAttributeWeight = 1.0f;
	float simplifyTangentWeight = 0.01f;
	float simplifyTangentSignWeight = 0.5f;

	ClusterLODCoveragePreservationMode coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
	uint32_t voxelGridBaseResolution = 32u;
	uint32_t voxelMinResolution = 0u;
	uint32_t voxelRaysPerCell = 64u;
	float voxelFallbackScalingFactor = 0.75f;
	uint32_t voxelFallbackMaxRetryCount = 1000u;
	float voxelFallbackGrowthFactor = 1.1f;
	float voxelFallbackAcceptanceBias = 1.0f;
	float voxelFallbackOpacityThreshold = 0.0f;
	uint32_t voxelTailMaxLevels = 4u;
	float voxelTailGrowthFactor = 1.5f;
	// Most of a group's traced surface a proposed voxelization may lose before the group stays triangles. See VoxelSurfaceTrace.h.
	// 0 turns the test off.
	float voxelSurfaceLossLimit = 0.0f;
	bool doubleSidedVoxelSourceNormals = false;

	ClusterLODVoxelForkMode voxelFork = ClusterLODVoxelForkMode::Off;
	// Auto: a seed's triangle parents must keep more than this share of its triangles (the reduction stalls), or it must
	// have no parent (reduction stopped)...
	float voxelForkStallRatio = 0.6f;
	// ...while it still has this many triangles: a stall at the top of a mesh that reduced well saves too little to fork.
	uint32_t voxelForkMinTriangles = 512u;
	// Auto: only double-sided surfaces (doubleSidedVoxelSourceNormals), which is what foliage cards are. Solid surfaces
	// (architecture, rocks) pass the tests above wherever reduction stops at a large top, but read as blocks voxelized.
	bool voxelForkDoubleSidedOnly = true;
	// Auto: the surface-loss limit for fork seeds (voxelSurfaceLossLimit for this decision). 0 turns the test off.
	float voxelForkSurfaceLoss = 0.008f;
};

inline std::string GetClusterLODEnvironmentVariable(const char* name)
{
#if defined(_WIN32)
	char* value = nullptr;
	size_t valueLength = 0;
	if (_dupenv_s(&value, &valueLength, name) != 0 || value == nullptr)
	{
		return {};
	}
	std::string result(value);
	std::free(value);
	return result;
#else
	const char* value = std::getenv(name);
	return value != nullptr ? std::string(value) : std::string();
#endif
}

// BASICRENDERER_CLOD_FORCE_BRANCH=triangle|voxel makes raster views take that branch of forked DAGs, for A/B runs (the
// ray tracing view always takes triangles). 0: each view takes its own (CLOD_GROUP_FLAG_BRANCH_*).
inline uint32_t GetClusterLODForcedViewBranch()
{
	const std::string value = GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_FORCE_BRANCH");
	return value == "triangle" ? 1u : value == "voxel" ? 2u : 0u;
}

inline ClusterLODBuilderSettings ApplyClusterLODBuilderEnvironmentOverrides(ClusterLODBuilderSettings settings)
{
	const std::string modeString = GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_COVERAGE_PRESERVATION_MODE");
	if (!modeString.empty())
	{
		if (modeString == "none")
		{
			settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::None;
		}
		else if (modeString == "prioritizeEdges" || modeString == "prioritize-edges")
		{
			settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::PrioritizeEdges;
		}
		else if (modeString == "voxel")
		{
			settings.coveragePreservationMode = ClusterLODCoveragePreservationMode::Voxel;
		}
	}

	auto readUint = [](const char* name, uint32_t& outValue)
	{
		const std::string text = GetClusterLODEnvironmentVariable(name);
		if (!text.empty())
		{
			char* end = nullptr;
			const unsigned long value = std::strtoul(text.c_str(), &end, 10);
			if (end != text.c_str())
			{
				outValue = static_cast<uint32_t>(value);
			}
		}
	};

	auto readFloat = [](const char* name, float& outValue)
	{
		const std::string text = GetClusterLODEnvironmentVariable(name);
		if (!text.empty())
		{
			char* end = nullptr;
			const float value = std::strtof(text.c_str(), &end);
			if (end != text.c_str() && std::isfinite(value))
			{
				outValue = value;
			}
		}
	};

	auto readBool = [](const char* name, bool& outValue)
	{
		const std::string text = GetClusterLODEnvironmentVariable(name);
		if (text.empty())
		{
			return;
		}

		outValue = text == "1" || text == "true" || text == "on" || text == "yes";
	};

	readUint("BASICRENDERER_CLOD_VOXEL_GRID", settings.voxelGridBaseResolution);
	readUint("BASICRENDERER_CLOD_VOXEL_MIN_RES", settings.voxelMinResolution);
	readUint("BASICRENDERER_CLOD_VOXEL_RAYS", settings.voxelRaysPerCell);
	readUint("BASICRENDERER_CLOD_VOXEL_RETRIES", settings.voxelFallbackMaxRetryCount);
	readFloat("BASICRENDERER_CLOD_VOXEL_SCALE", settings.voxelFallbackScalingFactor);
	readFloat("BASICRENDERER_CLOD_VOXEL_GROWTH", settings.voxelFallbackGrowthFactor);
	readFloat("BASICRENDERER_CLOD_VOXEL_ACCEPTANCE_BIAS", settings.voxelFallbackAcceptanceBias);
	readFloat("BASICRENDERER_CLOD_VOXEL_OPACITY_THRESHOLD", settings.voxelFallbackOpacityThreshold);
	readUint("BASICRENDERER_CLOD_VOXEL_TAIL_LEVELS", settings.voxelTailMaxLevels);
	readUint("BASICRENDERER_CLOD_NODE_BONE_LIMIT", settings.nodeBoneLimit);
	readFloat("BASICRENDERER_CLOD_VOXEL_TAIL_GROWTH", settings.voxelTailGrowthFactor);
	readFloat("BASICRENDERER_CLOD_VOXEL_SURFACE_LOSS", settings.voxelSurfaceLossLimit);
	const std::string forkString = GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_FORK");
	if (forkString == "0" || forkString == "off")
	{
		settings.voxelFork = ClusterLODVoxelForkMode::Off;
	}
	else if (forkString == "auto")
	{
		settings.voxelFork = ClusterLODVoxelForkMode::Auto;
	}
	else if (forkString == "forced" || forkString == "1")
	{
		settings.voxelFork = ClusterLODVoxelForkMode::Forced;
	}
	readFloat("BASICRENDERER_CLOD_VOXEL_FORK_STALL", settings.voxelForkStallRatio);
	readUint("BASICRENDERER_CLOD_VOXEL_FORK_MIN_TRIANGLES", settings.voxelForkMinTriangles);
	readBool("BASICRENDERER_CLOD_VOXEL_FORK_DOUBLE_SIDED_ONLY", settings.voxelForkDoubleSidedOnly);
	readFloat("BASICRENDERER_CLOD_VOXEL_FORK_SURFACE_LOSS", settings.voxelForkSurfaceLoss);
	readBool("BASICRENDERER_CLOD_DISABLE_SLOPPY_FALLBACK", settings.disableSloppyFallback);
	readFloat("BASICRENDERER_CLOD_SLOPPY_ERROR_FACTOR", settings.sloppyFallbackErrorFactor);

	return settings;
}

// Runtime summary

struct ClusterLODRuntimeSummary
{
	struct GroupChunkHint
	{
		uint32_t groupVertexCount = 0;
		uint32_t meshletCount = 0;
		uint32_t meshletTrianglesByteCount = 0;
		uint32_t segmentCount = 0; // number of segments (= pages before re-binning) for per-segment paging
		uint32_t pageCount = 0;    // number of physical pages after bin-packing
	};

	struct GroupRange
	{
		uint32_t firstGroup = 0;
		uint32_t groupCount = 0;
		uint32_t branches = 3u; // the CLOD_BRANCH_* in which these groups are coarsest: pinned while one of them is active
	};

	std::vector<GroupChunkHint> groupChunkHints;
	std::vector<int32_t> parentGroupByLocal;
	// A forked DAG: each group's streaming parent while the voxel branch is active (parentGroupByLocal otherwise; the two
	// differ only for shared groups at the fork). Empty for an unforked mesh.
	std::vector<int32_t> voxelParentGroupByLocal;
	std::vector<float> groupErrorByLocal;
	std::vector<uint32_t> firstGroupVertexByLocal;
	std::vector<GroupRange> coarsestRanges;
};

// MeshIngestBuilder

// Accumulates raw vertex/index data and produces ClusterLOD artifacts
// or GPU Mesh objects (renderer-side).  The Build() method is only implemented
// in the renderer; BuildClusterLODArtifacts() has no GPU dependencies.
class MeshIngestBuilder {
public:
	MeshIngestBuilder(
		unsigned int vertexSize,
		unsigned int skinningVertexSize,
		unsigned int flags,
		ClusterLODBuilderSettings clusterLODBuilderSettings = {})
		: m_vertexSize(vertexSize),
		  m_skinningVertexSize(skinningVertexSize),
		  m_flags(flags),
		  m_clusterLODBuilderSettings(ApplyClusterLODBuilderEnvironmentOverrides(std::move(clusterLODBuilderSettings))) {}

	void ReserveVertices(size_t vertexCount) {
		m_vertices.reserve(vertexCount * static_cast<size_t>(m_vertexSize));
	}

	void ReserveIndices(size_t indexCount) {
		m_indices.reserve(indexCount);
	}

	void AppendVertexBytes(const std::byte* data, size_t byteCount) {
		if (byteCount != m_vertexSize) {
			throw std::runtime_error("MeshIngestBuilder vertex byte size mismatch");
		}
		m_vertices.insert(m_vertices.end(), data, data + byteCount);
	}

	void AppendSkinningVertexBytes(const std::byte* data, size_t byteCount) {
		if (m_skinningVertexSize == 0) {
			throw std::runtime_error("MeshIngestBuilder has no skinning vertex format");
		}
		if (byteCount != m_skinningVertexSize) {
			throw std::runtime_error("MeshIngestBuilder skinning vertex byte size mismatch");
		}
		constexpr size_t kMaxDebugVertices = 4u;
		constexpr size_t kMaxSkinInfluences = 8u;
		if (m_skinningDebugPositions.size() / 3u < kMaxDebugVertices) {
			const size_t jointsOffset = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3);
			const size_t weightsOffset = jointsOffset + sizeof(uint32_t) * kMaxSkinInfluences;
			if (byteCount >= weightsOffset + sizeof(float) * kMaxSkinInfluences) {
				const auto oldJointCount = m_skinningDebugJoints.size();
				const auto oldPositionCount = m_skinningDebugPositions.size();
				m_skinningDebugJoints.resize(oldJointCount + kMaxSkinInfluences);
				m_skinningDebugWeights.resize(oldJointCount + kMaxSkinInfluences);
				m_skinningDebugPositions.resize(oldPositionCount + 3u);
				m_skinningDebugNormals.resize(oldPositionCount + 3u);
				std::memcpy(m_skinningDebugPositions.data() + oldPositionCount, data, sizeof(float) * 3u);
				std::memcpy(m_skinningDebugNormals.data() + oldPositionCount, data + sizeof(DirectX::XMFLOAT3), sizeof(float) * 3u);
				std::memcpy(m_skinningDebugJoints.data() + oldJointCount, data + jointsOffset, sizeof(uint32_t) * kMaxSkinInfluences);
				std::memcpy(m_skinningDebugWeights.data() + oldJointCount, data + weightsOffset, sizeof(float) * kMaxSkinInfluences);
			}
		}
		m_skinningVertices.insert(m_skinningVertices.end(), data, data + byteCount);
	}

	void AppendIndex(uint32_t index) {
		m_indices.push_back(index);
	}

	void AppendIndices(const uint32_t* data, size_t count) {
		m_indices.insert(m_indices.end(), data, data + count);
	}

	void SetUvSets(std::vector<MeshUvSetData> uvSets) {
		m_uvSets = std::move(uvSets);
	}

	const std::vector<MeshUvSetData>& GetUvSets() const {
		return m_uvSets;
	}

	const std::vector<std::byte>& GetVertices() const {
		return m_vertices;
	}

	const std::vector<std::byte>& GetSkinningVertices() const {
		return m_skinningVertices;
	}

	const std::vector<uint32_t>& GetIndices() const {
		return m_indices;
	}

	unsigned int GetVertexSize() const {
		return m_vertexSize;
	}

	unsigned int GetSkinningVertexSize() const {
		return m_skinningVertexSize;
	}

	unsigned int GetFlags() const {
		return m_flags;
	}

	// GPU-side: creates a Mesh object with buffer views.
	// Only implemented in the renderer (Mesh.cpp); not available in headless builds.
	std::shared_ptr<Mesh> Build(
		const std::shared_ptr<Material>& material,
		std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD = std::nullopt,
		MeshCpuDataPolicy cpuDataPolicy = MeshCpuDataPolicy::Retain);

	// Headless: runs the full ClusterLOD build pipeline (CPU-only).
	ClusterLODPrebuildArtifacts BuildClusterLODArtifacts(
		const VoxelCoverageMaterialSampler* coverageMaterialSampler = nullptr) const;
	ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifacts(
		uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER) const;
	ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifacts(
		const ClusterLODVoxelGridOverride& grid,
		uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER) const;
	VoxelGroupPayload BuildVoxelOnlyPayload(const ClusterLODVoxelGridOverride& grid) const;
	VoxelGroupPayload BuildVoxelOnlyPayload(
		const ClusterLODVoxelGridOverride& grid,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler) const;
	static ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromPayload(
		const VoxelGroupPayload& payload,
		const ClusterLODBuilderSettings& settings,
		uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);

	void SetClusterLODBuilderSettings(const ClusterLODBuilderSettings& settings) {
		m_clusterLODBuilderSettings = settings;
	}

	const ClusterLODBuilderSettings& GetClusterLODBuilderSettings() const {
		return m_clusterLODBuilderSettings;
	}

private:
	unsigned int m_vertexSize = 0;
	unsigned int m_skinningVertexSize = 0;
	unsigned int m_flags = 0;
	std::vector<std::byte> m_vertices;
	std::vector<std::byte> m_skinningVertices;
	std::vector<uint32_t> m_skinningDebugJoints;
	std::vector<float> m_skinningDebugWeights;
	std::vector<float> m_skinningDebugPositions;
	std::vector<float> m_skinningDebugNormals;
	std::vector<uint32_t> m_indices;
	std::vector<MeshUvSetData> m_uvSets;
	ClusterLODBuilderSettings m_clusterLODBuilderSettings{};
};
