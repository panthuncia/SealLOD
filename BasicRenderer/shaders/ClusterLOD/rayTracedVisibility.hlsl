// Primary visibility by ray. Writes the same keys as the rasterizers, merged by depth, so material evaluation can't
// tell the difference and anything outside the BVH keeps rasterizing into the same buffer.
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/materialFlags.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"
#define CLOD_RT_INSTANCE_ROWS_SRV CLOD_RT_VIS_INSTANCE_ROWS_SRV
#include "include/clodRayTracingHits.hlsli"
#include "include/clodRayTracingCoverage.hlsli"
#include "PerPassRootConstants/clodRayTracingSetupRootConstants.h"

// A key needs a visible cluster index, but a hit only knows (draw, CLAS). Each pair gets a record the first time a ray
// hits it, and the record's index is simply its slot in this hash table, so a thread that loses the race to create it
// still knows the index without waiting. The region this maps to is far from both ends of the visible cluster buffer,
// where raster's lists grow from.
#define CLOD_RT_VIS_HASH_MAX_PROBES 32u
#define CLOD_RT_INVALID_RECORD 0xFFFFFFFFu

uint CLodRtHash(uint drawRecord, uint slot, uint transformIndex)
{
    uint hash = drawRecord * 0x9E3779B1u ^ slot * 0x85EBCA77u ^ transformIndex * 0x27D4EB2Fu;
    hash ^= hash >> 15u;
    hash *= 0xC2B2AE3Du;
    return hash ^ (hash >> 13u);
}

// The palette index culling would have put in the side band for this cluster.
uint CLodRtHitPaletteIndex(uint drawRecord, uint transformIndex, CLodRtClasInfo info)
{
    const PerMeshInstanceBuffer meshTemplate = LoadMeshTemplateForDrawRecord(LoadInstanceDrawRecord(drawRecord));
    if (!CLodTemplateHasPalette(meshTemplate)) {
        return 0u;
    }
    const uint pageOffset = info.pageOffsetUnits << CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT;
    const CLodPageHeader header = LoadPageHeader(info.slabDescriptor, pageOffset);
    const uint slot = CLodGroupPartHasMaterialSlots(CLodRtClasGroup(info))
        ? CLodLoadMeshletMaterialSlot(info.slabDescriptor, pageOffset, header.descriptorOffset, info.localMeshlet)
        : 0u;
    return CLodAssemblyMaterialBase(transformIndex) + slot;
}

// One record per (draw, CLAS, occurrence): a part's CLAS is shared by every portal occurrence that reached it.
uint CLodRtFindOrCreateRecord(uint drawRecord, uint slot, uint transformIndex, CLodRtClasInfo info)
{
    RWByteAddressBuffer table = ResourceDescriptorHeap[CLOD_RT_VIS_HASH_UAV];
    // [draw + 1 : 23][transform : 21][slot : 20]; the sentinel transform is the all-ones 21 bits.
    const uint64_t key = (uint64_t(drawRecord + 1u) << 41u) | (uint64_t(transformIndex & 0x1FFFFFu) << 20u) | uint64_t(slot & 0xFFFFFu);
    uint entry = CLodRtHash(drawRecord, slot, transformIndex) % CLOD_RT_VIS_HASH_ENTRIES;
    for (uint probe = 0u; probe < CLOD_RT_VIS_HASH_MAX_PROBES; ++probe) {
        uint64_t previous = 0ull;
        table.InterlockedCompareExchange64(entry * 8u, 0ull, key, previous);
        if (previous == 0ull || previous == key) {
            const uint record = CLOD_RT_VIS_RECORD_BASE + entry;
            if (previous == 0ull) {
                RWByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_RT_VIS_VISIBLE_CLUSTERS_UAV];
                RWStructuredBuffer<uint2> transformIndices = ResourceDescriptorHeap[CLOD_RT_VIS_TRANSFORM_INDICES_UAV];
                visibleClusters.Store4(record * 16u, CLodRtPackHitCluster(CLOD_RT_VIS_VIEW_ID, drawRecord, info));
                transformIndices[record] = uint2(transformIndex, CLodRtHitPaletteIndex(drawRecord, transformIndex, info));
            }
            return record;
        }
        entry = (entry + 1u) % CLOD_RT_VIS_HASH_ENTRIES;
    }
    return CLOD_RT_INVALID_RECORD;
}

void CLodRtAddStat(uint word)
{
    RWByteAddressBuffer stats = ResourceDescriptorHeap[CLOD_RT_VIS_STATS_UAV];
    stats.InterlockedAdd(word * 4u, 1u);
}

