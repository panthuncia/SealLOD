#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/reyesPatchCommon.hlsli"
#include "PerPassRootConstants/clodReyesBuildRasterWorkRootConstants.h"

static const uint REYES_BUILD_RASTER_WORK_GROUP_SIZE = 64u;

[shader("compute")]
[numthreads(REYES_BUILD_RASTER_WORK_GROUP_SIZE, 1, 1)]
void BuildReyesRasterWorkCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> diceQueueCounter = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    const uint diceCount = diceQueueCounter[0];
    uint queueReadOffset = 0u;
    if (CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> readOffsetBuffer = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_READ_OFFSET_DESCRIPTOR_INDEX];
        queueReadOffset = readOffsetBuffer.Load(0);
    }

    const uint diceIndex = queueReadOffset + dispatchThreadId.x;
    if (diceIndex >= diceCount)
    {
        return;
    }

    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_DICE_QUEUE_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesRasterWorkEntry> rasterWorkBuffer = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> rasterWorkCounter = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_OUTPUT_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_TELEMETRY_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];

    const CLodReyesDiceQueueEntry diceEntry = diceQueue[diceIndex];
    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(diceEntry.instanceID);
    const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
    const uint microTriangleCount = ReyesGetDicePatchMicroTriangleCount(tessTableConfigs, diceEntry);
    if (microTriangleCount == 0u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterZeroMicroTriangleCount, 1u);
        return;
    }

    const uint rasterBatchCount = (microTriangleCount + CLodReyesRasterBatchMicroTriangleCount - 1u) / CLodReyesRasterBatchMicroTriangleCount;
    uint firstRasterWorkIndex = 0u;
    InterlockedAdd(rasterWorkCounter[0], rasterBatchCount, firstRasterWorkIndex);

    if (firstRasterWorkIndex >= CLOD_REYES_BUILD_RASTER_WORK_CAPACITY)
    {
        InterlockedAdd(telemetryBuffer[0].rasterWorkOverflowPatchCount, 1u);
        InterlockedAdd(telemetryBuffer[0].rasterWorkOverflowBatchCount, rasterBatchCount);
        return;
    }

    const uint availableRasterWorkCount = min(rasterBatchCount, CLOD_REYES_BUILD_RASTER_WORK_CAPACITY - firstRasterWorkIndex);
    if (availableRasterWorkCount < rasterBatchCount)
    {
        InterlockedAdd(telemetryBuffer[0].rasterWorkOverflowPatchCount, 1u);
        InterlockedAdd(telemetryBuffer[0].rasterWorkOverflowBatchCount, rasterBatchCount - availableRasterWorkCount);
    }

    uint emittedMicroTriangleCount = 0u;
    [loop]
    for (uint batchIndex = 0u; batchIndex < availableRasterWorkCount; ++batchIndex)
    {
        CLodReyesRasterWorkEntry workEntry;
        workEntry.diceQueueIndex = diceIndex;
        workEntry.microTriangleOffset = batchIndex * CLodReyesRasterBatchMicroTriangleCount;
        workEntry.microTriangleCount = min(CLodReyesRasterBatchMicroTriangleCount, microTriangleCount - workEntry.microTriangleOffset);
        workEntry.rasterBucketIndex = perMesh.rasterBucketIndex;
        rasterWorkBuffer[firstRasterWorkIndex + batchIndex] = workEntry;
        emittedMicroTriangleCount += workEntry.microTriangleCount;
    }

    if (availableRasterWorkCount > 0u)
    {
        InterlockedAdd(telemetryBuffer[0].patchRasterizedPatchCount, 1u);
        InterlockedAdd(telemetryBuffer[0].patchRasterizedMicroTriangleCount, emittedMicroTriangleCount);
        InterlockedAdd(telemetryBuffer[0].rasterWorkEntryCount, availableRasterWorkCount);
    }
}
