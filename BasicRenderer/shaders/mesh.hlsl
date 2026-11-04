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
#include "include/visibleClusterPacking.hlsli"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"

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
    result.position = mul(worldPosition, viewCamera.viewProjection);
    result.position.x = result.position.x * rasterInfo.viewportScaleX + result.position.w * (rasterInfo.viewportScaleX - 1.0f);
    result.position.y = result.position.y * rasterInfo.viewportScaleY + result.position.w * (1.0f - rasterInfo.viewportScaleY);
    result.linearDepth = -viewPosition.z;
#if defined(PSO_ALPHA_TEST)
    result.texcoord = vertex.texcoord;
    result.materialDataIndex = materialDataIndex;
#endif
    result.visibleClusterIndex = clusterIndex;
#if !defined(CLOD_RASTER_SINGLE_VIEW)
    result.viewID = viewID;
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
