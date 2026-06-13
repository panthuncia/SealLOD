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
    uint4 d0 = slab.Load4(pageByteOffset +  0);
    uint4 d1 = slab.Load4(pageByteOffset + 16);
    uint4 d2 = slab.Load4(pageByteOffset + 32);

    CLodPageHeader hdr;
    hdr.meshletCount               = d0.x;
    hdr.compressedPositionQuantExp = d0.y;
    hdr.attributeMask              = d0.z;
    hdr.uvSetCount                 = d0.w;

    hdr.descriptorOffset           = d1.x;
    hdr.uvDescriptorOffset         = d1.y;
    hdr.positionBitstreamOffset    = d1.z;
    hdr.normalArrayOffset          = d1.w;
    hdr.colorArrayOffset           = d2.x;
    hdr.jointArrayOffset           = d2.y;
    hdr.weightArrayOffset          = d2.z;
    hdr.uvBitstreamDirectoryOffset = d2.w;
    uint4 d3 = slab.Load4(pageByteOffset + 48);
    hdr.triangleStreamOffset       = d3.x;
    hdr.boneIndexStreamOffset      = d3.y;
    hdr.tangentFrameArrayOffset    = d3.z;
    hdr.reserved1 = d3.w;

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
    desc.positionBitOffset           = d0.x;
    desc.vertexAttributeOffset       = d0.y;
    desc.triangleByteOffset          = d0.z;
    desc.boneListOffset              = d0.w;
    desc.minQx                       = asint(d1.x);
    desc.minQy                       = asint(d1.y);
    desc.minQz                       = asint(d1.z);
    desc.bitsAndVertexCount          = d1.w;
    desc.triangleCountAndRefinedGroup = d2.x;
    desc.boneCount                   = d2.y;
    desc.sourceGroupLocalIndex       = d2.z;
    desc.terrainRvtLocalSkyrimXYRadius = asfloat(d2.w);
    desc.bounds                      = asfloat(d3);

    return desc;
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

// Load only the meshletCount from the page header (first uint32).
uint LoadPageMeshletCount(uint slabDescriptorIndex, uint pageByteOffset)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    return slab.Load(pageByteOffset); // meshletCount is uint[0]
}

// Resolve a group-local page index to a physical slab location via the GroupPageMap buffer.
GroupPageMapEntry LoadGroupPageMapEntry(uint pageMapBase, uint pageIndex)
{
    StructuredBuffer<GroupPageMapEntry> pageMap =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::GroupPageMap)];
    return pageMap[pageMapBase + pageIndex];
}

#endif // CLOD_PAGE_ACCESS_HLSLI
