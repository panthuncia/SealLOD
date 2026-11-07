#include <BasicRenderer/Assets/Import/CLodCache.h>
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

namespace clod_detail
{
	bool VoxelizeMergeTriangles(
		const std::vector<std::byte>& vertices,
		size_t vertexStrideBytes,
		const std::vector<uint32_t>& indices,
		float voxelWidth,
		uint32_t raysPerCell,
		MergeVoxelization& out)
	{
		ZoneScopedN("ClusterLODUtilities::VoxelizeMergeTriangles");
		out = {};
		const size_t vertexCount = vertexStrideBytes != 0u ? vertices.size() / vertexStrideBytes : 0u;
		if (vertexCount == 0u || indices.empty() || !(voxelWidth > 0.0f) || !std::isfinite(voxelWidth))
		{
			return false;
		}
		DirectX::XMFLOAT3 aabbMin(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
		DirectX::XMFLOAT3 aabbMax(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
		for (const uint32_t index : indices)
		{
			if (index >= vertexCount) return false;
			const DirectX::XMFLOAT3 position = ReadGroupVertexPosition(vertices, vertexStrideBytes, index);
			aabbMin = DirectX::XMFLOAT3(std::min(aabbMin.x, position.x), std::min(aabbMin.y, position.y), std::min(aabbMin.z, position.z));
			aabbMax = DirectX::XMFLOAT3(std::max(aabbMax.x, position.x), std::max(aabbMax.y, position.y), std::max(aabbMax.z, position.z));
		}
		// A cubic grid of whole cells, padded by half a cell, as runtime cube placement needs one scalar width.
		const float longest = std::max({ aabbMax.x - aabbMin.x, aabbMax.y - aabbMin.y, aabbMax.z - aabbMin.z }) + voxelWidth;
		// Cube coordinates are 10 bits of 4-cell cubes.
		const uint32_t resolution = std::max(2u, static_cast<uint32_t>(std::ceil(longest / voxelWidth)));
		if (resolution > 4096u)
		{
			return false;
		}
		const float extent = static_cast<float>(resolution) * voxelWidth;
		const DirectX::XMFLOAT3 center(0.5f * (aabbMin.x + aabbMax.x), 0.5f * (aabbMin.y + aabbMax.y), 0.5f * (aabbMin.z + aabbMax.z));
		const DirectX::XMFLOAT3 gridMin(center.x - 0.5f * extent, center.y - 0.5f * extent, center.z - 0.5f * extent);
		const DirectX::XMFLOAT3 gridMax(center.x + 0.5f * extent, center.y + 0.5f * extent, center.z + 0.5f * extent);

		// One refinement domain; AppendMergeVoxelGroup names the real one.
		const std::vector<int32_t> refinedGroups(indices.size() / 3u, 0);
		VoxelSourceTriangleBVH coverage;
		coverage.Build(&vertices, vertexStrideBytes, &indices, nullptr, 0u, &refinedGroups, true);
		VoxelizeTrianglesInput input{};
		input.vertices = &vertices;
		input.vertexStrideBytes = vertexStrideBytes;
		input.triangleIndices = &indices;
		input.triangleRefinedGroupIds = &refinedGroups;
		input.doubleSidedTriangles = true;
		input.coverageSourceTriangles = coverage.IsValid() ? &coverage : nullptr;
		input.aabbMin = gridMin;
		input.aabbMax = gridMax;
		input.voxelWidth = voxelWidth;
		input.resolution = resolution;
		input.raysPerCell = std::max(1u, raysPerCell);
		input.emitSourcePayload = false;
		VoxelizeTrianglesResult result = VoxelizeTrianglesDetailed(input);
		if (result.renderPayload.activeCells.empty())
		{
			return false;
		}
		PackVoxelGroupInput packInput{};
		packInput.payload = &result.renderPayload;
		packInput.voxelError = ComputeVoxelRepresentationError(voxelWidth);
		packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		const PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
		out.cubes = static_cast<uint32_t>(packed.cubeRecords.size());
		out.payload = std::move(result.renderPayload);
		return out.cubes != 0u;
	}

	MergeGroupOutput AppendMergeVoxelGroup(ClusterLODBuildState& state, const MergeVoxelization& voxels, uint32_t groupIndex, int32_t refinedGroup)
	{
		ZoneScopedN("ClusterLODUtilities::AppendMergeVoxelGroup");
		VoxelGroupMapping& mapping = state.voxelGroupMapping;
		const uint32_t firstCluster = static_cast<uint32_t>(mapping.packedClusterRecords.size());
		const uint32_t firstCube = static_cast<uint32_t>(mapping.packedCubeRecords.size());
		const uint32_t firstAttribute = static_cast<uint32_t>(mapping.packedAttributeSamples.size());
		const float voxelError = ComputeVoxelRepresentationError(voxels.payload.voxelWidth);
		PackVoxelGroupInput packInput{};
		packInput.payload = &voxels.payload;
		packInput.voxelError = voxelError;
		packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
		packInput.firstCube = firstCube;
		packInput.firstAttribute = firstAttribute;
		PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
		packed.metadata.firstCluster = firstCluster;
		for (CLodVoxelCubeRecord& cube : packed.cubeRecords) cube.refinedGroup = refinedGroup;
		BuildVoxelClustersFromCubes(packed, CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);
		for (CLodVoxelClusterRecord& cluster : packed.clusterRecords) cluster.refinedGroup = refinedGroup;

		MergeGroupOutput out{};
		SplitVoxelClustersIntoPageSegments(packed, out.segments, out.segmentBounds, CLOD_NODE_BONE_LIMIT_DEFAULT);
		for (ClusterLODGroupSegment& segment : out.segments) segment.refinedGroup = refinedGroup;
		out.pageBlobs = BuildVoxelGroupPageBlobs(
			out.segments, packed.clusterRecords, packed.cubeRecords, packed.attributeSamples, firstAttribute, CLOD_NODE_BONE_LIMIT_DEFAULT);
		out.group.flags = CLOD_GROUP_FLAG_IS_VOXEL;
		out.group.segmentCount = static_cast<uint32_t>(out.segments.size());
		out.group.pageCount = static_cast<uint32_t>(out.pageBlobs.size());
		out.group.representationError = voxelError;
		for (const ClusterLODGroupSegment& segment : out.segments)
		{
			out.group.terminalSegmentCount += segment.refinedGroup < 0 ? 1u : 0u;
		}

		const uint32_t metadataIndex = static_cast<uint32_t>(mapping.packedGroupMetadata.size());
		mapping.packedGroupMetadata.push_back(packed.metadata);
		mapping.packedClusterRecords.insert(mapping.packedClusterRecords.end(), packed.clusterRecords.begin(), packed.clusterRecords.end());
		mapping.packedCubeRecords.insert(mapping.packedCubeRecords.end(), packed.cubeRecords.begin(), packed.cubeRecords.end());
		mapping.packedAttributeSamples.insert(mapping.packedAttributeSamples.end(), packed.attributeSamples.begin(), packed.attributeSamples.end());
		mapping.groupToPayloadIndex.resize(std::max<size_t>(mapping.groupToPayloadIndex.size(), groupIndex + 1u), -1);
		mapping.groupToPackedMetadataIndex.resize(std::max<size_t>(mapping.groupToPackedMetadataIndex.size(), groupIndex + 1u), -1);
		mapping.groupToPackedMetadataIndex[groupIndex] = static_cast<int32_t>(metadataIndex);
		return out;
	}
}

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<uint32_t>& indices,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster)
{
	return BuildVoxelOnlyClusterLODArtifactsFromGeometry(
		vertices,
		vertexSize,
		indices,
		settings,
		std::nullopt,
		maxCubesPerCluster);
}

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromPayload(
	const VoxelGroupPayload& payload,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster)
{
	ZoneScopedN("ClusterLODUtilities::BuildVoxelOnlyClusterLODArtifactsFromPayload");
	CLodCache::ThrowIfCacheOnly("BuildVoxelOnlyClusterLODArtifactsFromPayload");
	ClusterLODPrebuildArtifacts artifacts{};
	if (payload.activeCells.empty() || payload.resolution == 0u || !(payload.voxelWidth > 0.0f))
	{
		return artifacts;
	}

	const float voxelRepresentationError = ComputeVoxelRepresentationError(payload.voxelWidth);
	PackVoxelGroupInput packInput{};
	packInput.payload = &payload;
	packInput.voxelError = voxelRepresentationError;
	packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
	packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	PackedVoxelGroupBuildResult packed = PackVoxelGroupToCubes(packInput);
	BuildVoxelClustersFromCubes(packed, std::max(1u, maxCubesPerCluster));
	TracyPlot("CLOD.VoxelOnly.PayloadCells", static_cast<int64_t>(payload.activeCells.size()));
	if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
	{
		return artifacts;
	}

	std::vector<ClusterLODGroupSegment> voxelSegments;
	std::vector<BoundingSphere> voxelSegmentBounds;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::BuildVoxelPages");
		SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds, settings.nodeBoneLimit);
	}
	std::vector<std::vector<std::byte>> voxelPageBlobs;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::BuildVoxelPageBlobs");
		voxelPageBlobs = BuildVoxelGroupPageBlobs(
			voxelSegments,
			packed.clusterRecords,
			packed.cubeRecords,
			packed.attributeSamples,
			0u,
			settings.nodeBoneLimit);
	}
	if (voxelSegments.empty() || voxelPageBlobs.empty())
	{
		return artifacts;
	}

	ClusterLODBuildState state{};
	ClusterLODGroup group{};
	const DirectX::XMFLOAT3 payloadMin = payload.aabbMin;
	const DirectX::XMFLOAT3 payloadMax = payload.aabbMax;
	const float centerX = 0.5f * (payloadMin.x + payloadMax.x);
	const float centerY = 0.5f * (payloadMin.y + payloadMax.y);
	const float centerZ = 0.5f * (payloadMin.z + payloadMax.z);
	const float dx = payloadMax.x - centerX;
	const float dy = payloadMax.y - centerY;
	const float dz = payloadMax.z - centerZ;
	group.bounds.center[0] = centerX;
	group.bounds.center[1] = centerY;
	group.bounds.center[2] = centerZ;
	group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
	group.bounds.error = std::numeric_limits<float>::max();
	group.depth = 0;
	group.firstSegment = 0u;
	group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
	group.terminalSegmentCount = group.segmentCount;
	group.flags = CLOD_GROUP_FLAG_IS_VOXEL;
	group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
	group.representationError = voxelRepresentationError;

	state.groups.push_back(group);
	state.segments = std::move(voxelSegments);
	state.segmentBounds = std::move(voxelSegmentBounds);
	state.groupChunks.resize(1);
	state.groupPageBlobs.resize(1);
	state.groupPageBlobs[0] = std::move(voxelPageBlobs);
	state.voxelGroupMapping.groupToPayloadIndex = { 0 };
	state.voxelGroupMapping.groupToPackedMetadataIndex = { 0 };
	state.voxelGroupMapping.payloads.push_back(payload);
	state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
	state.voxelGroupMapping.packedClusterRecords = std::move(packed.clusterRecords);
	state.voxelGroupMapping.packedCubeRecords = std::move(packed.cubeRecords);
	state.voxelGroupMapping.packedAttributeSamples = std::move(packed.attributeSamples);

	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::BuildTraversalHierarchy");
		BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/8u);
	}
	std::vector<ClusterLODNodeSkinningInfo> nodeSkinningInfos;
	std::vector<uint32_t> nodeBoneIndices;
	BuildNodeSkinningSidecar(state, settings.nodeBoneLimit, nodeSkinningInfos, nodeBoneIndices);

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyPayload::FinalizeMeshWidePagePacking");
		FinalizeMeshWidePagePacking(
			state,
			meshPageBlobs,
			groupPageReferences,
			groupPageReferenceOffsets,
			trianglePageCount,
			voxelPageBase,
			voxelPageCount);
	}

	artifacts.prebuiltData.groups = std::move(state.groups);
	artifacts.prebuiltData.segments = std::move(state.segments);
	artifacts.prebuiltData.segmentBounds = std::move(state.segmentBounds);
	artifacts.prebuiltData.objectBoundingSphere = BuildObjectBoundingSphereFromRootNode(state.nodes, state.topRootNode);
	artifacts.prebuiltData.groupChunks = std::move(state.groupChunks);
	artifacts.prebuiltData.groupPageReferences = std::move(groupPageReferences);
	artifacts.prebuiltData.groupPageReferenceOffsets = std::move(groupPageReferenceOffsets);
	artifacts.prebuiltData.trianglePageCount = trianglePageCount;
	artifacts.prebuiltData.voxelPageBase = voxelPageBase;
	artifacts.prebuiltData.voxelPageCount = voxelPageCount;
	artifacts.prebuiltData.nodes = std::move(state.nodes);
	artifacts.prebuiltData.nodeSkinningInfos = std::move(nodeSkinningInfos);
	artifacts.prebuiltData.nodeBoneIndices = std::move(nodeBoneIndices);
	artifacts.prebuiltData.nodeBoneLimit = std::clamp(settings.nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
	artifacts.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	artifacts.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	artifacts.prebuiltData.maxDepth = state.maxDepth;
	artifacts.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	AssignSingleRootPartRecord(artifacts.prebuiltData, state.topRootNode);
	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);
	std::string representationError;
	if (!ValidateClusterLODPageRepresentations(
		artifacts.prebuiltData,
		&artifacts.cacheBuildData.meshPageBlobs,
		&representationError))
	{
		spdlog::error("ClusterLOD voxel page-representation validation failed: {}", representationError);
		throw std::runtime_error("ClusterLOD voxel page-representation validation failed: " + representationError);
	}
	EmitClusterLODPagePackingTelemetry(
		"voxel_payload",
		artifacts.prebuiltData,
		artifacts.cacheBuildData.meshPageBlobs);

	return artifacts;
}

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<uint32_t>& indices,
	const ClusterLODBuilderSettings& settings,
	const std::optional<ClusterLODVoxelGridOverride>& gridOverride,
	uint32_t maxCubesPerCluster)
{
	ZoneScopedN("ClusterLODUtilities::BuildVoxelOnlyClusterLODArtifactsFromGeometry");
	CLodCache::ThrowIfCacheOnly("BuildVoxelOnlyClusterLODArtifactsFromGeometry");
	ClusterLODPrebuildArtifacts artifacts{};
	const size_t vertexStrideBytes = vertexSize;
	const size_t vertexCount = vertexStrideBytes != 0u ? vertices.size() / vertexStrideBytes : 0u;
	TracyPlot("CLOD.VoxelOnly.Vertices", static_cast<int64_t>(vertexCount));
	TracyPlot("CLOD.VoxelOnly.Triangles", static_cast<int64_t>(indices.size() / 3u));
	if (vertexCount == 0u || vertexStrideBytes < sizeof(float) * 3u || indices.empty() || (indices.size() % 3u) != 0u)
	{
		return artifacts;
	}

	DirectX::XMFLOAT3 aabbMin(
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<float>::max());
	DirectX::XMFLOAT3 aabbMax(
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest(),
		std::numeric_limits<float>::lowest());

	for (uint32_t index : indices)
	{
		if (index >= vertexCount)
		{
			continue;
		}

		const DirectX::XMFLOAT3 position = ReadGroupVertexPosition(vertices, vertexStrideBytes, index);
		aabbMin.x = std::min(aabbMin.x, position.x);
		aabbMin.y = std::min(aabbMin.y, position.y);
		aabbMin.z = std::min(aabbMin.z, position.z);
		aabbMax.x = std::max(aabbMax.x, position.x);
		aabbMax.y = std::max(aabbMax.y, position.y);
		aabbMax.z = std::max(aabbMax.z, position.z);
	}

	const float extentX = aabbMax.x - aabbMin.x;
	const float extentY = aabbMax.y - aabbMin.y;
	const float extentZ = aabbMax.z - aabbMin.z;
	const float longestExtent = std::max({ extentX, extentY, extentZ });
	if (!std::isfinite(longestExtent) || longestExtent <= 1.0e-8f)
	{
		return artifacts;
	}

	const uint32_t resolution = std::max(
		2u,
		std::max(settings.voxelMinResolution, settings.voxelGridBaseResolution));
	float voxelWidth = longestExtent / static_cast<float>(resolution);
	if (gridOverride)
	{
		const auto& grid = *gridOverride;
		if (grid.resolution < 2u ||
			!(grid.voxelWidth > 0.0f) ||
			!std::isfinite(grid.voxelWidth) ||
			grid.aabbMax.x <= grid.aabbMin.x ||
			grid.aabbMax.y <= grid.aabbMin.y ||
			grid.aabbMax.z <= grid.aabbMin.z)
		{
			return artifacts;
		}

		aabbMin = grid.aabbMin;
		aabbMax = grid.aabbMax;
		voxelWidth = grid.voxelWidth;
	}
	else
	{
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
	}
	if (!(voxelWidth > 0.0f) || !std::isfinite(voxelWidth))
	{
		return artifacts;
	}
	const uint32_t voxelResolution = gridOverride ? gridOverride->resolution : resolution;

	VoxelSourceTriangleBVH coverageSourceTriangles;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::BuildCoverageBVH");
		coverageSourceTriangles.Build(
			&vertices,
			vertexStrideBytes,
			&indices,
			nullptr,
			0u,
			nullptr,
			settings.doubleSidedVoxelSourceNormals);
	}

	VoxelizeTrianglesInput voxelInput{};
	voxelInput.vertices = &vertices;
	voxelInput.vertexStrideBytes = vertexStrideBytes;
	voxelInput.triangleIndices = &indices;
	voxelInput.doubleSidedTriangles = settings.doubleSidedVoxelSourceNormals;
	voxelInput.coverageSourceTriangles = coverageSourceTriangles.IsValid() ? &coverageSourceTriangles : nullptr;
	voxelInput.aabbMin = aabbMin;
	voxelInput.aabbMax = aabbMax;
	voxelInput.voxelWidth = voxelWidth;
	voxelInput.resolution = voxelResolution;
	voxelInput.raysPerCell = settings.voxelRaysPerCell;
	voxelInput.emitSourcePayload = false;
	VoxelizeTrianglesResult voxelResult;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::Voxelize");
		voxelResult = VoxelizeTrianglesDetailed(voxelInput);
	}
	if (voxelResult.renderPayload.activeCells.empty())
	{
		return artifacts;
	}

	const float voxelRepresentationError = ComputeVoxelRepresentationError(voxelWidth);
	PackVoxelGroupInput packInput{};
	packInput.payload = &voxelResult.renderPayload;
	packInput.voxelError = voxelRepresentationError;
	packInput.opacityThreshold = settings.voxelFallbackOpacityThreshold;
	packInput.dominantBoneIndex = CLOD_VOXEL_STATIC_BONE_INDEX;
	PackedVoxelGroupBuildResult packed;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::PackAndCluster");
		packed = PackVoxelGroupToCubes(packInput);
		BuildVoxelClustersFromCubes(packed, std::max(1u, maxCubesPerCluster));
	}
	if (packed.cubeRecords.empty() || packed.clusterRecords.empty())
	{
		return artifacts;
	}

	std::vector<ClusterLODGroupSegment> voxelSegments;
	std::vector<BoundingSphere> voxelSegmentBounds;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::SplitVoxelPageSegments");
		SplitVoxelClustersIntoPageSegments(packed, voxelSegments, voxelSegmentBounds, settings.nodeBoneLimit);
	}
	std::vector<std::vector<std::byte>> voxelPageBlobs;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::BuildVoxelPageBlobs");
		voxelPageBlobs = BuildVoxelGroupPageBlobs(
			voxelSegments,
			packed.clusterRecords,
			packed.cubeRecords,
			packed.attributeSamples,
			0u,
			settings.nodeBoneLimit);
	}
	if (voxelSegments.empty() || voxelPageBlobs.empty())
	{
		return artifacts;
	}

	ClusterLODBuildState state{};
	ClusterLODGroup group{};
	const DirectX::XMFLOAT3 payloadMin = voxelResult.renderPayload.aabbMin;
	const DirectX::XMFLOAT3 payloadMax = voxelResult.renderPayload.aabbMax;
	const float centerX = 0.5f * (payloadMin.x + payloadMax.x);
	const float centerY = 0.5f * (payloadMin.y + payloadMax.y);
	const float centerZ = 0.5f * (payloadMin.z + payloadMax.z);
	const float dx = payloadMax.x - centerX;
	const float dy = payloadMax.y - centerY;
	const float dz = payloadMax.z - centerZ;
	group.bounds.center[0] = centerX;
	group.bounds.center[1] = centerY;
	group.bounds.center[2] = centerZ;
	group.bounds.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
	group.bounds.error = std::numeric_limits<float>::max();
	group.depth = 0;
	group.firstSegment = 0u;
	group.segmentCount = static_cast<uint32_t>(voxelSegments.size());
	group.terminalSegmentCount = group.segmentCount;
	group.flags = CLOD_GROUP_FLAG_IS_VOXEL;
	group.pageCount = static_cast<uint32_t>(voxelPageBlobs.size());
	group.representationError = voxelRepresentationError;

	state.groups.push_back(group);
	state.segments = std::move(voxelSegments);
	state.segmentBounds = std::move(voxelSegmentBounds);
	state.groupChunks.resize(1);
	state.groupPageBlobs.resize(1);
	state.groupPageBlobs[0] = std::move(voxelPageBlobs);
	state.voxelGroupMapping.groupToPayloadIndex = { 0 };
	state.voxelGroupMapping.groupToPackedMetadataIndex = { 0 };
	state.voxelGroupMapping.payloads.push_back(std::move(voxelResult.renderPayload));
	state.voxelGroupMapping.packedGroupMetadata.push_back(packed.metadata);
	state.voxelGroupMapping.packedClusterRecords = std::move(packed.clusterRecords);
	state.voxelGroupMapping.packedCubeRecords = std::move(packed.cubeRecords);
	state.voxelGroupMapping.packedAttributeSamples = std::move(packed.attributeSamples);

	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::BuildTraversalHierarchy");
		BuildClusterLODTraversalHierarchy(state, /*preferredNodeWidth=*/8u);
	}
	std::vector<ClusterLODNodeSkinningInfo> nodeSkinningInfos;
	std::vector<uint32_t> nodeBoneIndices;
	BuildNodeSkinningSidecar(state, settings.nodeBoneLimit, nodeSkinningInfos, nodeBoneIndices);

	std::vector<std::vector<std::byte>> meshPageBlobs;
	std::vector<uint32_t> groupPageReferences;
	std::vector<uint32_t> groupPageReferenceOffsets;
	uint32_t trianglePageCount = 0u;
	uint32_t voxelPageBase = 0u;
	uint32_t voxelPageCount = 0u;
	{
		ZoneScopedN("ClusterLODUtilities::VoxelOnlyGeometry::FinalizeMeshWidePagePacking");
		FinalizeMeshWidePagePacking(
			state,
			meshPageBlobs,
			groupPageReferences,
			groupPageReferenceOffsets,
			trianglePageCount,
			voxelPageBase,
			voxelPageCount);
	}

	artifacts.prebuiltData.groups = std::move(state.groups);
	artifacts.prebuiltData.segments = std::move(state.segments);
	artifacts.prebuiltData.segmentBounds = std::move(state.segmentBounds);
	artifacts.prebuiltData.objectBoundingSphere = BuildObjectBoundingSphereFromRootNode(state.nodes, state.topRootNode);
	artifacts.prebuiltData.groupChunks = std::move(state.groupChunks);
	artifacts.prebuiltData.groupPageReferences = std::move(groupPageReferences);
	artifacts.prebuiltData.groupPageReferenceOffsets = std::move(groupPageReferenceOffsets);
	artifacts.prebuiltData.trianglePageCount = trianglePageCount;
	artifacts.prebuiltData.voxelPageBase = voxelPageBase;
	artifacts.prebuiltData.voxelPageCount = voxelPageCount;
	artifacts.prebuiltData.nodes = std::move(state.nodes);
	artifacts.prebuiltData.nodeSkinningInfos = std::move(nodeSkinningInfos);
	artifacts.prebuiltData.nodeBoneIndices = std::move(nodeBoneIndices);
	artifacts.prebuiltData.nodeBoneLimit = std::clamp(settings.nodeBoneLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
	artifacts.prebuiltData.lodNodeRanges = std::move(state.lodNodeRanges);
	artifacts.prebuiltData.lodLevelRoots = std::move(state.lodLevelRoots);
	artifacts.prebuiltData.maxDepth = state.maxDepth;
	artifacts.prebuiltData.maxTraversalDepth = state.maxTraversalDepth;
	AssignSingleRootPartRecord(artifacts.prebuiltData, state.topRootNode);
	artifacts.cacheBuildData.groupPageBlobs = std::move(state.groupPageBlobs);
	artifacts.cacheBuildData.voxelGroupMapping = std::move(state.voxelGroupMapping);
	artifacts.cacheBuildData.meshPageBlobs = std::move(meshPageBlobs);
	std::string representationError;
	if (!ValidateClusterLODPageRepresentations(
		artifacts.prebuiltData,
		&artifacts.cacheBuildData.meshPageBlobs,
		&representationError))
	{
		spdlog::error("ClusterLOD voxel-only page-representation validation failed: {}", representationError);
		throw std::runtime_error("ClusterLOD voxel-only page-representation validation failed: " + representationError);
	}
	EmitClusterLODPagePackingTelemetry(
		"voxel_geometry",
		artifacts.prebuiltData,
		artifacts.cacheBuildData.meshPageBlobs);

	return artifacts;
}
