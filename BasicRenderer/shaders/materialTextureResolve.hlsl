#include "include/cbuffers.hlsli"
#include "include/structs.hlsli"
#include "include/materialTextureBinding.hlsli"

// ResolveMaterialTextureDescriptorsPass: for each row of the material evaluation table, the descriptor each texture slot
// samples this frame (ResolveMaterialTextureDescriptor), so material evaluation reads it by material index instead of
// chaining through the texture's streaming row per pixel (materialTextureBinding.hlsli).
[numthreads(MATERIAL_RESOLVED_TEXTURE_RESOLVE_GROUP_SIZE, 1, 1)]
void ResolveMaterialTextureDescriptorsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint row = dispatchThreadId.x;
    StructuredBuffer<MaterialEvalInfo> materials = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerMaterialEvalDataBuffer)];
    uint rowCount;
    uint rowStride;
    materials.GetDimensions(rowCount, rowStride);
    RWStructuredBuffer<uint4> resolvedTextures =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Material::ResolvedTextureDescriptors)];
    uint entries;
    uint entryStride;
    resolvedTextures.GetDimensions(entries, entryStride);
    // Material indices index the evaluation table, so rows past it are never read.
    if (row >= rowCount || row >= entries / MATERIAL_RESOLVED_TEXTURE_UINT4_PER_ROW)
    {
        return;
    }

    const MaterialEvalInfo material = materials[row];
    resolvedTextures[row * MATERIAL_RESOLVED_TEXTURE_UINT4_PER_ROW] = uint4(
        ResolveMaterialTextureDescriptor(material.baseColorTextureIndex, material.baseColorStreamingTextureID),
        ResolveMaterialTextureDescriptor(material.normalTextureIndex, material.normalStreamingTextureID),
        ResolveMaterialTextureDescriptor(material.metallicTextureIndex, material.metallicStreamingTextureID),
        ResolveMaterialTextureDescriptor(material.roughnessTextureIndex, material.roughnessStreamingTextureID));
    resolvedTextures[row * MATERIAL_RESOLVED_TEXTURE_UINT4_PER_ROW + 1u] = uint4(
        ResolveMaterialTextureDescriptor(material.opacityTextureIndex, material.opacityStreamingTextureID),
        ResolveMaterialTextureDescriptor(material.aoMapIndex, material.aoStreamingTextureID),
        ResolveMaterialTextureDescriptor(material.emissiveTextureIndex, material.emissiveStreamingTextureID),
        ResolveMaterialTextureDescriptor(material.heightMapIndex, material.heightStreamingTextureID));
}
