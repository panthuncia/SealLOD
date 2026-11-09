// Builds the ray tracing scene from the RT view's cluster list:
//   CLodRtCache*CS: the CLAS cache, which builds what culling asked for (below)
//   Count -> Allocate -> AllocateFinish -> Place: groups the unordered cluster list by instance, one CLAS address array per BLAS
//   CLodRtWriteInstancesCS: TLAS instances, once the BLAS addresses are known
#include "Common/defines.h"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/materialFlags.hlsli"
#include "include/vertex.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/rhiRtasArgs.hlsli"
#include "include/clodRayTracingScene.hlsli"
#include "include/clodRayTracingCache.hlsli"
#include "include/clodRayTracingAlpha.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"

[numthreads(256, 1, 1)]
void CLodRtClearCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLOD_RT_CLEAR_WORD_COUNT) {
        return;
    }
    RWByteAddressBuffer target = ResourceDescriptorHeap[CLOD_RT_CLEAR_UAV];
    target.Store(id.x * 4u, 0u);
}

// ---- The CLAS cache (clodRayTracingCache.hlsli)
//
// Driven by the RT view's cluster list, after culling: the list is the scene, and every group it names gets a CLAS this
// frame when the build budget and memory allow. Count (below, with the BLAS grouping) marks the groups with a current
// CLAS used, and sets the others' bits in the demand bitset. Then:
//   Collect        the demanded groups, in ascending group index: this frame's requests
//   OmmRetire      under micromap pool pressure, idle groups' micromaps are retired, and their CLAS evicted
//   Evict          while pages, chunks or entries run short, idle, stale and non-resident groups give theirs back
//   Admit          requests within the build budget, groups usable last frame first; then cached groups to build again
//                  with their micromaps, with what is left: a run of build arguments each, at computed offsets
//   Assign         an entry and CLAS ID chunks for each, usable-last-frame groups first, while there are; the rest are
//                  transient, with the frame's own IDs
//   Args           one argument record per cluster
//   (implicit CLAS build into the build arena)
//   Pack           each entry's clusters packed into pages while there are; then what isn't cached is placed by real
//                  size in this frame's transient arena while it has room, and has no CLAS this frame if it hasn't
//   (move into the pages and the transient arena)
//   End            the queue tails, the keep age, the frame's counters, the frame number
// Every one-group pass takes items in a fixed order with prefix sums, so what gets built, cached and placed depends only
// on the list and the cache's state, never on timing. A queue gives out only what was freed PROTECTED_FRAMES ago or
// before, so nothing a frame in flight may trace is reused, and a transient arena is moved into again only
// PROTECTED_FRAMES + 1 frames later. Nothing traces the build arena: the scene pass empties it before it ends.

// Within the batch's declared limits, and every index names one of the cluster's vertices. Anything else is undefined
// for the build; it can hang the GPU. indexByteBase is the cluster's first index byte in the slab.
bool CLodRtClusterArgumentsValid(uint slabDescriptor, uint indexByteBase, uint triangleCount, uint vertexCount)
{
    if (triangleCount > MS_MESHLET_SIZE || vertexCount > MS_MESHLET_SIZE) return false;
    if (triangleCount == 0u) return true;
    if (vertexCount == 0u) return false;
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptor)];
    const uint byteCount = triangleCount * 3u;
    const uint firstWord = indexByteBase & ~3u;
    const uint lastWord = (indexByteBase + byteCount - 1u) & ~3u;
    for (uint word = firstWord; word <= lastWord; word += 4u) {
        const uint packed = slab.Load(word);
        [unroll]
        for (uint b = 0u; b < 4u; ++b) {
            const uint byteAddress = word + b;
            if (byteAddress >= indexByteBase && byteAddress < indexByteBase + byteCount && ((packed >> (8u * b)) & 0xFFu) >= vertexCount) {
                return false;
            }
        }
    }
    return true;
}

uint CLodRtCacheFrame()
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    return ctrl.Load(CLOD_RT_CTRL_FRAME * 4u);
}

void CLodRtCacheCount(uint word, uint value)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    ctrl.InterlockedAdd(word * 4u, value);
}

uint64_t CLodRtCachePoolAddress()
{
    return uint64_t(CLOD_RT_CACHE_POOL_LOW) | (uint64_t(CLOD_RT_CACHE_POOL_HIGH) << 32u);
}

uint CLodRtCacheStateCount()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_CACHE_STATE_UAV];
    uint stateBytes = 0u;
    state.GetDimensions(stateBytes);
    return stateBytes / CLOD_RT_CACHE_STATE_BYTES;
}

// ---- The free queues: 0 pages, 1 chunks, 2 entries.

uint CLodRtQueueCount(uint queue)
{
    return queue == 0u ? CLOD_RT_CACHE_PAGE_COUNT : (queue == 1u ? CLOD_RT_CACHE_CHUNK_COUNT : CLOD_RT_CACHE_ENTRY_COUNT);
}

uint CLodRtQueueBase(uint queue)
{
    return queue == 0u ? 0u : (queue == 1u ? CLOD_RT_CACHE_PAGE_COUNT : CLOD_RT_CACHE_PAGE_COUNT + CLOD_RT_CACHE_CHUNK_COUNT);
}

uint CLodRtQueueItem(uint queue, uint position)
{
    RWByteAddressBuffer queues = ResourceDescriptorHeap[CLOD_RT_CACHE_QUEUES_UAV];
    return queues.Load((CLodRtQueueBase(queue) + position % CLodRtQueueCount(queue)) * 4u);
}

// Reserves count places at a queue's tail and returns the first. Every item exists once, so a queue never overflows.
uint CLodRtQueuePushPlaces(uint queue, uint count)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    uint first = 0u;
    ctrl.InterlockedAdd((CLOD_RT_CTRL_PAGE_TAIL + queue * 2u) * 4u, count, first);
    return first;
}

void CLodRtQueueWrite(uint queue, uint position, uint item)
{
    RWByteAddressBuffer queues = ResourceDescriptorHeap[CLOD_RT_CACHE_QUEUES_UAV];
    queues.Store((CLodRtQueueBase(queue) + position % CLodRtQueueCount(queue)) * 4u, item);
}

// How many items a queue can give out now: those freed PROTECTED_FRAMES frames ago or before.
uint CLodRtQueueAvailable(uint queue, uint frame)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    const uint safeTail = ctrl.Load((CLOD_RT_CTRL_TAIL_HISTORY + ((frame - CLOD_RT_CACHE_PROTECTED_FRAMES) % CLOD_RT_CACHE_HISTORY) * 3u + queue) * 4u);
    const uint head = ctrl.Load((CLOD_RT_CTRL_PAGE_HEAD + queue * 2u) * 4u);
    return safeTail - head <= CLodRtQueueCount(queue) ? safeTail - head : 0u;
}

// ---- Entries

// Gives back everything a cached group holds, and forgets the group's CLAS. The caller has checked that the group's
// state names the entry.
void CLodRtCacheFreeEntry(uint entry, uint groupGlobal)
{
    RWByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_CACHE_ENTRIES_UAV];
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_CACHE_STATE_UAV];
    RWStructuredBuffer<uint> pageNext = ResourceDescriptorHeap[CLOD_RT_CACHE_PAGE_NEXT_UAV];
    const uint base = entry * CLOD_RT_CACHE_ENTRY_BYTES;
    const uint4 head = entries.Load4(base); // group, pages, first page, micromap slot + 1
    const uint chunks = entries.Load(base + 16u);
    const uint units = entries.Load(base + CLOD_RT_CACHE_ENTRY_UNITS_OFFSET);
    state.Store(groupGlobal * CLOD_RT_CACHE_STATE_BYTES, 0u);
    const uint chunkPlace = CLodRtQueuePushPlaces(1u, chunks);
    [loop]
    for (uint c = 0u; c < chunks; ++c) {
        CLodRtQueueWrite(1u, chunkPlace + c, entries.Load(base + CLOD_RT_CACHE_ENTRY_CHUNKS_OFFSET + c * 4u));
    }
    // Its own pages, then its tail's page, which other tails may share: a page goes back when its last user does.
    RWStructuredBuffer<uint> pageRefs = ResourceDescriptorHeap[CLOD_RT_CACHE_PAGE_REFS_UAV];
    uint page = head.z;
    [loop]
    for (uint p = 0u; p < head.y; ++p) {
        const uint next = pageNext[page];
        uint previous = 0u;
        InterlockedAdd(pageRefs[page], 0xFFFFFFFFu, previous);
        if (previous == 1u) {
            CLodRtQueueWrite(0u, CLodRtQueuePushPlaces(0u, 1u), page);
            CLodRtCacheCount(CLOD_RT_CTRL_CACHED_PAGES, 0xFFFFFFFFu);
        }
        page = next;
    }
    CLodRtQueueWrite(2u, CLodRtQueuePushPlaces(2u, 1u), entry);
    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const uint clusters = CLodRtGroupClusterCount(groups[groupGlobal]);
    CLodRtCacheCount(CLOD_RT_CTRL_CACHED_GROUPS, 0xFFFFFFFFu);
    CLodRtCacheCount(CLOD_RT_CTRL_CACHED_CLUSTERS, 0u - clusters);
    CLodRtCacheCount(CLOD_RT_CTRL_CACHED_UNITS, 0u - units);
    CLodRtCacheCount(CLOD_RT_CTRL_FREED_TOTAL, clusters);
}

// The group's entry, if its state names one that holds it.
bool CLodRtCacheCurrentEntry(uint groupGlobal, uint entryPlusOne, out uint entry)
{
    RWByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_CACHE_ENTRIES_UAV];
    return CLodRtCacheStateEntry(entryPlusOne, CLOD_RT_CACHE_ENTRY_COUNT, entry)
        && entries.Load(entry * CLOD_RT_CACHE_ENTRY_BYTES) == groupGlobal;
}

uint CLodRtOmmHash(uint groupGlobal)
{
    uint h = groupGlobal * 0x9E3779B1u;
    h ^= h >> 15u;
    return h & (CLOD_RT_OMM_TABLE_ENTRIES - 1u);
}

// The group's micromap table slot, or INVALID when the CPU has not loaded (or ruled out) any.
uint CLodRtOmmLookup(uint groupGlobal, out uint4 entry)
{
    entry = uint4(0u, 0u, 0u, 0u);
    RWStructuredBuffer<uint4> table = ResourceDescriptorHeap[CLOD_RT_CACHE_OMM_TABLE_UAV];
    uint slot = CLodRtOmmHash(groupGlobal);
    [loop]
    for (uint probe = 0u; probe < CLOD_RT_OMM_TABLE_PROBES; ++probe) {
        entry = table[slot];
        if (entry.x == groupGlobal + 1u) return slot;
        if (entry.x == 0u) return CLOD_RT_CACHE_INVALID;
        slot = (slot + 1u) & (CLOD_RT_OMM_TABLE_ENTRIES - 1u); // Past tombstones too.
    }
    return CLOD_RT_CACHE_INVALID;
}

// A one-group exclusive prefix sum over CLOD_RT_CACHE_GROUP_THREADS threads, for the assign and pack passes.
groupshared uint gsCacheWaveSums[64];
groupshared uint gsCacheScanTotal;

