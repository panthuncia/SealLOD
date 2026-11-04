// Hardware rasterizer for CLod clusters: a mesh shader, dispatched per raster bucket, that pairs
// with one of the output pixel shaders (visibility, deep visibility, virtual shadow, AVBOIT).
//
//   CLodLoadRasterCluster   shared with softwareRaster.hlsl
//   MSBuildVertex           fetch, skin and project one vertex
//   MSEmitTriangles         decode triangles and their primitive IDs
//
// Virtual shadow variants add a step ahead of SetMeshOutputCounts: project the vertices into
// groupshared memory and keep only the triangles that touch a page needing render.

#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "Common/defines.h"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"

#ifndef CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
#define CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW 0
#endif
#define CLOD_RASTER_CLUSTER_VIRTUAL_SHADOW CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW

#include "include/clodRasterCluster.hlsli"
#include "include/clodClusterFormat.hlsli"

#ifndef CLOD_ENABLE_SOURCE_GROUP_VALIDATION
#define CLOD_ENABLE_SOURCE_GROUP_VALIDATION 0
#endif
#if CLOD_ENABLE_SOURCE_GROUP_VALIDATION
#include "include/clodSourceGroupValidation.hlsli"
#endif

#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
#include "include/clodRasterVirtualShadow.hlsli"
#endif

static const uint WG_COUNTER_RASTER_MESH_SHADER_GROUPS = 88u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_IN_RANGE = 89u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED = 90u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_OUTPUT_TRIANGLES = 91u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_ZERO_TRIANGLE_OUTPUTS = 92u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB = 93u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_MESHLET_OOB = 94u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_INVALID_OUTPUT_COUNTS = 95u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_SOURCE_GROUP_MISMATCH = 103u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_SKINNED_GROUPS = 235u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_SKINNED_OUTPUT_TRIANGLES = 237u;

void MSTelemetryAdd(uint counterIndex, uint value)
{
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == 0xFFFFFFFFu || value == 0u)
    {
        return;
    }

    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryCounters[counterIndex], value);
}

void MSRecordClusterTelemetry(uint loadResult)
{
#if defined(PSO_SKINNED)
    MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_SKINNED_GROUPS, 1u);
#endif
    MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_IN_RANGE, 1u);
    if (loadResult == CLOD_RASTER_CLUSTER_OK)
    {
        return;
    }

    MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED, 1u);
    if (loadResult == CLOD_RASTER_CLUSTER_ZERO_PAGE_SLAB)
    {
        MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB, 1u);
    }
    else if (loadResult == CLOD_RASTER_CLUSTER_MESHLET_OOB)
    {
        MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_MESHLET_OOB, 1u);
    }
    else
    {
        MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_INVALID_OUTPUT_COUNTS, 1u);
    }
}

void MSRecordOutputTelemetry(uint outputTriangleCount)
{
    MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_OUTPUT_TRIANGLES, outputTriangleCount);
#if defined(PSO_SKINNED)
    MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_SKINNED_OUTPUT_TRIANGLES, outputTriangleCount);
#endif
    if (outputTriangleCount == 0u)
    {
        MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_ZERO_TRIANGLE_OUTPUTS, 1u);
    }
}

// ---------------------------------------------------------------------------------------------
// Vertices
// ---------------------------------------------------------------------------------------------

// Per-cluster state each lane needs while building vertices.
struct MSVertexContext
{
    uint visibleClusterIndex; // Written to the visibility buffer, so unsorted.
    uint shadowVsmPayload;
    float2 viewportScale;
#if defined(PSO_ALPHA_TEST) || defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    CLodUvStream uvStream0;
#endif
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    PerObjectBuffer objectBuffer;
#endif
};

float4 MSApplyViewportScale(float4 clipPosition, float2 viewportScale)
{
    clipPosition.x = clipPosition.x * viewportScale.x + clipPosition.w * (viewportScale.x - 1.0f);
    clipPosition.y = clipPosition.y * viewportScale.y + clipPosition.w * (1.0f - viewportScale.y);
    return clipPosition;
}

