#include "PerPassRootConstants/clodReyesDeepVisibilityRasterRootConstants.h"

#define ReyesPatchRasterCS ReyesPatchRasterOpaqueUnusedCS
#include "ClusterLOD/reyesPatchRaster.hlsl"
#undef ReyesPatchRasterCS

static const uint CLOD_DEEP_VISIBILITY_LIST_NULL = 0xffffffffu;

void ReyesTryWriteDeepVisibilityNode(
    Texture2D<uint64_t> opaqueVisibility,
    RWTexture2D<uint> deepVisibilityHeadPointers,
    RWStructuredBuffer<CLodDeepVisibilityNode> deepVisibilityNodes,
    RWStructuredBuffer<uint> deepVisibilityNodeCounter,
    RWStructuredBuffer<uint> deepVisibilityOverflowCounter,
    uint nodeCapacity,
    uint2 pixel,
    float depth,
    uint patchVisibilityIndex,
    uint microTriangleIndex,
    bool isBackface)
{
    uint64_t opaqueVisKey = opaqueVisibility[pixel];
    if (opaqueVisKey != 0xFFFFFFFFFFFFFFFF)
    {
        float opaqueDepth;
        uint opaqueClusterIndex;
        uint opaquePrimID;
        UnpackVisKey(opaqueVisKey, opaqueDepth, opaqueClusterIndex, opaquePrimID);
        if (depth >= opaqueDepth)
        {
            return;
        }
    }

    uint nodeIndex;
    InterlockedAdd(deepVisibilityNodeCounter[0], 1u, nodeIndex);
    if (nodeIndex >= nodeCapacity)
    {
        uint ignored;
        InterlockedAdd(deepVisibilityOverflowCounter[0], 1u, ignored);
        return;
    }

    uint previousHead;
    InterlockedExchange(deepVisibilityHeadPointers[pixel], nodeIndex, previousHead);

    CLodDeepVisibilityNode node;
    node.visKey = PackVisKey(depth, patchVisibilityIndex, microTriangleIndex);
    node.next = previousHead;
    node.flags = isBackface ? 1u : 0u;
    deepVisibilityNodes[nodeIndex] = node;
}

bool ReyesTryRasterizeTinyProjectedDeepVisibilityMicroTriangle(
    Texture2D<uint64_t> opaqueVisibility,
    RWTexture2D<uint> deepVisibilityHeadPointers,
    RWStructuredBuffer<CLodDeepVisibilityNode> deepVisibilityNodes,
    RWStructuredBuffer<uint> deepVisibilityNodeCounter,
    RWStructuredBuffer<uint> deepVisibilityOverflowCounter,
    uint nodeCapacity,
    uint2 headPointerDims,
    ClodViewRasterInfo viewRasterInfo,
    float2 s0,
    float2 s1,
    float2 s2,
    float depth0,
    float depth1,
    float depth2,
    uint patchVisibilityIndex,
    uint microTriangleIndex,
    bool isBackface)
{
    const float2 bbMinF = min(min(s0, s1), s2);
    const float2 bbMaxF = max(max(s0, s1), s2);
    const float2 bbExtent = bbMaxF - bbMinF;
    if (bbExtent.x > REYES_PATCH_RASTER_TINY_TRIANGLE_MAX_EXTENT ||
        bbExtent.y > REYES_PATCH_RASTER_TINY_TRIANGLE_MAX_EXTENT)
    {
        return false;
    }

    const float fallbackDepth = min(depth0, min(depth1, depth2));

    int2 minPx = int2(floor(bbMinF));
    int2 maxPx = int2(floor(bbMaxF));
    minPx = max(minPx, int2(viewRasterInfo.scissorMinX, viewRasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(viewRasterInfo.scissorMaxX) - 1, int(viewRasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(headPointerDims.x) - 1, int(headPointerDims.y) - 1));

    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
        const float2 centroidPosition = (s0 + s1 + s2) * (1.0f / 3.0f);
        const int2 centroidPx = int2(floor(centroidPosition));
        if (centroidPx.x < int(viewRasterInfo.scissorMinX) || centroidPx.y < int(viewRasterInfo.scissorMinY) ||
            centroidPx.x >= int(viewRasterInfo.scissorMaxX) || centroidPx.y >= int(viewRasterInfo.scissorMaxY) ||
            centroidPx.x < 0 || centroidPx.y < 0 ||
            centroidPx.x >= int(headPointerDims.x) || centroidPx.y >= int(headPointerDims.y))
        {
            return false;
        }

        ReyesTryWriteDeepVisibilityNode(
            opaqueVisibility,
            deepVisibilityHeadPointers,
            deepVisibilityNodes,
            deepVisibilityNodeCounter,
            deepVisibilityOverflowCounter,
            nodeCapacity,
            uint2(centroidPx),
            fallbackDepth,
            patchVisibilityIndex,
            microTriangleIndex,
            isBackface);
        return true;
    }

    [loop]
    for (int py = minPx.y; py <= maxPx.y; ++py)
    {
        [loop]
        for (int px = minPx.x; px <= maxPx.x; ++px)
        {
            ReyesTryWriteDeepVisibilityNode(
                opaqueVisibility,
                deepVisibilityHeadPointers,
                deepVisibilityNodes,
                deepVisibilityNodeCounter,
                deepVisibilityOverflowCounter,
                nodeCapacity,
                uint2(px, py),
                fallbackDepth,
                patchVisibilityIndex,
                microTriangleIndex,
                isBackface);
        }
    }

    return true;
}