uint CLodRtCacheGroupScan(uint value, uint tid, out uint total)
{
    const uint lanes = WaveGetLaneCount();
    const uint waves = CLOD_RT_CACHE_GROUP_THREADS / lanes;
    const uint prefix = WavePrefixSum(value);
    if (WaveGetLaneIndex() == lanes - 1u) gsCacheWaveSums[tid / lanes] = prefix + value;
    GroupMemoryBarrierWithGroupSync();
    if (tid < waves) {
        const uint sum = gsCacheWaveSums[tid];
        const uint before = WavePrefixSum(sum);
        gsCacheWaveSums[tid] = before;
        if (tid == waves - 1u) gsCacheScanTotal = before + sum;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint result = prefix + gsCacheWaveSums[tid / lanes];
    total = gsCacheScanTotal;
    GroupMemoryBarrierWithGroupSync();
    return result;
}

// Once: every queue full, and every queue's history at its tail, so all of it can be given out from the first frame.
// Frame marks start above zero, which means never used.
[numthreads(256, 1, 1)]
void CLodRtCacheInitCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    if (id.x == 0u) {
        ctrl.Store(CLOD_RT_CTRL_FRAME * 4u, CLOD_RT_CACHE_HISTORY);
        ctrl.Store(CLOD_RT_CTRL_KEEP_AGE * 4u, CLOD_RT_CACHE_MAX_KEEP_AGE);
        [unroll]
        for (uint q = 0u; q < 3u; ++q) {
            ctrl.Store((CLOD_RT_CTRL_PAGE_TAIL + q * 2u) * 4u, CLodRtQueueCount(q));
            [unroll]
            for (uint h = 0u; h < CLOD_RT_CACHE_HISTORY; ++h) ctrl.Store((CLOD_RT_CTRL_TAIL_HISTORY + h * 3u + q) * 4u, CLodRtQueueCount(q));
        }
    }
    RWByteAddressBuffer queues = ResourceDescriptorHeap[CLOD_RT_CACHE_QUEUES_UAV];
    [unroll]
    for (uint q = 0u; q < 3u; ++q) {
        if (id.x < CLodRtQueueCount(q)) queues.Store((CLodRtQueueBase(q) + id.x) * 4u, id.x);
    }
}

// Under micromap pool pressure, a window of the table per frame: an entry whose group nothing has used for
// PROTECTED_FRAMES is retired, and the group's CLAS evicted if they were built with it. The CPU frees what it held once
// this frame's readback is back, by when nothing can be tracing those CLAS.
[numthreads(64, 1, 1)]
void CLodRtCacheOmmRetireCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLOD_RT_CACHE_OMM_RETIRE_WINDOW) return;
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    RWStructuredBuffer<uint4> table = ResourceDescriptorHeap[CLOD_RT_CACHE_OMM_TABLE_UAV];
    const uint slot = (ctrl.Load(CLOD_RT_CTRL_OMM_RETIRE_CURSOR * 4u) + id.x) & (CLOD_RT_OMM_TABLE_ENTRIES - 1u);
    const uint4 entry = table[slot];
    if (entry.x == 0u || entry.x == CLOD_RT_OMM_TOMBSTONE) return;
    const uint groupGlobal = entry.x - 1u;
    if (groupGlobal < CLodRtCacheStateCount()) {
        RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_CACHE_STATE_UAV];
        const uint4 groupState = state.Load4(groupGlobal * CLOD_RT_CACHE_STATE_BYTES);
        const uint mark = groupState.w & ~CLOD_RT_CACHE_MARK_FLAGS;
        const uint frame = CLodRtCacheFrame();
        if (mark <= frame && frame - mark <= CLOD_RT_CACHE_PROTECTED_FRAMES) return;
        uint cached = 0u;
        if (CLodRtCacheCurrentEntry(groupGlobal, groupState.x, cached)) {
            RWByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_CACHE_ENTRIES_UAV];
            if (entries.Load(cached * CLOD_RT_CACHE_ENTRY_BYTES + 12u) == slot + 1u) {
                CLodRtCacheFreeEntry(cached, groupGlobal);
                CLodRtCacheCount(CLOD_RT_CTRL_EVICTED, 1u);
            }
        }
    }
    table[slot] = uint4(CLOD_RT_OMM_TOMBSTONE, entry.y, entry.z, entry.w);
    uint released = 0u;
    ctrl.InterlockedAdd(CLOD_RT_CTRL_OMM_RELEASED * 4u, 1u, released);
    RWStructuredBuffer<uint4> releases = ResourceDescriptorHeap[CLOD_RT_CACHE_OMM_RELEASE_UAV];
    releases[released] = uint4(slot, entry.x, entry.z, entry.w);
}

// Per entry, while pages, chunks or entries that can be given out run short: a cached group not used for the keep age
// (four times that for a parentless one, which has no coarser stand-in), stale (its pages moved) or no longer resident
// gives back what it holds. Never one this frame's culling used: the frames before it that may still trace its CLAS
// are safe anyway, because the queues give what is freed out again only after them.
[numthreads(64, 1, 1)]
void CLodRtCacheEvictCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLOD_RT_CACHE_ENTRY_COUNT) return;
    const uint frame = CLodRtCacheFrame();
    const bool scarce = CLodRtQueueAvailable(0u, frame) < CLOD_RT_CACHE_WATERMARK_PAGES
        || CLodRtQueueAvailable(1u, frame) < CLOD_RT_CACHE_BUDGET / 8u
        || CLodRtQueueAvailable(2u, frame) < CLOD_RT_CACHE_BUDGET / 8u;
    if (!scarce) return;
    RWByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_CACHE_ENTRIES_UAV];
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_CACHE_STATE_UAV];
    const uint groupGlobal = entries.Load(id.x * CLOD_RT_CACHE_ENTRY_BYTES);
    if (groupGlobal >= CLodRtCacheStateCount()) return;
    const uint4 groupState = state.Load4(groupGlobal * CLOD_RT_CACHE_STATE_BYTES);
    if (groupState.x != id.x + 1u) return; // Free, or not yet placed.
    const uint mark = groupState.w & ~CLOD_RT_CACHE_MARK_FLAGS;
    if (mark == frame) return;
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const uint keepAge = ctrl.Load(CLOD_RT_CTRL_KEEP_AGE * 4u) * (groups[groupGlobal].parentGroupId < 0 ? 4u : 1u);
    bool evict = mark > frame || frame - mark > keepAge;
    if (!evict) {
        uint slab = 0u;
        uint signature = 0u;
        const bool current = CLodRtGroupLocation(groups[groupGlobal], slab, signature) && slab == groupState.y && signature == groupState.z;
        evict = !current || !CLodRtGroupResident(groupGlobal);
        if (evict && !current) CLodRtCacheCount(CLOD_RT_CTRL_STALE, 1u);
    }
    if (!evict) return;
    CLodRtCacheFreeEntry(id.x, groupGlobal);
    CLodRtCacheCount(CLOD_RT_CTRL_EVICTED, 1u);
    CLodRtCacheCount(CLOD_RT_CTRL_EVICTED_TOTAL, 1u);
}

// ---- Collect: the demand bitset into this frame's requests

groupshared uint gsCollectCount;

// One group: each thread takes a summary word, whose bits name the bitset words with demanded groups; a prefix sum over
// the groups they hold places them in ascending group index. Bits and identities are cleared as they are read, so the
// bitset is empty for the next frame's Count. The work follows the words that were touched, apart from the summary.
[numthreads(CLOD_RT_CACHE_GROUP_THREADS, 1, 1)]
void CLodRtCacheCollectCS(uint tid : SV_GroupIndex)
{
    RWByteAddressBuffer demand = ResourceDescriptorHeap[CLOD_RT_CACHE_DEMAND_UAV];
    RWStructuredBuffer<uint2> requests = ResourceDescriptorHeap[CLOD_RT_CACHE_REQUESTS_UAV];
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    if (tid == 0u) gsCollectCount = 0u;
    GroupMemoryBarrierWithGroupSync();
    [loop]
    for (uint base = 0u; base < CLOD_RT_DEMAND_SUMMARY_WORDS; base += CLOD_RT_CACHE_GROUP_THREADS) {
        const uint summaryIndex = base + tid;
        const uint summary = summaryIndex < CLOD_RT_DEMAND_SUMMARY_WORDS ? demand.Load(summaryIndex * 4u) : 0u;
        uint groups = 0u;
        uint bits = summary;
        [loop]
        while (bits != 0u) {
            const uint b = firstbitlow(bits);
            bits &= bits - 1u;
            groups += countbits(demand.Load(CLOD_RT_DEMAND_BITS_OFFSET + (summaryIndex * 32u + b) * 4u));
        }
        uint total = 0u;
        uint place = gsCollectCount + CLodRtCacheGroupScan(groups, tid, total);
        bits = summary;
        [loop]
        while (bits != 0u) {
            const uint b = firstbitlow(bits);
            bits &= bits - 1u;
            const uint wordIndex = summaryIndex * 32u + b;
            uint word = demand.Load(CLOD_RT_DEMAND_BITS_OFFSET + wordIndex * 4u);
            demand.Store(CLOD_RT_DEMAND_BITS_OFFSET + wordIndex * 4u, 0u);
            [loop]
            while (word != 0u) {
                const uint bit = firstbitlow(word);
                word &= word - 1u;
                const uint groupGlobal = wordIndex * 32u + bit;
                const uint identity = demand.Load(CLOD_RT_DEMAND_IDENTITY_OFFSET + groupGlobal * 4u);
                demand.Store(CLOD_RT_DEMAND_IDENTITY_OFFSET + groupGlobal * 4u, 0u);
                if (place < CLOD_RT_CACHE_REQUEST_CAPACITY) requests[place] = uint2(groupGlobal, identity);
                ++place;
            }
        }
        if (summary != 0u) demand.Store(summaryIndex * 4u, 0u);
        GroupMemoryBarrierWithGroupSync(); // Everyone has read gsCollectCount.
        if (tid == 0u) gsCollectCount += total;
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0u) {
        const uint kept = min(gsCollectCount, CLOD_RT_CACHE_REQUEST_CAPACITY);
        ctrl.Store(CLOD_RT_CTRL_REQUESTS * 4u, kept);
        ctrl.InterlockedAdd(CLOD_RT_CTRL_NOT_BUILT * 4u, gsCollectCount - kept); // Past the request list: not built.
    }
}

// ---- Admit: requests within the build budget

groupshared uint gsAdmitClusters;
groupshared uint gsAdmitRanges;
groupshared uint gsAdmitFull;
groupshared uint gsAdmitClustersTaken;
groupshared uint gsAdmitRangesTaken;

// The micromaps the asking draw's material was baked with, once the CPU has loaded them; until then none, and the
// group is listed for the CPU.
uint CLodRtCacheRequestMicromaps(uint groupGlobal, uint identity)
{
    if (CLOD_RT_CACHE_OMM_ACTIVE == 0u || identity == 0u) return 0u;
    uint4 omm;
    const uint ommSlot = CLodRtOmmLookup(groupGlobal, omm);
    if (ommSlot != CLOD_RT_CACHE_INVALID) return omm.z != CLOD_RT_OMM_NONE && omm.y == identity ? ommSlot + 1u : 0u;
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    uint wanted = 0u;
    ctrl.InterlockedAdd(CLOD_RT_CTRL_OMM_WANTED * 4u, 1u, wanted);
    if (wanted < CLOD_RT_CACHE_BUDGET) {
        RWStructuredBuffer<uint2> wantedList = ResourceDescriptorHeap[CLOD_RT_CACHE_OMM_WANTED_UAV];
        wantedList[wanted] = uint2(groupGlobal, identity);
    }
    return 0u;
}

