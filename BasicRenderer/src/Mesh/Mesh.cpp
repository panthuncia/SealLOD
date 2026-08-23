#include "Mesh/Mesh.h"
#include "Mesh/ClusterLODUtilities.h"

#include <limits>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <bit>
#include <cmath>
#include <array>
#include <functional>
#include <cstring>
#include <mutex>
#include <cassert>
#include <set>
#include <span>

#include <spdlog/spdlog.h>

#include "Utilities/Utilities.h"
#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Render/PSOFlags.h"
#include "Managers/Singletons/ResourceManager.h"
#include "Materials/Material.h"
#include "Mesh/VertexFlags.h"
#include "Managers/MeshManager.h"
#include "Animation/Skeleton.h"

#include "../shaders/Common/defines.h"

std::atomic<uint32_t> Mesh::globalMeshCount = 0;

namespace
{
	constexpr float kAnimatedBoundsPaddingScale = 1.0001f;
	constexpr float kAnimationBoundsMaxUniformSampleStep = 1.0f / 30.0f;
	constexpr uint32_t kAnimationBoundsMaxUniformSamples = 256u;

	DirectX::XMFLOAT3 ReadPositionFromVertexBytes(const std::vector<std::byte>& vertices, uint32_t vertexStrideBytes, uint32_t vertexIndex)
	{
		DirectX::XMFLOAT3 position{};
		const size_t offset = static_cast<size_t>(vertexIndex) * static_cast<size_t>(vertexStrideBytes);
		if (vertexStrideBytes >= sizeof(position) && offset + sizeof(position) <= vertices.size()) {
			std::memcpy(&position, vertices.data() + offset, sizeof(position));
		}
		return position;
	}

	DirectX::XMFLOAT3 SubFloat3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
	{
		return { a.x - b.x, a.y - b.y, a.z - b.z };
	}

	DirectX::XMFLOAT3 MulFloat3(const DirectX::XMFLOAT3& value, float scale)
	{
		return { value.x * scale, value.y * scale, value.z * scale };
	}

	float LengthFloat3(const DirectX::XMFLOAT3& value)
	{
		return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
	}

	DirectX::XMFLOAT2 EstimateUvDensityForSet(
		const std::vector<std::byte>& vertices,
		uint32_t vertexStrideBytes,
		const std::vector<UINT32>& indices,
		const MeshUvSetData& uvSet)
	{
		DirectX::XMFLOAT2 density{ 1.0f, 1.0f };
		if (vertexStrideBytes < sizeof(DirectX::XMFLOAT3) || indices.size() < 3u || uvSet.values.empty()) {
			return density;
		}

		const uint32_t vertexCount = static_cast<uint32_t>(
			std::min<size_t>(vertices.size() / static_cast<size_t>(vertexStrideBytes), uvSet.values.size()));
		for (size_t tri = 0; tri + 2u < indices.size(); tri += 3u) {
			const uint32_t i0 = indices[tri + 0u];
			const uint32_t i1 = indices[tri + 1u];
			const uint32_t i2 = indices[tri + 2u];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
				continue;
			}

			const DirectX::XMFLOAT3 p0 = ReadPositionFromVertexBytes(vertices, vertexStrideBytes, i0);
			const DirectX::XMFLOAT3 p1 = ReadPositionFromVertexBytes(vertices, vertexStrideBytes, i1);
			const DirectX::XMFLOAT3 p2 = ReadPositionFromVertexBytes(vertices, vertexStrideBytes, i2);
			const DirectX::XMFLOAT2 uv0 = uvSet.values[i0];
			const DirectX::XMFLOAT2 uv1 = uvSet.values[i1];
			const DirectX::XMFLOAT2 uv2 = uvSet.values[i2];

			const DirectX::XMFLOAT3 edge1 = SubFloat3(p1, p0);
			const DirectX::XMFLOAT3 edge2 = SubFloat3(p2, p0);
			const DirectX::XMFLOAT2 duv1{ uv1.x - uv0.x, uv1.y - uv0.y };
			const DirectX::XMFLOAT2 duv2{ uv2.x - uv0.x, uv2.y - uv0.y };
			const float det = duv1.x * duv2.y - duv2.x * duv1.y;
			if (std::abs(det) <= 1.0e-10f || !std::isfinite(det)) {
				continue;
			}

			const float invDet = 1.0f / det;
			const DirectX::XMFLOAT3 dPdu = MulFloat3(
				SubFloat3(MulFloat3(edge1, duv2.y), MulFloat3(edge2, duv1.y)),
				invDet);
			const DirectX::XMFLOAT3 dPdv = MulFloat3(
				SubFloat3(MulFloat3(edge2, duv1.x), MulFloat3(edge1, duv2.x)),
				invDet);
			const float lenU = LengthFloat3(dPdu);
			const float lenV = LengthFloat3(dPdv);
			if (std::isfinite(lenU) && lenU > 1.0e-6f) {
				density.x = std::max(density.x, 1.0f / lenU);
			}
			if (std::isfinite(lenV) && lenV > 1.0e-6f) {
				density.y = std::max(density.y, 1.0f / lenV);
			}
		}

