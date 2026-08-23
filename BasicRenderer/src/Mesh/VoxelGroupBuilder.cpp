#include "Mesh/VoxelGroupBuilder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <embree4/rtcore.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Mesh/SGGX.h"
#include "Mesh/VertexLayout.h"

uint32_t ComputeVoxelClusterCullMetadata(
	std::span<const CLodVoxelCubeRecord> cubeRecords,
	uint32_t firstCube,
	uint32_t cubeCount)
{
	uint32_t flags = CLOD_CLUSTER_KIND_VOXEL;
	bool hasSkinned = false;
	bool hasRigid = false;
	const uint32_t endCube = std::min<uint32_t>(
		static_cast<uint32_t>(cubeRecords.size()),
		firstCube + cubeCount);
	for (uint32_t cubeIndex = firstCube; cubeIndex < endCube; ++cubeIndex)
	{
		if (cubeRecords[cubeIndex].dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
		{
			hasSkinned = true;
		}
		else
		{
			hasRigid = true;
		}
	}
	if (hasSkinned)
		flags |= (CLOD_CLUSTER_CULL_FLAG_ANIMATED | CLOD_CLUSTER_CULL_FLAG_BONE_OVERFLOW) << CLOD_CLUSTER_CULL_FLAGS_SHIFT;
	if (hasRigid)
		flags |= CLOD_CLUSTER_CULL_FLAG_RIGID_COMPONENT << CLOD_CLUSTER_CULL_FLAGS_SHIFT;
	return flags;
}

namespace
{
	// Helpers

	using br::mesh::sggx::CompressSGGXToAxial;
	using br::mesh::sggx::EncodeAxialSGGX;
	using br::mesh::sggx::Float3;
	using br::mesh::sggx::SGGXFromNormal;
	using br::mesh::sggx::SymmetricMatrix3;
	using br::mesh::sggx::BuildSGGXFromNormals;
	using br::mesh::sggx::BuildSGGXFromWeightedNormals;

	std::pmr::memory_resource*& CurrentVoxelizationScratchResource()
	{
		thread_local std::pmr::memory_resource* resource = nullptr;
		return resource;
	}

	std::pmr::memory_resource* GetVoxelizationScratchResource()
	{
		std::pmr::memory_resource* resource = CurrentVoxelizationScratchResource();
		return resource != nullptr ? resource : std::pmr::get_default_resource();
	}

	struct ScopedVoxelizationScratchResource
	{
		std::pmr::memory_resource* previous = nullptr;

		explicit ScopedVoxelizationScratchResource(std::pmr::memory_resource* resource)
		{
			previous = CurrentVoxelizationScratchResource();
			CurrentVoxelizationScratchResource() = resource;
		}

		~ScopedVoxelizationScratchResource()
		{
			CurrentVoxelizationScratchResource() = previous;
		}
	};

	using PmrUInt32Vector = std::pmr::vector<uint32_t>;
	using PmrUInt64Vector = std::pmr::vector<uint64_t>;
	using PmrInt32Vector = std::pmr::vector<int32_t>;

	struct ScratchTriCell
	{
		uint32_t generation = 0u;
		PmrUInt32Vector triangleIndices{ GetVoxelizationScratchResource() };
	};

	using ScratchCellTriMap = std::pmr::unordered_map<uint64_t, ScratchTriCell>;

	struct PackedSkinningInfluences
	{
		DirectX::XMUINT4 joints0{ 0, 0, 0, 0 };
		DirectX::XMUINT4 joints1{ 0, 0, 0, 0 };
		DirectX::XMFLOAT4 weights0{ 0, 0, 0, 0 };
		DirectX::XMFLOAT4 weights1{ 0, 0, 0, 0 };
	};

	uint32_t PackVoxelCubeActiveBounds(uint32_t minX, uint32_t minY, uint32_t minZ, uint32_t maxX, uint32_t maxY, uint32_t maxZ)
	{
		return
			((minX & 0x3u) << 0u) |
			((minY & 0x3u) << 2u) |
			((minZ & 0x3u) << 4u) |
			((maxX & 0x3u) << 6u) |
			((maxY & 0x3u) << 8u) |
			((maxZ & 0x3u) << 10u);
	}

	Float3 ReadPosition(const std::vector<std::byte>& vertices, size_t stride, uint32_t index)
	{
		Float3 p;
		const size_t offset = static_cast<size_t>(index) * stride;
		std::memcpy(&p.x, vertices.data() + offset, sizeof(float));
		std::memcpy(&p.y, vertices.data() + offset + sizeof(float), sizeof(float));
		std::memcpy(&p.z, vertices.data() + offset + sizeof(float) * 2, sizeof(float));
		return p;
	}

	Float3 ReadNormal(const std::vector<std::byte>& vertices, size_t stride, uint32_t index)
	{
		if (stride < MeshVertexLayout::NormalOffset + sizeof(float) * 3u)
		{
			return Float3(0.0f, 0.0f, 0.0f);
		}

		Float3 n;
		const size_t offset = static_cast<size_t>(index) * stride + MeshVertexLayout::NormalOffset;
		std::memcpy(&n.x, vertices.data() + offset, sizeof(float));
		std::memcpy(&n.y, vertices.data() + offset + sizeof(float), sizeof(float));
		std::memcpy(&n.z, vertices.data() + offset + sizeof(float) * 2, sizeof(float));
		return n;
	}

	DirectX::XMFLOAT2 ReadTexcoord(const std::vector<std::byte>& vertices, size_t stride, uint32_t index)
	{
		const size_t texcoordOffset = MeshVertexLayout::TexcoordOffset(VertexFlags::VERTEX_TEXCOORDS);
		if (stride < texcoordOffset + sizeof(float) * 2u || stride == 0u || index >= vertices.size() / stride)
		{
			return DirectX::XMFLOAT2(0.0f, 0.0f);
		}

		DirectX::XMFLOAT2 uv{};
		const size_t offset = static_cast<size_t>(index) * stride + texcoordOffset;
		std::memcpy(&uv.x, vertices.data() + offset, sizeof(float));
		std::memcpy(&uv.y, vertices.data() + offset + sizeof(float), sizeof(float));
		return uv;
	}

	DirectX::XMFLOAT2 InterpolateUv(const DirectX::XMFLOAT2& uv0, const DirectX::XMFLOAT2& uv1, const DirectX::XMFLOAT2& uv2, float bary0, float bary1, float bary2)
	{
		return DirectX::XMFLOAT2(
			uv0.x * bary0 + uv1.x * bary1 + uv2.x * bary2,
			uv0.y * bary0 + uv1.y * bary1 + uv2.y * bary2);
	}

	struct WeldCellKey
	{
		int64_t x = 0;
		int64_t y = 0;
		int64_t z = 0;

		bool operator==(const WeldCellKey&) const = default;
	};

	struct WeldCellKeyHash
	{
		size_t operator()(const WeldCellKey& key) const
		{
			size_t seed = std::hash<int64_t>{}(key.x);
			seed ^= std::hash<int64_t>{}(key.y) + 0x9E3779B9u + (seed << 6u) + (seed >> 2u);
			seed ^= std::hash<int64_t>{}(key.z) + 0x9E3779B9u + (seed << 6u) + (seed >> 2u);
			return seed;
		}
	};

	struct WeldedEdgeKey
	{
		uint32_t a = 0;
		uint32_t b = 0;

		bool operator==(const WeldedEdgeKey&) const = default;
	};

	struct WeldedEdgeKeyHash
	{
		size_t operator()(const WeldedEdgeKey& key) const
		{
			return (static_cast<size_t>(key.a) << 32u) ^ static_cast<size_t>(key.b);
		}
	};

	struct UvChartEdge
	{
		uint32_t triangleIndex = 0;
		DirectX::XMFLOAT2 uvA{};
		DirectX::XMFLOAT2 uvB{};
	};

	class DisjointSet
	{
	public:
		explicit DisjointSet(uint32_t count) : m_parent(count), m_rank(count, 0u)
		{
			std::iota(m_parent.begin(), m_parent.end(), 0u);
		}

		uint32_t Find(uint32_t value)
		{
			uint32_t root = value;
			while (m_parent[root] != root)
			{
				root = m_parent[root];
			}
			while (m_parent[value] != value)
			{
				const uint32_t next = m_parent[value];
				m_parent[value] = root;
				value = next;
			}
			return root;
		}

		void Unite(uint32_t lhs, uint32_t rhs)
		{
			lhs = Find(lhs);
			rhs = Find(rhs);
			if (lhs == rhs)
			{
				return;
			}
			if (m_rank[lhs] < m_rank[rhs])
			{
				std::swap(lhs, rhs);
			}
			m_parent[rhs] = lhs;
			if (m_rank[lhs] == m_rank[rhs])
			{
				++m_rank[lhs];
			}
		}

	private:
		std::vector<uint32_t> m_parent;
		std::vector<uint8_t> m_rank;
	};

	std::vector<uint32_t> BuildTriangleUvChartIds(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& triangleIndices)
	{
		const size_t vertexCount = vertexStrideBytes > 0u ? vertices.size() / vertexStrideBytes : 0u;
		const uint32_t triangleCount = static_cast<uint32_t>(triangleIndices.size() / 3u);
		std::vector<uint32_t> chartIds(triangleCount);
		std::iota(chartIds.begin(), chartIds.end(), 0u);
		if (vertexCount == 0u || triangleCount == 0u)
		{
			return chartIds;
		}

		Float3 boundsMin(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		Float3 boundsMax(
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max());
		std::vector<Float3> positions(vertexCount);
		for (uint32_t vertexIndex = 0u; vertexIndex < vertexCount; ++vertexIndex)
		{
			const Float3 position = ReadPosition(vertices, vertexStrideBytes, vertexIndex);
			positions[vertexIndex] = position;
			if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z))
			{
				boundsMin.x = std::min(boundsMin.x, position.x);
				boundsMin.y = std::min(boundsMin.y, position.y);
				boundsMin.z = std::min(boundsMin.z, position.z);
				boundsMax.x = std::max(boundsMax.x, position.x);
				boundsMax.y = std::max(boundsMax.y, position.y);
				boundsMax.z = std::max(boundsMax.z, position.z);
			}
		}
		const Float3 boundsExtent = boundsMax - boundsMin;
		const float boundsDiagonal = std::sqrt(std::max(0.0f, boundsExtent.lengthSq()));
		const float positionEpsilon = std::max(1.0e-6f, std::isfinite(boundsDiagonal) ? boundsDiagonal * 1.0e-6f : 1.0e-6f);
		const float invPositionEpsilon = 1.0f / positionEpsilon;

		std::unordered_map<WeldCellKey, std::vector<uint32_t>, WeldCellKeyHash> weldGrid;
		weldGrid.reserve(vertexCount);
		std::vector<Float3> weldPositions;
		std::vector<uint32_t> vertexWeldIds(vertexCount);
		for (uint32_t vertexIndex = 0u; vertexIndex < vertexCount; ++vertexIndex)
		{
			const Float3 position = positions[vertexIndex];
			if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
			{
				vertexWeldIds[vertexIndex] = static_cast<uint32_t>(weldPositions.size());
				weldPositions.push_back(position);
				continue;
			}
			const WeldCellKey cell{
				static_cast<int64_t>(std::floor(position.x * invPositionEpsilon)),
				static_cast<int64_t>(std::floor(position.y * invPositionEpsilon)),
				static_cast<int64_t>(std::floor(position.z * invPositionEpsilon)) };
			uint32_t weldId = std::numeric_limits<uint32_t>::max();
			for (int32_t dz = -1; dz <= 1 && weldId == std::numeric_limits<uint32_t>::max(); ++dz)
			{
				for (int32_t dy = -1; dy <= 1 && weldId == std::numeric_limits<uint32_t>::max(); ++dy)
				{
					for (int32_t dx = -1; dx <= 1 && weldId == std::numeric_limits<uint32_t>::max(); ++dx)
					{
						const auto it = weldGrid.find(WeldCellKey{ cell.x + dx, cell.y + dy, cell.z + dz });
						if (it == weldGrid.end())
						{
							continue;
						}
						for (uint32_t candidate : it->second)
						{
							const Float3 delta = weldPositions[candidate] - position;
							if (delta.lengthSq() <= positionEpsilon * positionEpsilon)
							{
								weldId = candidate;
								break;
							}
						}
					}
				}
			}
			if (weldId == std::numeric_limits<uint32_t>::max())
			{
				weldId = static_cast<uint32_t>(weldPositions.size());
				weldPositions.push_back(position);
				weldGrid[cell].push_back(weldId);
			}
			vertexWeldIds[vertexIndex] = weldId;
		}

		constexpr float kUvContinuityEpsilon = 1.0e-5f;
		auto uvMatches = [](const DirectX::XMFLOAT2& lhs, const DirectX::XMFLOAT2& rhs)
		{
			return std::isfinite(lhs.x) && std::isfinite(lhs.y) &&
				std::isfinite(rhs.x) && std::isfinite(rhs.y) &&
				std::abs(lhs.x - rhs.x) <= kUvContinuityEpsilon &&
				std::abs(lhs.y - rhs.y) <= kUvContinuityEpsilon;
		};

		DisjointSet charts(triangleCount);
		std::unordered_map<WeldedEdgeKey, std::vector<UvChartEdge>, WeldedEdgeKeyHash> edges;
		edges.reserve(static_cast<size_t>(triangleCount) * 3u);
		for (uint32_t triangleIndex = 0u; triangleIndex < triangleCount; ++triangleIndex)
		{
			const uint32_t indices[3] = {
				triangleIndices[static_cast<size_t>(triangleIndex) * 3u + 0u],
				triangleIndices[static_cast<size_t>(triangleIndex) * 3u + 1u],
				triangleIndices[static_cast<size_t>(triangleIndex) * 3u + 2u] };
			for (uint32_t edgeIndex = 0u; edgeIndex < 3u; ++edgeIndex)
			{
				const uint32_t vertexA = indices[edgeIndex];
				const uint32_t vertexB = indices[(edgeIndex + 1u) % 3u];
				if (vertexA >= vertexCount || vertexB >= vertexCount)
				{
					continue;
				}
				uint32_t weldA = vertexWeldIds[vertexA];
				uint32_t weldB = vertexWeldIds[vertexB];
				DirectX::XMFLOAT2 uvA = ReadTexcoord(vertices, vertexStrideBytes, vertexA);
				DirectX::XMFLOAT2 uvB = ReadTexcoord(vertices, vertexStrideBytes, vertexB);
				if (weldA == weldB)
				{
					continue;
				}
				if (weldB < weldA)
				{
					std::swap(weldA, weldB);
					std::swap(uvA, uvB);
				}
				const WeldedEdgeKey key{ weldA, weldB };
				std::vector<UvChartEdge>& matches = edges[key];
				for (const UvChartEdge& match : matches)
				{
					if (uvMatches(uvA, match.uvA) && uvMatches(uvB, match.uvB))
					{
						charts.Unite(triangleIndex, match.triangleIndex);
					}
				}
				matches.push_back(UvChartEdge{ triangleIndex, uvA, uvB });
			}
		}

		std::vector<uint32_t> componentMinimum(triangleCount, std::numeric_limits<uint32_t>::max());
		for (uint32_t triangleIndex = 0u; triangleIndex < triangleCount; ++triangleIndex)
		{
			const uint32_t root = charts.Find(triangleIndex);
			componentMinimum[root] = std::min(componentMinimum[root], triangleIndex);
		}
		for (uint32_t triangleIndex = 0u; triangleIndex < triangleCount; ++triangleIndex)
		{
			chartIds[triangleIndex] = componentMinimum[charts.Find(triangleIndex)];
		}
		return chartIds;
	}

	Float3 ToFloat3(const DirectX::XMFLOAT3& v) { return { v.x, v.y, v.z }; }
	DirectX::XMFLOAT3 ToXM(const Float3& v) { return { v.x, v.y, v.z }; }
	Float3 TriangleNormal(const Float3& a, const Float3& b, const Float3& c)
	{
		return (b - a).cross(c - a).normalized();
	}

	Float3 OrientHitNormalForSidedness(const Float3& normal, const Float3& geometricNormal, const Float3& rayDirection, bool doubleSided)
	{
		if (doubleSided)
		{
			return normal * -1.0f;
		}
		return normal;
	}

	bool PointInsideAABB(const Float3& p, const Float3& boxMin, const Float3& boxMax, float epsilon)
	{
		return p.x >= boxMin.x - epsilon && p.x <= boxMax.x + epsilon &&
			p.y >= boxMin.y - epsilon && p.y <= boxMax.y + epsilon &&
			p.z >= boxMin.z - epsilon && p.z <= boxMax.z + epsilon;
	}

	bool ReadSkinningInfluences(
		const std::vector<std::byte>& skinningVertices,
		size_t skinningVertexStrideBytes,
		uint32_t vertexIndex,
		PackedSkinningInfluences& outInfluences)
	{
		constexpr size_t kSkinningInfluenceOffset = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT3);
		if (skinningVertexStrideBytes < kSkinningInfluenceOffset + sizeof(PackedSkinningInfluences))
		{
			return false;
		}

		const size_t vertexCount = skinningVertices.size() / skinningVertexStrideBytes;
		if (vertexIndex >= vertexCount)
		{
			return false;
		}

		const size_t sourceByteOffset = static_cast<size_t>(vertexIndex) * skinningVertexStrideBytes + kSkinningInfluenceOffset;
		std::memcpy(&outInfluences, skinningVertices.data() + sourceByteOffset, sizeof(PackedSkinningInfluences));
		return true;
	}

	void AccumulateInfluenceSet(
		const DirectX::XMUINT4& joints,
		const DirectX::XMFLOAT4& weights,
		std::unordered_map<uint32_t, float>& boneWeights)
	{
		const uint32_t jointValues[4] = { joints.x, joints.y, joints.z, joints.w };
		const float weightValues[4] = { weights.x, weights.y, weights.z, weights.w };
		for (uint32_t influenceIndex = 0; influenceIndex < 4u; ++influenceIndex)
		{
			if (weightValues[influenceIndex] <= 0.0f)
			{
				continue;
			}

			boneWeights[jointValues[influenceIndex]] += weightValues[influenceIndex];
		}
	}

	uint32_t SelectDominantBoneIndex(const std::unordered_map<uint32_t, float>& boneWeights)
	{
		uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		float dominantWeight = 0.0f;
		for (const auto& [boneIndex, weight] : boneWeights)
		{
			if (weight <= dominantWeight)
			{
				continue;
			}

			dominantBoneIndex = boneIndex;
			dominantWeight = weight;
		}

		return dominantBoneIndex;
	}

	uint32_t ComputeDominantBoneIndexForSourceTriangle(
		const VoxelSourceTriangleBVH& sourceTriangles,
		uint32_t triangleIndex)
	{
		const std::vector<std::byte>* skinningVertices = sourceTriangles.SkinningVertices();
		const std::vector<uint32_t>* triangleIndices = sourceTriangles.TriangleIndices();
		if (skinningVertices == nullptr || skinningVertices->empty() || sourceTriangles.SkinningVertexStrideBytes() == 0u || triangleIndices == nullptr)
		{
			return CLOD_VOXEL_STATIC_BONE_INDEX;
		}

		const size_t triangleBase = static_cast<size_t>(triangleIndex) * 3u;
		if (triangleBase + 2u >= triangleIndices->size())
		{
			return CLOD_VOXEL_STATIC_BONE_INDEX;
		}

		std::unordered_map<uint32_t, float> boneWeights;
		boneWeights.reserve(8);
		for (uint32_t corner = 0; corner < 3u; ++corner)
		{
			PackedSkinningInfluences influences{};
			if (!ReadSkinningInfluences(*skinningVertices, sourceTriangles.SkinningVertexStrideBytes(), (*triangleIndices)[triangleBase + corner], influences))
			{
				continue;
			}

			AccumulateInfluenceSet(influences.joints0, influences.weights0, boneWeights);
			AccumulateInfluenceSet(influences.joints1, influences.weights1, boneWeights);
		}

		return SelectDominantBoneIndex(boneWeights);
	}

	// Cell-key packing (supports resolutions up to 65535)
	uint64_t PackCell(uint32_t cx, uint32_t cy, uint32_t cz)
	{
		return static_cast<uint64_t>(cx) |
			(static_cast<uint64_t>(cy) << 16) |
			(static_cast<uint64_t>(cz) << 32);
	}

	void UnpackCell(uint64_t key, uint32_t& cx, uint32_t& cy, uint32_t& cz)
	{
		cx = static_cast<uint32_t>(key & 0xFFFF);
		cy = static_cast<uint32_t>((key >> 16) & 0xFFFF);
		cz = static_cast<uint32_t>((key >> 32) & 0xFFFF);
	}

	// Grid coordinate helpers
	uint32_t ToCellCoord(float val, float minVal, float invCellSize, uint32_t res)
	{
		int32_t c = static_cast<int32_t>(std::floor((val - minVal) * invCellSize));
		return static_cast<uint32_t>(std::max(0, std::min(c, static_cast<int32_t>(res) - 1)));
	}

	uint32_t ToCellCoordExclusiveMax(float val, float minVal, float invCellSize, uint32_t res)
	{
		int32_t c = static_cast<int32_t>(std::ceil((val - minVal) * invCellSize)) - 1;
		return static_cast<uint32_t>(std::max(0, std::min(c, static_cast<int32_t>(res) - 1)));
	}

	// Triangle-AABB overlap test (Separating Axis Theorem- Akenine-Möller method)
	bool TriangleAABBOverlap(const Float3& v0, const Float3& v1, const Float3& v2,
		const Float3& boxCenter, const Float3& boxHalfSize)
	{
		constexpr float kOverlapEpsilon = 1.0e-6f;
		const Float3 a = v0 - boxCenter;
		const Float3 b = v1 - boxCenter;
		const Float3 c = v2 - boxCenter;

		const Float3 edges[3] = { b - a, c - b, a - c };

		const Float3 boxAxes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 3; ++j)
			{
				Float3 axis = edges[i].cross(boxAxes[j]);
				if (axis.lengthSq() < 1e-12f)
					continue;
				float pa = a.dot(axis), pb = b.dot(axis), pc = c.dot(axis);
				float triMin = std::min({ pa, pb, pc });
				float triMax = std::max({ pa, pb, pc });
				float r = boxHalfSize.x * std::abs(axis.x) +
					boxHalfSize.y * std::abs(axis.y) +
					boxHalfSize.z * std::abs(axis.z);
				if (triMin > r + kOverlapEpsilon || triMax < -r - kOverlapEpsilon)
					return false;
			}
		}

		if (std::min({ a.x, b.x, c.x }) > boxHalfSize.x + kOverlapEpsilon || std::max({ a.x, b.x, c.x }) < -boxHalfSize.x - kOverlapEpsilon) return false;
		if (std::min({ a.y, b.y, c.y }) > boxHalfSize.y + kOverlapEpsilon || std::max({ a.y, b.y, c.y }) < -boxHalfSize.y - kOverlapEpsilon) return false;
		if (std::min({ a.z, b.z, c.z }) > boxHalfSize.z + kOverlapEpsilon || std::max({ a.z, b.z, c.z }) < -boxHalfSize.z - kOverlapEpsilon) return false;

		Float3 triNormal = edges[0].cross(edges[1]);
		float d = triNormal.dot(a);
		float r = boxHalfSize.x * std::abs(triNormal.x) +
			boxHalfSize.y * std::abs(triNormal.y) +
			boxHalfSize.z * std::abs(triNormal.z);
		if (d > r + kOverlapEpsilon || d < -r - kOverlapEpsilon)
			return false;

		return true;
	}

	// Deterministic ray set generation for a unit cube [0,1]^3
	struct Ray
	{
		Float3 origin;
		Float3 direction;
	};

	uint32_t HashVoxelCellSampleSeed(uint32_t baseSeed, uint64_t cellKey, int32_t refinedGroup)
	{
		uint64_t x = cellKey ^ (uint64_t{ static_cast<uint32_t>(refinedGroup) } << 32u) ^ baseSeed;
		x ^= x >> 33u;
		x *= 0xff51afd7ed558ccdull;
		x ^= x >> 33u;
		x *= 0xc4ceb9fe1a85ec53ull;
		x ^= x >> 33u;
		return static_cast<uint32_t>(x) ^ static_cast<uint32_t>(x >> 32u);
	}

	void GenerateCellRays(uint32_t rayCount, uint32_t seed, std::vector<Ray>& rays)
	{
		std::mt19937 rng(seed);
		std::uniform_real_distribution<float> dist(0.0f, 1.0f);

		rays.clear();
		rays.reserve(rayCount);

		const uint32_t faceCount = 6u;
		const uint32_t raysPerFace = rayCount / faceCount;
		const uint32_t extraRays = rayCount - raysPerFace * faceCount;

		for (uint32_t face = 0; face < faceCount; ++face)
		{
			const uint32_t faceRayCount = raysPerFace + (face < extraRays ? 1u : 0u);
			for (uint32_t i = 0; i < faceRayCount; ++i)
			{
				Float3 origin{};
				Float3 direction{};

				float u = dist(rng);
				float v = dist(rng);

				switch (face)
				{
				case 0:
					origin = { 0.0f, u, v };
					direction = { 1.0f, 0.0f, 0.0f };
					break;
				case 1:
					origin = { 1.0f, u, v };
					direction = { -1.0f, 0.0f, 0.0f };
					break;
				case 2:
					origin = { u, 0.0f, v };
					direction = { 0.0f, 1.0f, 0.0f };
					break;
				case 3:
					origin = { u, 1.0f, v };
					direction = { 0.0f, -1.0f, 0.0f };
					break;
				case 4:
					origin = { u, v, 0.0f };
					direction = { 0.0f, 0.0f, 1.0f };
					break;
				case 5:
				default:
					origin = { u, v, 1.0f };
					direction = { 0.0f, 0.0f, -1.0f };
					break;
				}

				rays.push_back({ origin, direction });
			}
		}
	}

	// Map ray from unit-cube [0,1]^3 to world-space cell and return world dir + tMax.
	void MapRayToWorldCell(const Ray& ray, const Float3& cellMin, const Float3& cellExtent,
		Float3& outOrigin, Float3& outDir, float& outTMax)
	{
		outOrigin = {
			cellMin.x + ray.origin.x * cellExtent.x,
			cellMin.y + ray.origin.y * cellExtent.y,
			cellMin.z + ray.origin.z * cellExtent.z
		};
		outDir = {
			ray.direction.x * cellExtent.x,
			ray.direction.y * cellExtent.y,
			ray.direction.z * cellExtent.z
		};
		outTMax = outDir.length();
		if (outTMax > 1e-12f)
			outDir = outDir * (1.0f / outTMax);
	}

	constexpr size_t kMaxDenseVoxelRasterCells = 512ull * 512ull * 512ull;
	constexpr uint32_t kVoxelRasterBrickDim = 16u;
	constexpr uint32_t kVoxelRasterBrickCellCount = kVoxelRasterBrickDim * kVoxelRasterBrickDim * kVoxelRasterBrickDim;

	struct VoxelCoverageWorkResult
	{
		std::pmr::vector<VoxelCell> emittedCells{ GetVoxelizationScratchResource() };
		std::pmr::vector<VoxelizeTrianglesResult::RefinedGroupStats> refinedGroupStats{ GetVoxelizationScratchResource() };
		uint32_t positiveCoverageCellCount = 0;
		float totalCoverage = 0.0f;
		float maxCoverage = 0.0f;
		uint64_t sourceCoverageQueryCount = 0;
		uint64_t sourceCoverageTriangleCandidateCount = 0;
		uint64_t sourceCoverageTriangleTestCount = 0;
		uint64_t sourceCoverageOutOfCellRejectionCount = 0;

		void Reset()
		{
			emittedCells.clear();
			refinedGroupStats.clear();
			positiveCoverageCellCount = 0;
			totalCoverage = 0.0f;
			maxCoverage = 0.0f;
			sourceCoverageQueryCount = 0;
			sourceCoverageTriangleCandidateCount = 0;
			sourceCoverageTriangleTestCount = 0;
			sourceCoverageOutOfCellRejectionCount = 0;
		}
	};

	struct BrickRasterRecord
	{
		uint32_t brickIndex = 0u;
		uint32_t xMin = 0u;
		uint32_t yMin = 0u;
		uint32_t zMin = 0u;
		uint32_t xMax = 0u;
		uint32_t yMax = 0u;
		uint32_t zMax = 0u;
		int32_t refinedGroup = -1;
	};

	struct BrickSpan
	{
		size_t firstRecord = 0u;
		size_t recordCount = 0u;
	};

	struct VoxelRasterBrickOutput;

	struct VoxelRasterCellData
	{
		uint64_t cellKey = 0u;
		VoxelRasterBrickOutput* owner = nullptr;
		uint32_t candidateGroupFirst = 0u;
		uint32_t candidateGroupCount = 0u;

		void Reset(uint64_t key, VoxelRasterBrickOutput* cellOwner)
		{
			cellKey = key;
			owner = cellOwner;
			candidateGroupFirst = 0u;
			candidateGroupCount = 0u;
		}
	};

	struct VoxelRasterBrickOutput
	{
		std::pmr::memory_resource* resource = GetVoxelizationScratchResource();
		std::pmr::vector<VoxelRasterCellData> cells{ resource };
		PmrInt32Vector candidateRefinedGroups{ resource };
		uint32_t activeCellCount = 0u;

		void Begin()
		{
			activeCellCount = 0u;
			candidateRefinedGroups.clear();
		}

		VoxelRasterCellData& AddCell(uint64_t cellKey)
		{
			if (activeCellCount == cells.size())
			{
				cells.emplace_back();
			}
			VoxelRasterCellData& cell = cells[activeCellCount++];
			cell.Reset(cellKey, this);
			return cell;
		}
	};

	struct VoxelRasterActiveCell
	{
		uint64_t cellKey = 0u;
		VoxelRasterCellData* cell = nullptr;
	};

	struct VoxelizeScratchState
	{
		std::pmr::memory_resource* resource = GetVoxelizationScratchResource();
		uint32_t generation = 0u;
		ScratchCellTriMap cellTriMap{ resource };
		PmrUInt64Vector cellTriKeys{ resource };
		PmrUInt64Vector candidateKeys{ resource };
		std::pmr::vector<PmrUInt64Vector> perTriangleCells{ resource };
		std::vector<VoxelSourcePayloadInstance> candidateVoxelPayloads;
		std::pmr::vector<VoxelCoverageWorkResult> coverageWorkResults{ resource };
		std::pmr::vector<BrickRasterRecord> voxelRasterRecords{ resource };
		std::pmr::vector<BrickRasterRecord> voxelRasterBinnedRecords{ resource };
		PmrUInt32Vector voxelRasterBrickCounts{ resource };
		PmrUInt32Vector voxelRasterBrickWriteOffsets{ resource };
		std::pmr::vector<BrickSpan> voxelRasterBrickSpans{ resource };
		PmrUInt32Vector voxelRasterActiveBrickIndices{ resource };
		std::vector<std::unique_ptr<VoxelRasterBrickOutput>> voxelRasterBrickOutputs;
		std::pmr::vector<VoxelRasterActiveCell> voxelRasterActiveCells{ resource };
		PmrUInt32Vector voxelRasterCellGenerations{ resource };
		std::pmr::vector<VoxelRasterCellData*> voxelRasterDenseCells{ resource };
		uint32_t voxelRasterCellGeneration = 0u;
		std::pmr::unordered_map<uint64_t, VoxelRasterCellData*> voxelRasterCellMap{ resource };
		bool voxelRasterUsesDenseLookup = false;
		uint32_t voxelRasterCandidateCellCount = 0u;

		void Begin()
		{
			if (++generation == 0u)
			{
				cellTriMap.clear();
				generation = 1u;
			}

			cellTriKeys.clear();
			candidateKeys.clear();
			candidateVoxelPayloads.clear();
			voxelRasterRecords.clear();
			voxelRasterBinnedRecords.clear();
			voxelRasterBrickCounts.clear();
			voxelRasterBrickWriteOffsets.clear();
			voxelRasterBrickSpans.clear();
			voxelRasterActiveBrickIndices.clear();
			voxelRasterActiveCells.clear();
			voxelRasterCellMap.clear();
			voxelRasterUsesDenseLookup = false;
			voxelRasterCandidateCellCount = 0u;
			if (++voxelRasterCellGeneration == 0u)
			{
				std::fill(voxelRasterCellGenerations.begin(), voxelRasterCellGenerations.end(), 0u);
				voxelRasterCellGeneration = 1u;
			}
		}
	};

	ScratchTriCell& TouchTriCell(ScratchCellTriMap& cells, PmrUInt64Vector& activeKeys, uint32_t generation, uint64_t key)
	{
		ScratchTriCell& cell = cells[key];
		if (cell.generation != generation)
		{
			cell.generation = generation;
			cell.triangleIndices.clear();
			activeKeys.push_back(key);
		}
		return cell;
	}

	size_t DenseCellIndexFromKey(uint64_t cellKey, uint32_t resolution)
	{
		const size_t x = static_cast<size_t>(cellKey & 0xFFFFu);
		const size_t y = static_cast<size_t>((cellKey >> 16u) & 0xFFFFu);
		const size_t z = static_cast<size_t>((cellKey >> 32u) & 0xFFFFu);
		const size_t res = static_cast<size_t>(resolution);
		return x + y * res + z * res * res;
	}

	bool TryGetDenseVoxelRasterCellCount(uint32_t resolution, size_t& cellCount)
	{
		const size_t res = static_cast<size_t>(resolution);
		if (res == 0u || res > kMaxDenseVoxelRasterCells / res || res * res > kMaxDenseVoxelRasterCells / res)
		{
			cellCount = 0u;
			return false;
		}

		cellCount = res * res * res;
		if (cellCount == 0u || cellCount > kMaxDenseVoxelRasterCells)
		{
			cellCount = 0u;
			return false;
		}
		return true;
	}

	template <class Vector>
	void AddUniqueRefinedGroup(Vector& refinedGroups, int32_t refinedGroup)
	{
		if (std::find(refinedGroups.begin(), refinedGroups.end(), refinedGroup) == refinedGroups.end())
		{
			refinedGroups.push_back(refinedGroup);
		}
	}

	bool HasVoxelSources(const VoxelizeTrianglesInput& input)
	{
		return (input.sourceVoxelPayloads != nullptr && !input.sourceVoxelPayloads->empty()) ||
			(input.sourceVoxelPayloadInstances != nullptr && !input.sourceVoxelPayloadInstances->empty());
	}

	bool HasCandidateVoxelSources(const VoxelizeTrianglesInput& input)
	{
		return (input.candidateVoxelPayloads != nullptr && !input.candidateVoxelPayloads->empty()) ||
			(input.candidateVoxelPayloadInstances != nullptr && !input.candidateVoxelPayloadInstances->empty());
	}

	Float3 TransformPoint3x4(const ClusterLODAssemblyTransform& transform, const Float3& point)
	{
		return {
			transform.row0.x * point.x + transform.row0.y * point.y + transform.row0.z * point.z + transform.row0.w,
			transform.row1.x * point.x + transform.row1.y * point.y + transform.row1.z * point.z + transform.row1.w,
			transform.row2.x * point.x + transform.row2.y * point.y + transform.row2.z * point.z + transform.row2.w
		};
	}

	struct VoxelPayloadRasterContext
	{
		Float3 centerBase{};
		Float3 xStep{};
		Float3 yStep{};
		Float3 zStep{};
		Float3 extent{};
	};

	VoxelPayloadRasterContext BuildVoxelPayloadRasterContext(
		const VoxelGroupPayload& payload,
		const ClusterLODAssemblyTransform& transform,
		float expansionRadius)
	{
		const float halfSourceVoxelWidth = payload.voxelWidth * 0.5f;
		const Float3 firstCenter(
			payload.aabbMin.x + halfSourceVoxelWidth,
			payload.aabbMin.y + halfSourceVoxelWidth,
			payload.aabbMin.z + halfSourceVoxelWidth);

		VoxelPayloadRasterContext context{};
		context.centerBase = TransformPoint3x4(transform, firstCenter);
		context.xStep = Float3(
			transform.row0.x * payload.voxelWidth,
			transform.row1.x * payload.voxelWidth,
			transform.row2.x * payload.voxelWidth);
		context.yStep = Float3(
			transform.row0.y * payload.voxelWidth,
			transform.row1.y * payload.voxelWidth,
			transform.row2.y * payload.voxelWidth);
		context.zStep = Float3(
			transform.row0.z * payload.voxelWidth,
			transform.row1.z * payload.voxelWidth,
			transform.row2.z * payload.voxelWidth);
		context.extent = Float3(
			(std::abs(transform.row0.x) + std::abs(transform.row0.y) + std::abs(transform.row0.z)) * halfSourceVoxelWidth + expansionRadius,
			(std::abs(transform.row1.x) + std::abs(transform.row1.y) + std::abs(transform.row1.z)) * halfSourceVoxelWidth + expansionRadius,
			(std::abs(transform.row2.x) + std::abs(transform.row2.y) + std::abs(transform.row2.z)) * halfSourceVoxelWidth + expansionRadius);
		return context;
	}

	void TransformVoxelCellAabbFast(
		const VoxelPayloadRasterContext& context,
		const VoxelCell& cell,
		Float3& outMin,
		Float3& outMax)
	{
		const Float3 center = context.centerBase +
			context.xStep * static_cast<float>(cell.x) +
			context.yStep * static_cast<float>(cell.y) +
			context.zStep * static_cast<float>(cell.z);
		outMin = center - context.extent;
		outMax = center + context.extent;
	}

	uint32_t VoxelRasterBrickResolution(uint32_t resolution)
	{
		return (resolution + kVoxelRasterBrickDim - 1u) / kVoxelRasterBrickDim;
	}

	uint32_t VoxelRasterBrickIndex(uint32_t bx, uint32_t by, uint32_t bz, uint32_t brickResolution)
	{
		return bx + by * brickResolution + bz * brickResolution * brickResolution;
	}

	uint32_t VoxelRasterLocalCellIndex(uint32_t x, uint32_t y, uint32_t z)
	{
		return (x & (kVoxelRasterBrickDim - 1u)) |
			((y & (kVoxelRasterBrickDim - 1u)) << 4u) |
			((z & (kVoxelRasterBrickDim - 1u)) << 8u);
	}

	void EmitVoxelRasterRecordsForPayloads(
		const std::vector<VoxelSourcePayloadInstance>& payloadInstances,
		const Float3& aabbMin,
		float voxelWidth,
		uint32_t resolution,
		VoxelizeScratchState& scratch)
	{
		if (payloadInstances.empty() || voxelWidth <= 0.0f || resolution == 0u)
		{
			return;
		}

		const float invCellSize = 1.0f / voxelWidth;
		const uint32_t brickResolution = VoxelRasterBrickResolution(resolution);

		for (uint32_t payloadIndex = 0u; payloadIndex < static_cast<uint32_t>(payloadInstances.size()); ++payloadIndex)
		{
			const VoxelSourcePayloadInstance& payloadInstance = payloadInstances[payloadIndex];
			const VoxelGroupPayload* payload = payloadInstance.payload;
			if (payload == nullptr || payload->voxelWidth <= 0.0f)
			{
				continue;
			}

			const float expansionRadius = std::max(0.0f, payloadInstance.expansionRadius);
			const VoxelPayloadRasterContext rasterContext = BuildVoxelPayloadRasterContext(*payload, payloadInstance.localToTarget, expansionRadius);
			const bool hasRefinedGroupOverride = payloadInstance.refinedGroupOverride != std::numeric_limits<int32_t>::min();
			const int32_t refinedGroupOverride = payloadInstance.refinedGroupOverride;
			const uint32_t cellCount = static_cast<uint32_t>(std::min<size_t>(payload->activeCells.size(), std::numeric_limits<uint32_t>::max()));

			for (uint32_t cellIndex = 0u; cellIndex < cellCount; ++cellIndex)
			{
				const VoxelCell& sourceCell = payload->activeCells[cellIndex];
				Float3 sourceMin{};
				Float3 sourceMax{};
				TransformVoxelCellAabbFast(rasterContext, sourceCell, sourceMin, sourceMax);
				const uint32_t cxMin = ToCellCoord(sourceMin.x, aabbMin.x, invCellSize, resolution);
				const uint32_t cyMin = ToCellCoord(sourceMin.y, aabbMin.y, invCellSize, resolution);
				const uint32_t czMin = ToCellCoord(sourceMin.z, aabbMin.z, invCellSize, resolution);
				const uint32_t cxMax = ToCellCoordExclusiveMax(sourceMax.x, aabbMin.x, invCellSize, resolution);
				const uint32_t cyMax = ToCellCoordExclusiveMax(sourceMax.y, aabbMin.y, invCellSize, resolution);
				const uint32_t czMax = ToCellCoordExclusiveMax(sourceMax.z, aabbMin.z, invCellSize, resolution);
				const uint32_t bxMin = cxMin / kVoxelRasterBrickDim;
				const uint32_t byMin = cyMin / kVoxelRasterBrickDim;
				const uint32_t bzMin = czMin / kVoxelRasterBrickDim;
				const uint32_t bxMax = cxMax / kVoxelRasterBrickDim;
				const uint32_t byMax = cyMax / kVoxelRasterBrickDim;
				const uint32_t bzMax = czMax / kVoxelRasterBrickDim;
				const int32_t refinedGroup = hasRefinedGroupOverride ? refinedGroupOverride : sourceCell.refinedGroup;

				for (uint32_t bz = bzMin; bz <= bzMax; ++bz)
				{
					const uint32_t brickZMin = bz * kVoxelRasterBrickDim;
					const uint32_t brickZMax = std::min(brickZMin + kVoxelRasterBrickDim - 1u, resolution - 1u);
					for (uint32_t by = byMin; by <= byMax; ++by)
					{
						const uint32_t brickYMin = by * kVoxelRasterBrickDim;
						const uint32_t brickYMax = std::min(brickYMin + kVoxelRasterBrickDim - 1u, resolution - 1u);
						for (uint32_t bx = bxMin; bx <= bxMax; ++bx)
						{
							const uint32_t brickXMin = bx * kVoxelRasterBrickDim;
							const uint32_t brickXMax = std::min(brickXMin + kVoxelRasterBrickDim - 1u, resolution - 1u);
							scratch.voxelRasterRecords.push_back(BrickRasterRecord{
								.brickIndex = VoxelRasterBrickIndex(bx, by, bz, brickResolution),
								.xMin = std::max(cxMin, brickXMin),
								.yMin = std::max(cyMin, brickYMin),
								.zMin = std::max(czMin, brickZMin),
								.xMax = std::min(cxMax, brickXMax),
								.yMax = std::min(cyMax, brickYMax),
								.zMax = std::min(czMax, brickZMax),
								.refinedGroup = refinedGroup });
						}
					}
				}
			}
		}
	}

	VoxelRasterCellData& TouchVoxelRasterLocalCell(
		VoxelRasterBrickOutput& output,
		std::array<uint32_t, kVoxelRasterBrickCellCount>& localCellIndices,
		uint64_t cellKey,
		uint32_t localCellIndex)
	{
		uint32_t& outputIndex = localCellIndices[localCellIndex];
		if (outputIndex == std::numeric_limits<uint32_t>::max())
		{
			outputIndex = output.activeCellCount;
			return output.AddCell(cellKey);
		}
		return output.cells[outputIndex];
	}

	void RasterizeVoxelBrickRecords(
		uint32_t resolution,
		VoxelizeScratchState& scratch)
	{
		const size_t brickCount = scratch.voxelRasterBrickSpans.size();
		if (brickCount == 0u)
		{
			return;
		}

		if (scratch.voxelRasterBrickOutputs.size() < brickCount)
		{
			scratch.voxelRasterBrickOutputs.resize(brickCount);
		}
		for (uint32_t brickIndex : scratch.voxelRasterActiveBrickIndices)
		{
			if (!scratch.voxelRasterBrickOutputs[brickIndex])
			{
				scratch.voxelRasterBrickOutputs[brickIndex] = std::make_unique<VoxelRasterBrickOutput>();
			}
			scratch.voxelRasterBrickOutputs[brickIndex]->Begin();
		}

		TaskSchedulerManager::GetInstance().ParallelFor("VoxelRaster::RasterizeBricks", scratch.voxelRasterActiveBrickIndices.size(),
			[&](size_t activeBrickWorkIndex)
		{
			const uint32_t brickIndex = scratch.voxelRasterActiveBrickIndices[activeBrickWorkIndex];
			const BrickSpan& span = scratch.voxelRasterBrickSpans[brickIndex];
			VoxelRasterBrickOutput& output = *scratch.voxelRasterBrickOutputs[brickIndex];
			struct CandidateGroupEmission
			{
				uint32_t localCellIndex = 0u;
				int32_t refinedGroup = -1;
			};

			std::array<uint32_t, kVoxelRasterBrickCellCount> localCellIndices{};
			std::array<uint32_t, kVoxelRasterBrickCellCount> candidateGroupCounts{};
			std::array<uint32_t, kVoxelRasterBrickCellCount> candidateGroupWriteOffsets{};
			localCellIndices.fill(std::numeric_limits<uint32_t>::max());
			std::pmr::vector<CandidateGroupEmission> candidateGroupEmissions{ scratch.resource };
			candidateGroupEmissions.reserve(std::min<size_t>(span.recordCount, 256u));

			{
				ZoneScopedN("VoxelRaster::RasterizeBrick::CandidateGroups");
				for (size_t recordOffset = 0u; recordOffset < span.recordCount; ++recordOffset)
				{
					const BrickRasterRecord& record = scratch.voxelRasterBinnedRecords[span.firstRecord + recordOffset];
					for (uint32_t z = record.zMin; z <= record.zMax; ++z)
					{
						const uint64_t zKey = static_cast<uint64_t>(z) << 32u;
						for (uint32_t y = record.yMin; y <= record.yMax; ++y)
						{
							const uint64_t yzKey = zKey | (static_cast<uint64_t>(y) << 16u);
							for (uint32_t x = record.xMin; x <= record.xMax; ++x)
							{
								const uint64_t cellKey = yzKey | static_cast<uint64_t>(x);
								const uint32_t localCellIndex = VoxelRasterLocalCellIndex(x, y, z);
								(void)TouchVoxelRasterLocalCell(
									output,
									localCellIndices,
									cellKey,
									localCellIndex);
								candidateGroupEmissions.push_back(CandidateGroupEmission{ localCellIndex, record.refinedGroup });
							}
						}
					}
				}
			}

			{
				ZoneScopedN("VoxelRaster::RasterizeBrick::AssignSpans");
				if (!candidateGroupEmissions.empty())
				{
					std::sort(candidateGroupEmissions.begin(), candidateGroupEmissions.end(), [](const CandidateGroupEmission& lhs, const CandidateGroupEmission& rhs) {
						if (lhs.localCellIndex != rhs.localCellIndex)
						{
							return lhs.localCellIndex < rhs.localCellIndex;
						}
						return lhs.refinedGroup < rhs.refinedGroup;
					});
					candidateGroupEmissions.erase(
						std::unique(candidateGroupEmissions.begin(), candidateGroupEmissions.end(), [](const CandidateGroupEmission& lhs, const CandidateGroupEmission& rhs) {
							return lhs.localCellIndex == rhs.localCellIndex && lhs.refinedGroup == rhs.refinedGroup;
						}),
						candidateGroupEmissions.end());
					for (const CandidateGroupEmission& emission : candidateGroupEmissions)
					{
						++candidateGroupCounts[emission.localCellIndex];
					}
				}

				uint32_t candidateGroupTotal = 0u;
				for (uint32_t localCellIndex = 0u; localCellIndex < kVoxelRasterBrickCellCount; ++localCellIndex)
				{
					const uint32_t outputCellIndex = localCellIndices[localCellIndex];
					if (outputCellIndex == std::numeric_limits<uint32_t>::max())
					{
						continue;
					}

					VoxelRasterCellData& cell = output.cells[outputCellIndex];
					cell.candidateGroupFirst = candidateGroupTotal;
					cell.candidateGroupCount = candidateGroupCounts[localCellIndex];
					candidateGroupTotal += candidateGroupCounts[localCellIndex];
				}

				output.candidateRefinedGroups.resize(candidateGroupTotal);
				for (const CandidateGroupEmission& emission : candidateGroupEmissions)
				{
					const uint32_t outputCellIndex = localCellIndices[emission.localCellIndex];
					const VoxelRasterCellData& cell = output.cells[outputCellIndex];
					output.candidateRefinedGroups[cell.candidateGroupFirst + candidateGroupWriteOffsets[emission.localCellIndex]++] = emission.refinedGroup;
				}
			}
		});
	}

	void RasterizeVoxelPayloadsToBricks(
		const std::vector<VoxelSourcePayloadInstance>& candidateVoxelPayloads,
		const Float3& aabbMin,
		float voxelWidth,
		uint32_t resolution,
		VoxelizeScratchState& scratch)
	{
		if (resolution == 0u || voxelWidth <= 0.0f || candidateVoxelPayloads.empty())
		{
			return;
		}

		ZoneScopedN("VoxelRaster::RasterizeVoxelPayloadsToBricks");
		const uint32_t brickResolution = VoxelRasterBrickResolution(resolution);
		const size_t brickCount = static_cast<size_t>(brickResolution) * brickResolution * brickResolution;
		if (brickCount == 0u)
		{
			return;
		}

		{
			ZoneScopedN("VoxelRaster::BuildBrickRecords");
			EmitVoxelRasterRecordsForPayloads(candidateVoxelPayloads, aabbMin, voxelWidth, resolution, scratch);
		}
		if (scratch.voxelRasterRecords.empty())
		{
			return;
		}

		{
			ZoneScopedN("VoxelRaster::BinBrickRecords");
			scratch.voxelRasterBrickCounts.assign(brickCount, 0u);
			for (const BrickRasterRecord& record : scratch.voxelRasterRecords)
			{
				++scratch.voxelRasterBrickCounts[record.brickIndex];
			}

			scratch.voxelRasterBrickSpans.resize(brickCount);
			scratch.voxelRasterBrickWriteOffsets.resize(brickCount);
			size_t runningOffset = 0u;
			for (size_t brickIndex = 0u; brickIndex < brickCount; ++brickIndex)
			{
				const uint32_t recordCount = scratch.voxelRasterBrickCounts[brickIndex];
				scratch.voxelRasterBrickSpans[brickIndex] = BrickSpan{ runningOffset, recordCount };
				scratch.voxelRasterBrickWriteOffsets[brickIndex] = static_cast<uint32_t>(runningOffset);
				if (recordCount != 0u)
				{
					scratch.voxelRasterActiveBrickIndices.push_back(static_cast<uint32_t>(brickIndex));
				}
				runningOffset += recordCount;
			}

			scratch.voxelRasterBinnedRecords.resize(scratch.voxelRasterRecords.size());
			for (const BrickRasterRecord& record : scratch.voxelRasterRecords)
			{
				scratch.voxelRasterBinnedRecords[scratch.voxelRasterBrickWriteOffsets[record.brickIndex]++] = record;
			}
		}

		{
			ZoneScopedN("VoxelRaster::RasterizeBricks");
			RasterizeVoxelBrickRecords(resolution, scratch);
		}

		{
			ZoneScopedN("VoxelRaster::BuildCandidateKeys");
			size_t denseCellCount = 0u;
			scratch.voxelRasterUsesDenseLookup = TryGetDenseVoxelRasterCellCount(resolution, denseCellCount);
			if (scratch.voxelRasterUsesDenseLookup)
			{
				if (scratch.voxelRasterCellGenerations.size() < denseCellCount)
				{
					scratch.voxelRasterCellGenerations.resize(denseCellCount, 0u);
					scratch.voxelRasterDenseCells.resize(denseCellCount, nullptr);
				}
			}
			else
			{
				scratch.voxelRasterCellMap.reserve(scratch.voxelRasterActiveBrickIndices.size() * kVoxelRasterBrickCellCount);
			}

			for (uint32_t brickIndex : scratch.voxelRasterActiveBrickIndices)
			{
				VoxelRasterBrickOutput* output = scratch.voxelRasterBrickOutputs[brickIndex].get();
				if (output == nullptr)
				{
					continue;
				}

				for (uint32_t cellIndex = 0u; cellIndex < output->activeCellCount; ++cellIndex)
				{
					VoxelRasterCellData& cell = output->cells[cellIndex];
					if (cell.candidateGroupCount != 0u)
					{
						++scratch.voxelRasterCandidateCellCount;
					}

					scratch.voxelRasterActiveCells.push_back(VoxelRasterActiveCell{ cell.cellKey, &cell });
					if (scratch.voxelRasterUsesDenseLookup)
					{
						const size_t denseIndex = DenseCellIndexFromKey(cell.cellKey, resolution);
						scratch.voxelRasterCellGenerations[denseIndex] = scratch.voxelRasterCellGeneration;
						scratch.voxelRasterDenseCells[denseIndex] = &cell;
					}
					else
					{
						scratch.voxelRasterCellMap[cell.cellKey] = &cell;
					}
				}
			}
		}
	}

	VoxelRasterCellData* FindVoxelRasterCell(VoxelizeScratchState& scratch, uint64_t cellKey, uint32_t resolution)
	{
		if (scratch.voxelRasterUsesDenseLookup)
		{
			const size_t denseIndex = DenseCellIndexFromKey(cellKey, resolution);
			if (denseIndex < scratch.voxelRasterCellGenerations.size() &&
				scratch.voxelRasterCellGenerations[denseIndex] == scratch.voxelRasterCellGeneration)
			{
				return scratch.voxelRasterDenseCells[denseIndex];
			}
			return nullptr;
		}

		const auto it = scratch.voxelRasterCellMap.find(cellKey);
		return it != scratch.voxelRasterCellMap.end() ? it->second : nullptr;
	}

	void RasterizeTrianglesToGrid(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& triangleIndices,
		const Float3& aabbMin,
		float voxelWidth,
		uint32_t resolution,
		VoxelizeScratchState& scratch)
	{
		const uint32_t triangleCount = static_cast<uint32_t>(triangleIndices.size() / 3);
		if (triangleCount == 0 || resolution == 0)
			return;

		const Float3 cellSize = {
			voxelWidth,
			voxelWidth,
			voxelWidth
		};

		if (cellSize.x <= 0.0f || cellSize.y <= 0.0f || cellSize.z <= 0.0f)
			return;

		ZoneScopedN("VoxelGroupBuilder::RasterizeTrianglesToGrid");
		TracyPlot("CLOD.Voxel.RasterizeTriangles.Triangles", static_cast<int64_t>(triangleCount));

		const Float3 invCellSize = {
			1.0f / cellSize.x,
			1.0f / cellSize.y,
			1.0f / cellSize.z
		};
		const Float3 halfCell = cellSize * 0.5f;

		std::pmr::vector<PmrUInt64Vector>& perTriangleCells = scratch.perTriangleCells;
		if (perTriangleCells.size() < triangleCount)
		{
			perTriangleCells.resize(triangleCount);
		}
		for (uint32_t triIdx = 0; triIdx < triangleCount; ++triIdx)
		{
			perTriangleCells[triIdx].clear();
		}

		TaskSchedulerManager::GetInstance().ParallelFor("VoxelGroupBuilder::RasterizeTriangles", triangleCount,
			[&](size_t triangleWorkIndex)
		{
			const uint32_t triIdx = static_cast<uint32_t>(triangleWorkIndex);
			const uint32_t i0 = triangleIndices[triIdx * 3 + 0];
			const uint32_t i1 = triangleIndices[triIdx * 3 + 1];
			const uint32_t i2 = triangleIndices[triIdx * 3 + 2];

			const Float3 v0 = ReadPosition(vertices, vertexStrideBytes, i0);
			const Float3 v1 = ReadPosition(vertices, vertexStrideBytes, i1);
			const Float3 v2 = ReadPosition(vertices, vertexStrideBytes, i2);

			Float3 triMin = {
				std::min({ v0.x, v1.x, v2.x }),
				std::min({ v0.y, v1.y, v2.y }),
				std::min({ v0.z, v1.z, v2.z })
			};
			Float3 triMax = {
				std::max({ v0.x, v1.x, v2.x }),
				std::max({ v0.y, v1.y, v2.y }),
				std::max({ v0.z, v1.z, v2.z })
			};

			const uint32_t cxMin = ToCellCoord(triMin.x - halfCell.x, aabbMin.x, invCellSize.x, resolution);
			const uint32_t cyMin = ToCellCoord(triMin.y - halfCell.y, aabbMin.y, invCellSize.y, resolution);
			const uint32_t czMin = ToCellCoord(triMin.z - halfCell.z, aabbMin.z, invCellSize.z, resolution);
			const uint32_t cxMax = ToCellCoord(triMax.x + halfCell.x, aabbMin.x, invCellSize.x, resolution);
			const uint32_t cyMax = ToCellCoord(triMax.y + halfCell.y, aabbMin.y, invCellSize.y, resolution);
			const uint32_t czMax = ToCellCoord(triMax.z + halfCell.z, aabbMin.z, invCellSize.z, resolution);

			PmrUInt64Vector& triangleCells = perTriangleCells[triIdx];
			const uint64_t candidateCellCount =
				static_cast<uint64_t>(cxMax - cxMin + 1u) *
				static_cast<uint64_t>(cyMax - cyMin + 1u) *
				static_cast<uint64_t>(czMax - czMin + 1u);
			triangleCells.reserve(static_cast<size_t>(std::min<uint64_t>(candidateCellCount, 64u)));

			for (uint32_t cz = czMin; cz <= czMax; ++cz)
			{
				for (uint32_t cy = cyMin; cy <= cyMax; ++cy)
				{
					for (uint32_t cx = cxMin; cx <= cxMax; ++cx)
					{
						Float3 center = {
							aabbMin.x + (static_cast<float>(cx) + 0.5f) * cellSize.x,
							aabbMin.y + (static_cast<float>(cy) + 0.5f) * cellSize.y,
							aabbMin.z + (static_cast<float>(cz) + 0.5f) * cellSize.z
						};

						if (TriangleAABBOverlap(v0, v1, v2, center, halfCell))
						{
							triangleCells.push_back(PackCell(cx, cy, cz));
						}
					}
				}
			}
		});

		size_t totalCellReferences = 0;
		for (uint32_t triIdx = 0; triIdx < triangleCount; ++triIdx)
		{
			totalCellReferences += perTriangleCells[triIdx].size();
		}
		TracyPlot("CLOD.Voxel.RasterizeTriangles.CellRefs", static_cast<int64_t>(totalCellReferences));

		scratch.cellTriMap.reserve(std::max<size_t>(scratch.cellTriMap.size(), std::max<size_t>(triangleCount * 2u, totalCellReferences)));
		for (uint32_t triIdx = 0; triIdx < triangleCount; ++triIdx)
		{
			for (uint64_t cellKey : perTriangleCells[triIdx])
			{
				TouchTriCell(scratch.cellTriMap, scratch.cellTriKeys, scratch.generation, cellKey).triangleIndices.push_back(triIdx);
			}
		}
	}


	bool RayAABBInterval(const Float3& origin, const Float3& dir, const Float3& boxMin, const Float3& boxMax, float tMax, float& outTEnter, float& outTExit)
	{
		float tMin = 0.0f;
		float tFar = tMax;
		const float originValues[3] = { origin.x, origin.y, origin.z };
		const float dirValues[3] = { dir.x, dir.y, dir.z };
		const float minValues[3] = { boxMin.x, boxMin.y, boxMin.z };
		const float maxValues[3] = { boxMax.x, boxMax.y, boxMax.z };

		for (uint32_t axis = 0; axis < 3u; ++axis)
		{
			if (std::abs(dirValues[axis]) < 1.0e-8f)
			{
				if (originValues[axis] < minValues[axis] || originValues[axis] > maxValues[axis])
				{
					return false;
				}
				continue;
			}

			const float invDir = 1.0f / dirValues[axis];
			float nearT = (minValues[axis] - originValues[axis]) * invDir;
			float farT = (maxValues[axis] - originValues[axis]) * invDir;
			if (nearT > farT)
			{
				std::swap(nearT, farT);
			}

			tMin = std::max(tMin, nearT);
			tFar = std::min(tFar, farT);
			if (tMin > tFar)
			{
				return false;
			}
		}

		outTEnter = tMin;
		outTExit = tFar;
		return tFar >= 0.0f && tMin < tMax;
	}

	struct CellCoverageSample
	{
		struct UvSample
		{
			DirectX::XMFLOAT2 uv{};
			float weight = 0.0f;
			uint32_t triangleIndex = std::numeric_limits<uint32_t>::max();
		};

		struct UvChart
		{
			uint64_t chartId = std::numeric_limits<uint64_t>::max();
			float weight = 0.0f;
			DirectX::XMFLOAT2 accumulatedUv{};
			std::vector<UvSample> samples;
		};

		float coverage = 0.0f;
		float coverageWeight = 0.0f;
		uint32_t hitCount = 0;
		uint32_t representativeTriangleIndex = std::numeric_limits<uint32_t>::max();
		Float3 accumulatedNormal{};
		SymmetricMatrix3 accumulatedSGGX{};
		float sggxWeight = 0.0f;
		DirectX::XMFLOAT2 representativeUv{};
		std::vector<UvChart> uvCharts;
		std::vector<Float3> normalSamples;
		std::vector<float> normalWeights;
	};

	DirectX::XMFLOAT2 ComputeTriangleUvDensity(
		const Float3& p0,
		const Float3& p1,
		const Float3& p2,
		const DirectX::XMFLOAT2& uv0,
		const DirectX::XMFLOAT2& uv1,
		const DirectX::XMFLOAT2& uv2)
	{
		const Float3 edge1 = p1 - p0;
		const Float3 edge2 = p2 - p0;
		const DirectX::XMFLOAT2 duv1{ uv1.x - uv0.x, uv1.y - uv0.y };
		const DirectX::XMFLOAT2 duv2{ uv2.x - uv0.x, uv2.y - uv0.y };
		const float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
		if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-10f)
		{
			return { -1.0f, -1.0f };
		}

		const float invDeterminant = 1.0f / determinant;
		const Float3 dPdu = (edge1 * duv2.y - edge2 * duv1.y) * invDeterminant;
		const Float3 dPdv = (edge2 * duv1.x - edge1 * duv2.x) * invDeterminant;
		const float dPduLength = std::sqrt(std::max(0.0f, dPdu.lengthSq()));
		const float dPdvLength = std::sqrt(std::max(0.0f, dPdv.lengthSq()));
		const float densityU = std::isfinite(dPduLength) && dPduLength > 1.0e-6f
			? 1.0f / dPduLength
			: -1.0f;
		const float densityV = std::isfinite(dPdvLength) && dPdvLength > 1.0e-6f
			? 1.0f / dPdvLength
			: -1.0f;
		return {
			std::isfinite(densityU) ? densityU : -1.0f,
			std::isfinite(densityV) ? densityV : -1.0f };
	}

	void AddCoverageUvSample(
		CellCoverageSample& sample,
		uint64_t chartId,
		uint32_t triangleIndex,
		const DirectX::XMFLOAT2& uv,
		float weight)
	{
		if (weight <= 0.0f || !std::isfinite(uv.x) || !std::isfinite(uv.y))
		{
			return;
		}

		auto chartIt = std::find_if(sample.uvCharts.begin(), sample.uvCharts.end(), [chartId](const CellCoverageSample::UvChart& chart) {
			return chart.chartId == chartId;
		});
		if (chartIt == sample.uvCharts.end())
		{
			chartIt = sample.uvCharts.insert(sample.uvCharts.end(), CellCoverageSample::UvChart{ .chartId = chartId });
		}
		chartIt->weight += weight;
		chartIt->accumulatedUv.x += uv.x * weight;
		chartIt->accumulatedUv.y += uv.y * weight;
		chartIt->samples.push_back(CellCoverageSample::UvSample{ uv, weight, triangleIndex });
	}

	void SelectRepresentativeUv(CellCoverageSample& sample)
	{
		if (sample.uvCharts.empty())
		{
			return;
		}

		const CellCoverageSample::UvChart* selectedChart = &sample.uvCharts.front();
		for (const CellCoverageSample::UvChart& chart : sample.uvCharts)
		{
			if (chart.weight > selectedChart->weight ||
				(chart.weight == selectedChart->weight && chart.chartId < selectedChart->chartId))
			{
				selectedChart = &chart;
			}
		}
		if (selectedChart->weight <= 0.0f || selectedChart->samples.empty())
		{
			return;
		}

		const DirectX::XMFLOAT2 mean(
			selectedChart->accumulatedUv.x / selectedChart->weight,
			selectedChart->accumulatedUv.y / selectedChart->weight);
		const CellCoverageSample::UvSample* representative = &selectedChart->samples.front();
		float bestDistanceSq = std::numeric_limits<float>::max();
		for (const CellCoverageSample::UvSample& candidate : selectedChart->samples)
		{
			const float dx = candidate.uv.x - mean.x;
			const float dy = candidate.uv.y - mean.y;
			const float distanceSq = dx * dx + dy * dy;
			if (distanceSq < bestDistanceSq ||
				(distanceSq == bestDistanceSq && candidate.weight > representative->weight) ||
				(distanceSq == bestDistanceSq && candidate.weight == representative->weight && candidate.triangleIndex < representative->triangleIndex))
			{
				bestDistanceSq = distanceSq;
				representative = &candidate;
			}
		}
		sample.representativeUv = representative->uv;
		sample.representativeTriangleIndex = representative->triangleIndex;

	}

	bool ApplyCoverageMaterialSample(
		const VoxelCoverageMaterialSampler* sampler,
		VoxelCoverageHit& hit,
		Float3& normal,
		float& weight)
	{
		weight = 1.0f;
		if (sampler == nullptr)
		{
			return true;
		}

		const VoxelCoverageMaterialSample sample = (*sampler)(hit);
		if (!sample.accepted || sample.weight <= 0.0f || !std::isfinite(sample.weight))
		{
			return false;
		}

		weight = sample.weight;
		if (sample.overrideNormal &&
			std::isfinite(sample.normal.x) &&
			std::isfinite(sample.normal.y) &&
			std::isfinite(sample.normal.z))
		{
			const Float3 sampledNormal(sample.normal.x, sample.normal.y, sample.normal.z);
			if (sampledNormal.lengthSq() > 1.0e-20f)
			{
				normal = sampledNormal.normalized();
				hit.normal = sample.normal;
			}
		}
		return true;
	}

	CellCoverageSample SampleCellCoverageSourceTriangles(
		const VoxelSourceTriangleBVH& sourceTriangles,
		int32_t refinedGroupFilter,
		const Float3& cellWorldMin,
		const Float3& cellWorldMax,
		const std::vector<Ray>& rays,
		const VoxelCoverageMaterialSampler* materialSampler,
		uint64_t& sourceCoverageQueryCount,
		uint64_t& sourceCoverageTriangleCandidateCount,
		uint64_t& sourceCoverageTriangleTestCount,
		uint64_t& sourceCoverageOutOfCellRejectionCount)
	{
		CellCoverageSample sample{};
		if (!sourceTriangles.IsValid() || rays.empty())
		{
			return sample;
		}

		const bool doubleSidedTriangles = sourceTriangles.DoubleSidedTriangles();

		const Float3 cellExtent = cellWorldMax - cellWorldMin;
		constexpr float kCellHitEpsilon = 1.0e-5f;
		for (const Ray& ray : rays)
		{
			Float3 origin, dir;
			float tMax;
			MapRayToWorldCell(ray, cellWorldMin, cellExtent, origin, dir, tMax);
			if (tMax < 1e-12f)
			{
				continue;
			}

			float tEnter = 0.0f;
			float tExit = tMax;
			if (!RayAABBInterval(origin, dir, cellWorldMin, cellWorldMax, tMax, tEnter, tExit))
			{
				continue;
			}

			uint32_t nearestTriangleIndex = std::numeric_limits<uint32_t>::max();
			uint64_t nearestChartId = std::numeric_limits<uint64_t>::max();
			float traceTMin = tEnter;
			Float3 nearestNormal{};
			DirectX::XMFLOAT2 nearestUv{};
			float nearestWeight = 1.0f;
			while (traceTMin < tExit + kCellHitEpsilon)
			{
				float hitT = 0.0f;
				float hitU = 0.0f;
				float hitV = 0.0f;
				uint32_t triLocalIdx = std::numeric_limits<uint32_t>::max();
				++sourceCoverageQueryCount;
				if (!sourceTriangles.IntersectNearest(refinedGroupFilter, ToXM(origin), ToXM(dir), traceTMin, tExit + kCellHitEpsilon, triLocalIdx, hitT, hitU, hitV))
				{
					break;
				}
				++sourceCoverageTriangleCandidateCount;
				++sourceCoverageTriangleTestCount;

				const float nextTraceTMin = std::max(hitT + kCellHitEpsilon, std::nextafter(traceTMin, std::numeric_limits<float>::infinity()));
				VoxelSourceTriangleSample triangleSample{};
				if (!sourceTriangles.GetTriangleSample(triLocalIdx, triangleSample))
				{
					traceTMin = nextTraceTMin;
					continue;
				}

				const Float3 hitPoint = origin + dir * hitT;
				if (hitT + kCellHitEpsilon < tEnter || hitT > tExit + kCellHitEpsilon || !PointInsideAABB(hitPoint, cellWorldMin, cellWorldMax, kCellHitEpsilon))
				{
					++sourceCoverageOutOfCellRejectionCount;
					traceTMin = nextTraceTMin;
					continue;
				}

				const Float3 v0 = ToFloat3(triangleSample.positions[0]);
				const Float3 v1 = ToFloat3(triangleSample.positions[1]);
				const Float3 v2 = ToFloat3(triangleSample.positions[2]);
				const Float3 n0 = ToFloat3(triangleSample.normals[0]);
				const Float3 n1 = ToFloat3(triangleSample.normals[1]);
				const Float3 n2 = ToFloat3(triangleSample.normals[2]);
				const Float3 faceNormal = TriangleNormal(v0, v1, v2);
				const float hitW = 1.0f - hitU - hitV;
				const Float3 interpolatedNormal = n0 * hitW + n1 * hitU + n2 * hitV;
				Float3 candidateNormal = interpolatedNormal.lengthSq() > 1.0e-20f
					? interpolatedNormal.normalized()
					: faceNormal;
				candidateNormal = OrientHitNormalForSidedness(candidateNormal, faceNormal, dir, doubleSidedTriangles);
				const DirectX::XMFLOAT2 uv0 = triangleSample.uvs[0];
				const DirectX::XMFLOAT2 uv1 = triangleSample.uvs[1];
				const DirectX::XMFLOAT2 uv2 = triangleSample.uvs[2];
				DirectX::XMFLOAT2 candidateUv = InterpolateUv(
					uv0,
					uv1,
					uv2,
					hitW,
					hitU,
					hitV);
				float candidateWeight = 1.0f;
				VoxelCoverageHit materialHit{
					.triangleIndex = triLocalIdx,
					.position = ToXM(hitPoint),
					.normal = ToXM(candidateNormal),
					.uv = candidateUv,
					.trianglePositions = { ToXM(v0), ToXM(v1), ToXM(v2) },
					.triangleUvs = { uv0, uv1, uv2 },
					.barycentrics = { hitW, hitU, hitV },
				};
				if (!ApplyCoverageMaterialSample(materialSampler, materialHit, candidateNormal, candidateWeight))
				{
					traceTMin = nextTraceTMin;
					continue;
				}

				nearestTriangleIndex = triLocalIdx;
				nearestChartId = triangleSample.uvChartId;
				nearestNormal = candidateNormal;
				nearestUv = candidateUv;
				nearestWeight = candidateWeight;
				break;
			}

			if (nearestTriangleIndex != std::numeric_limits<uint32_t>::max())
			{
				++sample.hitCount;
				sample.coverageWeight += nearestWeight;
				sample.accumulatedNormal = sample.accumulatedNormal + nearestNormal * nearestWeight;
				sample.normalSamples.push_back(nearestNormal);
				sample.normalWeights.push_back(nearestWeight);
				AddCoverageUvSample(sample, nearestChartId, nearestTriangleIndex, nearestUv, nearestWeight);
			}
		}

		sample.coverage = sample.coverageWeight / static_cast<float>(rays.size());
		if (!sample.normalSamples.empty())
		{
			sample.accumulatedSGGX = BuildSGGXFromWeightedNormals(sample.normalSamples, sample.normalWeights);
			sample.sggxWeight = 1.0f;
		}
		SelectRepresentativeUv(sample);
		return sample;
	}

	uint32_t PruneCellsByPureCoverage(std::vector<VoxelCell>& cells)
	{
		if (cells.empty())
		{
			return 0u;
		}

		const size_t originalCount = cells.size();
		std::sort(cells.begin(), cells.end(), [](const VoxelCell& lhs, const VoxelCell& rhs) {
			if (lhs.refinedGroup != rhs.refinedGroup)
			{
				return lhs.refinedGroup < rhs.refinedGroup;
			}
			const uint64_t lhsCell = PackCell(lhs.x, lhs.y, lhs.z);
			const uint64_t rhsCell = PackCell(rhs.x, rhs.y, rhs.z);
			return lhsCell < rhsCell;
		});

		std::vector<VoxelCell> keptCells;
		keptCells.reserve(cells.size());

		auto mortonKey = [](const VoxelCell& cell)
		{
			auto expandBits = [](uint32_t value)
			{
				value &= 1023u;
				value = (value | (value << 16u)) & 0x030000FFu;
				value = (value | (value << 8u)) & 0x0300F00Fu;
				value = (value | (value << 4u)) & 0x030C30C3u;
				value = (value | (value << 2u)) & 0x09249249u;
				return value;
			};
			return (expandBits(cell.x) << 2u) | (expandBits(cell.y) << 1u) | expandBits(cell.z);
		};

		auto appendPrunedSection = [&keptCells, &mortonKey](std::vector<VoxelCell>::iterator begin, std::vector<VoxelCell>::iterator end)
		{
			float totalCoverage = 0.0f;
			for (auto it = begin; it != end; ++it)
			{
				totalCoverage += std::clamp(it->opacity, 0.0f, 1.0f);
			}

			const size_t sectionCellCount = static_cast<size_t>(std::distance(begin, end));
			const uint32_t targetCellCount = std::min<uint32_t>(
				static_cast<uint32_t>(std::ceil(std::max(0.0f, totalCoverage))),
				static_cast<uint32_t>(std::min<size_t>(sectionCellCount, std::numeric_limits<uint32_t>::max())));
			if (targetCellCount == 0u)
			{
				return;
			}

			std::vector<VoxelCell> sectionCells(begin, end);
			std::sort(sectionCells.begin(), sectionCells.end(), [&](const VoxelCell& lhs, const VoxelCell& rhs) {
				const uint32_t lhsMorton = mortonKey(lhs);
				const uint32_t rhsMorton = mortonKey(rhs);
				if (lhsMorton != rhsMorton)
				{
					return lhsMorton < rhsMorton;
				}
				const uint64_t lhsCell = PackCell(lhs.x, lhs.y, lhs.z);
				const uint64_t rhsCell = PackCell(rhs.x, rhs.y, rhs.z);
				return lhsCell < rhsCell;
			});

			if (targetCellCount >= sectionCells.size())
			{
				keptCells.insert(keptCells.end(), sectionCells.begin(), sectionCells.end());
				return;
			}

			std::vector<uint8_t> selected(sectionCells.size(), 0u);
			const float coverageStep = totalCoverage / static_cast<float>(targetCellCount);
			float nextCoverageThreshold = coverageStep * 0.5f;
			float runningCoverage = 0.0f;
			uint32_t keptCount = 0u;
			for (size_t cellIndex = 0u; cellIndex < sectionCells.size() && keptCount < targetCellCount; ++cellIndex)
			{
				runningCoverage += std::clamp(sectionCells[cellIndex].opacity, 0.0f, 1.0f);
				if (runningCoverage >= nextCoverageThreshold)
				{
					selected[cellIndex] = 1u;
					++keptCount;
					nextCoverageThreshold += coverageStep;
				}
			}

			if (keptCount < targetCellCount)
			{
				std::vector<uint32_t> fallbackOrder(sectionCells.size());
				std::iota(fallbackOrder.begin(), fallbackOrder.end(), 0u);
				std::sort(fallbackOrder.begin(), fallbackOrder.end(), [&](uint32_t lhsIndex, uint32_t rhsIndex) {
					const VoxelCell& lhs = sectionCells[lhsIndex];
					const VoxelCell& rhs = sectionCells[rhsIndex];
					const float lhsOpacity = std::clamp(lhs.opacity, 0.0f, 1.0f);
					const float rhsOpacity = std::clamp(rhs.opacity, 0.0f, 1.0f);
					if (lhsOpacity != rhsOpacity)
					{
						return lhsOpacity > rhsOpacity;
					}
					const uint64_t lhsCell = PackCell(lhs.x, lhs.y, lhs.z);
					const uint64_t rhsCell = PackCell(rhs.x, rhs.y, rhs.z);
					return lhsCell < rhsCell;
				});
				for (uint32_t cellIndex : fallbackOrder)
				{
					if (selected[cellIndex] != 0u)
					{
						continue;
					}
					selected[cellIndex] = 1u;
					if (++keptCount >= targetCellCount)
					{
						break;
					}
				}
			}

			for (size_t cellIndex = 0u; cellIndex < sectionCells.size(); ++cellIndex)
			{
				if (selected[cellIndex] != 0u)
				{
					keptCells.push_back(sectionCells[cellIndex]);
				}
			}
		};

		auto sectionBegin = cells.begin();
		while (sectionBegin != cells.end())
		{
			auto sectionEnd = sectionBegin + 1;
			while (sectionEnd != cells.end() && sectionEnd->refinedGroup == sectionBegin->refinedGroup)
			{
				++sectionEnd;
			}

			appendPrunedSection(sectionBegin, sectionEnd);
			sectionBegin = sectionEnd;
		}

		std::sort(keptCells.begin(), keptCells.end(), [](const VoxelCell& lhs, const VoxelCell& rhs) {
			const uint64_t lhsCell = PackCell(lhs.x, lhs.y, lhs.z);
			const uint64_t rhsCell = PackCell(rhs.x, rhs.y, rhs.z);
			return lhsCell == rhsCell ? lhs.refinedGroup < rhs.refinedGroup : lhsCell < rhsCell;
		});

		const uint32_t removedCount = static_cast<uint32_t>(std::min<size_t>(originalCount - keptCells.size(), std::numeric_limits<uint32_t>::max()));
		cells = std::move(keptCells);
		return removedCount;
	}

	// Morton code: 10-bit per axis -> 30-bit interleaved
	uint32_t ExpandBits10(uint32_t v)
	{
		// Spread 10 bits across 30 bits: --9--8--7--6--5--4--3--2--1--0
		v = (v | (v << 16)) & 0x030000FFu;
		v = (v | (v << 8)) & 0x0300F00Fu;
		v = (v | (v << 4)) & 0x030C30C3u;
		v = (v | (v << 2)) & 0x09249249u;
		return v;
	}

	uint32_t Morton3D(uint32_t x, uint32_t y, uint32_t z)
	{
		return (ExpandBits10(x) << 2) | (ExpandBits10(y) << 1) | ExpandBits10(z);
	}

	uint32_t PackCubeCoord(uint32_t cubeX, uint32_t cubeY, uint32_t cubeZ)
	{
		return (cubeX & 0x3FFu) | ((cubeY & 0x3FFu) << 10u) | ((cubeZ & 0x3FFu) << 20u);
	}
}