// One group, three passes over a fixed order: requests for groups that were usable last frame (transient then, or cached
// and now stale), so nothing on screen loses its CLAS for a newcomer; the other requests; then cached groups whose
// micromaps the CPU has just loaded, built again with them (Pack replaces the old CLAS only once all the new ones have
// a place). Each takes a run of build arguments while the budget lasts; once one doesn't fit, the rest don't either.
// A request that doesn't fit has no CLAS this frame, and asks again next frame.
[numthreads(CLOD_RT_CACHE_GROUP_THREADS, 1, 1)]
void CLodRtCacheAdmitCS(uint tid : SV_GroupIndex)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    RWByteAddressBuffer ranges = ResourceDescriptorHeap[CLOD_RT_CACHE_RANGES_UAV];
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_CACHE_STATE_UAV];
    RWStructuredBuffer<uint> pending = ResourceDescriptorHeap[CLOD_RT_CACHE_PENDING_UAV];
    RWStructuredBuffer<uint2> requests = ResourceDescriptorHeap[CLOD_RT_CACHE_REQUESTS_UAV];
    RWStructuredBuffer<uint2> transient = ResourceDescriptorHeap[CLOD_RT_CACHE_TRANSIENT_UAV];
    ByteAddressBuffer rebuilds = ResourceDescriptorHeap[CLOD_RT_CACHE_OMM_REBUILD_SRV];
    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const uint frame = CLodRtCacheFrame();
    const uint requestCount = ctrl.Load(CLOD_RT_CTRL_REQUESTS * 4u);
    const uint rebuildCount = CLOD_RT_CACHE_OMM_ACTIVE != 0u ? min(rebuilds.Load(0u), CLOD_RT_CACHE_OMM_REBUILD_CAPACITY) : 0u;
    if (tid == 0u) {
        gsAdmitClusters = 0u;
        gsAdmitRanges = 0u;
        gsAdmitFull = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    [loop]
    for (uint pass = 0u; pass < 3u; ++pass) {
        const uint count = pass < 2u ? requestCount : rebuildCount;
        [loop]
        for (uint base = 0u; base < count; base += CLOD_RT_CACHE_GROUP_THREADS) {
            const bool full = gsAdmitFull != 0u; // Before any thread of this chunk can set it.
            const uint r = base + tid;
            bool valid = false;
            bool owned = false; // A request this pass takes, whether or not it can be built.
            uint groupGlobal = 0u;
            uint identity = 0u;
            uint replacedPlusOne = 0u;
            uint4 groupState = uint4(0u, 0u, 0u, 0u);
            if (r < count) {
                if (pass < 2u) {
                    const uint2 request = requests[r];
                    groupGlobal = request.x;
                    identity = request.y;
                    if (groupGlobal < CLodRtCacheStateCount()) {
                        groupState = state.Load4(groupGlobal * CLOD_RT_CACHE_STATE_BYTES);
                        uint stale = 0u;
                        const bool kept = transient[groupGlobal].x == frame - 1u || CLodRtCacheCurrentEntry(groupGlobal, groupState.x, stale);
                        owned = kept == (pass == 0u);
                        valid = owned;
                    }
                }
                else {
                    groupGlobal = rebuilds.Load(4u + r * 4u);
                    if (groupGlobal < CLodRtCacheStateCount()) {
                        groupState = state.Load4(groupGlobal * CLOD_RT_CACHE_STATE_BYTES);
                        uint cached = 0u;
                        // Cached, current, used this frame (so resident) and not rebuilt this frame anyway.
                        valid = CLodRtCacheCurrentEntry(groupGlobal, groupState.x, cached) && groupState.w == frame;
                        replacedPlusOne = cached + 1u;
                    }
                }
            }
            uint clusters = 0u;
            uint slab = 0u;
            uint signature = 0u;
            if (valid) {
                const ClusterLODGroup group = groups[groupGlobal];
                clusters = CLodRtGroupClusterCount(group);
                valid = CLodRtGroupBuildable(group, clusters, slab, signature);
                if (valid && pass == 2u) valid = slab == groupState.y && signature == groupState.z;
                if (!valid) clusters = 0u;
            }
            uint totalClusters = 0u;
            const uint clustersBefore = CLodRtCacheGroupScan(clusters, tid, totalClusters);
            uint totalRanges = 0u;
            const uint rangesBefore = CLodRtCacheGroupScan(valid ? 1u : 0u, tid, totalRanges);
            if (tid == 0u) {
                gsAdmitClustersTaken = 0u;
                gsAdmitRangesTaken = 0u;
            }
            GroupMemoryBarrierWithGroupSync();
            const bool fits = valid && !full && gsAdmitClusters + clustersBefore + clusters <= CLOD_RT_CACHE_BUDGET;
            if (fits) {
                uint ommSlotPlusOne = 0u;
                if (pass < 2u) {
                    ommSlotPlusOne = CLodRtCacheRequestMicromaps(groupGlobal, identity);
                }
                else {
                    uint4 omm;
                    const uint ommSlot = CLodRtOmmLookup(groupGlobal, omm);
                    ommSlotPlusOne = ommSlot != CLOD_RT_CACHE_INVALID && omm.z != CLOD_RT_OMM_NONE ? ommSlot + 1u : 0u;
                }
                const uint range = gsAdmitRanges + rangesBefore;
                const uint first = gsAdmitClusters + clustersBefore;
                const uint kind = pass < 2u ? CLOD_RT_RANGE_REQUEST : CLOD_RT_RANGE_REBUILD;
                ranges.Store4(range * CLOD_RT_CACHE_RANGE_BYTES, uint4(groupGlobal, kind, first, clusters));
                ranges.Store4(range * CLOD_RT_CACHE_RANGE_BYTES + 16u, uint4(CLOD_RT_CACHE_INVALID, slab, signature, ommSlotPlusOne));
                ranges.Store4(range * CLOD_RT_CACHE_RANGE_BYTES + 32u, uint4(replacedPlusOne, pass == 0u ? 1u : 0u, 0u, 0u));
                [loop]
                for (uint i = 0u; i < clusters; ++i) pending[first + i] = range;
                InterlockedMax(gsAdmitClustersTaken, clustersBefore + clusters);
                InterlockedMax(gsAdmitRangesTaken, rangesBefore + 1u);
                CLodRtCacheCount(pass < 2u ? CLOD_RT_CTRL_ADMITTED : CLOD_RT_CTRL_REBUILT, 1u);
            }
            else if (valid) {
                if (pass < 2u) CLodRtCacheCount(CLOD_RT_CTRL_NOT_BUILT, 1u);
                gsAdmitFull = 1u;
            }
            else if (owned) {
                CLodRtCacheCount(CLOD_RT_CTRL_NOT_BUILT, 1u); // Can't be built at all (culling keeps those out): no CLAS.
            }
            GroupMemoryBarrierWithGroupSync();
            if (tid == 0u) {
                gsAdmitClusters += gsAdmitClustersTaken;
                gsAdmitRanges += gsAdmitRangesTaken;
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    if (tid == 0u) {
        ctrl.Store(CLOD_RT_CTRL_RANGE_COUNT * 4u, gsAdmitRanges);
        ctrl.Store(CLOD_RT_CTRL_BUILD_COUNT * 4u, gsAdmitClusters);
    }
}

groupshared uint gsAssignEntries;
groupshared uint gsAssignChunks;
groupshared uint gsAssignEntryHead;
groupshared uint gsAssignChunkHead;
groupshared uint gsAssignEntriesTaken;
groupshared uint gsAssignChunksTaken;
groupshared uint gsAssignFull;

// One group. Groups usable last frame first, then the rest, in range order: each range takes an entry and its CLAS ID
// chunks while the queues can give them; once one can't, the rest can't either. A request that gets none is transient;
// a rebuild that gets none is discarded. A request
// for a group whose entry is stale gives that entry back: it is no use to anything this frame.
[numthreads(CLOD_RT_CACHE_GROUP_THREADS, 1, 1)]
void CLodRtCacheAssignCS(uint tid : SV_GroupIndex)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    RWByteAddressBuffer ranges = ResourceDescriptorHeap[CLOD_RT_CACHE_RANGES_UAV];
    RWByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_CACHE_ENTRIES_UAV];
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_CACHE_STATE_UAV];
    const uint frame = CLodRtCacheFrame();
    const uint rangeCount = ctrl.Load(CLOD_RT_CTRL_RANGE_COUNT * 4u);
    if (tid == 0u) {
        gsAssignEntries = CLodRtQueueAvailable(2u, frame);
        gsAssignChunks = CLodRtQueueAvailable(1u, frame);
        gsAssignEntryHead = ctrl.Load(CLOD_RT_CTRL_ENTRY_HEAD * 4u);
        gsAssignChunkHead = ctrl.Load(CLOD_RT_CTRL_CHUNK_HEAD * 4u);
        gsAssignFull = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    [loop]
    for (uint pass = 0u; pass < 2u; ++pass)
    [loop]
    for (uint base = 0u; base < rangeCount; base += CLOD_RT_CACHE_GROUP_THREADS) {
        const bool full = gsAssignFull != 0u; // Before any thread of this chunk can set it.
        const uint r = base + tid;
        const bool valid = r < rangeCount && (ranges.Load(r * CLOD_RT_CACHE_RANGE_BYTES + 36u) != 0u) == (pass == 0u);
        const uint4 range = valid ? ranges.Load4(r * CLOD_RT_CACHE_RANGE_BYTES) : uint4(0u, 0u, 0u, 0u);
        const uint chunks = valid ? (range.w + CLOD_RT_CACHE_CHUNK_IDS - 1u) / CLOD_RT_CACHE_CHUNK_IDS : 0u;
        uint totalEntries = 0u;
        const uint entriesBefore = CLodRtCacheGroupScan(valid ? 1u : 0u, tid, totalEntries);
        uint totalChunks = 0u;
        const uint chunksBefore = CLodRtCacheGroupScan(chunks, tid, totalChunks);
        if (tid == 0u) {
            gsAssignEntriesTaken = 0u;
            gsAssignChunksTaken = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        const bool fits = valid && !full && entriesBefore + 1u <= gsAssignEntries && chunksBefore + chunks <= gsAssignChunks;
        if (valid) {
            // A stale entry: the group was asked for because it isn't usable.
            if (range.y == CLOD_RT_RANGE_REQUEST) {
                uint stale = 0u;
                if (CLodRtCacheCurrentEntry(range.x, state.Load(range.x * CLOD_RT_CACHE_STATE_BYTES), stale)) CLodRtCacheFreeEntry(stale, range.x);
            }
            if (fits) {
                const uint entry = CLodRtQueueItem(2u, gsAssignEntryHead + entriesBefore);
                const uint entryBase = entry * CLOD_RT_CACHE_ENTRY_BYTES;
                entries.Store4(entryBase, uint4(range.x, 0u, 0u, ranges.Load(r * CLOD_RT_CACHE_RANGE_BYTES + 28u)));
                entries.Store(entryBase + 16u, chunks);
                [loop]
                for (uint c = 0u; c < chunks; ++c) {
                    entries.Store(entryBase + CLOD_RT_CACHE_ENTRY_CHUNKS_OFFSET + c * 4u, CLodRtQueueItem(1u, gsAssignChunkHead + chunksBefore + c));
                }
                ranges.Store(r * CLOD_RT_CACHE_RANGE_BYTES + 4u, range.y == CLOD_RT_RANGE_REQUEST ? CLOD_RT_RANGE_PERSISTENT : CLOD_RT_RANGE_PERSISTENT_REBUILD);
                ranges.Store(r * CLOD_RT_CACHE_RANGE_BYTES + 16u, entry);
                InterlockedMax(gsAssignEntriesTaken, entriesBefore + 1u);
                InterlockedMax(gsAssignChunksTaken, chunksBefore + chunks);
            }
            else {
                ranges.Store(r * CLOD_RT_CACHE_RANGE_BYTES + 4u, range.y == CLOD_RT_RANGE_REQUEST ? CLOD_RT_RANGE_TRANSIENT : CLOD_RT_RANGE_DISCARD);
                CLodRtCacheCount(CLOD_RT_CTRL_REFUSED, 1u);
                gsAssignFull = 1u;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0u) {
            gsAssignEntries -= gsAssignEntriesTaken;
            gsAssignChunks -= gsAssignChunksTaken;
            gsAssignEntryHead += gsAssignEntriesTaken;
            gsAssignChunkHead += gsAssignChunksTaken;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0u) {
        ctrl.Store(CLOD_RT_CTRL_ENTRY_HEAD * 4u, gsAssignEntryHead);
        ctrl.Store(CLOD_RT_CTRL_CHUNK_HEAD * 4u, gsAssignChunkHead);
        // What the build reads as its count: a rebuild over the budget left the build count past it.
        ctrl.Store(CLOD_RT_CTRL_BUILD_ARG_COUNT * 4u, min(ctrl.Load(CLOD_RT_CTRL_BUILD_COUNT * 4u), CLOD_RT_CACHE_BUDGET));
    }
}

// The cluster a built cluster index is, within its group: its page and meshlet.
bool CLodRtCacheClusterSource(uint index, out uint4 range, out uint ordinal, out uint slabDescriptor, out uint pageByteOffset, out uint localMeshlet)
{
    RWStructuredBuffer<uint> pending = ResourceDescriptorHeap[CLOD_RT_CACHE_PENDING_UAV];
    RWByteAddressBuffer ranges = ResourceDescriptorHeap[CLOD_RT_CACHE_RANGES_UAV];
    slabDescriptor = 0u;
    pageByteOffset = 0u;
    localMeshlet = 0u;
    ordinal = 0u;
    range = uint4(0u, CLOD_RT_RANGE_DISCARD, index, 1u);
    if (pending[index] == CLOD_RT_CACHE_INVALID) return false; // Builds empty, with the arena's ID.
    range = ranges.Load4(pending[index] * CLOD_RT_CACHE_RANGE_BYTES);
    ordinal = index - range.z;
    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    StructuredBuffer<ClusterLODGroupSegment> segments = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
    const ClusterLODGroup group = groups[range.x];
    uint before = 0u;
    [loop]
    for (uint s = 0u; s < group.segmentCount; ++s) {
        const ClusterLODGroupSegment segment = segments[group.firstSegment + s];
        if (segment.meshletCount == 0u) continue;
        if (ordinal < before + segment.meshletCount) {
            const GroupPageMapEntry page = LoadGroupPageMapEntry(0u, segment.pageIndex);
            slabDescriptor = page.slabDescriptorIndex;
            pageByteOffset = page.slabByteOffset;
            localMeshlet = segment.firstMeshletInPage + (ordinal - before);
            return slabDescriptor != 0u;
        }
        before += segment.meshletCount;
    }
    return false;
}

// A built cluster's CLAS ID: its entry's, or for what stays in the arena only (transient, discarded), the arena's.
uint CLodRtCacheBuiltId(uint index, uint kind, uint entry, uint ordinal)
{
    if (kind == CLOD_RT_RANGE_PERSISTENT || kind == CLOD_RT_RANGE_PERSISTENT_REBUILD || kind == CLOD_RT_RANGE_FALLBACK) {
        RWByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_CACHE_ENTRIES_UAV];
        const uint chunk = entries.Load(entry * CLOD_RT_CACHE_ENTRY_BYTES + CLOD_RT_CACHE_ENTRY_CHUNKS_OFFSET + (ordinal / CLOD_RT_CACHE_CHUNK_IDS) * 4u);
        return chunk * CLOD_RT_CACHE_CHUNK_IDS + (ordinal % CLOD_RT_CACHE_CHUNK_IDS);
    }
    return CLOD_RT_CACHE_TRANSIENT_ID_BASE + index;
}

[numthreads(64, 1, 1)]
void CLodRtCacheArgsCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    if (id.x >= ctrl.Load(CLOD_RT_CTRL_BUILD_ARG_COUNT * 4u)) return;
    uint4 range;
    uint ordinal, slabDescriptor, pageByteOffset, localMeshlet;
    const bool found = CLodRtCacheClusterSource(id.x, range, ordinal, slabDescriptor, pageByteOffset, localMeshlet);
    RWByteAddressBuffer ranges = ResourceDescriptorHeap[CLOD_RT_CACHE_RANGES_UAV];
    RWStructuredBuffer<uint> pendingRanges = ResourceDescriptorHeap[CLOD_RT_CACHE_PENDING_UAV];
    const uint pendingRange = pendingRanges[id.x];
    const uint4 extra = pendingRange == CLOD_RT_CACHE_INVALID ? uint4(CLOD_RT_CACHE_INVALID, 0u, 0u, 0u)
        : ranges.Load4(pendingRange * CLOD_RT_CACHE_RANGE_BYTES + 16u); // entry, slab, page signature, micromap slot + 1
    const uint slot = CLodRtCacheBuiltId(id.x, range.y, extra.x, ordinal);

    // An empty cluster is a valid CLAS, which is what anything unexpected builds.
    uint triangleCount = 0u;
    uint vertexCount = 0u;
    uint64_t vertexBuffer = 0ull;
    uint64_t indexBuffer = 0ull;
    StructuredBuffer<uint2> slabAddresses = ResourceDescriptorHeap[CLOD_RT_CACHE_SLAB_ADDRESSES_SRV];
    const uint2 slabAddressWords = found && slabDescriptor < CLOD_RT_CACHE_SLAB_ADDRESS_COUNT ? slabAddresses[slabDescriptor] : uint2(0u, 0u);
    const uint64_t slabAddress = uint64_t(slabAddressWords.x) | (uint64_t(slabAddressWords.y) << 32u);
    if (slabAddress != 0ull && range.y != CLOD_RT_RANGE_DISCARD) {
        const CLodPageHeader header = LoadPageHeader(slabDescriptor, pageByteOffset);
        if (header.formatAndKind == CLOD_TRIANGLE_PAGE_MAGIC
            && header.compressedPositionQuantExp == CLOD_POSITION_FORMAT_FLOAT3
            && localMeshlet < header.meshletCount) {
            const CLodMeshletDescriptor descriptor = LoadMeshletDescriptor(slabDescriptor, pageByteOffset, header.descriptorOffset, localMeshlet);
            triangleCount = descriptor.triangleCountAndRefinedGroup & 0xFFFFu;
            vertexCount = (descriptor.bitsAndVertexCount >> 24u) & 0xFFu;
            const uint64_t pageAddress = slabAddress + pageByteOffset;
            vertexBuffer = pageAddress + header.positionBitstreamOffset + descriptor.positionBitOffset;
            indexBuffer = pageAddress + header.triangleStreamOffset + descriptor.triangleByteOffset;
            const uint indexByteBase = pageByteOffset + header.triangleStreamOffset + descriptor.triangleByteOffset;
            if (!CLodRtClusterArgumentsValid(slabDescriptor, indexByteBase, triangleCount, vertexCount)) {
                triangleCount = 0u;
                vertexCount = 0u;
                RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_CACHE_COUNTERS_UAV];
                counters.InterlockedAdd(CLOD_RT_COUNTER_REJECTED_CLAS_ARGS * 4u, 1u);
            }
        }
    }

    // Opacity is decided per instance with FORCE_OPAQUE: a page can be shared by meshes with different materials.
    // Micromaps belong to one material, so instances of any other disable them (CLodRtWriteInstancesCS).
    RtasClusterBuildTriangleInfo build = RtasMakeClusterBuildTriangleInfo(
        slot, triangleCount, vertexCount, RTAS_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT,
        CLOD_POSITION_FORMAT_FLOAT3_STRIDE_BYTES, 1u, vertexBuffer, indexBuffer);
    uint ommKey = 0u;
    if (extra.w != 0u && triangleCount != 0u) {
        RWStructuredBuffer<uint4> ommTable = ResourceDescriptorHeap[CLOD_RT_CACHE_OMM_TABLE_UAV];
        const uint4 omm = ommTable[extra.w - 1u];
        const uint64_t indexPool = uint64_t(CLOD_RT_CACHE_OMM_INDEX_POOL_LOW) | (uint64_t(CLOD_RT_CACHE_OMM_INDEX_POOL_HIGH) << 32u);
        const uint64_t arrayPool = uint64_t(CLOD_RT_CACHE_OMM_ARRAY_POOL_LOW) | (uint64_t(CLOD_RT_CACHE_OMM_ARRAY_POOL_HIGH) << 32u);
        const uint64_t ommIndices = indexPool + uint64_t(omm.z + ordinal) * CLOD_RT_OMM_INDEX_UNIT_BYTES;
        const uint64_t ommArray = omm.w == CLOD_RT_OMM_NONE ? 0ull : arrayPool + uint64_t(omm.w) * CLOD_RT_OMM_ARRAY_ALIGNMENT;
        RtasClusterBuildTriangleInfoSetOpacityMicromaps(build, ommArray, ommIndices, 4u, 0u, true);
        ommKey = omm.y;
    }

    RWStructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_CACHE_CLAS_INFO_UAV];
    CLodRtClasInfo info;
    info.slabDescriptor = slabDescriptor;
    info.pageOffsetUnits = pageByteOffset >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT;
    info.localMeshlet = localMeshlet;
    info.groupLocalIndex = range.x | (ommKey != 0u ? CLOD_RT_CLAS_INFO_HAS_MICROMAPS : 0u); // Hit clusters name groups globally.
    clasInfo[slot] = info;
    RWStructuredBuffer<uint> slotOmmKeys = ResourceDescriptorHeap[CLOD_RT_CACHE_SLOT_OMM_KEYS_UAV];
    slotOmmKeys[slot] = ommKey;

    RWStructuredBuffer<RtasClusterBuildTriangleInfo> args = ResourceDescriptorHeap[CLOD_RT_CACHE_ARGS_UAV];
    args[id.x] = build;
}

