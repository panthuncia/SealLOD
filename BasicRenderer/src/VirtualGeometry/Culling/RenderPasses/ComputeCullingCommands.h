#pragma once

// Command recording for pure-compute hierarchical culling. Traversal runs one
// BVH level per step: each level expands the current node and leaf frontiers
// into the next frontiers plus a cluster frontier, then culls that cluster
// frontier. Phase 1 seeds the first node frontier from object culling; phase 2
// seeds node and cluster frontiers from what phase 1 rejected for occlusion.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <rhi.h>

#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "Render/DeclaredTableLayout.h"
#include "VirtualGeometry/GraphIntegration/CLodViewTables.h"
#include <BasicRenderer/Pipeline/PipelineDesc.h>
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "VirtualGeometry/Culling/PreparedCullingWorkloads.h"
#include "VirtualGeometry/Culling/RenderPasses/CullingPassCommon.h"

inline constexpr uint32_t kComputeCullingMaxTraversalLevels = 64u;
inline constexpr uint32_t kComputeCullingObjectThreadsPerGroup = 64u;

struct ComputeCullingPrograms {
    br::PipelineState clearCounter;
    br::PipelineState createRasterBucketCommand;
    br::PipelineState buildDispatchArgs;
    br::PipelineState buildDualDispatchArgs;
    br::PipelineState buildReplayDispatchArgs;
    br::PipelineState clearTraversalCounters;
    br::PipelineState objectCull;
    br::PipelineState seedReplayNodes;
    br::PipelineState seedReplayClusters;
    br::PipelineState traverseNodes;
    br::PipelineState traverseLeaves;
    br::PipelineState cullClusters;
    br::CommandSignatureState dispatchSignature;
};

// Traversal working set. Node and leaf frontiers ping-pong between levels;
// each frontier has a counter and an indirect dispatch argument buffer.
struct ComputeCullingFrontiers {
    std::shared_ptr<org::Buffer> currentNodes, nextNodes, currentLeaves, nextLeaves, clusters;
    std::shared_ptr<org::Buffer> currentNodeCounter, nextNodeCounter, currentLeafCounter, nextLeafCounter, clusterCounter;
    std::shared_ptr<org::Buffer> nodeDispatchArgs, leafDispatchArgs, clusterDispatchArgs;
};

// The frontiers as declared: unordered-access views. Stages that only read a
// frontier use its shader-resource view after an in-pass barrier.
struct ComputeCullingFrontierViews {
    org::DeclaredViewToken currentNodes, nextNodes, currentLeaves, nextLeaves, clusters;
    org::DeclaredViewToken currentNodeCounter, nextNodeCounter, currentLeafCounter, nextLeafCounter, clusterCounter;
    org::DeclaredViewToken nodeDispatchArgs, leafDispatchArgs, clusterDispatchArgs;
};

// Per-meshlet bounds cached across frames for DynamicWind-animated shadow
// casters. Phase 1 virtual shadows only.
struct ComputeCullingWindCache {
    std::shared_ptr<org::Buffer> bounds;
    uint32_t entryCount = 0u;
    uint32_t generation = 1u;
};

// Virtual shadow map state, declared only for virtual shadow output. Absent
// views read as descriptor 0.
struct ComputeCullingShadowViews {
    org::DeclaredViewToken dirtyPageHierarchy, invalidatedInstancesBitset, invalidationCount;
    org::DeclaredViewToken predictiveInvalidationCandidates, predictiveInvalidationCandidateCount;
    org::DeclaredViewToken pageTable, staticPhysicalPages, dynamicPhysicalPages;
    org::DeclaredViewToken activeBlockMetadata, receiverSubpageMask, dynamicActiveBlockMetadata;
};

struct ComputeCullingBindings {
    ComputeCullingPrograms programs;
    org::DeclaredViewToken visibleClusters, visibleClusterTransformIndices, visibleClustersCounter;
    org::DeclaredViewToken swVisibleClustersCounter;  // Software classification only.
    // The unified main view's phase 1: the ray tracing view's cluster list.
    org::DeclaredViewToken rtVisibleClusters, rtVisibleClusterTransformIndices, rtVisibleClustersCounter;
    uint32_t rtVisibleClusterCapacity = 0u;
    org::DeclaredViewToken rasterBucketHistogramCommand, telemetry, replay, replayState;
    ComputeCullingFrontierViews frontiers;
    org::DeclaredViewToken windBounds, windMembership;  // DynamicWind bounds cache only.
    // Phase 2 reads phase 1's counters as write bases.
    org::DeclaredViewToken phase1VisibleClustersCounter, phase1SwVisibleClustersCounter;
    ComputeCullingShadowViews shadow;
    // Rigid work, rigid counter, skinned work, skinned counter.
    std::optional<std::array<org::DeclaredViewToken, 4>> voxelQueues;
    CLodViewRasterDeclaration viewRasterInfoLayout;
    org::DeclaredTableLayout<CLodViewDepthSRVIndex> viewDepthLayout;  // Per-view depth occlusion only.
    uint32_t maxVisibleClusters = 0u;
    bool isFirstPass = true;
    bool perViewDepthOcclusion = false;
    uint32_t rtCoarseCut = 0u;
};

struct ComputeCullingFrame {
    std::vector<br::render::PreparedCullingWorkload> workloads;
    std::shared_ptr<const br::render::PublishedRendererState> publication;
    br::render::CullingSettings settings;
    uint32_t cullingFlags = 0u;
    uint32_t traversalDepth = 0u;
    // Lanes each cluster record expands across in dense cluster culling.
    uint32_t expansionFactor = 1u;
    uint32_t rasterBucketCount = 0u;
    // SRV indices of the per-view tables published for this frame.
    uint32_t viewRasterInfoTable = 0xFFFFFFFFu;
    uint32_t viewDepthTable = 0u;
    uint32_t windEntryCount = 0u;
    uint32_t windGeneration = 1u;
    // False when the frame has no render snapshot: nothing is recorded.
    bool active = false;
    // The revision audit's comparison: everything Record reads. `publication` only keeps the workloads' published tables
    // alive while the packet is, and a newer publication with the same indirect workloads records the same commands.
    bool operator==(const ComputeCullingFrame& other) const {
        return workloads == other.workloads && settings == other.settings && cullingFlags == other.cullingFlags
            && traversalDepth == other.traversalDepth && expansionFactor == other.expansionFactor
            && rasterBucketCount == other.rasterBucketCount && viewRasterInfoTable == other.viewRasterInfoTable
            && viewDepthTable == other.viewDepthTable && windEntryCount == other.windEntryCount
            && windGeneration == other.windGeneration && active == other.active;
    }
};

void RecordComputeCulling(const ComputeCullingBindings& bindings, const ComputeCullingFrame& frame,
    org::PassRecordContext& context);
