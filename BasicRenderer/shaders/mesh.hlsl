#include "include/vertex.hlsli"
#include "include/utilities.hlsli"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/loadingUtils.hlsli"
#include "Common/defines.h"
#include "include/meshletPayload.hlsli"
#include "include/meshletCommon.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/reyesPatchCommon.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"

#define CLOD_COMPRESSED_POSITIONS 1u
#define CLOD_COMPRESSED_NORMALS 4u

static const uint CLOD_TELEMETRY_DISABLED_DESCRIPTOR = 0xFFFFFFFFu;
#ifndef CLOD_ENABLE_SOURCE_GROUP_VALIDATION
#define CLOD_ENABLE_SOURCE_GROUP_VALIDATION 1
#endif
static const uint WG_COUNTER_RASTER_MESH_SHADER_GROUPS = 117u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_IN_RANGE = 118u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED = 119u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_OUTPUT_TRIANGLES = 120u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_ZERO_TRIANGLE_OUTPUTS = 121u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB = 122u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_MESHLET_OOB = 123u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_INVALID_OUTPUT_COUNTS = 124u;
static const uint WG_COUNTER_RASTER_MESH_SHADER_SOURCE_GROUP_MISMATCH = 132u;
static const uint CLOD_SOURCE_GROUP_MISMATCH_DETAIL_CAPACITY = 1024u;

static const uint CLOD_RASTER_INIT_FAILURE_NONE = 0u;
static const uint CLOD_RASTER_INIT_FAILURE_ZERO_PAGE_SLAB = 1u;
static const uint CLOD_RASTER_INIT_FAILURE_MESHLET_OOB = 2u;
static const uint CLOD_RASTER_INIT_FAILURE_INVALID_OUTPUT_COUNTS = 3u;

void CLodRasterTelemetryAdd(uint counterIndex, uint value)
{
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == CLOD_TELEMETRY_DISABLED_DESCRIPTOR || value == 0u)
    {
        return;
    }

    RWStructuredBuffer<uint> telemetryCounters = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    InterlockedAdd(telemetryCounters[counterIndex], value);
}

#if CLOD_ENABLE_SOURCE_GROUP_VALIDATION
struct CLodSourceGroupMismatchDetail
{
    uint expectedGroupLocalIndex;
    uint foundGroupLocalIndex;
    uint expectedGroupGlobalIndex;
    uint foundGroupGlobalIndex;
    uint clodMeshMetadataIndex;
    uint groupsBase;
    uint expectedSegmentGlobalIndex;
    uint expectedSegmentPageIndex;
    uint expectedSegmentFirstMeshlet;
    uint expectedSegmentMeshletCount;
    uint expectedSegmentPageSlabDescriptorIndex;
    uint expectedSegmentPageSlabByteOffset;
    uint pageLocalMeshletIndex;
    uint pageSlabDescriptorIndex;
    uint pageSlabByteOffset;
    uint visibleClusterIndex;
    uint unsortedClusterIndex;
    uint instanceId;
    uint viewId;
    uint bucketMeshletIndex;
    uint bucketCount;
    uint pad0;
};

void CLodRecordSourceGroupMismatch(
    uint expectedGroupLocalIndex,
    uint foundGroupLocalIndex,
    uint pageLocalMeshletIndex,
    uint pageSlabDescriptorIndex,
    uint pageSlabByteOffset,
    uint visibleClusterIndex,
    uint unsortedClusterIndex,
    uint instanceId,
    uint viewId,
    uint bucketMeshletIndex,
    uint bucketCount)
{
    if (CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX == CLOD_TELEMETRY_DISABLED_DESCRIPTOR ||
        CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX == CLOD_TELEMETRY_DISABLED_DESCRIPTOR)
    {
        return;
    }

    StructuredBuffer<CLodMeshMetadata> clodMeshMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    StructuredBuffer<ClusterLODGroup> clodGroups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    StructuredBuffer<ClusterLODGroupSegment> clodSegments =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Segments)];
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceId);
    const CLodMeshMetadata metadata = clodMeshMetadataBuffer[offsets.clodMeshMetadataIndex];
    const ClusterLODGroup expectedGroup = clodGroups[metadata.groupsBase + expectedGroupLocalIndex];

    uint expectedSegmentGlobalIndex = 0xFFFFFFFFu;
    uint expectedSegmentPageIndex = 0xFFFFFFFFu;
    uint expectedSegmentFirstMeshlet = 0u;
    uint expectedSegmentMeshletCount = 0u;
    uint expectedSegmentPageSlabDescriptorIndex = 0u;
    uint expectedSegmentPageSlabByteOffset = 0u;
    const uint expectedSegmentEnd = expectedGroup.firstSegment + expectedGroup.segmentCount;
    for (uint segmentIndex = expectedGroup.firstSegment; segmentIndex < expectedSegmentEnd; ++segmentIndex)
    {
        const ClusterLODGroupSegment segment = clodSegments[metadata.segmentsBase + segmentIndex];
        if (pageLocalMeshletIndex < segment.firstMeshletInPage ||
            pageLocalMeshletIndex >= segment.firstMeshletInPage + segment.meshletCount)
        {
            continue;
        }

        const GroupPageMapEntry segmentPageEntry = LoadGroupPageMapEntry(metadata.pageMapBase, segment.pageIndex);
        if (segmentPageEntry.slabDescriptorIndex != pageSlabDescriptorIndex ||
            segmentPageEntry.slabByteOffset != pageSlabByteOffset)
        {
            continue;
        }

        expectedSegmentGlobalIndex = metadata.segmentsBase + segmentIndex;
        expectedSegmentPageIndex = segment.pageIndex;
        expectedSegmentFirstMeshlet = segment.firstMeshletInPage;
        expectedSegmentMeshletCount = segment.meshletCount;
        expectedSegmentPageSlabDescriptorIndex = segmentPageEntry.slabDescriptorIndex;
        expectedSegmentPageSlabByteOffset = segmentPageEntry.slabByteOffset;
        break;
    }

    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CLOD_RASTER_SOURCE_GROUP_MISMATCH_COUNTER_DESCRIPTOR_INDEX];
    uint detailIndex = 0u;
    InterlockedAdd(counter[0], 1u, detailIndex);
    if (detailIndex >= CLOD_SOURCE_GROUP_MISMATCH_DETAIL_CAPACITY)
    {
        return;
    }

    CLodSourceGroupMismatchDetail detail;
    detail.expectedGroupLocalIndex = expectedGroupLocalIndex;
    detail.foundGroupLocalIndex = foundGroupLocalIndex;
    detail.expectedGroupGlobalIndex = metadata.groupsBase + expectedGroupLocalIndex;
    detail.foundGroupGlobalIndex = foundGroupLocalIndex != 0xFFFFFFFFu ? metadata.groupsBase + foundGroupLocalIndex : 0xFFFFFFFFu;
    detail.clodMeshMetadataIndex = offsets.clodMeshMetadataIndex;
    detail.groupsBase = metadata.groupsBase;
    detail.expectedSegmentGlobalIndex = expectedSegmentGlobalIndex;
    detail.expectedSegmentPageIndex = expectedSegmentPageIndex;
    detail.expectedSegmentFirstMeshlet = expectedSegmentFirstMeshlet;
    detail.expectedSegmentMeshletCount = expectedSegmentMeshletCount;
    detail.expectedSegmentPageSlabDescriptorIndex = expectedSegmentPageSlabDescriptorIndex;
    detail.expectedSegmentPageSlabByteOffset = expectedSegmentPageSlabByteOffset;
    detail.pageLocalMeshletIndex = pageLocalMeshletIndex;
    detail.pageSlabDescriptorIndex = pageSlabDescriptorIndex;
    detail.pageSlabByteOffset = pageSlabByteOffset;
    detail.visibleClusterIndex = visibleClusterIndex;
    detail.unsortedClusterIndex = unsortedClusterIndex;
    detail.instanceId = instanceId;
    detail.viewId = viewId;
    detail.bucketMeshletIndex = bucketMeshletIndex;
    detail.bucketCount = bucketCount;
    detail.pad0 = 0u;

    RWStructuredBuffer<CLodSourceGroupMismatchDetail> details =
        ResourceDescriptorHeap[CLOD_RASTER_SOURCE_GROUP_MISMATCH_DETAILS_DESCRIPTOR_INDEX];
    details[detailIndex] = detail;
}
#endif

#ifndef CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
#define CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW 0
#endif

#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
static const uint kClodInvalidTriangleOutputIndex = 0xFFFFFFFFu;
static const uint kClodReyesShadowMaxOutputTriangles = CLodReyesHardwareRasterMaxPackedMicroTriangles;
static const uint kClodReyesShadowMaxOutputVertices = kClodReyesShadowMaxOutputTriangles * 3u;

groupshared float4 gs_clodVsmVertexPosition[MS_MESHLET_SIZE];
groupshared float gs_clodVsmLinearDepth[MS_MESHLET_SIZE];
#if defined(PSO_ALPHA_TEST)
groupshared float2 gs_clodVsmTexcoord[MS_MESHLET_SIZE];
#endif
groupshared uint gs_clodVsmTriangleOutputIndex[MS_MESHLET_SIZE];
groupshared uint gs_clodVsmKeptTriangleCount;
groupshared uint gs_clodVsmHasClipmapInfo;
groupshared CLodVirtualShadowClipmapInfo gs_clodVsmClipmapInfo;

struct ReyesShadowVisVertex
{
    float4 position;
    float linearDepth;
#if defined(PSO_ALPHA_TEST)
    float2 texcoord;
#endif
};

