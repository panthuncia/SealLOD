#pragma once

#include <directxmath.h>
#include <vector>
#include <memory>
#include <atomic>
#include <optional>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <array>
#include <rhi.h>
#include <meshoptimizer.h>

#include "Mesh/ClusterLODTypes.h"
#include "Import/MeshData.h"
#include "Materials/MaterialDescription.h"
#include "ShaderBuffers.h"
#include "Resources/Buffers/BufferView.h"
#include "Managers/Singletons/DeletionManager.h"

class MeshManager;
class Skeleton;
class Buffer;

class Mesh {
public:
	using SparseChunkViewTable = std::unordered_map<uint32_t, std::unique_ptr<BufferView>>;

	struct ObjectReyesAtlasBakeData {
		std::uint32_t atlasWidth = 0;
		std::uint32_t atlasHeight = 0;
		std::uint32_t atlasUvSetIndex = 0;
		float texelsPerUnit = 1.0f;
		std::vector<DirectX::XMFLOAT3> positions;
		std::vector<DirectX::XMFLOAT3> normals;
		std::vector<DirectX::XMFLOAT2> atlasUvs;
		std::vector<std::uint32_t> indices;
		std::vector<std::uint32_t> triangleMaterialIndices;
		std::vector<std::string> sourceMaterialNames;
		std::vector<MaterialDescription> sourceMaterials;
	};

	~Mesh()
	{
	}
	static std::shared_ptr<Mesh> CreateShared(std::unique_ptr<std::vector<std::byte>> vertices, unsigned int vertexSize, std::optional<std::unique_ptr<std::vector<std::byte>>> skinningVertices, unsigned int skinningVertexSize, const std::vector<UINT32>& indices, std::vector<MeshUvSetData>&& uvSets, const std::shared_ptr<Material> material, unsigned int flags, std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD = std::nullopt, MeshCpuDataPolicy cpuDataPolicy = MeshCpuDataPolicy::Retain) {
		return std::shared_ptr<Mesh>(new Mesh(std::move(vertices), vertexSize, std::move(skinningVertices), skinningVertexSize, indices, std::move(uvSets), material, flags, std::move(prebuiltClusterLOD), cpuDataPolicy));
    }
	static std::shared_ptr<Mesh> CreateSharedFromIngest(std::unique_ptr<std::vector<std::byte>> vertices, unsigned int vertexSize, std::optional<std::unique_ptr<std::vector<std::byte>>> skinningVertices, unsigned int skinningVertexSize, std::vector<UINT32>&& indices, std::vector<MeshUvSetData>&& uvSets, const std::shared_ptr<Material> material, unsigned int flags, std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD = std::nullopt, MeshCpuDataPolicy cpuDataPolicy = MeshCpuDataPolicy::Retain) {
		return std::shared_ptr<Mesh>(new Mesh(std::move(vertices), vertexSize, std::move(skinningVertices), skinningVertexSize, indices, std::move(uvSets), material, flags, std::move(prebuiltClusterLOD), cpuDataPolicy));
	}
	uint64_t GetNumVertices() const {
		return static_cast<uint64_t>(m_perMeshBufferData.numVertices);
	}

	uint32_t GetCLodGroupCount() const {
		return static_cast<uint32_t>(m_clodRuntimeSummary.groupChunkHints.size());
	}

    bool IsCLodMesh() const {
        return GetCLodGroupCount() > 0u;
    }

	PerMeshCB& GetPerMeshCBData() { return m_perMeshBufferData; };
	const PerMeshCB& GetPerMeshCBData() const { return m_perMeshBufferData; };
	uint64_t GetGlobalID() const;

    std::shared_ptr<Material> material;