void ReyesRasterizeProjectedDeepVisibilityMicroTriangle(
    Texture2D<uint64_t> opaqueVisibility,
    RWTexture2D<uint> deepVisibilityHeadPointers,
    RWStructuredBuffer<CLodDeepVisibilityNode> deepVisibilityNodes,
    RWStructuredBuffer<uint> deepVisibilityNodeCounter,
    RWStructuredBuffer<uint> deepVisibilityOverflowCounter,
    uint nodeCapacity,
    uint2 headPointerDims,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer,
    ClodViewRasterInfo viewRasterInfo,
    float4 clip0,
    float4 clip1,
    float4 clip2,
    float depth0,
    float depth1,
    float depth2,
    uint patchVisibilityIndex,
    uint microTriangleIndex,
    bool isBackface)
{
    const float clipWEpsilon = 1e-6f;
    if (clip0.w <= clipWEpsilon || clip1.w <= clipWEpsilon || clip2.w <= clipWEpsilon)
    {
        InterlockedAdd(telemetryBuffer[0].rasterClipCullCount, 1u);
        return;
    }

    const float2 ndc0 = clip0.xy / clip0.w;
    const float2 ndc1 = clip1.xy / clip1.w;
    const float2 ndc2 = clip2.xy / clip2.w;

    const float visWidth = float(viewRasterInfo.scissorMaxX - viewRasterInfo.scissorMinX);
    const float visHeight = float(viewRasterInfo.scissorMaxY - viewRasterInfo.scissorMinY);
    const float scissorMinXf = float(viewRasterInfo.scissorMinX);
    const float scissorMinYf = float(viewRasterInfo.scissorMinY);
    float2 s0 = float2((ndc0.x + 1.0f) * 0.5f * visWidth + scissorMinXf, (1.0f - ndc0.y) * 0.5f * visHeight + scissorMinYf);
    float2 s1 = float2((ndc1.x + 1.0f) * 0.5f * visWidth + scissorMinXf, (1.0f - ndc1.y) * 0.5f * visHeight + scissorMinYf);
    float2 s2 = float2((ndc2.x + 1.0f) * 0.5f * visWidth + scissorMinXf, (1.0f - ndc2.y) * 0.5f * visHeight + scissorMinYf);

    float2 e01 = s1 - s0;
    float2 e02 = s2 - s0;
    float twiceArea = e01.x * e02.y - e01.y * e02.x;
    const bool originalBackface = twiceArea > 0.0f;
    if (abs(twiceArea) <= REYES_PATCH_RASTER_TINY_TRIANGLE_AREA_EPSILON)
    {
#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
        if (ReyesTryRasterizeTinyProjectedDeepVisibilityMicroTriangle(
            opaqueVisibility,
            deepVisibilityHeadPointers,
            deepVisibilityNodes,
            deepVisibilityNodeCounter,
            deepVisibilityOverflowCounter,
            nodeCapacity,
            headPointerDims,
            viewRasterInfo,
            s0,
            s1,
            s2,
            depth0,
            depth1,
            depth2,
            patchVisibilityIndex,
            microTriangleIndex,
            originalBackface))
        {
            InterlockedAdd(telemetryBuffer[0].rasterTinyTriangleFallbackCount, 1u);
            return;
        }
#endif

        InterlockedAdd(telemetryBuffer[0].rasterPreAreaCullCount, 1u);
        return;
    }

    if (twiceArea > 0.0f)
    {
        InterlockedAdd(telemetryBuffer[0].rasterWindingSwapCount, 1u);
        float2 tmpPos = s1;
        s1 = s2;
        s2 = tmpPos;

        float tmpDepth = depth1;
        depth1 = depth2;
        depth2 = tmpDepth;

        e01 = s1 - s0;
        e02 = s2 - s0;
        twiceArea = e01.x * e02.y - e01.y * e02.x;
    }
    else if (twiceArea >= 0.0f)
    {
#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
        if (ReyesTryRasterizeTinyProjectedDeepVisibilityMicroTriangle(
            opaqueVisibility,
            deepVisibilityHeadPointers,
            deepVisibilityNodes,
            deepVisibilityNodeCounter,
            deepVisibilityOverflowCounter,
            nodeCapacity,
            headPointerDims,
            viewRasterInfo,
            s0,
            s1,
            s2,
            depth0,
            depth1,
            depth2,
            patchVisibilityIndex,
            microTriangleIndex,
            originalBackface))
        {
            InterlockedAdd(telemetryBuffer[0].rasterTinyTriangleFallbackCount, 1u);
            return;
        }
#endif

        InterlockedAdd(telemetryBuffer[0].rasterPostSwapNonNegativeAreaCount, 1u);
        return;
    }

    const float invTwiceArea = -1.0f / twiceArea;
    const float2 bbMinF = min(min(s0, s1), s2);
    const float2 bbMaxF = max(max(s0, s1), s2);
    int2 minPx = int2(floor(bbMinF));
    int2 maxPx = int2(floor(bbMaxF));
    minPx = max(minPx, int2(viewRasterInfo.scissorMinX, viewRasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(viewRasterInfo.scissorMaxX) - 1, int(viewRasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(headPointerDims.x) - 1, int(headPointerDims.y) - 1));
    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
#if REYES_PATCH_RASTER_ENABLE_TINY_TRIANGLE_FALLBACK
        if (ReyesTryRasterizeTinyProjectedDeepVisibilityMicroTriangle(
            opaqueVisibility,
            deepVisibilityHeadPointers,
            deepVisibilityNodes,
            deepVisibilityNodeCounter,
            deepVisibilityOverflowCounter,
            nodeCapacity,
            headPointerDims,
            viewRasterInfo,
            s0,
            s1,
            s2,
            depth0,
            depth1,
            depth2,
            patchVisibilityIndex,
            microTriangleIndex,
            originalBackface))
        {
            InterlockedAdd(telemetryBuffer[0].rasterTinyTriangleFallbackCount, 1u);
            return;
        }
#endif

        InterlockedAdd(telemetryBuffer[0].rasterEmptyBoundsCullCount, 1u);
        return;
    }

    const float2 origin = float2(float(minPx.x) + 0.5f, float(minPx.y) + 0.5f);
    const float2 e12 = s2 - s1;
    const float2 e20 = s0 - s2;
    const float row_b0 = ((origin.x - s1.x) * e12.y - (origin.y - s1.y) * e12.x) * invTwiceArea;
    const float row_b1 = ((origin.x - s2.x) * e20.y - (origin.y - s2.y) * e20.x) * invTwiceArea;
    const float dx_b0 = e12.y * invTwiceArea;
    const float dx_b1 = e20.y * invTwiceArea;
    const float dy_b0 = -e12.x * invTwiceArea;
    const float dy_b1 = -e20.x * invTwiceArea;

    float scanline_b0 = row_b0;
    float scanline_b1 = row_b1;
    [loop]
    for (int py = minPx.y; py <= maxPx.y; ++py)
    {
        float b0 = scanline_b0;
        float b1 = scanline_b1;
        [loop]
        for (int px = minPx.x; px <= maxPx.x; ++px)
        {
            const float b2 = 1.0f - b0 - b1;
            if (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f)
            {
                const float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                ReyesTryWriteDeepVisibilityNode(
                    opaqueVisibility,
                    deepVisibilityHeadPointers,
                    deepVisibilityNodes,
                    deepVisibilityNodeCounter,
                    deepVisibilityOverflowCounter,
                    nodeCapacity,
                    uint2(px, py),
                    depth,
                    patchVisibilityIndex,
                    microTriangleIndex,
                    originalBackface);
            }

            b0 += dx_b0;
            b1 += dx_b1;
        }

        scanline_b0 += dy_b0;
        scanline_b1 += dy_b1;
    }
}

