#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>

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

bool ValidateClusterLODPageRepresentations(
	const ClusterLODPrebuiltData& prebuiltData,
	const std::vector<std::vector<std::byte>>* meshPageBlobs,
	std::string* outError)
{
	auto fail = [&](std::string message) -> bool
		{
			if (outError != nullptr)
			{
				*outError = std::move(message);
			}
			return false;
		};

	if (prebuiltData.nodeBoneLimit < 1u || prebuiltData.nodeBoneLimit > CLOD_NODE_BONE_LIMIT_HARD_MAX)
	{
		return fail(std::format("node bone limit {} is outside supported range [1, {}]",
			prebuiltData.nodeBoneLimit, CLOD_NODE_BONE_LIMIT_HARD_MAX));
	}
	if (prebuiltData.nodeSkinningInfos.size() != prebuiltData.nodes.size())
	{
		return fail(std::format("node skinning info count {} does not match node count {}",
			prebuiltData.nodeSkinningInfos.size(), prebuiltData.nodes.size()));
	}
	constexpr uint16_t validSkinningFlags =
		CLOD_NODE_SKINNING_FLAG_OVERFLOW | CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK;
	for (size_t nodeIndex = 0; nodeIndex < prebuiltData.nodeSkinningInfos.size(); ++nodeIndex)
	{
		const ClusterLODNodeSkinningInfo& info = prebuiltData.nodeSkinningInfos[nodeIndex];
		if ((info.flags & ~validSkinningFlags) != 0u || info.flags == validSkinningFlags ||
			(info.flags != 0u && info.boneCount != 0u) ||
			info.boneCount > prebuiltData.nodeBoneLimit)
		{
			return fail(std::format("node {} has invalid skinning metadata (count={}, flags=0x{:X}, limit={})",
				nodeIndex, info.boneCount, info.flags, prebuiltData.nodeBoneLimit));
		}
		const uint64_t boneEnd = static_cast<uint64_t>(info.boneListOffset) + info.boneCount;
		if (boneEnd > prebuiltData.nodeBoneIndices.size())
		{
			return fail(std::format("node {} bone range [{}, {}) exceeds bone index count {}",
					nodeIndex, info.boneListOffset, boneEnd, prebuiltData.nodeBoneIndices.size()));
		}
		const auto boneBeginIt = prebuiltData.nodeBoneIndices.begin() + info.boneListOffset;
		const auto boneEndIt = prebuiltData.nodeBoneIndices.begin() + static_cast<size_t>(boneEnd);
		if (!std::is_sorted(boneBeginIt, boneEndIt) || std::adjacent_find(boneBeginIt, boneEndIt) != boneEndIt)
		{
			return fail(std::format("node {} explicit bone list is not sorted and unique", nodeIndex));
		}
	}
	for (size_t nodeIndex = 0; nodeIndex < prebuiltData.nodes.size(); ++nodeIndex)
	{
		const ClusterLODNode& node = prebuiltData.nodes[nodeIndex];
		if (node.range.isGroup != CLOD_NODE_INTERNAL) continue;
		const uint32_t childCount = node.range.countMinusOne + 1u;
		if (node.range.indexOrOffset > prebuiltData.nodes.size() ||
			childCount > prebuiltData.nodes.size() - node.range.indexOrOffset)
		{
			return fail(std::format("internal node {} has invalid child range", nodeIndex));
		}
		const ClusterLODNodeSkinningInfo& parentInfo = prebuiltData.nodeSkinningInfos[nodeIndex];
		std::vector<uint32_t> expectedUnion;
		for (uint32_t childOffset = 0u; childOffset < childCount; ++childOffset)
		{
			const ClusterLODNodeSkinningInfo& childInfo =
				prebuiltData.nodeSkinningInfos[node.range.indexOrOffset + childOffset];
			if ((childInfo.flags & CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK) != 0u &&
				(parentInfo.flags & CLOD_NODE_SKINNING_FLAG_COARSE_FALLBACK) == 0u)
			{
				return fail(std::format("internal node {} does not propagate child coarse fallback", nodeIndex));
			}
			if ((childInfo.flags & CLOD_NODE_SKINNING_FLAG_OVERFLOW) != 0u && parentInfo.flags == 0u)
			{
				return fail(std::format("internal node {} does not propagate child bone overflow", nodeIndex));
			}
			if (parentInfo.flags == 0u)
			{
				expectedUnion.insert(
					expectedUnion.end(),
					prebuiltData.nodeBoneIndices.begin() + childInfo.boneListOffset,
					prebuiltData.nodeBoneIndices.begin() + childInfo.boneListOffset + childInfo.boneCount);
			}
		}
		if (parentInfo.flags == 0u)
		{
			std::ranges::sort(expectedUnion);
			expectedUnion.erase(std::unique(expectedUnion.begin(), expectedUnion.end()), expectedUnion.end());
			const auto parentBegin = prebuiltData.nodeBoneIndices.begin() + parentInfo.boneListOffset;
			const auto parentEnd = parentBegin + parentInfo.boneCount;
			if (!std::equal(expectedUnion.begin(), expectedUnion.end(), parentBegin, parentEnd))
			{
				return fail(std::format("internal node {} explicit bone list does not equal its child union", nodeIndex));
			}
		}
	}

	const uint64_t totalPageCount64 =
		static_cast<uint64_t>(prebuiltData.voxelPageBase) + prebuiltData.voxelPageCount;
	if (prebuiltData.voxelPageBase != prebuiltData.trianglePageCount)
	{
		return fail(std::format(
			"voxelPageBase {} does not equal trianglePageCount {}",
			prebuiltData.voxelPageBase,
			prebuiltData.trianglePageCount));
	}
	if (totalPageCount64 > std::numeric_limits<uint32_t>::max())
	{
		return fail("mesh page count overflows uint32");
	}
	const uint32_t totalPageCount = static_cast<uint32_t>(totalPageCount64);
	if (meshPageBlobs != nullptr && meshPageBlobs->size() != totalPageCount)
	{
		return fail(std::format(
			"mesh page blob count {} does not match metadata page count {}",
			meshPageBlobs->size(),
			totalPageCount));
	}
	if (prebuiltData.groupPageReferenceOffsets.size() != prebuiltData.groups.size() + 1ull)
	{
		return fail(std::format(
			"group page reference offset count {} does not equal group count + 1 ({})",
			prebuiltData.groupPageReferenceOffsets.size(),
			prebuiltData.groups.size() + 1ull));
	}

	for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(prebuiltData.groups.size()); ++groupIndex)
	{
		const ClusterLODGroup& group = prebuiltData.groups[groupIndex];
		const bool expectVoxel = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
		const uint32_t refBegin = prebuiltData.groupPageReferenceOffsets[groupIndex];
		const uint32_t refEnd = prebuiltData.groupPageReferenceOffsets[groupIndex + 1u];
		if (refBegin > refEnd || refEnd > prebuiltData.groupPageReferences.size())
		{
			return fail(std::format(
				"group {} has invalid page reference range [{}, {}) of {}",
				groupIndex, refBegin, refEnd, prebuiltData.groupPageReferences.size()));
		}
		if (refEnd - refBegin != group.pageCount)
		{
			return fail(std::format(
				"group {} pageCount {} does not match reference count {}",
				groupIndex, group.pageCount, refEnd - refBegin));
		}

		for (uint32_t refIndex = refBegin; refIndex < refEnd; ++refIndex)
		{
			const uint32_t meshPageIndex = prebuiltData.groupPageReferences[refIndex];
			if (meshPageIndex >= totalPageCount)
			{
				return fail(std::format(
					"group {} references mesh page {} outside page count {}",
					groupIndex, meshPageIndex, totalPageCount));
			}
			const bool referencedPageIsVoxel =
				meshPageIndex >= prebuiltData.voxelPageBase &&
				meshPageIndex < prebuiltData.voxelPageBase + prebuiltData.voxelPageCount;
			if (referencedPageIsVoxel != expectVoxel)
			{
				return fail(std::format(
					"group {} flags=0x{:08x} expects {} pages but reference {} points to {} mesh page {}",
					groupIndex,
					group.flags,
					expectVoxel ? "voxel" : "triangle",
					refIndex - refBegin,
					referencedPageIsVoxel ? "voxel" : "triangle",
					meshPageIndex));
			}

			if (meshPageBlobs != nullptr)
			{
				const std::vector<std::byte>& blob = (*meshPageBlobs)[meshPageIndex];
				uint32_t firstWord = 0u;
				if (blob.size() >= sizeof(uint32_t))
				{
					std::memcpy(&firstWord, blob.data(), sizeof(uint32_t));
				}
				const bool blobIsVoxel = firstWord == CLOD_VOXEL_PAGE_MAGIC;
				if (blob.empty() || blobIsVoxel != expectVoxel)
				{
					return fail(std::format(
						"group {} flags=0x{:08x} references mesh page {} with size {} and firstWord=0x{:08x}",
						groupIndex, group.flags, meshPageIndex, blob.size(), firstWord));
				}
			}
		}

		const uint32_t segmentEnd = std::min<uint32_t>(
			group.firstSegment + group.segmentCount,
			static_cast<uint32_t>(prebuiltData.segments.size()));
		if (segmentEnd != group.firstSegment + group.segmentCount)
		{
			return fail(std::format("group {} segment range is out of bounds", groupIndex));
		}
		for (uint32_t segmentIndex = group.firstSegment; segmentIndex < segmentEnd; ++segmentIndex)
		{
			const ClusterLODGroupSegment& segment = prebuiltData.segments[segmentIndex];
			if (segment.meshletCount == 0u)
			{
				continue;
			}
			if (segment.pageIndex < group.pageMapBase ||
				segment.pageIndex >= group.pageMapBase + group.pageCount)
			{
				return fail(std::format(
					"group {} segment {} page-map index {} is outside [{}, {})",
					groupIndex,
					segmentIndex,
					segment.pageIndex,
					group.pageMapBase,
					group.pageMapBase + group.pageCount));
			}
		}
	}

	for (uint32_t nodeIndex = 0; nodeIndex < static_cast<uint32_t>(prebuiltData.nodes.size()); ++nodeIndex)
	{
		const ClusterLODNode& node = prebuiltData.nodes[nodeIndex];
		if (node.range.isGroup != CLOD_NODE_VOXEL_LEAF && node.range.isGroup != CLOD_NODE_SEGMENT_LEAF)
		{
			continue;
		}
		if (node.range.ownerGroupId >= prebuiltData.groups.size())
		{
			return fail(std::format(
				"node {} owns group {} outside group count {}",
				nodeIndex, node.range.ownerGroupId, prebuiltData.groups.size()));
		}
		const bool groupIsVoxel =
			(prebuiltData.groups[node.range.ownerGroupId].flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
		const bool nodeIsVoxel = node.range.isGroup == CLOD_NODE_VOXEL_LEAF;
		if (groupIsVoxel != nodeIsVoxel)
		{
			return fail(std::format(
				"node {} type {} disagrees with owner group {} flags=0x{:08x}",
				nodeIndex,
				node.range.isGroup,
				node.range.ownerGroupId,
				prebuiltData.groups[node.range.ownerGroupId].flags));
		}
		if (node.range.isGroup == CLOD_NODE_SEGMENT_LEAF)
		{
			const ClusterLODGroup& ownerGroup = prebuiltData.groups[node.range.ownerGroupId];
			const uint64_t ownerSegmentEnd =
				static_cast<uint64_t>(ownerGroup.firstSegment) + ownerGroup.segmentCount;
			if (node.range.indexOrOffset < ownerGroup.firstSegment ||
				node.range.indexOrOffset >= ownerSegmentEnd)
			{
				return fail(std::format(
					"segment leaf node {} references segment {} outside owner group {} range [{}, {})",
					nodeIndex,
					node.range.indexOrOffset,
					node.range.ownerGroupId,
					ownerGroup.firstSegment,
					ownerSegmentEnd));
			}
		}
	}

	if (outError != nullptr)
	{
		outError->clear();
	}
	return true;
}

