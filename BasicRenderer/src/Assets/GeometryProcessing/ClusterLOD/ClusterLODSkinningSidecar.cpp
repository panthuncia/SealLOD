#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.h"

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

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODSkinningSidecar.h"

namespace clod_detail
{
	bool CollectSegmentBoneSet(const ClusterLODBuildState& state, uint32_t segmentIndex, std::vector<uint32_t>& outBones)
	{
		if (segmentIndex >= state.segments.size()) return false;
		const ClusterLODGroupSegment& segment = state.segments[segmentIndex];
		uint32_t ownerGroup = UINT32_MAX;
		for (uint32_t groupIndex = 0u; groupIndex < state.groups.size(); ++groupIndex)
		{
			const ClusterLODGroup& group = state.groups[groupIndex];
			if (segmentIndex >= group.firstSegment && segmentIndex < group.firstSegment + group.segmentCount)
			{
				ownerGroup = groupIndex;
				break;
			}
		}
		if (ownerGroup == UINT32_MAX || ownerGroup >= state.groupPageBlobs.size() ||
			segment.pageIndex >= state.groupPageBlobs[ownerGroup].size()) return false;

		const std::vector<std::byte>& blob = state.groupPageBlobs[ownerGroup][segment.pageIndex];
		if (blob.size() < sizeof(CLodPageHeader)) return false;
		CLodPageHeader header{};
		std::memcpy(&header, blob.data(), sizeof(header));
		const uint64_t descriptorEnd = static_cast<uint64_t>(header.descriptorOffset) +
			static_cast<uint64_t>(header.meshletCount) * sizeof(CLodMeshletDescriptor);
		if (descriptorEnd > blob.size() || segment.firstMeshletInPage > header.meshletCount ||
			segment.meshletCount > header.meshletCount - segment.firstMeshletInPage) return false;

		for (uint32_t local = 0u; local < segment.meshletCount; ++local)
		{
			CLodMeshletDescriptor desc{};
			const size_t descriptorOffset = header.descriptorOffset +
				static_cast<size_t>(segment.firstMeshletInPage + local) * sizeof(CLodMeshletDescriptor);
			std::memcpy(&desc, blob.data() + descriptorOffset, sizeof(desc));
			const uint64_t boneBegin = static_cast<uint64_t>(header.boneIndexStreamOffset) +
				static_cast<uint64_t>(desc.boneListOffset) * sizeof(uint32_t);
			const uint32_t descriptorBoneCount = CLodClusterCullMetadataBoneCount(desc.boneCount);
			const uint64_t boneEnd = boneBegin + static_cast<uint64_t>(descriptorBoneCount) * sizeof(uint32_t);
			if (boneEnd > blob.size()) return false;
			for (uint32_t bone = 0u; bone < descriptorBoneCount; ++bone)
			{
				uint32_t joint = 0u;
				std::memcpy(&joint, blob.data() + boneBegin + static_cast<size_t>(bone) * sizeof(uint32_t), sizeof(joint));
				outBones.push_back(joint);
			}
		}
		std::ranges::sort(outBones);
		outBones.erase(std::unique(outBones.begin(), outBones.end()), outBones.end());
		return true;
	}

	bool CollectVoxelGroupBoneSet(const ClusterLODBuildState& state, uint32_t groupIndex, std::vector<uint32_t>& outBones)
	{
		if (groupIndex >= state.voxelGroupMapping.groupToPackedMetadataIndex.size()) return false;
		const int32_t metadataIndex = state.voxelGroupMapping.groupToPackedMetadataIndex[groupIndex];
		if (metadataIndex < 0 || static_cast<size_t>(metadataIndex) >= state.voxelGroupMapping.packedGroupMetadata.size()) return false;
		const VoxelGroupPackedMetadata& metadata = state.voxelGroupMapping.packedGroupMetadata[metadataIndex];
		if (metadata.firstCube > state.voxelGroupMapping.packedCubeRecords.size() ||
			metadata.cubeCount > state.voxelGroupMapping.packedCubeRecords.size() - metadata.firstCube) return false;
		for (uint32_t cube = 0u; cube < metadata.cubeCount; ++cube)
		{
			const uint32_t joint = state.voxelGroupMapping.packedCubeRecords[metadata.firstCube + cube].dominantBoneIndex;
			if (joint != CLOD_VOXEL_STATIC_BONE_INDEX) outBones.push_back(joint);
		}
		std::ranges::sort(outBones);
		outBones.erase(std::unique(outBones.begin(), outBones.end()), outBones.end());
		return true;
	}

