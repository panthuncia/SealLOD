#ifndef CLOD_RAY_TRACING_SCENE_HLSLI
#define CLOD_RAY_TRACING_SCENE_HLSLI

// CLAS bookkeeping shared by the scene build and the passes that interpret hits. CLodRayTracingSystem.h has the
// C++ side of these layouts.

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

#endif // CLOD_RAY_TRACING_SCENE_HLSLI
