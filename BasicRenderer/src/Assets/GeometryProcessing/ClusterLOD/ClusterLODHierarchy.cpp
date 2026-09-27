#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelPacking.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODPagePackingTelemetry.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.h"

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
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODHierarchy.h"
#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODVoxelFallback.h"

namespace clod_detail
{
	void BuildClusterLODTraversalHierarchy(ClusterLODBuildState& state, uint32_t preferredNodeWidth)
	{
		ZoneScopedN("ClusterLODUtilities::BuildClusterLODTraversalHierarchy");
		if (state.groups.empty())
			return;
		TracyPlot("CLOD.Traversal.Groups", static_cast<int64_t>(state.groups.size()));

		preferredNodeWidth = std::max(2u, preferredNodeWidth);

		auto includeGroupInTraversal = [&](uint32_t groupID) -> bool
		{
			return state.traversalGroupMask.empty() ||
				(groupID < state.traversalGroupMask.size() && state.traversalGroupMask[groupID] != 0u);
		};

		state.maxDepth = 0;
		for (uint32_t groupID = 0; groupID < uint32_t(state.groups.size()); ++groupID)
		{
			if (includeGroupInTraversal(groupID))
			{
				state.maxDepth = std::max(state.maxDepth, uint32_t(state.groups[groupID].depth));
			}
		}

		const uint32_t lodLevelCount = state.maxDepth + 1;

		// Collect traversal leaves by depth. Voxelized groups replace their triangle
		// representation at the group/section level; streaming pages are expanded only
		// after the voxel section wins the normal LOD decision.
		struct TraversalLeafInfo { uint32_t nodeKind; uint32_t indexOrOffset; uint32_t ownerGroupId; int32_t refinedGroup; };
		std::vector<std::vector<TraversalLeafInfo>> leavesByDepth(lodLevelCount);
		for (uint32_t groupID = 0; groupID < uint32_t(state.groups.size()); ++groupID)
		{
			if (!includeGroupInTraversal(groupID))
			{
				continue;
			}

			const ClusterLODGroup& grp = state.groups[groupID];
			const uint32_t d = uint32_t(grp.depth);
			if ((grp.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u)
			{
				leavesByDepth[d].push_back({ CLOD_NODE_INSTANCE_ROOT, grp.firstMeshlet, groupID, -1 });
				continue;
			}

			const bool isVoxelGroup = (grp.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
			if (isVoxelGroup)
			{
				uint32_t s = 0;
				while (s < grp.segmentCount)
				{
					const uint32_t firstSectionSegment = s;
					const int32_t refinedGroup = state.segments[grp.firstSegment + s].refinedGroup;
					while (s < grp.segmentCount && state.segments[grp.firstSegment + s].refinedGroup == refinedGroup)
					{
						++s;
					}
					leavesByDepth[d].push_back({ 1u, firstSectionSegment, groupID, refinedGroup });
				}
				continue;
			}

			for (uint32_t s = 0; s < grp.segmentCount; ++s)
			{
				leavesByDepth[d].push_back({ 2u, grp.firstSegment + s, groupID, state.segments[grp.firstSegment + s].refinedGroup });
			}
		}

		for (uint32_t d = 0; d < lodLevelCount; ++d)
		{
			if (leavesByDepth[d].empty())
			{
				throw std::runtime_error("Cluster LOD: missing traversal leaves for an intermediate depth; compact depths or handle gaps.");
			}
		}

		// Build parent error map: for each group, store the max traversal error
		// of any parent (coarser) group that refines into it. Also track the
		// parent group ID associated with that max error.
		std::vector<float> parentErrorForGroup(state.groups.size(), 0.0f);
		std::vector<int32_t> parentGroupIdForGroup(state.groups.size(), -1);
		for (uint32_t groupID = 0; groupID < uint32_t(state.groups.size()); ++groupID)
		{
			const ClusterLODGroup& grp = state.groups[groupID];
			const float parentError = grp.bounds.error;
			for (uint32_t s = 0; s < grp.segmentCount; ++s)
			{
				const ClusterLODGroupSegment& seg = state.segments[grp.firstSegment + s];
				if (seg.refinedGroup >= 0)
				{
					const uint32_t childGroupId = static_cast<uint32_t>(seg.refinedGroup);
					if (parentError >= parentErrorForGroup[childGroupId])
					{
						parentErrorForGroup[childGroupId] = parentError;
						parentGroupIdForGroup[childGroupId] = static_cast<int32_t>(groupID);
					}
				}
			}
		}
		// Root groups (no parent) get FLT_MAX so they are always traversed.
		// Assign parentGroupId to each group.
		for (uint32_t i = 0; i < uint32_t(state.groups.size()); ++i)
		{
			if (parentGroupIdForGroup[i] < 0)
			{
				parentErrorForGroup[i] = std::numeric_limits<float>::max();
			}
			state.groups[i].parentGroupId = parentGroupIdForGroup[i];
			state.groups[i].maxParentError = parentErrorForGroup[i];
		}

		state.lodNodeRanges.assign(lodLevelCount, {});
		state.lodLevelRoots.resize(lodLevelCount);
		for (uint32_t d = 0; d < lodLevelCount; ++d) {
			state.lodLevelRoots[d] = 1 + d;
		}

		uint32_t nodeOffset = 1 + lodLevelCount;

		for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
		{
			const uint32_t leafCount = uint32_t(leavesByDepth[depth].size());

			if (leafCount == 1u)
			{
				state.lodNodeRanges[depth].offset = state.lodLevelRoots[depth];
				state.lodNodeRanges[depth].count = 1u;
				continue;
			}

			uint32_t nodeCount = leafCount;
			uint32_t iterCount = leafCount;

			while (iterCount > 1)
			{
				iterCount = (iterCount + preferredNodeWidth - 1) / preferredNodeWidth;
				nodeCount += iterCount;
			}

			nodeCount--;

			state.lodNodeRanges[depth].offset = nodeOffset;
			state.lodNodeRanges[depth].count = nodeCount;
			nodeOffset += nodeCount;
		}

		state.nodes.clear();
		state.nodes.resize(nodeOffset);

		for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
		{
			const auto& leaves = leavesByDepth[depth];
			const uint32_t leafCount = uint32_t(leaves.size());
			const ClusterLODNodeRangeAlloc& range = state.lodNodeRanges[depth];

			uint32_t writeOffset = range.offset;
			uint32_t lastLayerOffset = writeOffset;

			for (uint32_t i = 0; i < leafCount; ++i)
			{
				const TraversalLeafInfo& info = leaves[i];
				const ClusterLODGroup& grp = state.groups[info.ownerGroupId];

				ClusterLODNode& node = (leafCount == 1) ? state.nodes[1 + depth] : state.nodes[writeOffset++];

				node = {};
				node.range.isGroup = info.nodeKind;
				node.range.indexOrOffset = info.indexOrOffset;
				node.range.countMinusOne = (info.refinedGroup >= 0)
					? static_cast<uint32_t>(info.refinedGroup + 1)
					: 0u;
				node.range.ownerGroupId = info.ownerGroupId;

				if (info.nodeKind == 2u)
				{
					const BoundingSphere& segBounds = state.segmentBounds[info.indexOrOffset];
					// Expand the BVH leaf bounding sphere to enclose the owning
					// group's bounding sphere for conservative frustum culling.
					// TraverseNodes uses the actual group sphere for LOD checks.
					const float sx = segBounds.sphere.x, sy = segBounds.sphere.y, sz = segBounds.sphere.z;
					const float sr = segBounds.sphere.w;
					const float gx = grp.bounds.center[0], gy = grp.bounds.center[1], gz = grp.bounds.center[2];
					const float gr = grp.bounds.radius;

					const float dx = gx - sx, dy = gy - sy, dz = gz - sz;
					const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

					float cx, cy, cz, cr;
					if (dist + gr <= sr) {
						cx = sx; cy = sy; cz = sz; cr = sr;
					}
					else if (dist + sr <= gr) {
						cx = gx; cy = gy; cz = gz; cr = gr;
					}
					else {
						cr = (dist + sr + gr) * 0.5f;
						const float t = (cr - sr) / std::max(dist, 1e-12f);
						cx = sx + dx * t;
						cy = sy + dy * t;
						cz = sz + dz * t;
					}
					// Pad for floating-point rounding in the minimal-enclosing
					// sphere formula to guarantee strict enclosure.
					cr *= (1.0f + 1e-5f);

					node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(cx, cy, cz, cr);
				}
				else
				{
					node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(
						grp.bounds.center[0],
						grp.bounds.center[1],
						grp.bounds.center[2],
						grp.bounds.radius * (1.0f + 1e-5f));
				}
				node.traversalMetric.lodBoundingSphere = DirectX::XMFLOAT4(
					grp.bounds.center[0],
					grp.bounds.center[1],
					grp.bounds.center[2],
					grp.bounds.radius);
				// Meshoptimizer condition 1 is evaluated against the simplified
				// error of the group that owns this segment. Internal traversal
				// nodes propagate the max of their children below.
				node.traversalMetric.maxQuadricError = TraversalNodeErrorFromGroupError(grp.bounds.error);
			}

			if (leafCount == 1)
			{
				if (range.offset != state.lodLevelRoots[depth] || range.count != 1u)
				{
					throw std::runtime_error("Cluster LOD: single-leaf traversal range mismatch.");
				}
				continue;
			}

			uint32_t iterCount = leafCount;

			std::vector<uint32_t> partitioned;
			std::vector<ClusterLODNode> scratch;

			while (iterCount > 1)
			{
				const uint32_t lastCount = iterCount;
				ClusterLODNode* lastNodes = &state.nodes[lastLayerOffset];

				partitioned.resize(lastCount);
				meshopt_spatialClusterPoints(
					partitioned.data(),
					&lastNodes->traversalMetric.cullingSphere.x,
					lastCount,
					sizeof(ClusterLODNode),
					preferredNodeWidth);

				scratch.assign(lastNodes, lastNodes + lastCount);
				for (uint32_t n = 0; n < lastCount; ++n)
					lastNodes[n] = scratch[partitioned[n]];

				iterCount = (lastCount + preferredNodeWidth - 1) / preferredNodeWidth;

				ClusterLODNode* newNodes = (iterCount == 1) ? &state.nodes[1 + depth] : &state.nodes[writeOffset];

				for (uint32_t n = 0; n < iterCount; ++n)
				{
					ClusterLODNode& node = newNodes[n];

					const uint32_t childBegin = n * preferredNodeWidth;
					const uint32_t childEnd = std::min(childBegin + preferredNodeWidth, lastCount);
					const uint32_t childCount = childEnd - childBegin;

					ClusterLODNode* children = &lastNodes[childBegin];

					node = {};
					node.range.isGroup = 0;
					node.range.indexOrOffset = lastLayerOffset + childBegin;
					node.range.countMinusOne = childCount - 1;

					float maxErr = 0.f;
					for (uint32_t c = 0; c < childCount; ++c)
						maxErr = std::max(maxErr, children[c].traversalMetric.maxQuadricError);
					node.traversalMetric.maxQuadricError = maxErr;

					meshopt_Bounds mergedCull = meshopt_computeSphereBounds(
						&children[0].traversalMetric.cullingSphere.x,
						childCount,
						sizeof(ClusterLODNode),
						&children[0].traversalMetric.cullingSphere.w,
						sizeof(ClusterLODNode));
					meshopt_Bounds mergedLod = meshopt_computeSphereBounds(
						&children[0].traversalMetric.lodBoundingSphere.x,
						childCount,
						sizeof(ClusterLODNode),
						&children[0].traversalMetric.lodBoundingSphere.w,
						sizeof(ClusterLODNode));

					node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(
						mergedCull.center[0],
						mergedCull.center[1],
						mergedCull.center[2],
						mergedCull.radius * (1.0f + 1e-5f));
					node.traversalMetric.lodBoundingSphere = DirectX::XMFLOAT4(
						mergedLod.center[0],
						mergedLod.center[1],
						mergedLod.center[2],
						mergedLod.radius * (1.0f + 1e-5f));
				}

				lastLayerOffset = writeOffset;
				writeOffset += iterCount;
			}

			writeOffset--;
			if (range.offset + range.count != writeOffset) {
				throw std::runtime_error("Cluster LOD: traversal node allocation mismatch (range/count).");
			}
		}

		{
			auto BuildInternalNode = [&](uint32_t childOffset, uint32_t childCount, bool structuralNode = false) -> ClusterLODNode {
				if (childCount == 0)
					throw std::runtime_error("Cluster LOD: internal node with zero children");

				ClusterLODNode node{};
				node.range.isGroup = 0;
				node.range.indexOrOffset = childOffset;
				node.range.countMinusOne = childCount - 1;

				const ClusterLODNode* children = &state.nodes[childOffset];

				float maxErr = 0.f;
				for (uint32_t c = 0; c < childCount; ++c)
					maxErr = std::max(maxErr, children[c].traversalMetric.maxQuadricError);
				node.traversalMetric.maxQuadricError = structuralNode
					? std::max(kClusterLODStructuralTraversalError, maxErr)
					: maxErr;

				meshopt_Bounds mergedCull = meshopt_computeSphereBounds(
					&children[0].traversalMetric.cullingSphere.x,
					childCount,
					sizeof(ClusterLODNode),
					&children[0].traversalMetric.cullingSphere.w,
					sizeof(ClusterLODNode));
				meshopt_Bounds mergedLod = meshopt_computeSphereBounds(
					&children[0].traversalMetric.lodBoundingSphere.x,
					childCount,
					sizeof(ClusterLODNode),
					&children[0].traversalMetric.lodBoundingSphere.w,
					sizeof(ClusterLODNode));

				node.traversalMetric.cullingSphere = DirectX::XMFLOAT4(
					mergedCull.center[0],
					mergedCull.center[1],
					mergedCull.center[2],
					mergedCull.radius * (1.0f + 1e-5f));
				node.traversalMetric.lodBoundingSphere = DirectX::XMFLOAT4(
					mergedLod.center[0],
					mergedLod.center[1],
					mergedLod.center[2],
					mergedLod.radius * (1.0f + 1e-5f));

				return node;
			};

			std::vector<uint32_t> currentLayer;
			currentLayer.reserve(lodLevelCount);
			for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
				currentLayer.push_back(state.lodLevelRoots[depth]);

			while (currentLayer.size() > preferredNodeWidth)
			{
				std::vector<uint32_t> nextLayer;
				nextLayer.reserve((currentLayer.size() + preferredNodeWidth - 1) / preferredNodeWidth);

				for (uint32_t begin = 0; begin < currentLayer.size(); begin += preferredNodeWidth)
				{
					const uint32_t childCount = std::min<uint32_t>(preferredNodeWidth, uint32_t(currentLayer.size()) - begin);
					const uint32_t childOffset = currentLayer[begin];

					for (uint32_t c = 1; c < childCount; ++c)
					{
						if (currentLayer[begin + c] != childOffset + c)
						{
							throw std::runtime_error("Cluster LOD: expected contiguous node ids while building top hierarchy");
						}
					}

					ClusterLODNode parent = BuildInternalNode(childOffset, childCount, true);
					const uint32_t parentId = uint32_t(state.nodes.size());
					state.nodes.push_back(parent);
					nextLayer.push_back(parentId);
				}

				currentLayer = std::move(nextLayer);
			}

			if (currentLayer.empty())
				throw std::runtime_error("Cluster LOD: top hierarchy has no roots");

			const uint32_t rootChildOffset = currentLayer.front();
			const uint32_t rootChildCount = uint32_t(currentLayer.size());

			for (uint32_t c = 1; c < rootChildCount; ++c)
			{
				if (currentLayer[c] != rootChildOffset + c)
				{
					throw std::runtime_error("Cluster LOD: expected contiguous root children in top hierarchy");
				}
			}

			ClusterLODNode& root = state.nodes[0];
			root = BuildInternalNode(rootChildOffset, rootChildCount, true);

			state.topRootNode = 0;
			state.maxTraversalDepth = ComputeCLodTraversalDepth(state.nodes, state.topRootNode);

			uint32_t internalNodes = 0;
			uint32_t voxelLeafNodes = 0;
			uint32_t segmentLeafNodes = 0;
			uint32_t instanceRootNodes = 0;
			uint32_t invalidNodeKindCount = 0;
			for (const ClusterLODNode& node : state.nodes)
			{
				switch (node.range.isGroup)
				{
				case 0u: ++internalNodes; break;
				case 1u: ++voxelLeafNodes; break;
				case 2u: ++segmentLeafNodes; break;
				case 3u: ++instanceRootNodes; break;
				default: ++invalidNodeKindCount; break;
				}
			}

			uint32_t refinedEdgeCount = 0u;
			uint32_t monotonicErrorViolations = 0u;
			uint32_t invalidSegmentRanges = 0u;
			uint32_t invalidSegmentDomains = 0u;
			uint32_t voxelPayloadMissing = 0u;
			uint32_t voxelTrianglePayloadLeaks = 0u;
			uint32_t invalidPageMapSegments = 0u;
			for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(state.groups.size()); ++groupIndex)
			{
				const ClusterLODGroup& group = state.groups[groupIndex];
				if (group.firstSegment + group.segmentCount > state.segments.size())
				{
					invalidSegmentRanges++;
					continue;
				}

				const bool isVoxelGroup = (group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u;
				if (isVoxelGroup)
				{
					if (GetVoxelPackedClusterCountForGroup(state, groupIndex) == 0u || GetVoxelPackedCubeCountForGroup(state, groupIndex) == 0u || group.pageCount == 0u)
					{
						voxelPayloadMissing++;
					}
					const bool groupCountsLeak = group.meshletCount != 0u || group.groupVertexCount != 0u;
					const bool chunkCountsLeak = groupIndex < state.groupChunks.size() &&
						(state.groupChunks[groupIndex].meshletCount != 0u ||
							state.groupChunks[groupIndex].groupVertexCount != 0u ||
							state.groupChunks[groupIndex].meshletTrianglesByteCount != 0u);
					if (groupCountsLeak || chunkCountsLeak)
					{
						voxelTrianglePayloadLeaks++;
					}
				}

				for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
				{
					const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
					if (segment.meshletCount != 0u &&
						(segment.pageIndex < group.pageMapBase ||
							segment.pageIndex >= group.pageMapBase + group.pageCount))
					{
						invalidPageMapSegments++;
					}
					if (!isVoxelGroup && groupIndex < state.groupMeshletRefinedGroupChunks.size())
					{
						const std::vector<int32_t>& tags = state.groupMeshletRefinedGroupChunks[groupIndex];
						const uint32_t firstMeshlet = ComputeGroupSegmentFirstMeshlet(state, group, segment);
						if (firstMeshlet + segment.meshletCount > tags.size())
						{
							invalidSegmentRanges++;
						}
						else
						{
							for (uint32_t meshletOffset = 0; meshletOffset < segment.meshletCount; ++meshletOffset)
							{
								if (tags[firstMeshlet + meshletOffset] != segment.refinedGroup)
								{
									invalidSegmentDomains++;
									if (invalidSegmentDomains <= 8u)
									{
										spdlog::error(
											"ClusterLOD hierarchy validation segment domain violation: group={} segment={} page={} first_in_page={} meshlets={} segment_refined={} meshlet={} meshlet_refined={}",
											groupIndex,
											segmentOffset,
											segment.pageIndex,
											segment.firstMeshletInPage,
											segment.meshletCount,
											segment.refinedGroup,
											firstMeshlet + meshletOffset,
											tags[firstMeshlet + meshletOffset]);
									}
									break;
								}
							}
						}
					}

					if (segment.refinedGroup < 0)
					{
						continue;
					}

					refinedEdgeCount++;
					const uint32_t childGroupIndex = static_cast<uint32_t>(segment.refinedGroup);
					if (childGroupIndex >= state.groups.size())
					{
						invalidSegmentRanges++;
						continue;
					}

					const float parentError = group.bounds.error;
					const float childError = state.groups[childGroupIndex].bounds.error;
					const bool finiteParent = IsFiniteContentTraversalError(parentError);
					const bool finiteChild = IsFiniteContentTraversalError(childError);
					if (finiteParent && finiteChild && !(parentError > childError))
					{
						monotonicErrorViolations++;
						if (monotonicErrorViolations <= 8u)
						{
							spdlog::error(
								"ClusterLOD hierarchy validation monotonic violation: parent_group={} child_group={} parent_depth={} child_depth={} parent_error={} child_error={} parent_flags=0x{:X} child_flags=0x{:X}",
								groupIndex,
								childGroupIndex,
								group.depth,
								state.groups[childGroupIndex].depth,
								parentError,
								childError,
								group.flags,
								state.groups[childGroupIndex].flags);
						}
					}
				}
			}

			uint32_t invalidNodeRanges = 0u;
			uint32_t invalidLeafOwners = 0u;
			uint32_t invalidLeafPayloads = 0u;
			uint32_t internalMaxErrorViolations = 0u;
			std::vector<uint8_t> reachableNodes(state.nodes.size(), 0u);
			std::vector<uint32_t> nodeStack;
			nodeStack.push_back(state.topRootNode);
			while (!nodeStack.empty())
			{
				const uint32_t nodeIndex = nodeStack.back();
				nodeStack.pop_back();
				if (nodeIndex >= state.nodes.size() || reachableNodes[nodeIndex] != 0u)
				{
					continue;
				}

				reachableNodes[nodeIndex] = 1u;
				const ClusterLODNode& node = state.nodes[nodeIndex];
				if (node.range.isGroup == 0u)
				{
					const uint32_t childCount = node.range.countMinusOne + 1u;
					if (childCount == 0u || childCount > preferredNodeWidth || node.range.indexOrOffset + childCount > state.nodes.size())
					{
						invalidNodeRanges++;
						continue;
					}

					float maxChildError = 0.0f;
					for (uint32_t childOffset = 0; childOffset < childCount; ++childOffset)
					{
						const uint32_t childNodeIndex = node.range.indexOrOffset + childOffset;
						maxChildError = std::max(maxChildError, state.nodes[childNodeIndex].traversalMetric.maxQuadricError);
						nodeStack.push_back(childNodeIndex);
					}
					if (node.traversalMetric.maxQuadricError + 1.0e-8f < maxChildError)
					{
						internalMaxErrorViolations++;
					}
					continue;
				}

				if (node.range.ownerGroupId >= state.groups.size())
				{
					invalidLeafOwners++;
					continue;
				}

				const ClusterLODGroup& ownerGroup = state.groups[node.range.ownerGroupId];
				if (node.range.isGroup == 1u)
				{
					if ((ownerGroup.flags & CLOD_GROUP_FLAG_IS_VOXEL) == 0u || node.range.indexOrOffset >= ownerGroup.segmentCount)
					{
						invalidLeafPayloads++;
					}
				}
				else if (node.range.isGroup == 2u)
				{
					if (node.range.indexOrOffset >= state.segments.size())
					{
						invalidLeafPayloads++;
					}
				}
				else if (node.range.isGroup == 3u)
				{
					if ((ownerGroup.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) == 0u)
					{
						invalidLeafPayloads++;
					}
				}
			}

			uint32_t unreachableNodes = 0u;
			for (uint8_t reachable : reachableNodes)
			{
				if (reachable == 0u)
				{
					unreachableNodes++;
				}
			}

			const bool hierarchyValidationFailed =
				invalidNodeKindCount != 0u || invalidNodeRanges != 0u || invalidLeafOwners != 0u || invalidLeafPayloads != 0u ||
				internalMaxErrorViolations != 0u || monotonicErrorViolations != 0u || invalidSegmentRanges != 0u || invalidSegmentDomains != 0u ||
				invalidPageMapSegments != 0u || voxelPayloadMissing != 0u || voxelTrianglePayloadLeaks != 0u;
			auto logHierarchyValidationSummary = [&]()
			{
				spdlog::log(
					hierarchyValidationFailed ? spdlog::level::warn : spdlog::level::debug,
					"ClusterLOD runtime hierarchy validation: groups={} refined_edges={} nodes={} reachable_nodes={} unreachable_nodes={} invalid_node_kinds={} invalid_node_ranges={} invalid_leaf_owners={} invalid_leaf_payloads={} internal_max_error_violations={} monotonic_error_violations={} invalid_segment_ranges={} invalid_segment_domains={} invalid_page_map_segments={} voxel_payload_missing={} voxel_triangle_payload_leaks={}",
					state.groups.size(),
					refinedEdgeCount,
					state.nodes.size(),
					state.nodes.size() - unreachableNodes,
					unreachableNodes,
					invalidNodeKindCount,
					invalidNodeRanges,
					invalidLeafOwners,
					invalidLeafPayloads,
					internalMaxErrorViolations,
					monotonicErrorViolations,
					invalidSegmentRanges,
					invalidSegmentDomains,
					invalidPageMapSegments,
					voxelPayloadMissing,
					voxelTrianglePayloadLeaks);
			};
			logHierarchyValidationSummary();

			for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
			{
				uint32_t groupsAtDepth = 0u;
				uint32_t voxelGroupsAtDepth = 0u;
				uint32_t triangleGroupsAtDepth = 0u;
				uint32_t refinedEdgesAtDepth = 0u;
				float minErrorAtDepth = std::numeric_limits<float>::max();
				float maxErrorAtDepth = 0.0f;
				for (const ClusterLODGroup& group : state.groups)
				{
					if (static_cast<uint32_t>(std::max(group.depth, 0)) != depth)
					{
						continue;
					}

					groupsAtDepth++;
					if ((group.flags & CLOD_GROUP_FLAG_IS_VOXEL) != 0u)
					{
						voxelGroupsAtDepth++;
					}
					else
					{
						triangleGroupsAtDepth++;
					}
					for (uint32_t segmentOffset = 0; segmentOffset < group.segmentCount; ++segmentOffset)
					{
						const ClusterLODGroupSegment& segment = state.segments[group.firstSegment + segmentOffset];
						if (segment.refinedGroup >= 0)
						{
							refinedEdgesAtDepth++;
						}
					}
					minErrorAtDepth = std::min(minErrorAtDepth, group.bounds.error);
					maxErrorAtDepth = std::max(maxErrorAtDepth, group.bounds.error);
				}

				if (groupsAtDepth == 0u)
				{
					minErrorAtDepth = 0.0f;
				}

				uint32_t voxelLeavesAtDepth = 0u;
				uint32_t segmentLeavesAtDepth = 0u;
				for (const TraversalLeafInfo& leaf : leavesByDepth[depth])
				{
					voxelLeavesAtDepth += (leaf.nodeKind == 1u) ? 1u : 0u;
					segmentLeavesAtDepth += (leaf.nodeKind == 2u) ? 1u : 0u;
				}

				const uint32_t rootNodeId = state.lodLevelRoots[depth];
				const ClusterLODNode& rootNode = state.nodes[rootNodeId];
				const ClusterLODNodeRangeAlloc range = state.lodNodeRanges[depth];
				spdlog::debug(
					"ClusterLOD runtime hierarchy level: depth={} root={} root_kind={} root_children={} root_error={} range_offset={} range_count={} groups={} voxel_groups={} triangle_groups={} refined_edges={} leaves={} voxel_leaves={} segment_leaves={} min_group_error={} max_group_error={}",
					depth,
					rootNodeId,
					rootNode.range.isGroup,
					(rootNode.range.isGroup == 0u) ? (rootNode.range.countMinusOne + 1u) : 0u,
					rootNode.traversalMetric.maxQuadricError,
					range.offset,
					range.count,
					groupsAtDepth,
					voxelGroupsAtDepth,
					triangleGroupsAtDepth,
					refinedEdgesAtDepth,
					leavesByDepth[depth].size(),
					voxelLeavesAtDepth,
					segmentLeavesAtDepth,
					minErrorAtDepth,
					maxErrorAtDepth);
			}

			if (hierarchyValidationFailed)
			{
				throw std::runtime_error("Cluster LOD: runtime hierarchy validation failed; see preceding ClusterLOD runtime hierarchy validation logs");
			}

			spdlog::debug(
				"ClusterLOD traversal hierarchy: nodes={} levels={} top_root={} max_depth={} max_traversal_depth={} internal_nodes={} voxel_leaf_nodes={} segment_leaf_nodes={} instance_root_nodes={}",
				state.nodes.size(),
				lodLevelCount,
				state.topRootNode,
				state.maxDepth,
				state.maxTraversalDepth,
				internalNodes,
				voxelLeafNodes,
				segmentLeafNodes,
				instanceRootNodes);

			const ClusterLODNode& topRoot = state.nodes[state.topRootNode];
			const uint32_t topRootChildCount = topRoot.range.countMinusOne + 1u;
			spdlog::debug(
				"ClusterLOD traversal top root: child_offset={} child_count={} max_error={} lod_radius={}",
				topRoot.range.indexOrOffset,
				topRootChildCount,
				topRoot.traversalMetric.maxQuadricError,
				topRoot.traversalMetric.lodBoundingSphere.w);
			for (uint32_t childIndex = 0; childIndex < topRootChildCount; ++childIndex)
			{
				const uint32_t childNodeId = topRoot.range.indexOrOffset + childIndex;
				const ClusterLODNode& childNode = state.nodes[childNodeId];
				spdlog::debug(
					"ClusterLOD traversal top root child: index={} node_id={} kind={} child_count={} max_error={} lod_radius={} owner_group={}",
					childIndex,
					childNodeId,
					childNode.range.isGroup,
					(childNode.range.isGroup == 0u) ? (childNode.range.countMinusOne + 1u) : 0u,
					childNode.traversalMetric.maxQuadricError,
					childNode.traversalMetric.lodBoundingSphere.w,
					(childNode.range.isGroup == 0u) ? -1 : static_cast<int32_t>(childNode.range.ownerGroupId));
			}

			for (uint32_t depth = 0; depth < lodLevelCount; ++depth)
			{
				const uint32_t nodeId = state.lodLevelRoots[depth];
				const ClusterLODNode& depthRoot = state.nodes[nodeId];
				const uint32_t nodeKind = depthRoot.range.isGroup;
				const uint32_t childCount = (nodeKind == 0u) ? (depthRoot.range.countMinusOne + 1u) : 0u;
				int32_t ownerGroupId = -1;
				float ownerGroupError = -1.0f;
				uint32_t ownerGroupFlags = 0u;
				uint32_t ownerGroupSegments = 0u;

				if (nodeKind != 0u && depthRoot.range.ownerGroupId < state.groups.size())
				{
					ownerGroupId = static_cast<int32_t>(depthRoot.range.ownerGroupId);
					const ClusterLODGroup& ownerGroup = state.groups[depthRoot.range.ownerGroupId];
					ownerGroupError = ownerGroup.bounds.error;
					ownerGroupFlags = ownerGroup.flags;
					ownerGroupSegments = ownerGroup.segmentCount;
				}

				spdlog::debug(
					"ClusterLOD traversal depth root: depth={} node_id={} kind={} child_count={} max_error={} lod_radius={} owner_group={} owner_error={} owner_flags=0x{:X} owner_segments={}",
					depth,
					nodeId,
					nodeKind,
					childCount,
					depthRoot.traversalMetric.maxQuadricError,
					depthRoot.traversalMetric.lodBoundingSphere.w,
					ownerGroupId,
					ownerGroupError,
					ownerGroupFlags,
					ownerGroupSegments);
			}
		}
	}
}