#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
// Transparent forward shading reads its full surface from interpolants.
VisBufferPSInput MSBuildVertex(CLodRasterCluster cluster, MSVertexContext context, ByteAddressBuffer slab, uint vertexIndex)
{
    const uint vertexAttributeIndex = cluster.vertexAttributeOffset + vertexIndex;

    Vertex vertex = (Vertex)0;
    vertex.position = CLodRasterLoadPosition(cluster, slab, vertexIndex);
    vertex.normal = CLodDecodeNormal(slab.Load(cluster.pageByteOffset + cluster.normalArrayOffset + vertexAttributeIndex * 4u));
    vertex.texcoord = CLodDecodeUv(slab, context.uvStream0, vertexIndex);
    vertex.color = (cluster.attributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u
        ? CLodDecodeColor(slab.Load(cluster.pageByteOffset + cluster.colorArrayOffset + vertexAttributeIndex * 4u))
        : float3(1.0f, 1.0f, 1.0f);
#if defined(PSO_SKINNED)
    ApplyAssemblySkinningToVertex(
        cluster.skinningInstanceSlot,
        CLodRasterLoadSkinning(cluster, slab, vertexIndex),
        cluster.assemblyTransformIndex,
        vertex);
#endif

    StructuredBuffer<SingleMatrix> normalMatrixBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::NormalMatrixBuffer)];
    const float3x3 normalMatrix = (float3x3)normalMatrixBuffer[context.objectBuffer.normalMatrixBufferIndex].value;

    VisBufferPSInput result = (VisBufferPSInput)0;
    result.position = MSApplyViewportScale(CLodRasterClipPosition(cluster, vertex.position), context.viewportScale);
    result.linearDepth = CLodRasterLinearDepth(cluster, vertex.position);
    result.positionWorldSpace = mul(float4(vertex.position, 1.0f), context.objectBuffer.model).xyz;
    result.normalWorldSpace = normalize(mul(vertex.normal, normalMatrix));
#if !defined(CLOD_FORWARD_VERTEX_COLOR) || CLOD_FORWARD_VERTEX_COLOR
    result.color = vertex.color;
#endif
    result.materialDataIndex = cluster.materialDataIndex;
#if !defined(CLOD_RASTER_SINGLE_VIEW)
    result.viewID = cluster.viewID;
#endif
#if defined(PSO_ALPHA_TEST)
    result.texcoord = vertex.texcoord;
#endif
#if CLOD_FORWARD_UV_SET_COUNT > 0
    result.uvSet01.xy = vertex.texcoord;
    result.uvSet01.zw = CLodDecodeUv(slab, CLodRasterLoadUvStream(cluster, 1u), vertexIndex);
#endif
#if CLOD_FORWARD_UV_SET_COUNT > 2
    result.uvSet23.xy = CLodDecodeUv(slab, CLodRasterLoadUvStream(cluster, 2u), vertexIndex);
    result.uvSet23.zw = CLodDecodeUv(slab, CLodRasterLoadUvStream(cluster, 3u), vertexIndex);
#endif
#if CLOD_FORWARD_UV_SET_COUNT > 4
    result.uvSet45.xy = CLodDecodeUv(slab, CLodRasterLoadUvStream(cluster, 4u), vertexIndex);
    result.uvSet45.zw = CLodDecodeUv(slab, CLodRasterLoadUvStream(cluster, 5u), vertexIndex);
#endif
#if CLOD_FORWARD_UV_SET_COUNT > 6
    result.uvSet67.xy = CLodDecodeUv(slab, CLodRasterLoadUvStream(cluster, 6u), vertexIndex);
    result.uvSet67.zw = CLodDecodeUv(slab, CLodRasterLoadUvStream(cluster, 7u), vertexIndex);
