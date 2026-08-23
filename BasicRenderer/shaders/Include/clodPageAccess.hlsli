#ifndef CLOD_PAGE_ACCESS_HLSLI
#define CLOD_PAGE_ACCESS_HLSLI

#include "clodStructs.hlsli"

static const uint CLOD_MESHLET_DESCRIPTOR_STRIDE = 64u;
static const uint CLOD_MESHLET_UV_DESCRIPTOR_STRIDE = 32u;

// Load a CLodPageHeader from a slab ByteAddressBuffer at a given byte offset.
// The header occupies 16 x uint32 = 64 bytes at the start of each page tile.
CLodPageHeader LoadPageHeader(uint slabDescriptorIndex, uint pageByteOffset)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    uint4 d0 = slab.Load4(pageByteOffset + CLOD_PAGE_HEADER_FORMAT_AND_KIND_BYTE_OFFSET);
    uint4 d1 = slab.Load4(pageByteOffset + CLOD_PAGE_HEADER_COMPRESSED_POSITION_QUANT_EXP_BYTE_OFFSET);
    uint4 d2 = slab.Load4(pageByteOffset + CLOD_PAGE_HEADER_POSITION_BITSTREAM_OFFSET_BYTE_OFFSET);

    CLodPageHeader hdr;
    hdr.formatAndKind              = d0.x;
    hdr.meshletCount               = d0.y;
    hdr.descriptorOffset           = d0.z;
    hdr.boneIndexStreamOffset      = d0.w;
    hdr.compressedPositionQuantExp = d1.x;
    hdr.attributeMask              = d1.y;
    hdr.uvSetCount                 = d1.z;
    hdr.uvDescriptorOffset         = d1.w;
    hdr.positionBitstreamOffset    = d2.x;
    hdr.normalArrayOffset          = d2.y;
    hdr.colorArrayOffset           = d2.z;
    hdr.jointArrayOffset           = d2.w;
    uint4 d3 = slab.Load4(pageByteOffset + CLOD_PAGE_HEADER_WEIGHT_ARRAY_OFFSET_BYTE_OFFSET);
    hdr.weightArrayOffset          = d3.x;
    hdr.uvBitstreamDirectoryOffset = d3.y;
    hdr.triangleStreamOffset       = d3.z;
    hdr.tangentFrameArrayOffset    = d3.w;

    return hdr;
}

// Load a CLodMeshletDescriptor from a slab ByteAddressBuffer.
CLodMeshletDescriptor LoadMeshletDescriptor(uint slabDescriptorIndex, uint pageByteOffset, uint descriptorOffset, uint meshletIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    uint addr = pageByteOffset + descriptorOffset + meshletIndex * CLOD_MESHLET_DESCRIPTOR_STRIDE;
    uint4 d0 = slab.Load4(addr +  0);
    uint4 d1 = slab.Load4(addr + 16);
    uint4 d2 = slab.Load4(addr + 32);
    uint4 d3 = slab.Load4(addr + 48);

    CLodMeshletDescriptor desc;
    desc.bounds                       = asfloat(d0);
    desc.positionBitOffset            = d1.x;
    desc.triangleCountAndRefinedGroup = d1.y;
    desc.boneListOffset               = d1.z;
    desc.boneCount                    = d1.w;
    desc.vertexAttributeOffset        = d2.x;
    desc.triangleByteOffset           = d2.y;
    desc.minQx                        = asint(d2.z);
    desc.minQy                        = asint(d2.w);
    desc.minQz                        = asint(d3.x);
    desc.bitsAndVertexCount           = d3.y;
    desc.sourceGroupLocalIndex        = d3.z;
    desc.terrainRvtLocalSkyrimXYRadius = asfloat(d3.w);

    return desc;
}

// Rigid culling needs only the sphere and packed refined-group word.
CLodClusterCullHeader LoadMeshletCullHeader(
    uint slabDescriptorIndex,
    uint pageByteOffset,
    uint descriptorOffset,
    uint meshletIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    const uint addr = pageByteOffset + descriptorOffset + meshletIndex * CLOD_MESHLET_DESCRIPTOR_STRIDE;
    const uint4 d0 = slab.Load4(addr);
    const uint triangleCountAndRefinedGroup = slab.Load(addr + 20u);

    CLodClusterCullHeader header;
    header.bounds = asfloat(d0);
    header.payloadBase = 0u;
    header.primitiveCountAndRefinedGroup = triangleCountAndRefinedGroup;
    header.boneListOffset = 0u;
    header.kindFlagsAndBoneCount = 0u;
    return header;
}

CLodMeshletUvDescriptor LoadMeshletUvDescriptor(
    uint slabDescriptorIndex,
    uint pageByteOffset,
    uint uvDescriptorOffset,
    uint pageUvSetCount,
    uint meshletIndex,
    uint uvSetIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    uint descriptorIndex = meshletIndex * pageUvSetCount + uvSetIndex;
    uint addr = pageByteOffset + uvDescriptorOffset + descriptorIndex * CLOD_MESHLET_UV_DESCRIPTOR_STRIDE;
    uint4 d0 = slab.Load4(addr + 0);
    uint4 d1 = slab.Load4(addr + 16);

    CLodMeshletUvDescriptor desc;
    desc.uvBitOffset = d0.x;
    desc.uvMinU = asfloat(d0.y);
    desc.uvMinV = asfloat(d0.z);
    desc.uvScaleU = asfloat(d0.w);
    desc.uvScaleV = asfloat(d1.x);
    desc.uvBits = d1.y;
    desc.reserved0 = d1.z;
    desc.reserved1 = d1.w;
    return desc;
}

uint LoadPageUvBitstreamOffset(uint slabDescriptorIndex, uint pageByteOffset, uint uvBitstreamDirectoryOffset, uint uvSetIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    return slab.Load(pageByteOffset + uvBitstreamDirectoryOffset + uvSetIndex * 4u);
}

// Load only the meshletCount from the common page prefix (second uint32).
uint LoadPageMeshletCount(uint slabDescriptorIndex, uint pageByteOffset)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    return slab.Load(pageByteOffset + CLOD_PAGE_HEADER_MESHLET_COUNT_BYTE_OFFSET);
}

// Resolve a group-local page index to a physical slab location via the GroupPageMap buffer.
GroupPageMapEntry LoadGroupPageMapEntry(uint pageMapBase, uint pageIndex)
{
    StructuredBuffer<GroupPageMapEntry> pageMap =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::GroupPageMap)];
    return pageMap[pageMapBase + pageIndex];
}

#endif // CLOD_PAGE_ACCESS_HLSLI
