// Software rasterizer for small CLod clusters. One 32-lane thread group handles one cluster:
//
//   SWLoadClusterSetup    lane 0 publishes the cluster's page, mesh and transform state
//   SWTransformVertices   each lane projects every 32nd vertex into groupshared memory
//   SWRasterTriangles     each lane rasterizes every 32nd triangle
//   SWRasterPixel         alpha test, then the visibility or virtual shadow write
//
// Entry points: SWRasterIndirectCSMain (compute, the default path) and WG_SWRaster (work graph
// node, embedded in workGraphCulling.hlsl). They only differ in how they find their cluster.

#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/clodVirtualShadowDepth.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/vertex.hlsli"
#include "include/materialFlags.hlsli"
#include "PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodClusterFormat.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/debugPayload.hlsli"

#ifndef CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#define CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW 0
#endif

#define SW_CLUSTER_RASTER_THREADS 32

#ifndef CLOD_WG_SW_RASTER_THREADS
#define CLOD_WG_SW_RASTER_THREADS SW_CLUSTER_RASTER_THREADS
#endif

#ifndef CLOD_WG_RIGID_SW_RASTER
#define CLOD_WG_RIGID_SW_RASTER CLOD_WG_RIGID_ONLY
#endif

#if defined(PSO_ALPHA_TEST) || defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST)
#define CLOD_SW_RASTER_MAY_ALPHA_TEST 1
#else
#define CLOD_SW_RASTER_MAY_ALPHA_TEST 0
#endif

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#include "include/clodSwRasterVirtualShadow.hlsli"
#endif

// Work counters, compiled in only on request because they cost atomics per pixel. Enable them
// live with a pso.recompile define override, in a session launched with visibility telemetry.
#ifndef CLOD_SW_RASTER_TELEMETRY
#define CLOD_SW_RASTER_TELEMETRY 0
#endif

static const uint SW_COUNTER_CLUSTERS = 252u;
static const uint SW_COUNTER_INPUT_TRIANGLES = 253u;
static const uint SW_COUNTER_BACKFACE_CULLED = 254u;
static const uint SW_COUNTER_DEGENERATE_OR_OFFSCREEN = 255u;
static const uint SW_COUNTER_NO_SAMPLE_COVERED = 256u;
static const uint SW_COUNTER_COVERED_PIXELS = 257u;
static const uint SW_COUNTER_ALPHA_REJECTED_PIXELS = 258u;
static const uint SW_COUNTER_VISIBILITY_WRITES = 259u;

void SWTelemetryAdd(uint counter, uint value)
{
#if CLOD_SW_RASTER_TELEMETRY && !defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> telemetry = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
        InterlockedAdd(telemetry[counter], value);
    }
#endif
}

// What an entry point hands to SWRasterCluster.
struct SWClusterWork
{
    uint4 packedCluster;
    uint assemblyTransformIndex;
    uint visibleClusterIndex; // Written to the visibility buffer, so unsorted.
    bool hasResolvedSkinningSlot;
    uint resolvedSkinningSlot;
    uint cachedPositionBase; // 0xFFFFFFFF unless the dynamic wind skin cache has this cluster.
};

// Published by lane 0.
struct SWClusterSetup
{
    float4 modelViewProjectionX;
    float4 modelViewProjectionY;
    float4 modelViewProjectionW;
    float4 modelViewZ;

    uint vertexCount;
    uint triangleCount;
    uint positionFormat;
    uint positionStreamBase; // Page-relative, like every stream offset here.
    uint positionByteOffset;
    uint triangleStreamBase;
    uint triangleByteOffset;
    uint attributeMask;
    uint vertexAttributeOffset;
    uint jointArrayOffset;
    uint weightArrayOffset;
    uint uvSetCount;
    uint uvDescriptorOffset;
    uint uvBitstreamDirectoryOffset;

    uint materialDataIndex;
    uint vertexFlags;
    uint alphaTestEnabled;
    uint reverseWinding;
    uint dynamicShadowLayer;
    uint skinningInstanceSlot;
    uint activeBoneRemapIndexBase;
    uint activeBoneRemapIndexCount;
    uint singleRemappedJoint;
    uint groupFlags;
    uint debugOutputType;
};

groupshared SWClusterSetup gs_setup;
groupshared float2 gs_screenPos[SW_RASTER_MAX_VERTS];
groupshared float gs_linearDepth[SW_RASTER_MAX_VERTS];
groupshared float gs_invClipW[SW_RASTER_MAX_VERTS];
groupshared float2 gs_texcoord[SW_RASTER_MAX_VERTS];

// ---------------------------------------------------------------------------------------------
// Skinning
// ---------------------------------------------------------------------------------------------

