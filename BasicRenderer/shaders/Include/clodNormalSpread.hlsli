#ifndef CLOD_NORMAL_SPREAD_HLSLI
#define CLOD_NORMAL_SPREAD_HLSLI

// Normal spread blocks (stage 6 step 3, docs/clod-appearance-stage6.md): a coarse cluster carries, after its page's triangle stream
// and any UV remap blocks, one byte per vertex holding sqrt(v), v the second moment of the original surface's normals about the
// cluster's interpolated normal over the area that vertex stands for; its UV set 0 descriptor's reserved1 is the block's
// page-relative byte offset (0: none). Material evaluation interpolates v and widens the material's roughness by it
// (alpha'^2 = alpha^2 + scale * v, Toksvig / LEAN-style), so a coarse surface standing for a bumpy or folded one keeps the leaves'
// highlight width and energy. The layout and the reference are ClusterLODNormalSpread.h's; this follows it.
// Requires clodResolveMeshlet.hlsli and clodPageAccess.hlsli.

#ifndef CLOD_NORMAL_SPREAD
#define CLOD_NORMAL_SPREAD 1
#endif

static const uint CLOD_NORMAL_SPREAD_HEADER_BYTES = 4u;

float CLodNormalSpreadVertex(ByteAddressBuffer slab, uint blockAddress, uint meshletLocalVertex)
{
    const uint address = blockAddress + CLOD_NORMAL_SPREAD_HEADER_BYTES + meshletLocalVertex;
    const uint word = slab.Load(address & ~3u);
    const float root = float((word >> ((address & 3u) * 8u)) & 0xFFu) * (1.0f / 255.0f);
    return root * root;
}

// v at barycentrics lambda of the triangle triIdx (meshlet-local vertices, in the order lambda weights them); 0 without a block.
float CLodLoadNormalSpread(MeshletResolveData md, uint3 triIdx, float3 lambda)
{
    if ((md.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL_SPREAD) == 0u || md.uvSetCount == 0u)
    {
        return 0.0f;
    }
    const CLodMeshletUvDescriptor desc = LoadMeshletUvDescriptor(
        md.pagePoolSlabDescriptorIndex,
        md.pageByteOffset,
        md.uvDescriptorBase - md.pageByteOffset,
        md.uvSetCount,
        md.drawcallAndMeshlet.y,
        0u);
    if (desc.reserved1 == 0u)
    {
        return 0.0f;
    }
    ByteAddressBuffer slab = ResourceDescriptorHeap[NonUniformResourceIndex(md.pagePoolSlabDescriptorIndex)];
    const uint block = md.pageByteOffset + desc.reserved1;
    return max(dot(lambda, float3(
        CLodNormalSpreadVertex(slab, block, triIdx.x),
        CLodNormalSpreadVertex(slab, block, triIdx.y),
        CLodNormalSpreadVertex(slab, block, triIdx.z))), 0.0f);
}

// Perceptual roughness (alpha = roughness^2, as FragmentInfo and the BRDF take it) widened by v, by at most cap times the lobe's own
// variance: alpha'^2 = alpha^2 + min(v, cap alpha^2) (ClusterLODNormalSpread.h's WidenPerceptualRoughness).
float CLodWidenPerceptualRoughness(float perceptualRoughness, float v, float cap)
{
    const float alpha = perceptualRoughness * perceptualRoughness;
    const float alpha2 = alpha * alpha;
    return min(max(sqrt(sqrt(alpha2 + min(v, cap * alpha2))), perceptualRoughness), 1.0f);
}

// Widens the specular and coat roughness of a CLod surface by its cluster's stored normal spread, scaled by
// clodNormalSpreadRoughnessScale and capped at clodNormalSpreadRoughnessCap times the lobe's own variance (PerFrameBuffer).
// Clusters without a block, and a scale or cap of 0 (binaries that still write those words as padding), leave them as they are.
void CLodApplyNormalSpread(MeshletResolveData md, uint3 triIdx, float3 lambda, inout MaterialInputs materialInputs)
{
#if CLOD_NORMAL_SPREAD
    ConstantBuffer<PerFrameBuffer> spreadFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    // Diagnostics (clodDebugMaterialRoughness / clodDebugMaterialMetallic): a uniform material, to judge specular appearance.
    // Stored plus one, 0 off.
    if (spreadFrame.clodDebugMaterialRoughnessPlusOne > 0.5f)
    {
        materialInputs.roughness = spreadFrame.clodDebugMaterialRoughnessPlusOne - 1.0f;
    }
    if (spreadFrame.clodDebugMaterialMetallicPlusOne > 0.5f)
    {
        materialInputs.metallic = spreadFrame.clodDebugMaterialMetallicPlusOne - 1.0f;
    }
    if ((md.pageAttributeMask & CLOD_PAGE_ATTRIBUTE_NORMAL_SPREAD) == 0u)
    {
        return;
    }
    const float scale = spreadFrame.clodNormalSpreadRoughnessScale;
    if (!(scale > 0.0f))
    {
        return;
    }
    const float v = scale * CLodLoadNormalSpread(md, triIdx, lambda);
    if (v > 0.0f)
    {
        const float cap = max(spreadFrame.clodNormalSpreadRoughnessCap, 0.0f);
        materialInputs.roughness = CLodWidenPerceptualRoughness(materialInputs.roughness, v, cap);
        materialInputs.coatRoughness = CLodWidenPerceptualRoughness(materialInputs.coatRoughness, v, cap);
    }
#endif
}

#endif
