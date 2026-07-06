#pragma once

// Standalone header containing all ClusterLOD data types and the
// MeshIngestBuilder class.  No GPU / RHI / DeletionManager / BufferView
// dependencies - only standard library, DirectXMath, meshoptimizer, and the
// CLod shader types header.

#include <cstdint>
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

#include "Mesh/ClusterLODShaderTypes.h"
#include "Mesh/VertexFlags.h"
#include "Import/MeshData.h"

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
	uint32_t reserved0 = 0;
	uint32_t reserved1 = 0;
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
	std::vector<ClusterLODNodeRangeAlloc> lodNodeRanges;
	std::vector<uint32_t> lodLevelRoots;
	std::vector<ClusterLODAssemblyTransform> assemblyTransforms;
	std::vector<ClusterLODAssemblyInstance> assemblyInstances;
	uint32_t maxDepth = 0;
	uint32_t maxTraversalDepth = 0;
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

enum class ClusterLODVoxelFallbackMode : uint8_t
{
	Auto,
	MeshOnly,
	VoxelOnly,
};

struct ClusterLODBuilderSettings
{
	bool disableSloppyFallback = false;
	float sloppyFallbackErrorFactor = 2.0f;
	float lodErrorMergePrevious = 1.5f;
	float lodErrorMergeAdditive = 0.0f;
	uint32_t partitionSizeFloor = 8u;
	bool preserveImportedNormals = true;
	bool enableNormalAttributeSimplification = true;
	float normalAttributeWeight = 1.0f;
	float simplifyTangentWeight = 0.01f;
	float simplifyTangentSignWeight = 0.5f;

	bool enableVoxelFallback = true;
	ClusterLODVoxelFallbackMode voxelFallbackMode = ClusterLODVoxelFallbackMode::Auto;
	uint32_t voxelGridBaseResolution = 32u;
	uint32_t voxelMinResolution = 0u;
	uint32_t voxelRaysPerCell = 64u;
	float voxelFallbackScalingFactor = 0.75f;
	uint32_t voxelFallbackMaxRetryCount = 1000u;
	float voxelFallbackGrowthFactor = 1.1f;
	float voxelFallbackAcceptanceBias = 1.0f;
	float voxelFallbackOpacityThreshold = 0.0f;
	bool doubleSidedVoxelSourceNormals = false;
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

inline ClusterLODBuilderSettings ApplyClusterLODBuilderEnvironmentOverrides(ClusterLODBuilderSettings settings)
{
	const std::string modeString = GetClusterLODEnvironmentVariable("BASICRENDERER_CLOD_VOXEL_MODE");
	if (!modeString.empty())
	{
		if (modeString == "mesh" || modeString == "mesh-only")
		{
			settings.enableVoxelFallback = false;
			settings.voxelFallbackMode = ClusterLODVoxelFallbackMode::MeshOnly;
		}
		else if (modeString == "auto")
		{
			settings.enableVoxelFallback = true;
			settings.voxelFallbackMode = ClusterLODVoxelFallbackMode::Auto;
		}
		else if (modeString == "voxel" || modeString == "voxel-only")
		{
			settings.enableVoxelFallback = true;
			settings.voxelFallbackMode = ClusterLODVoxelFallbackMode::VoxelOnly;
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
	};

	std::vector<GroupChunkHint> groupChunkHints;
	std::vector<int32_t> parentGroupByLocal;
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
