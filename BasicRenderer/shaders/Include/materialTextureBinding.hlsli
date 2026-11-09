#ifndef MATERIAL_TEXTURE_BINDING_HLSLI
#define MATERIAL_TEXTURE_BINDING_HLSLI

#include "include/structs.hlsli"
#include "include/cbuffers.hlsli"
#include "include/materialUvTransform.hlsli"

// A material texture is bound two ways. Its streaming row (the texture image table) names the image the streaming
// system last published; the material's own descriptor index is set when the material is built, and is a neutral
// fallback while the texture has no image (MaterialTextureFallbacks). Only an unstreamed texture (ID 0) is current
// through the material's index alone, so every sampler of a material texture resolves through the row first.

TextureStreamingGPUInfo LoadTextureStreamingInfo(uint streamingTextureID)
{
    StructuredBuffer<TextureStreamingGPUInfo> textureStreamingMetadataBuffer =
        ResourceDescriptorHeap[ResourceDescriptorIndex(Builtin::Material::TextureStreamingMetadataBuffer)];
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

#endif // MATERIAL_TEXTURE_BINDING_HLSLI