uint SWResolveAssemblyBoneIndex(uint localJointId)
{
    if (localJointId >= gs_setup.activeBoneRemapIndexCount)
    {
        return localJointId;
    }

    if (gs_setup.activeBoneRemapIndexCount == 1u)
    {
        return gs_setup.singleRemappedJoint;
    }

    StructuredBuffer<uint> remapIndices =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemapIndices)];
    return remapIndices[gs_setup.activeBoneRemapIndexBase + localJointId];
}

SkinningInfluences SWResolveAssemblySkinningInfluences(SkinningInfluences skinning)
{
    if (gs_setup.activeBoneRemapIndexCount == 0u)
    {
        return skinning;
    }

    if (gs_setup.activeBoneRemapIndexCount == 1u)
    {
        skinning.joints0 = select(skinning.joints0 == 0u, gs_setup.singleRemappedJoint.xxxx, skinning.joints0);
        skinning.joints1 = select(skinning.joints1 == 0u, gs_setup.singleRemappedJoint.xxxx, skinning.joints1);
        return skinning;
    }

    skinning.joints0.x = SWResolveAssemblyBoneIndex(skinning.joints0.x);
    skinning.joints0.y = SWResolveAssemblyBoneIndex(skinning.joints0.y);
    skinning.joints0.z = SWResolveAssemblyBoneIndex(skinning.joints0.z);
    skinning.joints0.w = SWResolveAssemblyBoneIndex(skinning.joints0.w);
    skinning.joints1.x = SWResolveAssemblyBoneIndex(skinning.joints1.x);
    skinning.joints1.y = SWResolveAssemblyBoneIndex(skinning.joints1.y);
    skinning.joints1.z = SWResolveAssemblyBoneIndex(skinning.joints1.z);
    skinning.joints1.w = SWResolveAssemblyBoneIndex(skinning.joints1.w);
    return skinning;
}

SkinningInfluences SWDecodePackedSkinning(ByteAddressBuffer slab, uint pageByteOffset, uint vertexIndex)
{
    SkinningInfluences skinning = (SkinningInfluences)0;
    const uint vertexAttributeIndex = gs_setup.vertexAttributeOffset + vertexIndex;

    if ((gs_setup.attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) != 0u)
    {
        const uint weightAddress = pageByteOffset + gs_setup.weightArrayOffset + vertexAttributeIndex * 32u;
        skinning.weights0 = LoadFloat4(weightAddress, slab);
        skinning.weights1 = LoadFloat4(weightAddress + 16u, slab);
    }

    if ((gs_setup.attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) != 0u)
    {
        const uint jointAddress = pageByteOffset + gs_setup.jointArrayOffset + vertexAttributeIndex * 32u;
        if (!any(skinning.weights0.yzw != 0.0f) && !any(skinning.weights1 != 0.0f))
        {
            skinning.joints0.x = slab.Load(jointAddress);
        }
        else
        {
            skinning.joints0 = LoadUint4(jointAddress, slab);
            skinning.joints1 = LoadUint4(jointAddress + 16u, slab);
        }
    }

    return skinning;
}

float3 SWApplySkinning(ByteAddressBuffer slab, uint pageByteOffset, uint vertexIndex, float3 position, uint assemblyTransformIndex)
{
    SkinningInfluences skinning = SWDecodePackedSkinning(slab, pageByteOffset, vertexIndex);
    skinning = SWResolveAssemblySkinningInfluences(skinning);
    return mul(float4(position, 1.0f), BuildAssemblyLocalSkinMatrix(
        gs_setup.skinningInstanceSlot, skinning, assemblyTransformIndex)).xyz;
}

// ---------------------------------------------------------------------------------------------
// Stage 1: cluster setup (lane 0)
// ---------------------------------------------------------------------------------------------

