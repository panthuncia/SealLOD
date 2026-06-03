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
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_SNOW = 1u << 0;
inline constexpr std::uint32_t kTerrainMaxBlendLayers = 12;

struct TerrainLayerDesc
{
    std::shared_ptr<TextureAsset> diffuse;
    std::shared_ptr<TextureAsset> normal;
    DirectX::XMFLOAT4 fallbackColor = { 0.45f, 0.42f, 0.36f, 1.0f };
    float uvScale = kDefaultTerrainLayerUvScale;
    // Close landscape layer flags copied from Skyrim LTEX metadata. Distant land LOD overlays are not terrain layers.
    std::uint32_t flags = 0u;
};

struct TerrainQuadrantDesc
{
    std::int32_t cellX = 0;
    std::int32_t cellY = 0;
    std::uint32_t quadrant = 0;
    std::array<std::uint32_t, kTerrainMaxBlendLayers> layerIndices = {};
    std::uint32_t weightAtlasX = 0;
    std::uint32_t weightAtlasY = 0;
    std::uint32_t weightAtlasStride = 19;
};

struct TerrainMaterialDesc
{
    std::vector<TerrainLayerDesc> layers;
    std::vector<TerrainQuadrantDesc> quadrants;
    std::vector<std::uint8_t> weights0Rgba8;
    std::vector<std::uint8_t> weights1Rgba8;
    std::vector<std::uint8_t> weights2Rgba8;
    std::uint32_t weightAtlasWidth = 0;
    std::uint32_t weightAtlasHeight = 0;
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

    static std::shared_ptr<TextureAsset> CreateWeightAtlasTexture(
        const char* name,
        std::uint32_t width,
        std::uint32_t height,
        const std::vector<std::uint8_t>& rgba8,
        TextureFactory* textureFactory);

    std::shared_ptr<DynamicStructuredBuffer<TerrainSetGPU>> m_sets;
    std::shared_ptr<DynamicStructuredBuffer<TerrainLayerGPU>> m_layers;
    std::shared_ptr<DynamicStructuredBuffer<TerrainQuadrantGPU>> m_quadrants;
    std::shared_ptr<ResourceGroup> m_textureGroup;
    std::shared_ptr<TextureAsset> m_weightAtlas0;
    std::shared_ptr<TextureAsset> m_weightAtlas1;
    std::shared_ptr<TextureAsset> m_weightAtlas2;
    std::vector<std::shared_ptr<TextureAsset>> m_layerTextures;
};
