#ifndef PATH_TRACING_WATCH_HLSLI
#define PATH_TRACING_WATCH_HLSLI

// The pixel watch (SARP_RT_PIXEL_WATCH): see the CLOD_RT_WATCH_* layout in clodRayTracingSetupRootConstants.h, and
// CLodRayTracingSystem::LogPixelWatch for what the CPU makes of it. Off when CLOD_RT_PT_WATCH_UAV is CLOD_RT_PT_NO_DESCRIPTOR.
//
// A record is 16 words:
//   0      kind | bounce << 8 | flags << 16
//   1      pixel x | y << 16
//   2-5    instance ID, instance index, cluster ID, primitive
//   6      per kind
//   7-9    a, 10-12 b, 13-15 c: per kind (floats or uints)

#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// Compiled in only when PathTracePass asks (SARP_RT_PIXEL_WATCH): left in, its state costs registers in ray generation, and
// every ray shader runs at the occupancy the largest of them allows.
#ifndef PATH_TRACE_WATCH
#define PATH_TRACE_WATCH 0
#endif

bool PathWatchEnabled()
{
    return PATH_TRACE_WATCH != 0 && CLOD_RT_PT_WATCH_UAV != CLOD_RT_PT_NO_DESCRIPTOR;
}

// Counts one of a kind and returns the byte offset of its record, or 0xFFFFFFFF past the frame's records.
uint PathWatchClaim(uint kind)
{
    RWByteAddressBuffer watch = ResourceDescriptorHeap[CLOD_RT_PT_WATCH_UAV];
    uint index;
    watch.InterlockedAdd(kind * 4u, 1u, index);
    if (index >= CLOD_RT_WATCH_RECORDS) {
        return 0xFFFFFFFFu;
    }
    return (CLOD_RT_WATCH_HEADER_WORDS + ((kind - 1u) * CLOD_RT_WATCH_RECORDS + index) * CLOD_RT_WATCH_RECORD_WORDS) * 4u;
}

void PathWatchRecord(uint kind, uint bounce, uint flags, uint2 pixel, uint4 hitIds, uint misc, uint3 a, uint3 b, uint3 c)
{
    const uint offset = PathWatchClaim(kind);
    if (offset == 0xFFFFFFFFu) {
        return;
    }
    RWByteAddressBuffer watch = ResourceDescriptorHeap[CLOD_RT_PT_WATCH_UAV];
    watch.Store4(offset, uint4(kind | (bounce << 8u) | (flags << 16u), pixel.x | (pixel.y << 16u), hitIds.x, hitIds.y));
    watch.Store4(offset + 16u, uint4(hitIds.z, hitIds.w, misc, a.x));
    watch.Store4(offset + 32u, uint4(a.y, a.z, b.x, b.y));
    watch.Store4(offset + 48u, uint4(b.z, c.x, c.y, c.z));
}

void PathWatchCount(uint word)
{
    RWByteAddressBuffer watch = ResourceDescriptorHeap[CLOD_RT_PT_WATCH_UAV];
    watch.InterlockedAdd(word * 4u, 1u);
}

void PathWatchMaxLuminance(float luminance)
{
    RWByteAddressBuffer watch = ResourceDescriptorHeap[CLOD_RT_PT_WATCH_UAV];
    watch.InterlockedMax(CLOD_RT_WATCH_MAX_LUMINANCE * 4u, asuint(max(luminance, 0.0f))); // Positive floats order as their bits.
}

bool PathWatchFinite3(float3 v)
{
    return all(isfinite(v));
}

#endif // PATH_TRACING_WATCH_HLSLI