void SWLoadAssemblyState(SWClusterWork work, PerMeshBuffer meshData)
{
    gs_setup.activeBoneRemapIndexBase = CLOD_ASSEMBLY_BONE_REMAP_SENTINEL;
    gs_setup.activeBoneRemapIndexCount = 0u;
    gs_setup.singleRemappedJoint = 0u;
    gs_setup.groupFlags = 0u;

#if CLOD_WG_RIGID_SW_RASTER
    const bool needsAssemblyRemap = false;
#else
    const bool needsAssemblyRemap =
        (meshData.vertexFlags & VERTEX_SKINNED) != 0u &&
        work.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL;
#endif
    const bool needsAssemblyDebug =
        gs_setup.debugOutputType == OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE ||
        gs_setup.debugOutputType == OUTPUT_CLOD_ASSEMBLY_PARTS;
    if (!needsAssemblyRemap && !needsAssemblyDebug)
    {
        return;
    }

    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(CLodVisibleClusterInstanceID(work.packedCluster));
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];

    if (needsAssemblyRemap && work.assemblyTransformIndex >= metadata.assemblyTransformBase)
    {
        const uint localTransformIndex = work.assemblyTransformIndex - metadata.assemblyTransformBase;
        if (localTransformIndex < metadata.assemblyBoneRemapCount)
        {
            StructuredBuffer<ClusterLODAssemblyBoneRemap> remaps =
                ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemaps)];
            const ClusterLODAssemblyBoneRemap remap = remaps[metadata.assemblyBoneRemapBase + localTransformIndex];
            if (remap.remapIndexBase != CLOD_ASSEMBLY_BONE_REMAP_SENTINEL && remap.remapIndexCount != 0u)
            {
                gs_setup.activeBoneRemapIndexBase = remap.remapIndexBase;
                gs_setup.activeBoneRemapIndexCount = remap.remapIndexCount;
                if (remap.remapIndexCount == 1u)
                {
                    StructuredBuffer<uint> remapIndices =
                        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemapIndices)];
                    gs_setup.singleRemappedJoint = remapIndices[remap.remapIndexBase];
                }
            }
        }
    }

    if (needsAssemblyDebug)
    {
        StructuredBuffer<ClusterLODGroup> groups =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
        gs_setup.groupFlags = groups[metadata.groupsBase + CLodVisibleClusterGroupID(work.packedCluster)].flags;
    }
}

