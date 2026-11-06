#ifndef CLOD_RASTER_CLUSTER_HLSLI
#define CLOD_RASTER_CLUSTER_HLSLI

// What both CLod rasterizers need to know about one visible cluster, and how to fetch and project
// its vertices.
//
// softwareRaster.hlsl loads this on lane 0 and shares it through groupshared memory.
// clusterRasterMesh.hlsl loads it on every lane, because setup on one lane ahead of
// SetMeshOutputCounts produces invalid DXIL. Fields a lane never reads are dead code there, so
// nothing here needs to be conditional on what a particular variant uses.

#include "Common/defines.h"
#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/vertex.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/clodClusterFormat.hlsli"
#include "include/visibleClusterPacking.hlsli"

// Virtual shadows only deform clusters in the dynamic layer.
#ifndef CLOD_RASTER_CLUSTER_VIRTUAL_SHADOW
#define CLOD_RASTER_CLUSTER_VIRTUAL_SHADOW 0
#endif

#ifndef CLOD_RASTER_CLUSTER_RIGID_ONLY
#define CLOD_RASTER_CLUSTER_RIGID_ONLY 0
#endif

static const uint CLOD_RASTER_CLUSTER_OK = 0u;
static const uint CLOD_RASTER_CLUSTER_ZERO_PAGE_SLAB = 1u;
static const uint CLOD_RASTER_CLUSTER_MESHLET_OOB = 2u;
static const uint CLOD_RASTER_CLUSTER_INVALID_PAGE_OR_COUNTS = 3u;

struct CLodRasterClusterSource
{
    uint4 packedCluster;
    uint assemblyTransformIndex;
    uint paletteIndex; // culling's palette index, or CLOD_PALETTE_INDEX_FROM_PAGE
    bool hasResolvedSkinningSlot;
    uint resolvedSkinningSlot;
};

struct CLodRasterCluster
{
    // Local position to clip space and to view Z, as dot products with float4(position, 1).
    float4 modelViewProjectionX;
    float4 modelViewProjectionY;
    float4 modelViewProjectionZ;
    float4 modelViewProjectionW;
    float4 modelViewZ;

    uint viewID;
    uint drawRecordIndex;
    uint meshletIndex;
    uint slabDescriptorIndex;
    uint pageByteOffset;

    // Stream offsets are relative to the page.
    uint vertexCount;
    uint triangleCount;
    uint positionFormat;
    uint positionStreamBase;
    uint positionByteOffset;
    uint triangleStreamBase;
    uint triangleByteOffset;
    uint attributeMask;
    uint vertexAttributeOffset;
    uint normalArrayOffset;
    uint colorArrayOffset;
    uint jointArrayOffset;
    uint weightArrayOffset;
    uint uvSetCount;
    uint uvDescriptorOffset;
    uint uvBitstreamDirectoryOffset;

    uint materialDataIndex;
    uint vertexFlags;
    uint reverseWinding;
    uint dynamicShadowLayer;

    uint assemblyTransformIndex;
    uint skinningInstanceSlot;
    uint activeBoneRemapIndexBase;
    uint activeBoneRemapIndexCount;
    uint singleRemappedJoint;
};

void CLodRasterLoadAssemblyBoneRemap(uint drawRecordIndex, inout CLodRasterCluster cluster)
{
    cluster.activeBoneRemapIndexBase = CLOD_ASSEMBLY_BONE_REMAP_SENTINEL;
    cluster.activeBoneRemapIndexCount = 0u;
    cluster.singleRemappedJoint = 0u;

#if !CLOD_RASTER_CLUSTER_RIGID_ONLY
    if ((cluster.vertexFlags & VERTEX_SKINNED) == 0u ||
        cluster.assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
    {
        return;
    }

    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(drawRecordIndex);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    if (cluster.assemblyTransformIndex < metadata.assemblyTransformBase)
    {
        return;
    }

    const uint localTransformIndex = cluster.assemblyTransformIndex - metadata.assemblyTransformBase;
    if (localTransformIndex >= metadata.assemblyBoneRemapCount)
    {
        return;
    }

    StructuredBuffer<ClusterLODAssemblyBoneRemap> remaps =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemaps)];
    const ClusterLODAssemblyBoneRemap remap = remaps[metadata.assemblyBoneRemapBase + localTransformIndex];
    if (remap.remapIndexBase == CLOD_ASSEMBLY_BONE_REMAP_SENTINEL || remap.remapIndexCount == 0u)
    {
        return;
    }

    cluster.activeBoneRemapIndexBase = remap.remapIndexBase;
    cluster.activeBoneRemapIndexCount = remap.remapIndexCount;
    if (remap.remapIndexCount == 1u)
    {
        StructuredBuffer<uint> remapIndices =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemapIndices)];
        cluster.singleRemappedJoint = remapIndices[remap.remapIndexBase];
    }