void ReyesRasterizeDeepVisibilityMicroTriangle(
    Texture2D<uint64_t> opaqueVisibility,
    RWTexture2D<uint> deepVisibilityHeadPointers,
    RWStructuredBuffer<CLodDeepVisibilityNode> deepVisibilityNodes,
    RWStructuredBuffer<uint> deepVisibilityNodeCounter,
    RWStructuredBuffer<uint> deepVisibilityOverflowCounter,
    uint nodeCapacity,
    uint2 headPointerDims,
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer,
    ClodViewRasterInfo viewRasterInfo,
    float4 clip0,
    float4 clip1,
    float4 clip2,
    float depth0,
    float depth1,
    float depth2,
    float nearDepth,
    uint patchVisibilityIndex,
    uint microTriangleIndex)
{
#if REYES_PATCH_RASTER_ENABLE_NEAR_PLANE_CLIPPING
    ReyesRasterVertex inputVertices[3];
    inputVertices[0].clip = clip0;
    inputVertices[0].depth = depth0;
    inputVertices[1].clip = clip1;
    inputVertices[1].depth = depth1;
    inputVertices[2].clip = clip2;
    inputVertices[2].depth = depth2;

    ReyesRasterVertex clippedVertices[4];
    uint clippedCount = 0u;

    [unroll]
    for (uint edgeIndex = 0u; edgeIndex < 3u; ++edgeIndex)
    {
        const ReyesRasterVertex currentVertex = inputVertices[edgeIndex];
        const ReyesRasterVertex nextVertex = inputVertices[(edgeIndex + 1u) % 3u];
        const bool currentInside = currentVertex.depth >= nearDepth;
        const bool nextInside = nextVertex.depth >= nearDepth;

        if (currentInside)
        {
            clippedVertices[clippedCount++] = currentVertex;
        }

        if (currentInside != nextInside)
        {
            const float depthDelta = nextVertex.depth - currentVertex.depth;
            const float t = saturate((nearDepth - currentVertex.depth) / depthDelta);

            ReyesRasterVertex clippedVertex;
            clippedVertex.clip = lerp(currentVertex.clip, nextVertex.clip, t);
            clippedVertex.depth = nearDepth;
            clippedVertices[clippedCount++] = clippedVertex;
        }
    }

    if (clippedCount < 3u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterClipCullCount, 1u);
        return;
    }

    ReyesRasterizeProjectedDeepVisibilityMicroTriangle(
        opaqueVisibility,
        deepVisibilityHeadPointers,
        deepVisibilityNodes,
        deepVisibilityNodeCounter,
        deepVisibilityOverflowCounter,
        nodeCapacity,
        headPointerDims,
        telemetryBuffer,
        viewRasterInfo,
        clippedVertices[0].clip,
        clippedVertices[1].clip,
        clippedVertices[2].clip,
        clippedVertices[0].depth,
        clippedVertices[1].depth,
        clippedVertices[2].depth,
        patchVisibilityIndex,
        microTriangleIndex,
        false);

    if (clippedCount == 4u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterNearPlaneClippedQuadCount, 1u);
        ReyesRasterizeProjectedDeepVisibilityMicroTriangle(
            opaqueVisibility,
            deepVisibilityHeadPointers,
            deepVisibilityNodes,
            deepVisibilityNodeCounter,
            deepVisibilityOverflowCounter,
            nodeCapacity,
            headPointerDims,
            telemetryBuffer,
            viewRasterInfo,
            clippedVertices[0].clip,
            clippedVertices[2].clip,
            clippedVertices[3].clip,
            clippedVertices[0].depth,
            clippedVertices[2].depth,
            clippedVertices[3].depth,
            patchVisibilityIndex,
            microTriangleIndex,
            false);
    }