void SWLoadClusterSetup(SWClusterWork work, uint viewID)
{
    const uint instanceID = CLodVisibleClusterInstanceID(work.packedCluster);
    const uint slabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(work.packedCluster);
    const uint pageByteOffset = CLodVisibleClusterPageSlabByteOffset(work.packedCluster);

    // Page and meshlet stream layout.
    const CLodPageHeader pageHeader = LoadPageHeader(slabDescriptorIndex, pageByteOffset);
    const CLodMeshletDescriptor descriptor = LoadMeshletDescriptor(
        slabDescriptorIndex,
        pageByteOffset,
        pageHeader.descriptorOffset,
        CLodVisibleClusterLocalMeshletIndex(work.packedCluster));
    gs_setup.vertexCount = CLodDescVertexCount(descriptor);
    gs_setup.triangleCount = CLodDescTriangleCount(descriptor);
    gs_setup.positionFormat = pageHeader.compressedPositionQuantExp;
    gs_setup.positionStreamBase = pageHeader.positionBitstreamOffset;
    gs_setup.positionByteOffset = descriptor.positionBitOffset;
    gs_setup.triangleStreamBase = pageHeader.triangleStreamOffset;
    gs_setup.triangleByteOffset = descriptor.triangleByteOffset;
    gs_setup.attributeMask = pageHeader.attributeMask;
    gs_setup.vertexAttributeOffset = descriptor.vertexAttributeOffset;
    gs_setup.jointArrayOffset = pageHeader.jointArrayOffset;
    gs_setup.weightArrayOffset = pageHeader.weightArrayOffset;
    gs_setup.uvSetCount = pageHeader.uvSetCount;
    gs_setup.uvDescriptorOffset = pageHeader.uvDescriptorOffset;
    gs_setup.uvBitstreamDirectoryOffset = pageHeader.uvBitstreamDirectoryOffset;

    // Mesh and material.
    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDraw(instanceID);
    ConstantBuffer<PerFrameBuffer> perFrameBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    gs_setup.debugOutputType = perFrameBuffer.outputType;
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const PerMeshBuffer meshData = perMeshBuffer[meshInstance.perMeshBufferIndex];
    gs_setup.materialDataIndex = meshData.materialDataIndex;
    gs_setup.vertexFlags = meshData.vertexFlags;
    gs_setup.dynamicShadowLayer = CLodVisibleClusterUsesDynamicShadowLayer(work.packedCluster) ? 1u : 0u;

#if CLOD_WG_RIGID_SW_RASTER
    gs_setup.skinningInstanceSlot = 0xFFFFFFFFu;
#else
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    const bool allowSkinnedDeformation = gs_setup.dynamicShadowLayer != 0u;
#else
    const bool allowSkinnedDeformation = true;
#endif
    gs_setup.skinningInstanceSlot = allowSkinnedDeformation && (meshData.vertexFlags & VERTEX_SKINNED) != 0u
        ? (work.hasResolvedSkinningSlot
            ? work.resolvedSkinningSlot
            : ResolveProceduralWindSkinningSlot(instanceID, meshInstance.skinningInstanceSlot))
        : 0xFFFFFFFFu;
#endif

#if defined(PSO_ALPHA_TEST)
    gs_setup.alphaTestEnabled = 1u;
#elif defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST)
    StructuredBuffer<MaterialInfo> materialDataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    gs_setup.alphaTestEnabled =
        (materialDataBuffer[meshData.materialDataIndex].materialFlags & MATERIAL_ALPHA_TEST) != 0u ? 1u : 0u;
#else
    gs_setup.alphaTestEnabled = 0u;
#endif

    SWLoadAssemblyState(work, meshData);

    // Transform. Only the matrix rows the vertex stage needs.
    const PerObjectBuffer objData = LoadInstanceTransformForDrawWithAssemblyTransform(instanceID, work.assemblyTransformIndex);
    StructuredBuffer<CullingCameraInfo> cullingCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    const CullingCameraInfo camera = cullingCameras[viewID];
    const row_major float4x4 modelViewProjection = mul(objData.model, camera.viewProjection);
    gs_setup.modelViewProjectionX = float4(
        modelViewProjection[0][0], modelViewProjection[1][0], modelViewProjection[2][0], modelViewProjection[3][0]);
    gs_setup.modelViewProjectionY = float4(
        modelViewProjection[0][1], modelViewProjection[1][1], modelViewProjection[2][1], modelViewProjection[3][1]);
    gs_setup.modelViewProjectionW = float4(
        modelViewProjection[0][3], modelViewProjection[1][3], modelViewProjection[2][3], modelViewProjection[3][3]);
    gs_setup.modelViewZ = mul(objData.model, camera.viewZ);
    gs_setup.reverseWinding = (objData.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u ? 1u : 0u;
}

// ---------------------------------------------------------------------------------------------
// Stage 2: vertices
// ---------------------------------------------------------------------------------------------

void SWRecordCacheTelemetry(SWClusterWork work)
{
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW && !defined(CLOD_SW_RASTER_EMBEDDED_WORK_GRAPH)
    if (CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX == 0xFFFFFFFFu)
    {
        return;
    }

    RWStructuredBuffer<uint> cacheTelemetry = ResourceDescriptorHeap[CLOD_RASTER_TELEMETRY_DESCRIPTOR_INDEX];
    if (work.cachedPositionBase != 0xFFFFFFFFu)
    {
        InterlockedAdd(cacheTelemetry[244u], gs_setup.vertexCount);
    }
    else if ((gs_setup.vertexFlags & VERTEX_SKINNED) != 0u)
    {
        InterlockedAdd(cacheTelemetry[245u], gs_setup.vertexCount);
    }
#endif
}

float3 SWLoadVertexPosition(SWClusterWork work, ByteAddressBuffer slab, uint pageByteOffset, uint vertexIndex)
{
    if (work.cachedPositionBase != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<float3> cachedPositions =
            ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_POSITIONS_DESCRIPTOR_INDEX];
        return cachedPositions[work.cachedPositionBase + vertexIndex];
    }

    const float3 position = CLodLoadPagePosition(
        slab,
        gs_setup.positionFormat,
        pageByteOffset + gs_setup.positionStreamBase,
        gs_setup.positionByteOffset,
        vertexIndex);
#if defined(PSO_SKINNED)
    return SWApplySkinning(slab, pageByteOffset, vertexIndex, position, work.assemblyTransformIndex);
#elif !CLOD_WG_RIGID_SW_RASTER
    if ((gs_setup.vertexFlags & VERTEX_SKINNED) != 0u)
    {
        return SWApplySkinning(slab, pageByteOffset, vertexIndex, position, work.assemblyTransformIndex);
    }
    return position;
#else
    return position;
#endif
}

