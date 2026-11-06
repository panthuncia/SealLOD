#ifndef CLOD_MATERIAL_PALETTE_HLSLI
#define CLOD_MATERIAL_PALETTE_HLSLI

// Hierarchy-root material palettes (docs/merge_hierarchies.md).
//
// Every cluster names a part-local material slot in its page descriptor (word 15). A cluster reached through a portal
// adds that portal occurrence's material base (Builtin::CLod::AssemblyMaterialBases, one per assembly transform), and
// the drawing root's palette (PerMeshInstanceBuffer::materialPaletteBase/Count) maps the sum to a material. Culling
// writes the sum into the visible cluster's side band (.y), so later passes never touch the page for it.
//
// A template with no palette (count 0 or 1) is an ordinary mesh: its material is the PerMeshBuffer's, and every
// cluster's palette index is 0.

struct CLodMaterialPaletteEntry
{
    uint materialDataIndex;
    uint materialEvalCompileFlagsID;
    uint materialReyesEvalCompileFlagsID;
    uint rasterBucketIndex;
};

// The visible cluster carries no palette index (a ray hit): read the slot from the page.
static const uint CLOD_PALETTE_INDEX_FROM_PAGE = 0xFFFFFFFFu;
static const uint CLOD_MESHLET_DESCRIPTOR_MATERIAL_SLOT_BYTE = 60u; // word 15 of the 64-byte descriptor

bool CLodTemplateHasPalette(PerMeshInstanceBuffer meshTemplate)
{
    return meshTemplate.materialPaletteCount > 1u;
}

uint CLodLoadMeshletMaterialSlot(uint slabDescriptorIndex, uint pageByteOffset, uint descriptorOffset, uint meshletIndex)
{
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(slabDescriptorIndex)];
    return slab.Load(pageByteOffset + descriptorOffset + meshletIndex * 64u + CLOD_MESHLET_DESCRIPTOR_MATERIAL_SLOT_BYTE);
}

uint CLodAssemblyMaterialBase(uint assemblyTransformIndex)
{
    if (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
    {
        return 0u;
    }
    StructuredBuffer<uint> bases = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::AssemblyMaterialBases)];
    uint count = 0u;
    uint stride = 0u;
    bases.GetDimensions(count, stride);
    return assemblyTransformIndex < count ? bases[assemblyTransformIndex] : 0u;
}

// Whether the part owning a group stores material slots in its descriptors. A part with one slot does not: word 15 is
// its terrain data, and its slot is 0.
bool CLodGroupPartHasMaterialSlots(uint globalGroupIndex)
{
    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    StructuredBuffer<CLodMeshMetadata> metadata = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    return metadata[groups[globalGroupIndex].partIndex].materialSlotCount > 1u;
}

// A cluster's palette index from its descriptor word 15 (the descriptor's terrainRvtLocalSkyrimXYRadius field), for
// passes without culling's side band.
uint CLodPaletteIndexFromDescriptor(PerMeshInstanceBuffer meshTemplate, uint globalGroupIndex, uint assemblyTransformIndex, uint descriptorWord15)
{
    if (!CLodTemplateHasPalette(meshTemplate))
    {
        return 0u;
    }
    return CLodAssemblyMaterialBase(assemblyTransformIndex) +
        (CLodGroupPartHasMaterialSlots(globalGroupIndex) ? descriptorWord15 : 0u);
}

// The PerMeshBuffer row with a cluster's geometry fields (vertex layout flags, sizes): its draw's own, unless a part
// portal reached it, when it is the part's, found through the cluster's group. Its material still comes from the
// drawing root (CLodResolveMaterial).
uint CLodClusterGeometryPerMeshIndex(uint drawPerMeshIndex, uint globalGroupIndex, uint assemblyTransformIndex)
{
    if (assemblyTransformIndex == CLOD_ASSEMBLY_TRANSFORM_SENTINEL)
    {
        return drawPerMeshIndex;
    }
    StructuredBuffer<ClusterLODGroup> groups = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::Groups)];
    StructuredBuffer<CLodMeshMetadata> metadata = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MeshMetadata)];
    return metadata[groups[globalGroupIndex].partIndex].perMeshBufferIndex;
}

CLodMaterialPaletteEntry CLodResolveMaterial(PerMeshInstanceBuffer meshTemplate, PerMeshBuffer mesh, uint paletteIndex)
{
    CLodMaterialPaletteEntry entry;
    entry.materialDataIndex = mesh.materialDataIndex;
    entry.materialEvalCompileFlagsID = mesh.materialEvalCompileFlagsID;
    entry.materialReyesEvalCompileFlagsID = mesh.materialReyesEvalCompileFlagsID;
    entry.rasterBucketIndex = mesh.rasterBucketIndex;
    if (CLodTemplateHasPalette(meshTemplate))
    {
        StructuredBuffer<CLodMaterialPaletteEntry> palettes =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::CLod::MaterialPalettes)];
        entry = palettes[meshTemplate.materialPaletteBase + min(paletteIndex, meshTemplate.materialPaletteCount - 1u)];
    }
    return entry;
}

#endif // CLOD_MATERIAL_PALETTE_HLSLI