		return density;
	}

	float MaxAxisScale_RowVector(const DirectX::XMMATRIX& matrix)
	{
		using namespace DirectX;

		const float sx = XMVectorGetX(XMVector3Length(matrix.r[0]));
		const float sy = XMVectorGetX(XMVector3Length(matrix.r[1]));
		const float sz = XMVectorGetX(XMVector3Length(matrix.r[2]));
		return std::max(sx, std::max(sy, sz));
	}

	BoundingSphere TransformBoundingSphere(const BoundingSphere& sphere, const DirectX::XMMATRIX& matrix)
	{
		using namespace DirectX;

		BoundingSphere transformed = sphere;
		const XMVECTOR center = XMVectorSet(sphere.sphere.x, sphere.sphere.y, sphere.sphere.z, 1.0f);
		const XMVECTOR transformedCenter = XMVector3TransformCoord(center, matrix);
		XMStoreFloat3(reinterpret_cast<DirectX::XMFLOAT3*>(&transformed.sphere), transformedCenter);
		transformed.sphere.w = sphere.sphere.w * MaxAxisScale_RowVector(matrix);
		return transformed;
	}

	BoundingSphere MergeBoundingSpheres(const BoundingSphere& lhs, const BoundingSphere& rhs)
	{
		using namespace DirectX;

		const XMVECTOR lhsCenter = XMVectorSet(lhs.sphere.x, lhs.sphere.y, lhs.sphere.z, 1.0f);
		const XMVECTOR rhsCenter = XMVectorSet(rhs.sphere.x, rhs.sphere.y, rhs.sphere.z, 1.0f);
		const float lhsRadius = lhs.sphere.w;
		const float rhsRadius = rhs.sphere.w;

		const XMVECTOR delta = XMVectorSubtract(rhsCenter, lhsCenter);
		const float dist = XMVectorGetX(XMVector3Length(delta));

		if (dist + rhsRadius <= lhsRadius) {
			return lhs;
		}
		if (dist + lhsRadius <= rhsRadius) {
			return rhs;
		}

		BoundingSphere merged{};
		const float newRadius = 0.5f * (dist + lhsRadius + rhsRadius);
		const float t = (dist > 1e-12f) ? ((newRadius - lhsRadius) / dist) : 0.0f;
		const XMVECTOR mergedCenter = XMVectorAdd(lhsCenter, XMVectorScale(delta, t));
		XMStoreFloat3(reinterpret_cast<DirectX::XMFLOAT3*>(&merged.sphere), mergedCenter);
		merged.sphere.w = newRadius;
		return merged;
	}

	uint32_t ComputeCLodTraversalDepth(const std::vector<ClusterLODNode>& nodes, uint32_t rootNodeIndex)
	{
		if (rootNodeIndex >= nodes.size()) {
			return 0u;
		}

		std::function<uint32_t(uint32_t)> computeNodeDepth = [&](uint32_t nodeIndex) -> uint32_t {
			if (nodeIndex >= nodes.size()) {
				return 0u;
			}

			const ClusterLODNode& node = nodes[nodeIndex];
			if (node.range.isGroup != 0u) {
				return 1u;
			}

			const uint32_t childCount = node.range.countMinusOne + 1u;
			uint32_t maxChildDepth = 0u;
			for (uint32_t childIndex = 0; childIndex < childCount; ++childIndex) {
				maxChildDepth = std::max(maxChildDepth, computeNodeDepth(node.range.indexOrOffset + childIndex));
			}

			return 1u + maxChildDepth;
		};

		return computeNodeDepth(rootNodeIndex);
	}

	void AppendClipKeyframeTimes(const std::shared_ptr<AnimationClip>& clip, std::vector<float>& sampleTimes)
	{
		if (!clip) {
			return;
		}

		auto appendKeyframes = [&sampleTimes](const std::vector<Keyframe>& keyframes) {
			for (const auto& keyframe : keyframes) {
				sampleTimes.push_back(keyframe.time);
			}
		};

		appendKeyframes(clip->positionKeyframes);
		appendKeyframes(clip->rotationKeyframes);
		appendKeyframes(clip->scaleKeyframes);
	}

	std::vector<float> BuildAnimationSampleTimes(const Animation& animation)
	{
		std::vector<float> sampleTimes;
		sampleTimes.push_back(0.0f);

		float duration = 0.0f;
		for (const auto& [_, clip] : animation.nodesMap) {
			AppendClipKeyframeTimes(clip, sampleTimes);
			if (clip) {
				duration = std::max(duration, clip->duration);
			}
		}

		if (duration > 0.0f) {
			sampleTimes.push_back(duration);
			const uint32_t uniformSampleCount = std::min<uint32_t>(
				kAnimationBoundsMaxUniformSamples,
				std::max<uint32_t>(1u, static_cast<uint32_t>(std::ceil(duration / kAnimationBoundsMaxUniformSampleStep))));
			for (uint32_t sampleIndex = 1; sampleIndex < uniformSampleCount; ++sampleIndex) {
				sampleTimes.push_back((duration * static_cast<float>(sampleIndex)) / static_cast<float>(uniformSampleCount));
			}
		}

		std::sort(sampleTimes.begin(), sampleTimes.end());
		sampleTimes.erase(
			std::unique(sampleTimes.begin(), sampleTimes.end(), [](float lhs, float rhs) {
				return std::abs(lhs - rhs) <= 1e-4f;
			}),
			sampleTimes.end());

		if (sampleTimes.empty()) {
			sampleTimes.push_back(0.0f);
		}

		return sampleTimes;
	}

	BoundingSphere BuildObjectBoundingSphereFromRootNode(const std::vector<ClusterLODNode>& nodes, uint32_t rootNodeIndex)
	{
		BoundingSphere sphere{};
		if (rootNodeIndex >= nodes.size()) {
			return sphere;
		}

		const ClusterLODTraversalMetric& metric = nodes[rootNodeIndex].traversalMetric;
		sphere.sphere = metric.cullingSphere;
		return sphere;
	}

	ClusterLODRuntimeSummary BuildRuntimeSummary(
		const std::vector<ClusterLODGroup>& groups,
		const std::vector<ClusterLODGroupSegment>& segments,
		const std::vector<ClusterLODGroupChunk>& groupChunks,
		std::span<const ClusterLODPartRecord> partRecords,
		uint32_t rootPartIndex)
	{
		ClusterLODRuntimeSummary summary{};
		summary.groupChunkHints.resize(groups.size());
		summary.parentGroupByLocal.assign(groups.size(), -1);
		summary.groupErrorByLocal.resize(groups.size(), 0.0f);
		summary.firstGroupVertexByLocal.resize(groups.size(), 0u);

		if (groups.empty()) {
			return summary;
		}

		for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
			const auto& group = groups[groupIndex];
			summary.firstGroupVertexByLocal[groupIndex] = group.firstGroupVertex;
			summary.groupErrorByLocal[groupIndex] = group.bounds.error;
			if (groupIndex < groupChunks.size()) {
				const auto& chunk = groupChunks[groupIndex];
				auto& hint = summary.groupChunkHints[groupIndex];
				hint.groupVertexCount = chunk.groupVertexCount;
				hint.meshletCount = chunk.meshletCount;
				hint.meshletTrianglesByteCount = chunk.meshletTrianglesByteCount;
				hint.segmentCount = group.segmentCount;
				hint.pageCount = group.pageCount;
			}
			else {
				auto& hint = summary.groupChunkHints[groupIndex];
				hint.groupVertexCount = group.groupVertexCount;
				hint.meshletCount = group.meshletCount;
				hint.segmentCount = group.segmentCount;
				hint.pageCount = group.pageCount;
			}
		}

		const uint32_t localGroupCount = static_cast<uint32_t>(groups.size());
		for (uint32_t groupLocalIndex = 0u; groupLocalIndex < localGroupCount; ++groupLocalIndex) {
			const auto& group = groups[groupLocalIndex];
			const uint32_t segBegin = group.firstSegment;
			const uint32_t segEnd = std::min<uint32_t>(segBegin + group.segmentCount, static_cast<uint32_t>(segments.size()));
			for (uint32_t segIndex = segBegin; segIndex < segEnd; ++segIndex) {
				const int32_t refinedGroupLocal = segments[segIndex].refinedGroup;
				if (refinedGroupLocal < 0) {
					continue;
				}

				const uint32_t refinedGroupLocalU32 = static_cast<uint32_t>(refinedGroupLocal);
				if (refinedGroupLocalU32 >= localGroupCount) {
					continue;
				}

				summary.parentGroupByLocal[refinedGroupLocalU32] = static_cast<int32_t>(groupLocalIndex);
			}
		}

		uint32_t rootGroupBegin = 0u;
		uint32_t rootGroupEnd = localGroupCount;
		if (!partRecords.empty() && rootPartIndex < partRecords.size()) {
			const ClusterLODPartRecord& rootPart = partRecords[rootPartIndex];
			rootGroupBegin = std::min(rootPart.groupBase, localGroupCount);
			const uint32_t availableGroups = localGroupCount - rootGroupBegin;
			rootGroupEnd = rootGroupBegin + std::min(rootPart.groupCount, availableGroups);
			if (rootGroupBegin == rootGroupEnd) {
				rootGroupBegin = 0u;
				rootGroupEnd = localGroupCount;
			}
		}

		uint32_t runStart = std::numeric_limits<uint32_t>::max();
		for (uint32_t groupLocalIndex = rootGroupBegin; groupLocalIndex < rootGroupEnd; ++groupLocalIndex) {
			const bool shouldPinForStreaming = summary.parentGroupByLocal[groupLocalIndex] < 0;
			if (shouldPinForStreaming) {
				if (runStart == std::numeric_limits<uint32_t>::max()) {
					runStart = groupLocalIndex;
				}
				continue;
			}

			if (runStart != std::numeric_limits<uint32_t>::max()) {
				summary.coarsestRanges.push_back({ runStart, groupLocalIndex - runStart });
				runStart = std::numeric_limits<uint32_t>::max();
			}
		}

		if (runStart != std::numeric_limits<uint32_t>::max()) {
			summary.coarsestRanges.push_back({
				runStart,
				rootGroupEnd - runStart });
		}

		return summary;
	}

	void EnsureRootPartRecord(
		std::vector<ClusterLODPartRecord>& partRecords,
		uint32_t& rootPartIndex,
		uint32_t groupCount,
		uint32_t nodeCount,
		uint32_t transformCount,
		uint32_t instanceCount,
		uint32_t rootNode)
	{
		if (partRecords.empty()) {
			ClusterLODPartRecord rootPart{};
			rootPart.groupBase = 0u;
			rootPart.groupCount = groupCount;
			rootPart.nodeBase = 0u;
			rootPart.nodeCount = nodeCount;
			rootPart.transformBase = 0u;
			rootPart.transformCount = transformCount;
			rootPart.instanceBase = 0u;
			rootPart.instanceCount = instanceCount;
			rootPart.rootNode = rootNode;
			rootPart.flags = CLOD_PART_RECORD_FLAG_ROOT;
			partRecords.push_back(rootPart);
			rootPartIndex = 0u;
			return;
		}

		if (rootPartIndex >= partRecords.size()) {
			rootPartIndex = 0u;
		}
		partRecords[rootPartIndex].flags |= CLOD_PART_RECORD_FLAG_ROOT;
	}

	bool HasRenderableImportedMeshPayload(
		unsigned int vertexSize,
		const std::optional<ClusterLODPrebuiltData>& prebuiltClusterLOD,
		const Material* material,
		const char*& outReason)
	{
		if (!prebuiltClusterLOD.has_value()) {
			outReason = "missing ClusterLOD payload";
			return false;
		}

		const auto& prebuilt = prebuiltClusterLOD.value();
		if (prebuilt.groups.empty()) {
			outReason = "empty ClusterLOD group payload";
			return false;
		}

		const uint32_t expectedPageCount = prebuilt.voxelPageBase + prebuilt.voxelPageCount;
		if (expectedPageCount == 0u || prebuilt.pageDiskLocators.size() != expectedPageCount) {
			outReason = "missing disk-backed ClusterLOD locators";
			return false;
		}

		if (prebuilt.cacheSource.containerFileName.empty()) {
			outReason = "missing ClusterLOD container file";
			return false;
		}

		outReason = nullptr;
		return true;
	}
}