#endif
}

// Returns CLOD_RASTER_CLUSTER_OK, or why this cluster can't be drawn. The cluster is only
// complete when it returns OK.
uint CLodLoadRasterCluster(CLodRasterClusterSource source, out CLodRasterCluster cluster)
{
    cluster = (CLodRasterCluster)0;
    const uint4 packedCluster = source.packedCluster;

    cluster.viewID = CLodVisibleClusterViewID(packedCluster);
    cluster.drawRecordIndex = CLodVisibleClusterInstanceID(packedCluster);
    cluster.meshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    cluster.slabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    cluster.pageByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);
    cluster.assemblyTransformIndex = source.assemblyTransformIndex;
    cluster.dynamicShadowLayer = CLodVisibleClusterUsesDynamicShadowLayer(packedCluster) ? 1u : 0u;
    if (cluster.slabDescriptorIndex == 0u)
    {
        return CLOD_RASTER_CLUSTER_ZERO_PAGE_SLAB;
    }

    // Page and meshlet stream layout.
    const CLodPageHeader pageHeader = LoadPageHeader(cluster.slabDescriptorIndex, cluster.pageByteOffset);
    if (pageHeader.formatAndKind != CLOD_TRIANGLE_PAGE_MAGIC || pageHeader.descriptorOffset == 0u)
    {
        return CLOD_RASTER_CLUSTER_INVALID_PAGE_OR_COUNTS;
    }
    if (cluster.meshletIndex >= pageHeader.meshletCount)
    {
        return CLOD_RASTER_CLUSTER_MESHLET_OOB;
    }
    const CLodMeshletDescriptor descriptor = LoadMeshletDescriptor(
        cluster.slabDescriptorIndex,
        cluster.pageByteOffset,
        pageHeader.descriptorOffset,
        cluster.meshletIndex);
    cluster.vertexCount = CLodDescVertexCount(descriptor);
    cluster.triangleCount = CLodDescTriangleCount(descriptor);
    cluster.positionFormat = pageHeader.compressedPositionQuantExp;
    cluster.positionStreamBase = pageHeader.positionBitstreamOffset;
    cluster.positionByteOffset = descriptor.positionBitOffset;
    cluster.triangleStreamBase = pageHeader.triangleStreamOffset;
    cluster.triangleByteOffset = descriptor.triangleByteOffset;
    cluster.attributeMask = pageHeader.attributeMask;
    cluster.vertexAttributeOffset = descriptor.vertexAttributeOffset;
    cluster.normalArrayOffset = pageHeader.normalArrayOffset;
    cluster.colorArrayOffset = pageHeader.colorArrayOffset;
    cluster.jointArrayOffset = pageHeader.jointArrayOffset;
    cluster.weightArrayOffset = pageHeader.weightArrayOffset;
    cluster.uvSetCount = pageHeader.uvSetCount;
    cluster.uvDescriptorOffset = pageHeader.uvDescriptorOffset;
    cluster.uvBitstreamDirectoryOffset = pageHeader.uvBitstreamDirectoryOffset;

    // Mesh and skinning.
    const InstanceDrawRecordBuffer drawRecord = LoadInstanceDrawRecord(cluster.drawRecordIndex);
    const PerMeshInstanceBuffer meshInstance = LoadMeshTemplateForDrawRecord(drawRecord);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const PerMeshBuffer meshData = perMeshBuffer[meshInstance.perMeshBufferIndex];
    const PerMeshBuffer geometryData = perMeshBuffer[CLodClusterGeometryPerMeshIndex(
        meshInstance.perMeshBufferIndex, CLodVisibleClusterGroupID(packedCluster), source.assemblyTransformIndex)];
    const uint paletteIndex = source.paletteIndex != CLOD_PALETTE_INDEX_FROM_PAGE
        ? source.paletteIndex
        : CLodPaletteIndexFromDescriptor(meshInstance, CLodVisibleClusterGroupID(packedCluster), source.assemblyTransformIndex,
            asuint(descriptor.terrainRvtLocalSkyrimXYRadius));
    cluster.materialDataIndex = CLodResolveMaterial(meshInstance, meshData, paletteIndex).materialDataIndex;
    cluster.vertexFlags = geometryData.vertexFlags;

