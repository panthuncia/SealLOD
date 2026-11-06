#ifndef CLOD_RAY_TRACING_SCENE_HLSLI
#define CLOD_RAY_TRACING_SCENE_HLSLI

// CLAS bookkeeping shared by the scene build and the passes that interpret hits. CLodRayTracingSystem.h has the
// C++ side of these layouts.

#define CLOD_RT_INVALID_SLOT 0xFFFFFFFFu

// One per CLAS, indexed by slot, which is also the CLAS's ClusterID: what a hit needs to find its geometry.
struct CLodRtClasInfo
{
    uint slabDescriptor;
    uint pageOffsetUnits; // Page byte offset >> 14, as in the packed visible cluster.
    uint localMeshlet;
    uint groupLocalIndex;
};

// A contiguous run of meshlets in one page, and the CLAS slots they were given.
struct CLodRtClasSegment
{
    uint slabDescriptor;
    uint pageOffsetUnits;
    uint firstMeshlet;
    uint meshletCount;
    uint firstClasSlot;
    uint padding;
};

// Group table: [first segment, segment count] by global group index. A count of zero means no CLAS yet.
uint CLodRtFindClasSlot(uint groupTableIndex, uint segmentTableIndex, uint groupGlobal, uint slabDescriptor, uint pageOffsetUnits, uint localMeshlet)
{
    StructuredBuffer<uint2> groupTable = ResourceDescriptorHeap[groupTableIndex];
    StructuredBuffer<CLodRtClasSegment> segments = ResourceDescriptorHeap[segmentTableIndex];
    const uint2 entry = groupTable[groupGlobal];
    for (uint i = 0u; i < entry.y; ++i) {
        const CLodRtClasSegment segment = segments[entry.x + i];
        if (segment.slabDescriptor == slabDescriptor && segment.pageOffsetUnits == pageOffsetUnits
            && localMeshlet >= segment.firstMeshlet && localMeshlet - segment.firstMeshlet < segment.meshletCount) {
            return segment.firstClasSlot + (localMeshlet - segment.firstMeshlet);
        }
    }
    return CLOD_RT_INVALID_SLOT;
}

#endif // CLOD_RAY_TRACING_SCENE_HLSLI
