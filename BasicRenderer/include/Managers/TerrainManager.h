#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Interfaces/IResourceProvider.h"
#include "ShaderBuffers.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/ResourceGroup.h"
#include "Resources/Texture.h"

class TextureFactory;

inline constexpr float kDefaultTerrainLayerUvScale = 24.0f / 4096.0f;
inline constexpr float kDefaultTerrainRegionSizeWorld = 2048.0f;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_SNOW = 1u << 0;

struct TerrainLayerDesc
{
    std::shared_ptr<TextureAsset> diffuse;
    std::shared_ptr<TextureAsset> normal;
    DirectX::XMFLOAT4 fallbackColor = { 0.45f, 0.42f, 0.36f, 1.0f };
    float uvScale = kDefaultTerrainLayerUvScale;
    // Close landscape layer flags copied from Skyrim LTEX metadata. Distant land LOD overlays are not terrain layers.
    std::uint32_t flags = 0u;
};

struct TerrainLayerRefDesc
{
    std::uint32_t layerIndex = 0;
};

struct TerrainRegionDesc
{
    std::int32_t regionX = 0;
    std::int32_t regionY = 0;
    std::uint32_t layerRefStart = 0;
    std::uint32_t layerRefCount = 0;
    std::uint32_t weightBlockStart = 0;
    std::uint32_t weightSampleSide = 19;
};

struct TerrainMaterialDesc
{
    std::vector<TerrainLayerDesc> layers;
    std::vector<TerrainLayerRefDesc> layerRefs;
    std::vector<TerrainRegionDesc> regions;
    std::vector<std::uint32_t> weightBlocks;
    float regionSizeWorld = kDefaultTerrainRegionSizeWorld;
};

class TerrainManager : public IResourceProvider
{
public:
    static std::unique_ptr<TerrainManager> CreateUnique();

    std::uint32_t SetActiveTerrain(const TerrainMaterialDesc& desc, TextureFactory* textureFactory);
    void ClearActiveTerrain();

    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
    std::vector<ResourceIdentifier> GetSupportedKeys() override;
    std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
    std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

private:
    TerrainManager();

    std::shared_ptr<DynamicStructuredBuffer<TerrainSetGPU>> m_sets;
    std::shared_ptr<DynamicStructuredBuffer<TerrainLayerGPU>> m_layers;
    std::shared_ptr<DynamicStructuredBuffer<TerrainLayerRefGPU>> m_layerRefs;
    std::shared_ptr<DynamicStructuredBuffer<TerrainRegionGPU>> m_regions;
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> m_weightBlocks;
    std::shared_ptr<ResourceGroup> m_textureGroup;
    std::vector<std::shared_ptr<TextureAsset>> m_layerTextures;
};
