// Software rasterizer work graph node for tiny CLod meshlets.
// Compile with DXC target: lib_6_8 (Shader Model 6.8)
//
// Broadcasting launch: receives SWRasterBatchRecord from ClusterCull nodes.
// Each record carries up to 8 cluster indices; each cluster gets one 128-thread group.
// Vertices are decoded into per-group
// groupshared, then triangles are rasterized via edge functions + InterlockedMin
// to the per-view visibility buffer.

#include "include/cbuffers.hlsli"
#include "include/clodVirtualShadowClipmap.hlsli"
#include "include/structs.hlsli"
#include "include/instanceDrawRecordHelpers.hlsli"
#include "include/skinningCommon.hlsli"
#include "include/vertex.hlsli"
#include "include/materialFlags.hlsli"
#include "PerPassRootConstants/clodWorkGraphRootConstants.h"
#include "PerPassRootConstants/clodRasterizationRootConstants.h"
#include "include/clodStructs.hlsli"
#include "include/clodPageAccess.hlsli"
#include "include/visibleClusterPacking.hlsli"
#include "include/visibilityPacking.hlsli"
#include "include/debugPayload.hlsli"

#ifndef CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
#define CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW 0
#endif

// Bit-packed position decode (mirrors mesh.hlsl / gbuffer.hlsl)

#ifndef CLOD_READ_PACKED_BITS32_DEFINED
#define CLOD_READ_PACKED_BITS32_DEFINED 1
uint ReadPackedBits32(ByteAddressBuffer buf, uint startBit, uint bitCount)
{
    if (bitCount == 0u) return 0u;

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
#endif

float3 SWDecodeCompressedPosition(
    uint meshletLocalVertex,
    uint positionBitstreamBase,
    uint positionBitOffset,
    uint bitsX, uint bitsY, uint bitsZ,
    uint quantExp,
    int3 minQ,
    uint pagePoolSlabDescriptorIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    return CLodLoadPagePosition(slab, quantExp, positionBitstreamBase, positionBitOffset, meshletLocalVertex);
}

// Triangle index decode (mirrors meshletCommon.hlsli)

uint3 SWDecodeTriangle(ByteAddressBuffer slab, uint triStreamBase, uint triByteOffset, uint triLocalIndex)
{
    uint triOffset = triStreamBase + triByteOffset + triLocalIndex * 3u;
    uint alignedOffset = (triOffset / 4u) * 4u;
    uint firstWord = slab.Load(alignedOffset);
    uint byteOff = triOffset % 4u;

    uint i0 = (firstWord >> (byteOff * 8u)) & 0xFFu;
    uint i1, i2;

    if (byteOff <= 1u)
    {
        i1 = (firstWord >> ((byteOff + 1u) * 8u)) & 0xFFu;
        i2 = (firstWord >> ((byteOff + 2u) * 8u)) & 0xFFu;
    }
    else if (byteOff == 2u)
    {
        i1 = (firstWord >> 24u) & 0xFFu;
        uint secondWord = slab.Load(alignedOffset + 4u);
        i2 = secondWord & 0xFFu;
    }
    else // byteOff == 3
    {
        uint secondWord = slab.Load(alignedOffset + 4u);
        i1 = secondWord & 0xFFu;
        i2 = (secondWord >> 8u) & 0xFFu;
    }

    return uint3(i0, i1, i2);
}

groupshared float2  gs_screenPos[SW_RASTER_MAX_VERTS];
groupshared float   gs_linearDepth[SW_RASTER_MAX_VERTS];
groupshared float   gs_invClipW[SW_RASTER_MAX_VERTS];
groupshared float2  gs_texcoord[SW_RASTER_MAX_VERTS];

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
bool SWRasterWriteVirtualShadow(uint2 pixel, uint viewID, float linearDepth)
{
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];

    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    if (!CLodVirtualShadowTryGetClipmapInfoForView(viewID, clipmapInfos, clipmapInfo))
    {
        return false;
    }

    const float2 shadowUv = saturate((float2(pixel) + 0.5f) / max((float)clipmapInfo.virtualResolution, 1.0f));
    const uint2 virtualPageCoords = CLodVirtualShadowVirtualPageCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 wrappedPageCoords = CLodVirtualShadowWrappedPageCoords(virtualPageCoords, clipmapInfo);

    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
    const uint3 pageCoords = uint3(wrappedPageCoords, clipmapInfo.pageTableLayer);
    const uint pageEntry = pageTable[pageCoords];
    if (!CLodVirtualShadowPageEntryCanRaster(pageEntry))
    {
        return false;
    }

    const uint physicalPageIndex = pageEntry & kCLodVirtualShadowPhysicalPageIndexMask;
    const uint2 virtualTexelCoords = CLodVirtualShadowVirtualTexelCoordsFromUv(shadowUv, clipmapInfo);
    const uint2 atlasPixel = CLodVirtualShadowPhysicalAtlasPixel(physicalPageIndex, virtualTexelCoords, clipmapInfo);

    RWTexture2D<uint> physicalPages = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PHYSICAL_PAGES_DESCRIPTOR_INDEX];
    InterlockedMin(physicalPages[atlasPixel], asuint(linearDepth));
    uint ignored = 0u;
    InterlockedOr(
        pageTable[pageCoords],
        kCLodVirtualShadowContentValidMask | kCLodVirtualShadowRerenderedThisFrameMask,
        ignored);
    return true;
}
#endif

