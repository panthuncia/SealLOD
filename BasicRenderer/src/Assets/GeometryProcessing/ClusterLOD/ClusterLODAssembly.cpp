#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSkinningSidecar.h"

#include <limits>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <bit>
#include <cmath>
#include <array>
#include <functional>
#include <cstring>
#include <iterator>
#include <mutex>
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <numeric>
#include <optional>
#include <span>
#include <chrono>
#include <string>
#include <format>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include <BasicRenderer/Scene/VertexFlags.h>
#include <BasicRenderer/Assets/SGGX.h>
#include "BasicRenderer/Assets/Import/VoxelGroupBuilder.h"
#include <meshoptimizer.h>

#include "../shaders/Common/defines.h"

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSkinningSidecar.h"

using namespace clod_detail;

namespace
{
	DirectX::XMFLOAT3 TransformPoint3x4(const ClusterLODAssemblyTransform& transform, const DirectX::XMFLOAT3& point)
	{
		return {
			transform.row0.x * point.x + transform.row0.y * point.y + transform.row0.z * point.z + transform.row0.w,
			transform.row1.x * point.x + transform.row1.y * point.y + transform.row1.z * point.z + transform.row1.w,
			transform.row2.x * point.x + transform.row2.y * point.y + transform.row2.z * point.z + transform.row2.w,
		};
	}

	float MaxScale3x4(const ClusterLODAssemblyTransform& transform)
	{
		auto rowLength = [](const DirectX::XMFLOAT4& row) {
			return std::sqrt(row.x * row.x + row.y * row.y + row.z * row.z);
		};
		return std::max(rowLength(transform.row0), std::max(rowLength(transform.row1), rowLength(transform.row2)));
	}

	DirectX::XMFLOAT4 TransformSphere3x4(const ClusterLODAssemblyTransform& transform, const DirectX::XMFLOAT4& sphere)
	{
		const DirectX::XMFLOAT3 center{ sphere.x, sphere.y, sphere.z };
		const DirectX::XMFLOAT3 transformedCenter = TransformPoint3x4(transform, center);
		const float scale = MaxScale3x4(transform);
		return { transformedCenter.x, transformedCenter.y, transformedCenter.z, sphere.w * scale };
	}

	DirectX::XMFLOAT3 VoxelCellMin3(const VoxelGroupPayload& payload, const VoxelCell& cell)
	{
		return {
			payload.aabbMin.x + static_cast<float>(cell.x) * payload.voxelWidth,
			payload.aabbMin.y + static_cast<float>(cell.y) * payload.voxelWidth,
			payload.aabbMin.z + static_cast<float>(cell.z) * payload.voxelWidth
		};
	}

	DirectX::XMFLOAT3 VoxelCellMax3(const VoxelGroupPayload& payload, const VoxelCell& cell)
	{
		const DirectX::XMFLOAT3 cellMin = VoxelCellMin3(payload, cell);
		return {
			cellMin.x + payload.voxelWidth,
			cellMin.y + payload.voxelWidth,
			cellMin.z + payload.voxelWidth
		};
	}

	void ExpandAabbWithPoint(
		DirectX::XMFLOAT3& aabbMin,
		DirectX::XMFLOAT3& aabbMax,
		const DirectX::XMFLOAT3& point)
	{
		aabbMin.x = std::min(aabbMin.x, point.x);
		aabbMin.y = std::min(aabbMin.y, point.y);
		aabbMin.z = std::min(aabbMin.z, point.z);
		aabbMax.x = std::max(aabbMax.x, point.x);
		aabbMax.y = std::max(aabbMax.y, point.y);
		aabbMax.z = std::max(aabbMax.z, point.z);
	}

	void ExpandAabbWithTransformedAabb3x4(
		const ClusterLODAssemblyTransform& transform,
		const DirectX::XMFLOAT3& localMin,
		const DirectX::XMFLOAT3& localMax,
		DirectX::XMFLOAT3& aabbMin,
		DirectX::XMFLOAT3& aabbMax)
	{
		for (uint32_t corner = 0; corner < 8u; ++corner)
		{
			const DirectX::XMFLOAT3 point{
				(corner & 1u) != 0u ? localMax.x : localMin.x,
				(corner & 2u) != 0u ? localMax.y : localMin.y,
				(corner & 4u) != 0u ? localMax.z : localMin.z
			};
			ExpandAabbWithPoint(aabbMin, aabbMax, TransformPoint3x4(transform, point));
		}
	}