struct VoxelSourceTriangleBVH::EmbreeScene
{
	struct Triangle
	{
		uint32_t v0 = 0;
		uint32_t v1 = 0;
		uint32_t v2 = 0;
	};

	struct RefinedGroupScene
	{
		int32_t refinedGroup = -1;
		RTCScene scene = nullptr;
		std::vector<uint32_t> sourceTriangleIndices;
	};

	struct PartScene
	{
		RTCScene scene = nullptr;
		const std::vector<std::byte>* vertices = nullptr;
		size_t vertexStrideBytes = 0;
		const std::vector<std::byte>* skinningVertices = nullptr;
		size_t skinningVertexStrideBytes = 0;
		const std::vector<uint32_t>* triangleIndices = nullptr;
		uint32_t triangleCount = 0;
		std::vector<uint32_t> triangleUvChartIds;
		std::vector<DirectX::XMFLOAT2> triangleUvDensities;
		std::vector<float> triangleAreas;
		std::vector<uint32_t> uvDensitySampleTriangles;
		std::vector<float> uvDensitySampleWeights;
	};

	struct InstanceRef
	{
		uint32_t partIndex = 0;
		ClusterLODAssemblyTransform localToWorld{};
		int32_t refinedGroup = -1;
		uint32_t firstTriangle = 0;
		uint32_t triangleCount = 0;
		std::vector<uint32_t> boneRemapIndices;
	};