#if defined(PSO_ALPHA_TEST) || defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST)
bool SWAlphaTestFailed(float2 texcoords, uint materialDataIndex)
{
    StructuredBuffer<MaterialInfo> materialDataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialDataBuffer)];
    MaterialInfo materialInfo = materialDataBuffer[materialDataIndex];
    uint materialFlags = materialInfo.materialFlags;

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

float2 SWDecodeCompressedUV(
    uint meshletLocalVertex,
    uint uvSetIndex,
    uint localMeshletIndex,
    uint pageByteOffset,
    uint uvSetCount,
    uint uvDescriptorOffset,
    uint uvBitstreamDirectoryOffset,
    uint pagePoolSlabDescriptorIndex)
{
    if (uvSetIndex >= uvSetCount)
    {
        return float2(0.0f, 0.0f);
    }

    CLodMeshletUvDescriptor uvDesc = LoadMeshletUvDescriptor(
        pagePoolSlabDescriptorIndex,
        pageByteOffset,
        uvDescriptorOffset,
        uvSetCount,
        localMeshletIndex,
        uvSetIndex);
    uint uvBitstreamBase = pageByteOffset + LoadPageUvBitstreamOffset(
        pagePoolSlabDescriptorIndex,
        pageByteOffset,
        uvBitstreamDirectoryOffset,
        uvSetIndex);
    uint uvBitsU = CLodUvDescBitsU(uvDesc);
    uint uvBitsV = CLodUvDescBitsV(uvDesc);

    uint bitsPerVertex = uvBitsU + uvBitsV;
    uint bitCursor = uvBitstreamBase * 8u + uvDesc.uvBitOffset + meshletLocalVertex * bitsPerVertex;

    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    uint encodedU = ReadPackedBits32(slab, bitCursor, uvBitsU);
    bitCursor += uvBitsU;
    uint encodedV = ReadPackedBits32(slab, bitCursor, uvBitsV);

    return float2(
        uvDesc.uvMinU + float(encodedU) * uvDesc.uvScaleU,
        uvDesc.uvMinV + float(encodedV) * uvDesc.uvScaleV);
}

