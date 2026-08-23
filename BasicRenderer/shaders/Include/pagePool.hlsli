#ifndef PAGE_POOL_HLSLI
#define PAGE_POOL_HLSLI

// Page-pool constants - retained for virtual-page users. Runtime CLod group
// maps carry the descriptor and exact byte offset directly.
#define PAGE_POOL_PAGE_SIZE  256 * 1024

struct PagePoolAddress {
    uint slabIndex;
    uint slabByteOffset;
};

PagePoolAddress ResolvePageAddress(
    StructuredBuffer<PageTableEntry> pageTable,
    uint pageID,
    uint intraPageByteOffset)
{
    PageTableEntry entry = pageTable[pageID];
    PagePoolAddress addr;
    addr.slabIndex = entry.slabIndex;
    addr.slabByteOffset = entry.slabByteOffset + intraPageByteOffset;
    return addr;
}

ByteAddressBuffer GetPagePoolSlab(uint pagePoolSlabBase, uint slabIndex)
{
    return ResourceDescriptorHeap[pagePoolSlabBase + slabIndex];
}

#endif // PAGE_POOL_HLSLI