	static constexpr int32_t kUnfilteredRefinedGroupScene = std::numeric_limits<int32_t>::min();

	std::vector<DirectX::XMFLOAT3> vertices;
	std::vector<uint32_t> triangleUvChartIds;
	std::vector<RefinedGroupScene> refinedGroupScenes;
	std::vector<PartScene> partScenes;
	std::vector<InstanceRef> instances;
	std::vector<uint32_t> instanceIndexByGeometryId;
	bool instanced = false;

	~EmbreeScene()
	{
		for (RefinedGroupScene& refinedGroupScene : refinedGroupScenes)
		{
			if (refinedGroupScene.scene != nullptr)
			{
				rtcReleaseScene(refinedGroupScene.scene);
				refinedGroupScene.scene = nullptr;
			}
		}
		for (PartScene& partScene : partScenes)
		{
			if (partScene.scene != nullptr)
			{
				rtcReleaseScene(partScene.scene);
				partScene.scene = nullptr;
			}
		}
	}
};

namespace
{
	RTCDevice GetVoxelCoverageEmbreeDevice()
	{
		static RTCDevice device = rtcNewDevice(nullptr);
		return device;
	}

	DirectX::XMFLOAT3 TransformCoveragePoint(
		const ClusterLODAssemblyTransform& transform,
		const DirectX::XMFLOAT3& point)
	{
		return DirectX::XMFLOAT3(
			transform.row0.x * point.x + transform.row0.y * point.y + transform.row0.z * point.z + transform.row0.w,
			transform.row1.x * point.x + transform.row1.y * point.y + transform.row1.z * point.z + transform.row1.w,
			transform.row2.x * point.x + transform.row2.y * point.y + transform.row2.z * point.z + transform.row2.w);
	}