	void SetBaseSkin(std::shared_ptr<Skeleton> skeleton);
	bool HasBaseSkin() const { return m_baseSkeleton != nullptr; }
	std::shared_ptr<Skeleton> GetBaseSkin() const { return m_baseSkeleton; }
	void SetSkinJointNames(std::vector<std::string> names) { m_skinJointNames = std::move(names); }
	const std::vector<std::string>& GetSkinJointNames() const { return m_skinJointNames; }
	void SetSkinJointSourceIndices(std::vector<std::uint32_t> indices) { m_skinJointSourceIndices = std::move(indices); }
	const std::vector<std::uint32_t>& GetSkinJointSourceIndices() const { return m_skinJointSourceIndices; }
	void SetSkinInverseBindMatrices(std::vector<DirectX::XMMATRIX> matrices) { m_skinInverseBindMatrices = std::move(matrices); }
	const std::vector<DirectX::XMMATRIX>& GetSkinInverseBindMatrices() const { return m_skinInverseBindMatrices; }
	void SetSkinningDebugSample(std::vector<std::uint32_t> joints, std::vector<float> weights, std::vector<float> positions = {}, std::vector<float> normals = {})
	{
		m_skinningDebugJoints = std::move(joints);
		m_skinningDebugWeights = std::move(weights);
		m_skinningDebugPositions = std::move(positions);
		m_skinningDebugNormals = std::move(normals);
	}
	const std::vector<std::uint32_t>& GetSkinningDebugJoints() const { return m_skinningDebugJoints; }
	const std::vector<float>& GetSkinningDebugWeights() const { return m_skinningDebugWeights; }
	const std::vector<float>& GetSkinningDebugPositions() const { return m_skinningDebugPositions; }
	const std::vector<float>& GetSkinningDebugNormals() const { return m_skinningDebugNormals; }

	uint32_t GetCLodMeshletCount() {
		return m_perMeshBufferData.clodNumMeshlets;
	}

	void SetPerMeshBufferView(std::unique_ptr<BufferView> view) {
		m_perMeshBufferView = std::move(view);
	}

	std::unique_ptr<BufferView>& GetPerMeshBufferView() {
		return m_perMeshBufferView;
	}

	void SetCurrentMeshManager(MeshManager* manager) {
		m_pCurrentMeshManager = manager;
	}

	unsigned int GetSkinningVertexSize() const {
		return m_skinningVertexSize;
	}

	BoundingSphere GetAnimatedBoundingSphere(size_t animationIndex) const;

	void SetMaterialDataIndex(unsigned int index);
	void SetMaterialEvalCompileFlagsID(unsigned int index);
	void SetMaterialReyesEvalCompileFlagsID(unsigned int index);
	void SetRasterBucketIndex(unsigned int index);

	void SetCLodBufferViews(
		std::unique_ptr<BufferView> clusterLODGroupsView,
		std::unique_ptr<BufferView> clusterLODSegmentsView,
		std::unique_ptr<BufferView> clodNodesView
	);

	const std::vector<ClusterLODGroup>& GetCLodGroups() const {
		return m_clodGroups;
	}

	const std::vector<ClusterLODGroupSegment>& GetCLodSegments() const {
		return m_clodSegments;
	}

	const ClusterLODRuntimeSummary& GetCLodRuntimeSummary() const {
		return m_clodRuntimeSummary;
	}

	const std::vector<ClusterLODRuntimeSummary::GroupChunkHint>& GetCLodGroupChunkHints() const {
		return m_clodRuntimeSummary.groupChunkHints;
	}

	const std::vector<ClusterLODGroupDiskLocator>& GetCLodGroupDiskLocators() const {
		return m_clodGroupDiskLocators;
	}

	const std::vector<ClusterLODGroupDiskLocator>& GetCLodPageDiskLocators() const {
		return m_clodPageDiskLocators;
	}

	const std::vector<uint32_t>& GetCLodGroupPageReferences() const {
		return m_clodGroupPageReferences;
	}

	const std::vector<uint32_t>& GetCLodGroupPageReferenceOffsets() const {
		return m_clodGroupPageReferenceOffsets;
	}