std::shared_ptr<Mesh> MeshIngestBuilder::Build(
	const std::shared_ptr<Material>& material,
	std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD,
	MeshCpuDataPolicy cpuDataPolicy) {
	const char* discardReason = nullptr;
	if (!HasRenderableImportedMeshPayload(m_vertexSize, prebuiltClusterLOD, material.get(), discardReason)) {
		spdlog::warn(
			"Discarding imported mesh: {} (vertex_bytes={}, vertex_size={}, index_count={}, clod_groups={}, disk_locators={})",
			discardReason != nullptr ? discardReason : "invalid payload",
			m_vertices.size(),
			m_vertexSize,
			m_indices.size(),
			prebuiltClusterLOD.has_value() ? prebuiltClusterLOD->groups.size() : 0ull,
			prebuiltClusterLOD.has_value() ? prebuiltClusterLOD->pageDiskLocators.size() : 0ull);
		return nullptr;
	}

	auto vertices = std::make_unique<std::vector<std::byte>>(std::move(m_vertices));
	auto uvSets = std::move(m_uvSets);

	std::optional<std::unique_ptr<std::vector<std::byte>>> skinningVertices;
	if (!m_skinningVertices.empty()) {
		skinningVertices = std::make_unique<std::vector<std::byte>>(std::move(m_skinningVertices));
	}

	auto mesh = Mesh::CreateSharedFromIngest(
		std::move(vertices),
		m_vertexSize,
		std::move(skinningVertices),
		m_skinningVertexSize,
		std::move(m_indices),
		std::move(uvSets),
		material,
		m_flags,
		std::move(prebuiltClusterLOD),
		cpuDataPolicy);
	if (mesh && !m_skinningDebugJoints.empty() && !m_skinningDebugWeights.empty()) {
		mesh->SetSkinningDebugSample(
			std::move(m_skinningDebugJoints),
			std::move(m_skinningDebugWeights),
			std::move(m_skinningDebugPositions),
			std::move(m_skinningDebugNormals));
	}
	return mesh;
}

