#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#include <directxmath.h>

#include "Mesh/ClusterLODTypes.h"

struct VoxelSourceCandidatePayload
{
	const VoxelGroupPayload* payload = nullptr;
	float expansionRadius = 0.0f;
};

struct VoxelSourcePayloadInstance
{
	const VoxelGroupPayload* payload = nullptr;
	ClusterLODAssemblyTransform localToTarget{};
	float expansionRadius = 0.0f;
	int32_t refinedGroupOverride = std::numeric_limits<int32_t>::min();
};

struct VoxelSourceTrianglePart
{
	const std::vector<std::byte>* vertices = nullptr;
	size_t vertexStrideBytes = 0;
	const std::vector<std::byte>* skinningVertices = nullptr;
	size_t skinningVertexStrideBytes = 0;
	const std::vector<uint32_t>* triangleIndices = nullptr;
};

struct VoxelSourceTriangleInstance
{
	uint32_t partIndex = 0;
	ClusterLODAssemblyTransform localToWorld{};
	int32_t refinedGroup = -1;
	std::span<const uint32_t> boneRemapIndices{};
};

struct VoxelSourceTriangleSample
{
	DirectX::XMFLOAT3 positions[3]{};
	DirectX::XMFLOAT3 normals[3]{};
	DirectX::XMFLOAT2 uvs[3]{};
	uint64_t uvChartId = std::numeric_limits<uint64_t>::max();
	uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
};

class VoxelSourceTriangleBVH
{
public:
	VoxelSourceTriangleBVH();
	~VoxelSourceTriangleBVH();

	void Build(
		const std::vector<std::byte>* vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>* triangleIndices,
		const std::vector<std::byte>* skinningVertices = nullptr,
		size_t skinningVertexStrideBytes = 0,
		const std::vector<int32_t>* triangleRefinedGroupIds = nullptr,
		bool doubleSidedTriangles = false,
		bool buildRefinedGroupScenes = true);
	void BuildInstanced(
		std::span<const VoxelSourceTrianglePart> parts,
		std::span<const VoxelSourceTriangleInstance> instances,
		bool doubleSidedTriangles = false);
	void SetRefinedGroupDomainMap(std::vector<std::vector<int32_t>> refinedGroupDomainMap);

	bool IsValid() const;
	bool IntersectNearest(
		int32_t refinedGroupFilter,
		const DirectX::XMFLOAT3& origin,
		const DirectX::XMFLOAT3& direction,
		float tMin,
		float tMax,
		uint32_t& outTriangleIndex,
		float& outT,
		float& outU,
		float& outV) const;
	bool GetTriangleSample(
		uint32_t triangleIndex,
		VoxelSourceTriangleSample& outSample) const;
	DirectX::XMFLOAT2 UvDensity() const { return m_uvDensity; }

	const std::vector<std::byte>* Vertices() const { return m_vertices; }
	size_t VertexStrideBytes() const { return m_vertexStrideBytes; }
	const std::vector<std::byte>* SkinningVertices() const { return m_skinningVertices; }
	size_t SkinningVertexStrideBytes() const { return m_skinningVertexStrideBytes; }
	const std::vector<uint32_t>* TriangleIndices() const { return m_triangleIndices; }
	const std::vector<int32_t>* TriangleRefinedGroupIds() const { return m_triangleRefinedGroupIds; }
	bool DoubleSidedTriangles() const { return m_doubleSidedTriangles; }

private:
	struct EmbreeScene;
	DirectX::XMFLOAT2 ComputeUvDensity() const;

	const std::vector<std::byte>* m_vertices = nullptr;
	size_t m_vertexStrideBytes = 0;
	const std::vector<std::byte>* m_skinningVertices = nullptr;
	size_t m_skinningVertexStrideBytes = 0;
	const std::vector<uint32_t>* m_triangleIndices = nullptr;
	const std::vector<int32_t>* m_triangleRefinedGroupIds = nullptr;
	bool m_doubleSidedTriangles = false;
	DirectX::XMFLOAT2 m_uvDensity = { 0.0f, 0.0f };
	std::vector<std::vector<int32_t>> m_refinedGroupDomainMap;
	std::unique_ptr<EmbreeScene> m_embreeScene;
};

// Input: triangle-based source geometry to voxelize into a single group.
struct VoxelizeTrianglesInput
{
	// Source vertices (interleaved, position at offset 0 as float3).
	const std::vector<std::byte>* vertices = nullptr;
	size_t vertexStrideBytes = 0;
	const std::vector<std::byte>* skinningVertices = nullptr;
	size_t skinningVertexStrideBytes = 0;

	// Source triangle indices into the vertex buffer (3 per triangle).
	const std::vector<uint32_t>* triangleIndices = nullptr;
	const std::vector<int32_t>* triangleRefinedGroupIds = nullptr;
	bool doubleSidedTriangles = false;

