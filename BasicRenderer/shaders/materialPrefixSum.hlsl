#include "include/cbuffers.hlsli"

// Constants
static const uint K = 1024; // block size (must be power-of-two)
static const uint THREADS = 256;

// Shared memory
groupshared uint s_data[K]; // counts per block
groupshared uint s_scan[K]; // temporary for scan
groupshared uint s_blockSum; // per-block reduction result

// Utility: exclusive scan (Blelloch) over s_data[0..Nblock-1], result in s_scan[0..Nblock-1].
// Supports arbitrary Nblock by padding to next power-of-two (<= K).
void ExclusiveScanBlock(uint Nblock, uint threadId)
{
    if (Nblock == 0)
    {
        return;
    }

    // K must be a power-of-two and K >= Nblock
    // Compute next power-of-two >= Nblock (P <= K)
    uint P = 1;
    while (P < Nblock)
    {
        P <<= 1;
    }

    // Load s_data -> s_scan, pad [Nblock..P) with zeros
    for (uint i = threadId; i < Nblock; i += THREADS)
    {
        s_scan[i] = s_data[i];
    }
    for (uint i = threadId + Nblock; i < P; i += THREADS)
    {
        s_scan[i] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Upsweep (build sums) on range [0..P-1]
    for (uint stride = 1; stride < P; stride <<= 1)
    {
        uint idx = (threadId + 1) * (stride << 1) - 1;
        if (idx < P)
        {
            s_scan[idx] += s_scan[idx - stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // Set last element to 0 for exclusive scan
    if (threadId == 0)
    {
        s_scan[P - 1] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Downsweep on range [0..P-1]
    for (uint stride = (P >> 1); stride >= 1; stride >>= 1)
    {
        uint idx = (threadId + 1) * (stride << 1) - 1;
        if (idx < P)
        {
            uint t = s_scan[idx - stride];
            s_scan[idx - stride] = s_scan[idx];
            s_scan[idx] += t;
        }
        GroupMemoryBarrierWithGroupSync();

        // Avoid infinite loop when stride becomes 0
        if (stride == 1)
            break;
    }

    // Only s_scan[0..Nblock-1] are used by the caller.
}


// Per-block exclusive scan, output offsets and block sums
// Root constants:
// UintRootConstant0 = NumMaterials
[numthreads(THREADS, 1, 1)]
void BlockScanCS(uint3 groupThreadId : SV_GroupThreadID,
                 uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> counts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialPixelCountBuffer)];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialOffsetBuffer)];
    RWStructuredBuffer<uint> blockSums = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::BlockSumsBuffer)];

    uint N = UintRootConstant0;
    uint blockId = groupId.x;
    uint localTid = groupThreadId.x;
    uint base = blockId * K;

    // Load counts to LDS (pad with zeros up to K)
    for (uint i = localTid; i < K; i += THREADS)
    {
        uint globalIdx = base + i;
        s_data[i] = (globalIdx < N) ? counts[globalIdx] : 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // Number of valid elements in this block
    uint Nblock = 0;
    if (N > base)
        Nblock = min(K, N - base);

    // Exclusive scan per block (handles arbitrary Nblock)
    ExclusiveScanBlock(Nblock, localTid);

    // Write per-element local offsets (no block prefix yet)
    for (uint i = localTid; i < Nblock; i += THREADS)
    {
        offsets[base + i] = s_scan[i];
    }

    // Compute block sum = sum of counts in this block
    uint localSum = 0;
    for (uint i = localTid; i < Nblock; i += THREADS)
    {
        localSum += s_data[i];
    }

    // Accumulate to shared variable
    if (localTid == 0)
        s_blockSum = 0;
    GroupMemoryBarrierWithGroupSync();

    InterlockedAdd(s_blockSum, localSum);
    GroupMemoryBarrierWithGroupSync();

    // Write block sum
    if (localTid == 0)
    {
        uint numBlocks = (N + K - 1) / K;
        if (blockId < numBlocks)
        {
            blockSums[blockId] = s_blockSum;
        }
    }
}

