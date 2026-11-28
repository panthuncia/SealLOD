// The hair density volumes of a frame (HairVolumeGPU, HairRuntimeGPUTypes.h): one box of voxels per instance a view
// shows, planned by the CPU, filled here from the frame's strand pool and read by the shade pass's marches
// (hairVolume.hlsli) for self-shadowing and multiple scattering.
//
// HairVolumeClearCS zeroes the voxels the frame's volumes use. HairVoxelizeCS runs one thread per pool row, like the
// rasterizer: a row that starts a segment deposits it into its instance's volume.
//
// The pool, not the hair visibility target: the pool holds every strand the frame selected, in view or hidden behind
// other hair, at the frame's level of detail and where the simulation left it. What a segment deposits is its length
// times its diameter (HairVolumeSegmentDeposit), and the pool's diameters carry the LOD's width scale, so the density
// does not change with the strands a groom is drawn with.
//
// A deposit is spread over the eight voxels around it by its distance to their centres (HAIR_VOLUME_FLAG_TRILINEAR), so
// the density is smooth across voxels and hair moving through the grid changes it gradually; the marches then read
// single voxels. Voxels take whole units: a share is rounded up or down at random in proportion, by a number that is
// the same in every frame for the same row.
//
// The scalp. The volume holds hair only, and what hair grows on is opaque: light from the far side of a head does not
// reach the hair on the near side. A strand's first segment marks the voxels under its root, from
// HAIR_VOLUME_SCALP_OFFSET voxels along the strand's direction into the scalp and HairFrameGPU::volumeScalpDepth voxels
// on: a shell under the skin wherever hair grows, which ends any march that reads it. It starts far enough down that no
// voxel a strand vertex can be in is marked.

#include "HairShaders/hairCommon.hlsli"
#include "HairShaders/hairVolume.hlsli"

// HairVoxelizePass::Record.
#define HAIR_FRAME_STATE UintRootConstant0       // StructuredBuffer<HairFrameGPU>
#define HAIR_COUNTERS UintRootConstant1          // RWStructuredBuffer<uint>
#define HAIR_VISIBLE_CLUSTERS UintRootConstant2  // StructuredBuffer<HairVisibleCluster>
#define HAIR_POOL_VERTICES UintRootConstant3     // StructuredBuffer<HairPoolVertex>
#define HAIR_POOL_SEGMENT_IDS UintRootConstant4  // StructuredBuffer<uint>
#define HAIR_VOLUMES UintRootConstant5           // StructuredBuffer<HairVolumeGPU>
#define HAIR_VOXELS UintRootConstant6            // RWStructuredBuffer<uint>
#define HAIR_GROUPS_PER_ROW UintRootConstant7    // the dispatch is this many groups wide
#define HAIR_VOXEL_COUNT UintRootConstant8       // the clear's: the voxels the frame's volumes use