	bool BuildAabbFromVoxelSourceCells(
		std::span<const VoxelSourcePayloadInstance> sourceInstances,
		DirectX::XMFLOAT3& aabbMin,
		DirectX::XMFLOAT3& aabbMax)
	{
		aabbMin = DirectX::XMFLOAT3(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		aabbMax = DirectX::XMFLOAT3(
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());

		bool valid = false;
		for (const VoxelSourcePayloadInstance& source : sourceInstances)
		{
			if (source.payload == nullptr || source.payload->activeCells.empty() || source.payload->voxelWidth <= 0.0f)
			{
				continue;
			}

			for (const VoxelCell& cell : source.payload->activeCells)
			{
				const DirectX::XMFLOAT3 cellMin = VoxelCellMin3(*source.payload, cell);
				const DirectX::XMFLOAT3 cellMax = VoxelCellMax3(*source.payload, cell);
				ExpandAabbWithTransformedAabb3x4(source.localToTarget, cellMin, cellMax, aabbMin, aabbMax);
				valid = true;
			}
		}

		return valid;
	}

	struct VoxelCellRefinedKey
	{
		uint64_t cellKey = 0;
		int32_t refinedGroup = -1;

		bool operator==(const VoxelCellRefinedKey& other) const
		{
			return cellKey == other.cellKey && refinedGroup == other.refinedGroup;
		}
	};

	struct VoxelCellRefinedKeyHash
	{
		size_t operator()(const VoxelCellRefinedKey& key) const
		{
			size_t seed = std::hash<uint64_t>{}(key.cellKey);
			seed ^= std::hash<int32_t>{}(key.refinedGroup) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
			return seed;
		}
	};

	br::mesh::sggx::SymmetricMatrix3 TransformSGGX3x4(
		const ClusterLODAssemblyTransform& transform,
		const br::mesh::sggx::SymmetricMatrix3& source)
	{
		using br::mesh::sggx::Float3;

		const Float3 r0(transform.row0.x, transform.row0.y, transform.row0.z);
		const Float3 r1(transform.row1.x, transform.row1.y, transform.row1.z);
		const Float3 r2(transform.row2.x, transform.row2.y, transform.row2.z);
		const Float3 c0 = r1.cross(r2);
		const Float3 c1 = r2.cross(r0);
		const Float3 c2 = r0.cross(r1);
		const float det = r0.dot(c0);
		if (std::abs(det) <= 1.0e-8f)
		{
			return source;
		}

		const float invDet = 1.0f / det;
		const float n[3][3] = {
			{ c0.x * invDet, c0.y * invDet, c0.z * invDet },
			{ c1.x * invDet, c1.y * invDet, c1.z * invDet },
			{ c2.x * invDet, c2.y * invDet, c2.z * invDet }
		};
		const float m[3][3] = {
			{ source.xx, source.xy, source.xz },
			{ source.xy, source.yy, source.yz },
			{ source.xz, source.yz, source.zz }
		};
		float nm[3][3]{};
		for (uint32_t row = 0u; row < 3u; ++row)
		{
			for (uint32_t col = 0u; col < 3u; ++col)
			{
				for (uint32_t k = 0u; k < 3u; ++k)
				{
					nm[row][col] += n[row][k] * m[k][col];
				}
			}
		}

		float transformed[3][3]{};
		for (uint32_t row = 0u; row < 3u; ++row)
		{
			for (uint32_t col = 0u; col < 3u; ++col)
			{
				for (uint32_t k = 0u; k < 3u; ++k)
				{
					transformed[row][col] += nm[row][k] * n[col][k];
				}
			}
		}

		br::mesh::sggx::SymmetricMatrix3 result{
			transformed[0][0],
			transformed[1][1],
			transformed[2][2],
			0.5f * (transformed[0][1] + transformed[1][0]),
			0.5f * (transformed[0][2] + transformed[2][0]),
			0.5f * (transformed[1][2] + transformed[2][1])
		};

		const float sourceTrace = std::max(source.xx + source.yy + source.zz, 1.0e-8f);
		const float resultTrace = result.xx + result.yy + result.zz;
		if (resultTrace > 1.0e-8f)
		{
			result = result * (sourceTrace / resultTrace);
		}
		return result;
	}

	void ApplyChildPayloadSGGXToParentCells(
		VoxelGroupPayload& parentPayload,
		std::span<const VoxelSourcePayloadInstance> sourceInstances)
	{
		if (parentPayload.activeCells.empty() || parentPayload.voxelWidth <= 0.0f || parentPayload.resolution == 0u || sourceInstances.empty())
		{
			return;
		}

		struct SGGXAccum
		{
			br::mesh::sggx::SymmetricMatrix3 sum{};
			float weight = 0.0f;
		};

		std::unordered_set<VoxelCellRefinedKey, VoxelCellRefinedKeyHash> activeParentCells;
		activeParentCells.reserve(parentPayload.activeCells.size());
		for (const VoxelCell& parentCell : parentPayload.activeCells)
		{
			activeParentCells.insert(VoxelCellRefinedKey{
				PackVoxelTailCellKey(parentCell.x, parentCell.y, parentCell.z),
				parentCell.refinedGroup });
		}

		const float invParentVoxelWidth = 1.0f / parentPayload.voxelWidth;
		auto minCellCoord = [&](float value, float minValue) -> uint32_t
		{
			const int32_t coord = static_cast<int32_t>(std::floor((value - minValue) * invParentVoxelWidth));
			return static_cast<uint32_t>(std::clamp<int32_t>(coord, 0, static_cast<int32_t>(parentPayload.resolution) - 1));
		};
		auto maxCellCoord = [&](float value, float minValue) -> uint32_t
		{
			const int32_t coord = static_cast<int32_t>(std::ceil((value - minValue) * invParentVoxelWidth)) - 1;
			return static_cast<uint32_t>(std::clamp<int32_t>(coord, 0, static_cast<int32_t>(parentPayload.resolution) - 1));
		};
		auto parentCellMin = [&](uint32_t x, uint32_t y, uint32_t z) -> DirectX::XMFLOAT3
		{
			return DirectX::XMFLOAT3(
				parentPayload.aabbMin.x + static_cast<float>(x) * parentPayload.voxelWidth,
				parentPayload.aabbMin.y + static_cast<float>(y) * parentPayload.voxelWidth,
				parentPayload.aabbMin.z + static_cast<float>(z) * parentPayload.voxelWidth);
		};
		auto overlapAxis = [](float aMin, float aMax, float bMin, float bMax) -> float
		{
			return std::max(0.0f, std::min(aMax, bMax) - std::max(aMin, bMin));
		};
		auto overlapsParentAabb = [&](const DirectX::XMFLOAT3& minValue, const DirectX::XMFLOAT3& maxValue) -> bool
		{
			if (!std::isfinite(minValue.x) || !std::isfinite(minValue.y) || !std::isfinite(minValue.z) ||
				!std::isfinite(maxValue.x) || !std::isfinite(maxValue.y) || !std::isfinite(maxValue.z))
			{
				return false;
			}
			return maxValue.x > parentPayload.aabbMin.x &&
				maxValue.y > parentPayload.aabbMin.y &&
				maxValue.z > parentPayload.aabbMin.z &&
				minValue.x < parentPayload.aabbMax.x &&
				minValue.y < parentPayload.aabbMax.y &&
				minValue.z < parentPayload.aabbMax.z;
		};

		std::unordered_map<VoxelCellRefinedKey, SGGXAccum, VoxelCellRefinedKeyHash> accumulations;
		for (const VoxelSourcePayloadInstance& sourceInstance : sourceInstances)
		{
			const VoxelGroupPayload* sourcePayload = sourceInstance.payload;
			if (sourcePayload == nullptr || sourcePayload->activeCells.empty() || sourcePayload->voxelWidth <= 0.0f)
			{
				continue;
			}

			const bool hasRefinedGroupOverride = sourceInstance.refinedGroupOverride != std::numeric_limits<int32_t>::min();
			for (const VoxelCell& sourceCell : sourcePayload->activeCells)
			{
				const DirectX::XMFLOAT3 sourceMin = VoxelCellMin3(*sourcePayload, sourceCell);
				const DirectX::XMFLOAT3 sourceMax = VoxelCellMax3(*sourcePayload, sourceCell);
				DirectX::XMFLOAT3 transformedMin(
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max(),
					std::numeric_limits<float>::max());
				DirectX::XMFLOAT3 transformedMax(
					std::numeric_limits<float>::lowest(),
					std::numeric_limits<float>::lowest(),
					std::numeric_limits<float>::lowest());
				ExpandAabbWithTransformedAabb3x4(sourceInstance.localToTarget, sourceMin, sourceMax, transformedMin, transformedMax);
				if (!overlapsParentAabb(transformedMin, transformedMax))
				{
					continue;
				}

				const int32_t refinedGroup = hasRefinedGroupOverride ? sourceInstance.refinedGroupOverride : sourceCell.refinedGroup;
				const uint32_t xMin = minCellCoord(transformedMin.x, parentPayload.aabbMin.x);
				const uint32_t yMin = minCellCoord(transformedMin.y, parentPayload.aabbMin.y);
				const uint32_t zMin = minCellCoord(transformedMin.z, parentPayload.aabbMin.z);
				const uint32_t xMax = maxCellCoord(transformedMax.x, parentPayload.aabbMin.x);
				const uint32_t yMax = maxCellCoord(transformedMax.y, parentPayload.aabbMin.y);
				const uint32_t zMax = maxCellCoord(transformedMax.z, parentPayload.aabbMin.z);
				const float transformedVolume = std::max(
					(transformedMax.x - transformedMin.x) *
					(transformedMax.y - transformedMin.y) *
					(transformedMax.z - transformedMin.z),
					1.0e-12f);
				const br::mesh::sggx::SymmetricMatrix3 transformedSGGX =
					TransformSGGX3x4(sourceInstance.localToTarget, br::mesh::sggx::DecodeAxialSGGX(sourceCell.sggxAxisAndSigmas));

				for (uint32_t z = zMin; z <= zMax; ++z)
				{
					for (uint32_t y = yMin; y <= yMax; ++y)
					{
						for (uint32_t x = xMin; x <= xMax; ++x)
						{
							const VoxelCellRefinedKey key{ PackVoxelTailCellKey(x, y, z), refinedGroup };
							if (activeParentCells.find(key) == activeParentCells.end())
							{
								continue;
							}

							const DirectX::XMFLOAT3 cellMin = parentCellMin(x, y, z);
							const DirectX::XMFLOAT3 cellMax(
								cellMin.x + parentPayload.voxelWidth,
								cellMin.y + parentPayload.voxelWidth,
								cellMin.z + parentPayload.voxelWidth);
							const float overlapVolume =
								overlapAxis(transformedMin.x, transformedMax.x, cellMin.x, cellMax.x) *
								overlapAxis(transformedMin.y, transformedMax.y, cellMin.y, cellMax.y) *
								overlapAxis(transformedMin.z, transformedMax.z, cellMin.z, cellMax.z);
							if (overlapVolume <= 1.0e-12f)
							{
								continue;
							}

							const float weight = std::max(sourceCell.opacity, 1.0e-6f) * (overlapVolume / transformedVolume);
							SGGXAccum& accum = accumulations[key];
							accum.sum = accum.sum + transformedSGGX * weight;
							accum.weight += weight;
						}
					}
				}
			}
		}

		for (VoxelCell& parentCell : parentPayload.activeCells)
		{
			const VoxelCellRefinedKey key{ PackVoxelTailCellKey(parentCell.x, parentCell.y, parentCell.z), parentCell.refinedGroup };
			const auto accumIt = accumulations.find(key);
			if (accumIt == accumulations.end() || accumIt->second.weight <= 1.0e-12f)
			{
				continue;
			}

			const br::mesh::sggx::SymmetricMatrix3 averagedSGGX = accumIt->second.sum * (1.0f / accumIt->second.weight);
			parentCell.sggxAxisAndSigmas = br::mesh::sggx::EncodeAxialSGGX(br::mesh::sggx::CompressSGGXToAxial(averagedSGGX));
		}
	}

	ClusterLODNode BuildAssemblyInternalNode(
		const std::vector<ClusterLODNode>& nodes,
		uint32_t childOffset,
		uint32_t childCount)
	{
		if (childCount == 0u || childOffset + childCount > nodes.size())
		{
			throw std::runtime_error("ClusterLOD assembly: invalid internal node child range");
		}

		ClusterLODNode node{};
		node.range.isGroup = CLOD_NODE_INTERNAL;
		node.range.indexOrOffset = childOffset;
		node.range.countMinusOne = childCount - 1u;

		float maxError = 0.0f;
		for (uint32_t childIndex = 0; childIndex < childCount; ++childIndex)
		{
			maxError = std::max(maxError, nodes[childOffset + childIndex].traversalMetric.maxQuadricError);
		}
		node.traversalMetric.maxQuadricError = maxError;

		meshopt_Bounds mergedCull = meshopt_computeSphereBounds(
			&nodes[childOffset].traversalMetric.cullingSphere.x,
			childCount,
			sizeof(ClusterLODNode),
			&nodes[childOffset].traversalMetric.cullingSphere.w,
			sizeof(ClusterLODNode));
		meshopt_Bounds mergedLod = meshopt_computeSphereBounds(
			&nodes[childOffset].traversalMetric.lodBoundingSphere.x,
			childCount,
			sizeof(ClusterLODNode),
			&nodes[childOffset].traversalMetric.lodBoundingSphere.w,
			sizeof(ClusterLODNode));

		node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(
			mergedCull.center[0],
			mergedCull.center[1],
			mergedCull.center[2],
			mergedCull.radius * (1.0f + 1e-5f));
		node.traversalMetric.lodBoundingSphere = DirectX::XMFLOAT4(
			mergedLod.center[0],
			mergedLod.center[1],
			mergedLod.center[2],
			mergedLod.radius * (1.0f + 1e-5f));
		return node;
	}
}

ClusterLODPrebuildArtifacts BuildClusterLODAssemblyArtifacts(
	std::span<const ClusterLODAssemblyPart> parts,
	std::span<const ClusterLODAssemblyInstanceSpec> instances,
	const ClusterLODBuilderSettings& settings,
	uint32_t preferredNodeWidth,
	bool synthesizeVoxelParents)
{
	if (parts.empty())
	{
		throw std::runtime_error("ClusterLOD assembly: at least one part is required");
	}
	if (instances.empty())
	{
		throw std::runtime_error("ClusterLOD assembly: at least one instance is required");
	}

	preferredNodeWidth = std::max(2u, preferredNodeWidth);

	ClusterLODPrebuildArtifacts out{};
	ClusterLODBuildState state{};
	std::vector<ClusterLODNode> libraryNodes;
	std::vector<NodeBoneSet> libraryNodeBoneSets;
	std::vector<ClusterLODAssemblyTransform> assemblyTransforms;
	std::vector<ClusterLODAssemblyInstance> assemblyInstances;
	std::vector<ClusterLODAssemblyBoneRemap> assemblyBoneRemaps;
	std::vector<uint32_t> assemblyBoneRemapIndices;
	std::vector<std::vector<VoxelSourcePayloadInstance>> assemblyGroupSources;
	std::vector<std::vector<int32_t>> assemblyCoverageDomainMap;
	std::vector<VoxelSourceTrianglePart> assemblyCoverageParts;
	std::vector<VoxelSourceTriangleInstance> assemblyCoverageInstances;
	VoxelSourceTriangleBVH assemblyCoverageSourceTriangles;
	bool assemblyCoverageDoubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
	std::vector<uint32_t> groupBases(parts.size(), 0u);
	std::vector<std::vector<uint32_t>> segmentRemaps(parts.size());
	std::vector<uint32_t> nodeBases(parts.size(), 0u);
	std::vector<uint32_t> transformBases(parts.size(), 0u);
	std::vector<uint32_t> instanceBases(parts.size(), 0u);
	std::vector<ClusterLODPartRecord> copiedPartRecords;
	assemblyCoverageParts.resize(parts.size());

	size_t assemblyStorageReserve = instances.size() * 2ull + 64ull;
	size_t transientPayloadReserve = instances.size() * 4ull + 64ull;
	for (const ClusterLODAssemblyPart& partRef : parts)
	{
		if (partRef.artifacts != nullptr)
		{
			assemblyStorageReserve += partRef.artifacts->prebuiltData.groups.size();
			transientPayloadReserve += partRef.artifacts->cacheBuildData.voxelGroupMapping.payloads.size();
		}
	}
	state.groups.reserve(assemblyStorageReserve);
	state.groupChunks.reserve(assemblyStorageReserve);
	state.groupPageBlobs.reserve(assemblyStorageReserve);
	state.traversalGroupMask.reserve(assemblyStorageReserve);
	assemblyGroupSources.reserve(assemblyStorageReserve);
	state.voxelCarryPayloads.reserve(assemblyStorageReserve);
	state.voxelGroupMapping.groupToPayloadIndex.reserve(assemblyStorageReserve);
	state.voxelGroupMapping.groupToPackedMetadataIndex.reserve(assemblyStorageReserve);
	state.voxelGroupMapping.payloads.reserve(transientPayloadReserve);

	for (size_t partIndex = 0; partIndex < parts.size(); ++partIndex)
	{
		const ClusterLODPrebuildArtifacts* partArtifacts = parts[partIndex].artifacts;
		if (partArtifacts == nullptr)
		{
			throw std::runtime_error("ClusterLOD assembly: null part artifact");
		}
		const ClusterLODPrebuiltData& part = partArtifacts->prebuiltData;
		if (part.groups.empty() || part.nodes.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: part has no CLod hierarchy");
		}
		assemblyCoverageParts[partIndex] = VoxelSourceTrianglePart{
			.vertices = parts[partIndex].coverageVertices,
			.vertexStrideBytes = parts[partIndex].coverageVertexSize,
			.skinningVertices = parts[partIndex].coverageSkinningVertices,
			.skinningVertexStrideBytes = parts[partIndex].coverageSkinningVertexSize,
			.triangleIndices = parts[partIndex].coverageIndices };
		assemblyCoverageDoubleSidedTriangles =
			assemblyCoverageDoubleSidedTriangles || parts[partIndex].doubleSidedCoverageTriangles;

		groupBases[partIndex] = static_cast<uint32_t>(state.groups.size());
		segmentRemaps[partIndex].assign(part.segments.size(), UINT32_MAX);
		nodeBases[partIndex] = static_cast<uint32_t>(libraryNodes.size());
		transformBases[partIndex] = static_cast<uint32_t>(assemblyTransforms.size());
		instanceBases[partIndex] = static_cast<uint32_t>(assemblyInstances.size());

		const VoxelGroupMapping& partVoxelMapping = partArtifacts->cacheBuildData.voxelGroupMapping;
		const uint32_t payloadBase = static_cast<uint32_t>(state.voxelGroupMapping.payloads.size());
		const uint32_t metadataBase = static_cast<uint32_t>(state.voxelGroupMapping.packedGroupMetadata.size());
		const uint32_t clusterBase = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t cubeBase = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
		const uint32_t attributeBase = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
		state.voxelGroupMapping.payloads.insert(
			state.voxelGroupMapping.payloads.end(),
			partVoxelMapping.payloads.begin(),
			partVoxelMapping.payloads.end());
		for (VoxelGroupPackedMetadata metadata : partVoxelMapping.packedGroupMetadata)
		{
			metadata.firstCluster += clusterBase;
			metadata.firstCube += cubeBase;
			state.voxelGroupMapping.packedGroupMetadata.push_back(metadata);
		}
		for (CLodVoxelClusterRecord cluster : partVoxelMapping.packedClusterRecords)
		{
			cluster.firstCube += cubeBase;
			state.voxelGroupMapping.packedClusterRecords.push_back(cluster);
		}
		for (CLodVoxelCubeRecord cube : partVoxelMapping.packedCubeRecords)
		{
			cube.firstAttribute += attributeBase;
			state.voxelGroupMapping.packedCubeRecords.push_back(cube);
		}
		state.voxelGroupMapping.packedAttributeSamples.insert(
			state.voxelGroupMapping.packedAttributeSamples.end(),
			partVoxelMapping.packedAttributeSamples.begin(),
			partVoxelMapping.packedAttributeSamples.end());

		for (uint32_t localGroupIndex = 0; localGroupIndex < static_cast<uint32_t>(part.groups.size()); ++localGroupIndex)
		{
			const ClusterLODGroup& srcGroup = part.groups[localGroupIndex];
			ClusterLODGroup group = srcGroup;
			group.firstSegment = static_cast<uint32_t>(state.segments.size());
			group.firstMeshlet += 0u;
			group.firstGroupVertex += 0u;
			group.pageMapBase = 0u;
			if (group.parentGroupId >= 0)
			{
				group.parentGroupId += static_cast<int32_t>(groupBases[partIndex]);
			}

			for (uint32_t localSegmentOffset = 0; localSegmentOffset < srcGroup.segmentCount; ++localSegmentOffset)
			{
				const uint32_t localSegmentIndex = srcGroup.firstSegment + localSegmentOffset;
				if (localSegmentIndex >= part.segments.size())
				{
					throw std::runtime_error("ClusterLOD assembly: part segment range out of bounds");
				}

				ClusterLODGroupSegment segment = part.segments[localSegmentIndex];
				if (segment.refinedGroup >= 0)
				{
					segment.refinedGroup += static_cast<int32_t>(groupBases[partIndex]);
				}
				if (segment.meshletCount != 0u && srcGroup.pageCount != 0u)
				{
					if (segment.pageIndex < srcGroup.pageMapBase || segment.pageIndex >= srcGroup.pageMapBase + srcGroup.pageCount)
					{
						throw std::runtime_error("ClusterLOD assembly: part segment page index outside owning group page map");
					}
					segment.pageIndex -= srcGroup.pageMapBase;
				}
				segmentRemaps[partIndex][localSegmentIndex] = static_cast<uint32_t>(state.segments.size());
				state.segments.push_back(segment);
				if (localSegmentIndex < part.segmentBounds.size())
				{
					state.segmentBounds.push_back(part.segmentBounds[localSegmentIndex]);
				}
				else
				{
					state.segmentBounds.push_back({});
				}
			}

			state.groups.push_back(group);
			state.groupChunks.push_back(localGroupIndex < part.groupChunks.size() ? part.groupChunks[localGroupIndex] : ClusterLODGroupChunk{});
			if (part.groupPageReferenceOffsets.size() == part.groups.size() + 1ull &&
				localGroupIndex + 1u < part.groupPageReferenceOffsets.size() &&
				partArtifacts->cacheBuildData.meshPageBlobs.size() != 0u)
			{
				std::vector<std::vector<std::byte>> groupPages;
				const uint32_t pageRefBegin = part.groupPageReferenceOffsets[localGroupIndex];
				const uint32_t pageRefEnd = part.groupPageReferenceOffsets[localGroupIndex + 1u];
				groupPages.reserve(pageRefEnd - pageRefBegin);
				for (uint32_t pageRefIndex = pageRefBegin; pageRefIndex < pageRefEnd; ++pageRefIndex)
				{
					if (pageRefIndex >= part.groupPageReferences.size())
					{
						continue;
					}
					const uint32_t meshPageIndex = part.groupPageReferences[pageRefIndex];
					if (meshPageIndex < partArtifacts->cacheBuildData.meshPageBlobs.size())
					{
						groupPages.push_back(partArtifacts->cacheBuildData.meshPageBlobs[meshPageIndex]);
					}
				}
				state.groupPageBlobs.push_back(std::move(groupPages));
			}
			else if (localGroupIndex < partArtifacts->cacheBuildData.groupPageBlobs.size())
			{
				state.groupPageBlobs.push_back(partArtifacts->cacheBuildData.groupPageBlobs[localGroupIndex]);
			}
			else
			{
				state.groupPageBlobs.emplace_back();
			}
			state.traversalGroupMask.push_back(0u);
			assemblyGroupSources.emplace_back();
			state.voxelCarryPayloads.emplace_back();
			state.voxelGroupMapping.groupToPayloadIndex.push_back(-1);
			state.voxelGroupMapping.groupToPackedMetadataIndex.push_back(-1);

			if (localGroupIndex < partVoxelMapping.groupToPayloadIndex.size())
			{
				const int32_t localPayloadIndex = partVoxelMapping.groupToPayloadIndex[localGroupIndex];
				if (localPayloadIndex >= 0 && static_cast<size_t>(localPayloadIndex) < partVoxelMapping.payloads.size())
				{
					state.voxelGroupMapping.groupToPayloadIndex.back() = static_cast<int32_t>(payloadBase + static_cast<uint32_t>(localPayloadIndex));
				}
			}
			if (localGroupIndex < partVoxelMapping.groupToPackedMetadataIndex.size())
			{
				const int32_t localMetadataIndex = partVoxelMapping.groupToPackedMetadataIndex[localGroupIndex];
				if (localMetadataIndex >= 0 && static_cast<size_t>(localMetadataIndex) < partVoxelMapping.packedGroupMetadata.size())
				{
					state.voxelGroupMapping.groupToPackedMetadataIndex.back() = static_cast<int32_t>(metadataBase + static_cast<uint32_t>(localMetadataIndex));
				}
			}
		}

		for (uint32_t localNodeIndex = 0u; localNodeIndex < static_cast<uint32_t>(part.nodes.size()); ++localNodeIndex)
		{
			ClusterLODNode node = part.nodes[localNodeIndex];
			switch (node.range.isGroup)
			{
			case CLOD_NODE_INTERNAL:
				node.range.indexOrOffset += nodeBases[partIndex];
				break;
			case CLOD_NODE_VOXEL_LEAF:
				if (node.range.countMinusOne != 0u)
				{
					node.range.countMinusOne += groupBases[partIndex];
				}
				node.range.ownerGroupId += groupBases[partIndex];
				break;
			case CLOD_NODE_SEGMENT_LEAF:
				if (node.range.indexOrOffset >= segmentRemaps[partIndex].size() ||
					segmentRemaps[partIndex][node.range.indexOrOffset] == UINT32_MAX)
				{
					throw std::runtime_error("ClusterLOD assembly: segment leaf references an unmapped part segment");
				}
				node.range.indexOrOffset = segmentRemaps[partIndex][node.range.indexOrOffset];
				if (node.range.countMinusOne != 0u)
				{
					node.range.countMinusOne += groupBases[partIndex];
				}
				node.range.ownerGroupId += groupBases[partIndex];
				break;
			case CLOD_NODE_INSTANCE_ROOT:
				node.range.indexOrOffset += instanceBases[partIndex];
				break;
			default:
				throw std::runtime_error("ClusterLOD assembly: unknown part node kind");
			}
			libraryNodes.push_back(node);
			NodeBoneSet preservedSet{};
			if (localNodeIndex >= part.nodeSkinningInfos.size())
			{
				preservedSet.flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
			}
			else
			{
				const ClusterLODNodeSkinningInfo& info = part.nodeSkinningInfos[localNodeIndex];
				const uint64_t boneEnd = static_cast<uint64_t>(info.boneListOffset) + info.boneCount;
				constexpr uint16_t validFlags =
					CLOD_NODE_SKINNING_FLAG_OVERFLOW | CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
				if (boneEnd > part.nodeBoneIndices.size() ||
					(info.flags & ~validFlags) != 0u || info.flags == validFlags ||
					(info.flags != 0u && info.boneCount != 0u))
				{
					preservedSet.flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
				}
				else
				{
					preservedSet.flags = info.flags;
					preservedSet.bones.insert(
						preservedSet.bones.end(),
						part.nodeBoneIndices.begin() + info.boneListOffset,
						part.nodeBoneIndices.begin() + static_cast<size_t>(boneEnd));
				}
			}
			libraryNodeBoneSets.push_back(std::move(preservedSet));
		}

		const uint32_t remapIndexBase = static_cast<uint32_t>(assemblyBoneRemapIndices.size());
		assemblyBoneRemapIndices.insert(
			assemblyBoneRemapIndices.end(),
			part.assemblyBoneRemapIndices.begin(),
			part.assemblyBoneRemapIndices.end());

		for (uint32_t localTransformIndex = 0; localTransformIndex < static_cast<uint32_t>(part.assemblyTransforms.size()); ++localTransformIndex)
		{
			assemblyTransforms.push_back(part.assemblyTransforms[localTransformIndex]);
			ClusterLODAssemblyBoneRemap remap{};
			if (localTransformIndex < part.assemblyBoneRemaps.size())
			{
				remap = part.assemblyBoneRemaps[localTransformIndex];
				if (remap.remapIndexBase != CLOD_ASSEMBLY_BONE_REMAP_SENTINEL)
				{
					remap.remapIndexBase += remapIndexBase;
				}
			}
			assemblyBoneRemaps.push_back(remap);
		}
		for (ClusterLODAssemblyInstance instance : part.assemblyInstances)
		{
			instance.targetRootNode += nodeBases[partIndex];
			if (instance.transformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
			{
				instance.transformIndex += transformBases[partIndex];
			}
			if (instance.stackDepth > CLOD_ASSEMBLY_MAX_STACK_DEPTH)
			{
				throw std::runtime_error("ClusterLOD assembly: nested part exceeds max stack depth");
			}
			assemblyInstances.push_back(instance);
		}

		if (!part.partRecords.empty())
		{
			for (ClusterLODPartRecord record : part.partRecords)
			{
				record.groupBase += groupBases[partIndex];
				record.nodeBase += nodeBases[partIndex];
				record.transformBase += transformBases[partIndex];
				record.instanceBase += instanceBases[partIndex];
				record.rootNode += nodeBases[partIndex];
				record.flags &= ~CLOD_PART_RECORD_FLAG_ROOT;
				copiedPartRecords.push_back(record);
			}
		}
		else
		{
			ClusterLODPartRecord record{};
			record.groupBase = groupBases[partIndex];
			record.groupCount = static_cast<uint32_t>(part.groups.size());
			record.nodeBase = nodeBases[partIndex];
			record.nodeCount = static_cast<uint32_t>(part.nodes.size());
			record.transformBase = transformBases[partIndex];
			record.transformCount = static_cast<uint32_t>(part.assemblyTransforms.size());
			record.instanceBase = instanceBases[partIndex];
			record.instanceCount = static_cast<uint32_t>(part.assemblyInstances.size());
			record.rootNode = nodeBases[partIndex];
			record.flags = 0u;
			copiedPartRecords.push_back(record);
		}
	}

	const uint32_t rootAssemblyGroupBase = static_cast<uint32_t>(state.groups.size());
	auto getVoxelPayloadForGroup = [&](uint32_t groupIndex) -> const VoxelGroupPayload*
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPayloadIndex.size())
		{
			return nullptr;
		}
		const int32_t payloadIndex = state.voxelGroupMapping.groupToPayloadIndex[groupIndex];
		if (payloadIndex < 0 || static_cast<size_t>(payloadIndex) >= state.voxelGroupMapping.payloads.size())
		{
			return nullptr;
		}
		return &state.voxelGroupMapping.payloads[static_cast<size_t>(payloadIndex)];
	};