Mesh::Mesh(std::unique_ptr<std::vector<std::byte>> vertices, unsigned int vertexSize, std::optional<std::unique_ptr<std::vector<std::byte>>> skinningVertices, unsigned int skinningVertexSize, const std::vector<UINT32>& indices, std::vector<MeshUvSetData>&& uvSets, const std::shared_ptr<Material> material, unsigned int flags, std::optional<ClusterLODPrebuiltData>&& prebuiltClusterLOD, MeshCpuDataPolicy cpuDataPolicy, bool deferResourceCreation) {
	if (prebuiltClusterLOD.has_value()) {
		m_prebuiltClusterLOD = std::move(prebuiltClusterLOD);
	}
	m_uvSets = std::move(uvSets);
	const uint32_t cachedUvSetCount = static_cast<uint32_t>(
		std::min<size_t>(std::size(m_reyesUvDensityBySet), m_uvSets.size()));
	for (uint32_t uvSetIndex = 0; uvSetIndex < cachedUvSetCount; ++uvSetIndex) {
		m_reyesUvDensityBySet[uvSetIndex] = EstimateUvDensityForSet(*vertices, vertexSize, indices, m_uvSets[uvSetIndex]);
	}
	m_perMeshBufferData.vertexFlags = flags;
	m_perMeshBufferData.vertexByteSize = vertexSize;
	m_perMeshBufferData.numVertices = 0; //static_cast<uint32_t>(m_vertices->size() / vertexSize);
	m_perMeshBufferData.skinningVertexByteSize = skinningVertexSize;

	m_perMeshBufferData.vertexFlags = flags;
	m_perMeshBufferData.vertexByteSize = vertexSize;
	m_perMeshBufferData.numVertices = 0; // static_cast<uint32_t>(m_vertices->size() / vertexSize);
	m_perMeshBufferData.skinningVertexByteSize = skinningVertexSize;
	m_perMeshBufferData.clodMeshletBufferOffset = 0;
	m_perMeshBufferData.clodMeshletVerticesBufferOffset = 0;
	m_perMeshBufferData.clodMeshletTrianglesBufferOffset = 0;
	m_perMeshBufferData.clodNumMeshlets = 0;

	m_skinningVertexSize = skinningVertexSize;
    this->material = material;

	if (m_prebuiltClusterLOD.has_value()) {
		ApplyPrebuiltClusterLODData(*m_prebuiltClusterLOD);
	}

	if (!deferResourceCreation) {
		CreateBuffers(indices);
		if (cpuDataPolicy == MeshCpuDataPolicy::ReleaseAfterUpload && m_prebuiltClusterLOD.has_value()) {
			const auto& prebuilt = *m_prebuiltClusterLOD;
			const bool needsCloneableDiskMetadata =
				!prebuilt.pageDiskLocators.empty() &&
				!prebuilt.cacheSource.containerFileName.empty();
			if (!needsCloneableDiskMetadata) {
				m_prebuiltClusterLOD.reset();
			}
		}

		m_globalMeshID = GetNextGlobalIndex();
	}
	else {
		m_globalMeshID = 0;
	}
}

void Mesh::ClearCLodCacheBuildChunkData(bool shrinkToFit)
{
	auto clearChunkStorage = [shrinkToFit](auto& chunks) {
		chunks.clear();
		if (shrinkToFit) {
			chunks.shrink_to_fit();
		}
	};

	clearChunkStorage(m_clodCacheBuildChunkData.groupPageBlobs);
	clearChunkStorage(m_clodCacheBuildChunkData.meshPageBlobs);
}