[shader("compute")]
[numthreads(64, 1, 1)]
void HairVolumeClearCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    const uint index = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    if (index < HAIR_VOXEL_COUNT)
    {
        RWStructuredBuffer<uint> voxels = ResourceDescriptorHeap[HAIR_VOXELS];
        voxels[index] = 0u;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void HairVoxelizeCS(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    StructuredBuffer<HairFrameGPU> frameState = ResourceDescriptorHeap[HAIR_FRAME_STATE];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[HAIR_COUNTERS];
    const HairFrameGPU frame = frameState[0];
    const uint row = (groupId.y * HAIR_GROUPS_PER_ROW + groupId.x) * 64u + groupIndex;
    const uint poolVertexCount = min(counters[HAIR_COUNTER_POOL_VERTICES], frame.poolVertexCapacity);
    if (row + 1u >= poolVertexCount)
    {
        return;
    }
    StructuredBuffer<uint> poolSegmentIds = ResourceDescriptorHeap[HAIR_POOL_SEGMENT_IDS];
    const uint segmentId = poolSegmentIds[row];
    if (segmentId == HAIR_INVALID_INDEX)
    {
        return;
    }
    // As in the rasterizer: a row is this frame's when the visible cluster its id names owns it.
    StructuredBuffer<HairVisibleCluster> visibleClusters = ResourceDescriptorHeap[HAIR_VISIBLE_CLUSTERS];
    const uint visibleIndex = segmentId >> HAIR_VIS_CLUSTER_SHIFT;
    if (visibleIndex >= min(counters[HAIR_COUNTER_CLUSTERS_VISIBLE], frame.visibleClusterCapacity))
    {
        return;
    }
    const HairVisibleCluster visible = visibleClusters[visibleIndex];
    const uint clusterVertexCount = visible.strandCount * ((1u << visible.vertexLevel) + 1u);
    if (row < visible.poolVertexBase || row >= visible.poolVertexBase + clusterVertexCount || visible.instanceIndex >= frame.volumeInstanceCount)
    {
        return;
    }
    StructuredBuffer<HairVolumeGPU> volumes = ResourceDescriptorHeap[HAIR_VOLUMES];
    const HairVolumeGPU volume = volumes[visible.instanceIndex];
    if (!HairVolumeExists(volume))
    {
        return;
    }

    StructuredBuffer<HairPoolVertex> poolVertices = ResourceDescriptorHeap[HAIR_POOL_VERTICES];
    RWStructuredBuffer<uint> voxels = ResourceDescriptorHeap[HAIR_VOXELS];
    const HairPoolVertex start = poolVertices[row];
    const HairPoolVertex end = poolVertices[row + 1u];
    const float3 grid0 = HairVolumeGridPosition(volume, start.position);
    const float3 grid1 = HairVolumeGridPosition(volume, end.position);
    const float segmentLength = distance(start.position, end.position);
    // A hidden cluster is in the pool with a fraction of its strands at their own width (hairSelectEvaluate.hlsl): each
    // deposits for the ones left out, so the density is the whole cluster's.
    const float diameter = 0.5f * (start.width + end.width) * visible.densityScale;

    // The scalp shell under a strand's root.
    const uint segmentIndex = segmentId & ((1u << HAIR_VIS_SEGMENT_BITS) - 1u);
    if (segmentIndex == 0u && frame.volumeScalpDepth != 0u && segmentLength > 0.0f)
    {
        const float3 inward = normalize(grid0 - grid1);
        for (uint depth = 0u; depth < frame.volumeScalpDepth; ++depth)
        {
            const int3 voxel = (int3)floor(grid0 + inward * (HAIR_VOLUME_SCALP_OFFSET + (float)depth));
            if (HairVolumeContains(volume, voxel))
            {
                InterlockedOr(voxels[HairVolumeVoxelIndex(volume, (uint3)voxel)], HAIR_VOLUME_SCALP_BIT);
            }
        }
    }

    uint adds = 0u;
    const bool deposits = segmentLength > 0.0f && diameter > 0.0f;
    if (deposits)
    {
        const uint count = HairVolumeDepositCount(segmentLength / volume.voxelSize);
        const float amount = HairVolumeSegmentDeposit(segmentLength, diameter, volume.voxelSize) / (float)count;
        for (uint deposit = 0u; deposit < count; ++deposit)
        {
            const float3 at = lerp(grid0, grid1, ((float)deposit + 0.5f) / (float)count);
            if ((frame.volumeFlags & HAIR_VOLUME_FLAG_TRILINEAR) != 0u)
            {
                // Between the centres of the eight voxels around it.
                const float3 centred = at - 0.5f;
                const float3 low = floor(centred);
                const float3 share = centred - low;
                for (uint corner = 0u; corner < 8u; ++corner)
                {
                    const int3 voxel = (int3)low + int3(corner & 1u, (corner >> 1u) & 1u, corner >> 2u);
                    const uint units = HairVolumeRound(amount * HairVolumeCornerWeight(share.x, share.y, share.z, corner),
                        HairVolumeHash(row * 32u + deposit * 8u + corner));
                    if (units != 0u && HairVolumeContains(volume, voxel))
                    {
                        InterlockedAdd(voxels[HairVolumeVoxelIndex(volume, (uint3)voxel)], units);
                        ++adds;
                    }
                }
            }
            else
            {
                const int3 voxel = (int3)floor(at);
                const uint units = HairVolumeRound(amount, HairVolumeHash(row * 32u + deposit * 8u));
                if (units != 0u && HairVolumeContains(volume, voxel))
                {
                    InterlockedAdd(voxels[HairVolumeVoxelIndex(volume, (uint3)voxel)], units);
                    ++adds;
                }
            }
        }
    }

    const bool outside = !HairVolumeContains(volume, (int3)floor(0.5f * (grid0 + grid1)));
    const uint segmentCount = WaveActiveCountBits(deposits);
    const uint addCount = WaveActiveSum(adds);
    const uint outsideCount = WaveActiveCountBits(outside);
    if (WaveIsFirstLane())
    {
        if (segmentCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_VOLUME_SEGMENTS], segmentCount);
        if (addCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_VOLUME_DEPOSITS], addCount);
        if (outsideCount != 0u) InterlockedAdd(counters[HAIR_COUNTER_VOLUME_OUTSIDE], outsideCount);
    }
}