#endif
    return result;
}
#else
VisBufferPSInput MSBuildVertex(CLodRasterCluster cluster, MSVertexContext context, ByteAddressBuffer slab, uint vertexIndex)
{
    float3 position = CLodRasterLoadPosition(cluster, slab, vertexIndex);
#if defined(PSO_SKINNED)
    position = CLodRasterSkinPosition(cluster, slab, vertexIndex, position);
#endif

    VisBufferPSInput result = (VisBufferPSInput)0;
    result.position = MSApplyViewportScale(CLodRasterClipPosition(cluster, position), context.viewportScale);
    result.linearDepth = CLodRasterLinearDepth(cluster, position);
#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    result.virtualShadowClipDistances = CLodVirtualShadowBuildBlockClipDistances(result.position, context.shadowVsmPayload);
    result.shadowClipmapIndex = context.shadowVsmPayload;
#endif
#if defined(PSO_ALPHA_TEST)
    result.texcoord = CLodDecodeUv(slab, context.uvStream0, vertexIndex);
    result.materialDataIndex = cluster.materialDataIndex;
#endif
    result.visibleClusterIndex = context.visibleClusterIndex;
#if !defined(CLOD_RASTER_SINGLE_VIEW)
    result.viewID = cluster.viewID;
#endif
    return result;
}
#endif

MSVertexContext MSLoadVertexContext(
    CLodRasterCluster cluster,
    uint4 packedCluster,
    uint visibleClusterIndex,
    ClodViewRasterInfo viewRasterInfo)
{
    MSVertexContext context = (MSVertexContext)0;
    context.visibleClusterIndex = visibleClusterIndex;
    context.shadowVsmPayload = CLodVisibleClusterVsmPayload(packedCluster);
    context.viewportScale = float2(viewRasterInfo.viewportScaleX, viewRasterInfo.viewportScaleY);
#if defined(PSO_ALPHA_TEST) || defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    context.uvStream0 = CLodRasterLoadUvStream(cluster, 0u);
#endif
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    context.objectBuffer = LoadInstanceTransformForDrawRecordWithAssemblyTransform(
        LoadInstanceDrawRecord(cluster.drawRecordIndex),
        cluster.assemblyTransformIndex);
#endif
    return context;
}

// ---------------------------------------------------------------------------------------------
// Virtual shadow triangle filter
// ---------------------------------------------------------------------------------------------

#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
static const uint kMSInvalidTriangleOutputIndex = 0xFFFFFFFFu;

groupshared VisBufferPSInput gs_vertices[MS_MESHLET_SIZE];
groupshared uint gs_triangleOutputIndex[MS_MESHLET_SIZE];
groupshared uint gs_keptTriangleCount;