void SWTransformVertices(SWClusterWork work, ClodViewRasterInfo rasterInfo, uint GI)
{
    const uint slabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(work.packedCluster);
    const uint pageByteOffset = CLodVisibleClusterPageSlabByteOffset(work.packedCluster);
    ByteAddressBuffer slab = ResourceDescriptorHeap[slabDescriptorIndex];

    const float visWidth = float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX);
    const float visHeight = float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY);
    const float scissorMinX = float(rasterInfo.scissorMinX);
    const float scissorMinY = float(rasterInfo.scissorMinY);

    const bool needsTexcoords = gs_setup.alphaTestEnabled != 0u;
    CLodUvStream uvStream = (CLodUvStream)0;
    if (needsTexcoords)
    {
        uvStream = CLodLoadUvStream(
            slabDescriptorIndex,
            pageByteOffset,
            gs_setup.uvSetCount,
            gs_setup.uvDescriptorOffset,
            gs_setup.uvBitstreamDirectoryOffset,
            CLodVisibleClusterLocalMeshletIndex(work.packedCluster),
            0u);
    }

    for (uint v = GI; v < gs_setup.vertexCount; v += SW_CLUSTER_RASTER_THREADS)
    {
        const float4 position = float4(SWLoadVertexPosition(work, slab, pageByteOffset, v), 1.0f);
        const float2 clipXY = float2(
            dot(position, gs_setup.modelViewProjectionX),
            dot(position, gs_setup.modelViewProjectionY));
        const float invW = 1.0f / dot(position, gs_setup.modelViewProjectionW);
        const float2 ndc = clipXY * invW;

        gs_screenPos[v] = float2(
            (ndc.x + 1.0f) * 0.5f * visWidth + scissorMinX,
            (1.0f - ndc.y) * 0.5f * visHeight + scissorMinY);
        gs_linearDepth[v] = -dot(position, gs_setup.modelViewZ);
        gs_invClipW[v] = invW;
        if (needsTexcoords)
        {
            gs_texcoord[v] = CLodDecodeUv(slab, uvStream, v);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Stage 3: triangles and pixels
// ---------------------------------------------------------------------------------------------

// Everything a covered pixel needs from its triangle.
struct SWRasterTriangle
{
    uint3 vertices;
    uint triangleIndex;
    float3 depths;
    float3 invW;
};

// Per-cluster debug output. Only built when a debug view is on.
struct SWRasterDebug
{
    bool enabled;
    uint2 payload;
};

SWRasterDebug SWBuildRasterDebug(SWClusterWork work)
{
    const uint outputType = gs_setup.debugOutputType;
    SWRasterDebug debug = (SWRasterDebug)0;
    debug.enabled =
        outputType == OUTPUT_SW_RASTER ||
        outputType == OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE ||
        outputType == OUTPUT_CLOD_ASSEMBLY_PARTS;
    if (!debug.enabled)
    {
        return debug;
    }

    const bool isAssemblyVoxel = (gs_setup.groupFlags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u;
    if (outputType == OUTPUT_SW_RASTER)
    {
        debug.payload = PackDebugUint(1u);
    }
    else if (outputType == OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE)
    {
        float3 color = float3(0.08f, 0.10f, 0.12f);
        if (isAssemblyVoxel)
        {
            color = float3(1.0f, 0.58f, 0.08f);
        }
        else if (work.assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
        {
            color = max(HashToColor(work.assemblyTransformIndex + 17u), 0.18f.xxx);
        }
        else if ((gs_setup.groupFlags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u)
        {
            color = float3(0.82f, 0.18f, 1.0f);
        }
        debug.payload = PackDebugFloat3(color);
    }
    else
    {
        debug.payload = PackDebugUint(isAssemblyVoxel
            ? (0x40000000u ^ CLodVisibleClusterGroupID(work.packedCluster))
            : (work.assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL
                ? 1u
                : (0x80000000u + work.assemblyTransformIndex)));
    }
    return debug;
}

#if CLOD_SW_RASTER_MAY_ALPHA_TEST
bool SWAlphaTestFailed(float2 texcoords, uint materialDataIndex)
{
    StructuredBuffer<MaterialInfo> materialDataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    const MaterialInfo materialInfo = materialDataBuffer[materialDataIndex];
    const uint materialFlags = materialInfo.materialFlags;

#if defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST) && !defined(PSO_ALPHA_TEST)
    if ((materialFlags & MATERIAL_ALPHA_TEST) == 0u)
    {
        return false;
    }
#endif

    float alpha = materialInfo.baseColorFactor.a;

    if ((materialFlags & MATERIAL_BASE_COLOR_TEXTURE) != 0u)
    {
        Texture2D<float4> baseColorTexture =
            ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorTextureIndex)];
        SamplerState baseColorSamplerState =
            SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.baseColorSamplerIndex)];
        alpha *= baseColorTexture.SampleLevel(baseColorSamplerState, texcoords, 0.0f).a;
    }

    if ((materialFlags & MATERIAL_OPACITY_TEXTURE) != 0u)
    {
        Texture2D<float4> opacityTexture =
            ResourceDescriptorHeap[NonUniformResourceIndex(materialInfo.opacityTextureIndex)];
        SamplerState opacitySamplerState =
            SamplerDescriptorHeap[NonUniformResourceIndex(materialInfo.opacitySamplerIndex)];
        alpha *= opacityTexture.SampleLevel(opacitySamplerState, texcoords, 0.0f).a;
    }

    return alpha < materialInfo.alphaCutoff;
}
#endif

// Everything a covered pixel writes to, resolved once per lane.
struct SWRasterOutput
{
    uint viewID;
    uint visibleClusterIndex;
    SWRasterDebug debug;
};

void SWRasterPixel(
    SWRasterTriangle tri,
    SWRasterOutput output,
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    RWTexture2D<uint64_t> visBuffer,
#endif
    RWTexture2D<uint2> debugVisTex,
    uint2 pixel,
    float3 barycentrics)
{
    SWTelemetryAdd(SW_COUNTER_COVERED_PIXELS, 1u);
#if CLOD_SW_RASTER_MAY_ALPHA_TEST
    if (gs_setup.alphaTestEnabled != 0u)
    {
        const float3 perspective = barycentrics * tri.invW;
        const float invSum = rcp(perspective.x + perspective.y + perspective.z);
        const float2 texcoord = (
            gs_texcoord[tri.vertices.x] * perspective.x +
            gs_texcoord[tri.vertices.y] * perspective.y +
            gs_texcoord[tri.vertices.z] * perspective.z) * invSum;
        if (SWAlphaTestFailed(texcoord, gs_setup.materialDataIndex))
        {
            SWTelemetryAdd(SW_COUNTER_ALPHA_REJECTED_PIXELS, 1u);
            return;
        }
    }
#endif

    if (output.debug.enabled)
    {
        WriteDebugPixel(debugVisTex, pixel, output.debug.payload);
    }

    const float depth = dot(barycentrics, tri.depths);
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    SWRasterWriteVirtualShadow(pixel, output.viewID, depth, gs_setup.dynamicShadowLayer != 0u);
#else
    InterlockedMin(visBuffer[pixel], PackVisKey(depth, output.visibleClusterIndex, tri.triangleIndex));
#endif
    SWTelemetryAdd(SW_COUNTER_VISIBILITY_WRITES, 1u);
}

// Narrows [firstPixelOffset, lastPixelOffset] to where value + offset * step >= 0.
void SWClipScanlineConstraint(
    float value,
    float step,
    inout int firstPixelOffset,
    inout int lastPixelOffset,
    inout bool hasPixels)
{
    if (!hasPixels)
    {
        return;
    }

    if (step > 0.0f)
    {
        firstPixelOffset = max(firstPixelOffset, int(ceil(-value / step)));
    }
    else if (step < 0.0f)
    {
        lastPixelOffset = min(lastPixelOffset, int(floor(value / -step)));
    }
    else
    {
        hasPixels = value >= 0.0f;
    }

    hasPixels = hasPixels && firstPixelOffset <= lastPixelOffset;
}

void SWRasterTriangles(SWClusterWork work, ClodViewRasterInfo rasterInfo, uint GI)
{
    const uint slabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(work.packedCluster);
    const uint pageByteOffset = CLodVisibleClusterPageSlabByteOffset(work.packedCluster);
    const uint viewID = CLodVisibleClusterViewID(work.packedCluster);
    ByteAddressBuffer slab = ResourceDescriptorHeap[slabDescriptorIndex];

    const uint triangleCount = gs_setup.triangleCount;
    const uint triangleStreamAddress = pageByteOffset + gs_setup.triangleStreamBase + gs_setup.triangleByteOffset;
    const bool reverseWinding = gs_setup.reverseWinding != 0u;
    const int2 scissorMin = int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY);
    const int2 scissorMax = int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1);

    SWRasterOutput output;
    output.viewID = viewID;
    output.visibleClusterIndex = work.visibleClusterIndex;
    output.debug = SWBuildRasterDebug(work);
    const bool debugEnabled = output.debug.enabled;

    RWTexture2D<uint2> debugVisTex = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_SW_VSM_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    const bool hasClipmapInfo = CLodVirtualShadowTryGetClipmapInfoForView(viewID, clipmapInfos, clipmapInfo);
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_SW_VSM_PAGE_TABLE_DESCRIPTOR_INDEX];
#else
    RWTexture2D<uint64_t> visBuffer =
        ResourceDescriptorHeap[NonUniformResourceIndex(rasterInfo.visibilityUAVDescriptorIndex)];
