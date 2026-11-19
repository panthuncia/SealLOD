#include "Core/ErrorChannels.h"

#include <algorithm>

namespace clodeval {

GroupErrorChannels ReadGroupErrorChannels(const ClusterLODPrebuiltData& data)
{
	GroupErrorChannels channels;
	channels.measured.resize(data.groups.size());
	channels.appearance.assign(data.groups.size(), 0.0f);
	for (size_t index = 0; index < data.groups.size(); ++index) {
		channels.measured[index] = data.groups[index].bounds.error;
	}
	// As MeshManager::AddMesh: boundaries that voxels, forks, merges or assemblies set have no channels.
	bool present = data.assemblyInstances.empty() && data.externalParts.empty() &&
		std::any_of(data.nodes.begin(), data.nodes.end(), [](const ClusterLODNode& node) {
			return (node.traversalMetric.channelFlags & CLOD_NODE_CHANNELS_PRESENT) != 0u;
		});
	for (const ClusterLODGroup& group : data.groups) {
		if ((group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY | CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL |
				CLOD_GROUP_FLAG_MERGE_CHILD | CLOD_GROUP_BRANCH_FLAGS)) != 0u) {
			present = false;
		}
	}
	if (!present) {
		return channels;
	}
	channels.present = true;
	for (const ClusterLODNode& node : data.nodes) {
		if (node.range.isGroup == CLOD_NODE_SEGMENT_LEAF && node.range.ownerGroupId < data.groups.size() &&
			(node.traversalMetric.channelFlags & CLOD_NODE_CHANNELS_PRESENT) != 0u) {
			channels.measured[node.range.ownerGroupId] = node.traversalMetric.maxMeasuredGeometricError;
			channels.appearance[node.range.ownerGroupId] = node.traversalMetric.maxAppearanceError;
		}
	}
	return channels;
}

float SelectionError(float quadricError, float measuredError, float appearanceError, const CutRule& rule)
{
	float error = quadricError;
	if (rule.geometric == CutGeometricChannel::Measured) {
		error = measuredError;
	}
	else if (rule.geometric == CutGeometricChannel::Max) {
		error = std::max(quadricError, measuredError);
	}
	if (rule.appearanceWeight > 0.0f) {
		error = std::max(error, rule.appearanceWeight * appearanceError);
	}
	return error;
}

bool ParseCutGeometricChannel(const std::string& text, CutGeometricChannel& out)
{
	if (text == "quadric") {
		out = CutGeometricChannel::Quadric;
	}
	else if (text == "measured") {
		out = CutGeometricChannel::Measured;
	}
	else if (text == "max") {
		out = CutGeometricChannel::Max;
	}
	else {
		return false;
	}
	return true;
}

const char* CutGeometricChannelName(CutGeometricChannel channel)
{
	switch (channel) {
	case CutGeometricChannel::Measured: return "measured";
	case CutGeometricChannel::Max: return "max";
	default: return "quadric";
	}
}

}
