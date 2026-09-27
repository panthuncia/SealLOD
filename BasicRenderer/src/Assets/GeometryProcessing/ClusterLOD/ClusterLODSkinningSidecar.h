#pragma once

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"
#include <unordered_map>

namespace clod_detail {

struct NodeBoneSet {
    std::vector<uint32_t> bones;
    uint16_t flags = 0u;
};

void BuildNodeSkinningSidecar(
    const ClusterLODBuildState& state, uint32_t requestedLimit,
    std::vector<ClusterLODNodeSkinningInfo>& outInfos,
    std::vector<uint32_t>& outBoneIndices,
    const std::unordered_map<uint32_t, NodeBoneSet>* preservedSets = nullptr);

}
