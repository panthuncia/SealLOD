// The layered path's shade list: one thread per row of the strand pool gathers the rows the rasterizer marked this frame
// (hairShading.hlsli) into a list, so that the shade pass (hairShade.hlsl) runs one thread per vertex to light and its
// time follows the hair that is seen, not the pool. The marked rows are scattered through the pool: lit in place, most
// of the shade pass's thread waves ran with some lanes idle.
//
// A wave appends its marked rows with one atomic: the list's order is the pool's within a wave, and the waves' in
// whatever order they ran. The list is the frame's; the density volume's raymarch is to read the same rows.

#include "HairShaders/hairCommon.hlsli"
#include "HairShaders/hairShading.hlsli"

// HairShadeListPass::Record.
#define HAIR_FRAME_STATE UintRootConstant0    // StructuredBuffer<HairFrameGPU>
#define HAIR_COUNTERS UintRootConstant1       // RWStructuredBuffer<uint>
#define HAIR_POOL_COLORS UintRootConstant2    // StructuredBuffer<uint2>: the marks
#define HAIR_SHADE_LIST UintRootConstant3     // RWStructuredBuffer<uint>: pool rows
#define HAIR_GROUPS_PER_ROW UintRootConstant4 // the dispatch is this many groups wide

[shader("compute")]
[numthreads(64, 1, 1)]
void HairShadeListCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint row = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    if (row >= min(counters[HAIR_COUNTER_POOL_VERTICES], frame.poolVertexCapacity))
    {
        return;
    }
    StructuredBuffer<uint2> poolColors = ResourceDescriptorHeap[HAIR_POOL_COLORS];
    const bool marked = HairIsMarkedForShading(poolColors[row], frame.frameNumber);
    const uint waveCount = WaveActiveCountBits(marked);
    if (waveCount == 0u)
    {
        return;
    }
    const uint before = WavePrefixCountBits(marked);
    uint first = 0u;
    if (WaveIsFirstLane())
    {
        InterlockedAdd(counters[HAIR_COUNTER_SHADE_LIST], waveCount, first);
    }
    first = WaveReadLaneFirst(first);
    if (marked)
    {
        // The list has a row for every pool row, so it cannot overflow.
        RWStructuredBuffer<uint> shadeList = ResourceDescriptorHeap[HAIR_SHADE_LIST];
        shadeList[first + before] = row;
    }
}