groupshared ReyesShadowVisVertex gs_reyesShadowVertices[kClodReyesShadowMaxOutputVertices];
groupshared uint3 gs_reyesShadowTriangles[kClodReyesShadowMaxOutputTriangles];
groupshared uint gs_reyesShadowPrimitiveIDs[kClodReyesShadowMaxOutputTriangles];
groupshared uint gs_reyesShadowOutputVertexCount;
groupshared uint gs_reyesShadowOutputTriangleCount;
groupshared uint gs_reyesShadowDispatchValid;
groupshared uint gs_reyesShadowCurrentEntryValid;
groupshared MeshletSetup gs_reyesShadowSetup;
groupshared CLodReyesDiceQueueEntry gs_reyesShadowDiceEntry;
groupshared CLodReyesPackedRasterWorkGroupEntry gs_reyesShadowPackedWorkGroup;
groupshared ClodViewRasterInfo gs_reyesShadowViewRasterInfo;
groupshared CLodVirtualShadowClipmapInfo gs_reyesShadowClipmapInfo;
groupshared uint3 gs_reyesShadowSourceTriangle;
groupshared float3 gs_reyesShadowSourcePositions[3u];
groupshared float3 gs_reyesShadowSourceNormals[3u];
groupshared float2 gs_reyesShadowSourceTexcoords[3u];
groupshared float3 gs_reyesShadowDomainBarycentrics[3u];
groupshared uint gs_reyesShadowMicroTriangleStart;
groupshared uint gs_reyesShadowMicroTriangleEnd;
#endif

uint ReadPackedBits32(StructuredBuffer<uint> words, uint startBit, uint bitCount)
{
    if (bitCount == 0u)
    {
        return 0u;
    }

    uint wordIndex = startBit >> 5;
    uint bitOffset = startBit & 31u;
    uint packed = words[wordIndex] >> bitOffset;
    if (bitOffset + bitCount > 32u)
    {
        packed |= words[wordIndex + 1u] << (32u - bitOffset);
    }

    uint mask = (bitCount >= 32u) ? 0xffffffffu : ((1u << bitCount) - 1u);
    return packed & mask;
}

// ByteAddressBuffer variant for page-pool slab reads.
uint ReadPackedBits32_BA(ByteAddressBuffer buf, uint startBit, uint bitCount)
{
    if (bitCount == 0u)
    {
        return 0u;
    }

    uint wordIndex = startBit >> 5;
    uint bitOffset = startBit & 31u;
    uint packed = buf.Load(wordIndex * 4u) >> bitOffset;
    if (bitOffset + bitCount > 32u)
    {
        packed |= buf.Load((wordIndex + 1u) * 4u) << (32u - bitOffset);
    }

    uint mask = (bitCount >= 32u) ? 0xffffffffu : ((1u << bitCount) - 1u);
    return packed & mask;
}

float3 DecodeCompressedPosition(
    uint meshletLocalVertex,
    uint positionBitstreamBase,
    uint positionBitOffset,
    uint bitsX,
    uint bitsY,
    uint bitsZ,
    uint quantExp,
    int3 minQ,
    uint pagePoolSlabDescriptorIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    return CLodLoadPagePosition(slab, quantExp, positionBitstreamBase, positionBitOffset, meshletLocalVertex);
}

float2 UnpackSnorm16x2(uint packed)
{
    int signedPacked = asint(packed);
    int x = (signedPacked << 16) >> 16;
    int y = signedPacked >> 16;
    float sx = max(-1.0f, (float)x / 32767.0f);
    float sy = max(-1.0f, (float)y / 32767.0f);
    return float2(sx, sy);
}

float3 OctDecodeNormal(float2 e)
{
    float3 v = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    if (v.z < 0.0f)
    {
        float2 folded = (1.0f - abs(v.yx)) * float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
        v.x = folded.x;
        v.y = folded.y;
    }
    return normalize(v);
}

float3 DecodeCompressedNormal(uint meshletLocalVertex, uint normalArrayBase, uint vertexAttributeOffset, uint pagePoolSlabDescriptorIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    uint addr = normalArrayBase + (vertexAttributeOffset + meshletLocalVertex) * 4u;
    uint packed = slab.Load(addr);
    return OctDecodeNormal(UnpackSnorm16x2(packed));
}

float3 DecodeCompressedColor(uint meshletLocalVertex, uint colorArrayBase, uint vertexAttributeOffset, uint pagePoolSlabDescriptorIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    uint addr = colorArrayBase + (vertexAttributeOffset + meshletLocalVertex) * 4u;
    uint packed = slab.Load(addr);
    return float3(
        float(packed & 0xFFu) / 255.0f,
        float((packed >> 8u) & 0xFFu) / 255.0f,
        float((packed >> 16u) & 0xFFu) / 255.0f);
}

SkinningInfluences DecodePackedJoints(uint meshletLocalVertex, MeshletSetup setup)
{
    SkinningInfluences skinning;
    skinning.joints0 = uint4(0, 0, 0, 0);
    skinning.joints1 = uint4(0, 0, 0, 0);
    skinning.weights0 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    skinning.weights1 = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if ((setup.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[setup.pagePoolSlabDescriptorIndex];
    uint addr = setup.jointArrayBase + (setup.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.joints0 = LoadUint4(addr, slab);
    skinning.joints1 = LoadUint4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences DecodePackedWeights(uint meshletLocalVertex, MeshletSetup setup, SkinningInfluences skinning)
{
    if ((setup.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[setup.pagePoolSlabDescriptorIndex];
    uint addr = setup.weightArrayBase + (setup.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.weights0 = LoadFloat4(addr, slab);
    skinning.weights1 = LoadFloat4(addr + 16u, slab);
    return skinning;
}

void ApplyClodSkinningToVertex(uint meshletLocalVertex, MeshletSetup setup, inout Vertex vertex)
{
#if defined(PSO_SKINNED)
    SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, setup);
    skinning = DecodePackedWeights(meshletLocalVertex, setup, skinning);
    ApplySkinningToVertex(setup.meshInstanceBuffer.skinningInstanceSlot, skinning, vertex);
#else
    if ((setup.meshBuffer.vertexFlags & VERTEX_SKINNED) == 0u)
    {
        return;
    }

    SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, setup);
    skinning = DecodePackedWeights(meshletLocalVertex, setup, skinning);
    ApplySkinningToVertex(setup.meshInstanceBuffer.skinningInstanceSlot, skinning, vertex);
#endif
}

float2 DecodeCompressedUV(
    uint meshletLocalVertex,
    uint uvSetIndex,
    MeshletSetup setup)
{
    if (uvSetIndex >= setup.uvSetCount)
    {
        return float2(0.0f, 0.0f);
    }

    CLodMeshletUvDescriptor uvDesc = LoadMeshletUvDescriptorAbsolute(setup, uvSetIndex);
    uint uvBitstreamBase = LoadPageUvBitstreamBaseAbsolute(setup, uvSetIndex);
    uint uvBitsU = CLodUvDescBitsU(uvDesc);
    uint uvBitsV = CLodUvDescBitsV(uvDesc);

    uint bitsPerVertex = uvBitsU + uvBitsV;
    uint bitCursor = uvBitstreamBase * 8u + uvDesc.uvBitOffset + meshletLocalVertex * bitsPerVertex;

    ByteAddressBuffer slab = ResourceDescriptorHeap[setup.pagePoolSlabDescriptorIndex];
    uint encodedU = ReadPackedBits32_BA(slab, bitCursor, uvBitsU);
    bitCursor += uvBitsU;
    uint encodedV = ReadPackedBits32_BA(slab, bitCursor, uvBitsV);

    return float2(
        uvDesc.uvMinU + float(encodedU) * uvDesc.uvScaleU,
        uvDesc.uvMinV + float(encodedV) * uvDesc.uvScaleV);
}

VisBufferPSInput BuildVisBufferVertexAttributesForView(
    Vertex vertex,
    uint3 vGroupID,
    PerObjectBuffer objectBuffer,
    uint viewID,
    uint shadowClipmapIndex,
    uint clusterIndex,
    uint materialDataIndex,
    ClodViewRasterInfo rasterInfo)
{
    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera viewCamera = cameras[viewID];

    float4 pos = float4(vertex.position.xyz, 1.0f);
    float4 worldPosition = mul(pos, objectBuffer.model);
    float4 viewPosition = mul(worldPosition, viewCamera.view);

    VisBufferPSInput result = (VisBufferPSInput)0;
    result.position = mul(viewPosition, viewCamera.projection);
    result.position.x = result.position.x * rasterInfo.viewportScaleX + result.position.w * (rasterInfo.viewportScaleX - 1.0f);
    result.position.y = result.position.y * rasterInfo.viewportScaleY + result.position.w * (1.0f - rasterInfo.viewportScaleY);
    result.linearDepth = -viewPosition.z;
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    StructuredBuffer<SingleMatrix> normalMatrixBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::NormalMatrixBuffer)];
    float3x3 normalMatrix = (float3x3)normalMatrixBuffer[objectBuffer.normalMatrixBufferIndex].value;
    result.positionWorldSpace = worldPosition.xyz;
    result.normalWorldSpace = normalize(mul(vertex.normal, normalMatrix));
    result.color = vertex.color;
    result.materialDataIndex = materialDataIndex;
    result.visibleClusterIndex = clusterIndex;
    result.viewID = viewID;
    result.shadowClipmapIndex = shadowClipmapIndex;
#endif
#if defined(PSO_ALPHA_TEST)
    result.texcoord = vertex.texcoord;
#endif
#if !defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
#if defined(PSO_ALPHA_TEST)
    result.materialDataIndex = materialDataIndex;
#endif
    result.visibleClusterIndex = clusterIndex;
    result.viewID = viewID;
#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    result.shadowClipmapIndex = shadowClipmapIndex;
#endif
#endif

    return result;
}

PSInput GetVertexAttributes(uint blockByteOffset, uint prevBlockByteOffset, uint index, uint flags, uint vertexSize, uint3 vGroupID, PerObjectBuffer objectBuffer) {
    uint byteOffset = blockByteOffset + index * vertexSize;
    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PostSkinningVertices)];
    Vertex vertex = LoadVertex(byteOffset, vertexBuffer, flags);
    
    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    float4 pos = float4(vertex.position.xyz, 1.0f);
    float4 prevPos;
    if (flags & VERTEX_SKINNED)
    {
        uint prevByteOffset = prevBlockByteOffset + index * vertexSize;
        prevPos = float4(LoadFloat3(prevByteOffset, vertexBuffer), 1.0);
    }
    else
    {
        prevPos = float4(vertex.position.xyz, 1.0f);
    }

    float4 worldPosition = mul(pos, objectBuffer.model);
    PSInput result;

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];

    if (flags & VERTEX_TEXCOORDS) {
        result.texcoord = vertex.texcoord;
    }
    
#if defined(PSO_SHADOW)
    StructuredBuffer<LightInfo> lights = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::InfoBuffer)];
    LightInfo light = lights[GetRootCurrentLightID()];
    matrix lightMatrix;
    matrix viewMatrix;
    switch(light.type) {
        case 0: { // Point light
            StructuredBuffer<unsigned int> pointLightCubemapIndicesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::PointLightCubemapBuffer)];
            uint lightCameraIndex = pointLightCubemapIndicesBuffer[GetRootLightViewIndex()];
            Camera lightCamera = cameras[lightCameraIndex];
            lightMatrix = lightCamera.viewProjection;
            viewMatrix = lightCamera.view;
            break;
        }
        case 1: { // Spot light
            StructuredBuffer<unsigned int> spotLightMapIndicesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::SpotLightMatrixBuffer)];
            uint lightCameraIndex = spotLightMapIndicesBuffer[GetRootLightViewIndex()];
            Camera lightCamera = cameras[lightCameraIndex];
            lightMatrix = lightCamera.viewProjection;
            viewMatrix = lightCamera.view;
            break;
        }
        case 2: { // Directional light
            StructuredBuffer<unsigned int> directionalLightCascadeIndicesBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Light::DirectionalLightCascadeBuffer)];
            uint lightCameraIndex = directionalLightCascadeIndicesBuffer[GetRootLightViewIndex()];
            Camera lightCamera = cameras[lightCameraIndex];
            lightMatrix = lightCamera.viewProjection;
            viewMatrix = lightCamera.view;
            break;
        }
    }
    result.position = mul(worldPosition, lightMatrix);
    result.positionViewSpace = mul(worldPosition, viewMatrix);
    return result;
