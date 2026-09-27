#pragma once

#include "Assets/GeometryProcessing/ClusterLOD/ClusterLODBuildState.h"

namespace clod_detail {

inline constexpr float kClusterLODStructuralTraversalError = 1.0e20f;

float GetVoxelCandidateExpansionRadiusForPayload(const VoxelGroupPayload* payload);
DirectX::XMFLOAT3 ReadGroupVertexPosition(
    const std::vector<std::byte>& vertices, size_t vertexStrideBytes, uint32_t vertexIndex);
uint32_t ComputeGroupSegmentFirstMeshlet(
    const ClusterLODBuildState& state, const ClusterLODGroup& group,
    const ClusterLODGroupSegment& segment);
uint32_t GetVoxelPackedCubeCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex);
uint32_t GetVoxelPackedClusterCountForGroup(const ClusterLODBuildState& state, uint32_t groupIndex);
float ComputeVoxelRepresentationError(float voxelWidth);
uint64_t PackVoxelTailCellKey(uint32_t x, uint32_t y, uint32_t z);
float GetFiniteVoxelErrorForGroup(const ClusterLODBuildState& state, uint32_t groupIndex);
std::vector<uint32_t> CollectUniqueRefinedChildren(
    const ClusterLODBuildState& state, uint32_t groupIndex);
bool IsTerminalErrorSentinel(float error);
bool IsFiniteContentTraversalError(float error);
float TraversalNodeErrorFromGroupError(float error);

void BuildVoxelFallbackCandidates(
    ClusterLODBuildState& state, size_t vertexStrideBytes,
    size_t skinningVertexStrideBytes,
    const VoxelCoverageMaterialSampler* coverageMaterialSampler,
    const ClusterLODBuilderSettings& settings);

}