#endif

    for (uint triangleBlockBase = 0u; triangleBlockBase < triangleCount; triangleBlockBase += SW_CLUSTER_RASTER_THREADS)
    {
        // Every lane takes part in the wave load, including lanes past the last triangle.
        const uint triangleIndex = triangleBlockBase + GI;
        uint3 vertices = CLodDecodeTriangleWave32(slab, triangleStreamAddress, triangleBlockBase, GI);
        if (triangleIndex >= triangleCount)
        {
            continue;
        }
        if (reverseWinding)
        {
            vertices = vertices.xzy;
        }

        float2 s0 = gs_screenPos[vertices.x];
        float2 s1 = gs_screenPos[vertices.y];
        float2 s2 = gs_screenPos[vertices.z];

        SWRasterTriangle tri;
        tri.vertices = vertices;
        tri.triangleIndex = triangleIndex;
        tri.depths = float3(gs_linearDepth[vertices.x], gs_linearDepth[vertices.y], gs_linearDepth[vertices.z]);
        tri.invW = float3(gs_invClipW[vertices.x], gs_invClipW[vertices.y], gs_invClipW[vertices.z]);

        if (tri.depths.x <= 0.0f || tri.depths.y <= 0.0f || tri.depths.z <= 0.0f)
        {
            SWTelemetryAdd(SW_COUNTER_DEGENERATE_OR_OFFSCREEN, 1u);
            continue;
        }

        float2 e01 = s1 - s0;
        float2 e02 = s2 - s0;
        float twiceArea = e01.x * e02.y - e01.y * e02.x;
        if (debugEnabled)
        {
            // Debug views show both faces.
            if (abs(twiceArea) <= 1e-8f)
            {
                continue;
            }
            if (twiceArea > 0.0f)
            {
                const float2 swappedPosition = s1;
                s1 = s2;
                s2 = swappedPosition;
                tri.depths = tri.depths.xzy;
                e01 = s1 - s0;
                e02 = s2 - s0;
                twiceArea = e01.x * e02.y - e01.y * e02.x;
            }
        }
        else if (twiceArea >= 0.0f)
        {
            SWTelemetryAdd(SW_COUNTER_BACKFACE_CULLED, 1u);
            continue;
        }

        const float invTwiceArea = -1.0f / twiceArea;

        int2 minPixel = int2(floor(min(min(s0, s1), s2)));
        int2 maxPixel = int2(floor(max(max(s0, s1), s2)));
        minPixel = max(max(minPixel, scissorMin), int2(0, 0));
        maxPixel = min(maxPixel, scissorMax);
        if (minPixel.x > maxPixel.x || minPixel.y > maxPixel.y)
        {
            SWTelemetryAdd(SW_COUNTER_DEGENERATE_OR_OFFSCREEN, 1u);
            continue;
        }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (!hasClipmapInfo ||
            !CLodVirtualShadowAnyRenderablePageInPixelRect(
                uint2(minPixel),
                uint2(maxPixel),
                clipmapInfo,
                (gs_setup.vertexFlags & VERTEX_SKINNED) != 0u,
                pageTable))
        {
            continue;
        }
#endif

        // Barycentrics at the first pixel center, and their steps.
        const float2 origin = float2(minPixel) + 0.5f;
        const float2 e12 = s2 - s1;
        const float2 e20 = s0 - s2;
        float scanlineB0 = ((origin.x - s1.x) * e12.y - (origin.y - s1.y) * e12.x) * invTwiceArea;
        float scanlineB1 = ((origin.x - s2.x) * e20.y - (origin.y - s2.y) * e20.x) * invTwiceArea;
        const float dxB0 = e12.y * invTwiceArea;
        const float dxB1 = e20.y * invTwiceArea;
        const float dxB2 = -(dxB0 + dxB1);
        const float dyB0 = -e12.x * invTwiceArea;
        const float dyB1 = -e20.x * invTwiceArea;

        const int rectWidth = maxPixel.x - minPixel.x + 1;
        // Decided per wave to keep the lanes together.
        const bool useScanlineRanges = WaveActiveAnyTrue(rectWidth > 4);
#if CLOD_SW_RASTER_TELEMETRY
        bool coveredAnySample = false;
#endif

        for (int y = minPixel.y; y <= maxPixel.y; y++)
        {
            int firstPixelOffset = 0;
            int lastPixelOffset = rectWidth - 1;
            bool hasPixels = true;
            if (useScanlineRanges)
            {
                const float scanlineB2 = 1.0f - scanlineB0 - scanlineB1;
                SWClipScanlineConstraint(scanlineB0, dxB0, firstPixelOffset, lastPixelOffset, hasPixels);
                SWClipScanlineConstraint(scanlineB1, dxB1, firstPixelOffset, lastPixelOffset, hasPixels);
                SWClipScanlineConstraint(scanlineB2, dxB2, firstPixelOffset, lastPixelOffset, hasPixels);
            }

            if (hasPixels)
            {
                float b0 = scanlineB0 + float(firstPixelOffset) * dxB0;
                float b1 = scanlineB1 + float(firstPixelOffset) * dxB1;
                for (int x = minPixel.x + firstPixelOffset; x <= minPixel.x + lastPixelOffset; x++)
                {
                    const float b2 = 1.0f - b0 - b1;
                    // Scanline ranges already guarantee coverage up to rounding at the range ends.
                    if (useScanlineRanges || (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f))
                    {
#if CLOD_SW_RASTER_TELEMETRY
                        coveredAnySample = true;
#endif
                        SWRasterPixel(
                            tri,
                            output,
#if !CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                            visBuffer,
#endif
                            debugVisTex,
                            uint2(x, y),
                            float3(b0, b1, b2));
                    }
                    b0 += dxB0;
                    b1 += dxB1;
                }
            }

            scanlineB0 += dyB0;
            scanlineB1 += dyB1;
        }
#if CLOD_SW_RASTER_TELEMETRY
        if (!coveredAnySample)
        {
            SWTelemetryAdd(SW_COUNTER_NO_SAMPLE_COVERED, 1u);
        }
#endif
    }
}