#endif // SHADOW
    
    Camera mainCamera = cameras[perFrameBuffer.mainCameraIndex];
    
    result.positionWorldSpace = worldPosition;
    float4 viewPosition = mul(worldPosition, mainCamera.view);
    result.positionViewSpace = viewPosition;
    result.position = mul(viewPosition, mainCamera.projection);
    result.clipPosition = mul(viewPosition, mainCamera.unjitteredProjection);
    
    float4 prevPosition = mul(prevPos, objectBuffer.prevModel);
    prevPosition = mul(prevPosition, mainCamera.prevView);
    result.prevClipPosition = mul(prevPosition, mainCamera.prevUnjitteredProjection);
    
    StructuredBuffer<SingleMatrix> normalMatrixBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::NormalMatrixBuffer)];
    float3x3 normalMatrix = (float3x3) normalMatrixBuffer[objectBuffer.normalMatrixBufferIndex].value;
    result.normalWorldSpace = normalize(mul(vertex.normal, normalMatrix));
    result.tangentWorldSpace = (flags & VERTEX_TANGENTS)
        ? float4(normalize(mul(vertex.tangent.xyz, normalMatrix)), vertex.tangent.w)
        : float4(1.0f, 0.0f, 0.0f, 0.0f);
    
    result.color = vertex.color;
    
    result.meshletIndex = vGroupID.x;
    
    result.normalModelSpace = normalize(vertex.normal);
    
    return result;
}

VisBufferPSInput GetVisBufferVertexAttributesForView(
    uint blockByteOffset,
    uint index,
    uint flags,
    uint vertexSize,
    uint3 vGroupID,
    PerObjectBuffer objectBuffer,
    uint viewID,
    uint shadowClipmapIndex,
    uint clusterIndex,
    uint materialDataIndex,
    ClodViewRasterInfo rasterInfo)
{
    ByteAddressBuffer vertexBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PostSkinningVertices)];
    uint byteOffset = blockByteOffset + index * vertexSize;
    Vertex vertex = LoadVertex(byteOffset, vertexBuffer, flags);
    return BuildVisBufferVertexAttributesForView(
        vertex,
        vGroupID,
        objectBuffer,
        viewID,
        shadowClipmapIndex,
        clusterIndex,
        materialDataIndex,
        rasterInfo);
}

void WriteTriangles(uint uGroupThreadID, MeshletSetup setup, out indices uint3 outputTriangles[MS_MESHLET_SIZE])
{
    bool reverseWinding = (setup.objectBuffer.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0;
    for (uint t = uGroupThreadID; t < setup.triCount; t += MS_THREAD_GROUP_SIZE)
    {
        uint3 tri = DecodeTriangle(t, setup);
        outputTriangles[t] = reverseWinding ? tri.xzy : tri;
    }
}

// Emit for GBuffer path
void EmitMeshletGBuffer(uint uGroupThreadID, MeshletSetup setup, out vertices PSInput outputVertices[MS_MESHLET_SIZE], out indices uint3 outputTriangles[MS_MESHLET_SIZE])
{
    for (uint i = uGroupThreadID; i < setup.vertCount; i += MS_THREAD_GROUP_SIZE)
    {
        outputVertices[i] = GetVertexAttributes(
            setup.postSkinningBufferOffset,
            setup.prevPostSkinningBufferOffset,
            setup.vertOffset + i,
            setup.meshBuffer.vertexFlags,
            setup.meshBuffer.vertexByteSize,
            setup.meshletIndex,
            setup.objectBuffer);
    }

    WriteTriangles(uGroupThreadID, setup, outputTriangles);
}

// Emit for Visibility Buffer path
void EmitMeshletVisBufferForView(
    uint uGroupThreadID,
    MeshletSetup setup,
    uint viewID,
    uint shadowClipmapIndex,
    uint clusterIndex,
    ClodViewRasterInfo rasterInfo,
    out vertices VisBufferPSInput outputVertices[MS_MESHLET_SIZE],
    out indices uint3 outputTriangles[MS_MESHLET_SIZE])
{
    for (uint i = uGroupThreadID; i < setup.vertCount; i += MS_THREAD_GROUP_SIZE)
    {
        outputVertices[i] = GetVisBufferVertexAttributesForView(
            setup.postSkinningBufferOffset,
            setup.vertOffset + i,
            setup.meshBuffer.vertexFlags,
            setup.meshBuffer.vertexByteSize,
            setup.meshletIndex,
            setup.objectBuffer,
            viewID,
            shadowClipmapIndex,
            clusterIndex,
            setup.meshBuffer.materialDataIndex,
            rasterInfo
        );
    }

    WriteTriangles(uGroupThreadID, setup, outputTriangles);
}

VisBufferPSInput GetVisBufferVertexAttributesForViewCLod(
    uint meshletLocalVertex,
    MeshletSetup setup,
    uint3 vGroupID,
    uint viewID,
    uint shadowClipmapIndex,
    uint clusterIndex,
    ClodViewRasterInfo rasterInfo)
{
#if defined(CLOD_AVBOIT_FORWARD_TRANSPARENT)
    Vertex vertex = (Vertex)0;
    vertex.position = DecodeCompressedPosition(
        meshletLocalVertex,
        setup.positionBitstreamBase,
        setup.positionBitOffset,
        setup.bitsX,
        setup.bitsY,
        setup.bitsZ,
        setup.compressedPositionQuantExp,
        setup.minQ,
        setup.pagePoolSlabDescriptorIndex);
    vertex.normal = DecodeCompressedNormal(
        meshletLocalVertex,
        setup.normalArrayBase,
        setup.vertexAttributeOffset,
        setup.pagePoolSlabDescriptorIndex);
    vertex.texcoord = DecodeCompressedUV(meshletLocalVertex, 0u, setup);
    vertex.color = ((setup.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_COLOR) != 0u)
        ? DecodeCompressedColor(meshletLocalVertex, setup.colorArrayBase, setup.vertexAttributeOffset, setup.pagePoolSlabDescriptorIndex)
        : float3(1.0f, 1.0f, 1.0f);
    ApplyClodSkinningToVertex(meshletLocalVertex, setup, vertex);

    VisBufferPSInput result = BuildVisBufferVertexAttributesForView(
        vertex,
        vGroupID,
        setup.objectBuffer,
        viewID,
        shadowClipmapIndex,
        clusterIndex,
        setup.meshBuffer.materialDataIndex,
        rasterInfo);

    result.uvSet01.xy = vertex.texcoord;
    result.uvSet01.zw = DecodeCompressedUV(meshletLocalVertex, 1u, setup);
    result.uvSet23.xy = DecodeCompressedUV(meshletLocalVertex, 2u, setup);
    result.uvSet23.zw = DecodeCompressedUV(meshletLocalVertex, 3u, setup);
    result.uvSet45.xy = DecodeCompressedUV(meshletLocalVertex, 4u, setup);
    result.uvSet45.zw = DecodeCompressedUV(meshletLocalVertex, 5u, setup);
    result.uvSet67.xy = DecodeCompressedUV(meshletLocalVertex, 6u, setup);
    result.uvSet67.zw = DecodeCompressedUV(meshletLocalVertex, 7u, setup);

    return result;
#else
    float3 position = DecodeCompressedPosition(
        meshletLocalVertex,
        setup.positionBitstreamBase,
        setup.positionBitOffset,
        setup.bitsX,
        setup.bitsY,
        setup.bitsZ,
        setup.compressedPositionQuantExp,
        setup.minQ,
        setup.pagePoolSlabDescriptorIndex);
    if ((setup.meshBuffer.vertexFlags & VERTEX_SKINNED) != 0u)
    {
        SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, setup);
        skinning = DecodePackedWeights(meshletLocalVertex, setup, skinning);
        position = ApplySkinningToPosition(setup.meshInstanceBuffer.skinningInstanceSlot, skinning, position);
    }

    StructuredBuffer<Camera> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CameraBuffer)];
    Camera viewCamera = cameras[viewID];
    float4 worldPosition = mul(float4(position, 1.0f), setup.objectBuffer.model);
    float4 viewPosition = mul(worldPosition, viewCamera.view);

    VisBufferPSInput result = (VisBufferPSInput)0;
    result.position = mul(viewPosition, viewCamera.projection);
    result.position.x = result.position.x * rasterInfo.viewportScaleX + result.position.w * (rasterInfo.viewportScaleX - 1.0f);
    result.position.y = result.position.y * rasterInfo.viewportScaleY + result.position.w * (1.0f - rasterInfo.viewportScaleY);
    result.linearDepth = -viewPosition.z;