SkinningInfluences SWDecodePackedJoints(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex)
{
    SkinningInfluences skinning;
    skinning.joints0 = uint4(0, 0, 0, 0);
    skinning.joints1 = uint4(0, 0, 0, 0);
    skinning.weights0 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    skinning.weights1 = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if ((hdr.attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    uint addr = pageByteOffset + hdr.jointArrayOffset + (desc.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.joints0 = LoadUint4(addr, slab);
    skinning.joints1 = LoadUint4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences SWDecodePackedWeights(
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

    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    uint addr = pageByteOffset + hdr.weightArrayOffset + (desc.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.weights0 = LoadFloat4(addr, slab);
    skinning.weights1 = LoadFloat4(addr + 16u, slab);
    return skinning;
}

void SWRasterClipScanlineConstraint(
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

void SWRasterCluster(
    uint4 packedCluster,
    uint assemblyTransformIndex,
    uint unsortedClusterIndex,
    uint GI,
    uint subGroup,
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuf)
{
    const uint viewID = CLodVisibleClusterViewID(packedCluster);
    const uint instanceID = CLodVisibleClusterInstanceID(packedCluster);
    const uint localMeshletIndex = CLodVisibleClusterLocalMeshletIndex(packedCluster);
    const uint pageSlabDescriptorIndex = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabByteOffset = CLodVisibleClusterPageSlabByteOffset(packedCluster);

    // Load page header + meshlet descriptor
    CLodPageHeader hdr = LoadPageHeader(pageSlabDescriptorIndex, pageSlabByteOffset);
    CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDescriptorIndex, pageSlabByteOffset,
        hdr.descriptorOffset, localMeshletIndex);

    const uint vertCount = CLodDescVertexCount(desc);
    const uint triCount  = CLodDescTriangleCount(desc);

    ConstantBuffer<PerFrameBuffer> perFrameBuffer = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    PerMeshInstanceBuffer meshInst = LoadMeshTemplateForDraw(instanceID);
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];
    const uint materialDataIndex = perMeshBuffer[meshInst.perMeshBufferIndex].materialDataIndex;

    PerObjectBuffer objData = LoadInstanceTransformForDrawWithAssemblyTransform(instanceID, assemblyTransformIndex);

    StructuredBuffer<CullingCameraInfo> cullingCameras =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CullingCameraBuffer)];
    CullingCameraInfo cam = cullingCameras[viewID];

    ClodViewRasterInfo rasterInfo = viewRasterInfoBuf[viewID];

    const float visWidth  = float(rasterInfo.scissorMaxX - rasterInfo.scissorMinX);
    const float visHeight = float(rasterInfo.scissorMaxY - rasterInfo.scissorMinY);
    const float scissorMinXf = float(rasterInfo.scissorMinX);
    const float scissorMinYf = float(rasterInfo.scissorMinY);

    const uint positionBitstreamBase = pageSlabByteOffset + hdr.positionBitstreamOffset;
    row_major matrix modelViewProjection = mul(objData.model, cam.viewProjection);
    float4 modelViewZ = mul(objData.model, cam.viewZ);

    for (uint v = GI; v < vertCount; v += SW_RASTER_THREADS)
    {
        float3 localPos = SWDecodeCompressedPosition(
            v,
            positionBitstreamBase,
            desc.positionBitOffset,
            CLodDescBitsX(desc), CLodDescBitsY(desc), CLodDescBitsZ(desc),
            hdr.compressedPositionQuantExp,
            int3(desc.minQx, desc.minQy, desc.minQz),
            pageSlabDescriptorIndex);
#if defined(PSO_SKINNED)
        SkinningInfluences skinning = SWDecodePackedJoints(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex);
        skinning = SWDecodePackedWeights(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex, skinning);
        localPos = mul(float4(localPos, 1.0f), BuildSkinMatrix(meshInst.skinningInstanceSlot, skinning)).xyz;
#else
        if ((perMeshBuffer[meshInst.perMeshBufferIndex].vertexFlags & VERTEX_SKINNED) != 0u)
        {
            SkinningInfluences skinning = SWDecodePackedJoints(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex);
            skinning = SWDecodePackedWeights(v, hdr, desc, pageSlabByteOffset, pageSlabDescriptorIndex, skinning);
            localPos = mul(float4(localPos, 1.0f), BuildSkinMatrix(meshInst.skinningInstanceSlot, skinning)).xyz;
        }
#endif

        float4 localPos4 = float4(localPos, 1.0f);
        float4 clipPos  = mul(localPos4, modelViewProjection);
        float viewZ = dot(localPos4, modelViewZ);

        float invW = 1.0f / clipPos.w;
        float2 ndc = clipPos.xy * invW;

        float2 screen;
        screen.x = (ndc.x + 1.0f) * 0.5f * visWidth  + scissorMinXf;
        screen.y = (1.0f - ndc.y) * 0.5f * visHeight + scissorMinYf;

        gs_screenPos[v] = screen;
        gs_linearDepth[v] = -viewZ;
        gs_invClipW[v] = invW;
        gs_texcoord[v] = SWDecodeCompressedUV(
            v,
            0u,
            localMeshletIndex,
            pageSlabByteOffset,
            hdr.uvSetCount,
            hdr.uvDescriptorOffset,
            hdr.uvBitstreamDirectoryOffset,
            pageSlabDescriptorIndex);
    }

    GroupMemoryBarrierWithGroupSync();

    RWTexture2D<uint2> debugVisTex =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::DebugVisualization)];
    uint2 visDims;
#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    visDims = uint2(rasterInfo.scissorMaxX, rasterInfo.scissorMaxY);
#else
    RWTexture2D<uint64_t> visBuffer =
        ResourceDescriptorHeap[NonUniformResourceIndex(rasterInfo.visibilityUAVDescriptorIndex)];
    visBuffer.GetDimensions(visDims.x, visDims.y);
#endif
    const bool swRasterDebugMode = perFrameBuffer.outputType == OUTPUT_SW_RASTER;
    const bool assemblyVoxelInheritanceDebugMode = perFrameBuffer.outputType == OUTPUT_CLOD_ASSEMBLY_VOXEL_INHERITANCE;
    const bool assemblyPartsDebugMode = perFrameBuffer.outputType == OUTPUT_CLOD_ASSEMBLY_PARTS;
    const bool debugRasterMode = swRasterDebugMode || assemblyVoxelInheritanceDebugMode || assemblyPartsDebugMode;
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(instanceID);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    StructuredBuffer<ClusterLODGroup> groups =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    const CLodMeshMetadata metadata = metadataBuffer[offsets.clodMeshMetadataIndex];
    const ClusterLODGroup group = groups[metadata.groupsBase + CLodVisibleClusterGroupID(packedCluster)];
    float3 assemblyPartDebugColor = float3(0.08f, 0.10f, 0.12f);
    if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u)
    {
        assemblyPartDebugColor = float3(1.0f, 0.58f, 0.08f);
    }
    else if (assemblyTransformIndex != CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
    {
        assemblyPartDebugColor = max(HashToColor(assemblyTransformIndex + 17u), 0.18f.xxx);
    }
    else if ((group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_PROXY) != 0u)
    {
        assemblyPartDebugColor = float3(0.82f, 0.18f, 1.0f);
    }
    const uint directAssemblyPartId = (group.flags & CLOD_GROUP_FLAG_IS_ASSEMBLY_VOXEL) != 0u
        ? (0x40000000u ^ CLodVisibleClusterGroupID(packedCluster))
        : (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL
            ? 1u
            : (0x80000000u + assemblyTransformIndex));
    const uint2 rasterDebugPayload = swRasterDebugMode
        ? PackDebugUint(1u)
        : (assemblyVoxelInheritanceDebugMode
            ? PackDebugFloat3(assemblyPartDebugColor)
            : PackDebugUint(directAssemblyPartId));

    const bool reverseWinding = (objData.objectFlags & OBJECT_FLAG_REVERSE_WINDING) != 0u;

    ByteAddressBuffer slab = ResourceDescriptorHeap[pageSlabDescriptorIndex];

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
    StructuredBuffer<CLodVirtualShadowClipmapInfo> clipmapInfos =
        ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_CLIPMAP_INFO_DESCRIPTOR_INDEX];
    CLodVirtualShadowClipmapInfo clipmapInfo = (CLodVirtualShadowClipmapInfo)0;
    const bool hasClipmapInfo = CLodVirtualShadowTryGetClipmapInfoForView(viewID, clipmapInfos, clipmapInfo);
    RWTexture2DArray<uint> pageTable = ResourceDescriptorHeap[CLOD_RASTER_VIRTUAL_SHADOW_PAGE_TABLE_DESCRIPTOR_INDEX];