	DirectX::XMFLOAT3 TransformCoverageVector(
		const ClusterLODAssemblyTransform& transform,
		const DirectX::XMFLOAT3& vector)
	{
		const Float3 r0(transform.row0.x, transform.row0.y, transform.row0.z);
		const Float3 r1(transform.row1.x, transform.row1.y, transform.row1.z);
		const Float3 r2(transform.row2.x, transform.row2.y, transform.row2.z);
		const Float3 c0 = r1.cross(r2);
		const Float3 c1 = r2.cross(r0);
		const Float3 c2 = r0.cross(r1);
		const float det = r0.dot(c0);
		if (std::abs(det) <= 1.0e-8f)
		{
			return vector;
		}

		const float invDet = 1.0f / det;
		const Float3 invRow0(c0.x * invDet, c1.x * invDet, c2.x * invDet);
		const Float3 invRow1(c0.y * invDet, c1.y * invDet, c2.y * invDet);
		const Float3 invRow2(c0.z * invDet, c1.z * invDet, c2.z * invDet);
		const Float3 transformed(
			invRow0.x * vector.x + invRow1.x * vector.y + invRow2.x * vector.z,
			invRow0.y * vector.x + invRow1.y * vector.y + invRow2.y * vector.z,
			invRow0.z * vector.x + invRow1.z * vector.y + invRow2.z * vector.z);
		return ToXM(transformed.lengthSq() > 1.0e-20f ? transformed.normalized() : Float3(0.0f, 1.0f, 0.0f));
	}

