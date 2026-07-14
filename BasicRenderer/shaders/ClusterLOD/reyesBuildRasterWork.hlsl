#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/vertexFlags.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodResolveCommon.hlsli"
#include "include/reyesPatchCommon.hlsli"
#include "include/occlusionCulling.hlsli"
#include "PerPassRootConstants/clodReyesBuildRasterWorkRootConstants.h"

static const uint REYES_BUILD_RASTER_WORK_GROUP_SIZE = 64u;

uint3 ReyesBuildRasterDecodeTriangle(ByteAddressBuffer slab, uint triStreamBase, uint triByteOffset, uint triLocalIndex)
{
    uint triOffset = triStreamBase + triByteOffset + triLocalIndex * 3u;
    uint alignedOffset = (triOffset / 4u) * 4u;
    uint firstWord = slab.Load(alignedOffset);
    uint byteOffset = triOffset % 4u;

    uint b0 = (firstWord >> (byteOffset * 8u)) & 0xFFu;
    uint b1;
    uint b2;

    if (byteOffset <= 1u)
    {
        b1 = (firstWord >> ((byteOffset + 1u) * 8u)) & 0xFFu;
        b2 = (firstWord >> ((byteOffset + 2u) * 8u)) & 0xFFu;
    }
    else if (byteOffset == 2u)
    {
        b1 = (firstWord >> 24u) & 0xFFu;
        uint secondWord = slab.Load(alignedOffset + 4u);
        b2 = secondWord & 0xFFu;
    }
    else
    {
        uint secondWord = slab.Load(alignedOffset + 4u);
        b1 = secondWord & 0xFFu;
        b2 = (secondWord >> 8u) & 0xFFu;
    }

    return uint3(b0, b1, b2);
}