#endif

    uint globalThread = subGroup * SW_RASTER_THREADS + GI;
    uint totalThreads = SW_RASTER_GROUPS_PER_CLUSTER * SW_RASTER_THREADS;

    for (uint t = globalThread; t < triCount; t += totalThreads)
    {
        uint3 tri = SWDecodeTriangle(slab, pageSlabByteOffset + hdr.triangleStreamOffset, desc.triangleByteOffset, t);
        if (reverseWinding) { uint tmp = tri.y; tri.y = tri.z; tri.z = tmp; }

        float2 s0 = gs_screenPos[tri.x];
        float2 s1 = gs_screenPos[tri.y];
        float2 s2 = gs_screenPos[tri.z];

        float depth0 = gs_linearDepth[tri.x];
        float depth1 = gs_linearDepth[tri.y];
        float depth2 = gs_linearDepth[tri.z];
        float invW0 = gs_invClipW[tri.x];
        float invW1 = gs_invClipW[tri.y];
        float invW2 = gs_invClipW[tri.z];

        if (depth0 <= 0.0f || depth1 <= 0.0f || depth2 <= 0.0f) continue;

        float2 e01 = s1 - s0;
        float2 e02 = s2 - s0;
        float twiceArea = e01.x * e02.y - e01.y * e02.x;
        if (debugRasterMode)
        {
            if (abs(twiceArea) <= 1e-8f) continue;

            if (twiceArea > 0.0f)
            {
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
        }
        else if (twiceArea >= 0.0f) continue;

        float invTwiceArea = -1.0f / twiceArea;

        float2 bbMinF = min(min(s0, s1), s2);
        float2 bbMaxF = max(max(s0, s1), s2);

        int2 minPx = int2(floor(bbMinF));
        int2 maxPx = int2(floor(bbMaxF));

        minPx = max(minPx, int2(rasterInfo.scissorMinX, rasterInfo.scissorMinY));
        maxPx = min(maxPx, int2(int(rasterInfo.scissorMaxX) - 1, int(rasterInfo.scissorMaxY) - 1));
        minPx = max(minPx, int2(0, 0));
        maxPx = min(maxPx, int2(int(visDims.x) - 1, int(visDims.y) - 1));
        if (minPx.x > maxPx.x || minPx.y > maxPx.y) continue;

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
        if (!hasClipmapInfo ||
            !CLodVirtualShadowAnyRenderablePageInPixelRect(
                uint2(minPx),
                uint2(maxPx),
                clipmapInfo,
                pageTable))
        {
            continue;
        }
#endif

        float2 origin = float2(float(minPx.x) + 0.5f, float(minPx.y) + 0.5f);

        float2 e12 = s2 - s1;
        float2 e20 = s0 - s2;

        float row_b0 = ((origin.x - s1.x) * e12.y - (origin.y - s1.y) * e12.x) * invTwiceArea;
        float row_b1 = ((origin.x - s2.x) * e20.y - (origin.y - s2.y) * e20.x) * invTwiceArea;
        float row_b2 = 1.0f - row_b0 - row_b1;

        float dx_b0 =  e12.y * invTwiceArea;
        float dx_b1 =  e20.y * invTwiceArea;
        float dy_b0 = -e12.x * invTwiceArea;
        float dy_b1 = -e20.x * invTwiceArea;

        float scanline_b0 = row_b0;
        float scanline_b1 = row_b1;
        const float dx_b2 = -(dx_b0 + dx_b1);
        const int rectWidth = maxPx.x - minPx.x + 1;
        const bool useScanlineRanges = WaveActiveAnyTrue(rectWidth > 4);

        if (useScanlineRanges)
        {
            for (int py = minPx.y; py <= maxPx.y; py++)
            {
                const float scanline_b2 = 1.0f - scanline_b0 - scanline_b1;
                int firstPixelOffset = 0;
                int lastPixelOffset = rectWidth - 1;
                bool hasPixels = true;

                SWRasterClipScanlineConstraint(scanline_b0, dx_b0, firstPixelOffset, lastPixelOffset, hasPixels);
                SWRasterClipScanlineConstraint(scanline_b1, dx_b1, firstPixelOffset, lastPixelOffset, hasPixels);
                SWRasterClipScanlineConstraint(scanline_b2, dx_b2, firstPixelOffset, lastPixelOffset, hasPixels);

                if (hasPixels)
                {
                    float b0 = scanline_b0 + float(firstPixelOffset) * dx_b0;
                    float b1 = scanline_b1 + float(firstPixelOffset) * dx_b1;

                    for (int px = minPx.x + firstPixelOffset; px <= minPx.x + lastPixelOffset; px++)
                    {
                        float b2 = 1.0f - b0 - b1;
#if defined(PSO_ALPHA_TEST) || defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST)
                        const float pc0 = b0 * invW0;
                        const float pc1 = b1 * invW1;
                        const float pc2 = b2 * invW2;
                        const float invSum = rcp(pc0 + pc1 + pc2);
                        const float2 texcoord =
                            (gs_texcoord[tri.x] * pc0 + gs_texcoord[tri.y] * pc1 + gs_texcoord[tri.z] * pc2) * invSum;
                        if (SWAlphaTestFailed(texcoord, materialDataIndex))
                        {
                            b0 += dx_b0;
                            b1 += dx_b1;
                            continue;
                        }
#endif
                        if (debugRasterMode)
                        {
                            WriteDebugPixel(debugVisTex, uint2(px, py), rasterDebugPayload);
                        }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                        float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                        SWRasterWriteVirtualShadow(uint2(px, py), viewID, depth);
#else
                        float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                        uint64_t visKey = PackVisKey(depth, unsortedClusterIndex, t);
                        InterlockedMin(visBuffer[uint2(px, py)], visKey);
#endif
                        b0 += dx_b0;
                        b1 += dx_b1;
                    }
                }

                scanline_b0 += dy_b0;
                scanline_b1 += dy_b1;
            }
        }
        else
        {
            for (int py = minPx.y; py <= maxPx.y; py++)
            {
                float b0 = scanline_b0;
                float b1 = scanline_b1;

                for (int px = minPx.x; px <= maxPx.x; px++)
                {
                    float b2 = 1.0f - b0 - b1;

                    if (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f)
                    {
#if defined(PSO_ALPHA_TEST) || defined(CLOD_SW_RASTER_DYNAMIC_ALPHA_TEST)
                        const float pc0 = b0 * invW0;
                        const float pc1 = b1 * invW1;
                        const float pc2 = b2 * invW2;
                        const float invSum = rcp(pc0 + pc1 + pc2);
                        const float2 texcoord =
                            (gs_texcoord[tri.x] * pc0 + gs_texcoord[tri.y] * pc1 + gs_texcoord[tri.z] * pc2) * invSum;
                        if (SWAlphaTestFailed(texcoord, materialDataIndex))
                        {
                            b0 += dx_b0;
                            b1 += dx_b1;
                            continue;
                        }
#endif
                        if (debugRasterMode)
                        {
                            WriteDebugPixel(debugVisTex, uint2(px, py), rasterDebugPayload);
                        }

#if CLOD_SW_RASTER_OUTPUT_VIRTUAL_SHADOW
                        float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                        SWRasterWriteVirtualShadow(uint2(px, py), viewID, depth);
#else
                        float depth = b0 * depth0 + b1 * depth1 + b2 * depth2;
                        uint64_t visKey = PackVisKey(depth, unsortedClusterIndex, t);
                        InterlockedMin(visBuffer[uint2(px, py)], visKey);
#endif
                    }

                    b0 += dx_b0;
                    b1 += dx_b1;
                }

                scanline_b0 += dy_b0;
                scanline_b1 += dy_b1;
            }
        }
    }
}