	void StoreEmbreeTransform3x4(const ClusterLODAssemblyTransform& transform, float (&outTransform)[12])
	{
		outTransform[0] = transform.row0.x;
		outTransform[1] = transform.row0.y;
		outTransform[2] = transform.row0.z;
		outTransform[3] = transform.row0.w;
		outTransform[4] = transform.row1.x;
		outTransform[5] = transform.row1.y;
		outTransform[6] = transform.row1.z;
		outTransform[7] = transform.row1.w;
		outTransform[8] = transform.row2.x;
		outTransform[9] = transform.row2.y;
		outTransform[10] = transform.row2.z;
		outTransform[11] = transform.row2.w;
	}
}

VoxelSourceTriangleBVH::VoxelSourceTriangleBVH() = default;
VoxelSourceTriangleBVH::~VoxelSourceTriangleBVH() = default;

void VoxelSourceTriangleBVH::Build(
	const std::vector<std::byte>* vertices,
	size_t vertexStrideBytes,
	const std::vector<uint32_t>* triangleIndices,
	const std::vector<std::byte>* skinningVertices,
	size_t skinningVertexStrideBytes,
	const std::vector<int32_t>* triangleRefinedGroupIds,
	bool doubleSidedTriangles,
	bool buildRefinedGroupScenes)
{
	ZoneScopedN("VoxelSourceTriangleBVH::Build");
	m_vertices = vertices;
	m_vertexStrideBytes = vertexStrideBytes;
	m_skinningVertices = skinningVertices;
	m_skinningVertexStrideBytes = skinningVertexStrideBytes;
	m_triangleIndices = triangleIndices;
	m_triangleRefinedGroupIds = triangleRefinedGroupIds;
	m_doubleSidedTriangles = doubleSidedTriangles;
	m_uvDensity = { 0.0f, 0.0f };
	m_embreeScene.reset();

	if (vertices == nullptr || triangleIndices == nullptr || vertexStrideBytes < sizeof(float) * 3u || triangleIndices->size() < 3u || (triangleIndices->size() % 3u) != 0u)
	{
		return;
	}

	const uint32_t triangleCount = static_cast<uint32_t>(std::min<size_t>(triangleIndices->size() / 3u, std::numeric_limits<uint32_t>::max()));
	TracyPlot("CLOD.Voxel.BVH.Triangles", static_cast<int64_t>(triangleCount));

	RTCDevice device = GetVoxelCoverageEmbreeDevice();
	if (device == nullptr)
	{
		return;
	}

	std::unique_ptr<EmbreeScene> embreeScene = std::make_unique<EmbreeScene>();
	embreeScene->triangleUvChartIds = BuildTriangleUvChartIds(*vertices, vertexStrideBytes, *triangleIndices);
	const size_t sourceVertexCount = vertices->size() / vertexStrideBytes;
	embreeScene->vertices.resize(sourceVertexCount);
	for (size_t vertexIndex = 0; vertexIndex < sourceVertexCount; ++vertexIndex)
	{
		embreeScene->vertices[vertexIndex] = ToXM(ReadPosition(*vertices, vertexStrideBytes, static_cast<uint32_t>(vertexIndex)));
	}

	auto buildEmbreeSceneForTriangles = [&](int32_t refinedGroup, std::vector<uint32_t> sourceTriangleIndices) -> bool
	{
		if (sourceTriangleIndices.empty())
		{
			return false;
		}

		std::vector<EmbreeScene::Triangle> embreeTriangles;
		embreeTriangles.reserve(sourceTriangleIndices.size());
		for (uint32_t triangleIndex : sourceTriangleIndices)
		{
			const size_t triangleBase = static_cast<size_t>(triangleIndex) * 3u;
			embreeTriangles.push_back(EmbreeScene::Triangle{
				(*triangleIndices)[triangleBase + 0u],
				(*triangleIndices)[triangleBase + 1u],
				(*triangleIndices)[triangleBase + 2u],
			});
		}

		EmbreeScene::RefinedGroupScene refinedGroupScene{};
		refinedGroupScene.refinedGroup = refinedGroup;
		refinedGroupScene.sourceTriangleIndices = std::move(sourceTriangleIndices);
		refinedGroupScene.scene = rtcNewScene(device);
		RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
		DirectX::XMFLOAT3* embreeVertices = static_cast<DirectX::XMFLOAT3*>(rtcSetNewGeometryBuffer(
			geometry,
			RTC_BUFFER_TYPE_VERTEX,
			0,
			RTC_FORMAT_FLOAT3,
			sizeof(DirectX::XMFLOAT3),
			embreeScene->vertices.size()));
		if (embreeVertices != nullptr && !embreeScene->vertices.empty())
		{
			std::memcpy(embreeVertices, embreeScene->vertices.data(), embreeScene->vertices.size() * sizeof(DirectX::XMFLOAT3));
		}

		EmbreeScene::Triangle* embreeTriangleBuffer = static_cast<EmbreeScene::Triangle*>(rtcSetNewGeometryBuffer(
			geometry,
			RTC_BUFFER_TYPE_INDEX,
			0,
			RTC_FORMAT_UINT3,
			sizeof(EmbreeScene::Triangle),
			embreeTriangles.size()));
		if (embreeTriangleBuffer != nullptr && !embreeTriangles.empty())
		{
			std::memcpy(embreeTriangleBuffer, embreeTriangles.data(), embreeTriangles.size() * sizeof(EmbreeScene::Triangle));
		}

		rtcCommitGeometry(geometry);
		rtcAttachGeometry(refinedGroupScene.scene, geometry);
		rtcReleaseGeometry(geometry);
		rtcCommitScene(refinedGroupScene.scene);
		embreeScene->refinedGroupScenes.push_back(std::move(refinedGroupScene));
		return true;
	};

	std::vector<uint32_t> allTriangleIndices(triangleCount);
	std::iota(allTriangleIndices.begin(), allTriangleIndices.end(), 0u);
	{
		ZoneScopedN("VoxelSourceTriangleBVH::Build::EmbreeUnfilteredScene");
		buildEmbreeSceneForTriangles(EmbreeScene::kUnfilteredRefinedGroupScene, std::move(allTriangleIndices));
	}

	if (buildRefinedGroupScenes)
	{
		std::unordered_map<int32_t, std::vector<uint32_t>> trianglesByRefinedGroup;
		trianglesByRefinedGroup.reserve(triangleCount);
		{
			ZoneScopedN("VoxelSourceTriangleBVH::Build::GroupTriangles");
			for (uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
			{
				int32_t refinedGroup = -1;
				if (triangleRefinedGroupIds != nullptr && triangleIndex < triangleRefinedGroupIds->size())
				{
					refinedGroup = (*triangleRefinedGroupIds)[triangleIndex];
				}
				trianglesByRefinedGroup[refinedGroup].push_back(triangleIndex);
			}
		}
		TracyPlot("CLOD.Voxel.BVH.RefinedGroupScenes", static_cast<int64_t>(trianglesByRefinedGroup.size()));

		{
			ZoneScopedN("VoxelSourceTriangleBVH::Build::EmbreeRefinedGroupScenes");
			for (auto& [refinedGroup, sourceTriangleIndices] : trianglesByRefinedGroup)
			{
				buildEmbreeSceneForTriangles(refinedGroup, std::move(sourceTriangleIndices));
			}
		}
	}

	if (!embreeScene->refinedGroupScenes.empty())
	{
		spdlog::debug(
			"Voxel coverage Embree BVH built: vertices={} triangles={} refined_group_scenes_requested={} scenes={}",
			embreeScene->vertices.size(),
			triangleCount,
			buildRefinedGroupScenes,
			embreeScene->refinedGroupScenes.size());
		m_embreeScene = std::move(embreeScene);
		m_uvDensity = ComputeUvDensity();
	}
}

void VoxelSourceTriangleBVH::BuildInstanced(
	std::span<const VoxelSourceTrianglePart> parts,
	std::span<const VoxelSourceTriangleInstance> instances,
	bool doubleSidedTriangles)
{
	ZoneScopedN("VoxelSourceTriangleBVH::BuildInstanced");
	m_vertices = nullptr;
	m_vertexStrideBytes = 0;
	m_skinningVertices = nullptr;
	m_skinningVertexStrideBytes = 0;
	m_triangleIndices = nullptr;
	m_triangleRefinedGroupIds = nullptr;
	m_doubleSidedTriangles = doubleSidedTriangles;
	m_uvDensity = { 0.0f, 0.0f };
	m_embreeScene.reset();

	if (parts.empty() || instances.empty())
	{
		return;
	}

	RTCDevice device = GetVoxelCoverageEmbreeDevice();
	if (device == nullptr)
	{
		return;
	}

	std::unique_ptr<EmbreeScene> embreeScene = std::make_unique<EmbreeScene>();
	embreeScene->instanced = true;
	embreeScene->partScenes.resize(parts.size());

	auto buildPartScene = [&](size_t partIndex) -> bool
	{
		const VoxelSourceTrianglePart& part = parts[partIndex];
		if (part.vertices == nullptr || part.triangleIndices == nullptr ||
			part.vertexStrideBytes < sizeof(float) * 3u ||
			part.triangleIndices->size() < 3u ||
			(part.triangleIndices->size() % 3u) != 0u)
		{
			return false;
		}

		const size_t sourceVertexCount = part.vertices->size() / part.vertexStrideBytes;
		const size_t sourceTriangleCount = part.triangleIndices->size() / 3u;
		if (sourceVertexCount == 0u || sourceTriangleCount == 0u ||
			sourceVertexCount > std::numeric_limits<uint32_t>::max() ||
			sourceTriangleCount > std::numeric_limits<uint32_t>::max())
		{
			return false;
		}

		EmbreeScene::PartScene& partScene = embreeScene->partScenes[partIndex];
		partScene.vertices = part.vertices;
		partScene.vertexStrideBytes = part.vertexStrideBytes;
		partScene.skinningVertices = part.skinningVertices;
		partScene.skinningVertexStrideBytes = part.skinningVertexStrideBytes;
		partScene.triangleIndices = part.triangleIndices;
		partScene.triangleCount = static_cast<uint32_t>(sourceTriangleCount);
		partScene.triangleUvChartIds = BuildTriangleUvChartIds(*part.vertices, part.vertexStrideBytes, *part.triangleIndices);
		partScene.triangleUvDensities.resize(sourceTriangleCount, DirectX::XMFLOAT2(-1.0f, -1.0f));
		partScene.triangleAreas.resize(sourceTriangleCount, 0.0f);
		partScene.scene = rtcNewScene(device);

		RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
		DirectX::XMFLOAT3* embreeVertices = static_cast<DirectX::XMFLOAT3*>(rtcSetNewGeometryBuffer(
			geometry,
			RTC_BUFFER_TYPE_VERTEX,
			0,
			RTC_FORMAT_FLOAT3,
			sizeof(DirectX::XMFLOAT3),
			sourceVertexCount));
		if (embreeVertices != nullptr)
		{
			for (size_t vertexIndex = 0; vertexIndex < sourceVertexCount; ++vertexIndex)
			{
				embreeVertices[vertexIndex] = ToXM(ReadPosition(*part.vertices, part.vertexStrideBytes, static_cast<uint32_t>(vertexIndex)));
			}
		}

		EmbreeScene::Triangle* embreeTriangles = static_cast<EmbreeScene::Triangle*>(rtcSetNewGeometryBuffer(
			geometry,
			RTC_BUFFER_TYPE_INDEX,
			0,
			RTC_FORMAT_UINT3,
			sizeof(EmbreeScene::Triangle),
			sourceTriangleCount));
		if (embreeTriangles != nullptr)
		{
			for (size_t triangleIndex = 0; triangleIndex < sourceTriangleCount; ++triangleIndex)
			{
				const size_t triangleBase = triangleIndex * 3u;
				const Float3 p0 = ReadPosition(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 0u]);
				const Float3 p1 = ReadPosition(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 1u]);
				const Float3 p2 = ReadPosition(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 2u]);
				const float area = 0.5f * std::sqrt(std::max(0.0f, (p1 - p0).cross(p2 - p0).lengthSq()));
				partScene.triangleAreas[triangleIndex] = std::isfinite(area) ? area : 0.0f;
				partScene.triangleUvDensities[triangleIndex] = ComputeTriangleUvDensity(
					p0,
					p1,
					p2,
					ReadTexcoord(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 0u]),
					ReadTexcoord(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 1u]),
					ReadTexcoord(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 2u]));
				embreeTriangles[triangleIndex] = EmbreeScene::Triangle{
					(*part.triangleIndices)[triangleBase + 0u],
					(*part.triangleIndices)[triangleBase + 1u],
					(*part.triangleIndices)[triangleBase + 2u],
				};
			}
		}

		constexpr size_t kMaxAffineUvDensitySamplesPerPart = 256u;
		float totalValidArea = 0.0f;
		for (float area : partScene.triangleAreas)
		{
			if (std::isfinite(area) && area > 1.0e-12f)
			{
				totalValidArea += area;
			}
		}
		if (totalValidArea > 0.0f)
		{
			if (sourceTriangleCount <= kMaxAffineUvDensitySamplesPerPart)
			{
				partScene.uvDensitySampleTriangles.reserve(sourceTriangleCount);
				partScene.uvDensitySampleWeights.reserve(sourceTriangleCount);
				for (uint32_t triangleIndex = 0u; triangleIndex < sourceTriangleCount; ++triangleIndex)
				{
					const float area = partScene.triangleAreas[triangleIndex];
					if (std::isfinite(area) && area > 1.0e-12f)
					{
						partScene.uvDensitySampleTriangles.push_back(triangleIndex);
						partScene.uvDensitySampleWeights.push_back(area);
					}
				}
			}
			else
			{
				partScene.uvDensitySampleTriangles.reserve(kMaxAffineUvDensitySamplesPerPart);
				partScene.uvDensitySampleWeights.reserve(kMaxAffineUvDensitySamplesPerPart);
				const float sampleWeight = totalValidArea / static_cast<float>(kMaxAffineUvDensitySamplesPerPart);
				float accumulatedArea = 0.0f;
				size_t triangleIndex = 0u;
				for (size_t sampleIndex = 0u; sampleIndex < kMaxAffineUvDensitySamplesPerPart; ++sampleIndex)
				{
					const float targetArea = (static_cast<float>(sampleIndex) + 0.5f) * sampleWeight;
					while (triangleIndex + 1u < sourceTriangleCount &&
						accumulatedArea + std::max(partScene.triangleAreas[triangleIndex], 0.0f) < targetArea)
					{
						accumulatedArea += std::max(partScene.triangleAreas[triangleIndex], 0.0f);
						++triangleIndex;
					}
					partScene.uvDensitySampleTriangles.push_back(static_cast<uint32_t>(triangleIndex));
					partScene.uvDensitySampleWeights.push_back(sampleWeight);
				}
			}
		}

		rtcCommitGeometry(geometry);
		rtcAttachGeometry(partScene.scene, geometry);
		rtcReleaseGeometry(geometry);
		rtcCommitScene(partScene.scene);
		return true;
	};

	std::vector<uint8_t> validParts(parts.size(), 0u);
	for (size_t partIndex = 0; partIndex < parts.size(); ++partIndex)
	{
		validParts[partIndex] = buildPartScene(partIndex) ? 1u : 0u;
	}

	EmbreeScene::RefinedGroupScene topScene{};
	topScene.refinedGroup = EmbreeScene::kUnfilteredRefinedGroupScene;
	topScene.scene = rtcNewScene(device);

	uint64_t totalTriangleCount = 0u;
	for (const VoxelSourceTriangleInstance& sourceInstance : instances)
	{
		if (sourceInstance.partIndex >= parts.size() || validParts[sourceInstance.partIndex] == 0u)
		{
			continue;
		}
		const EmbreeScene::PartScene& partScene = embreeScene->partScenes[sourceInstance.partIndex];
		if (partScene.scene == nullptr || partScene.triangleCount == 0u ||
			totalTriangleCount > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - partScene.triangleCount)
		{
			continue;
		}

		RTCGeometry instanceGeometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
		rtcSetGeometryInstancedScene(instanceGeometry, partScene.scene);
		float instanceTransform[12]{};
		StoreEmbreeTransform3x4(sourceInstance.localToWorld, instanceTransform);
		rtcSetGeometryTransform(instanceGeometry, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, instanceTransform);
		rtcCommitGeometry(instanceGeometry);
		const uint32_t geometryId = rtcAttachGeometry(topScene.scene, instanceGeometry);
		rtcReleaseGeometry(instanceGeometry);

		const uint32_t instanceIndex = static_cast<uint32_t>(embreeScene->instances.size());
		if (geometryId >= embreeScene->instanceIndexByGeometryId.size())
		{
			embreeScene->instanceIndexByGeometryId.resize(static_cast<size_t>(geometryId) + 1ull, std::numeric_limits<uint32_t>::max());
		}
		embreeScene->instanceIndexByGeometryId[geometryId] = instanceIndex;
		embreeScene->instances.push_back(EmbreeScene::InstanceRef{
			.partIndex = sourceInstance.partIndex,
			.localToWorld = sourceInstance.localToWorld,
			.refinedGroup = sourceInstance.refinedGroup,
			.firstTriangle = static_cast<uint32_t>(totalTriangleCount),
			.triangleCount = partScene.triangleCount,
			.boneRemapIndices = std::vector<uint32_t>(
				sourceInstance.boneRemapIndices.begin(), sourceInstance.boneRemapIndices.end()) });
		totalTriangleCount += partScene.triangleCount;
	}

	if (embreeScene->instances.empty())
	{
		rtcReleaseScene(topScene.scene);
		return;
	}

	rtcCommitScene(topScene.scene);
	embreeScene->refinedGroupScenes.push_back(std::move(topScene));
	TracyPlot("CLOD.Voxel.BVH.InstancedTriangles", static_cast<int64_t>(totalTriangleCount));
	TracyPlot("CLOD.Voxel.BVH.Instances", static_cast<int64_t>(embreeScene->instances.size()));
	spdlog::debug(
		"Voxel coverage Embree instanced BVH built: parts={} instances={} logical_triangles={} scenes=1",
		parts.size(),
		embreeScene->instances.size(),
		totalTriangleCount);
	m_embreeScene = std::move(embreeScene);
	m_uvDensity = ComputeUvDensity();
}