	const ClusterLODCacheSource& GetCLodCacheSource() const {
		return m_clodCacheSource;
	}

	bool HasCLodDiskStreamingSource() const {
		return !m_clodCacheSource.containerFileName.empty();
	}

	void AdoptCLodDiskStreamingMetadata(const ClusterLODPrebuiltData& data);

	void ReleaseCLodChunkUploadData();
	void ReleaseCLodHierarchyCpuData();
	void ReleaseCLodGroupChunkMetadataCpuData();

	const std::vector<ClusterLODNode>& GetCLodNodes() const {
		return m_clodNodes;
	}

	const std::vector<ClusterLODNodeRangeAlloc>& GetCLodLodNodeRanges() const {
		return m_clodLodNodeRanges;
	}

	const std::vector<uint32_t>& GetCLodLodLevelRoots() const {
		return m_clodLodLevelRoots;
	}

	uint32_t GetCLodMaxDepth() const {
		return m_clodMaxDepth;
	}

	uint32_t GetCLodMaxTraversalDepth() const {
		return m_clodMaxTraversalDepth;
	}

	const BufferView* GetCLodGroupsView() const {
		return m_clusterLODGroupsView.get();
	}

	const BufferView* GetCLodSegmentsView() const {
		return m_clusterLODSegmentsView.get();
	}

	const BufferView* GetCLodNodesView() const {
		return m_clusterLODNodesView.get();
	}

	uint32_t GetCLodRootNodeIndex() const { // For hierarchy cut
		return m_clodTopRootNode;
	}

	uint32_t GetCoarsestLODRootNodeIndex() const { // For DAG traversal
		return m_clodLodLevelRoots.back();
	}

	const std::vector<MeshUvSetData>& GetUvSets() const {
		return m_uvSets;
	}

	void SetObjectReyesAtlasBakeData(std::shared_ptr<const ObjectReyesAtlasBakeData> data) {
		m_objectReyesAtlasBakeData = std::move(data);
	}

	std::shared_ptr<const ObjectReyesAtlasBakeData> GetObjectReyesAtlasBakeData() const {
		return m_objectReyesAtlasBakeData;
	}

	DirectX::XMFLOAT2 EstimateReyesUvDensity(uint32_t uvSetIndex) const;

	ClusterLODPrebuiltData GetClusterLODPrebuiltData() const;
	ClusterLODCacheBuildPayload GetClusterLODCacheBuildPayload() const;
	ClusterLODCacheBuildOwnedData GetClusterLODCacheBuildOwnedData() const;

private:
	Mesh(std::unique_ptr<std::vector<std::byte>> vertices, unsigned int vertexSize, std::optional<std::unique_ptr<std::vector<std::byte>>> skinningVertices, unsigned int skinningVertexSize, const std::vector<UINT32>& indices, std::vector<MeshUvSetData>&& uvSets, const std::shared_ptr<Material>, unsigned int flags, std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD, MeshCpuDataPolicy cpuDataPolicy, bool deferResourceCreation = false);
    void CreateBuffers(const std::vector<UINT32>& indices);
	void ApplyPrebuiltClusterLODData(const ClusterLODPrebuiltData& data);
	void ClearCLodCacheBuildChunkData(bool shrinkToFit);
	void EnsureAnimatedBoundingSpheresBuilt_() const;
	static SparseChunkViewTable ToSparseChunkViewTable(std::vector<std::unique_ptr<BufferView>>&& denseViews) {
		SparseChunkViewTable sparseViews;
		sparseViews.reserve(denseViews.size());
		for (uint32_t i = 0; i < denseViews.size(); ++i) {
			if (denseViews[i] != nullptr) {
				sparseViews.emplace(i, std::move(denseViews[i]));
			}
		}
		return sparseViews;
	}
	static const BufferView* GetChunkViewAt(const SparseChunkViewTable& views, uint32_t groupIndex) {
		auto it = views.find(groupIndex);
		return (it != views.end() && it->second != nullptr) ? it->second.get() : nullptr;
	}
	static void SetChunkViewAt(SparseChunkViewTable& views, uint32_t groupIndex, std::unique_ptr<BufferView> view) {
		if (view == nullptr) {
			views.erase(groupIndex);
			return;
		}
		views[groupIndex] = std::move(view);
	}
    static int GetNextGlobalIndex();

