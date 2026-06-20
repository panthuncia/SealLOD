#pragma once

#include "Mesh/ClusterLODTypes.h"
#include "Mesh/VoxelGroupBuilder.h"

ClusterLODPrebuildArtifacts BuildClusterLODArtifactsFromGeometry(
	const std::vector<std::byte>& vertices,
	unsigned int vertexSize,
	const std::vector<std::byte>* skinningVertices,
	unsigned int skinningVertexSize,
	const std::vector<uint32_t>& indices,
	const std::vector<MeshUvSetData>& uvSets,
	unsigned int flags,
	const ClusterLODBuilderSettings& settings);

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
