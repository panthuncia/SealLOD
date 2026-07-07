#pragma once

#include "Mesh/ClusterLODTypes.h"
#include "Mesh/VoxelGroupBuilder.h"

#include <span>

ClusterLODPrebuildArtifacts BuildClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<std::byte>* skinningVertices,
	unsigned int skinningVertexSize,
	const std::vector<uint32_t>& indices,
	const std::vector<MeshUvSetData>& uvSets,
	unsigned int flags,
	const ClusterLODBuilderSettings& settings,
	const VoxelCoverageMaterialSampler* coverageMaterialSampler = nullptr);

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<uint32_t>& indices,
	const ClusterLODBuilderSettings& settings,
	const std::optional<ClusterLODVoxelGridOverride>& gridOverride,
	uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<uint32_t>& indices,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);

ClusterLODPrebuildArtifacts BuildVoxelOnlyClusterLODArtifactsFromPayload(
	const VoxelGroupPayload& payload,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster = CLOD_VOXEL_MAX_CUBES_PER_CLUSTER);

struct ClusterLODAssemblyPart
{
	const ClusterLODPrebuildArtifacts* artifacts = nullptr;
	const std::vector<std::byte>* coverageVertices = nullptr;
	const std::vector<uint32_t>* coverageIndices = nullptr;
	unsigned int coverageVertexSize = 0;
	bool doubleSidedCoverageTriangles = false;
};

struct ClusterLODAssemblyInstanceSpec
{
	uint32_t partIndex = 0;
	uint32_t rootNode = 0;
	ClusterLODAssemblyTransform transform{};
	uint32_t flags = 0;
};

ClusterLODPrebuildArtifacts BuildClusterLODAssemblyArtifacts(
	std::span<const ClusterLODAssemblyPart> parts,
	std::span<const ClusterLODAssemblyInstanceSpec> instances,
	const ClusterLODBuilderSettings& settings,
	uint32_t preferredNodeWidth = 8u,
	bool synthesizeVoxelParents = true);