// ---------------------------------------------------------------------------------------------
// One cluster
// ---------------------------------------------------------------------------------------------

void SWRasterCluster(SWClusterWork work, StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer, uint GI)
{
    const uint viewID = CLodVisibleClusterViewID(work.packedCluster);
    const ClodViewRasterInfo rasterInfo = viewRasterInfoBuffer[viewID];

    if (GI == 0u)
    {
        SWLoadClusterSetup(work, viewID);
    }
    GroupMemoryBarrierWithGroupSync();

    if (GI == 0u)
    {
        SWRecordCacheTelemetry(work);
        SWTelemetryAdd(SW_COUNTER_CLUSTERS, 1u);
        SWTelemetryAdd(SW_COUNTER_INPUT_TRIANGLES, gs_setup.triangleCount);
    }
    SWTransformVertices(work, rasterInfo, GI);
    GroupMemoryBarrierWithGroupSync();

    SWRasterTriangles(work, rasterInfo, GI);
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

[Shader("node")]
[NodeID("SWRaster")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(SW_BATCH_MAX_CLUSTERS * SW_RASTER_GROUPS_PER_CLUSTER, 1, 1)]
[NumThreads(CLOD_WG_SW_RASTER_THREADS, 1, 1)]
void WG_SWRaster(
    DispatchNodeInputRecord<SWRasterBatchRecord> inputRecord,
    uint GI : SV_GroupIndex,
    uint3 groupId : SV_GroupID)
{
    const SWRasterBatchRecord batch = inputRecord.Get();

    const uint clusterIndex = groupId.x / SW_RASTER_GROUPS_PER_CLUSTER;
    if (clusterIndex >= batch.numClusters)
    {
        return;
    }

    const uint unsortedClusterIndex = batch.clusterIndices[clusterIndex];
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer =
        ResourceDescriptorHeap[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];

    SWClusterWork work;
    work.packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, unsortedClusterIndex);
    work.assemblyTransformIndex = visibleClusterTransformIndices[unsortedClusterIndex];
    work.visibleClusterIndex = unsortedClusterIndex;
    work.hasResolvedSkinningSlot = false;
    work.resolvedSkinningSlot = 0xFFFFFFFFu;
    work.cachedPositionBase = 0xFFFFFFFFu;
    SWRasterCluster(work, viewRasterInfoBuffer, GI);
}

