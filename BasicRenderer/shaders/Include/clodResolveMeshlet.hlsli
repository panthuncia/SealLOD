#ifndef CLOD_RESOLVE_MESHLET_HLSLI
#define CLOD_RESOLVE_MESHLET_HLSLI

// Everything a resolve reads from a visible cluster's page: stream addresses, vertices, skinning
// and triangles.

struct MeshletResolveData {
    uint2 drawcallAndMeshlet;
    uint2 objAndMesh;
    uint4 meshInfo;
    uint materialDataIndex;
    uint vertexCount;
    uint triangleCount;
    uint bitsX;
    uint bitsY;
    uint bitsZ;
    int3 minQ;
    uint positionBitOffset;
    uint vertexAttributeOffset;
    uint triangleByteOffset;
    uint pageAttributeMask;
    uint uvSetCount;
    uint uvDescriptorBase;
    uint uvBitstreamDirectoryBase;
    uint pageByteOffset;
    uint positionBitstreamBase;
    uint normalArrayBase;
    uint tangentFrameArrayBase;
    uint colorArrayBase;
    uint jointArrayBase;
    uint weightArrayBase;
    uint triangleStreamBase;
    uint skinningInstanceSlot;
    uint compressedPositionQuantExp;
    uint pagePoolSlabDescriptorIndex;
};

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

float3 DecodeCompressedPosition(uint meshletLocalVertex, MeshletResolveData d)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    return CLodLoadPagePosition(slab, d.compressedPositionQuantExp, d.positionBitstreamBase, d.positionBitOffset, meshletLocalVertex);
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

float3 DecodeCompressedNormal(uint meshletLocalVertex, MeshletResolveData d)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    uint addr = d.normalArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 4u;
    uint packed = slab.Load(addr);
    return OctDecodeNormal(UnpackSnorm16x2(packed));
}

void CLodBuildTangentAngleBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    normal = normalize(normal);
    float3 helper = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    tangent = normalize(cross(helper, normal));
    bitangent = normalize(cross(tangent, normal));
}

float4 DecodeCompressedTangentFrame(uint meshletLocalVertex, float3 normalOS, MeshletResolveData d)
{
    if ((d.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_TANGENT_FRAME) == 0u)
    {
        return float4(1.0f, 0.0f, 0.0f, 0.0f);
    }

    uint addr = d.tangentFrameArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 4u;
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    uint packed = slab.Load(addr);
    float angle = (float(packed & 0xFFFFu) / 65535.0f) * 6.28318530718f;
    float handedness = ((packed & (1u << 16u)) != 0u) ? -1.0f : 1.0f;
    float s;
    float c;
    sincos(angle, s, c);
    float3 basisT;
    float3 basisB;
    CLodBuildTangentAngleBasis(normalOS, basisT, basisB);
    return float4(normalize(basisT * c + basisB * s), handedness);
}

float3 DecodeCompressedColor(uint meshletLocalVertex, MeshletResolveData d)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    uint addr = d.colorArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 4u;
    uint packed = slab.Load(addr);
    return float3(
        float(packed & 0xFFu) / 255.0f,
        float((packed >> 8u) & 0xFFu) / 255.0f,
        float((packed >> 16u) & 0xFFu) / 255.0f);
}