// SWRaster: broadcasting work graph node for WG dispatch
[Shader("node")]
[NodeID("SWRaster")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(SW_BATCH_MAX_CLUSTERS * SW_RASTER_GROUPS_PER_CLUSTER, 1, 1)]
[NumThreads(SW_RASTER_THREADS, 1, 1)]
void WG_SWRaster(
    DispatchNodeInputRecord<SWRasterBatchRecord> inputRecord,
    uint GI : SV_GroupIndex,
    uint3 groupId : SV_GroupID)
{
    SWRasterBatchRecord batch = inputRecord.Get();

    // Each cluster gets SW_RASTER_GROUPS_PER_CLUSTER thread groups.
    uint clusterIdx = groupId.x / SW_RASTER_GROUPS_PER_CLUSTER;
    uint subGroup   = groupId.x % SW_RASTER_GROUPS_PER_CLUSTER;

    if (clusterIdx >= batch.numClusters) return;

    // Load VisibleCluster from buffer via indirection
    uint unsortedClusterIndex = batch.clusterIndices[clusterIdx];
    globallycoherent RWByteAddressBuffer visibleClusters =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    RWStructuredBuffer<uint> visibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_WG_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    const uint4 packedCluster = CLodLoadVisibleClusterPackedGloballyCoherent(visibleClusters, unsortedClusterIndex);

    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuf =
        ResourceDescriptorHeap[CLOD_WG_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];
    SWRasterCluster(packedCluster, visibleClusterTransformIndices[unsortedClusterIndex], unsortedClusterIndex, GI, subGroup, viewRasterInfoBuf);
}