// Scan block sums to get block prefixes, add to per-element offsets; also write total
// Root constants:
// UintRootConstant0 = NumMaterials (N)
// UintRootConstant1 = NumBlocks    (B)
[numthreads(THREADS, 1, 1)]
void BlockOffsetsCS(uint3 groupThreadId : SV_GroupThreadID,
                    uint3 groupId : SV_GroupID)
{
    // This pass is intended to run with exactly one thread group.
    // If dispatch is larger by mistake, ignore all but group 0.
    if (groupId.x != 0 || groupId.y != 0 || groupId.z != 0)
        return;

    StructuredBuffer<uint> counts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialPixelCountBuffer)];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::MaterialOffsetBuffer)];
    StructuredBuffer<uint> blockSums = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::BlockSumsBuffer)];
    RWStructuredBuffer<uint> scannedBlockSums = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::ScannedBlockSumsBuffer)];
    RWStructuredBuffer<uint> totalOut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TotalPixelCountBuffer)];

    uint N = UintRootConstant0; // total element count
    uint B = UintRootConstant1; // number of blocks (BlockSums length)

    uint localTid = groupThreadId.x;

    // exclusive scan on blockSums (length B, B <= K)

    // Load blockSums into s_data[0..B-1]
    for (uint i = localTid; i < B; i += THREADS)
    {
        s_data[i] = blockSums[i];
    }
    GroupMemoryBarrierWithGroupSync();

    // s_data now holds the block sums; scan into s_scan
    // ExclusiveScanBlock handles arbitrary B by padding to next power-of-two.
    ExclusiveScanBlock(B, localTid);

    // Write scanned block prefixes to global buffer
    for (uint i = localTid; i < B; i += THREADS)
    {
        scannedBlockSums[i] = s_scan[i];
    }
    GroupMemoryBarrierWithGroupSync();

    // add block prefix to each element's local offset

    // Each thread walks over elements in strides of THREADS
    for (uint elem = localTid; elem < N; elem += THREADS)
    {
        uint blockId = elem / K; // which block this element belongs to
        uint prefix = s_scan[blockId]; // block's prefix sum (from scanned block sums)
        offsets[elem] += prefix;
    }

    // total = sum(counts[0..N-1]) = scannedBlockSums[B-1] + blockSums[B-1]
    if (localTid == 0 && B > 0)
    {
        // s_data still holds original blockSums; s_scan holds their exclusive scan.
        uint total = s_scan[B - 1] + s_data[B - 1];
        totalOut[0] = total;
    }
}

[numthreads(THREADS, 1, 1)]
void TerrainRegionBlockScanCS(uint3 groupThreadId : SV_GroupThreadID,
                              uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> counts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionPixelCountBuffer)];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionOffsetBuffer)];
    RWStructuredBuffer<uint> blockSums = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionBlockSumsBuffer)];

    uint N = UintRootConstant0;
    uint blockId = groupId.x;
    uint localTid = groupThreadId.x;
    uint base = blockId * K;

    for (uint i = localTid; i < K; i += THREADS)
    {
        uint globalIdx = base + i;
        s_data[i] = (globalIdx < N) ? counts[globalIdx] : 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint Nblock = 0;
    if (N > base)
        Nblock = min(K, N - base);

    ExclusiveScanBlock(Nblock, localTid);

    for (uint i = localTid; i < Nblock; i += THREADS)
    {
        offsets[base + i] = s_scan[i];
    }

    uint localSum = 0;
    for (uint i = localTid; i < Nblock; i += THREADS)
    {
        localSum += s_data[i];
    }

    if (localTid == 0)
        s_blockSum = 0;
    GroupMemoryBarrierWithGroupSync();

    InterlockedAdd(s_blockSum, localSum);
    GroupMemoryBarrierWithGroupSync();

    if (localTid == 0)
    {
        uint numBlocks = (N + K - 1) / K;
        if (blockId < numBlocks)
        {
            blockSums[blockId] = s_blockSum;
        }
    }
}

[numthreads(THREADS, 1, 1)]
void TerrainRegionBlockOffsetsCS(uint3 groupThreadId : SV_GroupThreadID,
                                 uint3 groupId : SV_GroupID)
{
    if (groupId.x != 0 || groupId.y != 0 || groupId.z != 0)
        return;

    StructuredBuffer<uint> counts = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionPixelCountBuffer)];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionOffsetBuffer)];
    StructuredBuffer<uint> blockSums = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionBlockSumsBuffer)];
    RWStructuredBuffer<uint> scannedBlockSums = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionScannedBlockSumsBuffer)];
    RWStructuredBuffer<uint> totalOut = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionTotalPixelCountBuffer)];

    uint N = UintRootConstant0;
    uint B = UintRootConstant1;
    uint localTid = groupThreadId.x;

    for (uint i = localTid; i < B; i += THREADS)
    {
        s_data[i] = blockSums[i];
    }
    GroupMemoryBarrierWithGroupSync();

    ExclusiveScanBlock(B, localTid);

    for (uint i = localTid; i < B; i += THREADS)
    {
        scannedBlockSums[i] = s_scan[i];
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint elem = localTid; elem < N; elem += THREADS)
    {
        uint blockId = elem / K;
        offsets[elem] += s_scan[blockId];
    }

    if (localTid == 0 && B > 0)
    {
        totalOut[0] = s_scan[B - 1] + s_data[B - 1];
    }
}
