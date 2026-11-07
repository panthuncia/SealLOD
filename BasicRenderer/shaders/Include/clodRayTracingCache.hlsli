#ifndef CLOD_RAY_TRACING_CACHE_HLSLI
#define CLOD_RAY_TRACING_CACHE_HLSLI

// The ray tracing CLAS cache, kept by the GPU within the frame (CLodRayTracingSystem.h, rayTracingSceneBuild.hlsl).
//
// It is driven by the RT view's cluster list, after culling: whatever groups the list references are the scene. Per
// group, 16 bytes of state: [entry + 1 (0: none, UNCACHEABLE), slab descriptor and byte offset of the group's first page
// when its CLAS were built, frame mark]. The frame mark is the last frame the list used the group, with REQUESTED when it
// had no current CLAS then. A group whose first page has moved since it was built is stale: its CLAS describe the right
// triangles, but their hit information names the old pages, so they are not used.
//
// A group the list needs has a CLAS that frame when the build budget and memory allow it. Either it is cached: an entry
// owns a chain of 8 KiB pages its CLAS are packed in, and CLAS ID chunks of 32; or it is transient: moved from the build
// arena into that frame's transient arena, and built again next frame if it is still needed and still doesn't fit. A
// group with neither has no CLAS this frame, and its clusters are null references, which the BLAS build ignores.
//
// Free pages, chunks and entries wait in FIFO queues. A queue holds only free items, and an item freed in one frame can
// be taken again only once the frames that might still trace it are done.

#include "include/clodRayTracingGroups.hlsli"

// The entry a state's first word names. Entries are only named by the group they hold, so the caller checks the group.
bool CLodRtCacheStateEntry(uint entryPlusOne, uint entryCapacity, out uint entry)
{
    entry = entryPlusOne - 1u;
    return entryPlusOne != 0u && entryPlusOne != CLOD_RT_CACHE_UNCACHEABLE && entry < entryCapacity;
}

// A cluster's CLAS ID within an entry: its chunk's, plus its place in the chunk.
uint CLodRtCacheEntryId(ByteAddressBuffer entries, uint entry, uint ordinal)
{
    const uint chunk = entries.Load(entry * CLOD_RT_CACHE_ENTRY_BYTES + CLOD_RT_CACHE_ENTRY_CHUNKS_OFFSET + (ordinal / CLOD_RT_CACHE_CHUNK_IDS) * 4u);
    return chunk * CLOD_RT_CACHE_CHUNK_IDS + (ordinal % CLOD_RT_CACHE_CHUNK_IDS);
}

// The CLAS ID of a listed cluster: the transient one if its group was built for this frame only, else its entry's when
// that is current. INVALID when the group has no CLAS this frame.
uint CLodRtCacheFindSlot(ByteAddressBuffer state, ByteAddressBuffer entries, StructuredBuffer<uint2> transient, uint frame,
    uint groupGlobal, uint slabDescriptor, uint pageOffsetUnits, uint localMeshlet)
{
    uint stateBytes = 0u;
    state.GetDimensions(stateBytes);
    if (groupGlobal >= stateBytes / CLOD_RT_CACHE_STATE_BYTES) return CLOD_RT_CACHE_INVALID;
    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    StructuredBuffer<ClusterLODGroupSegment> segments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
    const ClusterLODGroup group = groups[groupGlobal];
    uint ordinal = 0u;
    bool found = false;
    bool haveFirst = false;
    uint firstSlab = 0u;
    uint firstOffset = 0u;
    [loop]
    for (uint s = 0u; s < group.segmentCount && !found; ++s) {
        const ClusterLODGroupSegment segment = segments[group.firstSegment + s];
        if (segment.meshletCount == 0u) continue;
        const GroupPageMapEntry page = LoadGroupPageMapEntry(0u, segment.pageIndex);
        if (!haveFirst) {
            haveFirst = true;
            firstSlab = page.slabDescriptorIndex;
            firstOffset = page.slabByteOffset;
        }
        if (localMeshlet >= segment.firstMeshletInPage && localMeshlet - segment.firstMeshletInPage < segment.meshletCount
            && page.slabDescriptorIndex == slabDescriptor
            && (page.slabByteOffset >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT) == pageOffsetUnits) {
            ordinal += localMeshlet - segment.firstMeshletInPage;
            found = true;
            break;
        }
        ordinal += segment.meshletCount;
    }
    if (!found) return CLOD_RT_CACHE_INVALID;
    uint entryBytes = 0u;
    entries.GetDimensions(entryBytes);
    const uint entryCapacity = entryBytes / CLOD_RT_CACHE_ENTRY_BYTES;
    const uint2 built = transient[groupGlobal];
    if (built.x == frame) {
        if ((built.y & CLOD_RT_TRANSIENT_DIRECT) != 0u) return (built.y & ~CLOD_RT_TRANSIENT_DIRECT) + ordinal;
        return built.y < entryCapacity ? CLodRtCacheEntryId(entries, built.y, ordinal) : CLOD_RT_CACHE_INVALID;
    }
    const uint4 groupState = state.Load4(groupGlobal * CLOD_RT_CACHE_STATE_BYTES);
    uint entry = 0u;
    if (!CLodRtCacheStateEntry(groupState.x, entryCapacity, entry)) return CLOD_RT_CACHE_INVALID;
    if (entries.Load(entry * CLOD_RT_CACHE_ENTRY_BYTES) != groupGlobal) return CLOD_RT_CACHE_INVALID;
    if (groupState.y != firstSlab || groupState.z != firstOffset) return CLOD_RT_CACHE_INVALID; // Stale.
    return CLodRtCacheEntryId(entries, entry, ordinal);
}

#endif // CLOD_RAY_TRACING_CACHE_HLSLI