// Non-WG SW raster
[shader("compute")]
[numthreads(SW_RASTER_THREADS, 1, 1)]
void SWRasterIndirectCSMain(uint3 dtid : SV_DispatchThreadID, uint GI : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    StructuredBuffer<uint> histogram = ResourceDescriptorHeap[CLOD_RASTER_RASTER_BUCKETS_HISTOGRAM_DESCRIPTOR_INDEX];
    const uint bucketID = IndirectCommandSignatureRootConstant2;
    const uint clusterCount = histogram[bucketID];

    const uint linearizedGroupID = groupId.x + groupId.y * IndirectCommandSignatureRootConstant1;
    const uint clusterIdx = linearizedGroupID / SW_RASTER_GROUPS_PER_CLUSTER;
    const uint subGroup = linearizedGroupID % SW_RASTER_GROUPS_PER_CLUSTER;
    if (clusterIdx >= clusterCount) {
        return;
    }

    const uint sortedClusterIndex = IndirectCommandSignatureRootConstant0 + clusterIdx;
    ByteAddressBuffer compactedVisibleClusters =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTERS_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> compactedVisibleClusterTransformIndices =
        ResourceDescriptorHeap[CLOD_RASTER_COMPACTED_VISIBLE_CLUSTER_TRANSFORM_INDICES_DESCRIPTOR_INDEX];
    StructuredBuffer<uint> sortedToUnsortedMapping =
        ResourceDescriptorHeap[CLOD_RASTER_SORTED_TO_UNSORTED_MAPPING_DESCRIPTOR_INDEX];
    StructuredBuffer<ClodViewRasterInfo> viewRasterInfoBuf =
        ResourceDescriptorHeap[CLOD_RASTER_VIEW_RASTER_INFO_BUFFER_DESCRIPTOR_INDEX];

    const uint4 packedCluster = CLodLoadVisibleClusterPacked(compactedVisibleClusters, sortedClusterIndex);
    const uint unsortedClusterIndex = sortedToUnsortedMapping[sortedClusterIndex];
    SWRasterCluster(packedCluster, compactedVisibleClusterTransformIndices[sortedClusterIndex], unsortedClusterIndex, GI, subGroup, viewRasterInfoBuf);
}