// Tells why a pixel disagrees: run with raster's keys still in the buffer, on the same frame's cameras and residency.
void CLodRtCompare(uint2 pixel, CLodRtHit hit, float hitDepth, CLodRtClasInfo info)
{
    RWTexture2D<uint64_t> visibility = ResourceDescriptorHeap[CLOD_RT_VIS_VISIBILITY_UAV];
    const uint64_t rasterKey = visibility[pixel];
    if (rasterKey == 0xFFFFFFFFFFFFFFFFull) {
        CLodRtAddStat(hit.hit ? CLOD_RT_STAT_RAY_HIT_RASTER_MISSED : CLOD_RT_STAT_BOTH_MISSED);
        return;
    }

    float rasterDepth = 0.0f;
    uint rasterCluster = 0u;
    uint rasterTriangle = 0u;
    UnpackVisKey(rasterKey, rasterDepth, rasterCluster, rasterTriangle);

    RWByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_RT_VIS_VISIBLE_CLUSTERS_UAV];
    const uint4 rasterRecord = visibleClusters.Load4(rasterCluster * 16u);
    if (rasterCluster >= CLOD_RT_VIS_CAPACITY || CLodVisibleClusterIsVoxel(rasterRecord)) {
        CLodRtAddStat(CLOD_RT_STAT_RASTER_NOT_COVERED);
        return;
    }
    const uint rasterDraw = CLodVisibleClusterInstanceID(rasterRecord);
    const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(rasterDraw);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const PerMeshBuffer perMesh = perMeshBuffer[LoadMeshTemplateForDrawRecord(drawRecord).perMeshBufferIndex];
    StructuredBuffer<CLodMeshMetadata> metadata = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    if (!CLodDrawIsRayTracingCovered(perMesh, metadata[LoadCLodOffsetsForDrawRecord(drawRecord).clodMeshMetadataIndex])) {
        CLodRtAddStat(CLOD_RT_STAT_RASTER_NOT_COVERED);
        return;
    }

    if (!hit.hit) {
        CLodRtAddStat(CLOD_RT_STAT_RAY_MISSED_RASTER_HIT);
        return;
    }

    if (abs(hitDepth - rasterDepth) <= max(1e-3f * rasterDepth, 0.01f)) {
        CLodRtAddStat(CLOD_RT_STAT_DEPTH_WITHIN_TOLERANCE);
    }
    else {
        CLodRtAddStat(hitDepth > rasterDepth ? CLOD_RT_STAT_RAY_HIT_FARTHER : CLOD_RT_STAT_RAY_HIT_NEARER);
    }

    if (rasterDraw != hit.drawRecord) {
        CLodRtAddStat(CLOD_RT_STAT_DIFFERENT_INSTANCE);
        StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
        const uint rasterMaterialFlags = materials[perMesh.materialDataIndex].materialFlags;
        if ((rasterMaterialFlags & MATERIAL_ALPHA_TEST) != 0u) {
            CLodRtAddStat(CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_ALPHA);
        }
        if ((rasterMaterialFlags & MATERIAL_DOUBLE_SIDED) != 0u) {
            CLodRtAddStat(CLOD_RT_STAT_DIFFERENT_INSTANCE_RASTER_DOUBLE_SIDED);
        }
    }
    else if (CLodVisibleClusterLocalMeshletIndex(rasterRecord) != info.localMeshlet
        || CLodVisibleClusterPageSlabDescriptorIndex(rasterRecord) != info.slabDescriptor
        || (CLodVisibleClusterPageSlabByteOffset(rasterRecord) >> CLOD_PACKED_VISIBLE_CLUSTER_PAGE_OFFSET_SHIFT) != info.pageOffsetUnits) {
        CLodRtAddStat(CLOD_RT_STAT_DIFFERENT_CLUSTER);
    }
    else if (rasterTriangle != hit.primitive) {
        CLodRtAddStat(CLOD_RT_STAT_DIFFERENT_TRIANGLE);
    }
    else {
        CLodRtAddStat(CLOD_RT_STAT_BOTH_HIT_MATCH);
    }
}

[numthreads(8, 8, 1)]
void CLodRtVisibilityCS(uint3 id : SV_DispatchThreadID)
{
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    const float2 resolution = float2(perFrame.screenResX, perFrame.screenResY);
    if (id.x >= perFrame.screenResX || id.y >= perFrame.screenResY) {
        return;
    }
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    const Camera camera = cameras[perFrame.mainCameraIndex];

    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[CLOD_RT_VIS_TLAS_SRV];
    StructuredBuffer<CLodRtClasInfo> clasInfo = ResourceDescriptorHeap[CLOD_RT_VIS_CLAS_INFO_SRV];

    const RayDesc ray = CLodRtCameraRay(camera, id.xy, resolution);
    const CLodRtHit hit = CLodRtTrace(scene, clasInfo, ray, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFFu, CLodRtPixelConeSpread(camera, resolution.y));

    CLodRtClasInfo info = (CLodRtClasInfo)0;
    float depth = 0.0f;
    if (hit.hit) {
        info = clasInfo[hit.clasSlot];
        // Clip W, which is what the rasterizers store.
        depth = -mul(float4(ray.Origin + ray.Direction * hit.t, 1.0f), camera.view).z;
    }

    if (CLOD_RT_VIS_MODE == CLOD_RT_VIS_MODE_COMPARE) {
        CLodRtCompare(id.xy, hit, depth, info);
        return;
    }
    if (!hit.hit) {
        return;
    }

    const uint record = CLodRtFindOrCreateRecord(hit.drawRecord, hit.clasSlot, hit.transformIndex, info);
    if (record == CLOD_RT_INVALID_RECORD) {
        return;
    }
    RWTexture2D<uint64_t> visibility = ResourceDescriptorHeap[CLOD_RT_VIS_VISIBILITY_UAV];
    InterlockedMin(visibility[id.xy], PackVisKey(depth, record, hit.primitive));
}