void Mesh::ReleaseCLodChunkUploadData()
{
	ClearCLodCacheBuildChunkData(true);
}

void Mesh::ReleaseCLodHierarchyCpuData()
{
	m_clodGroups.clear();
	m_clodGroups.shrink_to_fit();
	m_clodSegments.clear();
	m_clodSegments.shrink_to_fit();
	m_clodNodes.clear();
	m_clodNodes.shrink_to_fit();
	m_clodNodeSkinningInfos.clear();
	m_clodNodeSkinningInfos.shrink_to_fit();
	m_clodNodeBoneIndices.clear();
	m_clodNodeBoneIndices.shrink_to_fit();
	m_clodAssemblyTransforms.clear();
	m_clodAssemblyTransforms.shrink_to_fit();
	m_clodAssemblyInstances.clear();
	m_clodAssemblyInstances.shrink_to_fit();
	m_clodAssemblyBoneRemaps.clear();
	m_clodAssemblyBoneRemaps.shrink_to_fit();
	m_clodAssemblyBoneRemapIndices.clear();
	m_clodAssemblyBoneRemapIndices.shrink_to_fit();
}

void Mesh::ReleaseCLodGroupChunkMetadataCpuData()
{
	m_clodGroupChunks.clear();
	m_clodGroupChunks.shrink_to_fit();
}

void Mesh::ApplyPrebuiltClusterLODData(const ClusterLODPrebuiltData& data)
{
	const bool hasDiskBackedStreamingSource = !data.pageDiskLocators.empty() && !data.cacheSource.containerFileName.empty();

	m_clodGroups = data.groups;
	m_clodSegments = data.segments;
	m_clodGroupChunks = data.groupChunks;
	if (hasDiskBackedStreamingSource && m_clodGroupChunks.size() != m_clodGroups.size()) {
		m_clodGroupChunks.assign(m_clodGroups.size(), {});
		for (size_t groupIndex = 0; groupIndex < m_clodGroups.size(); ++groupIndex) {
			m_clodGroupChunks[groupIndex].groupVertexCount = m_clodGroups[groupIndex].groupVertexCount;
			m_clodGroupChunks[groupIndex].meshletCount = m_clodGroups[groupIndex].meshletCount;
		}
	}
	ClearCLodCacheBuildChunkData(false);
	m_clodGroupDiskLocators = data.groupDiskLocators;
	m_clodPageDiskLocators = data.pageDiskLocators;
	m_clodGroupPageReferences = data.groupPageReferences;
	m_clodGroupPageReferenceOffsets = data.groupPageReferenceOffsets;
	m_clodTrianglePageCount = data.trianglePageCount;
	m_clodVoxelPageBase = data.voxelPageBase;
	m_clodVoxelPageCount = data.voxelPageCount;
	m_clodCacheSource = data.cacheSource;
	m_clodNodes = data.nodes;
	m_clodNodeSkinningInfos = data.nodeSkinningInfos;
	m_clodNodeBoneIndices = data.nodeBoneIndices;
	m_clodNodeBoneLimit = data.nodeBoneLimit;
	m_clodLodNodeRanges = data.lodNodeRanges;
	m_clodLodLevelRoots = data.lodLevelRoots;
	m_clodAssemblyTransforms = data.assemblyTransforms;
	m_clodAssemblyInstances = data.assemblyInstances;
	m_clodAssemblyBoneRemaps = data.assemblyBoneRemaps;
	m_clodAssemblyBoneRemapIndices = data.assemblyBoneRemapIndices;
	m_clodPartRecords = data.partRecords;
	m_clodRootPartIndex = data.rootPartIndex;
	m_clodTopRootNode = 0;
	EnsureRootPartRecord(
		m_clodPartRecords,
		m_clodRootPartIndex,
		static_cast<uint32_t>(m_clodGroups.size()),
		static_cast<uint32_t>(m_clodNodes.size()),
		static_cast<uint32_t>(m_clodAssemblyTransforms.size()),
		static_cast<uint32_t>(m_clodAssemblyInstances.size()),
		m_clodTopRootNode);
	m_clodRuntimeSummary = BuildRuntimeSummary(
		m_clodGroups,
		m_clodSegments,
		m_clodGroupChunks,
		m_clodPartRecords,
		m_clodRootPartIndex);
	if (m_prebuiltClusterLOD.has_value()) {
		m_prebuiltClusterLOD->partRecords = m_clodPartRecords;
		m_prebuiltClusterLOD->rootPartIndex = m_clodRootPartIndex;
	}
	m_perMeshBufferData.boundingSphere = data.objectBoundingSphere;
	{
		uint32_t voxelGroupCount = 0;
		for (const ClusterLODGroup& group : m_clodGroups) {
			if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u) {
				voxelGroupCount++;
			}
		}
		if (voxelGroupCount != 0u) {
			spdlog::debug(
				"ClusterLOD runtime adoption: groups={} voxel_groups={} nodes={} cache_hash=0x{:016X}",
				m_clodGroups.size(),
				voxelGroupCount,
				m_clodNodes.size(),
				data.cacheSource.buildConfigHash);
		}
	}

	m_clodMaxDepth = data.maxDepth;
	m_clodMaxTraversalDepth = data.maxTraversalDepth;
	if (m_clodLodLevelRoots.empty()) {
		m_clodMaxDepth = 0;
		for (const auto& group : m_clodGroups) {
			m_clodMaxDepth = std::max<uint32_t>(m_clodMaxDepth, static_cast<uint32_t>(std::max(group.depth, 0)));
		}
		m_clodLodLevelRoots.resize(m_clodMaxDepth + 1);
		for (uint32_t depth = 0; depth <= m_clodMaxDepth; ++depth) {
			m_clodLodLevelRoots[depth] = 1u + depth;
		}
	}
	if (m_clodLodNodeRanges.empty()) {
		m_clodLodNodeRanges.resize(m_clodLodLevelRoots.size());
	}
	if (m_clodMaxTraversalDepth == 0u && !m_clodNodes.empty()) {
		m_clodMaxTraversalDepth = ComputeCLodTraversalDepth(m_clodNodes, m_clodTopRootNode);
	}

	m_perMeshBufferData.numVertices = 0; // TODO: Remove, clod doesn't need it

	if (m_perMeshBufferData.boundingSphere.sphere.w <= 0.0f && !m_clodNodes.empty())
	{
		m_perMeshBufferData.boundingSphere = BuildObjectBoundingSphereFromRootNode(m_clodNodes, m_clodTopRootNode);
	}
}

