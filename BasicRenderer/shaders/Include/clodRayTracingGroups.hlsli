#ifndef CLOD_RAY_TRACING_GROUPS_HLSLI
#define CLOD_RAY_TRACING_GROUPS_HLSLI

// Read-only facts about a CLod group that the ray tracing view and the CLAS cache share. Nothing here depends on the
// cache's state: culling uses CLodRtGroupBuildable to keep groups no CLAS can be built from out of the BVH.

#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// Where a group's pages are now: its first page with clusters (slab and byte offset), and a signature of where every one
// of its pages is. False when it has none.
//
// The cache keeps the slab and the signature with an entry, and the entry is current only while both match. A group
// streamed out and back in can get its first page back and the others elsewhere, and a CLAS's info names the page its
// cluster was built from, which hits decode: matching the first page alone kept CLAS whose info named pages that now
// hold other clusters (hits past the decoded triangles, and junk vertex data).
bool CLodRtGroupPlacement(ClusterLODGroup group, out uint slabDescriptor, out uint slabByteOffset, out uint signature)
{
    slabDescriptor = 0u;
    slabByteOffset = 0u;
    signature = 0x811C9DC5u;
    bool any = false;
    StructuredBuffer<ClusterLODGroupSegment> segments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
    [loop]
    for (uint s = 0u; s < group.segmentCount; ++s) {
        const ClusterLODGroupSegment segment = segments[group.firstSegment + s];
        if (segment.meshletCount == 0u) continue;
        const GroupPageMapEntry page = LoadGroupPageMapEntry(0u, segment.pageIndex);
        if (!any) {
            any = true;
            slabDescriptor = page.slabDescriptorIndex;
            slabByteOffset = page.slabByteOffset;
        }
        signature = (signature ^ page.slabDescriptorIndex) * 16777619u;
        signature = (signature ^ page.slabByteOffset) * 16777619u;
    }
    return any && slabDescriptor != 0u;
}

// The group's placement as the cache compares it: its first page's slab, and the signature of all its pages.
bool CLodRtGroupLocation(ClusterLODGroup group, out uint slabDescriptor, out uint signature)
{
    uint slabByteOffset;
    return CLodRtGroupPlacement(group, slabDescriptor, slabByteOffset, signature);
}

// Whether streaming has the group's pages in, as culling's CLodGroupIsResident.
bool CLodRtGroupResident(uint groupGlobalIndex)
{
    StructuredBuffer<CLodStreamingRuntimeState> runtimeState = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingRuntimeState)];
    if (groupGlobalIndex >= runtimeState[0].activeGroupScanCount) return false;
    ByteAddressBuffer nonResidentBits = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::StreamingNonResidentBits)];
    return (nonResidentBits.Load((groupGlobalIndex >> 5u) * 4u) & (1u << (groupGlobalIndex & 31u))) == 0u;
}

uint CLodRtGroupClusterCount(ClusterLODGroup group)
{
    StructuredBuffer<ClusterLODGroupSegment> segments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
    uint count = 0u;
    [loop]
    for (uint s = 0u; s < group.segmentCount; ++s) {
        count += segments[group.firstSegment + s].meshletCount;
    }
    return count;
}

// What can be built at all: not voxels, float positions, at most 512 clusters, with pages. A property of the group, not
// of the cache: the RT view's culling treats any other group as not resident, so its parent is used instead. Gives the
// group's location as CLodRtGroupLocation does.
bool CLodRtGroupBuildable(ClusterLODGroup group, uint clusters, out uint slab, out uint signature)
{
    slab = 0u;
    signature = 0u;
    if (clusters == 0u || clusters > CLOD_RT_CACHE_MAX_GROUP_CLUSTERS) return false;
    if ((group.flags & (CLOD_GROUP_FLAG_IS_VOXEL | CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL)) != 0u) return false;
    uint offset = 0u;
    if (!CLodRtGroupPlacement(group, slab, offset, signature)) return false;
    const CLodPageHeader header = LoadPageHeader(slab, offset);
    return header.formatAndKind == CLOD_TRIANGLE_PAGE_MAGIC && header.compressedPositionQuantExp == CLOD_POSITION_FORMAT_FLOAT3;
}

#endif // CLOD_RAY_TRACING_GROUPS_HLSLI
