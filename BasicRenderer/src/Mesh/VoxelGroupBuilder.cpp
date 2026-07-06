#include "Mesh/VoxelGroupBuilder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <embree4/rtcore.h>
#include <spdlog/spdlog.h>

#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Mesh/SGGX.h"
#include "Mesh/VertexLayout.h"

namespace
{
	// Helpers

	using br::mesh::sggx::CompressSGGXToAxial;
	using br::mesh::sggx::DecodeAxialSGGX;
	using br::mesh::sggx::DecodeOctahedralAxis;
	using br::mesh::sggx::EncodeAxialSGGX;
	using br::mesh::sggx::Float3;
	using br::mesh::sggx::SGGXFromNormal;
	using br::mesh::sggx::SymmetricMatrix3;
	using br::mesh::sggx::BuildSGGXFromNormals;

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

	Float3 ToFloat3(const DirectX::XMFLOAT3& v) { return { v.x, v.y, v.z }; }
	DirectX::XMFLOAT3 ToXM(const Float3& v) { return { v.x, v.y, v.z }; }
	Float3 MinFloat3(const Float3& a, const Float3& b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
	Float3 MaxFloat3(const Float3& a, const Float3& b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }
	Float3 AxisExtent(const Float3& minValue, const Float3& maxValue) { return maxValue - minValue; }
	float AxisValue(const Float3& v, uint32_t axis)
	{
		switch (axis)
		{
		case 0: return v.x;
		case 1: return v.y;
		default: return v.z;
		}
	}
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

	struct TriangleBounds
	{
		Float3 boundsMin{};
		Float3 boundsMax{};
		Float3 centroid{};
	};

	TriangleBounds ComputeTriangleBounds(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& triangleIndices,
		uint32_t triangleIndex)
	{
		const size_t triangleBase = static_cast<size_t>(triangleIndex) * 3u;
		const uint32_t i0 = triangleIndices[triangleBase + 0u];
		const uint32_t i1 = triangleIndices[triangleBase + 1u];
		const uint32_t i2 = triangleIndices[triangleBase + 2u];
		const Float3 v0 = ReadPosition(vertices, vertexStrideBytes, i0);
		const Float3 v1 = ReadPosition(vertices, vertexStrideBytes, i1);
		const Float3 v2 = ReadPosition(vertices, vertexStrideBytes, i2);
		TriangleBounds bounds{};
		bounds.boundsMin = MinFloat3(v0, MinFloat3(v1, v2));
		bounds.boundsMax = MaxFloat3(v0, MaxFloat3(v1, v2));
		bounds.centroid = (v0 + v1 + v2) * (1.0f / 3.0f);
		return bounds;
	}

	bool AABBOverlap(const Float3& aMin, const Float3& aMax, const Float3& bMin, const Float3& bMax)
	{
		return aMin.x <= bMax.x && aMax.x >= bMin.x &&
			aMin.y <= bMax.y && aMax.y >= bMin.y &&
			aMin.z <= bMax.z && aMax.z >= bMin.z;
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

	uint32_t ComputeDominantBoneIndexForCell(
		const VoxelizeTrianglesInput& input,
		const std::vector<uint32_t>& overlappingTriangleIndices)
	{
		if (input.skinningVertices == nullptr || input.skinningVertices->empty() ||
			input.skinningVertexStrideBytes == 0 || input.triangleIndices == nullptr)
		{
			return CLOD_VOXEL_STATIC_BONE_INDEX;
		}

		std::unordered_map<uint32_t, float> boneWeights;
		boneWeights.reserve(8);

		for (uint32_t triangleIndex : overlappingTriangleIndices)
		{
			const size_t triangleBase = static_cast<size_t>(triangleIndex) * 3u;
			if (triangleBase + 2u >= input.triangleIndices->size())
			{
				continue;
			}

			const uint32_t vertexIndices[3] = {
				(*input.triangleIndices)[triangleBase + 0u],
				(*input.triangleIndices)[triangleBase + 1u],
				(*input.triangleIndices)[triangleBase + 2u]
			};

			for (uint32_t vertexIndex : vertexIndices)
			{
				PackedSkinningInfluences influences{};
				if (!ReadSkinningInfluences(*input.skinningVertices, input.skinningVertexStrideBytes, vertexIndex, influences))
				{
					continue;
				}

				AccumulateInfluenceSet(influences.joints0, influences.weights0, boneWeights);
				AccumulateInfluenceSet(influences.joints1, influences.weights1, boneWeights);
			}
		}

		return SelectDominantBoneIndex(boneWeights);
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

	// Möller-Trumbore ray-triangle intersection
	bool RayTriangleIntersect(const Float3& origin, const Float3& dir,
		const Float3& v0, const Float3& v1, const Float3& v2,
		float tMax,
		float& outT,
		float* outU = nullptr,
		float* outV = nullptr)
	{
		constexpr float kEpsilon = 1e-8f;
		Float3 e1 = v1 - v0;
		Float3 e2 = v2 - v0;
		Float3 h = dir.cross(e2);
		float a = e1.dot(h);
		if (std::abs(a) < kEpsilon)
			return false;

		float f = 1.0f / a;
		Float3 s = origin - v0;
		float u = f * s.dot(h);
		if (u < 0.0f || u > 1.0f)
			return false;

		Float3 q = s.cross(e1);
		float v = f * dir.dot(q);
		if (v < 0.0f || u + v > 1.0f)
			return false;

		float t = f * e2.dot(q);
		if (t > kEpsilon && t < tMax)
		{
			outT = t;
			if (outU != nullptr)
			{
				*outU = u;
			}
			if (outV != nullptr)
			{
				*outV = v;
			}
			return true;
		}

		return false;
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

	// Rasterize triangles into a grid, returning per-cell triangle index lists.
	struct CellTriEntry
	{
		uint64_t cellKey = 0;
		std::vector<uint32_t> triangleIndices; // indices into the flat triangle array (triangle index, not vertex index)
	};

	struct VoxelSourceCellRef
	{
		uint32_t payloadIndex = 0;
		uint32_t cellIndex = 0;
		Float3 aabbMin{};
		Float3 aabbMax{};
		int32_t refinedGroup = -1;
	};

	void AddUniqueRefinedGroup(std::vector<int32_t>& refinedGroups, int32_t refinedGroup)
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

	Float3 VoxelCellMin(const VoxelGroupPayload& payload, const VoxelCell& cell)
	{
		return {
			payload.aabbMin.x + static_cast<float>(cell.x) * payload.voxelWidth,
			payload.aabbMin.y + static_cast<float>(cell.y) * payload.voxelWidth,
			payload.aabbMin.z + static_cast<float>(cell.z) * payload.voxelWidth
		};
	}

	Float3 VoxelCellMax(const VoxelGroupPayload& payload, const VoxelCell& cell)
	{
		const Float3 cellMin = VoxelCellMin(payload, cell);
		return cellMin + Float3(payload.voxelWidth, payload.voxelWidth, payload.voxelWidth);
	}

	Float3 TransformPoint3x4(const ClusterLODAssemblyTransform& transform, const Float3& point)
	{
		return {
			transform.row0.x * point.x + transform.row0.y * point.y + transform.row0.z * point.z + transform.row0.w,
			transform.row1.x * point.x + transform.row1.y * point.y + transform.row1.z * point.z + transform.row1.w,
			transform.row2.x * point.x + transform.row2.y * point.y + transform.row2.z * point.z + transform.row2.w
		};
	}

	void TransformAabb3x4(
		const ClusterLODAssemblyTransform& transform,
		const Float3& localMin,
		const Float3& localMax,
		Float3& outMin,
		Float3& outMax)
	{
		outMin = Float3(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		outMax = Float3(
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());

		for (uint32_t corner = 0; corner < 8u; ++corner)
		{
			const Float3 p(
				(corner & 1u) != 0u ? localMax.x : localMin.x,
				(corner & 2u) != 0u ? localMax.y : localMin.y,
				(corner & 4u) != 0u ? localMax.z : localMin.z);
			const Float3 tp = TransformPoint3x4(transform, p);
			outMin.x = std::min(outMin.x, tp.x);
			outMin.y = std::min(outMin.y, tp.y);
			outMin.z = std::min(outMin.z, tp.z);
			outMax.x = std::max(outMax.x, tp.x);
			outMax.y = std::max(outMax.y, tp.y);
			outMax.z = std::max(outMax.z, tp.z);
		}
	}

	int32_t ResolveInstanceRefinedGroup(const VoxelSourcePayloadInstance& instance, const VoxelCell& sourceCell)
	{
		return instance.refinedGroupOverride != std::numeric_limits<int32_t>::min()
			? instance.refinedGroupOverride
			: sourceCell.refinedGroup;
	}

	std::unordered_map<uint64_t, std::vector<uint32_t>> RasterizeTrianglesToGrid(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& triangleIndices,
		const Float3& aabbMin,
		float voxelWidth,
		uint32_t resolution)
	{
		const uint32_t triangleCount = static_cast<uint32_t>(triangleIndices.size() / 3);
		std::unordered_map<uint64_t, std::vector<uint32_t>> cellTriMap;
		if (triangleCount == 0 || resolution == 0)
			return cellTriMap;

		const Float3 cellSize = {
			voxelWidth,
			voxelWidth,
			voxelWidth
		};

		if (cellSize.x <= 0.0f || cellSize.y <= 0.0f || cellSize.z <= 0.0f)
			return cellTriMap;

		const Float3 invCellSize = {
			1.0f / cellSize.x,
			1.0f / cellSize.y,
			1.0f / cellSize.z
		};
		const Float3 halfCell = cellSize * 0.5f;

		std::vector<std::vector<uint64_t>> perTriangleCells(triangleCount);

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

			std::vector<uint64_t>& triangleCells = perTriangleCells[triIdx];
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
		for (const std::vector<uint64_t>& triangleCells : perTriangleCells)
		{
			totalCellReferences += triangleCells.size();
		}

		cellTriMap.reserve(std::max<size_t>(triangleCount * 2u, totalCellReferences));
		for (uint32_t triIdx = 0; triIdx < triangleCount; ++triIdx)
		{
			for (uint64_t cellKey : perTriangleCells[triIdx])
			{
				cellTriMap[cellKey].push_back(triIdx);
			}
		}

		return cellTriMap;
	}

	std::unordered_map<uint64_t, std::vector<VoxelSourceCellRef>> RasterizeVoxelPayloadsToGrid(
		const std::vector<VoxelSourcePayloadInstance>& sourceVoxelPayloads,
		const Float3& aabbMin,
		float voxelWidth,
		uint32_t resolution)
	{
		std::unordered_map<uint64_t, std::vector<VoxelSourceCellRef>> cellVoxelMap;
		if (resolution == 0u || voxelWidth <= 0.0f)
		{
			return cellVoxelMap;
		}

		const float invCellSize = 1.0f / voxelWidth;
		for (uint32_t payloadIndex = 0; payloadIndex < static_cast<uint32_t>(sourceVoxelPayloads.size()); ++payloadIndex)
		{
			const VoxelSourcePayloadInstance& payloadInstance = sourceVoxelPayloads[payloadIndex];
			const VoxelGroupPayload* payload = payloadInstance.payload;
			if (payload == nullptr || payload->voxelWidth <= 0.0f)
			{
				continue;
			}

			for (uint32_t cellIndex = 0; cellIndex < static_cast<uint32_t>(payload->activeCells.size()); ++cellIndex)
			{
				const VoxelCell& sourceCell = payload->activeCells[cellIndex];
				Float3 sourceMin{};
				Float3 sourceMax{};
				TransformAabb3x4(
					payloadInstance.localToTarget,
					VoxelCellMin(*payload, sourceCell),
					VoxelCellMax(*payload, sourceCell),
					sourceMin,
					sourceMax);
				const uint32_t cxMin = ToCellCoord(sourceMin.x, aabbMin.x, invCellSize, resolution);
				const uint32_t cyMin = ToCellCoord(sourceMin.y, aabbMin.y, invCellSize, resolution);
				const uint32_t czMin = ToCellCoord(sourceMin.z, aabbMin.z, invCellSize, resolution);
				const uint32_t cxMax = ToCellCoord(std::nextafter(sourceMax.x, -std::numeric_limits<float>::infinity()), aabbMin.x, invCellSize, resolution);
				const uint32_t cyMax = ToCellCoord(std::nextafter(sourceMax.y, -std::numeric_limits<float>::infinity()), aabbMin.y, invCellSize, resolution);
				const uint32_t czMax = ToCellCoord(std::nextafter(sourceMax.z, -std::numeric_limits<float>::infinity()), aabbMin.z, invCellSize, resolution);

				for (uint32_t cz = czMin; cz <= czMax; ++cz)
				{
					for (uint32_t cy = cyMin; cy <= cyMax; ++cy)
					{
						for (uint32_t cx = cxMin; cx <= cxMax; ++cx)
						{
							cellVoxelMap[PackCell(cx, cy, cz)].push_back(VoxelSourceCellRef{
								payloadIndex,
								cellIndex,
								sourceMin,
								sourceMax,
								ResolveInstanceRefinedGroup(payloadInstance, sourceCell) });
						}
					}
				}
			}
		}

		return cellVoxelMap;
	}

	std::unordered_map<uint64_t, std::vector<int32_t>> RasterizeVoxelCandidatePayloadsToGrid(
		const std::vector<VoxelSourcePayloadInstance>& sourceVoxelPayloads,
		const Float3& aabbMin,
		float voxelWidth,
		uint32_t resolution)
	{
		std::unordered_map<uint64_t, std::vector<int32_t>> candidateCells;
		if (resolution == 0u || voxelWidth <= 0.0f)
		{
			return candidateCells;
		}

		const float invCellSize = 1.0f / voxelWidth;
		for (const VoxelSourcePayloadInstance& candidatePayload : sourceVoxelPayloads)
		{
			const VoxelGroupPayload* payload = candidatePayload.payload;
			if (payload == nullptr || payload->voxelWidth <= 0.0f)
			{
				continue;
			}

			const float expansionRadius = std::max(0.0f, candidatePayload.expansionRadius);
			for (const VoxelCell& sourceCell : payload->activeCells)
			{
				Float3 sourceMin{};
				Float3 sourceMax{};
				TransformAabb3x4(
					candidatePayload.localToTarget,
					VoxelCellMin(*payload, sourceCell),
					VoxelCellMax(*payload, sourceCell),
					sourceMin,
					sourceMax);
				sourceMin = sourceMin - Float3(expansionRadius, expansionRadius, expansionRadius);
				sourceMax = sourceMax + Float3(expansionRadius, expansionRadius, expansionRadius);
				const uint32_t cxMin = ToCellCoord(sourceMin.x, aabbMin.x, invCellSize, resolution);
				const uint32_t cyMin = ToCellCoord(sourceMin.y, aabbMin.y, invCellSize, resolution);
				const uint32_t czMin = ToCellCoord(sourceMin.z, aabbMin.z, invCellSize, resolution);
				const uint32_t cxMax = ToCellCoord(std::nextafter(sourceMax.x, -std::numeric_limits<float>::infinity()), aabbMin.x, invCellSize, resolution);
				const uint32_t cyMax = ToCellCoord(std::nextafter(sourceMax.y, -std::numeric_limits<float>::infinity()), aabbMin.y, invCellSize, resolution);
				const uint32_t czMax = ToCellCoord(std::nextafter(sourceMax.z, -std::numeric_limits<float>::infinity()), aabbMin.z, invCellSize, resolution);

				for (uint32_t cz = czMin; cz <= czMax; ++cz)
				{
					for (uint32_t cy = cyMin; cy <= cyMax; ++cy)
					{
						for (uint32_t cx = cxMin; cx <= cxMax; ++cx)
						{
							AddUniqueRefinedGroup(
								candidateCells[PackCell(cx, cy, cz)],
								ResolveInstanceRefinedGroup(candidatePayload, sourceCell));
						}
					}
				}
			}
		}

		return candidateCells;
	}

	bool RayAABBIntersect(const Float3& origin, const Float3& dir, const Float3& boxMin, const Float3& boxMax, float tMax, float& outT)
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

		outT = tMin;
		return tFar >= 0.0f && tMin < tMax;
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
		float coverage = 0.0f;
		uint32_t hitCount = 0;
		uint32_t representativeTriangleIndex = std::numeric_limits<uint32_t>::max();
		Float3 accumulatedNormal{};
		SymmetricMatrix3 accumulatedSGGX{};
		float sggxWeight = 0.0f;
		DirectX::XMFLOAT2 accumulatedUv{};
		float uvWeight = 0.0f;
		std::vector<DirectX::XMFLOAT2> uvSamples;
		std::vector<Float3> normalSamples;
	};

	void AddCoverageUvSample(CellCoverageSample& sample, const DirectX::XMFLOAT2& uv, float weight)
	{
		if (weight <= 0.0f || !std::isfinite(uv.x) || !std::isfinite(uv.y))
		{
			return;
		}

		sample.accumulatedUv.x += uv.x * weight;
		sample.accumulatedUv.y += uv.y * weight;
		sample.uvWeight += weight;
		sample.uvSamples.push_back(uv);
	}

	DirectX::XMFLOAT2 SelectRepresentativeUv(const CellCoverageSample& sample)
	{
		if (sample.uvWeight <= 0.0f)
		{
			return DirectX::XMFLOAT2(0.0f, 0.0f);
		}

		const DirectX::XMFLOAT2 mean(
			sample.accumulatedUv.x / sample.uvWeight,
			sample.accumulatedUv.y / sample.uvWeight);
		if (sample.uvSamples.empty())
		{
			return mean;
		}

		DirectX::XMFLOAT2 representative = sample.uvSamples.front();
		float bestDistanceSq = std::numeric_limits<float>::max();
		for (const DirectX::XMFLOAT2& uv : sample.uvSamples)
		{
			const float dx = uv.x - mean.x;
			const float dy = uv.y - mean.y;
			const float distanceSq = dx * dx + dy * dy;
			if (distanceSq < bestDistanceSq)
			{
				bestDistanceSq = distanceSq;
				representative = uv;
			}
		}

		return representative;
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

	// Per-cell coverage sampling via ray tracing against triangles.
	CellCoverageSample SampleCellCoverageTriangles(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& meshTriangleIndices,
		const std::vector<uint32_t>& cellTriangleIndices,
		const Float3& cellWorldMin,
		const Float3& cellWorldMax,
		const std::vector<Ray>& rays,
		bool doubleSidedTriangles,
		const VoxelCoverageMaterialSampler* materialSampler)
	{
		CellCoverageSample sample{};
		if (rays.empty() || cellTriangleIndices.empty())
			return sample;

		const Float3 cellExtent = cellWorldMax - cellWorldMin;

		for (const Ray& ray : rays)
		{
			Float3 origin, dir;
			float tMax;
			MapRayToWorldCell(ray, cellWorldMin, cellExtent, origin, dir, tMax);
			if (tMax < 1e-12f)
				continue;

			uint32_t nearestTriangleIndex = std::numeric_limits<uint32_t>::max();
			float nearestT = tMax;
			Float3 nearestNormal{};
			DirectX::XMFLOAT2 nearestUv{};
			float nearestWeight = 1.0f;
			for (uint32_t triLocalIdx : cellTriangleIndices)
			{
				const uint32_t i0 = meshTriangleIndices[triLocalIdx * 3 + 0];
				const uint32_t i1 = meshTriangleIndices[triLocalIdx * 3 + 1];
				const uint32_t i2 = meshTriangleIndices[triLocalIdx * 3 + 2];

				const Float3 v0 = ReadPosition(vertices, vertexStrideBytes, i0);
				const Float3 v1 = ReadPosition(vertices, vertexStrideBytes, i1);
				const Float3 v2 = ReadPosition(vertices, vertexStrideBytes, i2);

				float hitT = 0.0f;
				float hitU = 0.0f;
				float hitV = 0.0f;
				if (RayTriangleIntersect(origin, dir, v0, v1, v2, nearestT, hitT, &hitU, &hitV))
				{
					const Float3 n0 = ReadNormal(vertices, vertexStrideBytes, i0);
					const Float3 n1 = ReadNormal(vertices, vertexStrideBytes, i1);
					const Float3 n2 = ReadNormal(vertices, vertexStrideBytes, i2);
					const float hitW = 1.0f - hitU - hitV;
					const Float3 interpolatedNormal = n0 * hitW + n1 * hitU + n2 * hitV;
					Float3 candidateNormal = interpolatedNormal.lengthSq() > 1.0e-20f
						? interpolatedNormal.normalized()
						: TriangleNormal(v0, v1, v2);
					candidateNormal = OrientHitNormalForSidedness(candidateNormal, TriangleNormal(v0, v1, v2), dir, doubleSidedTriangles);
					const DirectX::XMFLOAT2 uv0 = ReadTexcoord(vertices, vertexStrideBytes, i0);
					const DirectX::XMFLOAT2 uv1 = ReadTexcoord(vertices, vertexStrideBytes, i1);
					const DirectX::XMFLOAT2 uv2 = ReadTexcoord(vertices, vertexStrideBytes, i2);
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
						.position = ToXM(origin + dir * hitT),
						.normal = ToXM(candidateNormal),
						.uv = candidateUv,
						.trianglePositions = { ToXM(v0), ToXM(v1), ToXM(v2) },
						.triangleUvs = { uv0, uv1, uv2 },
						.barycentrics = { hitW, hitU, hitV },
					};
					if (!ApplyCoverageMaterialSample(materialSampler, materialHit, candidateNormal, candidateWeight))
					{
						continue;
					}

					nearestT = hitT;
					nearestTriangleIndex = triLocalIdx;
					nearestNormal = candidateNormal;
					nearestUv = candidateUv;
					nearestWeight = candidateWeight;
				}
			}

			if (nearestTriangleIndex != std::numeric_limits<uint32_t>::max())
			{
				++sample.hitCount;
				if (sample.representativeTriangleIndex == std::numeric_limits<uint32_t>::max())
				{
					sample.representativeTriangleIndex = nearestTriangleIndex;
				}
				sample.accumulatedNormal = sample.accumulatedNormal + nearestNormal;
				sample.normalSamples.push_back(nearestNormal);
				AddCoverageUvSample(sample, nearestUv, nearestWeight);
			}
		}

		sample.coverage = static_cast<float>(sample.hitCount) / static_cast<float>(rays.size());
		if (!sample.normalSamples.empty())
		{
			sample.accumulatedSGGX = BuildSGGXFromNormals(sample.normalSamples);
			sample.sggxWeight = 1.0f;
		}
		return sample;
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

		const std::vector<std::byte>* vertices = sourceTriangles.Vertices();
		const std::vector<uint32_t>* meshTriangleIndices = sourceTriangles.TriangleIndices();
		const std::vector<int32_t>* triangleRefinedGroupIds = sourceTriangles.TriangleRefinedGroupIds();
		const bool doubleSidedTriangles = sourceTriangles.DoubleSidedTriangles();
		if (vertices == nullptr || meshTriangleIndices == nullptr)
		{
			return sample;
		}

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
				const size_t triangleBase = static_cast<size_t>(triLocalIdx) * 3u;
				if (triangleBase + 2u >= meshTriangleIndices->size())
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

				const uint32_t i0 = (*meshTriangleIndices)[triangleBase + 0u];
				const uint32_t i1 = (*meshTriangleIndices)[triangleBase + 1u];
				const uint32_t i2 = (*meshTriangleIndices)[triangleBase + 2u];
				const Float3 v0 = ReadPosition(*vertices, sourceTriangles.VertexStrideBytes(), i0);
				const Float3 v1 = ReadPosition(*vertices, sourceTriangles.VertexStrideBytes(), i1);
				const Float3 v2 = ReadPosition(*vertices, sourceTriangles.VertexStrideBytes(), i2);
				const Float3 n0 = ReadNormal(*vertices, sourceTriangles.VertexStrideBytes(), i0);
				const Float3 n1 = ReadNormal(*vertices, sourceTriangles.VertexStrideBytes(), i1);
				const Float3 n2 = ReadNormal(*vertices, sourceTriangles.VertexStrideBytes(), i2);
				const Float3 faceNormal = TriangleNormal(v0, v1, v2);
				const float hitW = 1.0f - hitU - hitV;
				const Float3 interpolatedNormal = n0 * hitW + n1 * hitU + n2 * hitV;
				Float3 candidateNormal = interpolatedNormal.lengthSq() > 1.0e-20f
					? interpolatedNormal.normalized()
					: faceNormal;
				candidateNormal = OrientHitNormalForSidedness(candidateNormal, faceNormal, dir, doubleSidedTriangles);
				const DirectX::XMFLOAT2 uv0 = ReadTexcoord(*vertices, sourceTriangles.VertexStrideBytes(), i0);
				const DirectX::XMFLOAT2 uv1 = ReadTexcoord(*vertices, sourceTriangles.VertexStrideBytes(), i1);
				const DirectX::XMFLOAT2 uv2 = ReadTexcoord(*vertices, sourceTriangles.VertexStrideBytes(), i2);
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
				nearestNormal = candidateNormal;
				nearestUv = candidateUv;
				nearestWeight = candidateWeight;
				break;
			}

			if (nearestTriangleIndex != std::numeric_limits<uint32_t>::max())
			{
				++sample.hitCount;
				if (sample.representativeTriangleIndex == std::numeric_limits<uint32_t>::max())
				{
					sample.representativeTriangleIndex = nearestTriangleIndex;
				}
				sample.accumulatedNormal = sample.accumulatedNormal + nearestNormal;
				sample.normalSamples.push_back(nearestNormal);
				AddCoverageUvSample(sample, nearestUv, nearestWeight);
			}
		}

		sample.coverage = static_cast<float>(sample.hitCount) / static_cast<float>(rays.size());
		if (!sample.normalSamples.empty())
		{
			sample.accumulatedSGGX = BuildSGGXFromNormals(sample.normalSamples);
			sample.sggxWeight = 1.0f;
		}
		return sample;
	}

	CellCoverageSample SampleCellCoverageVoxels(
		const std::vector<VoxelSourcePayloadInstance>& sourceVoxelPayloads,
		const std::vector<VoxelSourceCellRef>& cellVoxelRefs,
		const Float3& cellWorldMin,
		const Float3& cellWorldMax,
		uint32_t& outDominantBoneIndex)
	{
		CellCoverageSample sample{};
		outDominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		if (cellVoxelRefs.empty())
		{
			return sample;
		}

		const Float3 cellExtent = cellWorldMax - cellWorldMin;
		const float cellVolume = cellExtent.x * cellExtent.y * cellExtent.z;
		if (cellVolume <= 1.0e-20f)
		{
			return sample;
		}

		std::unordered_map<uint32_t, float> boneWeights;
		boneWeights.reserve(8);

		for (const VoxelSourceCellRef& cellRef : cellVoxelRefs)
		{
			if (cellRef.payloadIndex >= sourceVoxelPayloads.size())
			{
				continue;
			}
			const VoxelGroupPayload* payload = sourceVoxelPayloads[cellRef.payloadIndex].payload;
			if (payload == nullptr || cellRef.cellIndex >= payload->activeCells.size())
			{
				continue;
			}

			const VoxelCell& sourceCell = payload->activeCells[cellRef.cellIndex];
			const Float3 sourceMin = cellRef.aabbMin;
			const Float3 sourceMax = cellRef.aabbMax;
			const float overlapX = std::max(0.0f, std::min(cellWorldMax.x, sourceMax.x) - std::max(cellWorldMin.x, sourceMin.x));
			const float overlapY = std::max(0.0f, std::min(cellWorldMax.y, sourceMax.y) - std::max(cellWorldMin.y, sourceMin.y));
			const float overlapZ = std::max(0.0f, std::min(cellWorldMax.z, sourceMax.z) - std::max(cellWorldMin.z, sourceMin.z));
			const float weightedCoverage = (overlapX * overlapY * overlapZ / cellVolume) * std::clamp(sourceCell.opacity, 0.0f, 1.0f);
			if (weightedCoverage <= 0.0f)
			{
				continue;
			}

			sample.coverage += weightedCoverage;
			sample.accumulatedNormal = sample.accumulatedNormal + DecodeOctahedralAxis(sourceCell.sggxAxisAndSigmas.x, sourceCell.sggxAxisAndSigmas.y) * weightedCoverage;
			sample.accumulatedSGGX = sample.accumulatedSGGX + DecodeAxialSGGX(sourceCell.sggxAxisAndSigmas) * weightedCoverage;
			sample.sggxWeight += weightedCoverage;
			AddCoverageUvSample(sample, sourceCell.uv, weightedCoverage);
			if (sourceCell.dominantBoneIndex != CLOD_VOXEL_STATIC_BONE_INDEX)
			{
				boneWeights[sourceCell.dominantBoneIndex] += weightedCoverage;
			}
		}

		sample.coverage = std::clamp(sample.coverage, 0.0f, 1.0f);
		sample.hitCount = sample.coverage > 0.0f ? 1u : 0u;
		if (!boneWeights.empty())
		{
			outDominantBoneIndex = SelectDominantBoneIndex(boneWeights);
		}
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

		auto appendPrunedSection = [&keptCells](std::vector<VoxelCell>::iterator begin, std::vector<VoxelCell>::iterator end)
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
			if (targetCellCount < sectionCells.size())
			{
				std::sort(sectionCells.begin(), sectionCells.end(), [](const VoxelCell& lhs, const VoxelCell& rhs) {
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
				sectionCells.resize(targetCellCount);
			}

			keptCells.insert(keptCells.end(), sectionCells.begin(), sectionCells.end());
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

	uint32_t PruneCellsBySpatialCoverage(std::vector<VoxelCell>& cells)
	{
		if (cells.empty())
		{
			return 0u;
		}

		const size_t originalCount = cells.size();
		cells.erase(std::remove_if(cells.begin(), cells.end(), [](const VoxelCell& cell) {
			const float coverage = std::clamp(cell.opacity, 0.0f, 1.0f);
			if (coverage >= 1.0f)
			{
				return false;
			}
			if (coverage <= 0.0f)
			{
				return true;
			}

			uint32_t hash = 2166136261u;
			auto mix = [&hash](uint32_t value)
			{
				hash ^= value;
				hash *= 16777619u;
			};
			mix(cell.x);
			mix(cell.y);
			mix(cell.z);
			mix(static_cast<uint32_t>(cell.refinedGroup));
			hash ^= hash >> 16u;
			hash *= 0x7feb352du;
			hash ^= hash >> 15u;
			hash *= 0x846ca68bu;
			hash ^= hash >> 16u;

			const float stochasticCoverage = static_cast<float>(hash >> 8u) * (1.0f / 16777216.0f);
			return stochasticCoverage >= coverage;
		}), cells.end());

		std::sort(cells.begin(), cells.end(), [](const VoxelCell& lhs, const VoxelCell& rhs) {
			const uint64_t lhsCell = PackCell(lhs.x, lhs.y, lhs.z);
			const uint64_t rhsCell = PackCell(rhs.x, rhs.y, rhs.z);
			return lhsCell == rhsCell ? lhs.refinedGroup < rhs.refinedGroup : lhsCell < rhsCell;
		});

		return static_cast<uint32_t>(std::min<size_t>(originalCount - cells.size(), std::numeric_limits<uint32_t>::max()));
	}

	uint32_t PruneCellsByCoverage(std::vector<VoxelCell>& cells, ClusterLODVoxelPruningMode pruningMode)
	{
		switch (pruningMode)
		{
		case ClusterLODVoxelPruningMode::None:
			return 0u;
		case ClusterLODVoxelPruningMode::Coverage:
			return PruneCellsByPureCoverage(cells);
		case ClusterLODVoxelPruningMode::Spatial:
		default:
			return PruneCellsBySpatialCoverage(cells);
		}
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

	std::vector<DirectX::XMFLOAT3> vertices;
	std::vector<RefinedGroupScene> refinedGroupScenes;

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
	}
};

namespace
{
	RTCDevice GetVoxelCoverageEmbreeDevice()
	{
		static RTCDevice device = rtcNewDevice(nullptr);
		return device;
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
	bool doubleSidedTriangles)
{
	m_vertices = vertices;
	m_vertexStrideBytes = vertexStrideBytes;
	m_skinningVertices = skinningVertices;
	m_skinningVertexStrideBytes = skinningVertexStrideBytes;
	m_triangleIndices = triangleIndices;
	m_triangleRefinedGroupIds = triangleRefinedGroupIds;
	m_doubleSidedTriangles = doubleSidedTriangles;
	m_triangleOrder.clear();
	m_nodes.clear();
	m_embreeScene.reset();

	if (vertices == nullptr || triangleIndices == nullptr || vertexStrideBytes < sizeof(float) * 3u || triangleIndices->size() < 3u || (triangleIndices->size() % 3u) != 0u)
	{
		return;
	}

	const uint32_t triangleCount = static_cast<uint32_t>(std::min<size_t>(triangleIndices->size() / 3u, std::numeric_limits<uint32_t>::max()));
	m_triangleOrder.resize(triangleCount);
	std::iota(m_triangleOrder.begin(), m_triangleOrder.end(), 0u);
	m_nodes.reserve(std::max<uint32_t>(1u, triangleCount * 2u));
	BuildNode(0u, triangleCount);

	RTCDevice device = GetVoxelCoverageEmbreeDevice();
	if (device == nullptr)
	{
		return;
	}

	std::unique_ptr<EmbreeScene> embreeScene = std::make_unique<EmbreeScene>();
	const size_t sourceVertexCount = vertices->size() / vertexStrideBytes;
	embreeScene->vertices.resize(sourceVertexCount);
	for (size_t vertexIndex = 0; vertexIndex < sourceVertexCount; ++vertexIndex)
	{
		embreeScene->vertices[vertexIndex] = ToXM(ReadPosition(*vertices, vertexStrideBytes, static_cast<uint32_t>(vertexIndex)));
	}

	std::unordered_map<int32_t, std::vector<uint32_t>> trianglesByRefinedGroup;
	trianglesByRefinedGroup.reserve(triangleCount);
	for (uint32_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
	{
		int32_t refinedGroup = -1;
		if (triangleRefinedGroupIds != nullptr && triangleIndex < triangleRefinedGroupIds->size())
		{
			refinedGroup = (*triangleRefinedGroupIds)[triangleIndex];
		}
		trianglesByRefinedGroup[refinedGroup].push_back(triangleIndex);
	}

	embreeScene->refinedGroupScenes.reserve(trianglesByRefinedGroup.size());
	for (auto& [refinedGroup, sourceTriangleIndices] : trianglesByRefinedGroup)
	{
		if (sourceTriangleIndices.empty())
		{
			continue;
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
	}

	if (!embreeScene->refinedGroupScenes.empty())
	{
		m_embreeScene = std::move(embreeScene);
	}
}

bool VoxelSourceTriangleBVH::IsValid() const
{
	return m_vertices != nullptr && m_triangleIndices != nullptr && !m_triangleOrder.empty() && !m_nodes.empty();
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
		if (candidateScene.refinedGroup == -1)
		{
			unfilteredScene = &candidateScene;
		}
		if (candidateScene.refinedGroup == refinedGroupFilter)
		{
			scene = &candidateScene;
			break;
		}
	}
	if (scene == nullptr)
	{
		scene = unfilteredScene;
	}
	if (scene == nullptr || scene->scene == nullptr)
	{
		return false;
	}

	RTCRayHit rayHit{};
	rayHit.ray.org_x = origin.x;
	rayHit.ray.org_y = origin.y;
	rayHit.ray.org_z = origin.z;
	rayHit.ray.dir_x = direction.x;
	rayHit.ray.dir_y = direction.y;
	rayHit.ray.dir_z = direction.z;
	rayHit.ray.tnear = std::max(0.0f, tMin);
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

	if (rayHit.hit.primID >= scene->sourceTriangleIndices.size())
	{
		return false;
	}

	outTriangleIndex = scene->sourceTriangleIndices[rayHit.hit.primID];
	outT = rayHit.ray.tfar;
	outU = rayHit.hit.u;
	outV = rayHit.hit.v;
	return true;
}

uint32_t VoxelSourceTriangleBVH::BuildNode(uint32_t firstTriangle, uint32_t triangleCount)
{
	const uint32_t nodeIndex = static_cast<uint32_t>(m_nodes.size());
	m_nodes.push_back(Node{});
	Node& node = m_nodes.back();
	node.firstTriangle = firstTriangle;
	node.triangleCount = triangleCount;

	Float3 boundsMin(
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max());
	Float3 boundsMax(
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max());
	Float3 centroidMin = boundsMin;
	Float3 centroidMax = boundsMax;

	for (uint32_t offset = 0; offset < triangleCount; ++offset)
	{
		const uint32_t triangleIndex = m_triangleOrder[firstTriangle + offset];
		const TriangleBounds triangleBounds = ComputeTriangleBounds(*m_vertices, m_vertexStrideBytes, *m_triangleIndices, triangleIndex);
		boundsMin = MinFloat3(boundsMin, triangleBounds.boundsMin);
		boundsMax = MaxFloat3(boundsMax, triangleBounds.boundsMax);
		centroidMin = MinFloat3(centroidMin, triangleBounds.centroid);
		centroidMax = MaxFloat3(centroidMax, triangleBounds.centroid);
	}

	node.boundsMin = ToXM(boundsMin);
	node.boundsMax = ToXM(boundsMax);

	constexpr uint32_t kLeafTriangleCount = 8u;
	if (triangleCount <= kLeafTriangleCount)
	{
		return nodeIndex;
	}

	const Float3 centroidExtent = AxisExtent(centroidMin, centroidMax);
	uint32_t splitAxis = 0u;
	if (centroidExtent.y > centroidExtent.x && centroidExtent.y >= centroidExtent.z)
	{
		splitAxis = 1u;
	}
	else if (centroidExtent.z > centroidExtent.x && centroidExtent.z > centroidExtent.y)
	{
		splitAxis = 2u;
	}

	const uint32_t mid = firstTriangle + triangleCount / 2u;
	auto begin = m_triangleOrder.begin() + firstTriangle;
	auto middle = m_triangleOrder.begin() + mid;
	auto end = m_triangleOrder.begin() + firstTriangle + triangleCount;
	std::nth_element(begin, middle, end, [&](uint32_t lhs, uint32_t rhs)
		{
			const TriangleBounds lhsBounds = ComputeTriangleBounds(*m_vertices, m_vertexStrideBytes, *m_triangleIndices, lhs);
			const TriangleBounds rhsBounds = ComputeTriangleBounds(*m_vertices, m_vertexStrideBytes, *m_triangleIndices, rhs);
			return AxisValue(lhsBounds.centroid, splitAxis) < AxisValue(rhsBounds.centroid, splitAxis);
		});

	const uint32_t leftCount = mid - firstTriangle;
	const uint32_t rightCount = triangleCount - leftCount;
	if (leftCount == 0u || rightCount == 0u)
	{
		return nodeIndex;
	}

	const uint32_t leftChild = BuildNode(firstTriangle, leftCount);
	const uint32_t rightChild = BuildNode(mid, rightCount);
	m_nodes[nodeIndex].leftChild = leftChild;
	m_nodes[nodeIndex].rightChild = rightChild;
	m_nodes[nodeIndex].triangleCount = 0u;
	return nodeIndex;
}

void VoxelSourceTriangleBVH::QueryAABB(
	const DirectX::XMFLOAT3& aabbMin,
	const DirectX::XMFLOAT3& aabbMax,
	std::vector<uint32_t>& outTriangleIndices) const
{
	outTriangleIndices.clear();
	if (!IsValid())
	{
		return;
	}

	const Float3 queryMin = ToFloat3(aabbMin);
	const Float3 queryMax = ToFloat3(aabbMax);
	std::vector<uint32_t> stack;
	stack.push_back(0u);
	while (!stack.empty())
	{
		const uint32_t nodeIndex = stack.back();
		stack.pop_back();
		if (nodeIndex >= m_nodes.size())
		{
			continue;
		}

		const Node& node = m_nodes[nodeIndex];
		if (!AABBOverlap(ToFloat3(node.boundsMin), ToFloat3(node.boundsMax), queryMin, queryMax))
		{
			continue;
		}

		if (node.triangleCount > 0u)
		{
			for (uint32_t offset = 0; offset < node.triangleCount; ++offset)
			{
				const uint32_t triangleIndex = m_triangleOrder[node.firstTriangle + offset];
				const TriangleBounds triangleBounds = ComputeTriangleBounds(*m_vertices, m_vertexStrideBytes, *m_triangleIndices, triangleIndex);
				if (AABBOverlap(triangleBounds.boundsMin, triangleBounds.boundsMax, queryMin, queryMax))
				{
					outTriangleIndices.push_back(triangleIndex);
				}
			}
			continue;
		}

		if (node.leftChild != UINT32_MAX)
		{
			stack.push_back(node.leftChild);
		}
		if (node.rightChild != UINT32_MAX)
		{
			stack.push_back(node.rightChild);
		}
	}
}

// Public API: VoxelizeTriangles
VoxelizeTrianglesResult VoxelizeTrianglesDetailed(const VoxelizeTrianglesInput& input)
{
	VoxelizeTrianglesResult detailedResult{};
	VoxelGroupPayload& result = detailedResult.sourcePayload;

	const bool hasTriangleSources = input.vertices != nullptr && input.vertexStrideBytes >= sizeof(float) * 3 &&
		input.triangleIndices != nullptr && !input.triangleIndices->empty() && (input.triangleIndices->size() % 3) == 0;
	const bool hasVoxelSources = HasVoxelSources(input);
	const bool hasCandidateVoxelSources = HasCandidateVoxelSources(input);
	const bool hasCoverageSourceTriangles = input.coverageSourceTriangles != nullptr && input.coverageSourceTriangles->IsValid();

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

	std::unordered_map<uint64_t, std::vector<uint32_t>> cellTriMap;
	if (hasTriangleSources)
	{
		cellTriMap = RasterizeTrianglesToGrid(
			*input.vertices, input.vertexStrideBytes,
			*input.triangleIndices,
			aabbMin, input.voxelWidth,
			input.resolution);
	}

	std::vector<VoxelSourcePayloadInstance> sourceVoxelPayloads;
	std::unordered_map<uint64_t, std::vector<VoxelSourceCellRef>> cellVoxelMap;
	if (hasVoxelSources)
	{
		if (input.sourceVoxelPayloadInstances != nullptr)
		{
			sourceVoxelPayloads = *input.sourceVoxelPayloadInstances;
		}
		else if (input.sourceVoxelPayloads != nullptr)
		{
			sourceVoxelPayloads.reserve(input.sourceVoxelPayloads->size());
			for (const VoxelGroupPayload* payload : *input.sourceVoxelPayloads)
			{
				sourceVoxelPayloads.push_back(VoxelSourcePayloadInstance{ .payload = payload });
			}
		}
		cellVoxelMap = RasterizeVoxelPayloadsToGrid(sourceVoxelPayloads, aabbMin, input.voxelWidth, input.resolution);
	}

	std::unordered_map<uint64_t, std::vector<int32_t>> candidateVoxelMap;
	if (hasCandidateVoxelSources)
	{
		std::vector<VoxelSourcePayloadInstance> candidateVoxelPayloads;
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
		candidateVoxelMap = RasterizeVoxelCandidatePayloadsToGrid(
			candidateVoxelPayloads,
			aabbMin,
			input.voxelWidth,
			input.resolution);
	}
	detailedResult.triangleCandidateCellCount = static_cast<uint32_t>(std::min<size_t>(cellTriMap.size(), std::numeric_limits<uint32_t>::max()));
	detailedResult.voxelCandidateCellCount = static_cast<uint32_t>(std::min<size_t>(candidateVoxelMap.size() + cellVoxelMap.size(), std::numeric_limits<uint32_t>::max()));

	if (cellTriMap.empty() && cellVoxelMap.empty() && candidateVoxelMap.empty())
		return detailedResult;

	const uint32_t baseRaySeed = input.resolution * 2654435761u;

	const Float3 cellSize = {
		input.voxelWidth,
		input.voxelWidth,
		input.voxelWidth
	};

	std::vector<uint64_t> candidateKeys;
	candidateKeys.reserve(cellTriMap.size() + cellVoxelMap.size() + candidateVoxelMap.size());
	for (const auto& [key, triIndices] : cellTriMap)
	{
		candidateKeys.push_back(key);
	}
	for (const auto& [key, voxelRefs] : cellVoxelMap)
	{
		if (cellTriMap.find(key) == cellTriMap.end())
		{
			candidateKeys.push_back(key);
		}
	}
	for (const auto& [key, candidateRefinedGroups] : candidateVoxelMap)
	{
		if (cellTriMap.find(key) == cellTriMap.end() && cellVoxelMap.find(key) == cellVoxelMap.end())
		{
			candidateKeys.push_back(key);
		}
	}
	detailedResult.candidateCellCount = static_cast<uint32_t>(std::min<size_t>(candidateKeys.size(), std::numeric_limits<uint32_t>::max()));

	struct VoxelCoverageWorkResult
	{
		std::vector<VoxelCell> emittedCells;
		std::unordered_map<int32_t, VoxelizeTrianglesResult::RefinedGroupStats> refinedGroupStats;
		uint32_t positiveCoverageCellCount = 0;
		float totalCoverage = 0.0f;
		float maxCoverage = 0.0f;
		uint64_t sourceCoverageQueryCount = 0;
		uint64_t sourceCoverageTriangleCandidateCount = 0;
		uint64_t sourceCoverageTriangleTestCount = 0;
		uint64_t sourceCoverageOutOfCellRejectionCount = 0;
	};

	std::vector<VoxelCoverageWorkResult> coverageWorkResults(candidateKeys.size());
	TaskSchedulerManager::GetInstance().ParallelFor("VoxelGroupBuilder::TraceCoverage", candidateKeys.size(),
		[&](size_t candidateKeyIndex)
	{
		VoxelCoverageWorkResult& workResult = coverageWorkResults[candidateKeyIndex];
		auto getRefinedGroupStats = [&workResult](int32_t refinedGroup) -> VoxelizeTrianglesResult::RefinedGroupStats&
		{
			VoxelizeTrianglesResult::RefinedGroupStats& stats = workResult.refinedGroupStats[refinedGroup];
			stats.refinedGroup = refinedGroup;
			return stats;
		};

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
		const auto voxelIt = cellVoxelMap.find(key);
		const auto candidateIt = candidateVoxelMap.find(key);
		std::vector<int32_t> refinedGroups;
		if (triIt != cellTriMap.end() && hasTriangleSources)
		{
			for (uint32_t triangleIndex : triIt->second)
			{
				int32_t refinedGroup = -1;
				if (input.triangleRefinedGroupIds != nullptr && triangleIndex < input.triangleRefinedGroupIds->size())
				{
					refinedGroup = (*input.triangleRefinedGroupIds)[triangleIndex];
				}
				AddUniqueRefinedGroup(refinedGroups, refinedGroup);
			}
		}
		if (voxelIt != cellVoxelMap.end())
		{
			for (const VoxelSourceCellRef& cellRef : voxelIt->second)
			{
				int32_t refinedGroup = -1;
				if (cellRef.payloadIndex < sourceVoxelPayloads.size())
				{
					const VoxelGroupPayload* sourcePayload = sourceVoxelPayloads[cellRef.payloadIndex].payload;
					if (sourcePayload != nullptr && cellRef.cellIndex < sourcePayload->activeCells.size())
					{
						refinedGroup = cellRef.refinedGroup;
					}
				}
				AddUniqueRefinedGroup(refinedGroups, refinedGroup);
			}
		}
		if (candidateIt != candidateVoxelMap.end())
		{
			for (int32_t refinedGroup : candidateIt->second)
			{
				AddUniqueRefinedGroup(refinedGroups, refinedGroup);
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
			const bool hasOnlyCandidateSource = triIt == cellTriMap.end() && voxelIt == cellVoxelMap.end() && candidateIt != candidateVoxelMap.end();
			if (hasOnlyCandidateSource)
			{
				++stats.candidateOnlyCells;
			}

			std::vector<uint32_t> ownedTriangles;
			if (triIt != cellTriMap.end() && hasTriangleSources)
			{
				ownedTriangles.reserve(triIt->second.size());
				for (uint32_t triangleIndex : triIt->second)
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

			std::vector<VoxelSourceCellRef> ownedVoxelRefs;
			if (voxelIt != cellVoxelMap.end())
			{
				ownedVoxelRefs.reserve(voxelIt->second.size());
				for (const VoxelSourceCellRef& cellRef : voxelIt->second)
				{
					int32_t cellRefinedGroup = -1;
					if (cellRef.payloadIndex < sourceVoxelPayloads.size())
					{
						const VoxelGroupPayload* sourcePayload = sourceVoxelPayloads[cellRef.payloadIndex].payload;
						if (sourcePayload != nullptr && cellRef.cellIndex < sourcePayload->activeCells.size())
						{
							cellRefinedGroup = cellRef.refinedGroup;
						}
					}
					if (cellRefinedGroup == refinedGroup)
					{
						ownedVoxelRefs.push_back(cellRef);
					}
				}
			}
			if (!ownedVoxelRefs.empty())
			{
				++stats.voxelOwnedCells;
			}
			if (candidateIt != candidateVoxelMap.end() &&
				std::find(candidateIt->second.begin(), candidateIt->second.end(), refinedGroup) != candidateIt->second.end())
			{
				++stats.candidateOwnedCells;
			}

			CellCoverageSample coverage{};
			uint32_t dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
			if (hasCoverageSourceTriangles)
			{
				coverage = SampleCellCoverageSourceTriangles(
					*input.coverageSourceTriangles,
					refinedGroup,
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
					dominantBoneIndex = ComputeDominantBoneIndexForSourceTriangle(*input.coverageSourceTriangles, coverage.representativeTriangleIndex);
				}
			}
			else if (!ownedTriangles.empty())
			{
				coverage = SampleCellCoverageTriangles(
					*input.vertices, input.vertexStrideBytes,
					*input.triangleIndices,
					ownedTriangles,
					cellMin, cellMax,
					rays,
					input.doubleSidedTriangles,
					input.coverageMaterialSampler);
				dominantBoneIndex = ComputeDominantBoneIndexForCell(input, ownedTriangles);
			}

			if (!ownedVoxelRefs.empty())
			{
				uint32_t voxelDominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
				CellCoverageSample voxelCoverage = SampleCellCoverageVoxels(
					sourceVoxelPayloads,
					ownedVoxelRefs,
					cellMin,
					cellMax,
					voxelDominantBoneIndex);
				if (voxelCoverage.coverage > coverage.coverage)
				{
					coverage = voxelCoverage;
					dominantBoneIndex = voxelDominantBoneIndex;
				}
				else
				{
					coverage.accumulatedNormal = coverage.accumulatedNormal + voxelCoverage.accumulatedNormal;
					coverage.accumulatedSGGX = coverage.accumulatedSGGX + voxelCoverage.accumulatedSGGX;
					coverage.sggxWeight += voxelCoverage.sggxWeight;
					coverage.accumulatedUv.x += voxelCoverage.accumulatedUv.x;
					coverage.accumulatedUv.y += voxelCoverage.accumulatedUv.y;
					coverage.uvWeight += voxelCoverage.uvWeight;
					coverage.uvSamples.insert(coverage.uvSamples.end(), voxelCoverage.uvSamples.begin(), voxelCoverage.uvSamples.end());
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
			if (coverage.coverage <= 0.0f && !input.keepZeroCoverageSourceCells)
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
			vc.uv = SelectRepresentativeUv(coverage);
			vc.dominantBoneIndex = dominantBoneIndex;
			vc.refinedGroup = refinedGroup;

			workResult.emittedCells.push_back(vc);
			++stats.emittedSourceCells;
		}
	});

	result.activeCells.reserve(candidateKeys.size());
	std::unordered_map<int32_t, VoxelizeTrianglesResult::RefinedGroupStats> refinedGroupStats;
	auto accumulateRefinedGroupStats = [](VoxelizeTrianglesResult::RefinedGroupStats& dst, const VoxelizeTrianglesResult::RefinedGroupStats& src)
	{
		dst.refinedGroup = src.refinedGroup;
		dst.candidateKeys += src.candidateKeys;
		dst.triangleOwnedCells += src.triangleOwnedCells;
		dst.voxelOwnedCells += src.voxelOwnedCells;
		dst.candidateOwnedCells += src.candidateOwnedCells;
		dst.candidateOnlyCells += src.candidateOnlyCells;
		dst.positiveCoverageCells += src.positiveCoverageCells;
		dst.zeroCoverageDroppedCells += src.zeroCoverageDroppedCells;
		dst.emittedSourceCells += src.emittedSourceCells;
		dst.totalCoverage += src.totalCoverage;
		dst.maxCoverage = std::max(dst.maxCoverage, src.maxCoverage);
	};

	for (VoxelCoverageWorkResult& workResult : coverageWorkResults)
	{
		detailedResult.positiveCoverageCellCount += workResult.positiveCoverageCellCount;
		detailedResult.totalCoverage += workResult.totalCoverage;
		detailedResult.maxCoverage = std::max(detailedResult.maxCoverage, workResult.maxCoverage);
		detailedResult.sourceCoverageQueryCount += workResult.sourceCoverageQueryCount;
		detailedResult.sourceCoverageTriangleCandidateCount += workResult.sourceCoverageTriangleCandidateCount;
		detailedResult.sourceCoverageTriangleTestCount += workResult.sourceCoverageTriangleTestCount;
		detailedResult.sourceCoverageOutOfCellRejectionCount += workResult.sourceCoverageOutOfCellRejectionCount;

		for (const auto& [refinedGroup, stats] : workResult.refinedGroupStats)
		{
			accumulateRefinedGroupStats(refinedGroupStats[refinedGroup], stats);
		}

		result.activeCells.insert(result.activeCells.end(), workResult.emittedCells.begin(), workResult.emittedCells.end());
	}

	detailedResult.refinedGroupStats.reserve(refinedGroupStats.size());
	for (auto& [refinedGroup, stats] : refinedGroupStats)
	{
		detailedResult.refinedGroupStats.push_back(stats);
	}
	std::sort(detailedResult.refinedGroupStats.begin(), detailedResult.refinedGroupStats.end(), [](const auto& lhs, const auto& rhs) {
		return lhs.refinedGroup < rhs.refinedGroup;
	});

	detailedResult.sourcePayload = result;
	detailedResult.renderPayload = result;
	detailedResult.prunedCellCount = PruneCellsByCoverage(detailedResult.renderPayload.activeCells, input.pruningMode);

	return detailedResult;
}

VoxelGroupPayload VoxelizeTriangles(const VoxelizeTrianglesInput& input)
{
	return VoxelizeTrianglesDetailed(input).renderPayload;
}

PackedVoxelGroupBuildResult PackVoxelGroupToCubes(const PackVoxelGroupInput& input)
{
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
	return result;
}

void BuildVoxelClustersFromCubes(PackedVoxelGroupBuildResult& packed, uint32_t maxCubesPerCluster)
{
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
			cluster.bounds = DirectX::XMFLOAT4(center.x, center.y, center.z, radius);
			cluster.aabbMinAndVoxelWidth = packed.metadata.aabbMinAndVoxelWidth;
			cluster.resolution = packed.metadata.resolution;
			packed.clusterRecords.push_back(cluster);
		}

		runBegin = runEnd;
	}

	packed.metadata.clusterCount = static_cast<uint32_t>(packed.clusterRecords.size());
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