void Mesh::AdoptCLodDiskStreamingMetadata(const ClusterLODPrebuiltData& data)
{
	if (data.pageDiskLocators.empty() || data.cacheSource.containerFileName.empty()) {
		return;
	}

	m_clodGroupDiskLocators = data.groupDiskLocators;
	m_clodPageDiskLocators = data.pageDiskLocators;
	m_clodGroupPageReferences = data.groupPageReferences;
	m_clodGroupPageReferenceOffsets = data.groupPageReferenceOffsets;
	m_clodTrianglePageCount = data.trianglePageCount;
	m_clodVoxelPageBase = data.voxelPageBase;
	m_clodVoxelPageCount = data.voxelPageCount;
	m_clodCacheSource = data.cacheSource;
	m_clodPartRecords = data.partRecords;
	m_clodRootPartIndex = data.rootPartIndex;
	EnsureRootPartRecord(
		m_clodPartRecords,
		m_clodRootPartIndex,
		static_cast<uint32_t>(m_clodGroups.size()),
		static_cast<uint32_t>(m_clodNodes.size()),
		static_cast<uint32_t>(m_clodAssemblyTransforms.size()),
		static_cast<uint32_t>(m_clodAssemblyInstances.size()),
		m_clodTopRootNode);
	if (m_prebuiltClusterLOD.has_value()) {
		m_prebuiltClusterLOD->groupDiskLocators = data.groupDiskLocators;
		m_prebuiltClusterLOD->pageDiskLocators = data.pageDiskLocators;
		m_prebuiltClusterLOD->groupPageReferences = data.groupPageReferences;
		m_prebuiltClusterLOD->groupPageReferenceOffsets = data.groupPageReferenceOffsets;
		m_prebuiltClusterLOD->trianglePageCount = data.trianglePageCount;
		m_prebuiltClusterLOD->voxelPageBase = data.voxelPageBase;
		m_prebuiltClusterLOD->voxelPageCount = data.voxelPageCount;
		m_prebuiltClusterLOD->cacheSource = data.cacheSource;
		m_prebuiltClusterLOD->partRecords = m_clodPartRecords;
		m_prebuiltClusterLOD->rootPartIndex = m_clodRootPartIndex;
	}

	if (m_clodGroupChunks.size() != m_clodGroups.size()) {
		m_clodGroupChunks.assign(m_clodGroups.size(), {});
		for (size_t groupIndex = 0; groupIndex < m_clodGroups.size(); ++groupIndex) {
			m_clodGroupChunks[groupIndex].groupVertexCount = m_clodGroups[groupIndex].groupVertexCount;
			m_clodGroupChunks[groupIndex].meshletCount = m_clodGroups[groupIndex].meshletCount;
		}
	}

	m_clodRuntimeSummary = BuildRuntimeSummary(
		m_clodGroups,
		m_clodSegments,
		m_clodGroupChunks,
		m_clodPartRecords,
		m_clodRootPartIndex);

	ClearCLodCacheBuildChunkData(false);
}

ClusterLODPrebuiltData Mesh::GetClusterLODPrebuiltData() const
{
	ClusterLODPrebuiltData out = m_prebuiltClusterLOD.value_or(ClusterLODPrebuiltData{});
	if (!m_clodGroups.empty()) {
		out.groups = m_clodGroups;
	}
	if (!m_clodSegments.empty()) {
		out.segments = m_clodSegments;
	}
	out.objectBoundingSphere = m_perMeshBufferData.boundingSphere;
	if (!m_clodGroupChunks.empty()) {
		out.groupChunks = m_clodGroupChunks;
	}
	out.groupDiskLocators = m_clodGroupDiskLocators;
	out.pageDiskLocators = m_clodPageDiskLocators;
	out.groupPageReferences = m_clodGroupPageReferences;
	out.groupPageReferenceOffsets = m_clodGroupPageReferenceOffsets;
	out.trianglePageCount = m_clodTrianglePageCount;
	out.voxelPageBase = m_clodVoxelPageBase;
	out.voxelPageCount = m_clodVoxelPageCount;
	out.cacheSource = m_clodCacheSource;
	if (!m_clodNodes.empty()) {
		out.nodes = m_clodNodes;
	}
	if (!m_clodNodeSkinningInfos.empty()) out.nodeSkinningInfos = m_clodNodeSkinningInfos;
	if (!m_clodNodeBoneIndices.empty()) out.nodeBoneIndices = m_clodNodeBoneIndices;
	out.nodeBoneLimit = m_clodNodeBoneLimit;
	if (!m_clodLodNodeRanges.empty()) {
		out.lodNodeRanges = m_clodLodNodeRanges;
	}
	if (!m_clodLodLevelRoots.empty()) {
		out.lodLevelRoots = m_clodLodLevelRoots;
	}
	if (!m_clodAssemblyTransforms.empty()) {
		out.assemblyTransforms = m_clodAssemblyTransforms;
	}
	if (!m_clodAssemblyInstances.empty()) {
		out.assemblyInstances = m_clodAssemblyInstances;
	}
	if (!m_clodAssemblyBoneRemaps.empty()) {
		out.assemblyBoneRemaps = m_clodAssemblyBoneRemaps;
	}
	if (!m_clodAssemblyBoneRemapIndices.empty()) {
		out.assemblyBoneRemapIndices = m_clodAssemblyBoneRemapIndices;
	}
	if (!m_clodPartRecords.empty()) {
		out.partRecords = m_clodPartRecords;
	}
	out.rootPartIndex = m_clodRootPartIndex;
	out.maxDepth = m_clodMaxDepth;
	out.maxTraversalDepth = m_clodMaxTraversalDepth;
	return out;
}