	// Optional authoritative original source geometry used only for per-cell
	// coverage tracing. Candidate generation still uses triangleIndices and
	// source/candidate voxel payloads.
	const VoxelSourceTriangleBVH* coverageSourceTriangles = nullptr;
	const VoxelCoverageMaterialSampler* coverageMaterialSampler = nullptr;
	int32_t terminalCoverageRefinedGroupOverride = std::numeric_limits<int32_t>::min();

	// Optional already-voxelized sources. These only define candidate output
	// cells; coverage is evaluated from triangle sources.
	const std::vector<const VoxelGroupPayload*>* sourceVoxelPayloads = nullptr;
	const std::vector<VoxelSourcePayloadInstance>* sourceVoxelPayloadInstances = nullptr;

	// Optional already-voxelized sources used only to define candidate output
	// cells. Coverage for these candidates is evaluated from triangle sources.
	const std::vector<VoxelSourceCandidatePayload>* candidateVoxelPayloads = nullptr;
	const std::vector<VoxelSourcePayloadInstance>* candidateVoxelPayloadInstances = nullptr;

	// World-space AABB of the geometry to voxelize.
	DirectX::XMFLOAT3 aabbMin{};
	DirectX::XMFLOAT3 aabbMax{};
	float voxelWidth = 0.0f;

	// Resolution (cells per axis) for the output voxel grid.
	uint32_t resolution = 32;

	// Number of rays cast per active cell for opacity sampling.
	uint32_t raysPerCell = 64;

	// Output selection. Both default to true for compatibility with existing callers.
	bool emitRenderPayload = true;
	bool emitSourcePayload = true;

};

struct VoxelizeTrianglesResult
{
	// Cells used for rendering this group after coverage pruning.
	VoxelGroupPayload renderPayload;
	// Pre-prune candidate cells retained so coarser parents can reintroduce
	// cells trimmed from this group's render payload.
	VoxelGroupPayload sourcePayload;
	uint32_t triangleCandidateCellCount = 0;
	uint32_t voxelCandidateCellCount = 0;
	uint32_t candidateCellCount = 0;
	uint32_t positiveCoverageCellCount = 0;
	float totalCoverage = 0.0f;
	float maxCoverage = 0.0f;
	uint32_t prunedCellCount = 0;
	uint64_t sourceCoverageQueryCount = 0;
	uint64_t sourceCoverageTriangleCandidateCount = 0;
	uint64_t sourceCoverageTriangleTestCount = 0;
	uint64_t sourceCoverageOutOfCellRejectionCount = 0;

	struct RefinedGroupStats
	{
		int32_t refinedGroup = -1;
		uint32_t candidateKeys = 0;
		uint32_t triangleOwnedCells = 0;
		uint32_t candidateOwnedCells = 0;
		uint32_t candidateOnlyCells = 0;
		uint32_t positiveCoverageCells = 0;
		uint32_t zeroCoverageDroppedCells = 0;
		uint32_t emittedSourceCells = 0;
		float totalCoverage = 0.0f;
		float maxCoverage = 0.0f;
	};
	std::vector<RefinedGroupStats> refinedGroupStats;
};

// Voxelize a triangle set into a single VoxelGroupPayload.
// Rasterizes triangles into a 3D grid via triangle-AABB overlap (SAT),
// then casts rays for per-cell opacity sampling.
VoxelGroupPayload VoxelizeTriangles(const VoxelizeTrianglesInput& input);
VoxelizeTrianglesResult VoxelizeTrianglesDetailed(const VoxelizeTrianglesInput& input);

struct PackVoxelGroupInput
{
	const VoxelGroupPayload* payload = nullptr;
	float voxelError = 0.0f;
	float opacityThreshold = 0.0f;
	// Fallback for static content or cells with no usable skinning data.
	uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	uint32_t firstCube = 0;
	uint32_t firstAttribute = 0;
};

struct PackedVoxelGroupBuildResult
{
	VoxelGroupPackedMetadata metadata{};
	std::vector<CLodVoxelClusterRecord> clusterRecords;
	std::vector<CLodVoxelCubeRecord> cubeRecords;
	std::vector<CLodVoxelAttributeSample> attributeSamples;
};

PackedVoxelGroupBuildResult PackVoxelGroupToCubes(const PackVoxelGroupInput& input);
void BuildVoxelClustersFromCubes(PackedVoxelGroupBuildResult& packed, uint32_t maxCubesPerCluster);

// Morton sorting: returns a permutation of [0, count) that places positions
// in 3D Morton (Z-order) order within the given AABB.
std::vector<uint32_t> MortonSort(
	const DirectX::XMFLOAT3* positions,
	uint32_t count,
	const DirectX::XMFLOAT3& aabbMin,
	const DirectX::XMFLOAT3& aabbMax);

// Spatial merging: given a Morton-sorted list of group indices and their AABBs,
// greedily merge consecutive groups into batches of <= maxFanout.
// Returns: vector of batches, each batch is a vector of original group indices.
std::vector<std::vector<uint32_t>> MergeGroupsSpatial(
	const std::vector<uint32_t>& sortedGroupIndices,
	uint32_t maxFanout);