// A built cluster's size in 128-byte units.
uint CLodRtCacheBuiltUnits(uint index)
{
    ByteAddressBuffer sizes = ResourceDescriptorHeap[CLOD_RT_CACHE_ARENA_SIZES_SRV];
    return max(1u, (sizes.Load(index * 8u) + CLOD_RT_CACHE_UNIT_BYTES - 1u) / CLOD_RT_CACHE_UNIT_BYTES);
}

// The block size class of a group's last page: blocks of 1, 2, 4 ... PAGE_UNITS units.
uint CLodRtCacheTailClass(uint units)
{
    return units <= 1u ? 0u : firstbithigh(units - 1u) + 1u;
}

uint CLodRtCacheCeilDiv(uint value, uint divisor)
{
    return (value + divisor - 1u) / divisor;
}

groupshared uint gsPackPages;
groupshared uint gsPackHead;
groupshared uint gsPackFull;
groupshared uint gsPackOwnTaken;
groupshared uint gsPackBlocks[CLOD_RT_CACHE_TAIL_CLASSES];
groupshared uint gsPackClassPage[CLOD_RT_CACHE_TAIL_CLASSES];
groupshared uint gsPackBlocksTaken[CLOD_RT_CACHE_TAIL_CLASSES];

// One group. Groups usable last frame first, then the rest, in range order: each entry's clusters are packed while the
// page queue can give the pages; once a group can't be, the rest can't either. A group's clusters fill whole pages of its
// own, first fit over its last PACK_WINDOW pages; what goes on its last page, its tail, takes a block of the smallest
// power-of-two size that holds it in a page shared with other tails of that size. A shared page counts the blocks in
// it and is given back when the last goes. A packed group is moved into its pages and replaces whatever entry it had. A
// request that isn't packed is transient with its entry's CLAS IDs, and gives the entry back; a rebuild that isn't is
// discarded, and the group keeps its CLAS. Then everything transient is placed, in the same order, by real size in this
// frame's transient arena, and moved there; once one doesn't fit, the rest have no CLAS this frame.
[numthreads(CLOD_RT_CACHE_GROUP_THREADS, 1, 1)]
void CLodRtCachePackCS(uint tid : SV_GroupIndex)
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    RWByteAddressBuffer ranges = ResourceDescriptorHeap[CLOD_RT_CACHE_RANGES_UAV];
    RWByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_CACHE_ENTRIES_UAV];
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_CACHE_STATE_UAV];
    RWStructuredBuffer<uint> pageNext = ResourceDescriptorHeap[CLOD_RT_CACHE_PAGE_NEXT_UAV];
    RWStructuredBuffer<uint> pageRefs = ResourceDescriptorHeap[CLOD_RT_CACHE_PAGE_REFS_UAV];
    RWStructuredBuffer<uint> scratch = ResourceDescriptorHeap[CLOD_RT_CACHE_PACK_SCRATCH_UAV];
    RWStructuredBuffer<uint64_t> clasAddresses = ResourceDescriptorHeap[CLOD_RT_CACHE_CLAS_ADDRESSES_UAV];
    StructuredBuffer<uint64_t> arenaAddresses = ResourceDescriptorHeap[CLOD_RT_CACHE_ARENA_ADDRESSES_SRV];
    RWStructuredBuffer<uint64_t> moveArgs = ResourceDescriptorHeap[CLOD_RT_CACHE_MOVE_ARGS_UAV];
    RWStructuredBuffer<uint64_t> moveDestinations = ResourceDescriptorHeap[CLOD_RT_CACHE_MOVE_DESTINATIONS_UAV];
    const uint frame = CLodRtCacheFrame();
    const uint rangeCount = ctrl.Load(CLOD_RT_CTRL_RANGE_COUNT * 4u);
    if (tid == 0u) {
        gsPackPages = CLodRtQueueAvailable(0u, frame);
        gsPackHead = ctrl.Load(CLOD_RT_CTRL_PAGE_HEAD * 4u);
        gsPackFull = 0u;
        gsPackOwnTaken = 0u;
    }
    if (tid < CLOD_RT_CACHE_TAIL_CLASSES) {
        gsPackBlocks[tid] = 0u;
        gsPackClassPage[tid] = 0u;
        gsPackBlocksTaken[tid] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    [loop]
    for (uint pass = 0u; pass < 2u; ++pass)
    [loop]
    for (uint base = 0u; base < rangeCount; base += CLOD_RT_CACHE_GROUP_THREADS) {
        const bool full = gsPackFull != 0u; // Before any thread of this chunk can set it.
        const uint r = base + tid;
        uint4 range = uint4(0u, 0u, 0u, 0u);
        uint4 extra = uint4(0u, 0u, 0u, 0u);
        bool member = false;
        if (r < rangeCount && (ranges.Load(r * CLOD_RT_CACHE_RANGE_BYTES + 36u) != 0u) == (pass == 0u)) {
            range = ranges.Load4(r * CLOD_RT_CACHE_RANGE_BYTES);
            extra = ranges.Load4(r * CLOD_RT_CACHE_RANGE_BYTES + 16u);
            member = range.y == CLOD_RT_RANGE_PERSISTENT || range.y == CLOD_RT_RANGE_PERSISTENT_REBUILD;
        }
        // Where each cluster goes: the first of the group's last PACK_WINDOW pages with room, else a new page. The fill of
        // the group's k-th page and each cluster's (page, offset) are kept in the scratch, at the group's clusters: a
        // group has no more pages than clusters. A CLAS larger than a page can't be packed.
        uint pages = 0u;
        bool packable = member;
        if (member) {
            [loop]
            for (uint i = range.z; i < range.z + range.w && packable; ++i) {
                const uint units = CLodRtCacheBuiltUnits(i);
                if (units > CLOD_RT_CACHE_PAGE_UNITS) {
                    packable = false;
                    CLodRtCacheCount(CLOD_RT_CTRL_OVERSIZE, 1u);
                    break;
                }
                uint page = pages;
                [loop]
                for (uint k = pages > CLOD_RT_CACHE_PACK_WINDOW ? pages - CLOD_RT_CACHE_PACK_WINDOW : 0u; k < pages; ++k) {
                    if (scratch[(range.z + k) * 2u] + units <= CLOD_RT_CACHE_PAGE_UNITS) {
                        page = k;
                        break;
                    }
                }
                if (page == pages) {
                    scratch[(range.z + page) * 2u] = 0u;
                    ++pages;
                }
                const uint offset = scratch[(range.z + page) * 2u];
                scratch[(range.z + page) * 2u] = offset + units;
                scratch[i * 2u + 1u] = (page << 8u) | offset;
            }
        }
        packable = packable && pages != 0u;
        const uint own = packable ? pages - 1u : 0u;
        const uint tailUnits = packable ? scratch[(range.z + pages - 1u) * 2u] : 0u;
        const uint tailClass = CLodRtCacheTailClass(max(tailUnits, 1u));
        uint totalOwn = 0u;
        const uint ownBefore = CLodRtCacheGroupScan(own, tid, totalOwn);
        uint blocksBefore[CLOD_RT_CACHE_TAIL_CLASSES];
        [unroll]
        for (uint c = 0u; c < CLOD_RT_CACHE_TAIL_CLASSES; ++c) {
            uint total = 0u;
            blocksBefore[c] = CLodRtCacheGroupScan(packable && tailClass == c ? 1u : 0u, tid, total);
        }
        // The pages needed up to and including this group: its own pages, and new shared pages for the blocks so far.
        uint needed = ownBefore + own;
        uint myBlock = 0u;
        [unroll]
        for (uint c2 = 0u; c2 < CLOD_RT_CACHE_TAIL_CLASSES; ++c2) {
            const uint perPage = CLOD_RT_CACHE_PAGE_UNITS >> c2;
            const uint blocks = gsPackBlocks[c2] + blocksBefore[c2] + (packable && tailClass == c2 ? 1u : 0u);
            needed += CLodRtCacheCeilDiv(blocks, perPage) - CLodRtCacheCeilDiv(gsPackBlocks[c2], perPage);
            if (tailClass == c2) myBlock = gsPackBlocks[c2] + blocksBefore[c2];
        }
        const bool fits = packable && !full && needed <= gsPackPages;
        const uint carriedPage = gsPackClassPage[tailClass]; // Before this chunk's last block can replace it.
        if (fits) {
            InterlockedMax(gsPackOwnTaken, ownBefore + own);
            InterlockedMax(gsPackBlocksTaken[tailClass], blocksBefore[tailClass] + 1u);
        }
        GroupMemoryBarrierWithGroupSync();

        if (fits) {
            // Own pages first, then each class's new shared pages, in the queue's order.
            uint classBase = gsPackHead + gsPackOwnTaken;
            [unroll]
            for (uint c3 = 0u; c3 < CLOD_RT_CACHE_TAIL_CLASSES; ++c3) {
                if (c3 < tailClass) {
                    const uint perPage = CLOD_RT_CACHE_PAGE_UNITS >> c3;
                    classBase += CLodRtCacheCeilDiv(gsPackBlocks[c3] + gsPackBlocksTaken[c3], perPage) - CLodRtCacheCeilDiv(gsPackBlocks[c3], perPage);
                }
            }
            const uint perPage = CLOD_RT_CACHE_PAGE_UNITS >> tailClass;
            const uint carried = gsPackBlocks[tailClass];
            const uint pageOfBlock = myBlock / perPage;
            const bool continued = (carried % perPage) != 0u && pageOfBlock == carried / perPage;
            const uint tailPage = continued ? carriedPage : CLodRtQueueItem(0u, classBase + pageOfBlock - CLodRtCacheCeilDiv(carried, perPage));
            const uint tailOffset = (myBlock % perPage) << tailClass;
            uint previousRefs = 0u;
            InterlockedAdd(pageRefs[tailPage], 1u, previousRefs);
            if (previousRefs == 0u) CLodRtCacheCount(CLOD_RT_CTRL_CACHED_PAGES, 1u);
            // The class's last block, if its page has room left, is where the next chunk continues.
            if (myBlock == carried + gsPackBlocksTaken[tailClass] - 1u && ((myBlock + 1u) % perPage) != 0u) gsPackClassPage[tailClass] = tailPage;

            const uint entry = extra.x;
            const uint entryBase = entry * CLOD_RT_CACHE_ENTRY_BYTES;
            [loop]
            for (uint k = 0u; k < own; ++k) {
                const uint page = CLodRtQueueItem(0u, gsPackHead + ownBefore + k);
                pageRefs[page] = 1u;
                pageNext[page] = k + 1u < own ? CLodRtQueueItem(0u, gsPackHead + ownBefore + k + 1u) : tailPage;
            }
            const uint firstPage = own != 0u ? CLodRtQueueItem(0u, gsPackHead + ownBefore) : tailPage;
            uint packedUnits = 0u;
            uint move = 0u;
            ctrl.InterlockedAdd(CLOD_RT_CTRL_MOVE_COUNT * 4u, range.w, move);
            [loop]
            for (uint i = 0u; i < range.w; ++i) {
                const uint index = range.z + i;
                const uint place = scratch[index * 2u + 1u];
                const uint k = place >> 8u;
                const uint page = k < own ? CLodRtQueueItem(0u, gsPackHead + ownBefore + k) : tailPage;
                const uint offset = (place & 0xFFu) + (k < own ? 0u : tailOffset);
                const uint64_t destination = CLodRtCachePoolAddress() + (uint64_t(page) * CLOD_RT_CACHE_PAGE_UNITS + offset) * CLOD_RT_CACHE_UNIT_BYTES;
                packedUnits += CLodRtCacheBuiltUnits(index);
                moveArgs[move + i] = arenaAddresses[index];
                moveDestinations[move + i] = destination;
                clasAddresses[CLodRtCacheBuiltId(index, range.y, entry, i)] = destination;
            }
            entries.Store2(entryBase + 4u, uint2(own + 1u, firstPage));
            entries.Store(entryBase + CLOD_RT_CACHE_ENTRY_UNITS_OFFSET, packedUnits);
            // The entry a rebuild replaces goes only now, once all the new CLAS have a place.
            const uint replacedPlusOne = ranges.Load(r * CLOD_RT_CACHE_RANGE_BYTES + 32u);
            const uint4 groupState = state.Load4(range.x * CLOD_RT_CACHE_STATE_BYTES);
            uint replaced = 0u;
            if (replacedPlusOne != 0u && CLodRtCacheCurrentEntry(range.x, groupState.x, replaced) && replaced == replacedPlusOne - 1u) {
                CLodRtCacheFreeEntry(replaced, range.x);
            }
            // Usable from the next frame's culling. A group asked for keeps the mark culling gave it this frame.
            state.Store4(range.x * CLOD_RT_CACHE_STATE_BYTES, uint4(entry + 1u, extra.y, extra.z, range.y == CLOD_RT_RANGE_PERSISTENT ? frame : groupState.w));
            CLodRtCacheCount(CLOD_RT_CTRL_PERSISTENT, 1u);
            CLodRtCacheCount(CLOD_RT_CTRL_CACHED_GROUPS, 1u);
            CLodRtCacheCount(CLOD_RT_CTRL_CACHED_PAGES, own);
            CLodRtCacheCount(CLOD_RT_CTRL_CACHED_CLUSTERS, range.w);
            CLodRtCacheCount(CLOD_RT_CTRL_CACHED_UNITS, packedUnits);
            CLodRtCacheCount(CLOD_RT_CTRL_BUILT_TOTAL, range.w);
        }
        else if (member) {
            // Its CLAS IDs are in this frame's CLAS: the queue gives them out again only after the frames in flight.
            const uint entry = extra.x;
            const uint entryBase = entry * CLOD_RT_CACHE_ENTRY_BYTES;
            const uint chunks = entries.Load(entryBase + 16u);
            entries.Store2(entryBase + 4u, uint2(0u, 0u));
            const uint chunkPlace = CLodRtQueuePushPlaces(1u, chunks);
            [loop]
            for (uint c4 = 0u; c4 < chunks; ++c4) CLodRtQueueWrite(1u, chunkPlace + c4, entries.Load(entryBase + CLOD_RT_CACHE_ENTRY_CHUNKS_OFFSET + c4 * 4u));
            CLodRtQueueWrite(2u, CLodRtQueuePushPlaces(2u, 1u), entry);
            ranges.Store(r * CLOD_RT_CACHE_RANGE_BYTES + 4u, range.y == CLOD_RT_RANGE_PERSISTENT ? CLOD_RT_RANGE_FALLBACK : CLOD_RT_RANGE_DISCARD);
            CLodRtCacheCount(CLOD_RT_CTRL_REFUSED, 1u);
            if (packable) gsPackFull = 1u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0u) {
            uint taken = gsPackOwnTaken;
            [unroll]
            for (uint c5 = 0u; c5 < CLOD_RT_CACHE_TAIL_CLASSES; ++c5) {
                const uint perPage = CLOD_RT_CACHE_PAGE_UNITS >> c5;
                taken += CLodRtCacheCeilDiv(gsPackBlocks[c5] + gsPackBlocksTaken[c5], perPage) - CLodRtCacheCeilDiv(gsPackBlocks[c5], perPage);
                gsPackBlocks[c5] += gsPackBlocksTaken[c5];
                gsPackBlocksTaken[c5] = 0u;
            }
            gsPackPages -= taken;
            gsPackHead += taken;
            gsPackOwnTaken = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0u) {
        ctrl.Store(CLOD_RT_CTRL_PAGE_HEAD * 4u, gsPackHead);
        gsPackPages = 0u; // From here: the transient arena's fill, in units.
        gsPackFull = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // Transient placement: the requests Assign gave no entry (TRANSIENT, the frame's own IDs) and those Pack had no
    // pages for (FALLBACK, their entry's IDs), in the same order as above.
    RWStructuredBuffer<uint2> transient = ResourceDescriptorHeap[CLOD_RT_CACHE_TRANSIENT_UAV];
    [loop]
    for (uint tpass = 0u; tpass < 2u; ++tpass)
    [loop]
    for (uint tbase = 0u; tbase < rangeCount; tbase += CLOD_RT_CACHE_GROUP_THREADS) {
        const bool full = gsPackFull != 0u; // Before any thread of this chunk can set it.
        const uint r = tbase + tid;
        uint4 range = uint4(0u, 0u, 0u, 0u);
        uint entry = CLOD_RT_CACHE_INVALID;
        bool member = false;
        if (r < rangeCount && (ranges.Load(r * CLOD_RT_CACHE_RANGE_BYTES + 36u) != 0u) == (tpass == 0u)) {
            range = ranges.Load4(r * CLOD_RT_CACHE_RANGE_BYTES);
            entry = ranges.Load(r * CLOD_RT_CACHE_RANGE_BYTES + 16u);
            member = range.y == CLOD_RT_RANGE_TRANSIENT || range.y == CLOD_RT_RANGE_FALLBACK;
        }
        uint units = 0u;
        if (member) {
            [loop]
            for (uint i = range.z; i < range.z + range.w; ++i) units += CLodRtCacheBuiltUnits(i);
        }
        uint totalUnits = 0u;
        const uint unitsBefore = CLodRtCacheGroupScan(units, tid, totalUnits);
        if (tid == 0u) gsPackOwnTaken = 0u;
        GroupMemoryBarrierWithGroupSync();
        const uint fill = gsPackPages;
        const bool fits = member && !full && fill + unitsBefore + units <= CLOD_RT_CACHE_TRANSIENT_ARENA_UNITS;
        if (fits) {
            uint move = 0u;
            ctrl.InterlockedAdd(CLOD_RT_CTRL_MOVE_COUNT * 4u, range.w, move);
            uint place = CLOD_RT_CACHE_TRANSIENT_ARENA_UNIT + fill + unitsBefore;
            [loop]
            for (uint i = 0u; i < range.w; ++i) {
                const uint index = range.z + i;
                const uint64_t destination = CLodRtCachePoolAddress() + uint64_t(place) * CLOD_RT_CACHE_UNIT_BYTES;
                place += CLodRtCacheBuiltUnits(index);
                moveArgs[move + i] = arenaAddresses[index];
                moveDestinations[move + i] = destination;
                clasAddresses[CLodRtCacheBuiltId(index, range.y, entry, i)] = destination;
            }
            transient[range.x] = uint2(frame, range.y == CLOD_RT_RANGE_TRANSIENT
                ? (CLOD_RT_TRANSIENT_DIRECT | (CLOD_RT_CACHE_TRANSIENT_ID_BASE + range.z)) : entry);
            InterlockedMax(gsPackOwnTaken, unitsBefore + units);
            CLodRtCacheCount(CLOD_RT_CTRL_TRANSIENT, 1u);
            CLodRtCacheCount(CLOD_RT_CTRL_TRANSIENT_CLUSTERS, range.w);
            CLodRtCacheCount(CLOD_RT_CTRL_TRANSIENT_TOTAL, range.w);
            CLodRtCacheCount(CLOD_RT_CTRL_BUILT_TOTAL, range.w);
        }
        else if (member) {
            ranges.Store(r * CLOD_RT_CACHE_RANGE_BYTES + 4u, CLOD_RT_RANGE_DISCARD);
            CLodRtCacheCount(CLOD_RT_CTRL_DISCARDED, 1u);
            CLodRtCacheCount(CLOD_RT_CTRL_DISCARDED_CLUSTERS, range.w);
            gsPackFull = 1u;
        }
        GroupMemoryBarrierWithGroupSync();
        if (tid == 0u) gsPackPages += gsPackOwnTaken;
        GroupMemoryBarrierWithGroupSync();
    }
}

// After the frame's last cache work and its readback: the queue tails for the frames after, the keep age, the next frame.
[numthreads(1, 1, 1)]
void CLodRtCacheEndCS()
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_CACHE_CTRL_UAV];
    const uint frame = ctrl.Load(CLOD_RT_CTRL_FRAME * 4u);
    [unroll]
    for (uint q = 0u; q < 3u; ++q) {
        ctrl.Store((CLOD_RT_CTRL_TAIL_HISTORY + (frame % CLOD_RT_CACHE_HISTORY) * 3u + q) * 4u, ctrl.Load((CLOD_RT_CTRL_PAGE_TAIL + q * 2u) * 4u));
    }
    // Groups went transient for lack of room: what is idle goes sooner. Otherwise the keep age grows back.
    const uint keepAge = ctrl.Load(CLOD_RT_CTRL_KEEP_AGE * 4u);
    ctrl.Store(CLOD_RT_CTRL_KEEP_AGE * 4u, ctrl.Load(CLOD_RT_CTRL_REFUSED * 4u) != 0u
        ? max(keepAge / 2u, 1u) : min(keepAge + 2u, CLOD_RT_CACHE_MAX_KEEP_AGE));
    ctrl.Store(CLOD_RT_CTRL_OMM_RETIRE_CURSOR * 4u, (ctrl.Load(CLOD_RT_CTRL_OMM_RETIRE_CURSOR * 4u) + CLOD_RT_CACHE_OMM_RETIRE_WINDOW) & (CLOD_RT_OMM_TABLE_ENTRIES - 1u));
    ctrl.Store(CLOD_RT_CTRL_NOT_BUILT_TOTAL * 4u, ctrl.Load(CLOD_RT_CTRL_NOT_BUILT_TOTAL * 4u) + ctrl.Load(CLOD_RT_CTRL_NOT_BUILT * 4u));
    ctrl.Store(CLOD_RT_CTRL_DISCARDED_TOTAL * 4u, ctrl.Load(CLOD_RT_CTRL_DISCARDED_TOTAL * 4u) + ctrl.Load(CLOD_RT_CTRL_DISCARDED * 4u));
    [unroll]
    for (uint word = CLOD_RT_CTRL_FRAME_FIRST; word < CLOD_RT_CTRL_FRAME_END; ++word) ctrl.Store(word * 4u, 0u);
    RWByteAddressBuffer rebuilds = ResourceDescriptorHeap[CLOD_RT_CACHE_OMM_REBUILD_UAV];
    rebuilds.Store(0u, 0u);
    ctrl.Store(CLOD_RT_CTRL_FRAME * 4u, frame + 1u);
}