bool VoxelSourceTriangleBVH::IsValid() const
{
	if (m_embreeScene == nullptr || m_embreeScene->refinedGroupScenes.empty())
	{
		return false;
	}
	return m_embreeScene->instanced || (m_vertices != nullptr && m_triangleIndices != nullptr);
}

void VoxelSourceTriangleBVH::SetRefinedGroupDomainMap(std::vector<std::vector<int32_t>> refinedGroupDomainMap)
{
	m_refinedGroupDomainMap = std::move(refinedGroupDomainMap);
	for (std::vector<int32_t>& domain : m_refinedGroupDomainMap)
	{
		std::sort(domain.begin(), domain.end());
		domain.erase(std::unique(domain.begin(), domain.end()), domain.end());
	}
}

bool VoxelSourceTriangleBVH::IntersectNearest(
	int32_t refinedGroupFilter,
	const DirectX::XMFLOAT3& origin,
	const DirectX::XMFLOAT3& direction,
	float tMin,
	float tMax,
	uint32_t& outTriangleIndex,
	float& outT,
	float& outU,
	float& outV) const
{
	if (m_embreeScene == nullptr || !std::isfinite(tMin) || !std::isfinite(tMax) || tMax <= tMin)
	{
		return false;
	}

	const EmbreeScene::RefinedGroupScene* scene = nullptr;
	const EmbreeScene::RefinedGroupScene* unfilteredScene = nullptr;
	for (const EmbreeScene::RefinedGroupScene& candidateScene : m_embreeScene->refinedGroupScenes)
	{
		if (candidateScene.refinedGroup == EmbreeScene::kUnfilteredRefinedGroupScene)
		{
			unfilteredScene = &candidateScene;
		}
		if (candidateScene.refinedGroup == refinedGroupFilter)
		{
			scene = &candidateScene;
			break;
		}
	}
	const bool useDomainFilteredScene =
		refinedGroupFilter >= 0 &&
		static_cast<size_t>(refinedGroupFilter) < m_refinedGroupDomainMap.size() &&
		!m_refinedGroupDomainMap[static_cast<size_t>(refinedGroupFilter)].empty() &&
		unfilteredScene != nullptr &&
		unfilteredScene->scene != nullptr;
	if (useDomainFilteredScene)
	{
		scene = unfilteredScene;
	}
	// A non-terminal refined group must only trace its exact domain. Falling
	// back to the unfiltered scene makes each child sample the whole object,
	// which shows up as duplicated coarse voxel sections.
	if (scene == nullptr && refinedGroupFilter == -1)
	{
		for (const EmbreeScene::RefinedGroupScene& candidateScene : m_embreeScene->refinedGroupScenes)
		{
			if (candidateScene.refinedGroup == -1)
			{
				scene = &candidateScene;
				break;
			}
		}
	}
	if (scene == nullptr || scene->scene == nullptr)
	{
		return false;
	}

	auto triangleAcceptedByDomain = [&](uint32_t triangleIndex)
	{
		if (!useDomainFilteredScene)
		{
			return true;
		}
		if (m_triangleRefinedGroupIds == nullptr || triangleIndex >= m_triangleRefinedGroupIds->size())
		{
			return false;
		}

		const int32_t triangleGroup = (*m_triangleRefinedGroupIds)[triangleIndex];
		const std::vector<int32_t>& domain = m_refinedGroupDomainMap[static_cast<size_t>(refinedGroupFilter)];
		return std::binary_search(domain.begin(), domain.end(), triangleGroup);
	};
	auto instanceAcceptedByDomain = [&](const EmbreeScene::InstanceRef& instance)
	{
		if (!useDomainFilteredScene)
		{
			return true;
		}
		const std::vector<int32_t>& domain = m_refinedGroupDomainMap[static_cast<size_t>(refinedGroupFilter)];
		return std::binary_search(domain.begin(), domain.end(), instance.refinedGroup);
	};

	float traceTMin = std::max(0.0f, tMin);
	while (traceTMin < tMax)
	{
		RTCRayHit rayHit{};
		rayHit.ray.org_x = origin.x;
		rayHit.ray.org_y = origin.y;
		rayHit.ray.org_z = origin.z;
		rayHit.ray.dir_x = direction.x;
		rayHit.ray.dir_y = direction.y;
		rayHit.ray.dir_z = direction.z;
		rayHit.ray.tnear = traceTMin;
		rayHit.ray.tfar = tMax;
		rayHit.ray.mask = 0xFFFFFFFFu;
		rayHit.ray.flags = 0u;
		rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
		rayHit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

		RTCIntersectArguments args;
		rtcInitIntersectArguments(&args);
		rtcIntersect1(scene->scene, &rayHit, &args);
		if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID || rayHit.hit.primID == RTC_INVALID_GEOMETRY_ID)
		{
			return false;
		}

		if (m_embreeScene->instanced)
		{
			const uint32_t instanceGeometryId = rayHit.hit.instID[0];
			if (instanceGeometryId == RTC_INVALID_GEOMETRY_ID ||
				instanceGeometryId >= m_embreeScene->instanceIndexByGeometryId.size())
			{
				return false;
			}
			const uint32_t instanceIndex = m_embreeScene->instanceIndexByGeometryId[instanceGeometryId];
			if (instanceIndex == std::numeric_limits<uint32_t>::max() ||
				instanceIndex >= m_embreeScene->instances.size())
			{
				return false;
			}
			const EmbreeScene::InstanceRef& instance = m_embreeScene->instances[instanceIndex];
			if (rayHit.hit.primID >= instance.triangleCount)
			{
				return false;
			}
			if (instanceAcceptedByDomain(instance))
			{
				outTriangleIndex = instance.firstTriangle + rayHit.hit.primID;
				outT = rayHit.ray.tfar;
				outU = rayHit.hit.u;
				outV = rayHit.hit.v;
				return true;
			}
		}
		else
		{
			if (rayHit.hit.primID >= scene->sourceTriangleIndices.size())
			{
				return false;
			}

			const uint32_t sourceTriangleIndex = scene->sourceTriangleIndices[rayHit.hit.primID];
			if (triangleAcceptedByDomain(sourceTriangleIndex))
			{
				outTriangleIndex = sourceTriangleIndex;
				outT = rayHit.ray.tfar;
				outU = rayHit.hit.u;
				outV = rayHit.hit.v;
				return true;
			}
		}

		const float nextTraceTMin = std::max(
			rayHit.ray.tfar + 1.0e-5f,
			std::nextafter(traceTMin, std::numeric_limits<float>::infinity()));
		if (nextTraceTMin <= traceTMin)
		{
			return false;
		}
		traceTMin = nextTraceTMin;
	}

	return false;
}

bool VoxelSourceTriangleBVH::GetTriangleSample(
	uint32_t triangleIndex,
	VoxelSourceTriangleSample& outSample) const
{
	if (m_embreeScene == nullptr)
	{
		return false;
	}

	auto fillFromFlatTriangle = [&](
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& triangleIndices,
		uint32_t localTriangleIndex,
		const ClusterLODAssemblyTransform* transform,
		const std::vector<std::byte>* skinningVertices,
		size_t skinningVertexStrideBytes,
		std::span<const uint32_t> boneRemapIndices) -> bool
	{
		const size_t triangleBase = static_cast<size_t>(localTriangleIndex) * 3u;
		if (vertexStrideBytes < sizeof(float) * 3u || triangleBase + 2u >= triangleIndices.size())
		{
			return false;
		}

		for (uint32_t corner = 0; corner < 3u; ++corner)
		{
			const uint32_t vertexIndex = triangleIndices[triangleBase + corner];
			DirectX::XMFLOAT3 position = ToXM(ReadPosition(vertices, vertexStrideBytes, vertexIndex));
			DirectX::XMFLOAT3 normal = ToXM(ReadNormal(vertices, vertexStrideBytes, vertexIndex));
			if (transform != nullptr)
			{
				position = TransformCoveragePoint(*transform, position);
				normal = TransformCoverageVector(*transform, normal);
			}
			outSample.positions[corner] = position;
			outSample.normals[corner] = normal;
			outSample.uvs[corner] = ReadTexcoord(vertices, vertexStrideBytes, vertexIndex);
		}
		outSample.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		if (skinningVertices != nullptr && !skinningVertices->empty() && skinningVertexStrideBytes != 0u)
		{
			std::unordered_map<uint32_t, float> boneWeights;
			boneWeights.reserve(8u);
			for (uint32_t corner = 0; corner < 3u; ++corner)
			{
				PackedSkinningInfluences influences{};
				if (!ReadSkinningInfluences(
					*skinningVertices,
					skinningVertexStrideBytes,
					triangleIndices[triangleBase + corner],
					influences))
				{
					continue;
				}
				AccumulateInfluenceSet(influences.joints0, influences.weights0, boneWeights);
				AccumulateInfluenceSet(influences.joints1, influences.weights1, boneWeights);
			}
			const uint32_t localBoneIndex = SelectDominantBoneIndex(boneWeights);
			if (localBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
			{
				outSample.dominantBoneIndex = localBoneIndex < boneRemapIndices.size()
					? boneRemapIndices[localBoneIndex]
					: localBoneIndex;
			}
		}
		return true;
	};

	if (m_embreeScene->instanced)
	{
		const auto instanceIt = std::upper_bound(
			m_embreeScene->instances.begin(),
			m_embreeScene->instances.end(),
			triangleIndex,
			[](uint32_t value, const EmbreeScene::InstanceRef& instance) {
				return value < instance.firstTriangle;
			});
		if (instanceIt == m_embreeScene->instances.begin())
		{
			return false;
		}
		const EmbreeScene::InstanceRef& instance = *(instanceIt - 1);
		if (triangleIndex < instance.firstTriangle || triangleIndex >= instance.firstTriangle + instance.triangleCount ||
			instance.partIndex >= m_embreeScene->partScenes.size())
		{
			return false;
		}
		const EmbreeScene::PartScene& part = m_embreeScene->partScenes[instance.partIndex];
		if (part.vertices == nullptr || part.triangleIndices == nullptr)
		{
			return false;
		}
		if (!fillFromFlatTriangle(
			*part.vertices,
			part.vertexStrideBytes,
			*part.triangleIndices,
			triangleIndex - instance.firstTriangle,
			&instance.localToWorld,
			part.skinningVertices,
			part.skinningVertexStrideBytes,
			instance.boneRemapIndices))
		{
			return false;
		}
		const uint32_t localTriangleIndex = triangleIndex - instance.firstTriangle;
		const uint32_t localChartId = localTriangleIndex < part.triangleUvChartIds.size()
			? part.triangleUvChartIds[localTriangleIndex]
			: localTriangleIndex;
		outSample.uvChartId = (static_cast<uint64_t>(instance.partIndex) << 32u) | localChartId;
		return true;
	}

	if (m_vertices == nullptr || m_triangleIndices == nullptr)
	{
		return false;
	}
	if (!fillFromFlatTriangle(
		*m_vertices,
		m_vertexStrideBytes,
		*m_triangleIndices,
		triangleIndex,
		nullptr,
		m_skinningVertices,
		m_skinningVertexStrideBytes,
		{}))
	{
		return false;
	}
	outSample.uvChartId = triangleIndex < m_embreeScene->triangleUvChartIds.size()
		? m_embreeScene->triangleUvChartIds[triangleIndex]
		: triangleIndex;
	outSample.dominantBoneIndex = ComputeDominantBoneIndexForSourceTriangle(*this, triangleIndex);
	return true;
}

DirectX::XMFLOAT2 VoxelSourceTriangleBVH::ComputeUvDensity() const
{
	ZoneScopedN("VoxelSourceTriangleBVH::ComputeUvDensity");
	struct WeightedValue
	{
		float value;
		float weight;
		uint64_t stableIndex;
	};

	std::vector<WeightedValue> uValues;
	std::vector<WeightedValue> vValues;
	auto appendDensity = [&](const DirectX::XMFLOAT2& density, float area, uint64_t stableIndex)
	{
		if (!std::isfinite(area) || area <= 1.0e-12f)
		{
			return;
		}
		if (std::isfinite(density.x) && density.x >= 0.0f)
		{
			uValues.push_back({ density.x, area, stableIndex });
		}
		if (std::isfinite(density.y) && density.y >= 0.0f)
		{
			vValues.push_back({ density.y, area, stableIndex });
		}
	};

	if (m_embreeScene != nullptr && m_embreeScene->instanced)
	{
		struct SimilarityGroup
		{
			uint32_t partIndex = 0;
			float scale = 1.0f;
			float areaScaleSum = 0.0f;
		};
		std::vector<SimilarityGroup> similarityGroups;
		std::vector<uint32_t> nonSimilarityInstances;
		similarityGroups.reserve(m_embreeScene->instances.size());
		nonSimilarityInstances.reserve(m_embreeScene->instances.size());

		auto similarityScale = [](const ClusterLODAssemblyTransform& transform, float& outScale) -> bool
		{
			const Float3 c0(transform.row0.x, transform.row1.x, transform.row2.x);
			const Float3 c1(transform.row0.y, transform.row1.y, transform.row2.y);
			const Float3 c2(transform.row0.z, transform.row1.z, transform.row2.z);
			const float l0 = c0.lengthSq();
			const float l1 = c1.lengthSq();
			const float l2 = c2.lengthSq();
			const float maxLengthSq = std::max(l0, std::max(l1, l2));
			if (!std::isfinite(maxLengthSq) || maxLengthSq <= 1.0e-12f)
			{
				return false;
			}
			const float tolerance = maxLengthSq * 1.0e-5f;
			if (std::abs(l0 - l1) > tolerance || std::abs(l0 - l2) > tolerance ||
				std::abs(c0.dot(c1)) > tolerance || std::abs(c0.dot(c2)) > tolerance || std::abs(c1.dot(c2)) > tolerance)
			{
				return false;
			}
			outScale = std::sqrt((l0 + l1 + l2) / 3.0f);
			if (std::abs(outScale - 1.0f) <= 1.0e-5f)
			{
				outScale = 1.0f;
			}
			return std::isfinite(outScale) && outScale > 1.0e-6f;
		};

		for (uint32_t instanceIndex = 0u; instanceIndex < m_embreeScene->instances.size(); ++instanceIndex)
		{
			const EmbreeScene::InstanceRef& instance = m_embreeScene->instances[instanceIndex];
			float scale = 1.0f;
			if (!similarityScale(instance.localToWorld, scale))
			{
				nonSimilarityInstances.push_back(instanceIndex);
				continue;
			}
			auto groupIt = std::find_if(similarityGroups.begin(), similarityGroups.end(), [&](const SimilarityGroup& group) {
				return group.partIndex == instance.partIndex && std::bit_cast<uint32_t>(group.scale) == std::bit_cast<uint32_t>(scale);
			});
			if (groupIt == similarityGroups.end())
			{
				groupIt = similarityGroups.insert(similarityGroups.end(), SimilarityGroup{ instance.partIndex, scale, 0.0f });
			}
			groupIt->areaScaleSum += scale * scale;
		}

		size_t collapsedTriangleCount = 0u;
		for (const SimilarityGroup& group : similarityGroups)
		{
			if (group.partIndex < m_embreeScene->partScenes.size())
			{
				collapsedTriangleCount += m_embreeScene->partScenes[group.partIndex].triangleCount;
			}
		}
		uValues.reserve(collapsedTriangleCount);
		vValues.reserve(collapsedTriangleCount);
		uint64_t stableIndex = 0u;
		for (const SimilarityGroup& group : similarityGroups)
		{
			if (group.partIndex >= m_embreeScene->partScenes.size())
			{
				continue;
			}
			const EmbreeScene::PartScene& part = m_embreeScene->partScenes[group.partIndex];
			const size_t triangleCount = std::min(part.triangleUvDensities.size(), part.triangleAreas.size());
			for (size_t triangleIndex = 0u; triangleIndex < triangleCount; ++triangleIndex)
			{
				const DirectX::XMFLOAT2 localDensity = part.triangleUvDensities[triangleIndex];
				appendDensity(
					DirectX::XMFLOAT2(localDensity.x / group.scale, localDensity.y / group.scale),
					part.triangleAreas[triangleIndex] * group.areaScaleSum,
					stableIndex++);
			}
		}

		for (uint32_t instanceIndex : nonSimilarityInstances)
		{
			const EmbreeScene::InstanceRef& instance = m_embreeScene->instances[instanceIndex];
			if (instance.partIndex >= m_embreeScene->partScenes.size())
			{
				continue;
			}
			const EmbreeScene::PartScene& part = m_embreeScene->partScenes[instance.partIndex];
			const size_t sampleCount = std::min(part.uvDensitySampleTriangles.size(), part.uvDensitySampleWeights.size());
			for (size_t sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
			{
				const uint32_t triangleIndex = part.uvDensitySampleTriangles[sampleIndex];
				const size_t triangleBase = static_cast<size_t>(triangleIndex) * 3u;
				if (part.vertices == nullptr || part.triangleIndices == nullptr || triangleBase + 2u >= part.triangleIndices->size())
				{
					continue;
				}
				const Float3 p0 = ToFloat3(TransformCoveragePoint(instance.localToWorld, ToXM(ReadPosition(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 0u]))));
				const Float3 p1 = ToFloat3(TransformCoveragePoint(instance.localToWorld, ToXM(ReadPosition(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 1u]))));
				const Float3 p2 = ToFloat3(TransformCoveragePoint(instance.localToWorld, ToXM(ReadPosition(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 2u]))));
				const float transformedArea = 0.5f * std::sqrt(std::max(0.0f, (p1 - p0).cross(p2 - p0).lengthSq()));
				const float localArea = triangleIndex < part.triangleAreas.size() ? part.triangleAreas[triangleIndex] : 0.0f;
				const float transformedSampleWeight = localArea > 1.0e-12f
					? part.uvDensitySampleWeights[sampleIndex] * (transformedArea / localArea)
					: 0.0f;
				appendDensity(
					ComputeTriangleUvDensity(
						p0, p1, p2,
						ReadTexcoord(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 0u]),
						ReadTexcoord(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 1u]),
						ReadTexcoord(*part.vertices, part.vertexStrideBytes, (*part.triangleIndices)[triangleBase + 2u])),
					transformedSampleWeight,
					stableIndex++);
			}
		}
	}
	else if (m_vertices != nullptr && m_triangleIndices != nullptr)
	{
		const uint32_t triangleCount = static_cast<uint32_t>(std::min<size_t>(
			m_triangleIndices->size() / 3u,
			std::numeric_limits<uint32_t>::max()));
		uValues.reserve(triangleCount);
		vValues.reserve(triangleCount);
		for (uint32_t triangleIndex = 0u; triangleIndex < triangleCount; ++triangleIndex)
		{
			const size_t triangleBase = static_cast<size_t>(triangleIndex) * 3u;
			const Float3 p0 = ReadPosition(*m_vertices, m_vertexStrideBytes, (*m_triangleIndices)[triangleBase + 0u]);
			const Float3 p1 = ReadPosition(*m_vertices, m_vertexStrideBytes, (*m_triangleIndices)[triangleBase + 1u]);
			const Float3 p2 = ReadPosition(*m_vertices, m_vertexStrideBytes, (*m_triangleIndices)[triangleBase + 2u]);
			const float area = 0.5f * std::sqrt(std::max(0.0f, (p1 - p0).cross(p2 - p0).lengthSq()));
			appendDensity(
				ComputeTriangleUvDensity(
					p0, p1, p2,
					ReadTexcoord(*m_vertices, m_vertexStrideBytes, (*m_triangleIndices)[triangleBase + 0u]),
					ReadTexcoord(*m_vertices, m_vertexStrideBytes, (*m_triangleIndices)[triangleBase + 1u]),
					ReadTexcoord(*m_vertices, m_vertexStrideBytes, (*m_triangleIndices)[triangleBase + 2u])),
				area,
				triangleIndex);
		}
	}

	auto weightedMedian = [](std::vector<WeightedValue>& values) -> float
	{
		if (values.empty())
		{
			return 0.0f;
		}
		std::sort(values.begin(), values.end(), [](const WeightedValue& lhs, const WeightedValue& rhs) {
			return lhs.value == rhs.value ? lhs.stableIndex < rhs.stableIndex : lhs.value < rhs.value;
		});
		float totalWeight = 0.0f;
		for (const WeightedValue& value : values)
		{
			totalWeight += value.weight;
		}
		const float targetWeight = totalWeight * 0.5f;
		float accumulatedWeight = 0.0f;
		for (const WeightedValue& value : values)
		{
			accumulatedWeight += value.weight;
			if (accumulatedWeight >= targetWeight)
			{
				return value.value;
			}
		}
		return values.back().value;
	};

	return DirectX::XMFLOAT2(weightedMedian(uValues), weightedMedian(vValues));
}