[shader("compute")]
[numthreads(SW_CLUSTER_RASTER_THREADS, 1, 1)]
void SWRasterIndirectCSMain(uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketID = IndirectCommandSignatureRootConstant2;
    const uint linearizedGroupID = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    const uint clusterIndex = linearizedGroupID / SW_RASTER_GROUPS_PER_CLUSTER;
    if (clusterIndex >= histogram[bucketID])
    {
        return;
    }

    const uint sortedClusterIndex = IndirectCommandSignatureRootConstant0 + clusterIndex;
    ByteAddressBuffer compactedVisibleClusters =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> compactedVisibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    StructuredBuffer<CLodSoftwareRasterMapping> sortedToUnsortedMapping =
        ResourceDescriptorHeap[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuffer =
        ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    const CLodSoftwareRasterMapping mapping = sortedToUnsortedMapping[sortedClusterIndex];

    SWClusterWork work;
    work.packedCluster = CLodLoadVisibleClusterPacked(compactedVisibleClusters, sortedClusterIndex);
    work.assemblyTransformIndex = compactedVisibleClusterTransformIndices[sortedClusterIndex];
    work.visibleClusterIndex = mapping.unsortedClusterIndex;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    work.hasResolvedSkinningSlot = false;
    work.resolvedSkinningSlot = 0xFFFFFFFFu;
    work.cachedPositionBase = 0xFFFFFFFFu;
    if (CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX != 0xFFFFFFFFu)
    {
        RWStructuredBuffer<uint> dynamicWindSkinCacheMapping =
            ResourceDescriptorHeap[CLOD_RASTER_DYNAMIC_WIND_SKIN_CACHE_MAPPING_DESCRIPTOR_INDEX];
        work.cachedPositionBase = dynamicWindSkinCacheMapping[sortedClusterIndex];
    }
#else
    work.hasResolvedSkinningSlot = true;
    work.resolvedSkinningSlot = mapping.skinningInstanceSlot;
    work.cachedPositionBase = 0xFFFFFFFFu;
#endif
    SWRasterCluster(work, viewRasterInfoBuffer, GI);
}
