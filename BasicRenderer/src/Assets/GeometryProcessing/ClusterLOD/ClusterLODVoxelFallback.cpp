#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"

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

namespace clod_detail
{
	struct VoxelFallbackGroupAnalysis
	{
		bool valid = false;
		DirectX::XMFLOAT3 aabbMin{};
		DirectX::XMFLOAT3 aabbMax{};
		float surfaceArea = 0.0f;
		float targetVoxelWidth = 0.0f;
		uint32_t targetResolution = 0;
		uint32_t triangleCount = 0;
		uint32_t sourceVertexCount = 0;
		float voxelBudget = 0.0f;
		uint32_t sourcePrimitiveCountForCubeBudget = 0;
		uint32_t cubeBudget = 0;
	};

	struct VoxelFallbackBuildStats
	{
		uint32_t analyzedGroups = 0;
		uint32_t validGroups = 0;
		uint32_t autoCandidateGroups = 0;
		uint32_t acceptedSeedGroups = 0;
		uint32_t forcedGroups = 0;
		uint32_t propagatedGroups = 0;
		uint32_t generatedPayloads = 0;
		uint32_t generatedCubes = 0;
		uint32_t failedBuilds = 0;
		uint32_t coverageBvhBuilds = 0;
		uint32_t coverageBvhReuses = 0;
		uint64_t sourceCoverageQueries = 0;
		uint64_t sourceCoverageCandidates = 0;
		uint64_t sourceCoverageTests = 0;
		uint64_t sourceCoverageOutOfCell = 0;
		uint64_t analysisUs = 0;
		uint64_t sourceBuildUs = 0;
		uint64_t coverageBvhUs = 0;
		uint64_t voxelizeUs = 0;
		uint64_t packUs = 0;
	};

	struct VoxelFallbackGroupBuildInput
	{
		VoxelFallbackGroupAnalysis analysis{};
		std::vector<std::byte> voxelVertices;
		std::vector<std::byte> voxelSkinningVertices;
		std::vector<uint32_t> voxelTriangleIndices;
		std::vector<int32_t> voxelTriangleRefinedGroupIds;
		std::vector<uint32_t> sourceVoxelGroupIndices;
		uint32_t voxelVertexCount = 0;
		uint32_t sourcePrimitiveCountForCubeBudget = 0;
		bool autoWouldFitBudget = false;
		float autoAcceptanceErrorReference = 0.0f;
	};

	struct VoxelSourcePayloadRef
	{
		const VoxelGroupPayload* payload = nullptr;
		uint32_t budgetCellCount = 0;
		float expansionRadius = 0.0f;
	};

	struct VoxelFallbackResolutionTarget
	{
		float targetVoxelWidth = 0.0f;
		float voxelBudget = 0.0f;
	};

	VoxelFallbackResolutionTarget ComputeVoxelFallbackResolutionTarget(
		float surfaceArea,
		uint32_t sourceVertexCount,
		float scalingFactor)
	{
		const float safeScale = std::isfinite(scalingFactor) && scalingFactor > 1.0e-4f
			? scalingFactor
			: 1.0f;
		const float baseBudget = std::max(1.0f, static_cast<float>(std::max(1u, sourceVertexCount)));

		VoxelFallbackResolutionTarget target{};
		target.voxelBudget = std::max(1.0f, baseBudget / (safeScale * safeScale));
		target.targetVoxelWidth = std::sqrt(std::max(surfaceArea, 1.0e-12f) / baseBudget) * safeScale;
		return target;
	}

	uint32_t ComputeVoxelFallbackCubeBudget(uint32_t sourcePrimitiveCount)
	{
		if (sourcePrimitiveCount == 0u)
		{
			return 0u;
		}

		return std::max(1u, sourcePrimitiveCount / 2u + (sourcePrimitiveCount & 1u));
	}