// Public API: VoxelizeTriangles
VoxelizeTrianglesResult VoxelizeTrianglesDetailed(const VoxelizeTrianglesInput& input)
{
	ZoneScopedN("VoxelGroupBuilder::VoxelizeTrianglesDetailed");
	std::pmr::synchronized_pool_resource scratchResource;
	ScopedVoxelizationScratchResource scratchScope(&scratchResource);
	VoxelizeScratchState scratch;
	scratch.Begin();
	VoxelizeTrianglesResult detailedResult{};
	VoxelGroupPayload result{};

	const bool hasTriangleSources = input.vertices != nullptr && input.vertexStrideBytes >= sizeof(float) * 3 &&
		input.triangleIndices != nullptr && !input.triangleIndices->empty() && (input.triangleIndices->size() % 3) == 0;
	const bool hasVoxelSources = HasVoxelSources(input);
	const bool hasCandidateVoxelSources = HasCandidateVoxelSources(input);
	const bool hasCoverageSourceTriangles = input.coverageSourceTriangles != nullptr && input.coverageSourceTriangles->IsValid();
	TracyPlot("CLOD.Voxelize.SourceTriangles", static_cast<int64_t>(hasTriangleSources ? input.triangleIndices->size() / 3u : 0u));
	TracyPlot("CLOD.Voxelize.Resolution", static_cast<int64_t>(input.resolution));
	TracyPlot("CLOD.Voxelize.RaysPerCell", static_cast<int64_t>(input.raysPerCell));

	if (!hasTriangleSources && !hasVoxelSources && !hasCandidateVoxelSources)
		return detailedResult;

	if (hasTriangleSources && input.vertexStrideBytes < sizeof(float) * 3)
		return detailedResult;
	
	if (input.resolution < 2)
		return detailedResult;

	if (!(input.voxelWidth > 0.0f) || !std::isfinite(input.voxelWidth))
		return detailedResult;

	const Float3 aabbMin = ToFloat3(input.aabbMin);
	const Float3 aabbMax = ToFloat3(input.aabbMax);

	if (aabbMax.x - aabbMin.x <= 0.0f ||
		aabbMax.y - aabbMin.y <= 0.0f ||
		aabbMax.z - aabbMin.z <= 0.0f)
	{
		spdlog::warn("VoxelGroupBuilder: degenerate AABB, skipping triangle voxelization");
		return detailedResult;
	}

	result.resolution = input.resolution;
	result.aabbMin = input.aabbMin;
	result.aabbMax = DirectX::XMFLOAT3(
		input.aabbMin.x + input.voxelWidth * static_cast<float>(input.resolution),
		input.aabbMin.y + input.voxelWidth * static_cast<float>(input.resolution),
		input.aabbMin.z + input.voxelWidth * static_cast<float>(input.resolution));
	result.voxelWidth = input.voxelWidth;
	result.uvDensity = hasCoverageSourceTriangles
		? input.coverageSourceTriangles->UvDensity()
		: DirectX::XMFLOAT2(0.0f, 0.0f);

	ScratchCellTriMap& cellTriMap = scratch.cellTriMap;
	if (hasTriangleSources)
	{
		ZoneScopedN("VoxelGroupBuilder::VoxelizeTrianglesDetailed::TriangleSources");
		RasterizeTrianglesToGrid(
			*input.vertices, input.vertexStrideBytes,
			*input.triangleIndices,
			aabbMin, input.voxelWidth,
			input.resolution,
			scratch);
	}

	std::vector<VoxelSourcePayloadInstance>& candidateVoxelPayloads = scratch.candidateVoxelPayloads;
	if (hasCandidateVoxelSources)
	{
		ZoneScopedN("VoxelGroupBuilder::VoxelizeTrianglesDetailed::CandidateVoxelSources");
		if (input.candidateVoxelPayloadInstances != nullptr)
		{
			candidateVoxelPayloads = *input.candidateVoxelPayloadInstances;
		}
		else if (input.candidateVoxelPayloads != nullptr)
		{
			candidateVoxelPayloads.reserve(input.candidateVoxelPayloads->size());
			for (const VoxelSourceCandidatePayload& payload : *input.candidateVoxelPayloads)
			{
				candidateVoxelPayloads.push_back(VoxelSourcePayloadInstance{
					.payload = payload.payload,
					.expansionRadius = payload.expansionRadius });
			}
		}
	}
	else if (hasVoxelSources)
	{
		ZoneScopedN("VoxelGroupBuilder::VoxelizeTrianglesDetailed::SourceVoxelCandidateSources");
		if (input.sourceVoxelPayloadInstances != nullptr)
		{
			candidateVoxelPayloads = *input.sourceVoxelPayloadInstances;
		}
		else if (input.sourceVoxelPayloads != nullptr)
		{
			candidateVoxelPayloads.reserve(input.sourceVoxelPayloads->size());
			for (const VoxelGroupPayload* payload : *input.sourceVoxelPayloads)
			{
				candidateVoxelPayloads.push_back(VoxelSourcePayloadInstance{ .payload = payload });
			}
		}
	}
	if (!candidateVoxelPayloads.empty())
	{
		RasterizeVoxelPayloadsToBricks(
			candidateVoxelPayloads,
			aabbMin,
			input.voxelWidth,
			input.resolution,
			scratch);
	}
	detailedResult.triangleCandidateCellCount = static_cast<uint32_t>(std::min<size_t>(scratch.cellTriKeys.size(), std::numeric_limits<uint32_t>::max()));
	detailedResult.voxelCandidateCellCount = static_cast<uint32_t>(std::min<size_t>(
		scratch.voxelRasterCandidateCellCount,
		std::numeric_limits<uint32_t>::max()));
	TracyPlot("CLOD.Voxelize.TriangleCandidateCells", static_cast<int64_t>(detailedResult.triangleCandidateCellCount));
	TracyPlot("CLOD.Voxelize.VoxelCandidateCells", static_cast<int64_t>(detailedResult.voxelCandidateCellCount));

	if (scratch.cellTriKeys.empty() && scratch.voxelRasterActiveCells.empty())
		return detailedResult;

	const uint32_t baseRaySeed = input.resolution * 2654435761u;

	const Float3 cellSize = {
		input.voxelWidth,
		input.voxelWidth,
		input.voxelWidth
	};

	PmrUInt64Vector& candidateKeys = scratch.candidateKeys;
	candidateKeys.reserve(scratch.cellTriKeys.size() + scratch.voxelRasterActiveCells.size());
	for (uint64_t key : scratch.cellTriKeys)
	{
		candidateKeys.push_back(key);
	}
	for (const VoxelRasterActiveCell& rasterCell : scratch.voxelRasterActiveCells)
	{
		const uint64_t key = rasterCell.cellKey;
		const auto triIt = cellTriMap.find(key);
		if (triIt == cellTriMap.end() || triIt->second.generation != scratch.generation)
		{
			candidateKeys.push_back(key);
		}
	}
	detailedResult.candidateCellCount = static_cast<uint32_t>(std::min<size_t>(candidateKeys.size(), std::numeric_limits<uint32_t>::max()));
	TracyPlot("CLOD.Voxelize.CandidateCells", static_cast<int64_t>(detailedResult.candidateCellCount));

	{
		std::pmr::vector<VoxelCoverageWorkResult>& coverageWorkResults = scratch.coverageWorkResults;
		constexpr size_t kCoverageCellsPerWorkRange = 256u;
		const size_t coverageWorkRangeCount = (candidateKeys.size() + kCoverageCellsPerWorkRange - 1u) / kCoverageCellsPerWorkRange;
		TracyPlot("CLOD.Voxelize.CoverageWorkRanges", static_cast<int64_t>(coverageWorkRangeCount));
		if (coverageWorkResults.size() < coverageWorkRangeCount)
		{
			coverageWorkResults.resize(coverageWorkRangeCount);
		}
		for (size_t rangeIndex = 0; rangeIndex < coverageWorkRangeCount; ++rangeIndex)
		{
			coverageWorkResults[rangeIndex].Reset();
		}
		{
			ZoneNamedN(voxelTraceCoverageZone, "VoxelGroupBuilder::VoxelizeTrianglesDetailed::TraceCoverage", true);
			TaskSchedulerManager::GetInstance().ParallelFor("VoxelGroupBuilder::TraceCoverage", coverageWorkRangeCount,
				[&](size_t coverageWorkRangeIndex)
			{
				VoxelCoverageWorkResult& workResult = coverageWorkResults[coverageWorkRangeIndex];
		auto getRefinedGroupStats = [&workResult](int32_t refinedGroup) -> VoxelizeTrianglesResult::RefinedGroupStats&
		{
			for (VoxelizeTrianglesResult::RefinedGroupStats& stats : workResult.refinedGroupStats)
			{
				if (stats.refinedGroup == refinedGroup)
				{
					return stats;
				}
			}

			VoxelizeTrianglesResult::RefinedGroupStats& stats = workResult.refinedGroupStats.emplace_back();
			stats.refinedGroup = refinedGroup;
			return stats;
		};

		const size_t coverageBegin = coverageWorkRangeIndex * kCoverageCellsPerWorkRange;
		const size_t coverageEnd = std::min(candidateKeys.size(), coverageBegin + kCoverageCellsPerWorkRange);
		for (size_t candidateKeyIndex = coverageBegin; candidateKeyIndex < coverageEnd; ++candidateKeyIndex)
		{
		const uint64_t key = candidateKeys[candidateKeyIndex];
		uint32_t cx, cy, cz;
		UnpackCell(key, cx, cy, cz);

		Float3 cellMin = {
			aabbMin.x + static_cast<float>(cx) * cellSize.x,
			aabbMin.y + static_cast<float>(cy) * cellSize.y,
			aabbMin.z + static_cast<float>(cz) * cellSize.z
		};
		Float3 cellMax = {
			cellMin.x + cellSize.x,
			cellMin.y + cellSize.y,
			cellMin.z + cellSize.z
		};

		const auto triIt = cellTriMap.find(key);
		VoxelRasterCellData* rasterCell = FindVoxelRasterCell(scratch, key, input.resolution);
		const bool hasTriCell = triIt != cellTriMap.end() && triIt->second.generation == scratch.generation;
		const bool hasRasterOwner = rasterCell != nullptr && rasterCell->owner != nullptr;
		const bool hasCandidateCell = hasRasterOwner && rasterCell->candidateGroupCount != 0u;
		std::vector<int32_t> refinedGroups;
		if (hasTriCell && hasTriangleSources)
		{
			for (uint32_t triangleIndex : triIt->second.triangleIndices)
			{
				int32_t refinedGroup = -1;
				if (input.triangleRefinedGroupIds != nullptr && triangleIndex < input.triangleRefinedGroupIds->size())
				{
					refinedGroup = (*input.triangleRefinedGroupIds)[triangleIndex];
				}
				AddUniqueRefinedGroup(refinedGroups, refinedGroup);
			}
		}
		if (hasCandidateCell)
		{
			const PmrInt32Vector& candidateRefinedGroups = rasterCell->owner->candidateRefinedGroups;
			for (uint32_t candidateGroupOffset = 0u; candidateGroupOffset < rasterCell->candidateGroupCount; ++candidateGroupOffset)
			{
				AddUniqueRefinedGroup(refinedGroups, candidateRefinedGroups[rasterCell->candidateGroupFirst + candidateGroupOffset]);
			}
		}
		if (refinedGroups.empty())
		{
			refinedGroups.push_back(-1);
		}

		std::vector<Ray> rays;
		rays.reserve(std::max(1u, input.raysPerCell));
		std::sort(refinedGroups.begin(), refinedGroups.end());
		for (int32_t refinedGroup : refinedGroups)
		{
			GenerateCellRays(
				std::max(1u, input.raysPerCell),
				HashVoxelCellSampleSeed(baseRaySeed, key, refinedGroup),
				rays);
			VoxelizeTrianglesResult::RefinedGroupStats& stats = getRefinedGroupStats(refinedGroup);
			++stats.candidateKeys;
			const bool hasOnlyCandidateSource = !hasTriCell && hasCandidateCell;
			if (hasOnlyCandidateSource)
			{
				++stats.candidateOnlyCells;
			}

			std::vector<uint32_t> ownedTriangles;
			if (hasTriCell && hasTriangleSources)
			{
				ownedTriangles.reserve(triIt->second.triangleIndices.size());
				for (uint32_t triangleIndex : triIt->second.triangleIndices)
				{
					int32_t triangleRefinedGroup = -1;
					if (input.triangleRefinedGroupIds != nullptr && triangleIndex < input.triangleRefinedGroupIds->size())
					{
						triangleRefinedGroup = (*input.triangleRefinedGroupIds)[triangleIndex];
					}
					if (triangleRefinedGroup == refinedGroup)
					{
						ownedTriangles.push_back(triangleIndex);
					}
				}
			}
			if (!ownedTriangles.empty())
			{
				++stats.triangleOwnedCells;
			}

			if (hasCandidateCell)
			{
				const PmrInt32Vector& candidateRefinedGroups = rasterCell->owner->candidateRefinedGroups;
				const auto candidateBegin = candidateRefinedGroups.begin() + rasterCell->candidateGroupFirst;
				const auto candidateEnd = candidateBegin + rasterCell->candidateGroupCount;
				if (std::find(candidateBegin, candidateEnd, refinedGroup) != candidateEnd)
				{
					++stats.candidateOwnedCells;
				}
			}

			CellCoverageSample coverage{};
			uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
			if (hasCoverageSourceTriangles)
			{
				const int32_t coverageRefinedGroup =
					refinedGroup < 0 &&
					input.terminalCoverageRefinedGroupOverride != std::numeric_limits<int32_t>::min()
					? input.terminalCoverageRefinedGroupOverride
					: refinedGroup;
				coverage = SampleCellCoverageSourceTriangles(
					*input.coverageSourceTriangles,
					coverageRefinedGroup,
					cellMin,
					cellMax,
					rays,
					input.coverageMaterialSampler,
					workResult.sourceCoverageQueryCount,
					workResult.sourceCoverageTriangleCandidateCount,
					workResult.sourceCoverageTriangleTestCount,
					workResult.sourceCoverageOutOfCellRejectionCount);
				if (coverage.representativeTriangleIndex != std::numeric_limits<uint32_t>::max())
				{
					VoxelSourceTriangleSample representativeSample{};
					if (input.coverageSourceTriangles->GetTriangleSample(coverage.representativeTriangleIndex, representativeSample))
					{
						dominantBoneIndex = representativeSample.dominantBoneIndex;
					}
				}
			}

			if (coverage.coverage > 0.0f)
			{
				++workResult.positiveCoverageCellCount;
				workResult.totalCoverage += coverage.coverage;
				workResult.maxCoverage = std::max(workResult.maxCoverage, coverage.coverage);
				++stats.positiveCoverageCells;
				stats.totalCoverage += coverage.coverage;
				stats.maxCoverage = std::max(stats.maxCoverage, coverage.coverage);
			}
			if (coverage.coverage <= 0.0f)
			{
				++stats.zeroCoverageDroppedCells;
				continue;
			}

			Float3 normalSum = coverage.accumulatedNormal;
			if (normalSum.lengthSq() <= 1.0e-20f && !ownedTriangles.empty())
			{
				for (uint32_t triIndex : ownedTriangles)
				{
					const uint32_t i0 = (*input.triangleIndices)[static_cast<size_t>(triIndex) * 3u + 0u];
					const uint32_t i1 = (*input.triangleIndices)[static_cast<size_t>(triIndex) * 3u + 1u];
					const uint32_t i2 = (*input.triangleIndices)[static_cast<size_t>(triIndex) * 3u + 2u];
					const Float3 p0 = ReadPosition(*input.vertices, input.vertexStrideBytes, i0);
					const Float3 p1 = ReadPosition(*input.vertices, input.vertexStrideBytes, i1);
					const Float3 p2 = ReadPosition(*input.vertices, input.vertexStrideBytes, i2);
					normalSum = normalSum + TriangleNormal(p0, p1, p2);
				}
			}

			VoxelCell vc{};
			vc.x = cx;
			vc.y = cy;
			vc.z = cz;
			vc.opacity = coverage.coverage;
			SymmetricMatrix3 sggx = coverage.sggxWeight > 0.0f
				? coverage.accumulatedSGGX * (1.0f / coverage.sggxWeight)
				: SGGXFromNormal(normalSum);
			vc.sggxAxisAndSigmas = EncodeAxialSGGX(CompressSGGXToAxial(sggx));
			vc.uv = coverage.representativeUv;
			vc.dominantBoneIndex = dominantBoneIndex;
			vc.refinedGroup = refinedGroup;

			workResult.emittedCells.push_back(vc);
			++stats.emittedSourceCells;
		}
		}
			});
		}

		{
			ZoneNamedN(voxelMergeCoverageZone, "VoxelGroupBuilder::VoxelizeTrianglesDetailed::MergeCoverage", true);
			std::pmr::vector<VoxelizeTrianglesResult::RefinedGroupStats> refinedGroupStats{ scratch.resource };
			auto accumulateRefinedGroupStats = [](VoxelizeTrianglesResult::RefinedGroupStats& dst, const VoxelizeTrianglesResult::RefinedGroupStats& src)
			{
				dst.refinedGroup = src.refinedGroup;
				dst.candidateKeys += src.candidateKeys;
				dst.triangleOwnedCells += src.triangleOwnedCells;
				dst.candidateOwnedCells += src.candidateOwnedCells;
				dst.candidateOnlyCells += src.candidateOnlyCells;
				dst.positiveCoverageCells += src.positiveCoverageCells;
				dst.zeroCoverageDroppedCells += src.zeroCoverageDroppedCells;
				dst.emittedSourceCells += src.emittedSourceCells;
				dst.totalCoverage += src.totalCoverage;
				dst.maxCoverage = std::max(dst.maxCoverage, src.maxCoverage);
			};

			size_t emittedCellCount = 0u;
			for (VoxelCoverageWorkResult& workResult : coverageWorkResults)
			{
				emittedCellCount += workResult.emittedCells.size();
			}
			TracyPlot("CLOD.Voxelize.EmittedCells", static_cast<int64_t>(emittedCellCount));
			result.activeCells.resize(emittedCellCount);
			size_t emittedWriteOffset = 0u;
			for (VoxelCoverageWorkResult& workResult : coverageWorkResults)
			{
				detailedResult.positiveCoverageCellCount += workResult.positiveCoverageCellCount;
				detailedResult.totalCoverage += workResult.totalCoverage;
				detailedResult.maxCoverage = std::max(detailedResult.maxCoverage, workResult.maxCoverage);
				detailedResult.sourceCoverageQueryCount += workResult.sourceCoverageQueryCount;
				detailedResult.sourceCoverageTriangleCandidateCount += workResult.sourceCoverageTriangleCandidateCount;
				detailedResult.sourceCoverageTriangleTestCount += workResult.sourceCoverageTriangleTestCount;
				detailedResult.sourceCoverageOutOfCellRejectionCount += workResult.sourceCoverageOutOfCellRejectionCount;

				for (const VoxelizeTrianglesResult::RefinedGroupStats& stats : workResult.refinedGroupStats)
				{
					auto existingStats = std::find_if(refinedGroupStats.begin(), refinedGroupStats.end(), [&](const VoxelizeTrianglesResult::RefinedGroupStats& candidate) {
						return candidate.refinedGroup == stats.refinedGroup;
					});
					if (existingStats != refinedGroupStats.end())
					{
						accumulateRefinedGroupStats(*existingStats, stats);
					}
					else
					{
						refinedGroupStats.push_back(stats);
					}
				}

				std::move(
					workResult.emittedCells.begin(),
					workResult.emittedCells.end(),
					result.activeCells.begin() + emittedWriteOffset);
				emittedWriteOffset += workResult.emittedCells.size();
				workResult.Reset();
			}

			detailedResult.refinedGroupStats.reserve(refinedGroupStats.size());
			for (const VoxelizeTrianglesResult::RefinedGroupStats& stats : refinedGroupStats)
			{
				detailedResult.refinedGroupStats.push_back(stats);
			}
		}
	}
	{
		ZoneScopedN("VoxelGroupBuilder::VoxelizeTrianglesDetailed::SortRefinedGroupStats");
		std::sort(detailedResult.refinedGroupStats.begin(), detailedResult.refinedGroupStats.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.refinedGroup < rhs.refinedGroup;
		});
	}
	{
		ZoneScopedN("VoxelGroupBuilder::VoxelizeTrianglesDetailed::BuildPayloadOutputs");
		if (input.emitSourcePayload && input.emitRenderPayload)
		{
			detailedResult.sourcePayload = std::move(result);
			detailedResult.renderPayload = detailedResult.sourcePayload;
		}
		else if (input.emitSourcePayload)
		{
			detailedResult.sourcePayload = std::move(result);
		}
		else if (input.emitRenderPayload)
		{
			detailedResult.renderPayload = std::move(result);
		}
	}
	if (input.emitRenderPayload)
	{
		ZoneScopedN("VoxelGroupBuilder::VoxelizeTrianglesDetailed::PruneCoverage");
		detailedResult.prunedCellCount = PruneCellsByPureCoverage(detailedResult.renderPayload.activeCells);
	}
	TracyPlot("CLOD.Voxelize.SourceCells", static_cast<int64_t>(detailedResult.sourcePayload.activeCells.size()));
	TracyPlot("CLOD.Voxelize.RenderCells", static_cast<int64_t>(detailedResult.renderPayload.activeCells.size()));
	TracyPlot("CLOD.Voxelize.PrunedCells", static_cast<int64_t>(detailedResult.prunedCellCount));

	return detailedResult;
}