    static std::atomic<uint32_t> globalMeshCount;
	uint32_t m_globalMeshID;

	// TODO: packing
	std::vector<ClusterLODGroup> m_clodGroups;
	std::vector<ClusterLODGroupSegment> m_clodSegments;
	std::vector<ClusterLODGroupChunk> m_clodGroupChunks;
	ClusterLODRuntimeSummary m_clodRuntimeSummary;
	struct ClusterLODCacheBuildChunkData {
		std::vector<std::vector<std::vector<std::byte>>> groupPageBlobs;
		std::vector<std::vector<std::byte>> meshPageBlobs;
	} m_clodCacheBuildChunkData;
	std::vector<ClusterLODGroupDiskLocator> m_clodGroupDiskLocators;
	std::vector<ClusterLODGroupDiskLocator> m_clodPageDiskLocators;
	std::vector<uint32_t> m_clodGroupPageReferences;
	std::vector<uint32_t> m_clodGroupPageReferenceOffsets;
	uint32_t m_clodTrianglePageCount = 0;
	uint32_t m_clodVoxelPageBase = 0;
	uint32_t m_clodVoxelPageCount = 0;
	ClusterLODCacheSource m_clodCacheSource;

	std::vector<ClusterLODNode>      m_clodNodes;
	std::vector<ClusterLODNodeRangeAlloc> m_clodLodNodeRanges;  // per depth
	std::vector<uint32_t>            m_clodLodLevelRoots;        // node index per depth (== 1+depth)
	uint32_t                         m_clodTopRootNode = 0;      // always 0
	uint32_t                         m_clodMaxDepth = 0;
	uint32_t                         m_clodMaxTraversalDepth = 0;
	std::optional<ClusterLODPrebuiltData> m_prebuiltClusterLOD;

	std::unique_ptr<BufferView> m_clusterLODGroupsView = nullptr;
	std::unique_ptr<BufferView> m_clusterLODSegmentsView = nullptr;
	std::unique_ptr<BufferView> m_clusterLODNodesView = nullptr;

	//UINT m_indexCount = 0;
    //std::shared_ptr<Buffer> m_vertexBufferHandle;
	//std::shared_ptr<Buffer> m_indexBufferHandle;
    //rhi::VertexBufferView m_vertexBufferView;
    //rhi::IndexBufferView m_indexBufferView;

	PerMeshCB m_perMeshBufferData = { 0 };
	unsigned int m_skinningVertexSize = 0;
	DirectX::XMFLOAT2 m_reyesUvDensityBySet[8] = {};
	std::vector<MeshUvSetData> m_uvSets;
	std::shared_ptr<const ObjectReyesAtlasBakeData> m_objectReyesAtlasBakeData;
	mutable std::vector<BoundingSphere> m_animationBoundingSpheres;
	std::unique_ptr<BufferView> m_perMeshBufferView;
	MeshManager* m_pCurrentMeshManager = nullptr;

	std::shared_ptr<Skeleton> m_baseSkeleton = nullptr;
	std::vector<std::string> m_skinJointNames;
	std::vector<std::uint32_t> m_skinJointSourceIndices;
	std::vector<DirectX::XMMATRIX> m_skinInverseBindMatrices;
	std::vector<std::uint32_t> m_skinningDebugJoints;
	std::vector<float> m_skinningDebugWeights;
	std::vector<float> m_skinningDebugPositions;
	std::vector<float> m_skinningDebugNormals;
};