#if defined(PSO_ALPHA_TEST)
    result.texcoord = DecodeCompressedUV(meshletLocalVertex, 0u, setup);
    result.materialDataIndex = setup.meshBuffer.materialDataIndex;
#endif
    result.visibleClusterIndex = clusterIndex;
    result.viewID = viewID;
#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    result.shadowClipmapIndex = shadowClipmapIndex;
#endif
    return result;
#endif
}

void EmitMeshletVisBufferForViewCLod(
    uint uGroupThreadID,
    MeshletSetup setup,
    uint emittedVertexCount,
    uint viewID,
    uint shadowClipmapIndex,
    uint clusterIndex,
    ClodViewRasterInfo rasterInfo,
    out vertices VisBufferPSInput outputVertices[MS_MESHLET_SIZE],
    out indices uint3 outputTriangles[MS_MESHLET_SIZE])
{
    for (uint i = uGroupThreadID; i < emittedVertexCount; i += MS_THREAD_GROUP_SIZE)
    {
        // Vertex i is meshlet-local (0..vertexCount-1)
        outputVertices[i] = GetVisBufferVertexAttributesForViewCLod(
            i, setup, setup.meshletIndex, viewID, shadowClipmapIndex, clusterIndex, rasterInfo);
    }

    WriteTriangles(uGroupThreadID, setup, outputTriangles);
}

#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
void CacheMeshletVisBufferVerticesForViewCLod(
    uint uGroupThreadID,
    MeshletSetup setup,
    uint viewID,
    uint shadowClipmapIndex,
    uint clusterIndex,
    ClodViewRasterInfo rasterInfo)
{
    for (uint i = uGroupThreadID; i < setup.vertCount; i += MS_THREAD_GROUP_SIZE)
    {
        const VisBufferPSInput vertex = GetVisBufferVertexAttributesForViewCLod(
            i, setup, setup.meshletIndex, viewID, shadowClipmapIndex, clusterIndex, rasterInfo);
        gs_clodVsmVertexPosition[i] = vertex.position;
        gs_clodVsmLinearDepth[i] = vertex.linearDepth;
#if defined(PSO_ALPHA_TEST)
        gs_clodVsmTexcoord[i] = vertex.texcoord;
#endif
    }
}

void EmitCachedMeshletVisBufferVerticesForViewCLod(
    uint uGroupThreadID,
    MeshletSetup setup,
    uint emittedVertexCount,
    uint viewID,
    uint shadowClipmapIndex,
    uint clusterIndex,
    out vertices VisBufferPSInput outputVertices[MS_MESHLET_SIZE])
{
    for (uint i = uGroupThreadID; i < emittedVertexCount; i += MS_THREAD_GROUP_SIZE)
    {
        VisBufferPSInput vertex = (VisBufferPSInput)0;
        vertex.position = gs_clodVsmVertexPosition[i];
        vertex.linearDepth = gs_clodVsmLinearDepth[i];
#if defined(PSO_ALPHA_TEST)
        vertex.texcoord = gs_clodVsmTexcoord[i];
        vertex.materialDataIndex = setup.meshBuffer.materialDataIndex;
#endif
        vertex.visibleClusterIndex = clusterIndex;
        vertex.viewID = viewID;
#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
        vertex.shadowClipmapIndex = shadowClipmapIndex;
#endif
        outputVertices[i] = vertex;
    }
}

bool ClodTriangleTouchesRenderableVirtualShadowPages(
    uint3 tri,
    ClodViewRasterInfo rasterInfo,
    uint shadowVsmPayload,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable)
{
    const float4 p0 = gs_clodVsmVertexPosition[tri.x];
    const float4 p1 = gs_clodVsmVertexPosition[tri.y];
    const float4 p2 = gs_clodVsmVertexPosition[tri.z];

    // Keep triangles that straddle the near plane rather than risking false rejects from
    // a bbox built from invalid post-divide coordinates.
    if (p0.w <= 0.0f || p1.w <= 0.0f || p2.w <= 0.0f)
    {
        return true;
    }

    const float visWidth = float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX);
    const float visHeight = float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY);
    const float scissorMinX = float(rasterInfo.scissorMinX);
    const float scissorMinY = float(rasterInfo.scissorMinY);

    const float2 ndc0 = p0.xy / p0.w;
    const float2 ndc1 = p1.xy / p1.w;
    const float2 ndc2 = p2.xy / p2.w;

    const float2 s0 = float2(
        (ndc0.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc0.y) * 0.5f * visHeight + scissorMinY);
    const float2 s1 = float2(
        (ndc1.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc1.y) * 0.5f * visHeight + scissorMinY);
    const float2 s2 = float2(
        (ndc2.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc2.y) * 0.5f * visHeight + scissorMinY);

    int2 minPx = int2(floor(min(min(s0, s1), s2)));
    int2 maxPx = int2(floor(max(max(s0, s1), s2)));

    minPx = max(minPx, int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(clipmapInfo.virtualResolution) - 1, int(clipmapInfo.virtualResolution) - 1));
    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
        return false;
    }

    uint2 minPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(minPx), clipmapInfo);
    uint2 maxPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(maxPx), clipmapInfo);

    if (CLodVisibleClusterHasVsmBlockDataFromPayload(shadowVsmPayload))
    {
        const uint2 blockCoord = CLodVisibleClusterVsmBlockCoordFromPayload(shadowVsmPayload);
        const uint2 minTouchedBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(minPageCoords);
        const uint2 maxTouchedBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(maxPageCoords);
        if (any(blockCoord < minTouchedBlockCoord) || any(blockCoord > maxTouchedBlockCoord))
        {
            return false;
        }

        const uint packedActiveRect = CLodVisibleClusterVsmActiveRectFromPayload(shadowVsmPayload);
        const uint2 blockOriginPageCoord = CLodVisibleClusterVsmBlockOriginPageCoordFromPayload(shadowVsmPayload);
        const uint2 blockMaxPageCoord = blockOriginPageCoord + uint2(kCLodVirtualShadowBlockPagesPerAxis - 1u, kCLodVirtualShadowBlockPagesPerAxis - 1u);
        const uint2 activeMinLocalPageCoord = CLodVirtualShadowUnpackBlockActiveRectMin(packedActiveRect);
        const uint2 activeMaxLocalPageCoord = CLodVirtualShadowUnpackBlockActiveRectMax(packedActiveRect);
        const uint2 activeMinPageCoord = blockOriginPageCoord + activeMinLocalPageCoord;
        const uint2 activeMaxPageCoord = blockOriginPageCoord + activeMaxLocalPageCoord;

        if (minPageCoords.x > blockMaxPageCoord.x || minPageCoords.y > blockMaxPageCoord.y ||
            maxPageCoords.x < blockOriginPageCoord.x || maxPageCoords.y < blockOriginPageCoord.y)
        {
            return false;
        }

        if (minPageCoords.x > activeMaxPageCoord.x || minPageCoords.y > activeMaxPageCoord.y ||
            maxPageCoords.x < activeMinPageCoord.x || maxPageCoords.y < activeMinPageCoord.y)
        {
            return false;
        }

        minPageCoords = max(minPageCoords, activeMinPageCoord);
        maxPageCoords = min(maxPageCoords, activeMaxPageCoord);
    }

    return CLodVirtualShadowAnyRenderablePageInPageRect(
        minPageCoords,
        maxPageCoords,
        clipmapInfo,
        pageTable);
}