#if CLOD_RASTER_CLUSTER_RIGID_ONLY
    cluster.skinningInstanceSlot = 0xFFFFFFFFu;
#else
#if CLOD_RASTER_CLUSTER_VIRTUAL_SHADOW
    const bool allowDeformation = cluster.dynamicShadowLayer != 0u;
#else
    const bool allowDeformation = true;
#endif
    cluster.skinningInstanceSlot = allowDeformation && (geometryData.vertexFlags & VERTEX_SKINNED) != 0u
        ? (source.hasResolvedSkinningSlot
            ? source.resolvedSkinningSlot
            : ResolveProceduralWindSkinningSlot(cluster.drawRecordIndex, meshInstance.skinningInstanceSlot))
        : 0xFFFFFFFFu;
#endif
    CLodRasterLoadAssemblyBoneRemap(cluster.drawRecordIndex, cluster);

    // Transform.
    row_major float4x4 model;
    uint objectFlags;
    LoadClodRasterTransformForDrawRecord(drawRecord, source.assemblyTransformIndex, model, objectFlags);
    StructuredBuffer<CullingCameraInfo> cameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    const CullingCameraInfo camera = cameras[cluster.viewID];
    const row_major float4x4 modelViewProjection = mul(model, camera.viewProjection);
    cluster.modelViewProjectionX = float4(
        modelViewProjection[0][0], modelViewProjection[1][0], modelViewProjection[2][0], modelViewProjection[3][0]);
    cluster.modelViewProjectionY = float4(
        modelViewProjection[0][1], modelViewProjection[1][1], modelViewProjection[2][1], modelViewProjection[3][1]);
    cluster.modelViewProjectionZ = float4(
        modelViewProjection[0][2], modelViewProjection[1][2], modelViewProjection[2][2], modelViewProjection[3][2]);
    cluster.modelViewProjectionW = float4(
        modelViewProjection[0][3], modelViewProjection[1][3], modelViewProjection[2][3], modelViewProjection[3][3]);
    cluster.modelViewZ = mul(model, camera.viewZ);
    cluster.reverseWinding = (objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u ? 1u : 0u;

    return cluster.vertexCount <= MS_MESHLET_SIZE && cluster.triangleCount <= MS_MESHLET_SIZE
        ? CLOD_RASTER_CLUSTER_OK
        : CLOD_RASTER_CLUSTER_INVALID_PAGE_OR_COUNTS;
}

uint CLodRasterResolveAssemblyBone(CLodRasterCluster cluster, uint localJointId)
{
    if (localJointId >= cluster.activeBoneRemapIndexCount)
    {
        return localJointId;
    }
    if (cluster.activeBoneRemapIndexCount == 1u)
    {
        return cluster.singleRemappedJoint;
    }

    StructuredBuffer<uint> remapIndices =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyBoneRemapIndices)];
    return remapIndices[cluster.activeBoneRemapIndexBase + localJointId];
}

