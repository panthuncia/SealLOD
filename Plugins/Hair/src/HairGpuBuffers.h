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
    // uint2 per pool row, written by the shade pass on the layered path: the vertex's lit colour as three half floats
    // (hairShading.hlsli's HairPackColor).
    std::shared_ptr<org::Buffer> poolColors;
    // uint: the pool rows the rasterizer marked this frame, gathered by the shade-list pass for the shade pass to light.
    std::shared_ptr<org::Buffer> shadeList;
    std::shared_ptr<org::Buffer> probe;              // uint: the probe pass's header and rows (HairRuntimeGPUTypes.h)
    // The culled rasterizer (hairRasterCulled.hlsl): the first pass's items, each pool row's late chunks, the first
    // pass's dispatch arguments, and the frame each cluster of each instance last drew a pixel in.
    std::shared_ptr<org::Buffer> rasterItems;
    std::shared_ptr<org::Buffer> poolLate;
    std::shared_ptr<org::Buffer> rasterArgs;
    std::shared_ptr<org::Buffer> clusterSeen;
    // The density volumes (the layered path's self-shadowing): HairVolumeGPU, one row per instance row, uploaded every
    // frame; their voxels, uint, cleared and filled by the voxelize pass; and each instance's scatter table,
    // HAIR_SCATTER_TABLE_ROWS rows of HairScatterRow, uploaded when its material changes.
    std::shared_ptr<org::Buffer> volumes;
    std::shared_ptr<org::Buffer> volumeVoxels;
    std::shared_ptr<org::Buffer> scatterTables;
    // Shadow casting (hairShadow.hlsl): the clusters the shadow view keeps (HairVisibleCluster) with the clipmaps each
    // is drawn into (uint, parallel), and their evaluated strands: a second, smaller pool with its segment ids.
    std::shared_ptr<org::Buffer> shadowClusters;
    std::shared_ptr<org::Buffer> shadowClusterMasks;
    std::shared_ptr<org::Buffer> shadowPoolVertices;
    std::shared_ptr<org::Buffer> shadowPoolSegmentIds;
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
    // The layered path (HAIR_PATH_LAYERED). The visibility target is a pixel's nearest layer; these are its second and
    // third, the same format.
    std::shared_ptr<org::PixelBuffer> layer1;
    std::shared_ptr<org::PixelBuffer> layer2;
    // R8G8_UNorm, written by the composite: the hair alpha and HAIR_COMPOSITE_INFO_* (the debug views, the probe).
    std::shared_ptr<org::PixelBuffer> compositeInfo;
    // R8_UNorm, written by the composite: the reactive mask the upscaler takes (Extensions/UpscalerReactiveMask.h).
    std::shared_ptr<org::PixelBuffer> reactiveMask;
    // The culled rasterizer's, made with the layered path's. At the render resolution: the coverage behind a pixel's
    // split depth (R32_UInt), the hidden and split depths (R32G32_UInt, read as uint64_t) and the next frame's split
    // depth (R32_UInt). A texel per tile of HAIR_RASTER_TILE_SIZE pixels: the largest hidden depth and the frame a chunk
    // last touched the tile in (R32_UInt each).
    std::shared_ptr<org::PixelBuffer> coverageFar;
    std::shared_ptr<org::PixelBuffer> occlusion;
    std::shared_ptr<org::PixelBuffer> splitGuess;
    std::shared_ptr<org::PixelBuffer> occlusionCoarse;
    std::shared_ptr<org::PixelBuffer> occlusionHint; // each tile's largest hidden depth over the last frames (R32_UInt)
    std::shared_ptr<org::PixelBuffer> tileFlags;
    std::uint32_t visibilityWidth = 0u;
    std::uint32_t visibilityHeight = 0u;
};

} // namespace br::hair