bool ClodProjectedTriangleTouchesRenderableVirtualShadowPages(
    float4 p0,
    float4 p1,
    float4 p2,
    ClodViewRasterInfo rasterInfo,
    uint shadowVsmPayload,
    CLodVirtualShadowClipmapInfo clipmapInfo,
    RWTexture2DArray<uint> pageTable)
{
    if (p0.w <= 0.0f || p1.w <= 0.0f || p2.w <= 0.0f)
    {
        return true;
    }

    const float visWidth = float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX);
    const float visHeight = float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY);
    const float scissorMinX = float(rasterInfo.scissorMinX);
    const float scissorMinY = float(rasterInfo.scissorMinY);

    const float2 ndc0 = p0.xy / p0.w;
    const float2 ndc1 = p1.xy / p1.w;
    const float2 ndc2 = p2.xy / p2.w;

    const float2 s0 = float2(
        (ndc0.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc0.y) * 0.5f * visHeight + scissorMinY);
    const float2 s1 = float2(
        (ndc1.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc1.y) * 0.5f * visHeight + scissorMinY);
    const float2 s2 = float2(
        (ndc2.x + 1.0f) * 0.5f * visWidth + scissorMinX,
        (1.0f - ndc2.y) * 0.5f * visHeight + scissorMinY);

    int2 minPx = int2(floor(min(min(s0, s1), s2)));
    int2 maxPx = int2(floor(max(max(s0, s1), s2)));

    minPx = max(minPx, int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY));
    maxPx = min(maxPx, int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1));
    minPx = max(minPx, int2(0, 0));
    maxPx = min(maxPx, int2(int(clipmapInfo.virtualResolution) - 1, int(clipmapInfo.virtualResolution) - 1));
    if (minPx.x > maxPx.x || minPx.y > maxPx.y)
    {
        return false;
    }

    uint2 minPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(minPx), clipmapInfo);
    uint2 maxPageCoords = CLodVirtualShadowVirtualPageCoordsFromPixel(uint2(maxPx), clipmapInfo);

    if (CLodVisibleClusterHasVsmBlockDataFromPayload(shadowVsmPayload))
    {
        const uint2 blockCoord = CLodVisibleClusterVsmBlockCoordFromPayload(shadowVsmPayload);
        const uint2 minTouchedBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(minPageCoords);
        const uint2 maxTouchedBlockCoord = CLodVirtualShadowBlockCoordFromPageCoord(maxPageCoords);
        if (any(blockCoord < minTouchedBlockCoord) || any(blockCoord > maxTouchedBlockCoord))
        {
            return false;
        }

        const uint packedActiveRect = CLodVisibleClusterVsmActiveRectFromPayload(shadowVsmPayload);
        const uint2 blockOriginPageCoord = CLodVisibleClusterVsmBlockOriginPageCoordFromPayload(shadowVsmPayload);
        const uint2 blockMaxPageCoord = blockOriginPageCoord + uint2(kCLodVirtualShadowBlockPagesPerAxis - 1u, kCLodVirtualShadowBlockPagesPerAxis - 1u);
        const uint2 activeMinLocalPageCoord = CLodVirtualShadowUnpackBlockActiveRectMin(packedActiveRect);
        const uint2 activeMaxLocalPageCoord = CLodVirtualShadowUnpackBlockActiveRectMax(packedActiveRect);
        const uint2 activeMinPageCoord = blockOriginPageCoord + activeMinLocalPageCoord;
        const uint2 activeMaxPageCoord = blockOriginPageCoord + activeMaxLocalPageCoord;

        if (minPageCoords.x > blockMaxPageCoord.x || minPageCoords.y > blockMaxPageCoord.y ||
            maxPageCoords.x < blockOriginPageCoord.x || maxPageCoords.y < blockOriginPageCoord.y)
        {
            return false;
        }

        if (minPageCoords.x > activeMaxPageCoord.x || minPageCoords.y > activeMaxPageCoord.y ||
            maxPageCoords.x < activeMinPageCoord.x || maxPageCoords.y < activeMinPageCoord.y)
        {
            return false;
        }

        minPageCoords = max(minPageCoords, activeMinPageCoord);
        maxPageCoords = min(maxPageCoords, activeMaxPageCoord);
    }

    return CLodVirtualShadowAnyRenderablePageInPageRect(
        minPageCoords,
        maxPageCoords,
        clipmapInfo,
        pageTable);
}

VisBufferPSInput ReyesShadowVisVertexToPSInput(ReyesShadowVisVertex vertex)
{
    VisBufferPSInput output = (VisBufferPSInput)0;
    output.position = vertex.position;
    output.linearDepth = vertex.linearDepth;
#if defined(PSO_ALPHA_TEST)
    output.texcoord = vertex.texcoord;
    output.materialDataIndex = gs_reyesShadowSetup.meshBuffer.materialDataIndex;
#endif
    output.visibleClusterIndex = gs_reyesShadowDiceEntry.visibleClusterIndex;
    output.viewID = gs_reyesShadowSetup.viewID;
#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    output.shadowClipmapIndex = gs_reyesShadowSetup.virtualShadowPayload;
#endif
    return output;
}

uint3 DecodeCLodTriangle(MeshletSetup setup, uint triLocalIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[setup.pagePoolSlabDescriptorIndex];
    const uint triOffset = setup.triangleStreamBase + setup.triangleByteOffset + triLocalIndex * 3u;
    const uint alignedOffset = (triOffset / 4u) * 4u;
    const uint firstWord = slab.Load(alignedOffset);
    const uint byteOffset = triOffset % 4u;

    const uint b0 = (firstWord >> (byteOffset * 8u)) & 0xFFu;
    uint b1 = 0u;
    uint b2 = 0u;

    if (byteOffset <= 1u)
    {
        b1 = (firstWord >> ((byteOffset + 1u) * 8u)) & 0xFFu;
        b2 = (firstWord >> ((byteOffset + 2u) * 8u)) & 0xFFu;
    }
    else if (byteOffset == 2u)
    {
        b1 = (firstWord >> 24u) & 0xFFu;
        const uint secondWord = slab.Load(alignedOffset + 4u);
        b2 = secondWord & 0xFFu;
    }
    else
    {
        const uint secondWord = slab.Load(alignedOffset + 4u);
        b1 = secondWord & 0xFFu;
        b2 = (secondWord >> 8u) & 0xFFu;
    }

    return uint3(b0, b1, b2);
}

void EmitFilteredMeshletTriangles(
    uint uGroupThreadID,
    MeshletSetup setup,
    out indices uint3 outputTriangles[MS_MESHLET_SIZE],
    out primitives VisibilityPerPrimitive primitiveInfo[MS_MESHLET_SIZE])
{
    const bool reverseWinding = (setup.objectBuffer.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0;
    for (uint t = uGroupThreadID; t < setup.triCount; t += MS_THREAD_GROUP_SIZE)
    {
        const uint outputIndex = gs_clodVsmTriangleOutputIndex[t];
        if (outputIndex == kClodInvalidTriangleOutputIndex)
        {
            continue;
        }

        const uint3 tri = DecodeCLodTriangle(setup, t);
        outputTriangles[outputIndex] = reverseWinding ? tri.xzy : tri;
        primitiveInfo[outputIndex].triangleIndex = t;
    }
}
#endif

void EmitPrimitiveIDs(
    uint uGroupThreadID,
    MeshletSetup setup,
    out primitives VisibilityPerPrimitive primitiveInfo[MS_MESHLET_SIZE])
{
    for (uint t = uGroupThreadID; t < setup.triCount; t += MS_THREAD_GROUP_SIZE)
    {
        primitiveInfo[t].triangleIndex = t;
    }
}

// Old AS+MS entries
[outputtopology("triangle")]
[numthreads(MS_THREAD_GROUP_SIZE, 1, 1)]
void MSMain(
    const uint uGroupThreadID : SV_GroupThreadID,
    const uint vGroupID : SV_GroupID,
    in payload Payload payload,
    out vertices PSInput outputVertices[MS_MESHLET_SIZE],
    out indices uint3 outputTriangles[MS_MESHLET_SIZE])
{
    uint meshletIndex = payload.MeshletIndices[vGroupID];
    MeshletSetup setup = (MeshletSetup)0;
    bool draw = InitializeMeshlet(meshletIndex, setup);
    if (!draw)
    {
        setup = (MeshletSetup)0;
    }
    SetMeshOutputCounts(setup.vertCount, setup.triCount);
    EmitMeshletGBuffer(uGroupThreadID, setup, outputVertices, outputTriangles);
}

[outputtopology("triangle")]
[numthreads(MS_THREAD_GROUP_SIZE, 1, 1)]
void VisibilityBufferMSMain(
    const uint uGroupThreadID : SV_GroupThreadID,
    const uint vGroupID : SV_GroupID,
    in payload Payload payload,
    out vertices VisBufferPSInput outputVertices[MS_MESHLET_SIZE],
    out indices uint3 outputTriangles[MS_MESHLET_SIZE],
    out primitives VisibilityPerPrimitive primitiveInfo[MS_MESHLET_SIZE])
{
    uint meshletIndex = payload.MeshletIndices[vGroupID];
    MeshletSetup setup = (MeshletSetup)0;
    bool draw = InitializeMeshlet(meshletIndex, setup);
    if (!draw)
    {
        setup = (MeshletSetup)0;
    }
    ClodViewRasterInfo viewRasterInfo = (ClodViewRasterInfo)0;
    if (draw)
    {
        StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
        viewRasterInfo = viewRasterInfoBuffer[setup.viewID];
    }
    SetMeshOutputCounts(setup.vertCount, setup.triCount);
    EmitMeshletVisBufferForView(
        uGroupThreadID,
        setup,
        setup.viewID,
        setup.shadowClipmapIndex,
        0,
        viewRasterInfo,
        outputVertices,
        outputTriangles);
    EmitPrimitiveIDs(uGroupThreadID, setup, primitiveInfo);
}

bool InitializeMeshletFromCompactedCluster(uint4 packedCluster, out MeshletSetup setup, out uint failureReason, out bool sourceGroupMismatch, out uint foundSourceGroupLocalIndex, in uint bucketMeshletIndex, in uint bucketCount)
{
    failureReason = CLOD_RASTER_INIT_FAILURE_NONE;
    sourceGroupMismatch = false;
    foundSourceGroupLocalIndex = 0xFFFFFFFFu;

    setup.meshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    setup.meshInstanceBuffer = LoadMeshTemplateForDraw(drawRecordIndex);
    setup.viewID = CLodVisibleClusterViewID(packedCluster);
    setup.shadowClipmapIndex = CLodVisibleClusterShadowClipmapIndex(packedCluster);
    setup.virtualShadowPayload = CLodVisibleClusterVsmPayload(packedCluster);

    StructuredBuffer<PerMeshBuffer> perMeshBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];

    setup.meshBuffer = perMeshBuffer[setup.meshInstanceBuffer.perMeshBufferIndex];
    setup.objectBuffer = LoadInstanceTransformForDraw(drawRecordIndex);

    // Use pre-resolved page address from VisibleCluster
    const uint pageSlabDesc = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabOff  = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    if (pageSlabDesc == 0)
    {
        failureReason = CLOD_RASTER_INIT_FAILURE_ZERO_PAGE_SLAB;
        return false;
    }

    CLodPageHeader hdr = LoadPageHeader(pageSlabDesc, pageSlabOff);

    // meshletIndex is now page-local
    if (setup.meshletIndex >= hdr.meshletCount)
    {
        failureReason = CLOD_RASTER_INIT_FAILURE_MESHLET_OOB;
        return false;
    }

    // Load per-meshlet descriptor
    CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDesc, pageSlabOff, hdr.descriptorOffset, setup.meshletIndex);