// ---- Group the cluster list by instance

uint CLodRtClusterCount()
{
    StructuredBuffer<uint> counter = ResourceDescriptorHeap[CLOD_RT_SCENE_CLUSTER_COUNTER_SRV];
    return min(counter[0], CLOD_RT_SCENE_CLUSTER_CAPACITY);
}

uint4 CLodRtLoadCluster(uint index)
{
    ByteAddressBuffer clusters = ResourceDescriptorHeap[CLOD_RT_SCENE_CLUSTERS_SRV];
    return clusters.Load4(index * 16u);
}

// A selected cluster's instance key: its draw, or DRAW_KEYS + the assembly transform of the part portal occurrence that
// reached it (variant A roots are drawn once, so the transform alone names the occurrence). Invalid past the table.
#define CLOD_RT_INVALID_KEY 0xFFFFFFFFu

uint CLodRtClusterTransform(uint index)
{
    StructuredBuffer<uint2> sideBand = ResourceDescriptorHeap[CLOD_RT_SCENE_CLUSTER_SIDE_BAND_SRV];
    return sideBand[index].x;
}

uint CLodRtInstanceKey(uint draw, uint transformIndex)
{
    if (transformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL) {
        return draw < CLOD_RT_SCENE_DRAW_KEYS ? draw : CLOD_RT_INVALID_KEY;
    }
    return transformIndex < CLOD_RT_SCENE_DRAW_TABLE_ENTRIES - CLOD_RT_SCENE_DRAW_KEYS
        ? CLOD_RT_SCENE_DRAW_KEYS + transformIndex : CLOD_RT_INVALID_KEY;
}