ClusterLODCacheBuildPayload Mesh::GetClusterLODCacheBuildPayload() const
{
	ClusterLODCacheBuildPayload payload{};
	payload.groupPageBlobs = &m_clodCacheBuildChunkData.groupPageBlobs;
	payload.meshPageBlobs = &m_clodCacheBuildChunkData.meshPageBlobs;
	return payload;
}

DirectX::XMFLOAT2 Mesh::EstimateReyesUvDensity(uint32_t uvSetIndex) const
{
	if (uvSetIndex < std::size(m_reyesUvDensityBySet) &&
		m_reyesUvDensityBySet[uvSetIndex].x > 0.0f &&
		m_reyesUvDensityBySet[uvSetIndex].y > 0.0f) {
		return m_reyesUvDensityBySet[uvSetIndex];
	}
	return { 1.0f, 1.0f };
}

ClusterLODCacheBuildOwnedData Mesh::GetClusterLODCacheBuildOwnedData() const
{
	ClusterLODCacheBuildOwnedData owned{};
	owned.groupPageBlobs = m_clodCacheBuildChunkData.groupPageBlobs;
	owned.meshPageBlobs = m_clodCacheBuildChunkData.meshPageBlobs;
	return owned;
}

void Mesh::CreateBuffers(const std::vector<UINT32>& indices) {
	if (m_prebuiltClusterLOD.has_value()) {
		if (m_clodGroups.empty() || m_clodSegments.empty() || m_clodNodes.empty()) {
			ApplyPrebuiltClusterLODData(m_prebuiltClusterLOD.value());
		}
	} else {
		throw std::runtime_error("Mesh was created without prebuilt ClusterLOD data");
	}
}