#if CLOD_ENABLE_SOURCE_GROUP_VALIDATION
    const uint expectedGroupLocalIndex = CLodVisibleClusterGroupID(packedCluster);
    foundSourceGroupLocalIndex = desc.sourceGroupLocalIndex;
    sourceGroupMismatch =
        desc.sourceGroupLocalIndex != 0xFFFFFFFFu &&
        desc.sourceGroupLocalIndex != expectedGroupLocalIndex;
#else
#endif

    setup.meshlet = (Meshlet)0;
    setup.vertCount = CLodDescVertexCount(desc);
    setup.triCount = CLodDescTriangleCount(desc);
    if (!HasValidMeshShaderOutputCounts(setup.vertCount, setup.triCount))
    {
        failureReason = CLOD_RASTER_INIT_FAILURE_INVALID_OUTPUT_COUNTS;
        return false;
    }
    setup.vertOffset = 0;

    // Per-meshlet page stream addressing from descriptor
    setup.bitsX = CLodDescBitsX(desc);
    setup.bitsY = CLodDescBitsY(desc);
    setup.bitsZ = CLodDescBitsZ(desc);
    setup.minQ = int3(desc.minQx, desc.minQy, desc.minQz);
    setup.positionBitOffset = desc.positionBitOffset;
    setup.vertexAttributeOffset = desc.vertexAttributeOffset;
    setup.triangleByteOffset = desc.triangleByteOffset;
    setup.boneListOffset = desc.boneListOffset;
    setup.boneCount = CLodDescBoneCount(desc);
    setup.pageAttributeMask = hdr.attributeMask;
    setup.uvSetCount = hdr.uvSetCount;

    // Page-level stream base offsets (absolute in slab)
    setup.pageByteOffset = pageSlabOff;
    setup.positionBitstreamBase = pageSlabOff + hdr.positionBitstreamOffset;
    setup.normalArrayBase = pageSlabOff + hdr.normalArrayOffset;
    setup.colorArrayBase = pageSlabOff + hdr.colorArrayOffset;
    setup.jointArrayBase = pageSlabOff + hdr.jointArrayOffset;
    setup.weightArrayBase = pageSlabOff + hdr.weightArrayOffset;
    setup.uvDescriptorBase = pageSlabOff + hdr.uvDescriptorOffset;
    setup.uvBitstreamDirectoryBase = pageSlabOff + hdr.uvBitstreamDirectoryOffset;
    setup.triangleStreamBase = pageSlabOff + hdr.triangleStreamOffset;
    setup.boneIndexStreamBase = pageSlabOff + hdr.boneIndexStreamOffset;

    setup.compressedPositionQuantExp = hdr.compressedPositionQuantExp;
    setup.pagePoolSlabDescriptorIndex = pageSlabDesc;

    // Non-CLod fields unused
    setup.groupMeshletTrianglesByteOffset = 0;
    setup.postSkinningBufferOffset = 0;
    setup.prevPostSkinningBufferOffset = 0;

    return true;
}

// New pure-MS entry for Cluster LOD rendering
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MS_THREAD_GROUP_SIZE, 1, 1)]
void ClusterLODBucketMSMain(
    const uint uGroupThreadID : SV_GroupThreadID,
    const uint3 vGroupID : SV_GroupID,
    out vertices VisBufferPSInput outputVertices[MS_MESHLET_SIZE],
    out indices uint3 outputTriangles[MS_MESHLET_SIZE],
    out primitives VisibilityPerPrimitive primitiveInfo[MS_MESHLET_SIZE])
{
    // From command signature
    uint baseOffset = IndirectCommandSignatureRootConstant0;
    uint dispatchX = IndirectCommandSignatureRootConstant1;
    uint bucketIndex = IndirectCommandSignatureRootConstant2;

    uint linearizedID = vGroupID.x + vGroupID.y * dispatchX;

    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> sortedToUnsortedMapping = ResourceDescriptorHeap[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX];
    uint count = histogram[bucketIndex];

    bool draw = linearizedID < count;
    uint4 packedCluster = uint4(0, 0, 0, CLOD_PACKED_VISIBLE_CLUSTER_INVALID_SHADOW_CLIPMAP_INDEX);
    MeshletSetup setup = (MeshletSetup)0;
    uint visibleClusterIndex = baseOffset + linearizedID;
    uint unsortedClusterIndex = 0;
    uint outputVertCount = 0;
    uint outputTriCount = 0;
    ClodViewRasterInfo viewRasterInfo = (ClodViewRasterInfo)0;
    uint initFailureReason = CLOD_RASTER_INIT_FAILURE_NONE;
    bool sourceGroupMismatch = false;
    uint foundSourceGroupLocalIndex = 0xFFFFFFFFu;

    if (draw) {
        ByteAddressBuffer compactedClusters = ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
        packedCluster = CLodLoadVisibleClusterPacked(compactedClusters, visibleClusterIndex);
        unsortedClusterIndex = sortedToUnsortedMapping[visibleClusterIndex];
        draw = InitializeMeshletFromCompactedCluster(packedCluster, setup, initFailureReason, sourceGroupMismatch, foundSourceGroupLocalIndex, linearizedID, count);
        if (!draw)
        {
            setup = (MeshletSetup)0;
        }
    }

    if (uGroupThreadID == 0u)
    {
        CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_GROUPS, 1u);
        if (linearizedID < count)
        {
            CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_IN_RANGE, 1u);
            if (!draw)
            {
                CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED, 1u);
                if (initFailureReason == CLOD_RASTER_INIT_FAILURE_ZERO_PAGE_SLAB)
                {
                    CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_ZERO_PAGE_SLAB, 1u);
                }
                else if (initFailureReason == CLOD_RASTER_INIT_FAILURE_MESHLET_OOB)
                {
                    CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_MESHLET_OOB, 1u);
                }
                else if (initFailureReason == CLOD_RASTER_INIT_FAILURE_INVALID_OUTPUT_COUNTS)
                {
                    CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_INIT_FAILED_INVALID_OUTPUT_COUNTS, 1u);
                }
            }
#if CLOD_ENABLE_SOURCE_GROUP_VALIDATION
            if (sourceGroupMismatch)
            {
                CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_SOURCE_GROUP_MISMATCH, 1u);
                CLodRecordSourceGroupMismatch(
                    CLodVisibleClusterGroupID(packedCluster),
                    foundSourceGroupLocalIndex,
                    CLodVisibleClusterLocalMeshletIndex(packedCluster),
                    CLodVisibleClusterPageSlabDescriptorIndex(packedCluster),
                    CLodVisibleClusterPageSlabByteOffset(packedCluster),
                    visibleClusterIndex,
                    unsortedClusterIndex,
                    CLodVisibleClusterInstanceID(packedCluster),
                    CLodVisibleClusterViewID(packedCluster),
                    linearizedID,
                    count);
            }
#endif
        }
    }

