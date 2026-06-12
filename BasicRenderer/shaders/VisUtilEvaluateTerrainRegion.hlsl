#define TERRAIN_REGION_GROUPSHARED_WEIGHTS 1
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/visUtilCommon.hlsli"
#include "gbuffer.hlsl"

[shader("compute")]
[numthreads(MATERIAL_EXECUTION_GROUP_SIZE, 1, 1)]
void EvaluateTerrainRegionMaterialGroupCS(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint groupIndex : SV_GroupIndex)
{
    uint terrainSetIndex = IndirectCommandSignatureRootConstant0;
    uint regionIndex = IndirectCommandSignatureRootConstant1;
    uint baseOffset = IndirectCommandSignatureRootConstant2;
    uint count = IndirectCommandSignatureRootConstant3;
    uint dispatchXDimension = IndirectCommandSignatureRootConstant4;

    StructuredBuffer<TerrainSetInfo> terrainSets = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Sets)];
    StructuredBuffer<TerrainRegionInfo> terrainRegions = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::Regions)];
    StructuredBuffer<uint> terrainWeightBlocks = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Terrain::WeightBlocks)];
    TerrainLoadRegionWeightBlocksToShared(terrainSets, terrainRegions, terrainWeightBlocks, terrainSetIndex, regionIndex, groupIndex);

    uint idx = dispatchThreadId.y * dispatchXDimension + dispatchThreadId.x;
    if (idx >= count)
    {
        return;
    }

    StructuredBuffer<PixelRef> pixelList = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::VisUtil::TerrainRegionPixelListBuffer)];
    PixelRef ref = pixelList[baseOffset + idx];

    uint2 pixel;
    pixel.x = ref.pixelXY & 0xFFFFu;
    pixel.y = ref.pixelXY >> 16;

    EvaluateGBufferOptimized(pixel);
}