#else
    if (depth0 < nearDepth || depth1 < nearDepth || depth2 < nearDepth)
    {
        InterlockedAdd(telemetryBuffer[0].rasterClipCullCount, 1u);
        return;
    }

    ReyesRasterizeProjectedDeepVisibilityMicroTriangle(
        opaqueVisibility,
        deepVisibilityHeadPointers,
        deepVisibilityNodes,
        deepVisibilityNodeCounter,
        deepVisibilityOverflowCounter,
        nodeCapacity,
        headPointerDims,
        telemetryBuffer,
        viewRasterInfo,
        clip0,
        clip1,
        clip2,
        depth0,
        depth1,
        depth2,
        patchVisibilityIndex,
        microTriangleIndex,
        false);
#endif
}

[shader("compute")]
[numthreads(REYES_PATCH_RASTER_GROUP_SIZE, 1, 1)]
void ReyesPatchDeepVisibilityRasterCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint rasterWorkIndex = dispatchThreadId.x;
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueue = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> diceQueueCounter = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_DICE_QUEUE_COUNTER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesRasterWorkEntry> rasterWorkBuffer = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> rasterWorkCounter = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_WORK_COUNTER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableVertices = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableTriangles = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_VIEW_RASTER_INFO_DESCRIPTOR_INDEX];
    RWStructuredBuffer<CLodDeepVisibilityNode> deepVisibilityNodes = ResourceDescriptorHeap[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> deepVisibilityNodeCounter = ResourceDescriptorHeap[CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_COUNTER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> deepVisibilityOverflowCounter = ResourceDescriptorHeap[CLOD_REYES_DEEP_VISIBILITY_RASTER_OVERFLOW_COUNTER_DESCRIPTOR_INDEX];
    ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_REYES_PATCH_RASTER_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshInstanceBuffer> perMeshInstances = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshInstanceBuffer)];
    StructuredBuffer<PerMeshBuffer> perMeshes = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    StructuredBuffer<PerObjectBuffer> perObjects = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerObjectBuffer)];
    StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];

    const uint rasterWorkCount = rasterWorkCounter[0];
    if (rasterWorkIndex >= rasterWorkCount)
    {
        return;
    }

    const CLodReyesRasterWorkEntry rasterWorkEntry = rasterWorkBuffer[rasterWorkIndex];
    const uint diceIndex = rasterWorkEntry.diceQueueIndex;
    const uint diceCount = diceQueueCounter[0];
    if (diceIndex >= diceCount)
    {
        return;
    }

    const CLodReyesDiceQueueEntry diceEntry = diceQueue[diceIndex];
    uint viewRasterInfoCount = 0u;
    uint viewRasterInfoStride = 0u;
    viewRasterInfoBuffer.GetDimensions(viewRasterInfoCount, viewRasterInfoStride);
    if (diceEntry.viewID >= viewRasterInfoCount)
    {
        return;
    }

    const ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[diceEntry.viewID];
    if (viewRasterInfo.opaqueVisibilitySRVDescriptorIndex == 0xFFFFFFFFu ||
        viewRasterInfo.deepVisibilityHeadPointerUAVDescriptorIndex == 0xFFFFFFFFu)
    {
        return;
    }

    Texture2D<uint64_t> opaqueVisibility = ResourceDescriptorHeap[NonUniformResourceIndex(viewRasterInfo.opaqueVisibilitySRVDescriptorIndex)];
    RWTexture2D<uint> deepVisibilityHeadPointers = ResourceDescriptorHeap[NonUniformResourceIndex(viewRasterInfo.deepVisibilityHeadPointerUAVDescriptorIndex)];
    uint2 headPointerDims;
    deepVisibilityHeadPointers.GetDimensions(headPointerDims.x, headPointerDims.y);

    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, diceEntry.visibleClusterIndex);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    const CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    const CLodMeshletDescriptor meshletDesc = LoadMeshletDescriptor(pageSlabDescriptorIndex, pageSlabByteOffset, hdr.descriptorOffset, localMeshletIndex);

    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(diceEntry.instanceID);
    const PerMeshBuffer perMesh = perMeshes[meshInstance.perMeshBufferIndex];
    const PerObjectBuffer objectData = LoadInstanceTransformForDraw(diceEntry.instanceID);
    const CullingCameraInfo camera = cameras[diceEntry.viewID];
    const MaterialInfo materialInfo = materials[perMesh.materialDataIndex];

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(pageSlabDescriptorIndex)];
    const uint sourceTriangleIndex = diceEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
    if (sourceTriangleIndex >= CLodDescTriangleCount(meshletDesc))
    {
        return;
    }

    const uint3 sourceTriangle = DecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, meshletDesc.triangleByteOffset, sourceTriangleIndex);
    const float3 sourcePosition0 = DecodeSkinnedPosition(sourceTriangle.x, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const float3 sourcePosition1 = DecodeSkinnedPosition(sourceTriangle.y, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const float3 sourcePosition2 = DecodeSkinnedPosition(sourceTriangle.z, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
    const bool displacementEnabled = materialInfo.geometricDisplacementEnabled != 0u;
    float3 sourceNormal0 = float3(0.0f, 0.0f, 1.0f);
    float3 sourceNormal1 = float3(0.0f, 0.0f, 1.0f);
    float3 sourceNormal2 = float3(0.0f, 0.0f, 1.0f);
    float2 sourceUv0 = float2(0.0f, 0.0f);
    float2 sourceUv1 = float2(0.0f, 0.0f);
    float2 sourceUv2 = float2(0.0f, 0.0f);
    if (displacementEnabled)
    {
        sourceNormal0 = DecodeSkinnedNormal(sourceTriangle.x, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceNormal1 = DecodeSkinnedNormal(sourceTriangle.y, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceNormal2 = DecodeSkinnedNormal(sourceTriangle.z, hdr, meshletDesc, pageSlabByteOffset, pageSlabDescriptorIndex, perMesh.vertexFlags, meshInstance.skinningInstanceSlot);
        sourceUv0 = DecodeCompressedUV(sourceTriangle.x, materialInfo.heightUvSetIndex, hdr, meshletDesc, localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
        sourceUv1 = DecodeCompressedUV(sourceTriangle.y, materialInfo.heightUvSetIndex, hdr, meshletDesc, localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
        sourceUv2 = DecodeCompressedUV(sourceTriangle.z, materialInfo.heightUvSetIndex, hdr, meshletDesc, localMeshletIndex, pageSlabByteOffset, pageSlabDescriptorIndex);
    }

    const float3 domain0 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex0UV);
    const float3 domain1 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex1UV);
    const float3 domain2 = ReyesPatchDomainUVToBarycentrics(diceEntry.domainVertex2UV);
    const uint microTriangleCount = ReyesGetDicePatchMicroTriangleCount(tessTableConfigs, diceEntry);
    if (microTriangleCount == 0u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterZeroMicroTriangleCount, 1u);
        return;
    }

    if (microTriangleCount > 128u)
    {
        InterlockedAdd(telemetryBuffer[0].rasterMicroTriangleOverflowCount, 1u);
        return;
    }

    row_major matrix modelViewProjection = mul(objectData.model, camera.viewProjection);
    float4 modelViewZ = mul(objectData.model, camera.viewZ);
    const float patchDepth = max(
        ( -dot(float4(sourcePosition0, 1.0f), modelViewZ)
        + -dot(float4(sourcePosition1, 1.0f), modelViewZ)
        + -dot(float4(sourcePosition2, 1.0f), modelViewZ)) / 3.0f,
        max(camera.zNear, 1.0e-3f));

    const uint patchVisibilityIndex = CLOD_REYES_PATCH_RASTER_PATCH_INDEX_BASE + diceIndex;
    const uint rasterMicroTriangleEnd = min(rasterWorkEntry.microTriangleOffset + rasterWorkEntry.microTriangleCount, microTriangleCount);
    [loop]
    for (uint microTriangleIndex = rasterWorkEntry.microTriangleOffset; microTriangleIndex < rasterMicroTriangleEnd; ++microTriangleIndex)
    {
        float3 patchBary0;
        float3 patchBary1;
        float3 patchBary2;
        ReyesDecodeMicroTrianglePatchDomain(tessTableConfigs, tessTableVertices, tessTableTriangles, microTriangleIndex, diceEntry, patchBary0, patchBary1, patchBary2);

        float3 sourceBary0;
        float3 sourceBary1;
        float3 sourceBary2;
        float3 patchPosition0;
        float3 patchPosition1;
        float3 patchPosition2;
        ReyesEvaluateDisplacedPatchTriangle(
            materialInfo,
            displacementEnabled,
            camera,
            perFrame.heightFadeStartDistance,
            perFrame.heightFadeEndDistance,
            objectData.model,
            patchDepth,
            sourcePosition0,
            sourcePosition1,
            sourcePosition2,
            sourceNormal0,
            sourceNormal1,
            sourceNormal2,
            sourceUv0,
            sourceUv1,
            sourceUv2,
            domain0,
            domain1,
            domain2,
            patchBary0,
            patchBary1,
            patchBary2,
            sourceBary0,
            sourceBary1,
            sourceBary2,
            patchPosition0,
            patchPosition1,
            patchPosition2);

        const float4 clip0 = mul(float4(patchPosition0, 1.0f), modelViewProjection);
        const float4 clip1 = mul(float4(patchPosition1, 1.0f), modelViewProjection);
        const float4 clip2 = mul(float4(patchPosition2, 1.0f), modelViewProjection);
        const float depth0 = -dot(float4(patchPosition0, 1.0f), modelViewZ);
        const float depth1 = -dot(float4(patchPosition1, 1.0f), modelViewZ);
        const float depth2 = -dot(float4(patchPosition2, 1.0f), modelViewZ);

        ReyesRasterizeDeepVisibilityMicroTriangle(
            opaqueVisibility,
            deepVisibilityHeadPointers,
            deepVisibilityNodes,
            deepVisibilityNodeCounter,
            deepVisibilityOverflowCounter,
            CLOD_REYES_DEEP_VISIBILITY_RASTER_NODE_CAPACITY,
            headPointerDims,
            telemetryBuffer,
            viewRasterInfo,
            clip0,
            clip1,
            clip2,
            depth0,
            depth1,
            depth2,
            camera.zNear,
            patchVisibilityIndex,
            microTriangleIndex);
    }
}