	auto getOrBuildVoxelPayloadForGroup = [&](uint32_t groupIndex) -> const VoxelGroupPayload*
	{
		if (const VoxelGroupPayload* payload = getVoxelPayloadForGroup(groupIndex))
		{
			return payload;
		}

		if (groupIndex >= state.voxelCarryPayloads.size())
		{
			return nullptr;
		}

		VoxelGroupPayload& carryPayload = state.voxelCarryPayloads[groupIndex];
		if (carryPayload.voxelWidth > 0.0f && !carryPayload.activeCells.empty())
		{
			return &carryPayload;
		}

		if (!BuildVoxelGroupPayloadFromPackedMapping(state.voxelGroupMapping, groupIndex, carryPayload))
		{
			return nullptr;
		}

		TracyPlot("CLOD.Assembly.UnpackedSourceCells", static_cast<int64_t>(carryPayload.activeCells.size()));
		return &carryPayload;
	};

	auto appendGroupStorage = [&](ClusterLODGroup group, bool includeInTraversal) -> uint32_t
	{
		const uint32_t groupIndex = static_cast<uint32_t>(state.groups.size());
		state.groups.push_back(group);
		state.groupChunks.emplace_back();
		state.groupPageBlobs.emplace_back();
		state.traversalGroupMask.push_back(includeInTraversal ? 1u : 0u);
		assemblyGroupSources.emplace_back();
		state.voxelCarryPayloads.emplace_back();
		state.voxelGroupMapping.groupToPayloadIndex.push_back(-1);
		state.voxelGroupMapping.groupToPackedMetadataIndex.push_back(-1);
		assemblyCoverageDomainMap.emplace_back();
		return groupIndex;
	};