SkinningInfluences DecodePackedJoints(uint meshletLocalVertex, MeshletResolveData d)
{
    SkinningInfluences skinning;
    skinning.joints0 = uint4(0, 0, 0, 0);
    skinning.joints1 = uint4(0, 0, 0, 0);
    skinning.weights0 = float4(0.0f, 0.0f, 0.0f, 0.0f);
    skinning.weights1 = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if ((d.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    uint addr = d.jointArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.joints0 = LoadUint4(addr, slab);
    skinning.joints1 = LoadUint4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences DecodePackedWeights(uint meshletLocalVertex, MeshletResolveData d, SkinningInfluences skinning)
{
    if ((d.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) == 0u)
    {
        return skinning;
    }

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    uint addr = d.weightArrayBase + (d.vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.weights0 = LoadFloat4(addr, slab);
    skinning.weights1 = LoadFloat4(addr + 16u, slab);
    return skinning;
}

void ApplyClodSkinningToFrame(
    uint meshletLocalVertex,
    MeshletResolveData d,
    uint assemblyTransformIndex,
    inout float3 positionOS,
    inout float3 normalOS,
    inout float4 tangentOS)
{
    if ((d.meshInfo.y & VERTEX_SKINNED) == 0u)
    {
        return;
    }

    SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, d);
    skinning = DecodePackedWeights(meshletLocalVertex, d, skinning);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(d.objAndMesh.x);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    skinning = ResolveAssemblySkinningInfluences(
        skinning,
        metadataBuffer[offsets.clodMeshMetadataIndex],
        assemblyTransformIndex);
    float4x4 skinMatrix = BuildAssemblyLocalSkinMatrix(
        d.skinningInstanceSlot, skinning, assemblyTransformIndex);
    positionOS = mul(float4(positionOS, 1.0f), skinMatrix).xyz;
    normalOS = mul(normalOS, (float3x3)skinMatrix);
    tangentOS.xyz = mul(tangentOS.xyz, (float3x3)skinMatrix);
}

void ApplyClodSkinning(
    uint meshletLocalVertex,
    MeshletResolveData d,
    uint assemblyTransformIndex,
    inout float3 positionOS,
    inout float3 normalOS)
{
    float4 unusedTangent = float4(1.0f, 0.0f, 0.0f, 0.0f);
    ApplyClodSkinningToFrame(meshletLocalVertex, d, assemblyTransformIndex, positionOS, normalOS, unusedTangent);
}

// One cluster's decode data, loaded by the calling lane.
MeshletResolveData LoadMeshletResolveData(uint clusterIndex)
{
    MeshletResolveData d = (MeshletResolveData)0;
    ByteAddressBuffer visibleClusterBuffer = ResourceDescriptorHeap[VISBUF_VISIBLE_CLUSTERS_BUFFER_DESCRIPTOR_INDEX];
    StructuredBuffer<PerMeshBuffer> perMeshBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMeshBuffer)];

#if defined(CLOD_RESOLVE_RAY_HIT)
    // A ray hit has no visible cluster record: the caller builds it from the CLAS.
    const uint4 packedCluster = g_clodRtPackedCluster;
#else
    const uint4 packedCluster = CLodLoadVisibleClusterPacked(visibleClusterBuffer, clusterIndex);
#endif
    d.drawcallAndMeshlet.x = CLodVisibleClusterInstanceID(packedCluster);
    d.drawcallAndMeshlet.y = CLodVisibleClusterLocalMeshletIndex(packedCluster);

    PerMeshInstanceBuffer inst = LoadMeshTemplateForDraw(d.drawcallAndMeshlet.x);
    inst.skinningInstanceSlot = ResolveProceduralWindSkinningSlot(
        d.drawcallAndMeshlet.x, inst.skinningInstanceSlot);
    d.objAndMesh = uint2(d.drawcallAndMeshlet.x, inst.perMeshBufferIndex);
    d.skinningInstanceSlot = inst.skinningInstanceSlot;

    PerMeshBuffer mesh = perMeshBuffer[d.objAndMesh.y];

    const uint pageSlabDesc = CLodVisibleClusterPageSlabDescriptorIndex(packedCluster);
    const uint pageSlabOff = CLodVisibleClusterPageSlabByteOffset(packedCluster);

    CLodPageHeader hdr = LoadPageHeader(pageSlabDesc, pageSlabOff);
    CLodMeshletDescriptor desc = LoadMeshletDescriptor(
        pageSlabDesc, pageSlabOff, hdr.descriptorOffset, d.drawcallAndMeshlet.y);

    d.meshInfo = uint4(mesh.vertexByteSize, mesh.vertexFlags, mesh.numVertices, 0);
    d.materialDataIndex = mesh.materialDataIndex;

    d.vertexCount = CLodDescVertexCount(desc);
    d.triangleCount = CLodDescTriangleCount(desc);
    d.bitsX = CLodDescBitsX(desc);
    d.bitsY = CLodDescBitsY(desc);
    d.bitsZ = CLodDescBitsZ(desc);
    d.minQ = int3(desc.minQx, desc.minQy, desc.minQz);
    d.positionBitOffset = desc.positionBitOffset;
    d.vertexAttributeOffset = desc.vertexAttributeOffset;
    d.triangleByteOffset = desc.triangleByteOffset;
    d.pageAttributeMask = hdr.attributeMask;
    d.uvSetCount = hdr.uvSetCount;

    d.pageByteOffset = pageSlabOff;
    d.uvDescriptorBase = pageSlabOff + hdr.uvDescriptorOffset;
    d.uvBitstreamDirectoryBase = pageSlabOff + hdr.uvBitstreamDirectoryOffset;
    d.positionBitstreamBase = pageSlabOff + hdr.positionBitstreamOffset;
    d.normalArrayBase = pageSlabOff + hdr.normalArrayOffset;
    d.tangentFrameArrayBase = pageSlabOff + hdr.tangentFrameArrayOffset;
    d.colorArrayBase = pageSlabOff + hdr.colorArrayOffset;
    d.jointArrayBase = pageSlabOff + hdr.jointArrayOffset;
    d.weightArrayBase = pageSlabOff + hdr.weightArrayOffset;
    d.triangleStreamBase = pageSlabOff + hdr.triangleStreamOffset;

    d.compressedPositionQuantExp = hdr.compressedPositionQuantExp;
    d.pagePoolSlabDescriptorIndex = pageSlabDesc;
    return d;
}

// The same, loaded once per distinct cluster in the wave: each iteration takes the first active lane's cluster (a
// wave-uniform index, so the loads are scalar) and retires the lanes that share it. WaveReadLaneAt with a per-group leader
// is not this: its lane index must be wave-uniform, and drivers that scalarize it hand every lane the first group's data.
MeshletResolveData LoadMeshletResolveData_Wave(uint clusterIndex)
{
#if defined(CLOD_RESOLVE_RAY_HIT)
    return LoadMeshletResolveData(clusterIndex); // Threads see unrelated clusters, so there's nothing to share.
#endif
    MeshletResolveData d = (MeshletResolveData)0;
    [loop]
    for (;;)
    {
        const uint uniformCluster = WaveReadLaneFirst(clusterIndex);
        if (clusterIndex == uniformCluster)
        {
            d = LoadMeshletResolveData(uniformCluster);
            break;
        }
    }
    return d;
}

void ApplyPreviousClodSkinningToPosition(
    uint meshletLocalVertex,
    MeshletResolveData d,
    uint assemblyTransformIndex,
    inout float3 positionOS)
{
    if ((d.meshInfo.y & VERTEX_SKINNED) == 0u)
    {
        return;
    }

    SkinningInfluences skinning = DecodePackedJoints(meshletLocalVertex, d);
    skinning = DecodePackedWeights(meshletLocalVertex, d, skinning);
    const MeshInstanceClodOffsets offsets = LoadCLodOffsetsForDraw(d.objAndMesh.x);
    StructuredBuffer<CLodMeshMetadata> metadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    skinning = ResolveAssemblySkinningInfluences(
        skinning,
        metadataBuffer[offsets.clodMeshMetadataIndex],
        assemblyTransformIndex);
    const float4x4 previousSkinMatrix = BuildPreviousAssemblyLocalSkinMatrix(
        d.skinningInstanceSlot, skinning, assemblyTransformIndex);
    positionOS = mul(float4(positionOS, 1.0f), previousSkinMatrix).xyz;
}

uint3 DecodeTriangleCompact(uint triLocalIndex, MeshletResolveData d)
{
    uint triOffset = d.triangleStreamBase + d.triangleByteOffset + triLocalIndex * 3u;

    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(d.pagePoolSlabDescriptorIndex)];
    uint alignedOffset = (triOffset / 4u) * 4u;
    uint firstWord = slab.Load(alignedOffset);
    uint byteOffset = triOffset % 4u;

    uint b0 = (firstWord >> (byteOffset * 8u)) & 0xFFu;
    uint b1, b2;

    if (byteOffset <= 1u)
    {
        b1 = (firstWord >> ((byteOffset + 1u) * 8u)) & 0xFFu;
        b2 = (firstWord >> ((byteOffset + 2u) * 8u)) & 0xFFu;
    }
    else if (byteOffset == 2u)
    {
        b1 = (firstWord >> ((byteOffset + 1u) * 8u)) & 0xFFu;
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

float2 ComputeClodMotionVector(float3 previousPosOS, float3 worldPosition, float4x4 prevModel, float4x4 unjitteredViewProj, float4x4 prevUnjitteredViewProj)
{
    float4 clipCur = mul(float4(worldPosition, 1.0f), unjitteredViewProj);
    float3 prevWorldPosition = mul(float4(previousPosOS, 1.0f), prevModel).xyz;
    float4 clipPrev = mul(float4(prevWorldPosition, 1.0f), prevUnjitteredViewProj);

    float2 ndcCur = clipCur.xy / clipCur.w;
    float2 ndcPrev = clipPrev.xy / clipPrev.w;
    return ndcCur - ndcPrev;
}

#endif // CLOD_RESOLVE_MESHLET_HLSLI