// Vertex joints and weights, with joints remapped to the assembly's skeleton.
SkinningInfluences CLodRasterLoadSkinning(CLodRasterCluster cluster, ByteAddressBuffer slab, uint vertexIndex)
{
    SkinningInfluences skinning = (SkinningInfluences)0;
    const uint vertexAttributeIndex = cluster.vertexAttributeOffset + vertexIndex;

    if ((cluster.attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) != 0u)
    {
        const uint weightAddress = cluster.pageByteOffset + cluster.weightArrayOffset + vertexAttributeIndex * 32u;
        skinning.weights0 = LoadFloat4(weightAddress, slab);
        skinning.weights1 = LoadFloat4(weightAddress + 16u, slab);
    }

    if ((cluster.attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) != 0u)
    {
        const uint jointAddress = cluster.pageByteOffset + cluster.jointArrayOffset + vertexAttributeIndex * 32u;
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

    if (cluster.activeBoneRemapIndexCount == 1u)
    {
        skinning.joints0 = select(skinning.joints0 == 0u, cluster.singleRemappedJoint.xxxx, skinning.joints0);
        skinning.joints1 = select(skinning.joints1 == 0u, cluster.singleRemappedJoint.xxxx, skinning.joints1);
    }
    else if (cluster.activeBoneRemapIndexCount != 0u)
    {
        skinning.joints0.x = CLodRasterResolveAssemblyBone(cluster, skinning.joints0.x);
        skinning.joints0.y = CLodRasterResolveAssemblyBone(cluster, skinning.joints0.y);
        skinning.joints0.z = CLodRasterResolveAssemblyBone(cluster, skinning.joints0.z);
        skinning.joints0.w = CLodRasterResolveAssemblyBone(cluster, skinning.joints0.w);
        skinning.joints1.x = CLodRasterResolveAssemblyBone(cluster, skinning.joints1.x);
        skinning.joints1.y = CLodRasterResolveAssemblyBone(cluster, skinning.joints1.y);
        skinning.joints1.z = CLodRasterResolveAssemblyBone(cluster, skinning.joints1.z);
        skinning.joints1.w = CLodRasterResolveAssemblyBone(cluster, skinning.joints1.w);
    }
    return skinning;
}

float3 CLodRasterLoadPosition(CLodRasterCluster cluster, ByteAddressBuffer slab, uint vertexIndex)
{
    return CLodLoadPagePosition(
        slab,
        cluster.positionFormat,
        cluster.pageByteOffset + cluster.positionStreamBase,
        cluster.positionByteOffset,
        vertexIndex);
}

float3 CLodRasterSkinPosition(CLodRasterCluster cluster, ByteAddressBuffer slab, uint vertexIndex, float3 position)
{
    return ApplyAssemblySkinningToPosition(
        cluster.skinningInstanceSlot,
        CLodRasterLoadSkinning(cluster, slab, vertexIndex),
        cluster.assemblyTransformIndex,
        position);
}

float4 CLodRasterClipPosition(CLodRasterCluster cluster, float3 position)
{
    const float4 homogeneous = float4(position, 1.0f);
    return float4(
        dot(homogeneous, cluster.modelViewProjectionX),
        dot(homogeneous, cluster.modelViewProjectionY),
        dot(homogeneous, cluster.modelViewProjectionZ),
        dot(homogeneous, cluster.modelViewProjectionW));
}

float CLodRasterLinearDepth(CLodRasterCluster cluster, float3 position)
{
    return -dot(float4(position, 1.0f), cluster.modelViewZ);
}

uint CLodRasterTriangleStreamAddress(CLodRasterCluster cluster)
{
    return cluster.pageByteOffset + cluster.triangleStreamBase + cluster.triangleByteOffset;
}

CLodUvStream CLodRasterLoadUvStream(CLodRasterCluster cluster, uint uvSetIndex)
{
    return CLodLoadUvStream(
        cluster.slabDescriptorIndex,
        cluster.pageByteOffset,
        cluster.uvSetCount,
        cluster.uvDescriptorOffset,
        cluster.uvBitstreamDirectoryOffset,
        cluster.meshletIndex,
        uvSetIndex);
}

#endif // CLOD_RASTER_CLUSTER_HLSLI