VoxelGroupPayload VoxelizeTriangles(const VoxelizeTrianglesInput& input)
{
	VoxelizeTrianglesInput renderOnlyInput = input;
	renderOnlyInput.emitRenderPayload = true;
	renderOnlyInput.emitSourcePayload = false;
	return VoxelizeTrianglesDetailed(renderOnlyInput).renderPayload;
}

PackedVoxelGroupBuildResult PackVoxelGroupToCubes(const PackVoxelGroupInput& input)
{
	ZoneScopedN("VoxelGroupBuilder::PackVoxelGroupToCubes");
	PackedVoxelGroupBuildResult result{};

	if (input.payload == nullptr || input.payload->resolution == 0u)
	{
		return result;
	}

	const VoxelGroupPayload& payload = *input.payload;
	const float voxelWidth = payload.voxelWidth;

	result.metadata.aabbMinAndVoxelWidth = DirectX::XMFLOAT4(
		payload.aabbMin.x,
		payload.aabbMin.y,
		payload.aabbMin.z,
		voxelWidth);
	result.metadata.aabbMaxAndError = DirectX::XMFLOAT4(
		payload.aabbMax.x,
		payload.aabbMax.y,
		payload.aabbMax.z,
		input.voxelError);
	result.metadata.firstCube = input.firstCube;
	result.metadata.resolution = payload.resolution;
	result.metadata.uvDensity = payload.uvDensity;

	struct CubeAccum
	{
		uint32_t cubeCoord = 0;
		int32_t refinedGroup = -1;
		uint64_t mask = 0;
		uint32_t minLocalX = 3;
		uint32_t minLocalY = 3;
		uint32_t minLocalZ = 3;
		uint32_t maxLocalX = 0;
		uint32_t maxLocalY = 0;
		uint32_t maxLocalZ = 0;
		float opacitySum = 0.0f;
		std::array<CLodVoxelAttributeSample, 64> attributes{};
		std::unordered_map<uint32_t, float> boneWeights;
	};

	std::unordered_map<uint64_t, CubeAccum> cubeMap;
	cubeMap.reserve(payload.activeCells.size());

	for (const VoxelCell& cell : payload.activeCells)
	{
		if (cell.opacity < input.opacityThreshold)
		{
			continue;
		}

		const uint32_t cubeX = cell.x / 4u;
		const uint32_t cubeY = cell.y / 4u;
		const uint32_t cubeZ = cell.z / 4u;
		const uint32_t localX = cell.x & 3u;
		const uint32_t localY = cell.y & 3u;
		const uint32_t localZ = cell.z & 3u;
		const uint32_t localBit = localX | (localY << 2u) | (localZ << 4u);
		const uint32_t cubeCoord = PackCubeCoord(cubeX, cubeY, cubeZ);
		const uint64_t cubeKey = (uint64_t{ cubeCoord } << 32u) | static_cast<uint32_t>(cell.refinedGroup + 1);

		CubeAccum& accum = cubeMap[cubeKey];
		accum.cubeCoord = cubeCoord;
		accum.refinedGroup = cell.refinedGroup;
		accum.mask |= (uint64_t{ 1 } << localBit);
		accum.minLocalX = std::min(accum.minLocalX, localX);
		accum.minLocalY = std::min(accum.minLocalY, localY);
		accum.minLocalZ = std::min(accum.minLocalZ, localZ);
		accum.maxLocalX = std::max(accum.maxLocalX, localX);
		accum.maxLocalY = std::max(accum.maxLocalY, localY);
		accum.maxLocalZ = std::max(accum.maxLocalZ, localZ);
		accum.opacitySum += cell.opacity;
		accum.attributes[localBit].sggxAxisAndSigmas = cell.sggxAxisAndSigmas;
		accum.attributes[localBit].opacity = cell.opacity;
		accum.attributes[localBit].uv = cell.uv;
		if (cell.dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
		{
			accum.boneWeights[cell.dominantBoneIndex] += std::max(cell.opacity, 1.0e-6f);
		}
	}

	result.cubeRecords.reserve(cubeMap.size());
	for (const auto& [cubeKey, accum] : cubeMap)
	{
		if (accum.mask == 0)
		{
			continue;
		}

		CLodVoxelCubeRecord record{};
		record.cubeCoord = accum.cubeCoord;
		record.dominantBoneIndex = accum.boneWeights.empty()
			? input.dominantBoneIndex
			: SelectDominantBoneIndex(accum.boneWeights);
		record.refinedGroup = accum.refinedGroup;
		record.occupancyMask = accum.mask;
		record.opacitySum = accum.opacitySum;
		record.firstAttribute = input.firstAttribute + static_cast<uint32_t>(result.attributeSamples.size());
		record.activeBounds = PackVoxelCubeActiveBounds(
			accum.minLocalX,
			accum.minLocalY,
			accum.minLocalZ,
			accum.maxLocalX,
			accum.maxLocalY,
			accum.maxLocalZ);
		for (uint32_t localBit = 0; localBit < 64u; ++localBit)
		{
			if ((accum.mask & (uint64_t{ 1 } << localBit)) != 0u)
			{
				result.attributeSamples.push_back(accum.attributes[localBit]);
			}
		}
		result.cubeRecords.push_back(record);
	}

	std::sort(result.cubeRecords.begin(), result.cubeRecords.end(), [](const CLodVoxelCubeRecord& lhs, const CLodVoxelCubeRecord& rhs) {
		return lhs.refinedGroup == rhs.refinedGroup ? lhs.cubeCoord < rhs.cubeCoord : lhs.refinedGroup < rhs.refinedGroup;
	});

	result.metadata.cubeCount = static_cast<uint32_t>(result.cubeRecords.size());
	TracyPlot("CLOD.Voxel.Pack.ActiveCells", static_cast<int64_t>(payload.activeCells.size()));
	TracyPlot("CLOD.Voxel.Pack.Cubes", static_cast<int64_t>(result.cubeRecords.size()));
	TracyPlot("CLOD.Voxel.Pack.AttributeSamples", static_cast<int64_t>(result.attributeSamples.size()));
	return result;
}

void BuildVoxelClustersFromCubes(PackedVoxelGroupBuildResult& packed, uint32_t maxCubesPerCluster)
{
	ZoneScopedN("VoxelGroupBuilder::BuildVoxelClustersFromCubes");
	packed.clusterRecords.clear();
	packed.metadata.clusterCount = 0u;

	if (packed.cubeRecords.empty())
	{
		return;
	}

	const uint32_t clusterLimit = std::clamp(maxCubesPerCluster, 1u, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
	const DirectX::XMFLOAT3 aabbMin{
		packed.metadata.aabbMinAndVoxelWidth.x,
		packed.metadata.aabbMinAndVoxelWidth.y,
		packed.metadata.aabbMinAndVoxelWidth.z
	};
	const float voxelWidth = packed.metadata.aabbMinAndVoxelWidth.w;
	const float cubeWidth = voxelWidth * 4.0f;

	uint32_t runBegin = 0u;
	while (runBegin < static_cast<uint32_t>(packed.cubeRecords.size()))
	{
		const int32_t refinedGroup = packed.cubeRecords[runBegin].refinedGroup;
		uint32_t runEnd = runBegin + 1u;
		while (runEnd < static_cast<uint32_t>(packed.cubeRecords.size()) &&
			packed.cubeRecords[runEnd].refinedGroup == refinedGroup)
		{
			runEnd++;
		}

		for (uint32_t clusterBegin = runBegin; clusterBegin < runEnd; clusterBegin += clusterLimit)
		{
			const uint32_t clusterEnd = std::min(runEnd, clusterBegin + clusterLimit);
			DirectX::XMFLOAT3 clusterMin{
				std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max()
			};
			DirectX::XMFLOAT3 clusterMax{
				-std::numeric_limits<float>::max(),
				-std::numeric_limits<float>::max(),
				-std::numeric_limits<float>::max()
			};

			for (uint32_t cubeIndex = clusterBegin; cubeIndex < clusterEnd; ++cubeIndex)
			{
				const uint32_t packedCoord = packed.cubeRecords[cubeIndex].cubeCoord;
				const uint32_t cubeX = packedCoord & 0x3FFu;
				const uint32_t cubeY = (packedCoord >> 10u) & 0x3FFu;
				const uint32_t cubeZ = (packedCoord >> 20u) & 0x3FFu;
				const DirectX::XMFLOAT3 cubeMin{
					aabbMin.x + static_cast<float>(cubeX) * cubeWidth,
					aabbMin.y + static_cast<float>(cubeY) * cubeWidth,
					aabbMin.z + static_cast<float>(cubeZ) * cubeWidth
				};
				const DirectX::XMFLOAT3 cubeMax{
					cubeMin.x + cubeWidth,
					cubeMin.y + cubeWidth,
					cubeMin.z + cubeWidth
				};
				clusterMin.x = std::min(clusterMin.x, cubeMin.x);
				clusterMin.y = std::min(clusterMin.y, cubeMin.y);
				clusterMin.z = std::min(clusterMin.z, cubeMin.z);
				clusterMax.x = std::max(clusterMax.x, cubeMax.x);
				clusterMax.y = std::max(clusterMax.y, cubeMax.y);
				clusterMax.z = std::max(clusterMax.z, cubeMax.z);
			}

			const DirectX::XMFLOAT3 center{
				(clusterMin.x + clusterMax.x) * 0.5f,
				(clusterMin.y + clusterMax.y) * 0.5f,
				(clusterMin.z + clusterMax.z) * 0.5f
			};
			const float dx = clusterMax.x - center.x;
			const float dy = clusterMax.y - center.y;
			const float dz = clusterMax.z - center.z;
			const float radius = std::sqrt(dx * dx + dy * dy + dz * dz);

			CLodVoxelClusterRecord cluster{};
			cluster.firstCube = clusterBegin;
			cluster.cubeCount = clusterEnd - clusterBegin;
			cluster.refinedGroup = refinedGroup;
			cluster.flags = ComputeVoxelClusterCullMetadata(packed.cubeRecords, cluster.firstCube, cluster.cubeCount);
			cluster.bounds = DirectX::XMFLOAT4(center.x, center.y, center.z, radius);
			cluster.aabbMinAndVoxelWidth = packed.metadata.aabbMinAndVoxelWidth;
			cluster.resolution = packed.metadata.resolution;
			cluster.uvDensity = packed.metadata.uvDensity;
			packed.clusterRecords.push_back(cluster);
		}

		runBegin = runEnd;
	}

	packed.metadata.clusterCount = static_cast<uint32_t>(packed.clusterRecords.size());
	TracyPlot("CLOD.Voxel.Clusters", static_cast<int64_t>(packed.clusterRecords.size()));
}

// Public API: MortonSort
std::vector<uint32_t> MortonSort(
	const DirectX::XMFLOAT3* positions,
	uint32_t count,
	const DirectX::XMFLOAT3& aabbMin,
	const DirectX::XMFLOAT3& aabbMax)
{
	std::vector<uint32_t> permutation(count);
	std::iota(permutation.begin(), permutation.end(), 0u);

	if (count <= 1)
		return permutation;

	const float extX = aabbMax.x - aabbMin.x;
	const float extY = aabbMax.y - aabbMin.y;
	const float extZ = aabbMax.z - aabbMin.z;

	const float invExtX = extX > 1e-12f ? 1.0f / extX : 0.0f;
	const float invExtY = extY > 1e-12f ? 1.0f / extY : 0.0f;
	const float invExtZ = extZ > 1e-12f ? 1.0f / extZ : 0.0f;

	// Compute 30-bit Morton codes
	std::vector<uint32_t> mortonCodes(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		float nx = (positions[i].x - aabbMin.x) * invExtX;
		float ny = (positions[i].y - aabbMin.y) * invExtY;
		float nz = (positions[i].z - aabbMin.z) * invExtZ;

		nx = std::max(0.0f, std::min(nx, 1.0f));
		ny = std::max(0.0f, std::min(ny, 1.0f));
		nz = std::max(0.0f, std::min(nz, 1.0f));

		const uint32_t ix = std::min(static_cast<uint32_t>(nx * 1023.0f), 1023u);
		const uint32_t iy = std::min(static_cast<uint32_t>(ny * 1023.0f), 1023u);
		const uint32_t iz = std::min(static_cast<uint32_t>(nz * 1023.0f), 1023u);

		mortonCodes[i] = Morton3D(ix, iy, iz);
	}

	// Sort permutation by Morton code
	std::sort(permutation.begin(), permutation.end(),
		[&mortonCodes](uint32_t a, uint32_t b)
		{
			return mortonCodes[a] < mortonCodes[b];
		});

	return permutation;
}

// Public API: MergeGroupsSpatial
std::vector<std::vector<uint32_t>> MergeGroupsSpatial(
	const std::vector<uint32_t>& sortedGroupIndices,
	uint32_t maxFanout)
{
	std::vector<std::vector<uint32_t>> batches;

	if (sortedGroupIndices.empty() || maxFanout == 0)
		return batches;

	maxFanout = std::max(1u, maxFanout);

	std::vector<uint32_t> currentBatch;
	currentBatch.reserve(maxFanout);

	for (uint32_t idx : sortedGroupIndices)
	{
		currentBatch.push_back(idx);
		if (static_cast<uint32_t>(currentBatch.size()) >= maxFanout)
		{
			batches.push_back(std::move(currentBatch));
			currentBatch = {};
			currentBatch.reserve(maxFanout);
		}
	}

	if (!currentBatch.empty())
	{
		batches.push_back(std::move(currentBatch));
	}

	return batches;
}
