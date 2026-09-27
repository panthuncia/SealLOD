#pragma once

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include <functional>
#include <algorithm>

namespace clod_detail {

	inline void AssignSingleRootPartRecord(ClusterLODPrebuiltData& data, uint32_t rootNode)
	{
		ClusterLODPartRecord rootPart{};
		rootPart.groupBase = 0u;
		rootPart.groupCount = static_cast<uint32_t>(data.groups.size());
		rootPart.nodeBase = 0u;
		rootPart.nodeCount = static_cast<uint32_t>(data.nodes.size());
		rootPart.transformBase = 0u;
		rootPart.transformCount = static_cast<uint32_t>(data.assemblyTransforms.size());
		rootPart.instanceBase = 0u;
		rootPart.instanceCount = static_cast<uint32_t>(data.assemblyInstances.size());
		rootPart.rootNode = rootNode;
		rootPart.flags = CLOD_PART_RECORD_FLAG_ROOT;
		data.partRecords.clear();
		data.partRecords.push_back(rootPart);
		data.rootPartIndex = 0u;
	}



	inline BoundingSphere BuildObjectBoundingSphereFromRootNode(const std::vector<ClusterLODNode>& nodes, uint32_t rootNodeIndex)
	{
		BoundingSphere sphere{};
		if (rootNodeIndex >= nodes.size()) {
			return sphere;
		}

		const ClusterLODTraversalMetric& metric = nodes[rootNodeIndex].traversalMetric;
		sphere.sphere = metric.cullingSphere;
		return sphere;
	}


	inline uint32_t ComputeCLodTraversalDepth(const std::vector<ClusterLODNode>& nodes, uint32_t rootNodeIndex)
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
			for (uint32_t childIndex = 0; childIndex < childCount; ++childIndex)
			{
				maxChildDepth = std::max(maxChildDepth, computeNodeDepth(node.range.indexOrOffset + childIndex));
			}

			return 1u + maxChildDepth;
		};

		return computeNodeDepth(rootNodeIndex);
	}


void BuildClusterLODTraversalHierarchy(
    ClusterLODBuildState& state, uint32_t preferredNodeWidth);

}