	auto appendCoverageDomain = [&](uint32_t groupIndex, int32_t refinedGroup)
	{
		if (groupIndex >= assemblyCoverageDomainMap.size())
		{
			assemblyCoverageDomainMap.resize(static_cast<size_t>(groupIndex) + 1ull);
		}
		std::vector<int32_t>& domain = assemblyCoverageDomainMap[groupIndex];
		if (std::find(domain.begin(), domain.end(), refinedGroup) == domain.end())
		{
			domain.push_back(refinedGroup);
		}
	};

	auto buildAssemblyVoxelGroup = [&](std::span<const uint32_t> childGroups, int32_t depth) -> uint32_t
	{
		std::vector<VoxelSourcePayloadInstance> sourceInstances;
		float maxSourceVoxelWidth = 0.0f;
		sourceInstances.reserve(childGroups.size() * 2ull);
		for (uint32_t childGroup : childGroups)
		{
			for (VoxelSourcePayloadInstance source : assemblyGroupSources[childGroup])
			{
				if (source.payload == nullptr || source.payload->activeCells.empty() || source.payload->voxelWidth <= 0.0f)
				{
					continue;
				}
				source.refinedGroupOverride = static_cast<int32_t>(childGroup);
				sourceInstances.push_back(source);
				maxSourceVoxelWidth = std::max(maxSourceVoxelWidth, source.payload->voxelWidth * MaxScale3x4(source.localToTarget));
			}
		}
		if (sourceInstances.empty() || maxSourceVoxelWidth <= 0.0f)
		{
			throw std::runtime_error("ClusterLOD assembly: child assembly groups have no voxel payload sources");
		}

		DirectX::XMFLOAT3 aabbMin{};
		DirectX::XMFLOAT3 aabbMax{};
		if (!BuildAabbFromVoxelSourceCells(sourceInstances, aabbMin, aabbMax))
		{
			throw std::runtime_error("ClusterLOD assembly: child assembly groups have no finite voxel payload bounds");
		}
		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (!std::isfinite(longestExtent) || longestExtent <= 1.0e-8f)
		{
			throw std::runtime_error("ClusterLOD assembly: degenerate assembly voxel bounds");
		}

		auto expandAxisToExtent = [](float& minValue, float& maxValue, float targetExtent)
		{
			const float currentExtent = maxValue - minValue;
			if (currentExtent >= targetExtent)
			{
				return;
			}
			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * targetExtent;
			maxValue = center + 0.5f * targetExtent;
		};
		expandAxisToExtent(aabbMin.x, aabbMax.x, longestExtent);
		expandAxisToExtent(aabbMin.y, aabbMax.y, longestExtent);
		expandAxisToExtent(aabbMin.z, aabbMax.z, longestExtent);

		const float growthFactor = std::max(1.01f, settings.voxelFallbackGrowthFactor);
		const float baseResolution = static_cast<float>(std::max(2u, settings.voxelGridBaseResolution));
		const float voxelWidth = std::max({
			maxSourceVoxelWidth * growthFactor,
			longestExtent / baseResolution });
		const uint32_t resolution = std::max(
			std::max(2u, settings.voxelMinResolution),
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(voxelWidth, 1.0e-8f))));
		const float voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
		const float sourceToParentRatio = std::max(1.0f, voxelWidth / std::max(maxSourceVoxelWidth, 1.0e-8f));
		const uint32_t rayScale = std::clamp(
			static_cast<uint32_t>(std::ceil(sourceToParentRatio * sourceToParentRatio)),
			1u,
			32u);

		VoxelizeTrianglesInput voxelInput{};
		voxelInput.sourceVoxelPayloadInstances = &sourceInstances;
		voxelInput.candidateVoxelPayloadInstances = &sourceInstances;
		voxelInput.coverageSourceTriangles = assemblyCoverageSourceTriangles.IsValid() ? &assemblyCoverageSourceTriangles : nullptr;
		voxelInput.aabbMin = aabbMin;
		voxelInput.aabbMax = aabbMax;
		voxelInput.voxelWidth = voxelWidth;
		voxelInput.resolution = resolution;
		voxelInput.raysPerCell = std::max(1u, settings.voxelRaysPerCell) * rayScale;
		voxelInput.emitSourcePayload = false;
		if (assemblyCoverageSourceTriangles.IsValid())
		{
			assemblyCoverageSourceTriangles.SetRefinedGroupDomainMap(assemblyCoverageDomainMap);
		}
		VoxelizeTrianglesResult voxelResult = VoxelizeTrianglesDetailed(voxelInput);
		if (voxelResult.renderPayload.activeCells.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: assembly voxelization produced no render cells");
		}
		ApplyChildPayloadSGGXToParentCells(voxelResult.renderPayload, sourceInstances);

		const uint32_t firstCluster = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t firstCube = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
		const uint32_t firstAttribute = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
		PackVoxelGroupInput packInput{};
		packInput.payload = &voxelResult.renderPayload;
		packInput.voxelError = voxelRepresentationError;
		packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
		packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		packInput.firstCube = firstCube;
		packInput.firstAttribute = firstAttribute;
		PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
		packed.metadata.firstCluster = firstCluster;
		BuildVoxelClustersFromCubes(packed, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
		if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: assembly voxel pack produced no clusters");
		}

		std::vector<ClusterLODGroupSegment> voxelSegments;
		std::vector<BoundingSphere> voxelSegmentBounds;
		SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds, settings.nodeBoneLimit);
		std::vector<std::vector<std::byte>> voxelPageBlobs = BuildVoxelGroupPageBlobs(
			voxelSegments,
			packed.clusterRecords,
			packed.cubeRecords,
			packed.attributeSamples,
			firstAttribute,
			settings.nodeBoneLimit);
		if (voxelSegments.empty() || voxelPageBlobs.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: assembly voxel page build produced no pages");
		}

		ClusterLODGroup group{};
		group.bounds.center[0] = 0.5f * (aabbMin.x + aabbMax.x);
		group.bounds.center[1] = 0.5f * (aabbMin.y + aabbMax.y);
		group.bounds.center[2] = 0.5f * (aabbMin.z + aabbMax.z);
		const float dx = aabbMax.x - group.bounds.center[0];
		const float dy = aabbMax.y - group.bounds.center[1];
		const float dz = aabbMax.z - group.bounds.center[2];
		group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
		group.bounds.error = voxelRepresentationError;
		group.depth = depth;
		group.flags = CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL;
		group.firstSegment = static_cast<uint32_t>(state.segments.size());
		group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
		group.terminalSegmentCount = 0u;
		group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
		group.representationError = voxelRepresentationError;

		const uint32_t groupIndex = appendGroupStorage(group, true);
		for (uint32_t childGroup : childGroups)
		{
			if (childGroup < assemblyCoverageDomainMap.size())
			{
				for (int32_t refinedGroup : assemblyCoverageDomainMap[childGroup])
				{
					appendCoverageDomain(groupIndex, refinedGroup);
				}
			}
		}
		state.segments.insert(state.segments.end(), voxelSegments.begin(), voxelSegments.end());
		state.segmentBounds.insert(state.segmentBounds.end(), voxelSegmentBounds.begin(), voxelSegmentBounds.end());
		state.groupPageBlobs[groupIndex] = std::move(voxelPageBlobs);
		state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
		state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex] = static_cast<int32_t>(state.voxelGroupMapping.packedGroupMetadata.size() - 1u);
		state.voxelGroupMapping.packedClusterRecords.insert(
			state.voxelGroupMapping.packedClusterRecords.end(),
			packed.clusterRecords.begin(),
			packed.clusterRecords.end());
		state.voxelGroupMapping.packedCubeRecords.insert(
			state.voxelGroupMapping.packedCubeRecords.end(),
			packed.cubeRecords.begin(),
			packed.cubeRecords.end());
		state.voxelGroupMapping.packedAttributeSamples.insert(
			state.voxelGroupMapping.packedAttributeSamples.end(),
			packed.attributeSamples.begin(),
			packed.attributeSamples.end());

		assemblyGroupSources[groupIndex].reserve(sourceInstances.size());
		for (VoxelSourcePayloadInstance source : sourceInstances)
		{
			source.refinedGroupOverride = static_cast<int32_t>(groupIndex);
			assemblyGroupSources[groupIndex].push_back(source);
		}

		for (uint32_t childGroup : childGroups)
		{
			state.groups[childGroup].parentGroupId = static_cast<int32_t>(groupIndex);
		}
		return groupIndex;
	};

	std::vector<uint32_t> currentLayer;
	currentLayer.reserve(instances.size());

	DirectX::XMFLOAT3 assemblyInstanceAabbMin(
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max());
	DirectX::XMFLOAT3 assemblyInstanceAabbMax(
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max(),
		-std::numeric_limits<float>::max());
	for (const ClusterLODAssemblyInstanceSpec& spec : instances)
	{
		if (spec.partIndex >= parts.size())
		{
			continue;
		}
		const ClusterLODPrebuiltData& part = parts[spec.partIndex].artifacts->prebuiltData;
		if (spec.rootNode >= part.nodes.size())
		{
			continue;
		}
		const DirectX::XMFLOAT4 sphere = TransformSphere3x4(spec.transform, part.nodes[spec.rootNode].traversalMetric.lodBoundingSphere);
		assemblyInstanceAabbMin.x = std::min(assemblyInstanceAabbMin.x, sphere.x - sphere.w);
		assemblyInstanceAabbMin.y = std::min(assemblyInstanceAabbMin.y, sphere.y - sphere.w);
		assemblyInstanceAabbMin.z = std::min(assemblyInstanceAabbMin.z, sphere.z - sphere.w);
		assemblyInstanceAabbMax.x = std::max(assemblyInstanceAabbMax.x, sphere.x + sphere.w);
		assemblyInstanceAabbMax.y = std::max(assemblyInstanceAabbMax.y, sphere.y + sphere.w);
		assemblyInstanceAabbMax.z = std::max(assemblyInstanceAabbMax.z, sphere.z + sphere.w);
	}
	const float assemblyInstanceLongestExtent = std::max({
		assemblyInstanceAabbMax.x - assemblyInstanceAabbMin.x,
		assemblyInstanceAabbMax.y - assemblyInstanceAabbMin.y,
		assemblyInstanceAabbMax.z - assemblyInstanceAabbMin.z });
	const float assemblyBaselineVoxelWidth =
		std::isfinite(assemblyInstanceLongestExtent) && assemblyInstanceLongestExtent > 0.0f
		? assemblyInstanceLongestExtent / static_cast<float>(std::max(2u, settings.voxelGridBaseResolution))
		: 0.0f;

	auto collectVoxelTailChain = [&](uint32_t globalRootGroupIndex)
	{
		std::vector<uint32_t> chain;
		uint32_t groupIndex = globalRootGroupIndex;
		std::unordered_set<uint32_t> visited;
		while (groupIndex < state.groups.size() && visited.insert(groupIndex).second)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
			{
				break;
			}
			chain.push_back(groupIndex);
			std::vector<uint32_t> children = CollectUniqueRefinedChildren(state, groupIndex);
			if (children.size() != 1u)
			{
				break;
			}
			const uint32_t childGroupIndex = children.front();
			if (childGroupIndex >= state.groups.size() ||
				(state.groups[childGroupIndex].flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
			{
				break;
			}
			groupIndex = childGroupIndex;
		}
		return chain;
	};

	auto selectAssemblySourceGroup = [&](const std::vector<uint32_t>& chain, float instanceScale) -> uint32_t
	{
		if (chain.empty())
		{
			return std::numeric_limits<uint32_t>::max();
		}
		const float growthFactor = std::max(1.01f, settings.voxelTailGrowthFactor);
		const float targetSourceWidth = assemblyBaselineVoxelWidth > 0.0f
			? assemblyBaselineVoxelWidth / growthFactor
			: std::numeric_limits<float>::infinity();
		uint32_t selectedGroup = chain.back();
		for (uint32_t groupIndex : chain)
		{
			const float localWidth = GetFiniteVoxelErrorForGroup(state, groupIndex);
			const float worldWidth = localWidth * instanceScale;
			if (std::isfinite(worldWidth) && worldWidth > 0.0f && worldWidth <= targetSourceWidth)
			{
				selectedGroup = groupIndex;
				break;
			}
		}
		return selectedGroup;
	};

	for (const ClusterLODAssemblyInstanceSpec& spec : instances)
	{
		if (spec.partIndex >= parts.size())
		{
			throw std::runtime_error("ClusterLOD assembly: instance part index out of range");
		}
		const ClusterLODPrebuiltData& part = parts[spec.partIndex].artifacts->prebuiltData;
		if (spec.rootNode >= part.nodes.size())
		{
			throw std::runtime_error("ClusterLOD assembly: instance root node out of range");
		}

		const uint32_t targetRoot = nodeBases[spec.partIndex] + spec.rootNode;
		const ClusterLODNode& targetNode = libraryNodes[targetRoot];
		const uint32_t transformIndex = static_cast<uint32_t>(assemblyTransforms.size());
		assemblyTransforms.push_back(spec.transform);
		ClusterLODAssemblyBoneRemap boneRemap{};
		if (!spec.boneRemapIndices.empty())
		{
			boneRemap.remapIndexBase = static_cast<uint32_t>(assemblyBoneRemapIndices.size());
			boneRemap.remapIndexCount = static_cast<uint32_t>(spec.boneRemapIndices.size());
			assemblyBoneRemapIndices.insert(
				assemblyBoneRemapIndices.end(),
				spec.boneRemapIndices.begin(),
				spec.boneRemapIndices.end());
		}
		else if (spec.boneRemapBase != CLOD_ASSEMBLY_BONE_REMAP_SENTINEL && spec.boneRemapCount != 0u)
		{
			boneRemap.remapIndexBase = spec.boneRemapBase;
			boneRemap.remapIndexCount = spec.boneRemapCount;
		}
		assemblyBoneRemaps.push_back(boneRemap);

		ClusterLODAssemblyInstance assemblyInstance{};
		assemblyInstance.targetRootNode = targetRoot;
		assemblyInstance.transformIndex = transformIndex;
		assemblyInstance.flags = spec.flags;
		assemblyInstance.stackDepth = 1u;
		const uint32_t assemblyInstanceIndex = static_cast<uint32_t>(assemblyInstances.size());
		assemblyInstances.push_back(assemblyInstance);

		const float instanceErrorScale = MaxScale3x4(spec.transform);
		float proxyTraversalError = 0.0f;
		if (IsFiniteContentTraversalError(targetNode.traversalMetric.maxQuadricError))
		{
			proxyTraversalError = std::max(proxyTraversalError, targetNode.traversalMetric.maxQuadricError);
		}
		for (const ClusterLODGroup& partGroup : part.groups)
		{
			if (IsFiniteContentTraversalError(partGroup.bounds.error))
			{
				proxyTraversalError = std::max(proxyTraversalError, partGroup.bounds.error);
			}
			if (std::isfinite(partGroup.representationError) && partGroup.representationError > 0.0f)
			{
				proxyTraversalError = std::max(proxyTraversalError, partGroup.representationError);
			}
		}
		proxyTraversalError *= instanceErrorScale;

		const DirectX::XMFLOAT4 lodSphere = TransformSphere3x4(spec.transform, targetNode.traversalMetric.lodBoundingSphere);
		ClusterLODGroup proxyGroup{};
		proxyGroup.bounds.center[0] = lodSphere.x;
		proxyGroup.bounds.center[1] = lodSphere.y;
		proxyGroup.bounds.center[2] = lodSphere.z;
		proxyGroup.bounds.radius = lodSphere.w;
		proxyGroup.bounds.error = proxyTraversalError;
		proxyGroup.depth = 0;
		proxyGroup.flags = CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY;
		proxyGroup.firstMeshlet = assemblyInstanceIndex;
		const uint32_t proxyGroupIndex = appendGroupStorage(proxyGroup, true);
		currentLayer.push_back(proxyGroupIndex);
		appendCoverageDomain(proxyGroupIndex, static_cast<int32_t>(proxyGroupIndex));
		assemblyCoverageInstances.push_back(VoxelSourceTriangleInstance{
			.partIndex = spec.partIndex,
			.localToWorld = spec.transform,
			.refinedGroup = static_cast<int32_t>(proxyGroupIndex),
			.boneRemapIndices = spec.boneRemapIndices });

		uint32_t selectedSourceGroups = 0u;
		float selectedMaxWorldVoxelWidth = 0.0f;
		for (uint32_t localGroupIndex = 0; localGroupIndex < static_cast<uint32_t>(part.groups.size()); ++localGroupIndex)
		{
			const ClusterLODGroup& localGroup = part.groups[localGroupIndex];
			if (localGroup.parentGroupId >= 0)
			{
				continue;
			}
			const uint32_t globalGroupIndex = groupBases[spec.partIndex] + localGroupIndex;
			const std::vector<uint32_t> chain = collectVoxelTailChain(globalGroupIndex);
			const uint32_t selectedGroupIndex = selectAssemblySourceGroup(chain, instanceErrorScale);
			if (selectedGroupIndex == std::numeric_limits<uint32_t>::max())
			{
				continue;
			}
			const VoxelGroupPayload* payload = getOrBuildVoxelPayloadForGroup(selectedGroupIndex);
			if (payload == nullptr)
			{
				continue;
			}
			selectedSourceGroups++;
			selectedMaxWorldVoxelWidth = std::max(selectedMaxWorldVoxelWidth, payload->voxelWidth * instanceErrorScale);
			assemblyGroupSources[proxyGroupIndex].push_back(VoxelSourcePayloadInstance{
				.payload = payload,
				.localToTarget = spec.transform,
				.expansionRadius = GetVoxelCandidateExpansionRadiusForPayload(payload) * MaxScale3x4(spec.transform),
				.refinedGroupOverride = static_cast<int32_t>(proxyGroupIndex) });
		}
		if (selectedSourceGroups != 0u)
		{
			TracyPlot("CLOD.Assembly.SelectedSourceGroups", static_cast<int64_t>(selectedSourceGroups));
			TracyPlot("CLOD.Assembly.SelectedSourceWorldVoxelMicrons", static_cast<int64_t>(selectedMaxWorldVoxelWidth * 1000000.0f));
		}
	}

	if (currentLayer.empty())
	{
		throw std::runtime_error("ClusterLOD assembly: no proxy groups were produced");
	}
	if (synthesizeVoxelParents)
	{
		ZoneScopedN("ClusterLODUtilities::Assembly::BuildCoverageBVH");
		if (assemblyCoverageInstances.empty())
		{
			throw std::runtime_error("ClusterLOD assembly: parent voxel synthesis requires source triangle coverage geometry");
		}
		assemblyCoverageSourceTriangles.BuildInstanced(
			assemblyCoverageParts,
			assemblyCoverageInstances,
			assemblyCoverageDoubleSidedTriangles);
		if (!assemblyCoverageSourceTriangles.IsValid())
		{
			throw std::runtime_error("ClusterLOD assembly: failed to build parent voxel coverage BVH");
		}
		assemblyCoverageSourceTriangles.SetRefinedGroupDomainMap(assemblyCoverageDomainMap);
	}

	uint32_t assemblyDepth = 1u;
	while (synthesizeVoxelParents && currentLayer.size() > 1u)
	{
		std::vector<uint32_t> ordered = currentLayer;
		if (ordered.size() > preferredNodeWidth)
		{
			std::vector<uint32_t> partitioned(ordered.size());
			std::vector<DirectX::XMFLOAT4> spheres;
			spheres.reserve(ordered.size());
			for (uint32_t groupIndex : ordered)
			{
				const ClusterLODGroup& group = state.groups[groupIndex];
				spheres.push_back(DirectX::XMFLOAT4(
					group.bounds.center[0],
					group.bounds.center[1],
					group.bounds.center[2],
					group.bounds.radius));
			}
			meshopt_spatialClusterPoints(
				partitioned.data(),
				&spheres[0].x,
				static_cast<uint32_t>(spheres.size()),
				sizeof(DirectX::XMFLOAT4),
				preferredNodeWidth);
			std::vector<uint32_t> scratch = ordered;
			for (uint32_t i = 0; i < static_cast<uint32_t>(ordered.size()); ++i)
			{
				ordered[i] = scratch[partitioned[i]];
			}
		}

		std::vector<uint32_t> nextLayer;
		const uint32_t groupFanout = ordered.size() <= preferredNodeWidth
			? static_cast<uint32_t>(ordered.size())
			: preferredNodeWidth;
		for (uint32_t begin = 0; begin < static_cast<uint32_t>(ordered.size()); begin += groupFanout)
		{
			const uint32_t childCount = std::min<uint32_t>(groupFanout, static_cast<uint32_t>(ordered.size()) - begin);
			nextLayer.push_back(buildAssemblyVoxelGroup(
				std::span<const uint32_t>(ordered.data() + begin, childCount),
				static_cast<int32_t>(assemblyDepth)));
		}
		currentLayer = std::move(nextLayer);
		++assemblyDepth;
	}

	if (synthesizeVoxelParents && currentLayer.size() == 1u)
	{
		ClusterLODGroup& rootGroup = state.groups[currentLayer.front()];
		if ((rootGroup.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
		{
			rootGroup.bounds.error = std::numeric_limits<float>::max();
		}
	}

	uint32_t assemblyChildBoundaryRewrites = 0u;
	for (uint32_t parentGroupIndex = 0; parentGroupIndex < static_cast<uint32_t>(state.groups.size()); ++parentGroupIndex)
	{
		const ClusterLODGroup& parentGroup = state.groups[parentGroupIndex];
		if ((parentGroup.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) == 0u ||
			!std::isfinite(parentGroup.representationError) ||
			parentGroup.representationError <= 0.0f)
		{
			continue;
		}

		if (parentGroup.firstSegment + parentGroup.segmentCount > state.segments.size())
		{
			continue;
		}

		for (uint32_t segmentOffset = 0; segmentOffset < parentGroup.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[parentGroup.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				continue;
			}

			const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
			if (childGroupIndex >= state.groups.size())
			{
				continue;
			}

			ClusterLODGroup& childGroup = state.groups[childGroupIndex];
			if (IsTerminalErrorSentinel(childGroup.bounds.error))
			{
				continue;
			}

			childGroup.bounds.error = parentGroup.representationError;
			assemblyChildBoundaryRewrites++;
		}
	}
	if (assemblyChildBoundaryRewrites != 0u)
	{
		spdlog::debug(
			"ClusterLOD assembly rewrote {} child traversal boundaries from parent voxel representation errors",
			assemblyChildBoundaryRewrites);
	}

	if (!synthesizeVoxelParents)
	{
		spdlog::debug(
			"ClusterLOD assembly using direct instance-root traversal: parts={} instances={} proxy_groups={}",
			parts.size(),
			instances.size(),
			currentLayer.size());
	}

	uint32_t assemblyParentErrorRaises = 0u;
	bool raisedAssemblyParentError = true;
	while (raisedAssemblyParentError)
	{
		raisedAssemblyParentError = false;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) == 0u ||
				IsTerminalErrorSentinel(group.bounds.error))
			{
				continue;
			}

			float maxChildError = 0.0f;
			bool hasFiniteChild = false;
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex >= state.groups.size())
				{
					continue;
				}

				const float childError = state.groups[childGroupIndex].bounds.error;
				if (IsFiniteContentTraversalError(childError))
				{
					maxChildError = std::max(maxChildError, childError);
					hasFiniteChild = true;
				}
			}

			if (!hasFiniteChild || group.bounds.error > maxChildError)
			{
				continue;
			}

			group.bounds.error = std::nextafter(maxChildError, std::numeric_limits<float>::infinity());
			assemblyParentErrorRaises++;
			raisedAssemblyParentError = true;
		}
	}
	if (assemblyParentErrorRaises != 0u)
	{
		spdlog::debug(
			"ClusterLOD assembly raised {} assembly voxel parent traversal errors to preserve monotonic cuts",
			assemblyParentErrorRaises);
	}

	const uint32_t rootAssemblyGroupEnd = static_cast<uint32_t>(state.groups.size());
	BuildClusterLODTraversalHierarchy(state, preferredNodeWidth);

	const uint32_t libraryNodeBase = static_cast<uint32_t>(state.nodes.size());
	std::vector<ClusterLODPartRecord> partRecords;
	{
		ClusterLODPartRecord rootPart{};
		rootPart.groupBase = rootAssemblyGroupBase;
		rootPart.groupCount = rootAssemblyGroupEnd - rootAssemblyGroupBase;
		rootPart.nodeBase = 0u;
		rootPart.nodeCount = libraryNodeBase;
		rootPart.transformBase = 0u;
		rootPart.transformCount = static_cast<uint32_t>(assemblyTransforms.size());
		rootPart.instanceBase = 0u;
		rootPart.instanceCount = static_cast<uint32_t>(assemblyInstances.size());
		rootPart.rootNode = state.topRootNode;
		rootPart.flags = CLOD_PART_RECORD_FLAG_ROOT;
		partRecords.push_back(rootPart);

		for (ClusterLODPartRecord record : copiedPartRecords)
		{
			record.nodeBase += libraryNodeBase;
			record.rootNode += libraryNodeBase;
			record.flags &= ~CLOD_PART_RECORD_FLAG_ROOT;
			partRecords.push_back(record);
		}
	}
	for (ClusterLODNode node : libraryNodes)
	{
		if (node.range.isGroup == CLOD_NODE_INTERNAL)
		{
			node.range.indexOrOffset += libraryNodeBase;
		}
		state.nodes.push_back(node);
	}
	std::unordered_map<uint32_t, NodeBoneSet> preservedLibraryNodeSets;
	preservedLibraryNodeSets.reserve(static_cast<size_t>(libraryNodeBase) + libraryNodeBoneSets.size());
	for (uint32_t assemblyNodeIndex = 0u; assemblyNodeIndex < libraryNodeBase; ++assemblyNodeIndex)
	{
		preservedLibraryNodeSets.emplace(
			assemblyNodeIndex,
			NodeBoneSet{ {}, CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK });
	}
	for (uint32_t localNodeIndex = 0u; localNodeIndex < static_cast<uint32_t>(libraryNodeBoneSets.size()); ++localNodeIndex)
	{
		preservedLibraryNodeSets.emplace(libraryNodeBase + localNodeIndex, std::move(libraryNodeBoneSets[localNodeIndex]));
	}

	for (ClusterLODAssemblyInstance& instance : assemblyInstances)
	{
		instance.targetRootNode += libraryNodeBase;
	}
	const uint32_t topAssemblyTraversalDepth = ComputeCLodTraversalDepth(state.nodes, state.topRootNode);
	uint32_t maxAssemblyTargetTraversalDepth = 0u;
	for (const ClusterLODAssemblyInstance& instance : assemblyInstances)
	{
		maxAssemblyTargetTraversalDepth = std::max(
			maxAssemblyTargetTraversalDepth,
			ComputeCLodTraversalDepth(state.nodes, instance.targetRootNode));
	}
	state.maxTraversalDepth = topAssemblyTraversalDepth + maxAssemblyTargetTraversalDepth;
	std::vector<ClusterLODNodeSkinningInfo> nodeSkinningInfos;
	std::vector<uint32_t> nodeBoneIndices;
	BuildNodeSkinningSidecar(
		state, settings.nodeBoneLimit, nodeSkinningInfos, nodeBoneIndices, &preservedLibraryNodeSets);

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	{
		ZoneScopedN("ClusterLODUtilities::Assembly::FinalizeMeshWidePagePacking");
		FinalizeMeshWidePagePacking(
			state,
			meshPageBlobs,
			groupPageReferences,
			groupPageReferenceOffsets,
			trianglePageCount,
			voxelPageBase,
			voxelPageCount);
	}
	TracyPlot("CLOD.Build.MeshPages", static_cast<int64_t>(meshPageBlobs.size()));
	TracyPlot("CLOD.Build.TrianglePages", static_cast<int64_t>(trianglePageCount));
	TracyPlot("CLOD.Build.VoxelPages", static_cast<int64_t>(voxelPageCount));

	out.prebuiltData.groups = std::move(state.groups);
	out.prebuiltData.segments = std::move(state.segments);
	out.prebuiltData.segmentBounds = std::move(state.segmentBounds);
	out.prebuiltData.objectBoundingSphere = BuildObjectBoundingSphereFromRootNode(state.nodes, state.topRootNode);
	out.prebuiltData.groupChunks = std::move(state.groupChunks);
	out.prebuiltData.groupPageReferences = std::move(groupPageReferences);
	out.prebuiltData.groupPageReferenceOffsets = std::move(groupPageReferenceOffsets);
	out.prebuiltData.trianglePageCount = trianglePageCount;
	out.prebuiltData.voxelPageBase = voxelPageBase;
	out.prebuiltData.voxelPageCount = voxelPageCount;
	out.prebuiltData.nodes = std::move(state.nodes);
	out.prebuiltData.nodeSkinningInfos = std::move(nodeSkinningInfos);
	out.prebuiltData.nodeBoneIndices = std::move(nodeBoneIndices);
	out.prebuiltData.nodeBoneLimit = std::clamp(settings.nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
	out.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	out.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	out.prebuiltData.assemblyTransforms = std::move(assemblyTransforms);
	out.prebuiltData.assemblyInstances = std::move(assemblyInstances);
	out.prebuiltData.assemblyBoneRemaps = std::move(assemblyBoneRemaps);
	out.prebuiltData.assemblyBoneRemapIndices = std::move(assemblyBoneRemapIndices);
	out.prebuiltData.partRecords = std::move(partRecords);
	out.prebuiltData.rootPartIndex = 0u;
	out.prebuiltData.maxDepth = state.maxDepth;
	out.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	out.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	out.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	out.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);
	std::string representationError;
	if (!ValidateClusterLODPageRepresentations(
		out.prebuiltData,
		&out.cacheBuildData.meshPageBlobs,
		&representationError))
	{
		spdlog::error("ClusterLOD assembly page-representation validation failed: {}", representationError);
		throw std::runtime_error("ClusterLOD assembly page-representation validation failed: " + representationError);
	}
	EmitClusterLODPagePackingTelemetry(
		"assembly",
		out.prebuiltData,
		out.cacheBuildData.meshPageBlobs);

	return out;
}

ClusterLODPrebuildArtifacts BuildClusterLODAssemblyArtifactsPreservingTriangleOnly(
	std::span<const ClusterLODAssemblyPart> parts,
	std::span<const ClusterLODAssemblyInstanceSpec> instances,
	const ClusterLODBuilderSettings& settings,
	uint32_t preferredNodeWidth)
{
	try
	{
		return BuildClusterLODAssemblyArtifacts(parts, instances, settings, preferredNodeWidth, true);
	}
	catch (const std::runtime_error& e)
	{
		if (std::string_view(e.what()).find("child assembly groups have no voxel payload sources") == std::string_view::npos)
		{
			throw;
		}
	}

	spdlog::debug(
		"ClusterLOD assembly has no voxel payload sources; preserving {} instance(s) with direct instance-root traversal.",
		instances.size());
	return BuildClusterLODAssemblyArtifacts(parts, instances, settings, preferredNodeWidth, false);
}
