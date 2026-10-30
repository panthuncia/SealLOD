#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "BasicRenderer/Pipeline/CLodOptions.h"
#include "BasicRenderer/Pipeline/RenderPhase.h"

namespace org { class Buffer; }
namespace org { class PixelBuffer; }
namespace org { class ResourceGroup; }

// What a hierarchical culling pass culls for. Both backends, the work graph
// (WorkGraphCullingPass) and pure compute (ComputeCullingPass), take the same
// configuration and resources; each reads only the parts it implements.
struct CullingPassConfig {
    // Names the pass-owned voxel queue table; unique per pass instance.
    std::string stablePassIdentifier;
    // Phase 1 culls objects from the scene. Phase 2 replays what phase 1 rejected
    // for occlusion, against the depth phase 1 produced.
    bool isFirstPass = true;
    uint32_t maxVisibleClusters = 0u;
    CLodSoftwareRasterMode softwareRaster = CLodSoftwareRasterMode::WorkGraph;
    CLodRasterOutputKind rasterOutputKind = CLodRasterOutputKind::VisibilityBuffer;
    RenderPhase renderPhase;
    bool clodOnlyWorkloads = false;
    bool useShadowCascadeViews = false;

    // Clusters are classified into hardware and software raster lists.
    bool UsesSWClassification() const { return CLodSoftwareRasterEnabled(softwareRaster); }
    bool UsesVisibilityBufferOutput() const { return rasterOutputKind == CLodRasterOutputKind::VisibilityBuffer; }
    bool UsesVirtualShadowOutput() const { return rasterOutputKind == CLodRasterOutputKind::VirtualShadow; }
    // Every output except virtual shadows occludes against a per-view depth pyramid.
    bool UsesPerViewDepthOcclusion() const { return !UsesVirtualShadowOutput(); }
};

// Clusters that survived culling, and the counters raster passes consume.
struct CullingOutputBuffers {
    std::shared_ptr<org::Buffer> visibleClusters;
    std::shared_ptr<org::Buffer> visibleClusterTransformIndices;
    std::shared_ptr<org::Buffer> visibleClustersCounter;
    std::shared_ptr<org::Buffer> swVisibleClustersCounter;
    std::shared_ptr<org::Buffer> rasterBucketHistogramCommand;
};

// Phase 2 appends after phase 1, so it reads phase 1's counters as write bases.
// Empty in phase 1.
struct CullingPhase1Counters {
    std::shared_ptr<org::Buffer> visibleClustersCounter;
    std::shared_ptr<org::Buffer> swVisibleClustersCounter;
};

// Work phase 1 rejected for occlusion, replayed by phase 2.
struct CullingOcclusionReplayBuffers {
    std::shared_ptr<org::Buffer> replay;
    std::shared_ptr<org::Buffer> state;
    // GPU dispatch records for the work graph's phase-2 launch.
    std::shared_ptr<org::Buffer> nodeGpuInputs;
};

// Clusters routed to voxel rasterization. Disabled when capacity is zero.
struct CullingVoxelRasterQueues {
    std::shared_ptr<org::Buffer> rigidWork;
    std::shared_ptr<org::Buffer> rigidWorkCounter;
    std::shared_ptr<org::Buffer> skinnedWork;
    std::shared_ptr<org::Buffer> skinnedWorkCounter;
    uint32_t capacity = 0u;

    bool Enabled() const { return capacity != 0u; }
};

// Virtual shadow map state that culling reads and writes. Empty for other outputs.
struct CullingVirtualShadowResources {
    std::shared_ptr<org::PixelBuffer> dirtyPageHierarchy;
    std::shared_ptr<org::Buffer> predictiveInvalidationCandidates;
    std::shared_ptr<org::Buffer> predictiveInvalidationCandidateCount;
    std::shared_ptr<org::Buffer> invalidationCount;          // Pure compute only.
    std::shared_ptr<org::Buffer> invalidatedInstancesBitset;
    std::shared_ptr<org::PixelBuffer> pageTable;
    std::shared_ptr<org::PixelBuffer> staticPhysicalPages;
    std::shared_ptr<org::Buffer> activeBlockMetadata;
    std::shared_ptr<org::Buffer> receiverSubpageMask;        // Pure compute only.
    std::shared_ptr<org::PixelBuffer> dynamicPhysicalPages;
    std::shared_ptr<org::Buffer> dynamicActiveBlockMetadata;
};

// Reyes patch visibility inside the work graph. Work-graph backend only.
struct CullingWorkGraphReyesResources {
    bool enabled = false;
    std::shared_ptr<org::Buffer> diceQueue;
    std::shared_ptr<org::Buffer> diceQueueCounter;
    std::shared_ptr<org::Buffer> diceQueueOverflow;
    std::shared_ptr<org::Buffer> tessTableConfigs;
    std::shared_ptr<org::Buffer> tessTableVertices;
    std::shared_ptr<org::Buffer> tessTableTriangles;
    std::shared_ptr<org::Buffer> telemetry;
    uint32_t diceQueueCapacity = 0u;
};

struct CullingPassResources {
    CullingOutputBuffers outputs;
    CullingPhase1Counters phase1Counters;
    CullingOcclusionReplayBuffers occlusionReplay;
    std::shared_ptr<org::Buffer> telemetry;
    // Page pool slabs, declared for bindless access.
    std::shared_ptr<org::ResourceGroup> pagePoolSlabs;
    CullingVoxelRasterQueues voxelQueues;
    CullingVirtualShadowResources virtualShadow;
    CullingWorkGraphReyesResources reyes;
};