// Draw table, per instance key (a draw, or a portal occurrence): a 64-bit [cluster count : 20, signature : 44] updated with one atomic per cluster, then the
// [end, cursor] of the range this draw fills in, which is empty for a draw that shares another's BLAS.
#define CLOD_RT_DRAW_ENTRY_BYTES 16u
#define CLOD_RT_DRAW_COUNT_BITS 20u

uint CLodRtMix(uint value)
{
    value ^= value >> 16u;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13u;
    value *= 0xC2B2AE35u;
    return value ^ (value >> 16u);
}

uint CLodRtSceneFrame()
{
    RWByteAddressBuffer ctrl = ResourceDescriptorHeap[CLOD_RT_SCENE_CACHE_CTRL_UAV];
    return ctrl.Load(CLOD_RT_CTRL_FRAME * 4u);
}

// Whether the group's CLAS are current, marking it used if so; otherwise marks it requested and sets its demand bits.
bool CLodRtRecordDemandCurrent(uint groupGlobal, uint frame, uint4 groupState, uint stateOffset)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_SCENE_GROUP_STATE_UAV];
    ByteAddressBuffer entries = ResourceDescriptorHeap[CLOD_RT_SCENE_CACHE_ENTRIES_SRV];
    uint entryBytes = 0u;
    entries.GetDimensions(entryBytes);
    uint entry = 0u;
    if (CLodRtCacheStateEntry(groupState.x, entryBytes / CLOD_RT_CACHE_ENTRY_BYTES, entry) && entries.Load(entry * CLOD_RT_CACHE_ENTRY_BYTES) == groupGlobal) {
        StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
        uint slab = 0u;
        uint signature = 0u;
        if (CLodRtGroupLocation(groups[groupGlobal], slab, signature) && slab == groupState.y && signature == groupState.z) {
            state.Store(stateOffset + 12u, frame);
            return true;
        }
    }
    state.Store(stateOffset + 12u, frame | CLOD_RT_CACHE_REQUESTED);
    RWByteAddressBuffer demand = ResourceDescriptorHeap[CLOD_RT_SCENE_DEMAND_UAV];
    const uint wordIndex = groupGlobal >> 5u;
    demand.InterlockedOr(CLOD_RT_DEMAND_BITS_OFFSET + wordIndex * 4u, 1u << (groupGlobal & 31u));
    demand.InterlockedOr((wordIndex >> 5u) * 4u, 1u << (wordIndex & 31u));
    return false;
}

void CLodRtRecordDemandIdentity(uint4 packed, uint groupGlobal, uint draw, uint transformIndex)
{
    RWByteAddressBuffer demand = ResourceDescriptorHeap[CLOD_RT_SCENE_DEMAND_UAV];

    // The micromaps an alpha-tested material of this cluster can use: their identity (CLodOpacityMicromapCache.h), else 0.
    // The cluster's own material, through its palette index, as its hits and alpha tests take it. A hierarchy root's draw
    // has one PerMeshBuffer material of the many in its palette: asked for with that one, a part's group named micromaps
    // of a material it doesn't draw with, which were baked for another model's container if at all.
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const PerMeshInstanceBuffer meshTemplate = LoadMeshTemplateForDrawRecord(LoadInstanceDrawRecord(draw));
    uint paletteIndex = 0u;
    if (CLodTemplateHasPalette(meshTemplate)) {
        // The meshlet's material slot from its page, as CLodLoadRasterCluster finds it for hits.
        const uint slab = CLodVisibleClusterPageSlabDescriptorIndex(packed);
        const uint pageOffset = CLodVisibleClusterPageSlabByteOffset(packed);
        const uint localMeshlet = CLodVisibleClusterLocalMeshletIndex(packed);
        const CLodPageHeader header = LoadPageHeader(slab, pageOffset);
        if (slab == 0u || header.formatAndKind != CLOD_TRIANGLE_PAGE_MAGIC || header.descriptorOffset == 0u || localMeshlet >= header.meshletCount) return;
        const CLodMeshletDescriptor descriptor = LoadMeshletDescriptor(slab, pageOffset, header.descriptorOffset, localMeshlet);
        paletteIndex = CLodPaletteIndexFromDescriptor(meshTemplate, groupGlobal, transformIndex, asuint(descriptor.terrainRvtLocalSkyrimXYRadius));
    }
    const uint materialIndex = CLodResolveMaterial(meshTemplate, perMeshBuffer[meshTemplate.perMeshBufferIndex], paletteIndex).materialDataIndex;
    const MaterialInfo material = materials[materialIndex];
    if ((material.materialFlags & MATERIAL_ALPHA_TEST) != 0u && material.ommIdentity != 0u) {
        demand.InterlockedMax(CLOD_RT_DEMAND_IDENTITY_OFFSET + groupGlobal * 4u, material.ommIdentity);
    }
}