#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
    if (draw)
    {
        viewRasterInfo = viewRasterInfoBuffer[setup.viewID];

        if (uGroupThreadID == 0u)
        {
            gs_clodVsmKeptTriangleCount = 0u;
            gs_clodVsmHasClipmapInfo = 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        CacheMeshletVisBufferVerticesForViewCLod(
            uGroupThreadID,
            setup,
            setup.viewID,
            setup.virtualShadowPayload,
            unsortedClusterIndex,
            viewRasterInfo);
        GroupMemoryBarrierWithGroupSync();

        if (uGroupThreadID == 0u)
        {
            const uint clipmapIndex = setup.shadowClipmapIndex;
            if (clipmapIndex < kCLodVirtualShadowClipmapCount)
            {
                StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
                    ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
                const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[clipmapIndex];
                if (CLodVirtualShadowClipmapIsValid(clipmapInfo) &&
                    clipmapInfo.shadowCameraBufferIndex == setup.viewID)
                {
                    gs_clodVsmHasClipmapInfo = 1u;
                    gs_clodVsmClipmapInfo = clipmapInfo;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
        for (uint t = uGroupThreadID; t < setup.triCount; t += MS_THREAD_GROUP_SIZE)
        {
            gs_clodVsmTriangleOutputIndex[t] = kClodInvalidTriangleOutputIndex;

            uint3 tri = DecodeCLodTriangle(setup, t);
            if ((setup.objectBuffer.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u)
            {
                tri = tri.xzy;
            }

            if (gs_clodVsmHasClipmapInfo != 0u &&
                ClodTriangleTouchesRenderableVirtualShadowPages(
                    tri,
                    viewRasterInfo,
                    setup.virtualShadowPayload,
                    gs_clodVsmClipmapInfo,
                    pageTable))
            {
                uint outputIndex = 0u;
                InterlockedAdd(gs_clodVsmKeptTriangleCount, 1u, outputIndex);
                gs_clodVsmTriangleOutputIndex[t] = outputIndex;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        outputTriCount = gs_clodVsmKeptTriangleCount;
        outputVertCount = outputTriCount > 0u ? setup.vertCount : 0u;
    }

    SetMeshOutputCounts(outputVertCount, outputTriCount);
    if (uGroupThreadID == 0u && draw)
    {
        CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_OUTPUT_TRIANGLES, outputTriCount);
        if (outputTriCount == 0u)
        {
            CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_ZERO_TRIANGLE_OUTPUTS, 1u);
        }
    }
    EmitCachedMeshletVisBufferVerticesForViewCLod(
        uGroupThreadID,
        setup,
        outputVertCount,
        setup.viewID,
        setup.virtualShadowPayload,
        unsortedClusterIndex,
        outputVertices);
    EmitFilteredMeshletTriangles(uGroupThreadID, setup, outputTriangles, primitiveInfo);
#else
    if (draw)
    {
        viewRasterInfo = viewRasterInfoBuffer[setup.viewID];
        outputVertCount = setup.vertCount;
        outputTriCount = setup.triCount;
    }

    SetMeshOutputCounts(outputVertCount, outputTriCount);
    if (uGroupThreadID == 0u && draw)
    {
        CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_OUTPUT_TRIANGLES, outputTriCount);
        if (outputTriCount == 0u)
        {
            CLodRasterTelemetryAdd(WG_COUNTER_RASTER_MESH_SHADER_ZERO_TRIANGLE_OUTPUTS, 1u);
        }
    }
    EmitMeshletVisBufferForViewCLod(
        uGroupThreadID,
        setup,
        outputVertCount,
        setup.viewID,
        setup.virtualShadowPayload,
        unsortedClusterIndex,
        viewRasterInfo,
        outputVertices,
        outputTriangles);
    EmitPrimitiveIDs(uGroupThreadID, setup, primitiveInfo);
#endif
}

#if CLOD_RASTER_OUTPUT_VIRTUAL_SHADOW
Vertex DecodeReyesSourceVertex(MeshletSetup setup, uint meshletLocalVertex, uint uvSetIndex)
{
    Vertex vertex = (Vertex)0;
    vertex.position = DecodeCompressedPosition(
        meshletLocalVertex,
        setup.positionBitstreamBase,
        setup.positionBitOffset,
        setup.bitsX,
        setup.bitsY,
        setup.bitsZ,
        setup.compressedPositionQuantExp,
        setup.minQ,
        setup.pagePoolSlabDescriptorIndex);
    vertex.normal = DecodeCompressedNormal(
        meshletLocalVertex,
        setup.normalArrayBase,
        setup.vertexAttributeOffset,
        setup.pagePoolSlabDescriptorIndex);
    vertex.texcoord = DecodeCompressedUV(meshletLocalVertex, uvSetIndex, setup);
    ApplyClodSkinningToVertex(meshletLocalVertex, setup, vertex);
    return vertex;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(MS_THREAD_GROUP_SIZE, 1, 1)]
void ClusterLODReyesVirtualShadowMSMain(
    const uint uGroupThreadID : SV_GroupThreadID,
    const uint3 vGroupID : SV_GroupID,
    out vertices VisBufferPSInput outputVertices[kClodReyesShadowMaxOutputVertices],
    out indices uint3 outputTriangles[kClodReyesShadowMaxOutputTriangles],
    out primitives VisibilityPerPrimitive primitiveInfo[kClodReyesShadowMaxOutputTriangles])
{
    const uint baseOffset = IndirectCommandSignatureRootConstant0;
    const uint dispatchX = IndirectCommandSignatureRootConstant1;
    const uint bucketIndex = IndirectCommandSignatureRootConstant2;
    const uint linearizedID = vGroupID.x + vGroupID.y * dispatchX;

    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesPackedRasterWorkGroupEntry> packedRasterWorkGroups = ResourceDescriptorHeap[CLOD_RASTER_REYES_PACKED_RASTER_WORK_GROUPS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> compactedRasterWorkIndices = ResourceDescriptorHeap[CLOD_RASTER_REYES_COMPACTED_RASTER_WORK_INDICES_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesRasterWorkEntry> rasterWorkBuffer = ResourceDescriptorHeap[CLOD_RASTER_REYES_RASTER_WORK_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesDiceQueueEntry> diceQueueBuffer = ResourceDescriptorHeap[CLOD_RASTER_REYES_DICE_QUEUE_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodReyesTessTableConfigEntry> tessTableConfigs = ResourceDescriptorHeap[CLOD_RASTER_REYES_TESS_TABLE_CONFIGS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableVertices = ResourceDescriptorHeap[CLOD_RASTER_REYES_TESS_TABLE_VERTICES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> tessTableTriangles = ResourceDescriptorHeap[CLOD_RASTER_REYES_TESS_TABLE_TRIANGLES_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer = ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<MaterialInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    StructuredBuffer<CullingCameraInfo> cameras = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    ConstantBuffer<PerFrameBuffer> perFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    RWStructuredBuffer<CLodReyesTelemetry> telemetryBuffer = ResourceDescriptorHeap[CLOD_RASTER_REYES_TELEMETRY_DESCRIPTOR_INDEX];
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];

    if (uGroupThreadID == 0u)
    {
        gs_reyesShadowOutputVertexCount = 0u;
        gs_reyesShadowOutputTriangleCount = 0u;
        gs_reyesShadowDispatchValid = 0u;
        gs_reyesShadowCurrentEntryValid = 0u;
        gs_reyesShadowPackedWorkGroup = (CLodReyesPackedRasterWorkGroupEntry)0;

        const uint packedGroupCount = histogram[bucketIndex];
        if (linearizedID < packedGroupCount)
        {
            gs_reyesShadowPackedWorkGroup = packedRasterWorkGroups[baseOffset + linearizedID];
            gs_reyesShadowDispatchValid = gs_reyesShadowPackedWorkGroup.rasterWorkEntryCount > 0u ? 1u : 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gs_reyesShadowDispatchValid != 0u && uGroupThreadID == 0u)
    {
        InterlockedAdd(telemetryBuffer[0].hardwareRasterMeshGroupCount, 1u);
        InterlockedAdd(telemetryBuffer[0].hardwareRasterRequestedMicroTriangleCount, gs_reyesShadowPackedWorkGroup.requestedMicroTriangleCount);
        InterlockedAdd(telemetryBuffer[0].hardwareRasterPackedWorkEntryCount, gs_reyesShadowPackedWorkGroup.rasterWorkEntryCount);
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint packedEntryIndex = 0u; packedEntryIndex < gs_reyesShadowPackedWorkGroup.rasterWorkEntryCount; ++packedEntryIndex)
    {
        if (uGroupThreadID == 0u)
        {
            gs_reyesShadowCurrentEntryValid = 0u;

            const uint compactedWorkIndex = gs_reyesShadowPackedWorkGroup.firstCompactedRasterWorkIndex + packedEntryIndex;
            const uint rasterWorkIndex = compactedRasterWorkIndices[compactedWorkIndex];
            const CLodReyesRasterWorkEntry rasterWorkEntry = rasterWorkBuffer[rasterWorkIndex];
            if (rasterWorkEntry.rasterBucketIndex == bucketIndex)
            {
                gs_reyesShadowDiceEntry = diceQueueBuffer[rasterWorkEntry.diceQueueIndex];
                ByteAddressBuffer visibleClusters = ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
                const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusters, gs_reyesShadowDiceEntry.visibleClusterIndex);

                uint initFailureReason = CLOD_RASTER_INIT_FAILURE_NONE;
                bool sourceGroupMismatch = false;
                uint foundSourceGroupLocalIndex = 0xFFFFFFFFu;
                if (InitializeMeshletFromCompactedCluster(packedCluster, gs_reyesShadowSetup, initFailureReason, sourceGroupMismatch, foundSourceGroupLocalIndex, compactedWorkIndex, gs_reyesShadowPackedWorkGroup.rasterWorkEntryCount))
                {
                    const uint shadowClipmapIndex = CLodVisibleClusterShadowClipmapIndex(packedCluster);
                    if (shadowClipmapIndex < kCLodVirtualShadowClipmapCount)
                    {
                        const ClodViewRasterInfo viewRasterInfo = viewRasterInfoBuffer[gs_reyesShadowSetup.viewID];
                        const CLodVirtualShadowClipmapInfo clipmapInfo = clipmapInfos[shadowClipmapIndex];
                        if (CLodVirtualShadowClipmapIsValid(clipmapInfo) &&
                            clipmapInfo.shadowCameraBufferIndex == gs_reyesShadowSetup.viewID &&
                            viewRasterInfo.scissorMaxX != 0u &&
                            viewRasterInfo.scissorMaxY != 0u)
                        {
                            const uint sourceTriangleIndex = gs_reyesShadowDiceEntry.sourcePrimitiveAndSplitConfig & 0xFFFFu;
                            if (sourceTriangleIndex < gs_reyesShadowSetup.triCount)
                            {
                                gs_reyesShadowSourceTriangle = DecodeCLodTriangle(gs_reyesShadowSetup, sourceTriangleIndex);

                                const Vertex sourceVertex0 = DecodeReyesSourceVertex(gs_reyesShadowSetup, gs_reyesShadowSourceTriangle.x, 0u);
                                const Vertex sourceVertex1 = DecodeReyesSourceVertex(gs_reyesShadowSetup, gs_reyesShadowSourceTriangle.y, 0u);
                                const Vertex sourceVertex2 = DecodeReyesSourceVertex(gs_reyesShadowSetup, gs_reyesShadowSourceTriangle.z, 0u);

                                gs_reyesShadowSourcePositions[0u] = sourceVertex0.position;
                                gs_reyesShadowSourcePositions[1u] = sourceVertex1.position;
                                gs_reyesShadowSourcePositions[2u] = sourceVertex2.position;
                                gs_reyesShadowSourceNormals[0u] = sourceVertex0.normal;
                                gs_reyesShadowSourceNormals[1u] = sourceVertex1.normal;
                                gs_reyesShadowSourceNormals[2u] = sourceVertex2.normal;
                                gs_reyesShadowSourceTexcoords[0u] = sourceVertex0.texcoord;
                                gs_reyesShadowSourceTexcoords[1u] = sourceVertex1.texcoord;
                                gs_reyesShadowSourceTexcoords[2u] = sourceVertex2.texcoord;
                                gs_reyesShadowDomainBarycentrics[0u] = ReyesPatchDomainUVToBarycentrics(gs_reyesShadowDiceEntry.domainVertex0UV);
                                gs_reyesShadowDomainBarycentrics[1u] = ReyesPatchDomainUVToBarycentrics(gs_reyesShadowDiceEntry.domainVertex1UV);
                                gs_reyesShadowDomainBarycentrics[2u] = ReyesPatchDomainUVToBarycentrics(gs_reyesShadowDiceEntry.domainVertex2UV);
                                gs_reyesShadowViewRasterInfo = viewRasterInfo;
                                gs_reyesShadowClipmapInfo = clipmapInfo;

                                const uint microTriangleCount = ReyesGetDicePatchMicroTriangleCount(tessTableConfigs, gs_reyesShadowDiceEntry);
                                gs_reyesShadowMicroTriangleStart = rasterWorkEntry.microTriangleOffset;
                                gs_reyesShadowMicroTriangleEnd = min(rasterWorkEntry.microTriangleOffset + rasterWorkEntry.microTriangleCount, microTriangleCount);
                                gs_reyesShadowCurrentEntryValid = gs_reyesShadowMicroTriangleStart < gs_reyesShadowMicroTriangleEnd ? 1u : 0u;
                            }
                        }
                    }
                }
            }
        }

        GroupMemoryBarrierWithGroupSync();

        if (gs_reyesShadowCurrentEntryValid != 0u)
        {
            const MaterialInfo materialInfo = materials[gs_reyesShadowSetup.meshBuffer.materialDataIndex];
            const bool displacementEnabled = materialInfo.geometricDisplacementEnabled != 0u;
            const bool reverseWinding = (gs_reyesShadowSetup.objectBuffer.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u;
            const CullingCameraInfo camera = cameras[gs_reyesShadowSetup.viewID];
            const float4 modelViewZ = mul(gs_reyesShadowSetup.objectBuffer.model, camera.viewZ);
            const float patchDepth = max(
                ( -dot(float4(gs_reyesShadowSourcePositions[0u], 1.0f), modelViewZ)
                + -dot(float4(gs_reyesShadowSourcePositions[1u], 1.0f), modelViewZ)
                + -dot(float4(gs_reyesShadowSourcePositions[2u], 1.0f), modelViewZ)) / 3.0f,
                max(camera.zNear, 1.0e-3f));

            for (uint microTriangleIndex = gs_reyesShadowMicroTriangleStart + uGroupThreadID;
                 microTriangleIndex < gs_reyesShadowMicroTriangleEnd;
                 microTriangleIndex += MS_THREAD_GROUP_SIZE)
            {
                float3 patchBary0;
                float3 patchBary1;
                float3 patchBary2;
                ReyesDecodeMicroTrianglePatchDomain(
                    tessTableConfigs,
                    tessTableVertices,
                    tessTableTriangles,
                    microTriangleIndex,
                    gs_reyesShadowDiceEntry,
                    patchBary0,
                    patchBary1,
                    patchBary2);

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
                    gs_reyesShadowSetup.objectBuffer.model,
                    patchDepth,
                    gs_reyesShadowSourcePositions[0u],
                    gs_reyesShadowSourcePositions[1u],
                    gs_reyesShadowSourcePositions[2u],
                    gs_reyesShadowSourceNormals[0u],
                    gs_reyesShadowSourceNormals[1u],
                    gs_reyesShadowSourceNormals[2u],
                    gs_reyesShadowSourceTexcoords[0u],
                    gs_reyesShadowSourceTexcoords[1u],
                    gs_reyesShadowSourceTexcoords[2u],
                    gs_reyesShadowDomainBarycentrics[0u],
                    gs_reyesShadowDomainBarycentrics[1u],
                    gs_reyesShadowDomainBarycentrics[2u],
                    patchBary0,
                    patchBary1,
                    patchBary2,
                    sourceBary0,
                    sourceBary1,
                    sourceBary2,
                    patchPosition0,
                    patchPosition1,
                    patchPosition2);

                Vertex patchVertex0 = (Vertex)0;
                patchVertex0.position = patchPosition0;
                patchVertex0.texcoord = ReyesInterpolateFloat2Precise(
                    gs_reyesShadowSourceTexcoords[0u],
                    gs_reyesShadowSourceTexcoords[1u],
                    gs_reyesShadowSourceTexcoords[2u],
                    sourceBary0);
                Vertex patchVertex1 = (Vertex)0;
                patchVertex1.position = patchPosition1;
                patchVertex1.texcoord = ReyesInterpolateFloat2Precise(
                    gs_reyesShadowSourceTexcoords[0u],
                    gs_reyesShadowSourceTexcoords[1u],
                    gs_reyesShadowSourceTexcoords[2u],
                    sourceBary1);
                Vertex patchVertex2 = (Vertex)0;
                patchVertex2.position = patchPosition2;
                patchVertex2.texcoord = ReyesInterpolateFloat2Precise(
                    gs_reyesShadowSourceTexcoords[0u],
                    gs_reyesShadowSourceTexcoords[1u],
                    gs_reyesShadowSourceTexcoords[2u],
                    sourceBary2);

                const VisBufferPSInput visVertex0 = BuildVisBufferVertexAttributesForView(
                    patchVertex0,
                    uint3(gs_reyesShadowSetup.meshletIndex, gs_reyesShadowSetup.meshletIndex, gs_reyesShadowSetup.meshletIndex),
                    gs_reyesShadowSetup.objectBuffer,
                    gs_reyesShadowSetup.viewID,
                    gs_reyesShadowSetup.virtualShadowPayload,
                    gs_reyesShadowDiceEntry.visibleClusterIndex,
                    gs_reyesShadowSetup.meshBuffer.materialDataIndex,
                    gs_reyesShadowViewRasterInfo);
                const VisBufferPSInput visVertex1 = BuildVisBufferVertexAttributesForView(
                    patchVertex1,
                    uint3(gs_reyesShadowSetup.meshletIndex, gs_reyesShadowSetup.meshletIndex, gs_reyesShadowSetup.meshletIndex),
                    gs_reyesShadowSetup.objectBuffer,
                    gs_reyesShadowSetup.viewID,
                    gs_reyesShadowSetup.virtualShadowPayload,
                    gs_reyesShadowDiceEntry.visibleClusterIndex,
                    gs_reyesShadowSetup.meshBuffer.materialDataIndex,
                    gs_reyesShadowViewRasterInfo);
                const VisBufferPSInput visVertex2 = BuildVisBufferVertexAttributesForView(
                    patchVertex2,
                    uint3(gs_reyesShadowSetup.meshletIndex, gs_reyesShadowSetup.meshletIndex, gs_reyesShadowSetup.meshletIndex),
                    gs_reyesShadowSetup.objectBuffer,
                    gs_reyesShadowSetup.viewID,
                    gs_reyesShadowSetup.virtualShadowPayload,
                    gs_reyesShadowDiceEntry.visibleClusterIndex,
                    gs_reyesShadowSetup.meshBuffer.materialDataIndex,
                    gs_reyesShadowViewRasterInfo);

                if (!ClodProjectedTriangleTouchesRenderableVirtualShadowPages(
                        visVertex0.position,
                        visVertex1.position,
                        visVertex2.position,
                        gs_reyesShadowViewRasterInfo,
                        gs_reyesShadowSetup.virtualShadowPayload,
                        gs_reyesShadowClipmapInfo,
                        pageTable))
                {
                    continue;
                }

                uint triangleOutputIndex = 0u;
                InterlockedAdd(gs_reyesShadowOutputTriangleCount, 1u, triangleOutputIndex);
                if (triangleOutputIndex >= kClodReyesShadowMaxOutputTriangles)
                {
                    InterlockedAdd(telemetryBuffer[0].rasterMicroTriangleOverflowCount, 1u);
                    continue;
                }

                const uint vertexBase = triangleOutputIndex * 3u;

                gs_reyesShadowVertices[vertexBase + 0u].position = visVertex0.position;
                gs_reyesShadowVertices[vertexBase + 0u].linearDepth = visVertex0.linearDepth;
#if defined(PSO_ALPHA_TEST)
                gs_reyesShadowVertices[vertexBase + 0u].texcoord = visVertex0.texcoord;
#endif

                gs_reyesShadowVertices[vertexBase + 1u].position = visVertex1.position;
                gs_reyesShadowVertices[vertexBase + 1u].linearDepth = visVertex1.linearDepth;
#if defined(PSO_ALPHA_TEST)
                gs_reyesShadowVertices[vertexBase + 1u].texcoord = visVertex1.texcoord;
#endif

                gs_reyesShadowVertices[vertexBase + 2u].position = visVertex2.position;
                gs_reyesShadowVertices[vertexBase + 2u].linearDepth = visVertex2.linearDepth;
#if defined(PSO_ALPHA_TEST)
                gs_reyesShadowVertices[vertexBase + 2u].texcoord = visVertex2.texcoord;
#endif

                gs_reyesShadowTriangles[triangleOutputIndex] = reverseWinding
                    ? uint3(vertexBase + 0u, vertexBase + 2u, vertexBase + 1u)
                    : uint3(vertexBase + 0u, vertexBase + 1u, vertexBase + 2u);
                gs_reyesShadowPrimitiveIDs[triangleOutputIndex] = microTriangleIndex;
            }
        }

        GroupMemoryBarrierWithGroupSync();
    }

    if (uGroupThreadID == 0u)
    {
        gs_reyesShadowOutputTriangleCount = min(gs_reyesShadowOutputTriangleCount, kClodReyesShadowMaxOutputTriangles);
        gs_reyesShadowOutputVertexCount = gs_reyesShadowOutputTriangleCount * 3u;
        if (gs_reyesShadowDispatchValid != 0u)
        {
            InterlockedAdd(telemetryBuffer[0].hardwareRasterMicroTriangleCount, gs_reyesShadowOutputTriangleCount);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    SetMeshOutputCounts(gs_reyesShadowOutputVertexCount, gs_reyesShadowOutputTriangleCount);

    for (uint vertexIndex = uGroupThreadID; vertexIndex < gs_reyesShadowOutputVertexCount; vertexIndex += MS_THREAD_GROUP_SIZE)
    {
        outputVertices[vertexIndex] = ReyesShadowVisVertexToPSInput(gs_reyesShadowVertices[vertexIndex]);
    }
    for (uint triangleIndex = uGroupThreadID; triangleIndex < gs_reyesShadowOutputTriangleCount; triangleIndex += MS_THREAD_GROUP_SIZE)
    {
        outputTriangles[triangleIndex] = gs_reyesShadowTriangles[triangleIndex];
        primitiveInfo[triangleIndex].triangleIndex = gs_reyesShadowPrimitiveIDs[triangleIndex];
    }
}
#endif
