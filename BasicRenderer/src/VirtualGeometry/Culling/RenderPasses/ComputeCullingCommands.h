#pragma once

// Command emission for pure-compute hierarchical culling. Traversal runs one
// BVH level per step: each level expands the current node and leaf frontiers
// into the next frontiers plus a cluster frontier, then culls that cluster
// frontier. Phase 1 seeds the first node frontier from object culling; phase 2
// seeds node and cluster frontiers from what phase 1 rejected for occlusion.

#include <array>
#include <cstdint>
#include <memory>

#include <rhi.h>

#include "BasicRenderer/Extensions/PreparedRenderGraph/PreparedComputeCommands.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "Render/PipelineState.h"
#include "RenderPasses/Base/TypedRenderGraphPass.h"
#include "VirtualGeometry/Culling/RenderPasses/CullingPassCommon.h"

inline constexpr uint32_t kComputeCullingMaxTraversalLevels = 64u;
inline constexpr uint32_t kComputeCullingObjectThreadsPerGroup = 64u;

struct ComputeCullingPrograms {
    org::PipelineState clearCounter;
    org::PipelineState createRasterBucketCommand;
    org::PipelineState buildDispatchArgs;
    org::PipelineState buildDualDispatchArgs;
    org::PipelineState buildReplayDispatchArgs;
    org::PipelineState clearTraversalCounters;
    org::PipelineState objectCull;
    org::PipelineState seedReplayNodes;
    org::PipelineState seedReplayClusters;
    org::PipelineState traverseNodes;
    org::PipelineState traverseLeaves;
    org::PipelineState cullClusters;
    std::shared_ptr<rhi::CommandSignaturePtr> dispatchSignature;

    std::array<const org::PipelineState*, 12> All() const {
        return {&clearCounter, &createRasterBucketCommand, &buildDispatchArgs, &buildDualDispatchArgs,
            &buildReplayDispatchArgs, &clearTraversalCounters, &objectCull, &seedReplayNodes,
            &seedReplayClusters, &traverseNodes, &traverseLeaves, &cullClusters};
    }
};

// Traversal working set. Node and leaf frontiers ping-pong between levels;
// each frontier has a counter and an indirect dispatch argument buffer.
struct ComputeCullingFrontiers {
    std::shared_ptr<org::Buffer> currentNodes, nextNodes, currentLeaves, nextLeaves, clusters;
    std::shared_ptr<org::Buffer> currentNodeCounter, nextNodeCounter, currentLeafCounter, nextLeafCounter, clusterCounter;
    std::shared_ptr<org::Buffer> nodeDispatchArgs, leafDispatchArgs, clusterDispatchArgs;
};

// Per-meshlet bounds cached across frames for DynamicWind-animated shadow
// casters. Phase 1 virtual shadows only.
struct ComputeCullingWindCache {
    std::shared_ptr<org::Buffer> bounds;
    org::Resource* visibleMembership = nullptr;
    uint32_t entryCount = 0u;
    uint32_t generation = 1u;
};

// Commands before and after object culling. Object culling dispatches once
// per workload, with constants chosen at invocation preparation.
struct ComputeCullingRecipe {
    br::render::PreparedComputeCommandSequence prefix, suffix;
    std::array<uint32_t, NumMiscUintRootConstants> objectCullConstants{};
    bool hasObjectCull = false;
};
inline void RemapDescriptorIndices(ComputeCullingRecipe& recipe, const org::DescriptorIndexRemap& remap) {
    br::render::RemapDescriptorIndices(recipe.prefix, remap);
    br::render::RemapDescriptorIndices(recipe.suffix, remap);
}

struct ComputeCullingCommandInputs {
    const CullingPassConfig& config;
    const CullingPassResources& resources;
    const ComputeCullingPrograms& programs;
    const ComputeCullingFrontiers& frontiers;
    const ComputeCullingWindCache& windCache;
    br::render::CullingSettings settings;
    uint32_t traversalDepth = 0u;
    // Lanes each cluster record expands across in dense cluster culling.
    uint32_t expansionFactor = 1u;
    uint32_t rasterBucketCount = 0u;
    // SRV indices of the per-view tables published for this recipe.
    uint32_t viewRasterInfoTable = 0xFFFFFFFFu;
    uint32_t viewDepthTable = 0u;
};

ComputeCullingRecipe EmitComputeCullingCommands(const ComputeCullingCommandInputs& inputs,
    const org::PassPrepareContext& preparation);