// What the list needs from the CLAS cache, per listed cluster: a group whose CLAS are current (its entry holds it and
// was built from where all its pages are now) is marked used this frame, which keeps it from eviction; any other sets
// its bit in the demand bitset, which Collect turns into this frame's requests, with the largest micromap identity its
// clusters' materials ask for. Every step is idempotent, so the outcome doesn't depend on which cluster comes first.
void CLodRtRecordDemand(uint4 packed, uint groupGlobal, uint draw, uint transformIndex)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[CLOD_RT_SCENE_GROUP_STATE_UAV];
    uint stateBytes = 0u;
    state.GetDimensions(stateBytes);
    if (groupGlobal >= stateBytes / CLOD_RT_CACHE_STATE_BYTES || groupGlobal >= CLOD_RT_DEMAND_GROUPS) return;
    const uint frame = CLodRtSceneFrame();
    const uint stateOffset = groupGlobal * CLOD_RT_CACHE_STATE_BYTES;
    const uint4 groupState = state.Load4(stateOffset);
    if (groupState.w == frame) return; // Current, as another cluster found.
    // Requested: every cluster still adds its material below, so the identity is the largest of all of them whichever
    // cluster came first. Clusters of one group can take different materials.
    if (groupState.w != (frame | CLOD_RT_CACHE_REQUESTED)) {
        if (CLodRtRecordDemandCurrent(groupGlobal, frame, groupState, stateOffset)) return;
    }
    CLodRtRecordDemandIdentity(packed, groupGlobal, draw, transformIndex);
}

// Puts last frame's touched entries back to zero, which is far cheaper than clearing a table this size.
[numthreads(64, 1, 1)]
void CLodRtResetDrawsCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    if (id.x >= min(counters.Load(CLOD_RT_COUNTER_INSTANCES * 4u), CLOD_RT_SCENE_INSTANCE_CAPACITY)) {
        return;
    }
    RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    drawTable.Store4(instanceList[id.x] * CLOD_RT_DRAW_ENTRY_BYTES, uint4(0u, 0u, 0u, 0u));
}

[numthreads(256, 1, 1)]
void CLodRtCountCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLodRtClusterCount()) {
        return;
    }
    const uint4 packed = CLodRtLoadCluster(id.x);
    const uint draw = CLodVisibleClusterInstanceID(packed);
    const uint transformIndex = CLodRtClusterTransform(id.x);
    const uint key = CLodRtInstanceKey(draw, transformIndex);
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    if (key == CLOD_RT_INVALID_KEY) {
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_CLUSTERS * 4u, 1u);
        return;
    }
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    CLodRtRecordDemand(packed, CLodVisibleClusterGroupID(packed), draw, transformIndex);

    // A signature of which clusters this draw selected, independent of the order they were appended in. It is built
    // from the page location, so two draws agree exactly when they reference the same CLAS.
    const uint clusterKey = CLodVisibleClusterLocalMeshletIndex(packed)
        | (CLodVisibleClusterPageSlabDescriptorIndex(packed) << 14u);
    const uint pageKey = CLodVisibleClusterPageSlabByteOffset(packed) >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT;
    const uint mixed = CLodRtMix(clusterKey ^ CLodRtMix(pageKey));
    const uint64_t signature = (uint64_t(CLodRtMix(mixed + 0x9E3779B9u) & 0xFFFu) << 32u) | uint64_t(mixed);
    uint64_t previous = 0ull;
    drawTable.InterlockedAdd64(key * CLOD_RT_DRAW_ENTRY_BYTES, (signature << CLOD_RT_DRAW_COUNT_BITS) + 1ull, previous);
    if (previous == 0ull) {
        uint instance = 0u;
        counters.InterlockedAdd(CLOD_RT_COUNTER_INSTANCES * 4u, 1u, instance);
        if (instance < CLOD_RT_SCENE_INSTANCE_CAPACITY) {
            RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
            RWStructuredBuffer<uint4> instanceRows = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_ROWS_UAV];
            instanceList[instance] = key;
            // Any of the occurrence's clusters names its part, which says what palette range its materials take.
            instanceRows[instance] = uint4(draw, transformIndex, CLodVisibleClusterGroupID(packed), 0u);
        }
        else {
            counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_INSTANCES * 4u, 1u);
        }
    }
}

// One thread per instance. The share table key is the draw's whole [signature : 44, count : 20], claimed with one atomic,
// so the slot identifies the BLAS before its index is known. Consumers record the slot and resolve it when writing instances.
[numthreads(64, 1, 1)]
void CLodRtAllocateCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    if (id.x >= min(counters.Load(CLOD_RT_COUNTER_INSTANCES * 4u), CLOD_RT_SCENE_INSTANCE_CAPACITY)) {
        return;
    }
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
    RWStructuredBuffer<RtasClusterBuildBottomLevelInfo> blasArgs = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_ARGS_UAV];
    RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
    RWByteAddressBuffer shareTable = ResourceDescriptorHeap[CLOD_RT_SCENE_SHARE_TABLE_UAV];
    const uint64_t addressArray = uint64_t(CLOD_RT_SCENE_ADDRESS_ARRAY_LOW) | (uint64_t(CLOD_RT_SCENE_ADDRESS_ARRAY_HIGH) << 32u);

    const uint draws = instanceList[id.x] * CLOD_RT_DRAW_ENTRY_BYTES;
    const uint64_t key = drawTable.Load<uint64_t>(draws);
    const uint wanted = uint(key & ((1ull << CLOD_RT_DRAW_COUNT_BITS) - 1ull));
    drawTable.Store(draws, id.x + 1u); // From here on, what instances that skipped traversal need: this draw's row.

    const uint mask = CLOD_RT_SCENE_SHARE_TABLE_ENTRIES - 1u;
    uint slot = CLodRtMix(uint(key) ^ uint(key >> 32u)) & mask;
    bool claimed = false;
    for (uint probe = 0u; probe < 64u; ++probe) {
        uint64_t original = 0ull;
        shareTable.InterlockedCompareExchange64(slot * 16u, 0ull, key, original);
        if (original == 0ull) {
            claimed = true;
            break;
        }
        if (original == key) {
            instanceBlas[id.x] = CLOD_RT_INSTANCE_BLAS_SHARED | slot;
            return; // Its range stays empty: nothing for this draw to place.
        }
        slot = (slot + 1u) & mask;
    }

    // Both counters can overshoot their capacity here, and CLodRtAllocateFinishCS clamps them.
    uint blasIndex = 0u;
    counters.InterlockedAdd(CLOD_RT_COUNTER_BLAS_ARGS * 4u, 1u, blasIndex);
    if (blasIndex >= CLOD_RT_SCENE_BLAS_CAPACITY) {
        instanceBlas[id.x] = 0xFFFFFFFFu; // Inactive this frame, and so is anything sharing this slot.
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_BLASES * 4u, 1u);
        return;
    }
    const uint capped = min(wanted, CLOD_RT_SCENE_MAX_CLUSTERS_PER_INSTANCE);
    uint base = 0u;
    counters.InterlockedAdd(CLOD_RT_COUNTER_CLUSTERS * 4u, capped, base);
    const uint available = base < CLOD_RT_SCENE_REFERENCE_CAPACITY ? CLOD_RT_SCENE_REFERENCE_CAPACITY - base : 0u;
    const uint count = min(capped, available);
    if (count != wanted) {
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_BLASES * 4u, 1u);
    }
    // Every index below the final count must hold valid arguments, so this one is written even if it ended up empty.
    RtasClusterBuildBottomLevelInfo args;
    args.clusterReferencesCount = count;
    args.clusterReferencesStride = 8u;
    args.clusterReferences = addressArray + uint64_t(base) * 8ull;
    blasArgs[blasIndex] = args;
    instanceBlas[id.x] = count != 0u ? blasIndex : 0xFFFFFFFFu;
    // Only a whole selection can be shared: a truncated one differs from what its key says.
    if (claimed && count == wanted) {
        shareTable.Store((slot * 16u) + 12u, blasIndex + 1u);
    }
    if (count != 0u) {
        drawTable.Store2(draws + 8u, uint2(base + count, base));
    }
}

// Instances that took the coarsest level from another draw of the mesh without being traversed.
[numthreads(64, 1, 1)]
void CLodRtAddCoarseConsumersCS(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer coarse = ResourceDescriptorHeap[CLOD_RT_SCENE_COARSE_CUT_UAV];
    if (id.x >= min(coarse.Load(0u), CLOD_RT_COARSE_CONSUMER_CAPACITY)) {
        return;
    }
    const uint2 pair = coarse.Load2(16u + id.x * 8u);
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    const uint providerRow = drawTable.Load(pair.y * CLOD_RT_DRAW_ENTRY_BYTES);
    if (providerRow == 0u) {
        return; // Nothing of this mesh was selected, which means nothing it needs has a CLAS yet.
    }
    uint row = 0u;
    counters.InterlockedAdd(CLOD_RT_COUNTER_INSTANCES * 4u, 1u, row);
    if (row >= CLOD_RT_SCENE_INSTANCE_CAPACITY) {
        counters.InterlockedAdd(CLOD_RT_COUNTER_DROPPED_INSTANCES * 4u, 1u);
        return;
    }
    RWStructuredBuffer<uint> instanceList = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_LIST_UAV];
    RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
    instanceList[row] = pair.x;
    RWStructuredBuffer<uint4> instanceRows = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_ROWS_UAV];
    instanceRows[row] = uint4(pair.x, CLOD_ASSEMBLY_TRANSFORM_SENTINEL, 0xFFFFFFFFu, 0u);
    instanceBlas[row] = CLOD_RT_INSTANCE_BLAS_VIA_ROW | (providerRow - 1u);
    counters.InterlockedAdd(CLOD_RT_COUNTER_COARSE_CONSUMERS * 4u, 1u);
}

[numthreads(1, 1, 1)]
void CLodRtAllocateFinishCS()
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];
    counters.Store(CLOD_RT_COUNTER_BLAS_ARGS * 4u, min(counters.Load(CLOD_RT_COUNTER_BLAS_ARGS * 4u), CLOD_RT_SCENE_BLAS_CAPACITY));
    counters.Store(CLOD_RT_COUNTER_CLUSTERS * 4u, min(counters.Load(CLOD_RT_COUNTER_CLUSTERS * 4u), CLOD_RT_SCENE_REFERENCE_CAPACITY));
    counters.Store(CLOD_RT_COUNTER_TLAS_INSTANCES * 4u, min(counters.Load(CLOD_RT_COUNTER_INSTANCES * 4u), CLOD_RT_SCENE_INSTANCE_CAPACITY));
    StructuredBuffer<uint> rawCounter = ResourceDescriptorHeap[CLOD_RT_SCENE_CLUSTER_COUNTER_SRV]; // EXPERIMENT: demand, unclamped
    counters.Store(CLOD_RT_COUNTER_LIST_CLUSTERS * 4u, rawCounter[0]);
}

