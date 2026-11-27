#pragma once

#include <cstdint>
#include <memory>

#include "Resources/Buffers/Buffer.h"
#include "Resources/PixelBuffer.h"

namespace br::hair {

// The plugin's pooled buffers (HairCapacities rows each) and the hair visibility target. The groom arrays hold every
// loaded asset back to back; a HairInstanceGPU names an asset's rows by their bases. The struct is replaced, not edited,
// when the target is made again at a new resolution: the passes compare the pointer to know they must declare again.
struct HairGpuBuffers {
    std::shared_ptr<org::Buffer> clusters;           // HairClusterHeader
    std::shared_ptr<org::Buffer> aggregates;         // HairClusterAggregate, parallel to clusters
    std::shared_ptr<org::Buffer> strands;            // HairStrandRecord
    std::shared_ptr<org::Buffer> rootBindings;       // HairRootBinding, parallel to strands
    std::shared_ptr<org::Buffer> guideBindings;      // HairGuideBinding
    std::shared_ptr<org::Buffer> guideStrandIndices; // uint, parallel to guideBindings; groom-relative strand indices
    std::shared_ptr<org::Buffer> points;             // HairPackedPoint
    std::shared_ptr<org::Buffer> instances;          // HairInstanceGPU
    std::shared_ptr<org::Buffer> frameState;         // HairFrameGPU, one row
    std::shared_ptr<org::Buffer> rootCheckList;      // HairRootCheckSample: the roots a self-check asks for
    std::shared_ptr<org::Buffer> rootCheck;          // HairRootFramePair, parallel to it, written by the root-check pass
    std::shared_ptr<org::Buffer> counters;           // uint, HAIR_COUNTER_COUNT rows, incremented on the GPU
    std::shared_ptr<org::Buffer> visibleClusters;    // HairVisibleCluster, written by the select pass
    std::shared_ptr<org::Buffer> poolVertices;       // HairPoolVertex, written by the evaluate pass
    std::shared_ptr<org::Buffer> poolSegmentIds;     // uint, parallel to poolVertices: the segment a row starts
    std::shared_ptr<org::Buffer> probe;              // uint: the probe pass's header and rows (HairRuntimeGPUTypes.h)
    // Simulation.
    std::shared_ptr<org::Buffer> guidePointRanges;   // HairGuidePointRange, parallel to guideBindings
    // HairGuidePoint: the guide state of the instances that hold rows (HairInstanceGPU::guidePointBase), written by the
    // simulate and guide-level passes.
    std::shared_ptr<org::Buffer> guidePoints;
    std::shared_ptr<org::Buffer> colliders;          // HairColliderGPU, HAIR_MAX_COLLIDERS_PER_INSTANCE rows per instance
    std::shared_ptr<org::Buffer> simFrame;           // HairSimFrameGPU, one row
    std::shared_ptr<org::Buffer> simMetrics;         // uint, HAIR_SIM_METRIC_COUNT rows per instance
    std::shared_ptr<org::Buffer> simGroups;          // HairSimGroup: the frame's simulation thread groups
    // The hair visibility target (HairRuntimeGPUTypes.h): R32G32_UInt at the render resolution, read as uint64_t.
    std::shared_ptr<org::PixelBuffer> visibility;
    // The coverage target, the same size: R32_UInt, the summed coverage of the segments over each pixel.
    std::shared_ptr<org::PixelBuffer> coverage;
    std::uint32_t visibilityWidth = 0u;
    std::uint32_t visibilityHeight = 0u;
};

} // namespace br::hair