int Mesh::GetNextGlobalIndex() {
    return globalMeshCount.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Mesh::GetGlobalID() const {
	return m_globalMeshID;
}

void Mesh::SetCLodBufferViews(
	std::unique_ptr<BufferView> clusterLODGroupsView,
	std::unique_ptr<BufferView> clusterLODSegmentsView,
	std::unique_ptr<BufferView> clusterLODNodesView,
	std::unique_ptr<BufferView> clusterLODNodeSkinningInfosView,
	std::unique_ptr<BufferView> clusterLODNodeBoneIndicesView,
	std::unique_ptr<BufferView> clusterLODAssemblyTransformsView,
	std::unique_ptr<BufferView> clusterLODAssemblyInstancesView,
	std::unique_ptr<BufferView> clusterLODAssemblyBoneRemapsView,
	std::unique_ptr<BufferView> clusterLODAssemblyBoneRemapIndicesView
) {
	m_clusterLODGroupsView = std::move(clusterLODGroupsView);
	m_clusterLODSegmentsView = std::move(clusterLODSegmentsView);
	m_clusterLODNodesView = std::move(clusterLODNodesView);
	m_clusterLODNodeSkinningInfosView = std::move(clusterLODNodeSkinningInfosView);
	m_clusterLODNodeBoneIndicesView = std::move(clusterLODNodeBoneIndicesView);
	m_clusterLODAssemblyTransformsView = std::move(clusterLODAssemblyTransformsView);
	m_clusterLODAssemblyInstancesView = std::move(clusterLODAssemblyInstancesView);
	m_clusterLODAssemblyBoneRemapsView = std::move(clusterLODAssemblyBoneRemapsView);
	m_clusterLODAssemblyBoneRemapIndicesView = std::move(clusterLODAssemblyBoneRemapIndicesView);

	auto firstChunkOffsetDiv = [](const auto& chunkViews, uint32_t divisor) -> uint32_t {
		uint32_t minGroupIndex = std::numeric_limits<uint32_t>::max();
		uint64_t selectedOffset = 0u;
		bool found = false;
		for (const auto& [groupIndex, chunkView] : chunkViews) {
			if (chunkView != nullptr && groupIndex < minGroupIndex) {
				minGroupIndex = groupIndex;
				selectedOffset = chunkView->GetOffset();
				found = true;
			}
		}
		if (found) {
			return static_cast<uint32_t>(selectedOffset / divisor);
		}
		return 0u;
	};
	auto firstChunkOffsetBytes = [](const auto& chunkViews) -> uint32_t {
		uint32_t minGroupIndex = std::numeric_limits<uint32_t>::max();
		uint64_t selectedOffset = 0u;
		bool found = false;
		for (const auto& [groupIndex, chunkView] : chunkViews) {
			if (chunkView != nullptr && groupIndex < minGroupIndex) {
				minGroupIndex = groupIndex;
				selectedOffset = chunkView->GetOffset();
				found = true;
			}
		}
		if (found) {
			return static_cast<uint32_t>(selectedOffset);
		}
		return 0u;
	};

	uint32_t totalMeshletCount = 0;
	for (const auto& group : m_clodGroups) {
		totalMeshletCount += group.meshletCount;
	}
	m_perMeshBufferData.clodNumMeshlets = totalMeshletCount;
	if (m_pCurrentMeshManager != nullptr && m_perMeshBufferView != nullptr) {
		m_pCurrentMeshManager->UpdatePerMeshBuffer(m_perMeshBufferView, m_perMeshBufferData);
	}
}

void Mesh::SetBaseSkin(std::shared_ptr<Skeleton> skeleton) {
	m_baseSkeleton = skeleton;
	m_animationBoundingSpheres.clear();
	EnsureAnimatedBoundingSpheresBuilt_();
}

BoundingSphere Mesh::GetAnimatedBoundingSphere(size_t animationIndex) const
{
	EnsureAnimatedBoundingSpheresBuilt_();
	if (animationIndex >= m_animationBoundingSpheres.size()) {
		return m_perMeshBufferData.boundingSphere;
	}

	return m_animationBoundingSpheres[animationIndex];
}

void Mesh::EnsureAnimatedBoundingSpheresBuilt_() const
{
	if (!m_baseSkeleton) {
		m_animationBoundingSpheres.clear();
		return;
	}

	const size_t animationCount = m_baseSkeleton->animations.size();
	if (m_animationBoundingSpheres.size() == animationCount) {
		return;
	}

	m_animationBoundingSpheres.clear();
	m_animationBoundingSpheres.reserve(animationCount);

	if (animationCount == 0) {
		return;
	}

	const BoundingSphere staticSphere = m_perMeshBufferData.boundingSphere;
	if (staticSphere.sphere.w <= 0.0f) {
		m_animationBoundingSpheres.assign(animationCount, staticSphere);
		return;
	}

	auto samplingSkeleton = m_baseSkeleton->CopySkeleton();
	if (!samplingSkeleton) {
		m_animationBoundingSpheres.assign(animationCount, staticSphere);
		return;
	}

	const auto inverseBindMatrices = m_baseSkeleton->GetInverseBindMatrices();
	const size_t boneCount = m_baseSkeleton->GetBoneCount();
	if (boneCount == 0 || inverseBindMatrices.size() < boneCount) {
		m_animationBoundingSpheres.assign(animationCount, staticSphere);
		return;
	}

	for (size_t animationIndex = 0; animationIndex < animationCount; ++animationIndex) {
		const auto& animation = m_baseSkeleton->animations[animationIndex];
		if (!animation) {
			m_animationBoundingSpheres.push_back(staticSphere);
			continue;
		}

		samplingSkeleton->SetAnimation(animationIndex);
		const std::vector<float> sampleTimes = BuildAnimationSampleTimes(*animation);
		BoundingSphere animationSphere = staticSphere;
		float previousSampleTime = 0.0f;
		bool initialized = false;

		for (float sampleTime : sampleTimes) {
			const float deltaTime = sampleTime - previousSampleTime;
			samplingSkeleton->UpdateTransforms(deltaTime, true);
			previousSampleTime = sampleTime;

			const auto boneMatrices = samplingSkeleton->GetBoneMatrices();
			if (boneMatrices.size() < boneCount) {
				continue;
			}

			BoundingSphere sampledSphere{};
			for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
				const DirectX::XMMATRIX skinMatrix = DirectX::XMMatrixMultiply(inverseBindMatrices[boneIndex], boneMatrices[boneIndex]);
				const BoundingSphere transformedSphere = TransformBoundingSphere(staticSphere, skinMatrix);
				if (!initialized && boneIndex == 0) {
					sampledSphere = transformedSphere;
				}
				else {
					sampledSphere = MergeBoundingSpheres(sampledSphere, transformedSphere);
				}
			}

			if (!initialized) {
				animationSphere = sampledSphere;
				initialized = true;
			}
			else {
				animationSphere = MergeBoundingSpheres(animationSphere, sampledSphere);
			}
		}

		if (!initialized) {
			animationSphere = staticSphere;
		}
		animationSphere.sphere.w *= kAnimatedBoundsPaddingScale;
		m_animationBoundingSpheres.push_back(animationSphere);
	}
}

void Mesh::SetMaterialDataIndex(unsigned int index) {
	const unsigned int previousIndex = m_perMeshBufferData.materialDataIndex;
	if (previousIndex != index && previousIndex != 0u && m_perMeshBufferView != nullptr) {
		spdlog::warn(
			"Mesh material slot changed for resident mesh globalID={} oldSlot={} newSlot={} perMeshOffset={}",
			GetGlobalID(),
			previousIndex,
			index,
			m_perMeshBufferView->GetOffset());
	}
	m_perMeshBufferData.materialDataIndex = index;
	if (m_pCurrentMeshManager != nullptr) {
		m_pCurrentMeshManager->UpdatePerMeshBuffer(m_perMeshBufferView, m_perMeshBufferData);
	}
}

void Mesh::SetMaterialEvalCompileFlagsID(unsigned int index) {
	m_perMeshBufferData.materialEvalCompileFlagsID = index;
	if (m_pCurrentMeshManager != nullptr) {
		m_pCurrentMeshManager->UpdatePerMeshBuffer(m_perMeshBufferView, m_perMeshBufferData);
	}
}

void Mesh::SetMaterialReyesEvalCompileFlagsID(unsigned int index) {
	m_perMeshBufferData.materialReyesEvalCompileFlagsID = index;
	if (m_pCurrentMeshManager != nullptr) {
		m_pCurrentMeshManager->UpdatePerMeshBuffer(m_perMeshBufferView, m_perMeshBufferData);
	}
}

void Mesh::SetRasterBucketIndex(unsigned int index) {
	m_perMeshBufferData.rasterBucketIndex = index;
	if (m_pCurrentMeshManager != nullptr) {
		m_pCurrentMeshManager->UpdatePerMeshBuffer(m_perMeshBufferView, m_perMeshBufferData);
	}
}
