#ifndef CLOD_RAY_TRACING_SCENE_HLSLI
#define CLOD_RAY_TRACING_SCENE_HLSLI

// CLAS bookkeeping shared by the scene build and the passes that interpret hits. CLodRayTracingSystem.h has the
// C++ side of these layouts.

#define CLOD_RT_INVALID_SLOT 0xFFFFFFFFu

#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// Decoding a TLAS row (see CLOD_RT_INSTANCE_ID_PORTAL). A pass names its instance rows SRV by defining
// CLOD_RT_INSTANCE_ROWS_SRV before including this; secondary rays and path tracing use the default.
#ifndef CLOD_RT_INSTANCE_ROWS_SRV
#define CLOD_RT_INSTANCE_ROWS_SRV CLOD_RT_SEC_INSTANCE_ROWS_SRV
#endif

uint CLodRtInstanceDraw(uint instanceID)
{
    return instanceID & 0x7FFFFFu;
}

// The assembly transform a hit's cluster was placed with: sentinel for a draw's own clusters.
uint CLodRtInstanceTransform(uint instanceID, uint instanceIndex)
{
    if ((instanceID & 0x800000u) == 0u)
    {
        return 0xFFFFFFFFu;
    }
    StructuredBuffer<uint4> rows = ResourceDescriptorHeap[CLOD_RT_INSTANCE_ROWS_SRV];
    return rows[instanceIndex].y;
}

// One per CLAS, indexed by slot, which is also the CLAS's ClusterID: what a hit needs to find its geometry.
struct CLodRtClasInfo
{
    uint slabDescriptor;
    uint pageOffsetUnits; // Page byte offset >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT, as in the packed visible cluster.
    uint localMeshlet;
    uint groupLocalIndex; // The global group; bit 31 set when the CLAS has micromaps. Read through the helpers below.
};

#define CLOD_RT_CLAS_INFO_HAS_MICROMAPS 0x80000000u

uint CLodRtClasGroup(CLodRtClasInfo info)
{
    return info.groupLocalIndex & ~CLOD_RT_CLAS_INFO_HAS_MICROMAPS;
}

bool CLodRtClasHasMicromaps(CLodRtClasInfo info)
{
    return (info.groupLocalIndex & CLOD_RT_CLAS_INFO_HAS_MICROMAPS) != 0u;
}

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