	void BuildNodeSkinningSidecar(
		const ClusterLODBuildState& state,
		uint32_t requestedLimit,
		std::vector<ClusterLODNodeSkinningInfo>& outInfos,
		std::vector<uint32_t>& outBoneIndices,
		const std::unordered_map<uint32_t, NodeBoneSet>* preservedSets)
	{
		const uint32_t limit = std::clamp(requestedLimit, 1u, CLOD_NODE_BONE_LIMIT_HARD_MAX);
		if (limit != requestedLimit)
		{
			spdlog::warn("ClusterLOD node bone limit {} is outside the supported range [1, {}]; using {}",
				requestedLimit, CLOD_NODE_BONE_LIMIT_HARD_MAX, limit);
		}
		outInfos.assign(state.nodes.size(), {});
		outBoneIndices.clear();
		std::vector<NodeBoneSet> sets(state.nodes.size());
		std::vector<uint8_t> visitation(state.nodes.size(), 0u);

		std::function<const NodeBoneSet&(uint32_t)> build = [&](uint32_t nodeIndex) -> const NodeBoneSet&
		{
			static const NodeBoneSet invalidSet{ {}, CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK };
			if (nodeIndex >= state.nodes.size()) return invalidSet;
			if (visitation[nodeIndex] == 2u) return sets[nodeIndex];
			if (visitation[nodeIndex] == 1u)
			{
				sets[nodeIndex].flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
				return sets[nodeIndex];
			}
			visitation[nodeIndex] = 1u;
			const ClusterLODNode& node = state.nodes[nodeIndex];
			NodeBoneSet& result = sets[nodeIndex];

			bool usedPreservedSet = false;
			if (preservedSets != nullptr)
			{
				const auto preserved = preservedSets->find(nodeIndex);
				if (preserved != preservedSets->end())
				{
					result = preserved->second;
					usedPreservedSet = true;
				}
			}
			if (!usedPreservedSet && node.range.isGroup == CLOD_NODE_INSTANCE_ROOT)
			{
				result.flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
			}
			else if (!usedPreservedSet && node.range.isGroup == CLOD_NODE_SEGMENT_LEAF)
			{
				if (!CollectSegmentBoneSet(state, node.range.indexOrOffset, result.bones))
					result.flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
			}
			else if (!usedPreservedSet && node.range.isGroup == CLOD_NODE_VOXEL_LEAF)
			{
				if (!CollectVoxelGroupBoneSet(state, node.range.ownerGroupId, result.bones))
					result.flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
			}
			else if (!usedPreservedSet && node.range.isGroup == CLOD_NODE_INTERNAL)
			{
				const uint32_t childCount = node.range.countMinusOne + 1u;
				for (uint32_t child = 0u; child < childCount; ++child)
				{
					const NodeBoneSet& childSet = build(node.range.indexOrOffset + child);
					if ((childSet.flags & CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK) != 0u)
					{
						result.flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
						result.bones.clear();
						break;
					}
					if ((childSet.flags & CLOD_NODE_SKINNING_FLAG_OVERFLOW) != 0u)
					{
						result.flags = CLOD_NODE_SKINNING_FLAG_OVERFLOW;
						result.bones.clear();
						break;
					}
					result.bones.insert(result.bones.end(), childSet.bones.begin(), childSet.bones.end());
					std::ranges::sort(result.bones);
					result.bones.erase(std::unique(result.bones.begin(), result.bones.end()), result.bones.end());
					if (result.bones.size() > limit)
					{
						result.flags = CLOD_NODE_SKINNING_FLAG_OVERFLOW;
						result.bones.clear();
						break;
					}
				}
			}
			else if (!usedPreservedSet)
			{
				result.flags = CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
			}

			if (result.flags == 0u && result.bones.size() > limit)
			{
				result.flags = CLOD_NODE_SKINNING_FLAG_OVERFLOW;
				result.bones.clear();
			}
			visitation[nodeIndex] = 2u;
			return result;
		};

		for (uint32_t nodeIndex = 0u; nodeIndex < state.nodes.size(); ++nodeIndex) (void)build(nodeIndex);
		for (uint32_t nodeIndex = 0u; nodeIndex < state.nodes.size(); ++nodeIndex)
		{
			const NodeBoneSet& set = sets[nodeIndex];
			ClusterLODNodeSkinningInfo& info = outInfos[nodeIndex];
			if (outBoneIndices.size() > (std::numeric_limits<uint32_t>::max)() ||
				set.bones.size() > (std::numeric_limits<uint32_t>::max)() - outBoneIndices.size())
			{
				throw std::runtime_error("ClusterLOD node bone index stream exceeds uint32_t addressing");
			}
			info.boneListOffset = static_cast<uint32_t>(outBoneIndices.size());
			info.flags = set.flags;
			if (set.flags == 0u)
			{
				info.boneCount = static_cast<uint16_t>(set.bones.size());
				outBoneIndices.insert(outBoneIndices.end(), set.bones.begin(), set.bones.end());
			}
		}
		size_t explicitNodes = 0u;
		size_t staticNodes = 0u;
		size_t overflowNodes = 0u;
		size_t coarseFallbackNodes = 0u;
		for (const ClusterLODNodeSkinningInfo& info : outInfos)
		{
			explicitNodes += info.flags == 0u && info.boneCount != 0u ? 1u : 0u;
			staticNodes += info.flags == 0u && info.boneCount == 0u ? 1u : 0u;
			overflowNodes += (info.flags & CLOD_NODE_SKINNING_FLAG_OVERFLOW) != 0u ? 1u : 0u;
			coarseFallbackNodes += (info.flags & CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK) != 0u ? 1u : 0u;
		}
		spdlog::debug(
			"ClusterLOD node bone metadata: limit={} nodes={} explicit={} static={} overflow={} coarse_fallback={} bone_indices={}",
			limit, outInfos.size(), explicitNodes, staticNodes, overflowNodes, coarseFallbackNodes, outBoneIndices.size());
		TracyPlot("CLOD.Build.NodeBoneIndices", static_cast<int64_t>(outBoneIndices.size()));
		TracyPlot("CLOD.Build.NodeBoneOverflow", static_cast<int64_t>(overflowNodes));
	}

}
