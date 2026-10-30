#ifndef CLOD_PAGE_VERTEX_DECODE_HLSLI
#define CLOD_PAGE_VERTEX_DECODE_HLSLI

// Per-vertex decode helpers for cluster pages: compressed positions and packed
// skinning joints/weights. Shared by the VSM block-expansion raster and the
// dynamic-wind skin cache. Requires clodPageAccess.hlsli and skinningCommon.hlsli.

float3 CLodPageDecodeCompressedPosition(
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

SkinningInfluences CLodPageDecodePackedJointsScalar(
    uint meshletLocalVertex,
    uint attributeMask,
    uint vertexAttributeOffset,
    uint jointArrayOffset,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex)
{
    SkinningInfluences skinning = (SkinningInfluences)0;
    if ((attributeMask & CLOD_PAGE_ATTRIBUTE_JOINTS) == 0u) return skinning;
    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    uint addr = pageByteOffset + jointArrayOffset + (vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.joints0 = LoadUint4(addr, slab);
    skinning.joints1 = LoadUint4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences CLodPageDecodePackedJoints(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex)
{
    return CLodPageDecodePackedJointsScalar(
        meshletLocalVertex,
        hdr.attributeMask,
        desc.vertexAttributeOffset,
        hdr.jointArrayOffset,
        pageByteOffset,
        pagePoolSlabDescriptorIndex);
}

SkinningInfluences CLodPageDecodePackedWeightsScalar(
    uint meshletLocalVertex,
    uint attributeMask,
    uint vertexAttributeOffset,
    uint weightArrayOffset,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex,
    SkinningInfluences skinning)
{
    if ((attributeMask & CLOD_PAGE_ATTRIBUTE_WEIGHTS) == 0u) return skinning;
    ByteAddressBuffer slab = ResourceDescriptorHeap[pagePoolSlabDescriptorIndex];
    uint addr = pageByteOffset + weightArrayOffset + (vertexAttributeOffset + meshletLocalVertex) * 32u;
    skinning.weights0 = LoadFloat4(addr, slab);
    skinning.weights1 = LoadFloat4(addr + 16u, slab);
    return skinning;
}

SkinningInfluences CLodPageDecodePackedWeights(
    uint meshletLocalVertex,
    CLodPageHeader hdr,
    CLodMeshletDescriptor desc,
    uint pageByteOffset,
    uint pagePoolSlabDescriptorIndex,
    SkinningInfluences skinning)
{
    return CLodPageDecodePackedWeightsScalar(
        meshletLocalVertex,
        hdr.attributeMask,
        desc.vertexAttributeOffset,
        hdr.weightArrayOffset,
        pageByteOffset,
        pagePoolSlabDescriptorIndex,
        skinning);
}

#endif // CLOD_PAGE_VERTEX_DECODE_HLSLI