// Projects the vertices into groupshared memory, then decides which triangles to keep. Returns
// how many were kept. All lanes of a group call this together, since they share a cluster.
uint MSFilterVirtualShadowTriangles(
    CLodRasterCluster cluster,
    MSVertexContext context,
    ClodViewRasterInfo viewRasterInfo,
    uint4 packedCluster,
    ByteAddressBuffer slab,
    uint laneIndex)
{
    if (laneIndex == 0u)
    {
        gs_keptTriangleCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint vertexIndex = laneIndex; vertexIndex < cluster.vertexCount; vertexIndex += MS_THREAD_GROUP_SIZE)
    {
        gs_vertices[vertexIndex] = MSBuildVertex(cluster, context, slab, vertexIndex);
    }
    GroupMemoryBarrierWithGroupSync();

    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    bool hasClipmapInfo = false;
    const uint clipmapIndex = CLodVisibleClusterShadowClipmapIndex(packedCluster);
    if (clipmapIndex < kCLodVirtualShadowClipmapCount)
    {
        StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
            ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
        clipmapInfo = clipmapInfos[clipmapIndex];
        hasClipmapInfo = CLodVirtualShadowClipmapIsValid(clipmapInfo) && clipmapInfo.shadowCameraBufferIndex == cluster.viewID;
    }

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
    const uint triangleStreamAddress = CLodRasterTriangleStreamAddress(cluster);
    for (uint triangleIndex = laneIndex; triangleIndex < cluster.triangleCount; triangleIndex += MS_THREAD_GROUP_SIZE)
    {
        gs_triangleOutputIndex[triangleIndex] = kMSInvalidTriangleOutputIndex;

        uint3 vertices = CLodDecodeTriangle(slab, triangleStreamAddress, triangleIndex);
        if (cluster.reverseWinding != 0u)
        {
            vertices = vertices.xzy;
        }

        if (hasClipmapInfo &&
            CLodTriangleTouchesRenderableVirtualShadowPages(
                gs_vertices[vertices.x].position,
                gs_vertices[vertices.y].position,
                gs_vertices[vertices.z].position,
                viewRasterInfo,
                context.shadowVsmPayload,
                clipmapInfo,
                pageTable))
        {
            uint outputIndex = 0u;
            InterlockedAdd(gs_keptTriangleCount, 1u, outputIndex);
            gs_triangleOutputIndex[triangleIndex] = outputIndex;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    return gs_keptTriangleCount;
}
#endif

// ---------------------------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------------------------

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MS_THREAD_GROUP_SIZE, 1, 1)]
void ClusterLODBucketMSMain(
    const uint laneIndex : SV_GroupThreadID,
    const uint3 groupID : SV_GroupID,
    out vertices VisBufferPSInput outputVertices[MS_MESHLET_SIZE],
    out indices uint3 outputTriangles[MS_MESHLET_SIZE],
    out primitives VisibilityPerPrimitive primitiveInfo[MS_MESHLET_SIZE])
{
    if (laneIndex == 0u)
    {
        // Ahead of every frame-data read, to tell missing ExecuteIndirect work from invalid bindings.
        MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_GROUPS, 1u);
    }

    // From the command signature.
    const uint bucketBaseOffset = IndirectCommandSignatureRootConstant0;
    const uint dispatchX = IndirectCommandSignatureRootConstant1;
    const uint bucketIndex = IndirectCommandSignatureRootConstant2;
    const uint bucketMeshletIndex = groupID.x + groupID.y * dispatchX;

    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketCount = histogram[bucketIndex];
    const bool inRange = bucketMeshletIndex < bucketCount;
    const uint sortedClusterIndex = bucketBaseOffset + bucketMeshletIndex;

    CLodRasterCluster cluster = (CLodRasterCluster)0;
    uint4 packedCluster = uint4(0, 0, 0, CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX);
    uint unsortedClusterIndex = 0u;
    uint loadResult = CLOD_RASTER_CLUSTER_ZERO_PAGE_SLAB;
    if (inRange)
    {
        ByteAddressBuffer compactedClusters = ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
        StructuredBuffer<uint> compactedClusterTransformIndices =
            ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
        StructuredBuffer<uint> sortedToUnsortedMapping = ResourceDescriptorHeap[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX];
        packedCluster = CLodLoadVisibleClusterPacked(compactedClusters, sortedClusterIndex);
        unsortedClusterIndex = sortedToUnsortedMapping[sortedClusterIndex];

        CLodRasterClusterSource source;
        source.packedCluster = packedCluster;
        source.assemblyTransformIndex = compactedClusterTransformIndices[sortedClusterIndex];
        source.hasResolvedSkinningSlot = false;
        source.resolvedSkinningSlot = 0xFFFFFFFFu;
        loadResult = CLodLoadRasterCluster(source, cluster);
    }
    const bool draw = inRange && loadResult == CLOD_RASTER_CLUSTER_OK;

    if (laneIndex == 0u && inRange)
    {
        MSRecordClusterTelemetry(loadResult);
#if CLOD_ENABLE_SOURCE_GROUP_VALIDATION
        if (draw && CLodValidateSourceGroup(packedCluster, sortedClusterIndex, unsortedClusterIndex, bucketMeshletIndex, bucketCount))
        {
            MSTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_SOURCE_GROUP_MISMATCH, 1u);
        }
