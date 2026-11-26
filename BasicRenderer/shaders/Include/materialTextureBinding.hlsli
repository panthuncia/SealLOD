#ifndef MATERIAL_TEXTURE_BINDING_HLSLI
#define MATERIAL_TEXTURE_BINDING_HLSLI

#include "include/structs.hlsli"
#include "include/cbuffers.hlsli"
#include "include/materialUvTransform.hlsli"
#include "PerPassRootConstants/materialTextureResolveConstants.h"

// A material texture is bound two ways. Its streaming row (the texture image table) names the image the streaming
// system last published; the material's own descriptor index is set when the material is built, and is a neutral
// fallback while the texture has no image (MaterialTextureFallbacks). Only an unstreamed texture (ID 0) is current
// through the material's index alone, so every sampler of a material texture resolves through the row first.

// The table is read through the per-frame buffer's index, not a declared binding: a publication of the table then changes no
// recording (M6 step 5). Passes still declare Builtin::Material::TextureStreamingMetadataBuffer, for the table's upload waits.
TextureStreamingGPUInfo LoadTextureStreamingInfo(uint streamingTextureID)
{
    ConstantBuffer<PerFrameBuffer> textureTablePerFrame = ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::PerFrameBuffer)];
    StructuredBuffer<TextureStreamingGPUInfo> textureStreamingMetadataBuffer =
        ResourceDescriptorHeap[textureTablePerFrame.textureImageTableDescriptorIndex];
    return textureStreamingMetadataBuffer[streamingTextureID];
}

// The descriptor a material texture samples: the image its streaming row publishes, else the material's own index.
uint ResolveMaterialTextureDescriptor(uint materialDescriptorIndex, uint streamingTextureID)
{
    if (streamingTextureID != 0u)
    {
        const TextureStreamingGPUInfo binding = LoadTextureStreamingInfo(streamingTextureID);
        if (binding.imageDescriptorIndex != 0xffffffffu)
            return binding.imageDescriptorIndex;
    }
    return materialDescriptorIndex;
}

// Material evaluation (VisUtilEvaluate.hlsl defines MATERIAL_EVAL_RESOLVED_TEXTURE_TABLE) reads the descriptor from the
// table ResolveMaterialTextureDescriptorsPass wrote this frame, by the material's row: a load that depends only on the
// material index, beside the material record, instead of the record's streaming ID, then its streaming row, then the
// sample. That chain was the most sampled line of the pass (Nsight, 2026-11-14). The resolve pass sets the row before
// sampling; MATERIAL_EVAL_RESOLVED_TEXTURE_TABLE 0 keeps the binding and takes the chain, for A/B.
#if defined(MATERIAL_EVAL_RESOLVED_TEXTURE_TABLE)
static uint g_materialEvalResolvedTextureRow = 0xFFFFFFFFu;
#endif

// slot is a MATERIAL_RESOLVED_TEXTURE_* literal at every call, so the component index folds.
uint ResolveMaterialEvalTextureDescriptor(uint slot, uint materialDescriptorIndex, uint streamingTextureID)
{
#if defined(MATERIAL_EVAL_RESOLVED_TEXTURE_TABLE)
    if (MATERIAL_EVAL_RESOLVED_TEXTURE_TABLE != 0)
    {
        StructuredBuffer<uint4> resolvedTextures =
            ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Material::ResolvedTextureDescriptors)];
        // The table covers every material row (materialTextureResolveConstants.h). The clamp, not a branch, only keeps a
        // frame recorded between the material tables growing and the graph's rebuild inside the table.
        uint entries;
        uint stride;
        resolvedTextures.GetDimensions(entries, stride);
        const uint row = min(g_materialEvalResolvedTextureRow, entries / MATERIAL_RESOLVED_TEXTURE_UINT4_PER_ROW - 1u);
        const uint4 group = resolvedTextures[row * MATERIAL_RESOLVED_TEXTURE_UINT4_PER_ROW + (slot >> 2u)];
        return group[slot & 3u];
    }
#endif
    return ResolveMaterialTextureDescriptor(materialDescriptorIndex, streamingTextureID);
}

#endif // MATERIAL_TEXTURE_BINDING_HLSLI