SkinningInfluences ReyesBuildRasterDecodePackedJoints(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex)
{
    SkinningInfluences skinning = (SkinningInfluences)0;
    if ((hdr.attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    uint addr = pageByteOffset + hdr.jointArrayOffset + (desc.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.joints0 = LoadUint4(addr, slab);
    skinning.joints1 = LoadUint4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences ReyesBuildRasterDecodePackedWeights(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex,
    SkinningInfluences skinning)
{
    if ((hdr.attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    uint addr = pageByteOffset + hdr.weightArrayOffset + (desc.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.weights0 = LoadFloat4(addr, slab);
    skinning.weights1 = LoadFloat4(addr + 16u, slab);
    return skinning;
}

float3 ReyesBuildRasterDecodeSkinnedPosition(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex,
    uint vertexFlags,
    uint skinningInstanceSlot,
    CLodMeshMetadata metadata,
    uint assemblyTransformIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pagePoolSlabDescriptorIndex)];
    float3 localPos = CLodLoadPagePosition(
        slab,
        hdr.compressedPositionQuantExp,
        pageByteOffset + hdr.positionBitstreamOffset,
        desc.positionBitOffset,
        meshletLocalVertex);

    if ((vertexFlags & VERTEX_SKINNED) != 0u)
    {
        SkinningInfluences skinning = ReyesBuildRasterDecodePackedJoints(meshletLocalVertex, hdr, desc, pageByteOffset, pagePoolSlabDescriptorIndex);
        skinning = ReyesBuildRasterDecodePackedWeights(meshletLocalVertex, hdr, desc, pageByteOffset, pagePoolSlabDescriptorIndex, skinning);
        skinning = ResolveAssemblySkinningInfluences(skinning, metadata, assemblyTransformIndex);
        localPos = ApplyAssemblySkinningToPosition(
            skinningInstanceSlot, skinning, assemblyTransformIndex, localPos);
    }

    return localPos;
}

float ReyesBuildRasterMaxAxisScale_RowVector(row_major matrix m)
{
    const float3 row0 = float3(m._11, m._12, m._13);
    const float3 row1 = float3(m._21, m._22, m._23);
    const float3 row2 = float3(m._31, m._32, m._33);
    return sqrt(max(dot(row0, row0), max(dot(row1, row1), dot(row2, row2))));
}

void ReyesBuildRasterPatchObjectSphere(
    float3 p0,
    float3 p1,
    float3 p2,
    float displacementMagnitude,
    out float3 centerObject,
    out float radiusObject)
{
    centerObject = (p0 + p1 + p2) / 3.0f;
    radiusObject = max(distance(centerObject, p0), max(distance(centerObject, p1), distance(centerObject, p2))) + displacementMagnitude;
}

void ReyesBuildRasterPatchViewAABB(
    float3 p0,
    float3 p1,
    float3 p2,
    float displacementMagnitude,
    row_major matrix modelMatrix,
    row_major matrix viewMatrix,
    out float3 viewMin,
    out float3 viewMax)
{
    const float3 objectMin = min(p0, min(p1, p2)) - displacementMagnitude.xxx;
    const float3 objectMax = max(p0, max(p1, p2)) + displacementMagnitude.xxx;
    viewMin = 1.0e30f.xxx;
    viewMax = -1.0e30f.xxx;

    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        const float3 objectCorner = float3(
            (cornerIndex & 0x1u) != 0u ? objectMax.x : objectMin.x,
            (cornerIndex & 0x2u) != 0u ? objectMax.y : objectMin.y,
            (cornerIndex & 0x4u) != 0u ? objectMax.z : objectMin.z);
        const float3 viewCorner = mul(mul(float4(objectCorner, 1.0f), modelMatrix), viewMatrix).xyz;
        viewMin = min(viewMin, viewCorner);
        viewMax = max(viewMax, viewCorner);
    }
}

float3 ReyesBuildRasterInterpolateTriangle(float3 p0, float3 p1, float3 p2, float3 barycentrics)
{
    precise float3 result = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
    return result;
}

bool ReyesBuildRasterHZBOccluded(
    CLodReyesDiceQueueEntry diceEntry,
    float3 sourcePosition0,
    float3 sourcePosition1,
    float3 sourcePosition2,
    float displacementMagnitude,
    PerObjectBuffer objectData,
    Camera camera,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer)
{
    if (CLOD_REYES_BUILD_RASTER_WORK_ENABLE_PATCH_OCCLUSION == 0u ||
        CLOD_REYES_BUILD_RASTER_WORK_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return false;
    }

    StructuredBuffer<CLodViewDepthSRVIndex> viewDepthSRVIndices =
        ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_VIEW_DEPTH_SRV_INDICES_DESCRIPTOR_INDEX];
    const uint depthMapDescriptorIndex = viewDepthSRVIndices[diceEntry.viewID].linearDepthSRVIndex;
    if (depthMapDescriptorIndex == 0u)
    {
        return false;
    }

    const float3 domain0 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex0UV);
    const float3 domain1 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex1UV);
    const float3 domain2 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex2UV);
    if (!ReyesPatchDomainHasValidSimplex(domain0, domain1, domain2))
    {
        return false;
    }

    const float3 patchPosition0 = ReyesBuildRasterInterpolateTriangle(sourcePosition0, sourcePosition1, sourcePosition2, domain0);
    const float3 patchPosition1 = ReyesBuildRasterInterpolateTriangle(sourcePosition0, sourcePosition1, sourcePosition2, domain1);
    const float3 patchPosition2 = ReyesBuildRasterInterpolateTriangle(sourcePosition0, sourcePosition1, sourcePosition2, domain2);

    float3 objectCenter = 0.0f.xxx;
    float objectRadius = 0.0f;
    ReyesBuildRasterPatchObjectSphere(patchPosition0, patchPosition1, patchPosition2, displacementMagnitude, objectCenter, objectRadius);
    if (!all(isfinite(objectCenter)) || !isfinite(objectRadius) || objectRadius <= 0.0f)
    {
        return false;
    }

    const bool phaseOne = CLOD_REYES_BUILD_RASTER_WORK_PHASE_INDEX == 1u;
    row_major matrix modelMatrix = objectData.model;
    row_major matrix viewMatrix = camera.view;
    row_major matrix projectionMatrix = camera.projection;
    if (phaseOne)
    {
        modelMatrix = objectData.prevModel;
        viewMatrix = camera.prevView;
        projectionMatrix = camera.prevUnjitteredProjection;
    }
    const float scaledRadius = objectRadius * ReyesBuildRasterMaxAxisScale_RowVector(modelMatrix);
    const float3 viewSpaceCenter = mul(mul(float4(objectCenter, 1.0f), modelMatrix), viewMatrix).xyz;
    const float boundingSphereDepth = -viewSpaceCenter.z;
    if (!all(isfinite(viewSpaceCenter)) || !isfinite(scaledRadius) || boundingSphereDepth <= scaledRadius)
    {
        return false;
    }

    bool occluded = false;
    InterlockedAdd(telemetryBuffer[0].diceOcclusionTestCount, 1u);
    if (CLOD_REYES_BUILD_RASTER_WORK_USE_AABB_OCCLUSION != 0u)
    {
        float3 viewMin = 0.0f.xxx;
        float3 viewMax = 0.0f.xxx;
        ReyesBuildRasterPatchViewAABB(
            patchPosition0,
            patchPosition1,
            patchPosition2,
            displacementMagnitude,
            modelMatrix,
            viewMatrix,
            viewMin,
            viewMax);
        OcclusionCullingPerspectiveViewAABBTexture2D(
            occluded,
            uint2(camera.depthResX, camera.depthResY),
            camera.numDepthMips,
            projectionMatrix,
            viewMin,
            viewMax,
            depthMapDescriptorIndex);
    }
    else
    {
        OcclusionCullingPerspectiveTexture2D(
            occluded,
            uint2(camera.depthResX, camera.depthResY),
            camera.numDepthMips,
            camera.UVScaleToNextPowerOf2,
            projectionMatrix,
            viewSpaceCenter,
            boundingSphereDepth,
            scaledRadius,
            depthMapDescriptorIndex);
    }
    return occluded;
}