[numthreads(256, 1, 1)]
void CLodRtPlaceCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLodRtClusterCount()) {
        return;
    }
    const uint4 packed = CLodRtLoadCluster(id.x);
    const uint key = CLodRtInstanceKey(CLodVisibleClusterInstanceID(packed), CLodRtClusterTransform(id.x));
    if (key == CLOD_RT_INVALID_KEY) {
        return;
    }
    RWByteAddressBuffer drawTable = ResourceDescriptorHeap[CLOD_RT_SCENE_DRAW_TABLE_UAV];
    const uint draws = key * CLOD_RT_DRAW_ENTRY_BYTES;
    const uint end = drawTable.Load(draws + 8u);
    if (end == 0u) {
        return; // Shares another draw's BLAS, which is most of them.
    }
    uint element = 0u;
    drawTable.InterlockedAdd(draws + 12u, 1u, element);
    if (element >= end) {
        return;
    }

    const uint groupGlobal = CLodVisibleClusterGroupID(packed);
    ByteAddressBuffer groupState = ResourceDescriptorHeap[CLOD_RT_SCENE_GROUP_STATE_SRV];
    ByteAddressBuffer cacheEntries = ResourceDescriptorHeap[CLOD_RT_SCENE_CACHE_ENTRIES_SRV];
    StructuredBuffer<uint2> transient = ResourceDescriptorHeap[CLOD_RT_SCENE_CACHE_TRANSIENT_SRV];
    const uint slot = CLodRtCacheFindSlot(groupState, cacheEntries, transient, CLodRtSceneFrame(), groupGlobal,
        CLodVisibleClusterPageSlabDescriptorIndex(packed),
        CLodVisibleClusterPageSlabByteOffset(packed) >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT,
        CLodVisibleClusterLocalMeshletIndex(packed));

    // A group the cache built no CLAS for this frame (over the budget, or no room to keep it) leaves a null reference,
    // which the build ignores: that part of the scene is missing this frame, and it is asked for again next frame.
    uint64_t address = 0ull;
    if (slot == CLOD_RT_CACHE_INVALID) {
        RWByteAddressBuffer cacheCtrl = ResourceDescriptorHeap[CLOD_RT_SCENE_CACHE_CTRL_UAV];
        cacheCtrl.InterlockedAdd(CLOD_RT_CTRL_MISSING_TOTAL * 4u, 1u);
    }
    else {
        RWStructuredBuffer<uint64_t> clasAddresses = ResourceDescriptorHeap[CLOD_RT_SCENE_CLAS_ADDRESSES_UAV];
        address = clasAddresses[slot];
        // Which material the BLAS's micromaps were baked for, so instances of another can disable them.
        RWStructuredBuffer<uint> slotOmmKeys = ResourceDescriptorHeap[CLOD_RT_SCENE_SLOT_OMM_KEYS_UAV];
        const uint ommKey = slotOmmKeys[slot];
        if (ommKey != 0u) {
            RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
            const uint blas = instanceBlas[drawTable.Load(draws) - 1u];
            if (blas < CLOD_RT_SCENE_BLAS_CAPACITY) {
                RWStructuredBuffer<uint> blasOmmKeys = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_OMM_KEYS_UAV];
                uint previous = 0u;
                InterlockedCompareExchange(blasOmmKeys[blas], 0u, ommKey, previous);
                if (previous != 0u && previous != ommKey) {
                    InterlockedOr(blasOmmKeys[blas], CLOD_RT_OMM_KEY_CONFLICT);
                }
            }
        }
    }
    RWStructuredBuffer<uint64_t> addressArray = ResourceDescriptorHeap[CLOD_RT_SCENE_ADDRESS_ARRAY_UAV];
    addressArray[element] = address;
}

// ---- TLAS instances

[numthreads(64, 1, 1)]
void CLodRtWriteInstancesCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CLOD_RT_SCENE_TLAS_INSTANCE_COUNT) {
        return;
    }
    RWStructuredBuffer<RtasInstanceDesc> instances = ResourceDescriptorHeap[CLOD_RT_SCENE_TLAS_INSTANCES_UAV];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[CLOD_RT_SCENE_COUNTERS_UAV];

    RtasInstanceDesc desc = (RtasInstanceDesc)0; // A null acceleration structure makes the row inactive.
    if (id.x < counters.Load(CLOD_RT_COUNTER_TLAS_INSTANCES * 4u)) {
        StructuredBuffer<uint64_t> blasAddresses = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_ADDRESSES_SRV];
        RWStructuredBuffer<uint> instanceBlas = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_BLAS_UAV];
        RWStructuredBuffer<uint4> instanceRows = ResourceDescriptorHeap[CLOD_RT_SCENE_INSTANCE_ROWS_UAV];
        const uint4 rowInfo = instanceRows[id.x];
        const uint draw = rowInfo.x;
        const uint transformIndex = rowInfo.y;
        const InstanceDrawRecordBuffer record = LoadInstanceDrawRecord(draw);
        const PerObjectBuffer transform = LoadInstanceTransformForDrawRecordWithAssemblyTransform(record, transformIndex);
        const row_major matrix model = transform.model;

        // Row-vector model matrix to a 3x4 of column-vector rows.
        for (uint row = 0u; row < 3u; ++row) {
            desc.transformRows[row] = float4(model[0][row], model[1][row], model[2][row], model[3][row]);
        }

        StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
        const PerMeshInstanceBuffer meshTemplate = LoadMeshTemplateForDrawRecord(record);
        const PerMeshBuffer perMesh = perMeshBuffer[meshTemplate.perMeshBufferIndex];
        StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];

        // The materials this row's clusters can take: its draw's, or for a hierarchy root the palette range of the part
        // the row's clusters belong to (the root's own geometry spans its whole palette). Instance flags hold for the
        // whole range: culling off if any material is double-sided, forced opaque only if none tests alpha.
        uint rowMaterial = perMesh.materialDataIndex;
        uint rowMaterialCount = 1u;
        uint paletteFirst = 0u;
        if (CLodTemplateHasPalette(meshTemplate)) {
            uint slotCount = meshTemplate.materialPaletteCount;
            if (rowInfo.z != 0xFFFFFFFFu) {
                StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
                StructuredBuffer<CLodMeshMetadata> metadata = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
                slotCount = max(metadata[groups[rowInfo.z].partIndex].materialSlotCount, 1u);
            }
            const uint offset = min(CLodAssemblyMaterialBase(transformIndex), meshTemplate.materialPaletteCount - 1u);
            paletteFirst = meshTemplate.materialPaletteBase + offset;
            rowMaterialCount = min(slotCount, meshTemplate.materialPaletteCount - offset);
            StructuredBuffer<CLodMaterialPaletteEntry> palettes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MaterialPalettes)];
            rowMaterial = palettes[paletteFirst].materialDataIndex;
        }
        uint materialFlags = materials[rowMaterial].materialFlags;
        bool anyDoubleSided = (materialFlags & MATERIAL_DOUBLE_SIDED) != 0u;
        bool anyAlpha = (materialFlags & MATERIAL_ALPHA_TEST) != 0u;
        if (rowMaterialCount > 1u) {
            StructuredBuffer<CLodMaterialPaletteEntry> palettes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MaterialPalettes)];
            for (uint k = 1u; k < min(rowMaterialCount, 64u); ++k) {
                const uint other = materials[palettes[paletteFirst + k].materialDataIndex].materialFlags;
                anyDoubleSided = anyDoubleSided || (other & MATERIAL_DOUBLE_SIDED) != 0u;
                anyAlpha = anyAlpha || (other & MATERIAL_ALPHA_TEST) != 0u;
            }
        }

        // Raster's counter-clockwise front faces are clockwise here, because its projection flips Y. Mirrored objects swap.
        uint flags = (transform.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u ? RTAS_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE : 0u;
        if (anyDoubleSided) {
            flags |= RTAS_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        }
        if (!anyAlpha) {
            flags |= RTAS_INSTANCE_FLAG_FORCE_OPAQUE;
        }
        const uint portalBit = transformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL ? CLOD_RT_INSTANCE_ID_PORTAL : 0u;
        desc.instanceIDAndMask = (draw & CLOD_RT_INSTANCE_ID_DRAW_MASK) | portalBit | (0xFFu << 24u);
        // Selects the hit groups of pipelines that trace this BVH. See pathTrace.lib.hlsl. Ray queries ignore it.
        const uint hitGroupContribution = (materialFlags & MATERIAL_TERRAIN) != 0u ? 2u : 0u;
        desc.contributionAndFlags = (flags << 24u) | hitGroupContribution;
        uint blas = instanceBlas[id.x];
        if (blas != CLOD_RT_INSTANCE_BLAS_NONE && (blas & CLOD_RT_INSTANCE_BLAS_VIA_ROW) != 0u) {
            blas = instanceBlas[blas & ~CLOD_RT_INSTANCE_BLAS_VIA_ROW];
        }
        if (blas != CLOD_RT_INSTANCE_BLAS_NONE && (blas & CLOD_RT_INSTANCE_BLAS_SHARED) != 0u) {
            RWByteAddressBuffer shareTable = ResourceDescriptorHeap[CLOD_RT_SCENE_SHARE_TABLE_UAV];
            const uint owner = shareTable.Load(((blas & ~CLOD_RT_INSTANCE_BLAS_SHARED) * 16u) + 12u);
            blas = owner != 0u ? owner - 1u : 0xFFFFFFFFu;
        }
        desc.accelerationStructure = blas != 0xFFFFFFFFu ? blasAddresses[blas] : 0ull;

        // Every alpha-tested instance in the BVH asks for its test mip. Without that, foliage only secondary rays reach would
        // never get there, and baked foliage would drift back down, since rays that micromaps answer run no any-hit to ask.
        if (CLOD_RT_SCENE_OMM_ACTIVE != 0u && blas != 0xFFFFFFFFu && (materialFlags & MATERIAL_ALPHA_TEST) != 0u) {
            counters.InterlockedAdd(CLOD_RT_COUNTER_ALPHA_INSTANCES * 4u, 1u);
            const MaterialInfo alphaMaterial = materials[rowMaterial];
            if ((materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u && alphaMaterial.baseColorStreamingTextureID != 0u) {
                const CLodRtAlphaImage image = CLodRtResolveAlphaImage(alphaMaterial.baseColorTextureIndex, alphaMaterial.baseColorStreamingTextureID);
                RecordTextureStreamingFeedbackRequestedTopMip(LoadTextureStreamingInfo(alphaMaterial.baseColorStreamingTextureID), alphaMaterial.baseColorStreamingTextureID, image.absoluteMip);
            }
            if ((materialFlags & MATERIAL_OPACITY_TEXTURE) != 0u && alphaMaterial.opacityStreamingTextureID != 0u) {
                const CLodRtAlphaImage image = CLodRtResolveAlphaImage(alphaMaterial.opacityTextureIndex, alphaMaterial.opacityStreamingTextureID);
                RecordTextureStreamingFeedbackRequestedTopMip(LoadTextureStreamingInfo(alphaMaterial.opacityStreamingTextureID), alphaMaterial.opacityStreamingTextureID, image.absoluteMip);
            }
        }

        // Micromaps were baked offline against one material's alpha test at its test mip (the material's identity). An
        // instance drawn with any other material, or by BLAS sharing with clusters of several, tests alpha in any-hit
        // instead, which gives the same answer. Residency doesn't matter: a known micro-triangle's state is the test
        // mip's, and any-hit decides only the unknown ones, at whatever mip has streamed in.
        if (blas != 0xFFFFFFFFu) {
            RWStructuredBuffer<uint> blasOmmKeys = ResourceDescriptorHeap[CLOD_RT_SCENE_BLAS_OMM_KEYS_UAV];
            const uint ommKey = blasOmmKeys[blas];
            if (ommKey != 0u) {
                // A row whose clusters take several materials can't name the one its micromaps were baked for.
                const bool usable = CLOD_RT_SCENE_OMM_ACTIVE != 0u && (materialFlags & MATERIAL_ALPHA_TEST) != 0u
                    && rowMaterialCount == 1u && ommKey == materials[rowMaterial].ommIdentity;
                if (!usable) {
                    desc.contributionAndFlags |= RTAS_INSTANCE_FLAG_DISABLE_OMMS << 24u;
                    counters.InterlockedAdd(CLOD_RT_COUNTER_OMM_DISABLED_INSTANCES * 4u, 1u);
                }
                else {
                    counters.InterlockedAdd(CLOD_RT_COUNTER_OMM_INSTANCES * 4u, 1u);
                }
            }
        }
    }
    instances[id.x] = desc;
}