#endif
    }

    ClodViewRasterInfo viewRasterInfo = (ClodViewRasterInfo)0;
    MSVertexContext context = (MSVertexContext)0;
    if (draw)
    {
#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
        const bool usesViewRasterInfo = true;
#else
        const bool usesViewRasterInfo = CLOD_RASTER_SINGLE_VIEW_VISIBILITY_UAV_DESCRIPTOR_INDEX == 0xFFFFFFFFu;
#endif
        if (usesViewRasterInfo)
        {
            StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer =
                ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
            viewRasterInfo = viewRasterInfoBuffer[cluster.viewID];
        }
        else
        {
            viewRasterInfo.viewportScaleX = 1.0f;
            viewRasterInfo.viewportScaleY = 1.0f;
        }
        context = MSLoadVertexContext(cluster, packedCluster, unsortedClusterIndex, viewRasterInfo);
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[cluster.slabDescriptorIndex];
    const uint triangleStreamAddress = CLodRasterTriangleStreamAddress(cluster);
    const bool reverseWinding = cluster.reverseWinding != 0u;

#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    uint outputTriangleCount = 0u;
    if (draw)
    {
        outputTriangleCount = MSFilterVirtualShadowTriangles(cluster, context, viewRasterInfo, packedCluster, slab, laneIndex);
    }
    const uint outputVertexCount = outputTriangleCount > 0u ? cluster.vertexCount : 0u;

    SetMeshOutputCounts(outputVertexCount, outputTriangleCount);
    if (laneIndex == 0u && draw)
    {
        MSRecordOutputTelemetry(outputTriangleCount);
    }

    for (uint vertexIndex = laneIndex; vertexIndex < outputVertexCount; vertexIndex += MS_THREAD_GROUP_SIZE)
    {
        outputVertices[vertexIndex] = gs_vertices[vertexIndex];
    }
    if (draw)
    {
        for (uint triangleIndex = laneIndex; triangleIndex < cluster.triangleCount; triangleIndex += MS_THREAD_GROUP_SIZE)
        {
            const uint outputIndex = gs_triangleOutputIndex[triangleIndex];
            if (outputIndex != kMSInvalidTriangleOutputIndex)
            {
                const uint3 vertices = CLodDecodeTriangle(slab, triangleStreamAddress, triangleIndex);
                outputTriangles[outputIndex] = reverseWinding ? vertices.xzy : vertices;
                primitiveInfo[outputIndex].triangleIndex = triangleIndex;
            }
        }
    }
#else
    const uint outputVertexCount = draw ? cluster.vertexCount : 0u;
    const uint outputTriangleCount = draw ? cluster.triangleCount : 0u;

    SetMeshOutputCounts(outputVertexCount, outputTriangleCount);
    if (laneIndex == 0u && draw)
    {
        MSRecordOutputTelemetry(outputTriangleCount);
    }

    for (uint vertexIndex = laneIndex; vertexIndex < outputVertexCount; vertexIndex += MS_THREAD_GROUP_SIZE)
    {
        outputVertices[vertexIndex] = MSBuildVertex(cluster, context, slab, vertexIndex);
    }
    for (uint triangleIndex = laneIndex; triangleIndex < outputTriangleCount; triangleIndex += MS_THREAD_GROUP_SIZE)
    {
        const uint3 vertices = CLodDecodeTriangle(slab, triangleStreamAddress, triangleIndex);
        outputTriangles[triangleIndex] = reverseWinding ? vertices.xzy : vertices;
        primitiveInfo[triangleIndex].triangleIndex = triangleIndex;
    }
#endif
}
