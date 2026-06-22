// MeshIngestBuilder out-of-line methods that have no GPU dependencies.
// The GPU-dependent Build() method is in Mesh.cpp.

#include "Mesh/ClusterLODTypes.h"
#include "Mesh/ClusterLODUtilities.h"

ClusterLODPrebuildArtifacts MeshIngestBuilder::BuildClusterLODArtifacts() const {
	const std::vector<std::byte>* skinningVertices = m_skinningVertices.empty() ? nullptr : &m_skinningVertices;
	return BuildClusterLODArtifactsFromGeometry(
		m_vertices,
		m_vertexSize,
		skinningVertices,
		m_skinningVertexSize,
		m_indices,
		m_uvSets,
		m_flags,
		m_clusterLODBuilderSettings);
}

ClusterLODPrebuildArtifacts MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifacts(uint32_t maxCubesPerCluster) const {
	return BuildVoxelOnlyClusterLODArtifactsFromGeometry(
		m_vertices,
		m_vertexSize,
		m_indices,
		m_clusterLODBuilderSettings,
		maxCubesPerCluster);
}

ClusterLODPrebuildArtifacts MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifacts(
	const ClusterLODVoxelGridOverride& grid,
	uint32_t maxCubesPerCluster) const {
	return BuildVoxelOnlyClusterLODArtifactsFromGeometry(
		m_vertices,
		m_vertexSize,
		m_indices,
		m_clusterLODBuilderSettings,
		grid,
		maxCubesPerCluster);
}

VoxelGroupPayload MeshIngestBuilder::BuildVoxelOnlyPayload(const ClusterLODVoxelGridOverride& grid) const {
	return BuildVoxelOnlyPayload(grid, nullptr);
}

VoxelGroupPayload MeshIngestBuilder::BuildVoxelOnlyPayload(
	const ClusterLODVoxelGridOverride& grid,
	const VoxelCoverageMaterialSampler* coverageMaterialSampler) const {
	VoxelSourceTriangleBVH coverageSourceTriangles;
	coverageSourceTriangles.Build(
		&m_vertices,
		m_vertexSize,
		&m_indices,
		nullptr,
		0u,
		nullptr,
		m_clusterLODBuilderSettings.doubleSidedVoxelSourceNormals);

	VoxelizeTrianglesInput voxelInput{};
	voxelInput.vertices = &m_vertices;
	voxelInput.vertexStrideBytes = m_vertexSize;
	voxelInput.triangleIndices = &m_indices;
	voxelInput.doubleSidedTriangles = m_clusterLODBuilderSettings.doubleSidedVoxelSourceNormals;
	voxelInput.coverageSourceTriangles = coverageSourceTriangles.IsValid() ? &coverageSourceTriangles : nullptr;
	voxelInput.coverageMaterialSampler = coverageMaterialSampler;
	voxelInput.keepZeroCoverageSourceCells = m_clusterLODBuilderSettings.voxelFallbackCarryZeroCoverage;
	voxelInput.aabbMin = grid.aabbMin;
	voxelInput.aabbMax = grid.aabbMax;
	voxelInput.voxelWidth = grid.voxelWidth;
	voxelInput.resolution = grid.resolution;
	voxelInput.raysPerCell = m_clusterLODBuilderSettings.voxelRaysPerCell;
	voxelInput.pruningMode = m_clusterLODBuilderSettings.voxelFallbackPruningMode;
	return VoxelizeTrianglesDetailed(voxelInput).renderPayload;
}

ClusterLODPrebuildArtifacts MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifactsFromPayload(
	const VoxelGroupPayload& payload,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster) {
	return ::BuildVoxelOnlyClusterLODArtifactsFromPayload(payload, settings, maxCubesPerCluster);
}
