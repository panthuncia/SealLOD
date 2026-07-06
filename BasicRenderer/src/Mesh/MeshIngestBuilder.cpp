// MeshIngestBuilder out-of-line methods that have no GPU dependencies.
// The GPU-dependent Build() method is in Mesh.cpp.

#include "Mesh/ClusterLODTypes.h"
#include "Mesh/ClusterLODUtilities.h"

#include <tracy/Tracy.hpp>

ClusterLODPrebuildArtifacts MeshIngestBuilder::BuildClusterLODArtifacts(
	const VoxelCoverageMaterialSampler* coverageMaterialSampler) const {
	ZoneScopedN("MeshIngestBuilder::BuildClusterLODArtifacts");
	TracyPlot("CLOD.Ingest.Vertices", static_cast<int64_t>(m_vertexSize != 0u ? m_vertices.size() / m_vertexSize : 0u));
	TracyPlot("CLOD.Ingest.Triangles", static_cast<int64_t>(m_indices.size() / 3u));
	const std::vector<std::byte>* skinningVertices = m_skinningVertices.empty() ? nullptr : &m_skinningVertices;
	return BuildClusterLODArtifactsFromGeometry(
		m_vertices,
		m_vertexSize,
		skinningVertices,
		m_skinningVertexSize,
		m_indices,
		m_uvSets,
		m_flags,
		m_clusterLODBuilderSettings,
		coverageMaterialSampler);
}

ClusterLODPrebuildArtifacts MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifacts(uint32_t maxCubesPerCluster) const {
	ZoneScopedN("MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifacts");
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
	ZoneScopedN("MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifacts::Grid");
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
	ZoneScopedN("MeshIngestBuilder::BuildVoxelOnlyPayload");
	VoxelSourceTriangleBVH coverageSourceTriangles;
	{
		ZoneScopedN("MeshIngestBuilder::BuildVoxelOnlyPayload::BuildCoverageBVH");
		coverageSourceTriangles.Build(
			&m_vertices,
			m_vertexSize,
			&m_indices,
			nullptr,
			0u,
			nullptr,
			m_clusterLODBuilderSettings.doubleSidedVoxelSourceNormals);
	}

	VoxelizeTrianglesInput voxelInput{};
	voxelInput.vertices = &m_vertices;
	voxelInput.vertexStrideBytes = m_vertexSize;
	voxelInput.triangleIndices = &m_indices;
	voxelInput.doubleSidedTriangles = m_clusterLODBuilderSettings.doubleSidedVoxelSourceNormals;
	voxelInput.coverageSourceTriangles = coverageSourceTriangles.IsValid() ? &coverageSourceTriangles : nullptr;
	voxelInput.coverageMaterialSampler = coverageMaterialSampler;
	voxelInput.aabbMin = grid.aabbMin;
	voxelInput.aabbMax = grid.aabbMax;
	voxelInput.voxelWidth = grid.voxelWidth;
	voxelInput.resolution = grid.resolution;
	voxelInput.raysPerCell = m_clusterLODBuilderSettings.voxelRaysPerCell;
	voxelInput.emitSourcePayload = false;
	return VoxelizeTrianglesDetailed(voxelInput).renderPayload;
}

ClusterLODPrebuildArtifacts MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifactsFromPayload(
	const VoxelGroupPayload& payload,
	const ClusterLODBuilderSettings& settings,
	uint32_t maxCubesPerCluster) {
	ZoneScopedN("MeshIngestBuilder::BuildVoxelOnlyClusterLODArtifactsFromPayload");
	return ::BuildVoxelOnlyClusterLODArtifactsFromPayload(payload, settings, maxCubesPerCluster);
}