bool ReyesBuildRasterReplayOrDropDice(
    CLodReyesDiceQueueEntry diceEntry,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer)
{
    if (CLOD_REYES_BUILD_RASTER_WORK_PHASE_INDEX == 1u &&
        CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_DESCRIPTOR_INDEX != 0xFFFFFFFFu &&
        CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX != 0xFFFFFFFFu &&
        CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<CLodReyesDiceQueueEntry> replayDiceQueue =
            ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_DESCRIPTOR_INDEX];
        RWStructuredBuffer<uint> replayDiceCounter =
            ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
        RWStructuredBuffer<uint> replayDiceOverflow =
            ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_OVERFLOW_DESCRIPTOR_INDEX];

        uint replayIndex = 0u;
        InterlockedAdd(replayDiceCounter[0], 1u, replayIndex);
        if (replayIndex < CLOD_REYES_BUILD_RASTER_WORK_REPLAY_DICE_QUEUE_CAPACITY)
        {
            replayDiceQueue[replayIndex] = diceEntry;
            InterlockedAdd(telemetryBuffer[0].diceOcclusionDeferCount, 1u);
            return true;
        }

        InterlockedAdd(replayDiceOverflow[0], 1u);
        InterlockedAdd(telemetryBuffer[0].replayDiceQueueOverflowCount, 1u);
        return true;
    }

    InterlockedAdd(telemetryBuffer[0].diceOcclusionDropCount, 1u);
    return true;
}

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
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];

    const CLodReyesDiceQueueEntry diceEntry = diceQueue[diceIndex];
    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(diceEntry.instanceID);
    const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
    const MeshInstanceClodOffsets clodOffsets = LoadCLodOffsetsForDraw(diceEntry.instanceID);
    StructuredBuffer<CLodMeshMetadata> clodMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata clodMetadata = clodMetadataBuffer[clodOffsets.clodMeshMetadataIndex];
    uint assemblyTransformIndex = CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
    if (CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        StructuredBuffer<uint> visibleClusterTransformIndices = ResourceDescriptorHeap[
            CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
        assemblyTransformIndex = visibleClusterTransformIndices[diceEntry.visibleClusterIndex];
    }
    if ((CLOD_REYES_BUILD_RASTER_WORK_ENABLE_PATCH_OCCLUSION != 0u ||
         CLOD_REYES_BUILD_RASTER_WORK_TERRAIN_RVT_ENABLED != 0u) &&
        CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_REYES_BUILD_RASTER_WORK_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
        const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, diceEntry.visibleClusterIndex);
        const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
        const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
        const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
        const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
        const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, localMeshletIndex);
        const uint sourceTriangleIndex = diceEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
        if (sourceTriangleIndex < CLodDescTriangleCount(meshletDesc))
        {
            ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
            const uint3 sourceTriangle = ReyesBuildRasterDecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, meshletDesc.triangleByteOffset, sourceTriangleIndex);
            const float3 sourcePosition0 = ReyesBuildRasterDecodeSkinnedPosition(sourceTriangle.x, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot, clodMetadata, assemblyTransformIndex);
            const float3 sourcePosition1 = ReyesBuildRasterDecodeSkinnedPosition(sourceTriangle.y, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot, clodMetadata, assemblyTransformIndex);
            const float3 sourcePosition2 = ReyesBuildRasterDecodeSkinnedPosition(sourceTriangle.z, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot, clodMetadata, assemblyTransformIndex);
            StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
            const MaterialInfo materialInfo = materials[perMesh.materialDataIndex];
            const float displacementMagnitude = ReyesGeometricDisplacementMagnitude(materialInfo);
            const PerObjectBuffer objectData = LoadInstanceTransformForDrawWithAssemblyTransform(
                diceEntry.instanceID, assemblyTransformIndex);

            if (CLOD_REYES_BUILD_RASTER_WORK_ENABLE_PATCH_OCCLUSION != 0u)
            {
                const Camera camera = cameras[diceEntry.viewID];
                if (ReyesBuildRasterHZBOccluded(
                        diceEntry,
                        sourcePosition0,
                        sourcePosition1,
                        sourcePosition2,
                        displacementMagnitude,
                        objectData,
                        camera,
                        telemetryBuffer))
                {
                    ReyesBuildRasterReplayOrDropDice(diceEntry, telemetryBuffer);
                    return;
                }
            }
        }
    }
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