	const VoxelGroupPayload* GetVoxelRenderPayloadForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex < state.voxelGroupMapping.groupToPayloadIndex.size())
		{
			const int32_t payloadIndex = state.voxelGroupMapping.groupToPayloadIndex[groupIndex];
			if (payloadIndex >= 0 && static_cast<size_t>(payloadIndex) < state.voxelGroupMapping.payloads.size())
			{
				return &state.voxelGroupMapping.payloads[static_cast<size_t>(payloadIndex)];
			}
		}

		return nullptr;
	}

	void ReleaseVoxelGroupPayloadStorage(VoxelGroupPayload& payload)
	{
		payload.resolution = 0u;
		payload.aabbMin = {};
		payload.aabbMax = {};
		payload.voxelWidth = 0.0f;
		std::vector<VoxelCell>().swap(payload.activeCells);
	}

	uint64_t CountLiveCarryPayloadCells(const ClusterLODBuildState& state)
	{
		uint64_t liveCells = 0u;
		for (const VoxelGroupPayload& payload : state.voxelCarryPayloads)
		{
			liveCells += payload.activeCells.size();
		}
		return liveCells;
	}

	bool HasVoxelSourcePayloadForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex < state.voxelCarryPayloads.size() &&
			state.voxelCarryPayloads[groupIndex].voxelWidth > 0.0f &&
			!state.voxelCarryPayloads[groupIndex].activeCells.empty())
		{
			return true;
		}
		return GetVoxelRenderPayloadForGroup(state, groupIndex) != nullptr;
	}

	float GetVoxelCandidateExpansionRadiusForPayload(const VoxelGroupPayload* payload)
	{
		if (payload == nullptr || !std::isfinite(payload->voxelWidth) || payload->voxelWidth <= 0.0f)
		{
			return 0.0f;
		}

		return 0.5f * payload->voxelWidth;
	}

	void AppendVoxelSourcePayloadRefsForGroup(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		std::vector<VoxelSourcePayloadRef>& outPayloads)
	{
		const VoxelGroupPayload* renderPayload = GetVoxelRenderPayloadForGroup(state, groupIndex);
		const uint32_t renderCellCount = renderPayload != nullptr
			? static_cast<uint32_t>(std::min<size_t>(renderPayload->activeCells.size(), std::numeric_limits<uint32_t>::max()))
			: 0u;
		if (groupIndex < state.voxelCarryPayloads.size() &&
			state.voxelCarryPayloads[groupIndex].voxelWidth > 0.0f &&
			!state.voxelCarryPayloads[groupIndex].activeCells.empty())
		{
			const VoxelGroupPayload* carryPayload = &state.voxelCarryPayloads[groupIndex];
			const uint32_t carryCellCount = static_cast<uint32_t>(std::min<size_t>(
				carryPayload->activeCells.size(),
				std::numeric_limits<uint32_t>::max()));
			outPayloads.push_back(VoxelSourcePayloadRef{ carryPayload, carryCellCount, GetVoxelCandidateExpansionRadiusForPayload(carryPayload) });
			return;
		}

		if (renderPayload != nullptr)
		{
			outPayloads.push_back(VoxelSourcePayloadRef{ renderPayload, renderCellCount, GetVoxelCandidateExpansionRadiusForPayload(renderPayload) });
		}
	}

	DirectX::XMFLOAT3 ReadGroupVertexPosition(const std::vector<std::byte>& vertices, size_t vertexStrideBytes, uint32_t vertexIndex)
	{
		DirectX::XMFLOAT3 position{};
		const size_t offset = static_cast<size_t>(vertexIndex) * vertexStrideBytes;
		std::memcpy(&position.x, vertices.data() + offset + MeshVertexLayout::PositionOffset, sizeof(float));
		std::memcpy(&position.y, vertices.data() + offset + MeshVertexLayout::PositionOffset + sizeof(float), sizeof(float));
		std::memcpy(&position.z, vertices.data() + offset + MeshVertexLayout::PositionOffset + sizeof(float) * 2, sizeof(float));
		return position;
	}

	float TriangleArea(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, const DirectX::XMFLOAT3& c)
	{
		const float abx = b.x - a.x;
		const float aby = b.y - a.y;
		const float abz = b.z - a.z;
		const float acx = c.x - a.x;
		const float acy = c.y - a.y;
		const float acz = c.z - a.z;
		const float cx = aby * acz - abz * acy;
		const float cy = abz * acx - abx * acz;
		const float cz = abx * acy - aby * acx;
		return 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
	}

	std::vector<uint32_t> BuildGroupTriangleIndices(
		const std::vector<meshopt_Meshlet>& meshlets,
		const std::vector<uint32_t>& meshletVertices,
		const std::vector<uint8_t>& meshletTriangles,
		uint32_t firstMeshlet,
		uint32_t meshletCount)
	{
		std::vector<uint32_t> triangleIndices;
		if (firstMeshlet >= meshlets.size() || meshletCount == 0u)
		{
			return triangleIndices;
		}

		const uint32_t endMeshlet = std::min<uint32_t>(
			static_cast<uint32_t>(meshlets.size()),
			firstMeshlet + meshletCount);
		for (uint32_t meshletIndex = firstMeshlet; meshletIndex < endMeshlet; ++meshletIndex)
		{
			const meshopt_Meshlet& meshlet = meshlets[meshletIndex];
			triangleIndices.reserve(triangleIndices.size() + static_cast<size_t>(meshlet.triangle_count) * 3ull);
			for (uint32_t triangleIndex = 0; triangleIndex < meshlet.triangle_count; ++triangleIndex)
			{
				const uint32_t triBase = meshlet.triangle_offset + triangleIndex * 3u;
				if (triBase + 2u >= meshletTriangles.size())
				{
					continue;
				}

				const uint32_t localIndex0 = static_cast<uint32_t>(meshletTriangles[triBase + 0u]);
				const uint32_t localIndex1 = static_cast<uint32_t>(meshletTriangles[triBase + 1u]);
				const uint32_t localIndex2 = static_cast<uint32_t>(meshletTriangles[triBase + 2u]);
				if (localIndex0 >= meshlet.vertex_count || localIndex1 >= meshlet.vertex_count || localIndex2 >= meshlet.vertex_count)
				{
					continue;
				}

				const uint32_t vertexBase = meshlet.vertex_offset;
				if (vertexBase + localIndex0 >= meshletVertices.size() ||
					vertexBase + localIndex1 >= meshletVertices.size() ||
					vertexBase + localIndex2 >= meshletVertices.size())
				{
					continue;
				}

				triangleIndices.push_back(meshletVertices[vertexBase + localIndex0]);
				triangleIndices.push_back(meshletVertices[vertexBase + localIndex1]);
				triangleIndices.push_back(meshletVertices[vertexBase + localIndex2]);
			}
		}
		return triangleIndices;
	}

	std::vector<uint32_t> BuildGroupTriangleIndices(
		const std::vector<meshopt_Meshlet>& meshlets,
		const std::vector<uint32_t>& meshletVertices,
		const std::vector<uint8_t>& meshletTriangles)
	{
		return BuildGroupTriangleIndices(
			meshlets,
			meshletVertices,
			meshletTriangles,
			0u,
			static_cast<uint32_t>(meshlets.size()));
	}

	const std::vector<int32_t>* GetGroupMeshletRefinedGroups(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groupMeshletRefinedGroupChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size())
		{
			return nullptr;
		}

		const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[groupIndex];
		if (tags.size() != state.groupMeshletChunks[groupIndex].size())
		{
			return nullptr;
		}

		return &tags;
	}

	VoxelFallbackGroupAnalysis AnalyzeVoxelFallbackGroup(
		uint32_t sourceVertexCount,
		const std::vector<std::byte>& groupVertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& triangleIndices,
		const ClusterLODBuilderSettings& settings)
	{
		VoxelFallbackGroupAnalysis analysis{};
		analysis.triangleCount = static_cast<uint32_t>(triangleIndices.size() / 3u);
		analysis.sourceVertexCount = sourceVertexCount;
		if (analysis.triangleCount == 0u || groupVertices.empty() || vertexStrideBytes < sizeof(float) * 3u)
		{
			return analysis;
		}

		DirectX::XMFLOAT3 aabbMin(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		DirectX::XMFLOAT3 aabbMax(
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());

		float surfaceArea = 0.0f;
		for (uint32_t index : triangleIndices)
		{
			if (index >= sourceVertexCount)
			{
				continue;
			}

			const DirectX::XMFLOAT3 position = ReadGroupVertexPosition(groupVertices, vertexStrideBytes, index);
			aabbMin.x = std::min(aabbMin.x, position.x);
			aabbMin.y = std::min(aabbMin.y, position.y);
			aabbMin.z = std::min(aabbMin.z, position.z);
			aabbMax.x = std::max(aabbMax.x, position.x);
			aabbMax.y = std::max(aabbMax.y, position.y);
			aabbMax.z = std::max(aabbMax.z, position.z);
		}

		for (size_t triangleBase = 0; triangleBase + 2ull < triangleIndices.size(); triangleBase += 3ull)
		{
			const uint32_t i0 = triangleIndices[triangleBase + 0ull];
			const uint32_t i1 = triangleIndices[triangleBase + 1ull];
			const uint32_t i2 = triangleIndices[triangleBase + 2ull];
			if (i0 >= sourceVertexCount || i1 >= sourceVertexCount || i2 >= sourceVertexCount)
			{
				continue;
			}

			surfaceArea += TriangleArea(
				ReadGroupVertexPosition(groupVertices, vertexStrideBytes, i0),
				ReadGroupVertexPosition(groupVertices, vertexStrideBytes, i1),
				ReadGroupVertexPosition(groupVertices, vertexStrideBytes, i2));
		}

		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (longestExtent <= 1.0e-8f || !std::isfinite(longestExtent))
		{
			return analysis;
		}

		const float minVoxelizationThickness = std::max(longestExtent * 1.0e-4f, 1.0e-5f);
		auto padDegenerateAxis = [minVoxelizationThickness](float& minValue, float& maxValue)
		{
			if (maxValue - minValue > minVoxelizationThickness)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * minVoxelizationThickness;
			maxValue = center + 0.5f * minVoxelizationThickness;
		};
		padDegenerateAxis(aabbMin.x, aabbMax.x);
		padDegenerateAxis(aabbMin.y, aabbMax.y);
		padDegenerateAxis(aabbMin.z, aabbMax.z);

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

		// Runtime voxel reconstruction only stores one scalar voxel width, so the
		// offline voxel volume must be cubic to keep build-time rasterization and
		// runtime cube placement in the same space.
		expandAxisToExtent(aabbMin.x, aabbMax.x, longestExtent);
		expandAxisToExtent(aabbMin.y, aabbMax.y, longestExtent);
		expandAxisToExtent(aabbMin.z, aabbMax.z, longestExtent);

		if (surfaceArea <= 1.0e-12f || !std::isfinite(surfaceArea))
		{
			const float paddedExtentX = aabbMax.x - aabbMin.x;
			const float paddedExtentY = aabbMax.y - aabbMin.y;
			const float paddedExtentZ = aabbMax.z - aabbMin.z;
			surfaceArea = 2.0f * (paddedExtentX * paddedExtentY + paddedExtentX * paddedExtentZ + paddedExtentY * paddedExtentZ);
		}

		const VoxelFallbackResolutionTarget resolutionTarget = ComputeVoxelFallbackResolutionTarget(
			surfaceArea,
			analysis.sourceVertexCount,
			settings.voxelFallbackScalingFactor);
		const float voxelBudget = resolutionTarget.voxelBudget;
		float targetVoxelWidth = resolutionTarget.targetVoxelWidth;
		if (!std::isfinite(targetVoxelWidth) || targetVoxelWidth <= 1.0e-8f)
		{
			targetVoxelWidth = longestExtent / static_cast<float>(std::max(1u, settings.voxelGridBaseResolution));
		}

		const uint32_t minResolution = std::max(2u, settings.voxelMinResolution);
		const uint32_t targetResolution = std::max(
			minResolution,
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(targetVoxelWidth, 1.0e-8f))));

		analysis.valid = targetResolution >= minResolution;
		analysis.aabbMin = aabbMin;
		analysis.aabbMax = aabbMax;
		analysis.surfaceArea = surfaceArea;
		analysis.targetVoxelWidth = targetVoxelWidth;
		analysis.targetResolution = targetResolution;
		analysis.voxelBudget = voxelBudget;
		analysis.sourcePrimitiveCountForCubeBudget = analysis.triangleCount;
		analysis.cubeBudget = ComputeVoxelFallbackCubeBudget(analysis.sourcePrimitiveCountForCubeBudget);
		return analysis;
	}

	VoxelFallbackGroupAnalysis AnalyzeVoxelFallbackBuildInput(
		const ClusterLODBuildState& state,
		const VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		const ClusterLODBuilderSettings& settings)
	{
		VoxelFallbackGroupAnalysis analysis{};
		DirectX::XMFLOAT3 aabbMin(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		DirectX::XMFLOAT3 aabbMax(
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest());
		float surfaceArea = 0.0f;
		uint32_t sourceVertexCount = buildInput.voxelVertexCount;
		uint32_t triangleCount = static_cast<uint32_t>(buildInput.voxelTriangleIndices.size() / 3u);
		bool hasBounds = false;

		for (uint32_t index : buildInput.voxelTriangleIndices)
		{
			if (index >= buildInput.voxelVertexCount)
			{
				continue;
			}

			const DirectX::XMFLOAT3 position = ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, index);
			aabbMin.x = std::min(aabbMin.x, position.x);
			aabbMin.y = std::min(aabbMin.y, position.y);
			aabbMin.z = std::min(aabbMin.z, position.z);
			aabbMax.x = std::max(aabbMax.x, position.x);
			aabbMax.y = std::max(aabbMax.y, position.y);
			aabbMax.z = std::max(aabbMax.z, position.z);
			hasBounds = true;
		}

		for (size_t triangleBase = 0; triangleBase + 2ull < buildInput.voxelTriangleIndices.size(); triangleBase += 3ull)
		{
			const uint32_t i0 = buildInput.voxelTriangleIndices[triangleBase + 0ull];
			const uint32_t i1 = buildInput.voxelTriangleIndices[triangleBase + 1ull];
			const uint32_t i2 = buildInput.voxelTriangleIndices[triangleBase + 2ull];
			if (i0 >= buildInput.voxelVertexCount || i1 >= buildInput.voxelVertexCount || i2 >= buildInput.voxelVertexCount)
			{
				continue;
			}

			surfaceArea += TriangleArea(
				ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, i0),
				ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, i1),
				ReadGroupVertexPosition(buildInput.voxelVertices, vertexStrideBytes, i2));
		}

		for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
		{
			std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
			AppendVoxelSourcePayloadRefsForGroup(state, sourceVoxelGroupIndex, sourcePayloadRefs);
			for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
			{
				const VoxelGroupPayload* payload = payloadRef.payload;
				if (payload == nullptr)
				{
					continue;
				}

				if (payload->voxelWidth <= 0.0f)
				{
					continue;
				}

				if (payloadRef.budgetCellCount > 0u)
				{
					sourceVertexCount += std::min(payloadRef.budgetCellCount, std::numeric_limits<uint32_t>::max() - sourceVertexCount);
					triangleCount += std::min(payloadRef.budgetCellCount, std::numeric_limits<uint32_t>::max() - triangleCount);
				}
				else
				{
					triangleCount += payload->activeCells.empty() ? 0u : 1u;
				}

				const float expandedCellWidth = payload->voxelWidth + 2.0f * std::max(0.0f, payloadRef.expansionRadius);
				const float cellArea = 6.0f * expandedCellWidth * expandedCellWidth;
				for (const VoxelCell& cell : payload->activeCells)
				{
					const float x0 = payload->aabbMin.x + static_cast<float>(cell.x) * payload->voxelWidth - payloadRef.expansionRadius;
					const float y0 = payload->aabbMin.y + static_cast<float>(cell.y) * payload->voxelWidth - payloadRef.expansionRadius;
					const float z0 = payload->aabbMin.z + static_cast<float>(cell.z) * payload->voxelWidth - payloadRef.expansionRadius;
					const float x1 = x0 + expandedCellWidth;
					const float y1 = y0 + expandedCellWidth;
					const float z1 = z0 + expandedCellWidth;
					aabbMin.x = std::min(aabbMin.x, x0);
					aabbMin.y = std::min(aabbMin.y, y0);
					aabbMin.z = std::min(aabbMin.z, z0);
					aabbMax.x = std::max(aabbMax.x, x1);
					aabbMax.y = std::max(aabbMax.y, y1);
					aabbMax.z = std::max(aabbMax.z, z1);
					surfaceArea += cellArea * std::clamp(cell.opacity, 0.0f, 1.0f);
					hasBounds = true;
				}
			}
		}

		analysis.triangleCount = triangleCount;
		analysis.sourceVertexCount = sourceVertexCount;
		if (!hasBounds || triangleCount == 0u || sourceVertexCount == 0u)
		{
			return analysis;
		}

		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (longestExtent <= 1.0e-8f || !std::isfinite(longestExtent))
		{
			return analysis;
		}

		const float minVoxelizationThickness = std::max(longestExtent * 1.0e-4f, 1.0e-5f);
		auto padDegenerateAxis = [minVoxelizationThickness](float& minValue, float& maxValue)
		{
			if (maxValue - minValue > minVoxelizationThickness)
			{
				return;
			}

			const float center = 0.5f * (minValue + maxValue);
			minValue = center - 0.5f * minVoxelizationThickness;
			maxValue = center + 0.5f * minVoxelizationThickness;
		};
		padDegenerateAxis(aabbMin.x, aabbMax.x);
		padDegenerateAxis(aabbMin.y, aabbMax.y);
		padDegenerateAxis(aabbMin.z, aabbMax.z);

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

		if (surfaceArea <= 1.0e-12f || !std::isfinite(surfaceArea))
		{
			const float paddedExtentX = aabbMax.x - aabbMin.x;
			const float paddedExtentY = aabbMax.y - aabbMin.y;
			const float paddedExtentZ = aabbMax.z - aabbMin.z;
			surfaceArea = 2.0f * (paddedExtentX * paddedExtentY + paddedExtentX * paddedExtentZ + paddedExtentY * paddedExtentZ);
		}

		const VoxelFallbackResolutionTarget resolutionTarget = ComputeVoxelFallbackResolutionTarget(
			surfaceArea,
			sourceVertexCount,
			settings.voxelFallbackScalingFactor);
		const float voxelBudget = resolutionTarget.voxelBudget;
		float targetVoxelWidth = resolutionTarget.targetVoxelWidth;
		if (!std::isfinite(targetVoxelWidth) || targetVoxelWidth <= 1.0e-8f)
		{
			targetVoxelWidth = longestExtent / static_cast<float>(std::max(1u, settings.voxelGridBaseResolution));
		}

		const uint32_t minResolution = std::max(2u, settings.voxelMinResolution);
		const uint32_t targetResolution = std::max(
			minResolution,
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(targetVoxelWidth, 1.0e-8f))));

		analysis.valid = targetResolution >= minResolution;
		analysis.aabbMin = aabbMin;
		analysis.aabbMax = aabbMax;
		analysis.surfaceArea = surfaceArea;
		analysis.targetVoxelWidth = targetVoxelWidth;
		analysis.targetResolution = targetResolution;
		analysis.voxelBudget = voxelBudget;
		analysis.sourcePrimitiveCountForCubeBudget = buildInput.sourcePrimitiveCountForCubeBudget != 0u
			? buildInput.sourcePrimitiveCountForCubeBudget
			: triangleCount;
		analysis.cubeBudget = ComputeVoxelFallbackCubeBudget(analysis.sourcePrimitiveCountForCubeBudget);
		return analysis;
	}

	bool AppendGroupTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag = -1)
	{
		if (groupIndex >= state.groups.size() ||
			groupIndex >= state.groupVertexChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size() ||
			groupIndex >= state.groupMeshletVertexChunks.size() ||
			groupIndex >= state.groupMeshletTriangleChunks.size())
		{
			return false;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		const uint32_t vertexBase = buildInput.voxelVertexCount;
		const std::vector<std::byte>& vertices = state.groupVertexChunks[groupIndex];
		buildInput.voxelVertices.insert(buildInput.voxelVertices.end(), vertices.begin(), vertices.end());
		if (groupIndex < state.groupSkinningChunks.size())
		{
			const std::vector<std::byte>& skinning = state.groupSkinningChunks[groupIndex];
			buildInput.voxelSkinningVertices.insert(
				buildInput.voxelSkinningVertices.end(),
				skinning.begin(),
				skinning.end());
		}
		const uint32_t sourceVertexCount = vertexStrideBytes > 0u
			? static_cast<uint32_t>(std::min<size_t>(vertices.size() / vertexStrideBytes, std::numeric_limits<uint32_t>::max()))
			: group.groupVertexCount;
		buildInput.voxelVertexCount += sourceVertexCount;

		std::vector<uint32_t> triangles = BuildGroupTriangleIndices(
			state.groupMeshletChunks[groupIndex],
			state.groupMeshletVertexChunks[groupIndex],
			state.groupMeshletTriangleChunks[groupIndex]);
		buildInput.voxelTriangleIndices.reserve(buildInput.voxelTriangleIndices.size() + triangles.size());
		buildInput.voxelTriangleRefinedGroupIds.reserve(buildInput.voxelTriangleRefinedGroupIds.size() + triangles.size() / 3ull);
		for (uint32_t index : triangles)
		{
			buildInput.voxelTriangleIndices.push_back(vertexBase + index);
		}
		for (size_t triangleIndex = 0; triangleIndex < triangles.size() / 3ull; ++triangleIndex)
		{
			buildInput.voxelTriangleRefinedGroupIds.push_back(refinedGroupTag);
		}

		return true;
	}

	uint32_t ComputeGroupSegmentFirstMeshlet(const ClusterLODBuildState& state, const ClusterLODGroup& group, const ClusterLODGroupSegment& segment)
	{
		uint32_t firstMeshlet = 0u;
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& candidate = state.segments[group.firstSegment + segmentOffset];
			if (candidate.pageIndex < segment.pageIndex)
			{
				firstMeshlet += candidate.meshletCount;
			}
		}
		return firstMeshlet + segment.firstMeshletInPage;
	}

	bool AppendGroupSegmentTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const ClusterLODGroupSegment& segment,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag)
	{
		if (groupIndex >= state.groups.size() ||
			groupIndex >= state.groupVertexChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size() ||
			groupIndex >= state.groupMeshletVertexChunks.size() ||
			groupIndex >= state.groupMeshletTriangleChunks.size())
		{
			return false;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		const uint32_t vertexBase = buildInput.voxelVertexCount;
		const std::vector<std::byte>& vertices = state.groupVertexChunks[groupIndex];
		buildInput.voxelVertices.insert(buildInput.voxelVertices.end(), vertices.begin(), vertices.end());
		if (groupIndex < state.groupSkinningChunks.size())
		{
			const std::vector<std::byte>& skinning = state.groupSkinningChunks[groupIndex];
			buildInput.voxelSkinningVertices.insert(
				buildInput.voxelSkinningVertices.end(),
				skinning.begin(),
				skinning.end());
		}
		const uint32_t sourceVertexCount = vertexStrideBytes > 0u
			? static_cast<uint32_t>(std::min<size_t>(vertices.size() / vertexStrideBytes, std::numeric_limits<uint32_t>::max()))
			: group.groupVertexCount;
		buildInput.voxelVertexCount += sourceVertexCount;

		const uint32_t firstMeshlet = ComputeGroupSegmentFirstMeshlet(state, group, segment);
		std::vector<uint32_t> triangles = BuildGroupTriangleIndices(
			state.groupMeshletChunks[groupIndex],
			state.groupMeshletVertexChunks[groupIndex],
			state.groupMeshletTriangleChunks[groupIndex],
			firstMeshlet,
			segment.meshletCount);
		buildInput.voxelTriangleIndices.reserve(buildInput.voxelTriangleIndices.size() + triangles.size());
		buildInput.voxelTriangleRefinedGroupIds.reserve(buildInput.voxelTriangleRefinedGroupIds.size() + triangles.size() / 3ull);
		for (uint32_t index : triangles)
		{
			buildInput.voxelTriangleIndices.push_back(vertexBase + index);
		}
		for (size_t triangleIndex = 0; triangleIndex < triangles.size() / 3ull; ++triangleIndex)
		{
			buildInput.voxelTriangleRefinedGroupIds.push_back(refinedGroupTag);
		}

		return true;
	}

	bool AppendTerminalSegmentSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag)
	{
		if (groupIndex >= state.groups.size() ||
			groupIndex >= state.groupVertexChunks.size() ||
			groupIndex >= state.groupMeshletChunks.size() ||
			groupIndex >= state.groupMeshletVertexChunks.size() ||
			groupIndex >= state.groupMeshletTriangleChunks.size())
		{
			return false;
		}

		if (const std::vector<int32_t>* meshletRefinedGroups = GetGroupMeshletRefinedGroups(state, groupIndex))
		{
			bool hasTerminalMeshlet = false;
			for (int32_t refinedGroup : *meshletRefinedGroups)
			{
				if (refinedGroup < 0)
				{
					hasTerminalMeshlet = true;
					break;
				}
			}
			if (!hasTerminalMeshlet)
			{
				return true;
			}

			const ClusterLODGroup& group = state.groups[groupIndex];
			const uint32_t vertexBase = buildInput.voxelVertexCount;
			const std::vector<std::byte>& vertices = state.groupVertexChunks[groupIndex];
			buildInput.voxelVertices.insert(buildInput.voxelVertices.end(), vertices.begin(), vertices.end());
			if (groupIndex < state.groupSkinningChunks.size())
			{
				const std::vector<std::byte>& skinning = state.groupSkinningChunks[groupIndex];
				buildInput.voxelSkinningVertices.insert(
					buildInput.voxelSkinningVertices.end(),
					skinning.begin(),
					skinning.end());
			}

			const uint32_t sourceVertexCount = vertexStrideBytes > 0u
				? static_cast<uint32_t>(std::min<size_t>(vertices.size() / vertexStrideBytes, std::numeric_limits<uint32_t>::max()))
				: group.groupVertexCount;
			buildInput.voxelVertexCount += sourceVertexCount;

			for (uint32_t meshletIndex = 0; meshletIndex < static_cast<uint32_t>(meshletRefinedGroups->size()); ++meshletIndex)
			{
				if ((*meshletRefinedGroups)[meshletIndex] >= 0)
				{
					continue;
				}

				std::vector<uint32_t> triangles = BuildGroupTriangleIndices(
					state.groupMeshletChunks[groupIndex],
					state.groupMeshletVertexChunks[groupIndex],
					state.groupMeshletTriangleChunks[groupIndex],
					meshletIndex,
					1u);
				buildInput.voxelTriangleIndices.reserve(buildInput.voxelTriangleIndices.size() + triangles.size());
				buildInput.voxelTriangleRefinedGroupIds.reserve(buildInput.voxelTriangleRefinedGroupIds.size() + triangles.size() / 3ull);
				for (uint32_t index : triangles)
				{
					buildInput.voxelTriangleIndices.push_back(vertexBase + index);
				}
				for (size_t triangleIndex = 0; triangleIndex < triangles.size() / 3ull; ++triangleIndex)
				{
					buildInput.voxelTriangleRefinedGroupIds.push_back(refinedGroupTag);
				}
			}

			return true;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup >= 0)
			{
				continue;
			}

			if (!AppendGroupSegmentTriangleSourceGeometry(state, groupIndex, segment, buildInput, vertexStrideBytes, refinedGroupTag))
			{
				return false;
			}
		}

		return true;
	}

	bool AppendDescendantTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		VoxelFallbackGroupBuildInput& buildInput,
		std::unordered_set<uint32_t>& visitedGroups,
		size_t vertexStrideBytes,
		int32_t refinedGroupTag)
	{
		if (groupIndex >= state.groups.size())
		{
			return false;
		}

		if (!visitedGroups.insert(groupIndex).second)
		{
			return true;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		std::vector<uint32_t> refinedChildren;
		refinedChildren.reserve(group.segmentCount);
		std::unordered_set<uint32_t> seenChildren;
		seenChildren.reserve(group.segmentCount);
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				continue;
			}

			const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
			if (childGroupIndex < state.groups.size() && seenChildren.insert(childGroupIndex).second)
			{
				refinedChildren.push_back(childGroupIndex);
			}
		}

		if (refinedChildren.empty())
		{
			const bool appended = AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag);
			visitedGroups.erase(groupIndex);
			return appended;
		}

		if (!AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag))
		{
			visitedGroups.erase(groupIndex);
			return false;
		}

		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (!AppendDescendantTriangleSourceGeometry(state, childGroupIndex, buildInput, visitedGroups, vertexStrideBytes, refinedGroupTag))
			{
				visitedGroups.erase(groupIndex);
				return false;
			}
		}

		visitedGroups.erase(groupIndex);
		return true;
	}

	uint32_t GetVoxelPackedCubeCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedMetadataIndex.size())
		{
			return 0u;
		}

		const int32_t metadataIndex = state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= state.voxelGroupMapping.packedGroupMetadata.size())
		{
			return 0u;
		}

		return state.voxelGroupMapping.packedGroupMetadata[static_cast<size_t>(metadataIndex)].cubeCount;
	}

	uint32_t GetVoxelSourcePrimitiveCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		uint32_t sourceCellCount = 0u;
		std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
		AppendVoxelSourcePayloadRefsForGroup(state, groupIndex, sourcePayloadRefs);
		for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
		{
			sourceCellCount += std::min(
				payloadRef.budgetCellCount,
				std::numeric_limits<uint32_t>::max() - sourceCellCount);
		}

		if (sourceCellCount != 0u)
		{
			return sourceCellCount;
		}

		const VoxelGroupPayload* renderPayload = GetVoxelRenderPayloadForGroup(state, groupIndex);
		if (renderPayload != nullptr && !renderPayload->activeCells.empty())
		{
			return static_cast<uint32_t>(std::min<size_t>(
				renderPayload->activeCells.size(),
				std::numeric_limits<uint32_t>::max()));
		}

		return GetVoxelPackedCubeCountForGroup(state, groupIndex);
	}

	uint32_t CountGroupMeshTriangles(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groupMeshletChunks.size())
		{
			return 0u;
		}

		uint64_t triangleCount = 0u;
		for (const meshopt_Meshlet& meshlet : state.groupMeshletChunks[groupIndex])
		{
			triangleCount += meshlet.triangle_count;
		}

		return static_cast<uint32_t>(std::min<uint64_t>(triangleCount, std::numeric_limits<uint32_t>::max()));
	}

	uint32_t ComputeVoxelFallbackSourcePrimitiveCount(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const std::vector<uint32_t>& refinedChildren)
	{
		if (refinedChildren.empty())
		{
			return CountGroupMeshTriangles(state, groupIndex);
		}

		uint64_t sourcePrimitiveCount = 0u;
		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (childGroupIndex >= state.groups.size())
			{
				continue;
			}

			if (HasVoxelSourcePayloadForGroup(state, childGroupIndex))
			{
				const uint32_t voxelPrimitiveCount = GetVoxelSourcePrimitiveCountForGroup(state, childGroupIndex);
				if (voxelPrimitiveCount != 0u)
				{
					sourcePrimitiveCount += voxelPrimitiveCount;
					continue;
				}
			}

			sourcePrimitiveCount += CountGroupMeshTriangles(state, childGroupIndex);
		}

		return static_cast<uint32_t>(std::min<uint64_t>(sourcePrimitiveCount, std::numeric_limits<uint32_t>::max()));
	}

	uint32_t GetVoxelPackedClusterCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedMetadataIndex.size())
		{
			return 0u;
		}

		const int32_t metadataIndex = state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= state.voxelGroupMapping.packedGroupMetadata.size())
		{
			return 0u;
		}

		return state.voxelGroupMapping.packedGroupMetadata[static_cast<size_t>(metadataIndex)].clusterCount;
	}

	float GetVoxelMetadataErrorForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedMetadataIndex.size())
		{
			return 0.0f;
		}

		const int32_t metadataIndex = state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= state.voxelGroupMapping.packedGroupMetadata.size())
		{
			return 0.0f;
		}

		return state.voxelGroupMapping.packedGroupMetadata[static_cast<size_t>(metadataIndex)].aabbMaxAndError.w;
	}

	float GetMaxSourceVoxelWidthForBuildInput(
		const ClusterLODBuildState& state,
		const VoxelFallbackGroupBuildInput& buildInput)
	{
		float maxSourceVoxelWidth = 0.0f;
		for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
		{
			std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
			AppendVoxelSourcePayloadRefsForGroup(state, sourceVoxelGroupIndex, sourcePayloadRefs);
			for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
			{
				const VoxelGroupPayload* payload = payloadRef.payload;
				if (payload != nullptr && std::isfinite(payload->voxelWidth) && payload->voxelWidth > 0.0f)
				{
					maxSourceVoxelWidth = std::max(maxSourceVoxelWidth, payload->voxelWidth);
				}
			}
		}
		return maxSourceVoxelWidth;
	}

	float ComputeVoxelRepresentationError(float voxelWidth)
	{
		if (!std::isfinite(voxelWidth) || voxelWidth <= 0.0f)
		{
			return 0.0f;
		}

		return voxelWidth;
	}

	uint64_t PackVoxelTailCellKey(uint32_t x, uint32_t y, uint32_t z)
	{
		return uint64_t{ x } | (uint64_t{ y } << 21u) | (uint64_t{ z } << 42u);
	}

	VoxelGroupPayload DownsampleVoxelPayloadDirect(
		const VoxelGroupPayload& sourcePayload,
		float voxelWidth,
		uint32_t resolution,
		const DirectX::XMFLOAT3& aabbMin,
		int32_t refinedGroup)
	{
		VoxelGroupPayload result{};
		if (sourcePayload.activeCells.empty() ||
			sourcePayload.voxelWidth <= 0.0f ||
			voxelWidth <= 0.0f ||
			resolution < 2u)
		{
			return result;
		}

		result.resolution = resolution;
		result.aabbMin = aabbMin;
		result.aabbMax = DirectX::XMFLOAT3(
			aabbMin.x + voxelWidth * static_cast<float>(resolution),
			aabbMin.y + voxelWidth * static_cast<float>(resolution),
			aabbMin.z + voxelWidth * static_cast<float>(resolution));
		result.voxelWidth = voxelWidth;
		result.uvDensity = sourcePayload.uvDensity;

		struct DownsampleCellAccum
		{
			VoxelCell cell{};
			br::mesh::sggx::SymmetricMatrix3 sggxSum{};
			float sggxWeight = 0.0f;
		};

		std::unordered_map<uint64_t, DownsampleCellAccum> cells;
		cells.reserve(sourcePayload.activeCells.size());
		for (const VoxelCell& sourceCell : sourcePayload.activeCells)
		{
			const float centerX = sourcePayload.aabbMin.x + (static_cast<float>(sourceCell.x) + 0.5f) * sourcePayload.voxelWidth;
			const float centerY = sourcePayload.aabbMin.y + (static_cast<float>(sourceCell.y) + 0.5f) * sourcePayload.voxelWidth;
			const float centerZ = sourcePayload.aabbMin.z + (static_cast<float>(sourceCell.z) + 0.5f) * sourcePayload.voxelWidth;
			const auto cellCoord = [&](float value, float minValue) -> uint32_t
			{
				const float local = (value - minValue) / voxelWidth;
				const int32_t coord = static_cast<int32_t>(std::floor(local));
				return static_cast<uint32_t>(std::clamp<int32_t>(coord, 0, static_cast<int32_t>(resolution) - 1));
			};
			const uint32_t x = cellCoord(centerX, aabbMin.x);
			const uint32_t y = cellCoord(centerY, aabbMin.y);
			const uint32_t z = cellCoord(centerZ, aabbMin.z);
			const uint64_t key = PackVoxelTailCellKey(x, y, z);

			DownsampleCellAccum& accum = cells[key];
			VoxelCell& dst = accum.cell;
			const float sourceWeight = std::max(sourceCell.opacity, 1.0e-6f);
			accum.sggxSum = accum.sggxSum + br::mesh::sggx::DecodeAxialSGGX(sourceCell.sggxAxisAndSigmas) * sourceWeight;
			accum.sggxWeight += sourceWeight;

			if (dst.opacity <= sourceCell.opacity)
			{
				dst = sourceCell;
				dst.x = x;
				dst.y = y;
				dst.z = z;
				dst.refinedGroup = refinedGroup;
			}
			else
			{
				dst.opacity = std::min(1.0f, dst.opacity + sourceCell.opacity);
			}
		}

		result.activeCells.reserve(cells.size());
		for (auto& [key, accum] : cells)
		{
			if (accum.sggxWeight > 1.0e-12f)
			{
				const br::mesh::sggx::SymmetricMatrix3 sggx = accum.sggxSum * (1.0f / accum.sggxWeight);
				accum.cell.sggxAxisAndSigmas = br::mesh::sggx::EncodeAxialSGGX(br::mesh::sggx::CompressSGGXToAxial(sggx));
			}
			result.activeCells.push_back(accum.cell);
		}
		return result;
	}

	struct AppendedVoxelGroupResult
	{
		uint32_t groupIndex = std::numeric_limits<uint32_t>::max();
		uint32_t cubeCount = 0u;
		uint32_t clusterCount = 0u;
		VoxelGroupPayload renderPayload;
	};

	bool AppendPackedVoxelGroupToBuildState(
		ClusterLODBuildState& state,
		VoxelGroupPayload payload,
		float voxelRepresentationError,
		int32_t depth,
		uint32_t flags,
		uint32_t refinedChildGroup,
		const ClusterLODBuilderSettings& settings,
		AppendedVoxelGroupResult& outResult)
	{
		outResult = {};
		outResult.groupIndex = std::numeric_limits<uint32_t>::max();
		if (payload.activeCells.empty() || payload.resolution < 2u || payload.voxelWidth <= 0.0f)
		{
			return false;
		}

		const uint32_t firstCluster = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t firstCube = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
		const uint32_t firstAttribute = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
		PackVoxelGroupInput packInput{};
		packInput.payload = &payload;
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
			return false;
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
			return false;
		}

		ClusterLODGroup group{};
		group.bounds.center[0] = 0.5f * (payload.aabbMin.x + payload.aabbMax.x);
		group.bounds.center[1] = 0.5f * (payload.aabbMin.y + payload.aabbMax.y);
		group.bounds.center[2] = 0.5f * (payload.aabbMin.z + payload.aabbMax.z);
		const float dx = payload.aabbMax.x - group.bounds.center[0];
		const float dy = payload.aabbMax.y - group.bounds.center[1];
		const float dz = payload.aabbMax.z - group.bounds.center[2];
		group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
		group.bounds.error = std::numeric_limits<float>::max();
		group.depth = depth;
		group.flags = flags;
		group.firstSegment = static_cast<uint32_t>(state.segments.size());
		group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
		group.terminalSegmentCount = 0u;
		group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
		group.representationError = voxelRepresentationError;
		for (ClusterLODGroupSegment& segment : voxelSegments)
		{
			segment.refinedGroup = static_cast<int32_t>(refinedChildGroup);
		}

		const uint32_t groupIndex = static_cast<uint32_t>(state.groups.size());
		state.groups.push_back(group);
		state.groupChunks.emplace_back();
		state.groupPageBlobs.push_back(std::move(voxelPageBlobs));
		state.segments.insert(state.segments.end(), voxelSegments.begin(), voxelSegments.end());
		state.segmentBounds.insert(state.segmentBounds.end(), voxelSegmentBounds.begin(), voxelSegmentBounds.end());
		state.voxelCarryPayloads.emplace_back();
		state.voxelGroupMapping.groupToPayloadIndex.push_back(-1);
		state.voxelGroupMapping.groupToPackedMetadataIndex.push_back(static_cast<int32_t>(state.voxelGroupMapping.packedGroupMetadata.size()));
		state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
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
		state.groupVertexChunks.emplace_back();
		state.groupSkinningChunks.emplace_back();
		state.groupMeshletVertexChunks.emplace_back();
		state.groupMeshletChunks.emplace_back();
		state.groupMeshletTriangleChunks.emplace_back();
		state.groupMeshletRefinedGroupChunks.emplace_back();
		if (!state.traversalGroupMask.empty())
		{
			state.traversalGroupMask.push_back(1u);
		}
		if (refinedChildGroup < state.groups.size())
		{
			state.groups[refinedChildGroup].parentGroupId = static_cast<int32_t>(groupIndex);
		}

		outResult.groupIndex = groupIndex;
		outResult.cubeCount = static_cast<uint32_t>(packed.cubeRecords.size());
		outResult.clusterCount = static_cast<uint32_t>(packed.clusterRecords.size());
		outResult.renderPayload = std::move(payload);
		return true;
	}

	float GetFiniteVoxelErrorForGroup(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groups.size())
		{
			return 0.0f;
		}

		const float representationError = state.groups[groupIndex].representationError;
		if (std::isfinite(representationError) && representationError > 0.0f)
		{
			return representationError;
		}

		const float metadataError = GetVoxelMetadataErrorForGroup(state, groupIndex);
		if (std::isfinite(metadataError) && metadataError > 0.0f)
		{
			return metadataError;
		}

		const float groupError = state.groups[groupIndex].bounds.error;
		if (std::isfinite(groupError) && groupError > 0.0f && groupError < std::numeric_limits<float>::max() * 0.5f)
		{
			return groupError;
		}

		std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
		AppendVoxelSourcePayloadRefsForGroup(state, groupIndex, sourcePayloadRefs);
		float maxVoxelWidth = 0.0f;
		for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
		{
			if (payloadRef.payload != nullptr)
			{
				maxVoxelWidth = std::max(maxVoxelWidth, payloadRef.payload->voxelWidth);
			}
		}
		return maxVoxelWidth;
	}

	std::vector<uint32_t> CollectUniqueRefinedChildren(const ClusterLODBuildState& state, uint32_t groupIndex);

	struct RefinedChildErrorRange
	{
		float minError = std::numeric_limits<float>::max();
		float maxError = 0.0f;
		uint32_t count = 0u;
	};

	RefinedChildErrorRange GetRefinedChildTraversalErrorRange(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		RefinedChildErrorRange range{};
		for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, groupIndex))
		{
			if (childGroupIndex < state.groups.size())
			{
				const float childError = state.groups[childGroupIndex].bounds.error;
				if (std::isfinite(childError) && childError > 0.0f && childError < 5.0e19f)
				{
					range.minError = std::min(range.minError, childError);
					range.maxError = std::max(range.maxError, childError);
					range.count++;
				}
			}
		}

		if (range.count == 0u)
		{
			range.minError = 0.0f;
		}
		return range;
	}

	float GetMaxRefinedChildTraversalError(const ClusterLODBuildState& state, uint32_t groupIndex, uint32_t* outCount = nullptr)
	{
		const RefinedChildErrorRange range = GetRefinedChildTraversalErrorRange(state, groupIndex);
		if (outCount != nullptr)
		{
			*outCount = range.count;
		}
		return range.maxError;
	}

	bool IsTerminalErrorSentinel(float error)
	{
		return error >= std::numeric_limits<float>::max() * 0.5f;
	}


	bool IsStructuralTraversalError(float error)
	{
		return std::isfinite(error) &&
			error >= kClusterLODStructuralTraversalError * 0.5f &&
			!IsTerminalErrorSentinel(error);
	}

	bool IsFiniteContentTraversalError(float error)
	{
		return std::isfinite(error) &&
			error > 0.0f &&
			!IsTerminalErrorSentinel(error) &&
			!IsStructuralTraversalError(error);
	}

	float TraversalNodeErrorFromGroupError(float error)
	{
		return IsTerminalErrorSentinel(error) ? kClusterLODStructuralTraversalError : error;
	}

	void LogVoxelTriangleTagHistogram(
		const char* label,
		uint32_t groupIndex,
		int32_t depth,
		const VoxelFallbackGroupBuildInput& buildInput)
	{
		std::unordered_map<int32_t, uint32_t> tagTriangleCounts;
		for (int32_t refinedGroup : buildInput.voxelTriangleRefinedGroupIds)
		{
			tagTriangleCounts[refinedGroup]++;
		}

		std::vector<std::pair<int32_t, uint32_t>> sortedTags(tagTriangleCounts.begin(), tagTriangleCounts.end());
		std::sort(sortedTags.begin(), sortedTags.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.first < rhs.first;
		});

		spdlog::debug(
			"ClusterLOD voxel source histogram: group={} depth={} source={} vertices={} triangles={} source_voxel_groups={} tag_groups={}",
			groupIndex,
			depth,
			label,
			buildInput.voxelVertexCount,
			buildInput.voxelTriangleIndices.size() / 3ull,
			buildInput.sourceVoxelGroupIndices.size(),
			sortedTags.size());

		for (const auto& [refinedGroup, triangleCount] : sortedTags)
		{
			spdlog::debug(
				"ClusterLOD voxel source tag: group={} depth={} source={} refined_group={} triangles={}",
				groupIndex,
				depth,
				label,
				refinedGroup,
				triangleCount);
		}
	}

	void LogVoxelPayloadRefinedGroupCells(
		const char* label,
		uint32_t groupIndex,
		int32_t depth,
		const VoxelGroupPayload& payload)
	{
		std::unordered_map<int32_t, uint32_t> cellCounts;
		for (const VoxelCell& cell : payload.activeCells)
		{
			cellCounts[cell.refinedGroup]++;
		}

		std::vector<std::pair<int32_t, uint32_t>> sortedCounts(cellCounts.begin(), cellCounts.end());
		std::sort(sortedCounts.begin(), sortedCounts.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.first < rhs.first;
		});

		for (const auto& [refinedGroup, cellCount] : sortedCounts)
		{
			spdlog::debug(
				"ClusterLOD voxel payload cells: group={} depth={} payload={} refined_group={} cells={}",
				groupIndex,
				depth,
				label,
				refinedGroup,
				cellCount);
		}
	}

	std::vector<uint32_t> CollectUniqueRefinedChildren(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		std::vector<uint32_t> refinedChildren;
		if (groupIndex >= state.groups.size())
		{
			return refinedChildren;
		}

		if (const std::vector<int32_t>* meshletRefinedGroups = GetGroupMeshletRefinedGroups(state, groupIndex))
		{
			std::unordered_set<uint32_t> seenChildren;
			seenChildren.reserve(meshletRefinedGroups->size());
			for (int32_t refinedGroup : *meshletRefinedGroups)
			{
				if (refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(refinedGroup);
				if (childGroupIndex < state.groups.size() && seenChildren.insert(childGroupIndex).second)
				{
					refinedChildren.push_back(childGroupIndex);
				}
			}
			return refinedChildren;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		refinedChildren.reserve(group.segmentCount);
		std::unordered_set<uint32_t> seenChildren;
		seenChildren.reserve(group.segmentCount);
		for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				continue;
			}

			const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
			if (childGroupIndex < state.groups.size() && seenChildren.insert(childGroupIndex).second)
			{
				refinedChildren.push_back(childGroupIndex);
			}
		}

		return refinedChildren;
	}

	uint64_t CountGroupSegmentTriangles(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const ClusterLODGroupSegment& segment)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupMeshletChunks.size())
		{
			return 0u;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		const uint32_t firstMeshlet = ComputeGroupSegmentFirstMeshlet(state, group, segment);
		const uint32_t endMeshlet = std::min<uint32_t>(
			static_cast<uint32_t>(state.groupMeshletChunks[groupIndex].size()),
			firstMeshlet + segment.meshletCount);
		uint64_t triangleCount = 0u;
		for (uint32_t meshletIndex = firstMeshlet; meshletIndex < endMeshlet; ++meshletIndex)
		{
			triangleCount += state.groupMeshletChunks[groupIndex][meshletIndex].triangle_count;
		}
		return triangleCount;
	}

	uint64_t CountTerminalTriangleSourceGeometry(const ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupMeshletChunks.size())
		{
			return 0u;
		}

		if (const std::vector<int32_t>* meshletRefinedGroups = GetGroupMeshletRefinedGroups(state, groupIndex))
		{
			uint64_t triangleCount = 0u;
			const uint32_t meshletCount = std::min<uint32_t>(
				static_cast<uint32_t>(meshletRefinedGroups->size()),
				static_cast<uint32_t>(state.groupMeshletChunks[groupIndex].size()));
			for (uint32_t meshletIndex = 0u; meshletIndex < meshletCount; ++meshletIndex)
			{
				if ((*meshletRefinedGroups)[meshletIndex] < 0)
				{
					triangleCount += state.groupMeshletChunks[groupIndex][meshletIndex].triangle_count;
				}
			}
			return triangleCount;
		}

		const ClusterLODGroup& group = state.groups[groupIndex];
		if (group.firstSegment + group.segmentCount > state.segments.size())
		{
			return 0u;
		}

		uint64_t triangleCount = 0u;
		for (uint32_t segmentOffset = 0u; segmentOffset < group.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				triangleCount += CountGroupSegmentTriangles(state, groupIndex, segment);
			}
		}
		return triangleCount;
	}

	uint64_t CountDescendantTriangleSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		std::unordered_set<uint32_t>& visitedGroups)
	{
		if (groupIndex >= state.groups.size() || !visitedGroups.insert(groupIndex).second)
		{
			return 0u;
		}

		const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);
		uint64_t triangleCount = refinedChildren.empty()
			? CountGroupMeshTriangles(state, groupIndex)
			: CountTerminalTriangleSourceGeometry(state, groupIndex);
		for (uint32_t childGroupIndex : refinedChildren)
		{
			triangleCount += CountDescendantTriangleSourceGeometry(state, childGroupIndex, visitedGroups);
		}

		visitedGroups.erase(groupIndex);
		return triangleCount;
	}

	bool IsGroupReachableFromGroup(
		const ClusterLODBuildState& state,
		uint32_t targetGroupIndex,
		uint32_t rootGroupIndex)
	{
		if (targetGroupIndex == rootGroupIndex)
		{
			return true;
		}

		if (rootGroupIndex >= state.groups.size())
		{
			return false;
		}

		std::vector<uint32_t> stack;
		std::unordered_set<uint32_t> visitedGroups;
		stack.push_back(rootGroupIndex);
		while (!stack.empty())
		{
			const uint32_t currentGroupIndex = stack.back();
			stack.pop_back();
			if (!visitedGroups.insert(currentGroupIndex).second || currentGroupIndex >= state.groups.size())
			{
				continue;
			}

			const ClusterLODGroup& group = state.groups[currentGroupIndex];
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex == targetGroupIndex)
				{
					return true;
				}

				stack.push_back(childGroupIndex);
			}
		}

		return false;
	}

	int32_t ResolveVoxelSectionSuppressionRefinedGroup(
		const ClusterLODBuildState& state,
		uint32_t ownerGroupIndex,
		int32_t sectionRefinedGroup)
	{
		if (sectionRefinedGroup < 0 || ownerGroupIndex >= state.groups.size())
		{
			return -1;
		}

		const uint32_t sectionGroupIndex = static_cast<uint32_t>(sectionRefinedGroup);
		const ClusterLODGroup& ownerGroup = state.groups[ownerGroupIndex];
		for (uint32_t segmentOffset = 0; segmentOffset < ownerGroup.segmentCount; ++segmentOffset)
		{
			const ClusterLODGroupSegment& segment = state.segments[ownerGroup.firstSegment + segmentOffset];
			if (segment.refinedGroup < 0)
			{
				continue;
			}

			const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
			if (sectionGroupIndex == childGroupIndex || IsGroupReachableFromGroup(state, sectionGroupIndex, childGroupIndex))
			{
				return segment.refinedGroup;
			}
		}

		return sectionRefinedGroup;
	}

	void ClearTerminalSentinelForVoxelGroup(ClusterLODBuildState& state, uint32_t groupIndex)
	{
		if (groupIndex >= state.groups.size() || (state.groups[groupIndex].flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
		{
			return;
		}

		if (!IsTerminalErrorSentinel(state.groups[groupIndex].bounds.error))
		{
			return;
		}

		const float finiteError = GetFiniteVoxelErrorForGroup(state, groupIndex);
		if (std::isfinite(finiteError) && finiteError > 0.0f)
		{
			state.groups[groupIndex].bounds.error = finiteError;
		}
	}

	bool BuildVoxelFallbackSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		size_t vertexStrideBytes,
		VoxelFallbackGroupBuildInput& buildInput,
		const std::vector<uint8_t>* requiredVoxelSourceMask = nullptr)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupVertexChunks.size())
		{
			return false;
		}

		const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);

		if (refinedChildren.empty())
		{
			buildInput.voxelVertices.clear();
			buildInput.voxelSkinningVertices.clear();
			buildInput.voxelTriangleIndices.clear();
			buildInput.voxelTriangleRefinedGroupIds.clear();
			buildInput.sourceVoxelGroupIndices.clear();
			buildInput.voxelVertexCount = 0;
			buildInput.sourcePrimitiveCountForCubeBudget = ComputeVoxelFallbackSourcePrimitiveCount(state, groupIndex, refinedChildren);
			return AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes);
		}

		buildInput.voxelVertices.clear();
		buildInput.voxelSkinningVertices.clear();
		buildInput.voxelTriangleIndices.clear();
		buildInput.voxelTriangleRefinedGroupIds.clear();
		buildInput.sourceVoxelGroupIndices.clear();
		buildInput.voxelVertexCount = 0;
		buildInput.sourcePrimitiveCountForCubeBudget = ComputeVoxelFallbackSourcePrimitiveCount(state, groupIndex, refinedChildren);
		if (!AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, -1))
		{
			return false;
		}

		// Voxel sections preserve the same DAG cut contract as triangle sections:
		// each emitted section is tagged by the immediate refined child. Built
		// child voxel payloads are kept alive until the fallback pass completes,
		// so this triangle fallback should only be used for non-voxelized paths.
		for (uint32_t childGroupIndex : refinedChildren)
		{
			if (HasVoxelSourcePayloadForGroup(state, childGroupIndex))
			{
				buildInput.sourceVoxelGroupIndices.push_back(childGroupIndex);
				continue;
			}

			if (requiredVoxelSourceMask != nullptr &&
				childGroupIndex < requiredVoxelSourceMask->size() &&
				(*requiredVoxelSourceMask)[childGroupIndex] != 0u)
			{
				const ClusterLODGroup& childGroup = state.groups[childGroupIndex];
				const uint64_t carryCells = childGroupIndex < state.voxelCarryPayloads.size()
					? static_cast<uint64_t>(state.voxelCarryPayloads[childGroupIndex].activeCells.size())
					: 0u;
				const VoxelGroupPayload* renderPayload = GetVoxelRenderPayloadForGroup(state, childGroupIndex);
				spdlog::error(
					"ClusterLOD voxel source missing required child payload: parent={} parent_depth={} child={} child_depth={} child_flags=0x{:X} child_segments={} child_terminal_segments={} carry_cells={} render_cells={} build_required=1",
					groupIndex,
					std::max(state.groups[groupIndex].depth, 0),
					childGroupIndex,
					std::max(childGroup.depth, 0),
					childGroup.flags,
					childGroup.segmentCount,
					childGroup.terminalSegmentCount,
					carryCells,
					renderPayload != nullptr ? renderPayload->activeCells.size() : 0ull);
				return false;
			}

			if (!AppendGroupTriangleSourceGeometry(state, childGroupIndex, buildInput, vertexStrideBytes, static_cast<int32_t>(childGroupIndex)))
			{
				return false;
			}
		}

		return (!buildInput.voxelVertices.empty() && !buildInput.voxelTriangleIndices.empty()) || !buildInput.sourceVoxelGroupIndices.empty();
	}

	bool BuildVoxelFallbackCoverageSourceGeometry(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		size_t vertexStrideBytes,
		VoxelFallbackGroupBuildInput& buildInput)
	{
		if (groupIndex >= state.groups.size() || groupIndex >= state.groupVertexChunks.size())
		{
			return false;
		}

		buildInput.voxelVertices.clear();
		buildInput.voxelSkinningVertices.clear();
		buildInput.voxelTriangleIndices.clear();
		buildInput.voxelTriangleRefinedGroupIds.clear();
		buildInput.sourceVoxelGroupIndices.clear();
		buildInput.voxelVertexCount = 0;
		buildInput.sourcePrimitiveCountForCubeBudget = 0;

		const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);

		if (refinedChildren.empty())
		{
			return AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes);
		}

		if (!AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, -1))
		{
			return false;
		}

		// Coverage pruning must be evaluated against the original descendant
		// source geometry whenever possible.  Re-sampling already-pruned voxel
		// opacity between levels compounds partial coverage loss, especially
		// when parent and child voxel grids have similar cell sizes but different
		// origins.  Candidate ownership can still come from child voxel payloads;
		// this BVH is transient and only answers coverage rays.
		for (uint32_t childGroupIndex : refinedChildren)
		{
			std::unordered_set<uint32_t> appendVisitedGroups;
			if (!AppendDescendantTriangleSourceGeometry(
				state,
				childGroupIndex,
				buildInput,
				appendVisitedGroups,
				vertexStrideBytes,
				static_cast<int32_t>(childGroupIndex)))
			{
				return false;
			}
		}

		return !buildInput.voxelVertices.empty() && !buildInput.voxelTriangleIndices.empty();
	}

	void CollectCoverageDomainGroups(
		const ClusterLODBuildState& state,
		uint32_t groupIndex,
		const std::vector<uint8_t>& buildVoxelGroupMask,
		std::unordered_set<uint32_t>& visitedGroups,
		std::vector<int32_t>& outDomainGroups)
	{
		if (groupIndex >= state.groups.size() || !visitedGroups.insert(groupIndex).second)
		{
			return;
		}

		if (groupIndex < buildVoxelGroupMask.size() && buildVoxelGroupMask[groupIndex] != 0u)
		{
			outDomainGroups.push_back(static_cast<int32_t>(groupIndex));
		}
		for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, groupIndex))
		{
			CollectCoverageDomainGroups(state, childGroupIndex, buildVoxelGroupMask, visitedGroups, outDomainGroups);
		}
	}

	std::vector<std::vector<int32_t>> BuildVoxelCoverageDomainMap(
		const ClusterLODBuildState& state,
		const std::vector<uint8_t>& buildVoxelGroupMask)
	{
		std::vector<std::vector<int32_t>> domainMap(state.groups.size());
		for (uint32_t groupIndex = 0u; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			if (groupIndex >= buildVoxelGroupMask.size() || buildVoxelGroupMask[groupIndex] == 0u)
			{
				continue;
			}

			std::unordered_set<uint32_t> visitedGroups;
			CollectCoverageDomainGroups(state, groupIndex, buildVoxelGroupMask, visitedGroups, domainMap[groupIndex]);
		}
		return domainMap;
	}

	bool ComputePayloadActiveCellAabb(
		const VoxelGroupPayload& payload,
		DirectX::XMFLOAT3& outMin,
		DirectX::XMFLOAT3& outMax)
	{
		if (payload.activeCells.empty() || payload.voxelWidth <= 0.0f)
		{
			return false;
		}

		outMin = DirectX::XMFLOAT3(
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max());
		outMax = DirectX::XMFLOAT3(
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max(),
			-std::numeric_limits<float>::max());
		for (const VoxelCell& cell : payload.activeCells)
		{
			const float minX = payload.aabbMin.x + static_cast<float>(cell.x) * payload.voxelWidth;
			const float minY = payload.aabbMin.y + static_cast<float>(cell.y) * payload.voxelWidth;
			const float minZ = payload.aabbMin.z + static_cast<float>(cell.z) * payload.voxelWidth;
			const float maxX = minX + payload.voxelWidth;
			const float maxY = minY + payload.voxelWidth;
			const float maxZ = minZ + payload.voxelWidth;
			outMin.x = std::min(outMin.x, minX);
			outMin.y = std::min(outMin.y, minY);
			outMin.z = std::min(outMin.z, minZ);
			outMax.x = std::max(outMax.x, maxX);
			outMax.y = std::max(outMax.y, maxY);
			outMax.z = std::max(outMax.z, maxZ);
		}
		return
			std::isfinite(outMin.x) && std::isfinite(outMin.y) && std::isfinite(outMin.z) &&
			std::isfinite(outMax.x) && std::isfinite(outMax.y) && std::isfinite(outMax.z) &&
			outMax.x > outMin.x && outMax.y > outMin.y && outMax.z > outMin.z;
	}

	bool BuildPartVoxelTailLevel(
		ClusterLODBuildState& state,
		uint32_t childGroupIndex,
		const VoxelGroupPayload& sourcePayload,
		const VoxelSourceTriangleBVH* coverageSourceTriangles,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler,
		const ClusterLODBuilderSettings& settings,
		AppendedVoxelGroupResult& outResult)
	{
		outResult = {};
		outResult.groupIndex = std::numeric_limits<uint32_t>::max();
		if (childGroupIndex >= state.groups.size() ||
			sourcePayload.activeCells.empty() ||
			sourcePayload.voxelWidth <= 0.0f)
		{
			return false;
		}

		const float growthFactor = std::max(1.01f, settings.voxelTailGrowthFactor);
		const float voxelWidth = sourcePayload.voxelWidth * growthFactor;
		DirectX::XMFLOAT3 aabbMin{};
		DirectX::XMFLOAT3 aabbMax{};
		if (!ComputePayloadActiveCellAabb(sourcePayload, aabbMin, aabbMax))
		{
			return false;
		}

		const float extentX = aabbMax.x - aabbMin.x;
		const float extentY = aabbMax.y - aabbMin.y;
		const float extentZ = aabbMax.z - aabbMin.z;
		const float longestExtent = std::max({ extentX, extentY, extentZ });
		if (!std::isfinite(longestExtent) || longestExtent <= 1.0e-8f)
		{
			return false;
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

		const uint32_t resolution = std::max(
			std::max(2u, settings.voxelMinResolution),
			static_cast<uint32_t>(std::ceil(longestExtent / std::max(voxelWidth, 1.0e-8f))));
		if (resolution < 2u)
		{
			return false;
		}

		const int32_t refinedChildGroup = static_cast<int32_t>(childGroupIndex);
		VoxelGroupPayload payload{};
		if (coverageSourceTriangles != nullptr && coverageSourceTriangles->IsValid())
		{
			std::vector<VoxelSourcePayloadInstance> sourceInstances;
			sourceInstances.push_back(VoxelSourcePayloadInstance{
				.payload = &sourcePayload,
				.expansionRadius = GetVoxelCandidateExpansionRadiusForPayload(&sourcePayload),
				.refinedGroupOverride = refinedChildGroup });
			VoxelizeTrianglesInput voxelInput{};
			voxelInput.sourceVoxelPayloadInstances = &sourceInstances;
			voxelInput.candidateVoxelPayloadInstances = &sourceInstances;
			voxelInput.coverageSourceTriangles = coverageSourceTriangles;
			voxelInput.coverageMaterialSampler = coverageMaterialSampler;
			voxelInput.aabbMin = aabbMin;
			voxelInput.aabbMax = aabbMax;
			voxelInput.voxelWidth = voxelWidth;
			voxelInput.resolution = resolution;
			voxelInput.raysPerCell = std::max(1u, settings.voxelRaysPerCell);
			voxelInput.emitSourcePayload = false;
			voxelInput.emitRenderPayload = true;
			payload = std::move(VoxelizeTrianglesDetailed(voxelInput).renderPayload);
		}
		if (payload.activeCells.empty())
		{
			payload = DownsampleVoxelPayloadDirect(sourcePayload, voxelWidth, resolution, aabbMin, refinedChildGroup);
		}
		if (payload.activeCells.empty())
		{
			return false;
		}

		const int32_t tailDepth = std::max(state.groups[childGroupIndex].depth + 1, state.groups[childGroupIndex].depth);
		return AppendPackedVoxelGroupToBuildState(
			state,
			std::move(payload),
			ComputeVoxelRepresentationError(voxelWidth),
			tailDepth,
			CLOD_GROUP_FLAG_IS_VOXEL,
			childGroupIndex,
			settings,
			outResult);
	}

	void BuildPartVoxelTailGroups(
		ClusterLODBuildState& state,
		const VoxelSourceTriangleBVH* coverageSourceTriangles,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler,
		const ClusterLODBuilderSettings& settings)
	{
		ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildPartVoxelTailGroups");
		if (settings.voxelTailMaxLevels == 0u || settings.voxelTailGrowthFactor <= 1.0f)
		{
			return;
		}

		std::vector<uint32_t> parentRefCounts(state.groups.size(), 0u);
		for (const ClusterLODGroup& group : state.groups)
		{
			if (group.firstSegment + group.segmentCount > state.segments.size())
			{
				continue;
			}
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup >= 0 && static_cast<uint32_t>(segment.refinedGroup) < parentRefCounts.size())
				{
					parentRefCounts[static_cast<uint32_t>(segment.refinedGroup)]++;
				}
			}
		}

		std::vector<uint32_t> voxelRoots;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u ||
				(group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u ||
				groupIndex >= parentRefCounts.size() ||
				parentRefCounts[groupIndex] != 0u ||
				GetVoxelPackedCubeCountForGroup(state, groupIndex) == 0u)
			{
				continue;
			}
			voxelRoots.push_back(groupIndex);
		}

		uint32_t generatedLevels = 0u;
		uint32_t generatedCubes = 0u;
		for (uint32_t rootGroupIndex : voxelRoots)
		{
			VoxelGroupPayload sourcePayload;
			if (!BuildVoxelGroupPayloadFromPackedMapping(state.voxelGroupMapping, rootGroupIndex, sourcePayload))
			{
				continue;
			}

			uint32_t childGroupIndex = rootGroupIndex;
			uint32_t previousCubeCount = GetVoxelPackedCubeCountForGroup(state, childGroupIndex);
			for (uint32_t level = 0; level < settings.voxelTailMaxLevels; ++level)
			{
				if (previousCubeCount <= 64u || sourcePayload.activeCells.empty())
				{
					break;
				}

				AppendedVoxelGroupResult tailResult;
				if (!BuildPartVoxelTailLevel(
					state,
					childGroupIndex,
					sourcePayload,
					coverageSourceTriangles,
					coverageMaterialSampler,
					settings,
					tailResult))
				{
					break;
				}

				generatedLevels++;
				generatedCubes += tailResult.cubeCount;
				spdlog::debug(
					"ClusterLOD voxel tail: root={} child={} tail={} level={} voxel_width={} source_cells={} tail_cells={} source_cubes={} tail_cubes={} clusters={}",
					rootGroupIndex,
					childGroupIndex,
					tailResult.groupIndex,
					level,
					tailResult.renderPayload.voxelWidth,
					sourcePayload.activeCells.size(),
					tailResult.renderPayload.activeCells.size(),
					previousCubeCount,
					tailResult.cubeCount,
					tailResult.clusterCount);

				const bool reducedEnough = tailResult.cubeCount * 5u <= previousCubeCount * 4u;
				childGroupIndex = tailResult.groupIndex;
				previousCubeCount = tailResult.cubeCount;
				sourcePayload = std::move(tailResult.renderPayload);
				if (!reducedEnough)
				{
					break;
				}
			}
		}

		TracyPlot("CLOD.VoxelFallback.TailLevels", static_cast<int64_t>(generatedLevels));
		TracyPlot("CLOD.VoxelFallback.TailCubes", static_cast<int64_t>(generatedCubes));
		if (generatedLevels != 0u)
		{
			spdlog::debug(
				"ClusterLOD voxel tail groups: roots={} generated_levels={} generated_cubes={} growth={} max_levels={}",
				voxelRoots.size(),
				generatedLevels,
				generatedCubes,
				settings.voxelTailGrowthFactor,
				settings.voxelTailMaxLevels);
		}
	}

	bool AppendSharedVoxelCoverageSourceGeometry(
		const ClusterLODBuildState& state,
		const std::vector<uint8_t>& buildVoxelGroupMask,
		VoxelFallbackGroupBuildInput& buildInput,
		size_t vertexStrideBytes)
	{
		buildInput.voxelVertices.clear();
		buildInput.voxelSkinningVertices.clear();
		buildInput.voxelTriangleIndices.clear();
		buildInput.voxelTriangleRefinedGroupIds.clear();
		buildInput.sourceVoxelGroupIndices.clear();
		buildInput.voxelVertexCount = 0u;
		buildInput.sourcePrimitiveCountForCubeBudget = 0u;

		for (uint32_t groupIndex = 0u; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			if (groupIndex >= buildVoxelGroupMask.size() || buildVoxelGroupMask[groupIndex] == 0u)
			{
				continue;
			}

			const std::vector<uint32_t> refinedChildren = CollectUniqueRefinedChildren(state, groupIndex);
			const int32_t refinedGroupTag = static_cast<int32_t>(groupIndex);
			const bool appended = refinedChildren.empty()
				? AppendGroupTriangleSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag)
				: AppendTerminalSegmentSourceGeometry(state, groupIndex, buildInput, vertexStrideBytes, refinedGroupTag);
			if (!appended)
			{
				return false;
			}
		}

		return !buildInput.voxelVertices.empty() && !buildInput.voxelTriangleIndices.empty();
	}

	void BuildVoxelFallbackCandidates(
		ClusterLODBuildState& state,
		size_t vertexStrideBytes,
		size_t skinningVertexStrideBytes,
		const VoxelCoverageMaterialSampler* coverageMaterialSampler,
		const ClusterLODBuilderSettings& settings)
	{
		ZoneScopedN("ClusterLODUtilities::BuildVoxelFallbackCandidates");
		const bool enabled = settings.coveragePreservationMode == ClusterLODCoveragePreservationMode::Voxel;
		if (!enabled || state.groups.empty())
		{
			return;
		}

		state.voxelGroupMapping.groupToPayloadIndex.assign(state.groups.size(), -1);
		state.voxelGroupMapping.groupToPackedMetadataIndex.assign(state.groups.size(), -1);
		state.voxelCarryPayloads.assign(state.groups.size(), {});

		VoxelFallbackBuildStats stats{};
		const uint32_t originalGroupCount = static_cast<uint32_t>(state.groups.size());
		TracyPlot("CLOD.VoxelFallback.InputGroups", static_cast<int64_t>(originalGroupCount));
		std::vector<VoxelFallbackGroupBuildInput> groupInputs(originalGroupCount);
		std::vector<float> originalGroupErrors(originalGroupCount, 0.0f);
		uint32_t maxDepth = 0;

		auto finiteVoxelDecisionError = [](float error) -> bool
		{
			return std::isfinite(error) && error > 0.0f && error < std::numeric_limits<float>::max() * 0.5f;
		};
		auto elapsedUsSince = [](std::chrono::steady_clock::time_point start) -> uint64_t
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - start).count());
		};

		for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
		{
			originalGroupErrors[groupIndex] = state.groups[groupIndex].bounds.error;
		}

		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::AnalyzeGroups");
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				stats.analyzedGroups++;
				maxDepth = std::max(maxDepth, static_cast<uint32_t>(std::max(state.groups[groupIndex].depth, 0)));
				if (groupIndex >= state.groupVertexChunks.size() || groupIndex >= state.groupMeshletVertexChunks.size() ||
					groupIndex >= state.groupMeshletChunks.size() || groupIndex >= state.groupMeshletTriangleChunks.size())
				{
					stats.failedBuilds++;
					continue;
				}

				const ClusterLODGroup& group = state.groups[groupIndex];
				VoxelFallbackGroupBuildInput buildInput;
				const auto sourceBuildStart = std::chrono::steady_clock::now();
				if (!BuildVoxelFallbackSourceGeometry(state, groupIndex, vertexStrideBytes, buildInput))
				{
					stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);
					stats.failedBuilds++;
					continue;
				}
				stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);

				const auto analysisStart = std::chrono::steady_clock::now();
				buildInput.analysis = AnalyzeVoxelFallbackBuildInput(state, buildInput, vertexStrideBytes, settings);
				stats.analysisUs += elapsedUsSince(analysisStart);

				if (!buildInput.analysis.valid)
				{
					stats.failedBuilds++;
					continue;
				}

				stats.validGroups++;
				const bool hasOnlyRefinedDomain = group.segmentCount != 0u && group.terminalSegmentCount == 0u;
				const bool hasFiniteTriangleReductionError = finiteVoxelDecisionError(originalGroupErrors[groupIndex]);
				const RefinedChildErrorRange childErrorRange = GetRefinedChildTraversalErrorRange(state, groupIndex);
				buildInput.autoAcceptanceErrorReference = childErrorRange.minError;
				const float targetVoxelRepresentationError = ComputeVoxelRepresentationError(buildInput.analysis.targetVoxelWidth);
				// Auto voxel fallback is allowed only when every emitted voxel
				// segment has a refined child boundary that can suppress it near the
				// camera, and the voxel representation is already no worse than the
				// tightest child-side cut boundary. Terminal sections have no child
				// condition 2 guard, so they remain triangle-only unless voxel mode
				// is forced.
				buildInput.autoWouldFitBudget = hasOnlyRefinedDomain &&
					hasFiniteTriangleReductionError &&
					childErrorRange.count != 0u &&
					finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference) &&
					targetVoxelRepresentationError * std::max(1.0f, settings.voxelFallbackAcceptanceBias) <= buildInput.autoAcceptanceErrorReference;
				if (buildInput.autoWouldFitBudget)
				{
					stats.autoCandidateGroups++;
				}
				groupInputs[groupIndex].analysis = buildInput.analysis;
				groupInputs[groupIndex].autoWouldFitBudget = buildInput.autoWouldFitBudget;
				groupInputs[groupIndex].autoAcceptanceErrorReference = buildInput.autoAcceptanceErrorReference;
				groupInputs[groupIndex].sourcePrimitiveCountForCubeBudget = buildInput.sourcePrimitiveCountForCubeBudget;
			}
		}
		TracyPlot("CLOD.VoxelFallback.ValidGroups", static_cast<int64_t>(stats.validGroups));
		TracyPlot("CLOD.VoxelFallback.AutoCandidateGroups", static_cast<int64_t>(stats.autoCandidateGroups));

		VoxelFallbackGroupBuildInput sharedCoverageBuildInput;
		VoxelSourceTriangleBVH sharedCoverageSourceTriangles;
		const VoxelSourceTriangleBVH* sharedVoxelCoverageSourceTriangles = nullptr;

		auto buildVoxelGroup = [&](uint32_t groupIndex, const std::vector<uint8_t>& requiredVoxelSourceMask, bool requireBudgetFit, bool requireQualityFit, bool forceReplaceGroupWithVoxels) -> bool
		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildVoxelGroup");
			if (groupIndex >= groupInputs.size())
			{
				spdlog::error(
					"ClusterLOD voxel build failed: group={} reason=group_index_out_of_range group_inputs={}",
					groupIndex,
					groupInputs.size());
				return false;
			}

			VoxelFallbackGroupBuildInput buildInput = groupInputs[groupIndex];
			auto keepSourceCarryPayloadsForDagParents = [&]()
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::KeepCarryPayloadsForDagParents");
				TracyPlot("CLOD.VoxelFallback.LiveCarryCells", static_cast<int64_t>(CountLiveCarryPayloadCells(state)));
			};
			const auto sourceBuildStart = std::chrono::steady_clock::now();
			if (!BuildVoxelFallbackSourceGeometry(state, groupIndex, vertexStrideBytes, buildInput, &requiredVoxelSourceMask))
			{
				stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);
				stats.failedBuilds++;
				spdlog::error(
					"ClusterLOD voxel build failed: group={} reason=source_geometry_failed required_sources={} source_tris={} source_voxel_groups={}",
					groupIndex,
					groupIndex < requiredVoxelSourceMask.size() ? static_cast<uint32_t>(requiredVoxelSourceMask[groupIndex]) : 0u,
					buildInput.voxelTriangleIndices.size() / 3ull,
					buildInput.sourceVoxelGroupIndices.size());
				return false;
			}
			stats.sourceBuildUs += elapsedUsSince(sourceBuildStart);

			const auto analysisStart = std::chrono::steady_clock::now();
			buildInput.analysis = AnalyzeVoxelFallbackBuildInput(state, buildInput, vertexStrideBytes, settings);
			stats.analysisUs += elapsedUsSince(analysisStart);

			if (!buildInput.analysis.valid)
			{
				stats.failedBuilds++;
				keepSourceCarryPayloadsForDagParents();
				spdlog::error(
					"ClusterLOD voxel build failed: group={} depth={} reason=invalid_analysis source_tris={} source_voxel_groups={} source_vertices={} target_resolution={} target_voxel_width={} aabb_min=({}, {}, {}) aabb_max=({}, {}, {})",
					groupIndex,
					groupIndex < state.groups.size() ? std::max(state.groups[groupIndex].depth, 0) : 0,
					buildInput.voxelTriangleIndices.size() / 3ull,
					buildInput.sourceVoxelGroupIndices.size(),
					buildInput.voxelVertexCount,
					buildInput.analysis.targetResolution,
					buildInput.analysis.targetVoxelWidth,
					buildInput.analysis.aabbMin.x,
					buildInput.analysis.aabbMin.y,
					buildInput.analysis.aabbMin.z,
					buildInput.analysis.aabbMax.x,
					buildInput.analysis.aabbMax.y,
					buildInput.analysis.aabbMax.z);
				return false;
			}

			ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
			{
				keepSourceCarryPayloadsForDagParents();
				return true;
			}

			VoxelGroupPayload payload{};
			const uint32_t metadataIndex = static_cast<uint32_t>(state.voxelGroupMapping.packedGroupMetadata.size());
			const uint32_t firstCluster = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
			const uint32_t firstCube = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
			const uint32_t firstAttribute = static_cast<uint32_t>(state.voxelGroupMapping.packedAttributeSamples.size());
			uint32_t resolution = buildInput.analysis.targetResolution;
			uint32_t refinedChildErrorCount = 0u;
			const float maxRefinedChildError = GetMaxRefinedChildTraversalError(state, groupIndex, &refinedChildErrorCount);
			const RefinedChildErrorRange childErrorRange = GetRefinedChildTraversalErrorRange(state, groupIndex);
			const float childCutAcceptanceError = childErrorRange.minError;
			const float maxSourceVoxelWidth = GetMaxSourceVoxelWidthForBuildInput(state, buildInput);
			LogVoxelTriangleTagHistogram("candidate", groupIndex, group.depth, buildInput);
			auto resolutionForVoxelWidth = [&buildInput, &settings](float candidateVoxelWidth) -> uint32_t
			{
				const float extentX = buildInput.analysis.aabbMax.x - buildInput.analysis.aabbMin.x;
				const float extentY = buildInput.analysis.aabbMax.y - buildInput.analysis.aabbMin.y;
				const float extentZ = buildInput.analysis.aabbMax.z - buildInput.analysis.aabbMin.z;
				const float longestExtent = std::max({ extentX, extentY, extentZ });
				return std::max(
					std::max(2u, settings.voxelMinResolution),
					static_cast<uint32_t>(std::ceil(longestExtent / std::max(candidateVoxelWidth, 1.0e-8f))));
			};
			float voxelWidth = buildInput.analysis.targetVoxelWidth;
			float inheritedMinVoxelWidth = 0.0f;
			if (maxSourceVoxelWidth > 0.0f)
			{
				const float coarseningFactor = std::max(1.01f, settings.voxelFallbackGrowthFactor);
				inheritedMinVoxelWidth = maxSourceVoxelWidth * coarseningFactor;
				voxelWidth = inheritedMinVoxelWidth;
				resolution = resolutionForVoxelWidth(voxelWidth);
			}
			float maxQualityVoxelWidth = std::numeric_limits<float>::infinity();
			if (requireQualityFit && finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference))
			{
				maxQualityVoxelWidth = buildInput.autoAcceptanceErrorReference / std::max(1.0f, settings.voxelFallbackAcceptanceBias);
				if (std::isfinite(maxQualityVoxelWidth) && maxQualityVoxelWidth > 1.0e-8f && voxelWidth > maxQualityVoxelWidth)
				{
					if (inheritedMinVoxelWidth > 0.0f && maxQualityVoxelWidth < inheritedMinVoxelWidth)
					{
						spdlog::debug(
							"ClusterLOD voxel coarsening limited by quality: group={} depth={} target_voxel_width={} max_source_voxel_width={} inherited_min_voxel_width={} max_quality_voxel_width={} acceptance_error={}",
							groupIndex,
							group.depth,
							buildInput.analysis.targetVoxelWidth,
							maxSourceVoxelWidth,
							inheritedMinVoxelWidth,
							maxQualityVoxelWidth,
							buildInput.autoAcceptanceErrorReference);
					}
					voxelWidth = maxQualityVoxelWidth;
					resolution = resolutionForVoxelWidth(voxelWidth);
				}
			}
			float voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
			bool payloadFitsBudget = false;
			bool acceptedBudgetOverflowForQuality = false;
			PackedVoxelGroupBuildResult packed{};
			float packedVoxelRepresentationError = 0.0f;
			uint32_t lastPositiveCoverageCellCount = 0u;
			uint32_t lastCandidateCellCount = 0u;
			uint32_t lastTriangleCandidateCellCount = 0u;
			uint32_t lastVoxelCandidateCellCount = 0u;
			VoxelFallbackGroupBuildInput qualityTriangleSourceInput;
			bool useQualityTriangleSources = false;
			const bool sourceVoxelsTooCoarseForSeed =
				requireQualityFit &&
				maxSourceVoxelWidth > 0.0f &&
				std::isfinite(voxelWidth) &&
				voxelWidth > 1.0e-8f &&
				voxelWidth < maxSourceVoxelWidth;
			if (sourceVoxelsTooCoarseForSeed &&
				BuildVoxelFallbackCoverageSourceGeometry(state, groupIndex, vertexStrideBytes, qualityTriangleSourceInput) &&
				!qualityTriangleSourceInput.voxelTriangleIndices.empty())
			{
				useQualityTriangleSources = true;
				spdlog::debug(
					"ClusterLOD voxel seed using descendant triangle candidates: group={} depth={} voxel_width={} target_voxel_width={} max_source_voxel_width={} max_quality_voxel_width={} triangles={} vertices={}",
					groupIndex,
					group.depth,
					voxelWidth,
					buildInput.analysis.targetVoxelWidth,
					maxSourceVoxelWidth,
					maxQualityVoxelWidth,
					qualityTriangleSourceInput.voxelTriangleIndices.size() / 3ull,
					qualityTriangleSourceInput.voxelVertexCount);
			}
			auto logBuildFailure = [&](const char* reason)
			{
				spdlog::error(
					"ClusterLOD voxel build failed: group={} depth={} reason={} source_tris={} source_voxel_groups={} target_resolution={} voxel_width={} representation_error={} auto_acceptance_error={} require_budget={} require_quality={} force_replace={} payload_cells={} packed_cubes={} packed_clusters={} payload_fits_budget={} voxel_budget={} cube_budget={} positive_coverage={} candidate_cells={} triangle_candidates={} voxel_candidates={}",
					groupIndex,
					group.depth,
					reason,
					buildInput.voxelTriangleIndices.size() / 3ull,
					buildInput.sourceVoxelGroupIndices.size(),
					resolution,
					voxelWidth,
					voxelRepresentationError,
					buildInput.autoAcceptanceErrorReference,
					requireBudgetFit,
					requireQualityFit,
					forceReplaceGroupWithVoxels,
					payload.activeCells.size(),
					packed.cubeRecords.size(),
					packed.clusterRecords.size(),
					payloadFitsBudget,
					buildInput.analysis.voxelBudget,
					buildInput.analysis.cubeBudget,
					lastPositiveCoverageCellCount,
					lastCandidateCellCount,
					lastTriangleCandidateCellCount,
					lastVoxelCandidateCellCount);
			};
			auto packCurrentPayload = [&]() {
				PackVoxelGroupInput packInput{};
				packInput.payload = &payload;
				packInput.voxelError = voxelRepresentationError;
				packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
				packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
				packInput.firstCube = firstCube;
				packInput.firstAttribute = firstAttribute;
				PackedVoxelGroupBuildResult result = PackVoxelGroupToCubes(packInput);
				result.metadata.firstCluster = firstCluster;
				BuildVoxelClustersFromCubes(result, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
				return result;
			};
			auto computeMaxClusterCubeCount = [](const PackedVoxelGroupBuildResult& result) {
				uint32_t maxClusterCubeCount = 0u;
				for (const CLodVoxelClusterRecord& clusterRecord : result.clusterRecords)
				{
					maxClusterCubeCount = std::max(maxClusterCubeCount, clusterRecord.cubeCount);
				}
				return maxClusterCubeCount;
			};
			const VoxelSourceTriangleBVH* voxelCoverageSourceTriangles = sharedVoxelCoverageSourceTriangles;

			const uint32_t retryCount = std::max(1u, settings.voxelFallbackMaxRetryCount + 1u);
			bool qualityLimitExceeded = false;
			for (uint32_t attempt = 0; attempt < retryCount; ++attempt)
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildVoxelGroup::Attempt");
				if (requireQualityFit && finiteVoxelDecisionError(buildInput.autoAcceptanceErrorReference) &&
					voxelRepresentationError * std::max(1.0f, settings.voxelFallbackAcceptanceBias) > buildInput.autoAcceptanceErrorReference)
				{
					if (!qualityLimitExceeded)
					{
						qualityLimitExceeded = true;
						spdlog::warn(
							"ClusterLOD voxel build quality limit exceeded; continuing mandatory build: group={} depth={} attempt={} voxel_width={} representation_error={} acceptance_error={} acceptance_bias={} require_quality={} require_budget={}",
							groupIndex,
							group.depth,
							attempt,
							voxelWidth,
							voxelRepresentationError,
							buildInput.autoAcceptanceErrorReference,
							settings.voxelFallbackAcceptanceBias,
							requireQualityFit,
							requireBudgetFit);
					}
				}

				std::vector<VoxelSourcePayloadInstance> sourceVoxelPayloadInstances;
				std::vector<VoxelSourcePayloadInstance> candidateVoxelPayloadInstances;
				if (!useQualityTriangleSources && !buildInput.sourceVoxelGroupIndices.empty())
				{
					sourceVoxelPayloadInstances.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					candidateVoxelPayloadInstances.reserve(buildInput.sourceVoxelGroupIndices.size() * 2ull);
					for (uint32_t sourceVoxelGroupIndex : buildInput.sourceVoxelGroupIndices)
					{
						std::vector<VoxelSourcePayloadRef> sourcePayloadRefs;
						AppendVoxelSourcePayloadRefsForGroup(state, sourceVoxelGroupIndex, sourcePayloadRefs);
						if (sourcePayloadRefs.empty())
						{
							spdlog::error(
								"ClusterLOD voxel source refs empty: group={} depth={} source_group={} source_depth={} source_flags=0x{:X} source_required={} carry_cells={} render_payload_present={}",
								groupIndex,
								group.depth,
								sourceVoxelGroupIndex,
								sourceVoxelGroupIndex < state.groups.size() ? std::max(state.groups[sourceVoxelGroupIndex].depth, 0) : 0,
								sourceVoxelGroupIndex < state.groups.size() ? state.groups[sourceVoxelGroupIndex].flags : 0u,
								sourceVoxelGroupIndex < requiredVoxelSourceMask.size() ? static_cast<uint32_t>(requiredVoxelSourceMask[sourceVoxelGroupIndex]) : 0u,
								sourceVoxelGroupIndex < state.voxelCarryPayloads.size() ? state.voxelCarryPayloads[sourceVoxelGroupIndex].activeCells.size() : 0ull,
								GetVoxelRenderPayloadForGroup(state, sourceVoxelGroupIndex) != nullptr);
						}
						for (const VoxelSourcePayloadRef& payloadRef : sourcePayloadRefs)
						{
							if (payloadRef.payload != nullptr)
							{
								sourceVoxelPayloadInstances.push_back(VoxelSourcePayloadInstance{
									.payload = payloadRef.payload,
									.expansionRadius = 0.0f,
									.refinedGroupOverride = static_cast<int32_t>(sourceVoxelGroupIndex) });
								candidateVoxelPayloadInstances.push_back(VoxelSourcePayloadInstance{
									.payload = payloadRef.payload,
									.expansionRadius = payloadRef.expansionRadius,
									.refinedGroupOverride = static_cast<int32_t>(sourceVoxelGroupIndex) });
							}
						}
					}
				}

				const VoxelFallbackGroupBuildInput& voxelSourceInput = useQualityTriangleSources
					? qualityTriangleSourceInput
					: buildInput;
				VoxelizeTrianglesInput voxelInput{};
				voxelInput.vertices = voxelSourceInput.voxelVertices.empty() ? nullptr : &voxelSourceInput.voxelVertices;
				voxelInput.vertexStrideBytes = vertexStrideBytes;
				voxelInput.skinningVertices = voxelSourceInput.voxelSkinningVertices.empty() ? nullptr : &voxelSourceInput.voxelSkinningVertices;
				voxelInput.skinningVertexStrideBytes = skinningVertexStrideBytes;
				voxelInput.triangleIndices = voxelSourceInput.voxelTriangleIndices.empty() ? nullptr : &voxelSourceInput.voxelTriangleIndices;
				voxelInput.triangleRefinedGroupIds = voxelSourceInput.voxelTriangleRefinedGroupIds.empty() ? nullptr : &voxelSourceInput.voxelTriangleRefinedGroupIds;
				voxelInput.doubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
				voxelInput.coverageSourceTriangles = voxelCoverageSourceTriangles;
				voxelInput.coverageMaterialSampler = coverageMaterialSampler;
				voxelInput.terminalCoverageRefinedGroupOverride = static_cast<int32_t>(groupIndex);
				voxelInput.sourceVoxelPayloadInstances = sourceVoxelPayloadInstances.empty() ? nullptr : &sourceVoxelPayloadInstances;
				voxelInput.candidateVoxelPayloadInstances = candidateVoxelPayloadInstances.empty() ? nullptr : &candidateVoxelPayloadInstances;
				voxelInput.aabbMin = buildInput.analysis.aabbMin;
				voxelInput.aabbMax = buildInput.analysis.aabbMax;
				voxelInput.voxelWidth = voxelWidth;
				voxelInput.resolution = resolution;
				voxelInput.raysPerCell = settings.voxelRaysPerCell;
				const auto voxelizeStart = std::chrono::steady_clock::now();
				VoxelizeTrianglesResult voxelResult = VoxelizeTrianglesDetailed(voxelInput);
				stats.voxelizeUs += elapsedUsSince(voxelizeStart);
				lastPositiveCoverageCellCount = voxelResult.positiveCoverageCellCount;
				lastCandidateCellCount = voxelResult.candidateCellCount;
				lastTriangleCandidateCellCount = voxelResult.triangleCandidateCellCount;
				lastVoxelCandidateCellCount = voxelResult.voxelCandidateCellCount;
				{
					ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseVoxelSourceInstances");
					sourceVoxelPayloadInstances.clear();
					candidateVoxelPayloadInstances.clear();
				}
				stats.sourceCoverageQueries += voxelResult.sourceCoverageQueryCount;
				stats.sourceCoverageCandidates += voxelResult.sourceCoverageTriangleCandidateCount;
				stats.sourceCoverageTests += voxelResult.sourceCoverageTriangleTestCount;
				stats.sourceCoverageOutOfCell += voxelResult.sourceCoverageOutOfCellRejectionCount;
				spdlog::debug(
					"ClusterLOD voxel build detail: group={} depth={} attempt={} resolution={} voxel_width={} target_voxel_width={} representation_error={} child_cut_acceptance_error={} max_source_voxel_width={} inherited_min_voxel_width={} max_quality_voxel_width={} source_tris={} source_voxel_groups={} coverage_source_tris={} coverage_source_vertices={} source_primitives={} cube_budget={} tri_candidates={} voxel_candidates={} candidates={} positive_cells={} total_coverage={} max_coverage={} source_cells={} render_cells={} pruned={} source_coverage_queries={} source_coverage_candidates={} source_coverage_tests={} source_coverage_out_of_cell={}",
					groupIndex,
					group.depth,
					attempt,
					resolution,
					voxelWidth,
					buildInput.analysis.targetVoxelWidth,
					voxelRepresentationError,
					childCutAcceptanceError,
					maxSourceVoxelWidth,
					inheritedMinVoxelWidth,
					maxQualityVoxelWidth,
					voxelSourceInput.voxelTriangleIndices.size() / 3ull,
					useQualityTriangleSources ? 0ull : buildInput.sourceVoxelGroupIndices.size(),
					sharedCoverageBuildInput.voxelTriangleIndices.size() / 3ull,
					sharedCoverageBuildInput.voxelVertexCount,
					buildInput.analysis.sourcePrimitiveCountForCubeBudget,
					buildInput.analysis.cubeBudget,
					voxelResult.triangleCandidateCellCount,
					voxelResult.voxelCandidateCellCount,
					voxelResult.candidateCellCount,
					voxelResult.positiveCoverageCellCount,
					voxelResult.totalCoverage,
					voxelResult.maxCoverage,
					voxelResult.sourcePayload.activeCells.size(),
					voxelResult.renderPayload.activeCells.size(),
					voxelResult.prunedCellCount,
					voxelResult.sourceCoverageQueryCount,
					voxelResult.sourceCoverageTriangleCandidateCount,
					voxelResult.sourceCoverageTriangleTestCount,
					voxelResult.sourceCoverageOutOfCellRejectionCount);
				for (const VoxelizeTrianglesResult::RefinedGroupStats& stats : voxelResult.refinedGroupStats)
				{
					spdlog::debug(
						"ClusterLOD voxel refined detail: group={} depth={} attempt={} refined_group={} candidates={} tri_owned={} candidate_owned={} candidate_only={} positive={} zero_dropped={} emitted={} total_coverage={} max_coverage={}",
						groupIndex,
						group.depth,
						attempt,
						stats.refinedGroup,
						stats.candidateKeys,
						stats.triangleOwnedCells,
						stats.candidateOwnedCells,
						stats.candidateOnlyCells,
						stats.positiveCoverageCells,
						stats.zeroCoverageDroppedCells,
						stats.emittedSourceCells,
						stats.totalCoverage,
						stats.maxCoverage);
				}
				payload = std::move(voxelResult.renderPayload);
				state.voxelCarryPayloads[groupIndex] = std::move(voxelResult.sourcePayload);
				LogVoxelPayloadRefinedGroupCells("render", groupIndex, group.depth, payload);
				LogVoxelPayloadRefinedGroupCells("carry", groupIndex, group.depth, state.voxelCarryPayloads[groupIndex]);

				if (!payload.activeCells.empty())
				{
					const auto packStart = std::chrono::steady_clock::now();
					{
						ZoneScopedN("ClusterLODUtilities::VoxelFallback::PackAttempt");
						packed = packCurrentPayload();
					}
					stats.packUs += elapsedUsSince(packStart);
					packedVoxelRepresentationError = voxelRepresentationError;
				}

				const bool cellCountFits = !payload.activeCells.empty() &&
					static_cast<float>(payload.activeCells.size()) <= buildInput.analysis.voxelBudget;
				const bool cubeCountFits = !packed.cubeRecords.empty() &&
					(buildInput.analysis.cubeBudget == 0u ||
						static_cast<uint64_t>(packed.cubeRecords.size()) <= static_cast<uint64_t>(buildInput.analysis.cubeBudget));
				payloadFitsBudget = cellCountFits && cubeCountFits;
				const bool hasPackedOutput = !payload.activeCells.empty() &&
					!packed.cubeRecords.empty() &&
					!packed.clusterRecords.empty();
				spdlog::debug(
					"ClusterLOD voxel pack attempt: group={} depth={} attempt={} payload_cells={} voxel_budget={} source_primitives={} cube_budget={} packed_cubes={} packed_clusters={} max_cluster_cube_count={} cells_fit={} cubes_fit={}",
					groupIndex,
					group.depth,
					attempt,
					payload.activeCells.size(),
					buildInput.analysis.voxelBudget,
					buildInput.analysis.sourcePrimitiveCountForCubeBudget,
					buildInput.analysis.cubeBudget,
					packed.cubeRecords.size(),
					packed.clusterRecords.size(),
					computeMaxClusterCubeCount(packed),
					cellCountFits,
					cubeCountFits);
				if (payloadFitsBudget || (!requireBudgetFit && hasPackedOutput))
				{
					break;
				}

				const float nextVoxelWidth = voxelWidth * std::max(1.01f, settings.voxelFallbackGrowthFactor);
				const bool nextAttemptExceedsQuality = requireQualityFit &&
					std::isfinite(maxQualityVoxelWidth) &&
					maxQualityVoxelWidth > 1.0e-8f &&
					nextVoxelWidth > maxQualityVoxelWidth;
				const bool exhaustedRetries = attempt + 1u >= retryCount;
				const bool preserveQualityOverCellBudget = requireQualityFit &&
					hasPackedOutput &&
					(nextAttemptExceedsQuality || (exhaustedRetries && cubeCountFits));
				if (preserveQualityOverCellBudget)
				{
					acceptedBudgetOverflowForQuality = true;
					spdlog::debug(
						"ClusterLOD voxel build preserving quality over cell budget: group={} depth={} attempt={} reason={} voxel_width={} acceptance_error={} payload_cells={} voxel_budget={} packed_cubes={} cube_budget={} cells_fit={} cubes_fit={}",
						groupIndex,
						group.depth,
						attempt,
						nextAttemptExceedsQuality ? "quality_limit" : "retry_limit",
						voxelWidth,
						buildInput.autoAcceptanceErrorReference,
						payload.activeCells.size(),
						buildInput.analysis.voxelBudget,
						packed.cubeRecords.size(),
						buildInput.analysis.cubeBudget,
						cellCountFits,
						cubeCountFits);
					break;
				}

				voxelWidth = nextVoxelWidth;
				voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
				resolution = resolutionForVoxelWidth(voxelWidth);
			}

			const bool acceptedBudget = payloadFitsBudget || acceptedBudgetOverflowForQuality;
			if (payload.activeCells.empty() || packed.cubeRecords.empty() || packed.clusterRecords.empty() || (requireBudgetFit && !acceptedBudget))
			{
				stats.failedBuilds++;
				keepSourceCarryPayloadsForDagParents();
				logBuildFailure(qualityLimitExceeded ? "quality_limit_invalid_output_or_budget" : "invalid_output_or_budget");
				return false;
			}
			if (qualityLimitExceeded)
			{
				spdlog::warn(
					"ClusterLOD voxel build accepted beyond quality limit: group={} depth={} voxel_width={} representation_error={} acceptance_error={} require_budget={} payload_cells={} packed_cubes={}",
					groupIndex,
					group.depth,
					voxelWidth,
					voxelRepresentationError,
					buildInput.autoAcceptanceErrorReference,
					requireBudgetFit,
					payload.activeCells.size(),
					packed.cubeRecords.size());
			}
			const uint32_t maxClusterCubeCount = computeMaxClusterCubeCount(packed);
			spdlog::debug(
				"ClusterLOD voxel pack detail: group={} depth={} payload_cells={} voxel_budget={} source_primitives={} cube_budget={} packed_cubes={} packed_clusters={} max_cluster_cube_count={} packed_attributes={} payload_voxel_width={} voxel_representation_error={} opacity_threshold={}",
				groupIndex,
				group.depth,
				payload.activeCells.size(),
				buildInput.analysis.voxelBudget,
				buildInput.analysis.sourcePrimitiveCountForCubeBudget,
				buildInput.analysis.cubeBudget,
				packed.cubeRecords.size(),
				packed.clusterRecords.size(),
				maxClusterCubeCount,
				packed.attributeSamples.size(),
				payload.voxelWidth,
				packedVoxelRepresentationError,
				settings.voxelFallbackOpacityThreshold);
			if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
			{
				stats.failedBuilds++;
				logBuildFailure("empty_packed_output");
				return false;
			}
			uint32_t packedOccupiedCells = 0u;
			for (const CLodVoxelCubeRecord& cubeRecord : packed.cubeRecords)
			{
				packedOccupiedCells += static_cast<uint32_t>(std::popcount(cubeRecord.occupancyMask));
			}
			if (packedOccupiedCells != payload.activeCells.size())
			{
				spdlog::warn(
					"ClusterLOD voxel pack occupancy mismatch: group={} payload_cells={} packed_occupied_cells={} packed_cubes={}",
					groupIndex,
					payload.activeCells.size(),
					packedOccupiedCells,
					packed.cubeRecords.size());
			}

			state.voxelGroupMapping.groupToPayloadIndex[groupIndex] = -1;
			state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex] = static_cast<int32_t>(metadataIndex);
			std::vector<ClusterLODGroupSegment> voxelSegments;
			std::vector<BoundingSphere> voxelSegmentBounds;
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::SplitVoxelPageSegments");
				SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds, settings.nodeBoneLimit);
			}
			state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
			std::vector<std::vector<std::byte>> voxelPageBlobs;
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildVoxelPageBlobs");
				voxelPageBlobs = BuildVoxelGroupPageBlobs(
					voxelSegments,
					packed.clusterRecords,
					packed.cubeRecords,
					packed.attributeSamples,
					firstAttribute,
					settings.nodeBoneLimit);
			}
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

			const float triangleError = group.bounds.error;
			const bool terminalErrorSentinel = triangleError >= std::numeric_limits<float>::max() * 0.5f;
			const bool replaceGroupWithVoxels = forceReplaceGroupWithVoxels || group.terminalSegmentCount == 0u;
			group.representationError = packedVoxelRepresentationError;
			if (replaceGroupWithVoxels)
			{
				group.flags |= CLOD_GROUP_FLAG_IS_VOXEL;
				group.meshletCount = 0u;
				group.groupVertexCount = 0u;
				group.firstSegment = static_cast<uint32_t>(state.segments.size());
				group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
				group.terminalSegmentCount = 0u;
				for (const ClusterLODGroupSegment& segment : voxelSegments)
				{
					if (segment.refinedGroup < 0)
					{
						group.terminalSegmentCount++;
					}
					else
					{
						break;
					}
				}
				group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
				state.segments.insert(state.segments.end(), voxelSegments.begin(), voxelSegments.end());
				state.segmentBounds.insert(state.segmentBounds.end(), voxelSegmentBounds.begin(), voxelSegmentBounds.end());
				if (groupIndex < state.groupPageBlobs.size())
				{
					state.groupPageBlobs[groupIndex] = std::move(voxelPageBlobs);
				}
				if (groupIndex < state.groupChunks.size())
				{
					ClusterLODGroupChunk& chunk = state.groupChunks[groupIndex];
					chunk.groupVertexCount = 0u;
					chunk.meshletCount = 0u;
					chunk.meshletTrianglesByteCount = 0u;
				}
			}
			spdlog::debug(
				"ClusterLOD voxel group error: group={} depth={} triangle_cut_error={} voxel_width={} representation_error={} terminal_sentinel={} terminal_segments={}/{} refined_child_errors={} min_refined_child_error={} max_refined_child_error={} source_primitives={} cube_budget={} packed_cubes={} forced_budget_fit={} replaces_group={}",
				groupIndex,
				group.depth,
				triangleError,
				payload.voxelWidth,
				packedVoxelRepresentationError,
				terminalErrorSentinel,
				group.terminalSegmentCount,
				group.segmentCount,
				refinedChildErrorCount,
				childErrorRange.minError,
				maxRefinedChildError,
				buildInput.analysis.sourcePrimitiveCountForCubeBudget,
				buildInput.analysis.cubeBudget,
				packed.cubeRecords.size(),
				requireBudgetFit,
				replaceGroupWithVoxels);
			stats.generatedPayloads++;
			stats.generatedCubes += static_cast<uint32_t>(packed.cubeRecords.size());
			keepSourceCarryPayloadsForDagParents();
			return true;
		};

		std::vector<uint8_t> replaceVoxelGroupMask(originalGroupCount, 0u);
		std::vector<uint8_t> buildVoxelGroupMask(originalGroupCount, 0u);
		std::vector<uint8_t> seedVoxelGroupMask(originalGroupCount, 0u);
		for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
		{
			if (groupInputs[groupIndex].autoWouldFitBudget)
			{
				replaceVoxelGroupMask[groupIndex] = 1u;
				seedVoxelGroupMask[groupIndex] = 1u;
			}
		}

		bool propagatedAny = true;
		while (propagatedAny)
		{
			propagatedAny = false;
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				if (replaceVoxelGroupMask[groupIndex] != 0u)
				{
					continue;
				}

				const ClusterLODGroup& group = state.groups[groupIndex];
				bool hasVoxelRefinedChild = false;
				for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
				{
					const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
					if (segment.refinedGroup < 0)
					{
						continue;
					}

					const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
					if (childGroupIndex < replaceVoxelGroupMask.size() && replaceVoxelGroupMask[childGroupIndex] != 0u)
					{
						hasVoxelRefinedChild = true;
						break;
					}
				}

				if (hasVoxelRefinedChild)
				{
					replaceVoxelGroupMask[groupIndex] = 1u;
					propagatedAny = true;
				}
			}
		}

		buildVoxelGroupMask = replaceVoxelGroupMask;
		propagatedAny = true;
		while (propagatedAny)
		{
			propagatedAny = false;
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				if (buildVoxelGroupMask[groupIndex] == 0u)
				{
					continue;
				}

				for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, groupIndex))
				{
					if (childGroupIndex < buildVoxelGroupMask.size() && buildVoxelGroupMask[childGroupIndex] == 0u)
					{
						buildVoxelGroupMask[childGroupIndex] = 1u;
						propagatedAny = true;
					}
				}
			}
		}

		auto hasMarkedVoxelRefinedChild = [&](uint32_t groupIndex) -> bool
		{
			if (groupIndex >= originalGroupCount)
			{
				return false;
			}

			const ClusterLODGroup& group = state.groups[groupIndex];
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex < replaceVoxelGroupMask.size() && replaceVoxelGroupMask[childGroupIndex] != 0u)
				{
					return true;
				}
			}
			return false;
		};

		{
			uint32_t replaceVoxelGroupCount = 0u;
			uint32_t buildVoxelGroupCount = 0u;
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				replaceVoxelGroupCount += replaceVoxelGroupMask[groupIndex] != 0u ? 1u : 0u;
				buildVoxelGroupCount += buildVoxelGroupMask[groupIndex] != 0u ? 1u : 0u;
			}
			TracyPlot("CLOD.VoxelFallback.ReplaceGroups", static_cast<int64_t>(replaceVoxelGroupCount));
			TracyPlot("CLOD.VoxelFallback.BuildGroups", static_cast<int64_t>(buildVoxelGroupCount));
			spdlog::debug(
				"ClusterLOD voxel fallback masks: replace_groups={} build_groups={} dependency_groups={}",
				replaceVoxelGroupCount,
				buildVoxelGroupCount,
				buildVoxelGroupCount >= replaceVoxelGroupCount ? buildVoxelGroupCount - replaceVoxelGroupCount : 0u);
		}

		if (std::any_of(buildVoxelGroupMask.begin(), buildVoxelGroupMask.end(), [](uint8_t value) { return value != 0u; }) &&
			AppendSharedVoxelCoverageSourceGeometry(state, buildVoxelGroupMask, sharedCoverageBuildInput, vertexStrideBytes))
		{
			const auto coverageBvhStart = std::chrono::steady_clock::now();
			{
				ZoneScopedN("ClusterLODUtilities::VoxelFallback::BuildSharedCoverageBVH");
				sharedCoverageSourceTriangles.Build(
					&sharedCoverageBuildInput.voxelVertices,
					vertexStrideBytes,
					&sharedCoverageBuildInput.voxelTriangleIndices,
					sharedCoverageBuildInput.voxelSkinningVertices.empty() ? nullptr : &sharedCoverageBuildInput.voxelSkinningVertices,
					skinningVertexStrideBytes,
					sharedCoverageBuildInput.voxelTriangleRefinedGroupIds.empty() ? nullptr : &sharedCoverageBuildInput.voxelTriangleRefinedGroupIds,
					settings.doubleSidedVoxelSourceNormals,
					false);
				sharedCoverageSourceTriangles.SetRefinedGroupDomainMap(BuildVoxelCoverageDomainMap(state, buildVoxelGroupMask));
			}
			stats.coverageBvhUs += elapsedUsSince(coverageBvhStart);
			stats.coverageBvhBuilds++;
			if (sharedCoverageSourceTriangles.IsValid())
			{
				sharedVoxelCoverageSourceTriangles = &sharedCoverageSourceTriangles;
			}
			spdlog::debug(
				"ClusterLOD voxel shared coverage source: vertices={} triangles={} build_groups={} valid={}",
				sharedCoverageBuildInput.voxelVertexCount,
				sharedCoverageBuildInput.voxelTriangleIndices.size() / 3ull,
				std::count_if(buildVoxelGroupMask.begin(), buildVoxelGroupMask.end(), [](uint8_t value) { return value != 0u; }),
				sharedVoxelCoverageSourceTriangles != nullptr);
		}

		std::vector<uint32_t> remainingVoxelSourceConsumers(originalGroupCount, 0u);
		for (uint32_t parentGroupIndex = 0; parentGroupIndex < originalGroupCount; ++parentGroupIndex)
		{
			if (buildVoxelGroupMask[parentGroupIndex] == 0u)
			{
				continue;
			}

			for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, parentGroupIndex))
			{
				if (childGroupIndex < originalGroupCount && buildVoxelGroupMask[childGroupIndex] != 0u)
				{
					remainingVoxelSourceConsumers[childGroupIndex]++;
				}
			}
		}

		auto releaseCarryPayloadIfUnconsumed = [&](uint32_t groupIndex)
		{
			if (groupIndex >= remainingVoxelSourceConsumers.size() ||
				remainingVoxelSourceConsumers[groupIndex] != 0u ||
				groupIndex >= state.voxelCarryPayloads.size() ||
				state.voxelCarryPayloads[groupIndex].activeCells.empty())
			{
				return;
			}

			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseConsumedCarryPayload");
			ReleaseVoxelGroupPayloadStorage(state.voxelCarryPayloads[groupIndex]);
			TracyPlot("CLOD.VoxelFallback.LiveCarryCells", static_cast<int64_t>(CountLiveCarryPayloadCells(state)));
		};

		auto releaseConsumedSourcePayloads = [&](uint32_t parentGroupIndex)
		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseConsumedSourcePayloads");
			for (uint32_t childGroupIndex : CollectUniqueRefinedChildren(state, parentGroupIndex))
			{
				if (childGroupIndex >= remainingVoxelSourceConsumers.size() ||
					remainingVoxelSourceConsumers[childGroupIndex] == 0u)
				{
					continue;
				}

				remainingVoxelSourceConsumers[childGroupIndex]--;
				releaseCarryPayloadIfUnconsumed(childGroupIndex);
			}
		};

		for (uint32_t depth = 0; depth <= maxDepth; ++depth)
		{
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				ClusterLODGroup& group = state.groups[groupIndex];
				if (buildVoxelGroupMask[groupIndex] == 0u ||
					static_cast<uint32_t>(std::max(group.depth, 0)) != depth ||
					(group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
				{
					continue;
				}

				const bool inheritedVoxelPath = hasMarkedVoxelRefinedChild(groupIndex);
				const bool seedVoxelPath = seedVoxelGroupMask[groupIndex] != 0u;
				const bool requireBudgetFit = seedVoxelPath && !inheritedVoxelPath;
				const bool requireQualityFit = seedVoxelPath && !inheritedVoxelPath;
				const bool builtVoxelGroup = buildVoxelGroup(groupIndex, buildVoxelGroupMask, requireBudgetFit, requireQualityFit, inheritedVoxelPath);
				if (!builtVoxelGroup)
				{
					throw std::runtime_error(
						std::string("ClusterLOD voxel fallback: mandatory voxel build failed for group ") +
						std::to_string(groupIndex) +
						" depth=" + std::to_string(std::max(group.depth, 0)) +
						" seed=" + std::to_string(seedVoxelPath ? 1 : 0) +
						" inherited=" + std::to_string(inheritedVoxelPath ? 1 : 0) +
						" require_budget=" + std::to_string(requireBudgetFit ? 1 : 0) +
						" require_quality=" + std::to_string(requireQualityFit ? 1 : 0));
				}

				if (builtVoxelGroup)
				{
					if (inheritedVoxelPath)
					{
						stats.propagatedGroups++;
					}
					else if (seedVoxelPath)
					{
						stats.acceptedSeedGroups++;
					}

					releaseConsumedSourcePayloads(groupIndex);
					releaseCarryPayloadIfUnconsumed(groupIndex);
				}
			}
		}

		BuildPartVoxelTailGroups(
			state,
			sharedVoxelCoverageSourceTriangles,
			coverageMaterialSampler,
			settings);

		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ValidateVoxelParentClosure");
			for (uint32_t parentGroupIndex = 0; parentGroupIndex < static_cast<uint32_t>(state.groups.size()); ++parentGroupIndex)
			{
				const ClusterLODGroup& parentGroup = state.groups[parentGroupIndex];
				if (parentGroup.firstSegment + parentGroup.segmentCount > state.segments.size())
				{
					continue;
				}

				const bool parentIsVoxel = (parentGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
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

					const ClusterLODGroup& childGroup = state.groups[childGroupIndex];
					if ((childGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u && !parentIsVoxel)
					{
						throw std::runtime_error(
							std::string("ClusterLOD voxel fallback: voxel refined child has non-voxel DAG parent; child=") +
							std::to_string(childGroupIndex) +
							" child_depth=" + std::to_string(std::max(childGroup.depth, 0)) +
							" parent=" + std::to_string(parentGroupIndex) +
							" parent_depth=" + std::to_string(std::max(parentGroup.depth, 0)));
					}
				}
			}
		}

		{
			ZoneScopedN("ClusterLODUtilities::VoxelFallback::ReleaseRemainingCarryPayloads");
			for (VoxelGroupPayload& carryPayload : state.voxelCarryPayloads)
			{
				ReleaseVoxelGroupPayloadStorage(carryPayload);
			}
			TracyPlot("CLOD.VoxelFallback.LiveCarryCells", int64_t{ 0 });
		}

		std::vector<uint32_t> parentRefCounts(state.groups.size(), 0u);
		std::vector<uint32_t> nonVoxelParentRefCounts(state.groups.size(), 0u);
		std::vector<float> voxelParentRepresentationErrorForGroup(state.groups.size(), 0.0f);
		for (const ClusterLODGroup& group : state.groups)
		{
			if (group.firstSegment + group.segmentCount > state.segments.size())
			{
				continue;
			}

			const bool parentIsVoxel = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
			const float parentVoxelRepresentationError =
				parentIsVoxel && std::isfinite(group.representationError) && group.representationError > 0.0f
				? group.representationError
				: 0.0f;
			for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
			{
				const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
				if (segment.refinedGroup < 0)
				{
					continue;
				}

				const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
				if (childGroupIndex < parentRefCounts.size())
				{
					parentRefCounts[childGroupIndex]++;
					if (parentIsVoxel)
					{
						voxelParentRepresentationErrorForGroup[childGroupIndex] = std::max(
							voxelParentRepresentationErrorForGroup[childGroupIndex],
							parentVoxelRepresentationError);
					}
					else
					{
						nonVoxelParentRefCounts[childGroupIndex]++;
					}
				}
			}
		}

		uint32_t voxelTraversalBoundaryRewrites = 0u;
		uint32_t voxelTraversalRootSentinelsPreserved = 0u;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			ClusterLODGroup& group = state.groups[groupIndex];
			const bool hasVoxelParentBoundary =
				groupIndex < voxelParentRepresentationErrorForGroup.size() &&
				std::isfinite(voxelParentRepresentationErrorForGroup[groupIndex]) &&
				voxelParentRepresentationErrorForGroup[groupIndex] > 0.0f;
			const bool hasNonVoxelParent =
				groupIndex < nonVoxelParentRefCounts.size() &&
				nonVoxelParentRefCounts[groupIndex] != 0u;
			const bool isRootGroup =
				groupIndex < parentRefCounts.size() &&
				parentRefCounts[groupIndex] == 0u;

			if (!hasVoxelParentBoundary)
			{
				if (isRootGroup &&
					(group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u &&
					IsTerminalErrorSentinel(group.bounds.error))
				{
					voxelTraversalRootSentinelsPreserved++;
				}
				continue;
			}

			if (isRootGroup && IsTerminalErrorSentinel(group.bounds.error))
			{
				voxelTraversalRootSentinelsPreserved++;
				continue;
			}

			float traversalBoundary = voxelParentRepresentationErrorForGroup[groupIndex];
			if (hasNonVoxelParent && groupIndex < originalGroupErrors.size())
			{
				const float originalGroupError = originalGroupErrors[groupIndex];
				if (std::isfinite(originalGroupError) && originalGroupError > 0.0f)
				{
					traversalBoundary = std::max(traversalBoundary, originalGroupError);
				}
			}

			if (std::isfinite(traversalBoundary) && traversalBoundary > 0.0f)
			{
				group.bounds.error = traversalBoundary;
				voxelTraversalBoundaryRewrites++;
			}
		}

		uint32_t parentTraversalErrorRaises = 0u;
		bool raisedParentError = true;
		while (raisedParentError)
		{
			raisedParentError = false;
			for (uint32_t groupIndex = 0; groupIndex < originalGroupCount; ++groupIndex)
			{
				ClusterLODGroup& group = state.groups[groupIndex];
				if (IsTerminalErrorSentinel(group.bounds.error))
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
				parentTraversalErrorRaises++;
				raisedParentError = true;
			}
		}

		uint32_t voxelTraversalErrorUnderreports = 0u;
		for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u)
			{
				continue;
			}

			float minChildError = std::numeric_limits<float>::max();
			float maxChildError = 0.0f;
			uint32_t refinedChildCount = 0;
			uint32_t voxelChildCount = 0;
			uint32_t triangleChildCount = 0;
			bool monotonicWithChildren = true;

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

				const ClusterLODGroup& childGroup = state.groups[childGroupIndex];
				const float childError = childGroup.bounds.error;
				minChildError = std::min(minChildError, childError);
				maxChildError = std::max(maxChildError, childError);
				refinedChildCount++;
				if ((childGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
				{
					voxelChildCount++;
				}
				else
				{
					triangleChildCount++;
				}

				if (!(group.bounds.error > childError))
				{
					monotonicWithChildren = false;
				}
			}

			if (refinedChildCount == 0)
			{
				minChildError = -1.0f;
			}
			else if (std::isfinite(group.representationError) &&
				group.representationError > 0.0f &&
				std::isfinite(group.bounds.error) &&
				!IsTerminalErrorSentinel(group.bounds.error) &&
				group.representationError > group.bounds.error)
			{
				voxelTraversalErrorUnderreports++;
				if (voxelTraversalErrorUnderreports <= 8u)
				{
					spdlog::warn(
						"ClusterLOD voxel traversal error underreports representation error: group={} depth={} representation_error={} min_child_error={} max_child_error={} group_cut_error={} terminal_segments={}/{}",
						groupIndex,
						group.depth,
						group.representationError,
						minChildError,
						maxChildError,
						group.bounds.error,
						group.terminalSegmentCount,
						group.segmentCount);
				}
			}

			spdlog::debug(
				"ClusterLOD voxel hierarchy: group={} depth={} cut_error={} representation_error={} refined_children={} voxel_children={} triangle_children={} min_child_error={} max_child_error={} monotonic_with_children={}",
				groupIndex,
				group.depth,
				group.bounds.error,
				group.representationError,
				refinedChildCount,
				voxelChildCount,
				triangleChildCount,
				minChildError,
				maxChildError,
				monotonicWithChildren);
		}

		uint32_t voxelGroups = 0;
		for (const ClusterLODGroup& group : state.groups)
		{
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
			{
				voxelGroups++;
			}
		}
		const uint32_t triangleGroups = static_cast<uint32_t>(state.groups.size()) - voxelGroups;
		const uint32_t totalVoxelPayloads = static_cast<uint32_t>(state.voxelGroupMapping.payloads.size());
		const uint32_t totalVoxelClusters = static_cast<uint32_t>(state.voxelGroupMapping.packedClusterRecords.size());
		const uint32_t totalVoxelCubes = static_cast<uint32_t>(state.voxelGroupMapping.packedCubeRecords.size());
		uint64_t retainedPayloadCells = 0u;
		for (const VoxelGroupPayload& payload : state.voxelGroupMapping.payloads)
		{
			retainedPayloadCells += payload.activeCells.size();
		}
		TracyPlot("CLOD.VoxelFallback.VoxelGroups", static_cast<int64_t>(voxelGroups));
		TracyPlot("CLOD.VoxelFallback.TriangleGroups", static_cast<int64_t>(triangleGroups));
		TracyPlot("CLOD.VoxelFallback.Payloads", static_cast<int64_t>(totalVoxelPayloads));
		TracyPlot("CLOD.VoxelFallback.RetainedPayloadCells", static_cast<int64_t>(retainedPayloadCells));
		TracyPlot("CLOD.VoxelFallback.Clusters", static_cast<int64_t>(totalVoxelClusters));
		TracyPlot("CLOD.VoxelFallback.Cubes", static_cast<int64_t>(totalVoxelCubes));
		TracyPlot("CLOD.VoxelFallback.FailedBuilds", static_cast<int64_t>(stats.failedBuilds));

		spdlog::info(
			"ClusterLOD voxel fallback: analyzed={} valid={} auto_candidates={} accepted_seeds={} forced={} propagated={} voxel_groups={} triangle_groups={} payloads={} clusters={} cubes={} traversal_error_underreports={} voxel_boundary_rewrites={} parent_error_raises={} failed={} coverage_bvh_builds={} coverage_bvh_reuses={} source_coverage(queries={} candidates={} tests={} out_of_cell={}) timing_ms(analysis={:.2f} source={:.2f} coverage_bvh={:.2f} voxelize={:.2f} pack={:.2f})",
			stats.analyzedGroups,
			stats.validGroups,
			stats.autoCandidateGroups,
			stats.acceptedSeedGroups,
			stats.forcedGroups,
			stats.propagatedGroups,
			voxelGroups,
			triangleGroups,
			totalVoxelPayloads,
			totalVoxelClusters,
			totalVoxelCubes,
			voxelTraversalErrorUnderreports,
			voxelTraversalBoundaryRewrites,
			parentTraversalErrorRaises,
			stats.failedBuilds,
			stats.coverageBvhBuilds,
			stats.coverageBvhReuses,
			stats.sourceCoverageQueries,
			stats.sourceCoverageCandidates,
			stats.sourceCoverageTests,
			stats.sourceCoverageOutOfCell,
			static_cast<double>(stats.analysisUs) / 1000.0,
			static_cast<double>(stats.sourceBuildUs) / 1000.0,
			static_cast<double>(stats.coverageBvhUs) / 1000.0,
			static_cast<double>(stats.voxelizeUs) / 1000.0,
			static_cast<double>(stats.packUs) / 1000.0);
		if (voxelTraversalRootSentinelsPreserved != 0u)
		{
			spdlog::debug(
				"ClusterLOD voxel fallback preserved {} root terminal sentinel traversal errors after voxel boundary rewrites",
				voxelTraversalRootSentinelsPreserved);
		}
	}

}
