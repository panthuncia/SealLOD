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
class MaterialManager;
class TextureStreamingManager;

inline constexpr float kDefaultTerrainLayerUvScale = 24.0f / 4096.0f;
inline constexpr float kDefaultTerrainRegionSizeWorld = 2048.0f;
inline constexpr float kDefaultTerrainStochasticScale = 3.4641016f;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_SNOW = 1u << 0;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_HEIGHT_FROM_DIFFUSE_ALPHA = 1u << 1;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_PBR = 1u << 2;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_GLINT = 1u << 3;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_GRASS_FAR_OVERLAY = 1u << 4;
inline constexpr std::uint32_t TERRAIN_STOCHASTIC_FLAG_DIFFUSE = 1u << 0;
inline constexpr std::uint32_t TERRAIN_STOCHASTIC_FLAG_NORMAL = 1u << 1;
inline constexpr std::uint32_t TERRAIN_STOCHASTIC_FLAG_DIFFUSE_COLOR_SPACE = 1u << 2;
inline constexpr std::uint32_t TERRAIN_STOCHASTIC_FLAG_HEIGHT = 1u << 3;

struct TerrainStochasticTextureDesc
{
    std::shared_ptr<TextureAsset> gaussian;
    std::shared_ptr<TextureAsset> inverseLut;
    std::uint32_t flags = 0u;
    std::uint32_t lutHeight = 0u;
    DirectX::XMFLOAT3 colorSpaceOrigin = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 colorSpaceVector0 = { 1.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 colorSpaceVector1 = { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3 colorSpaceVector2 = { 0.0f, 0.0f, 1.0f };
};

struct TerrainLayerStochasticDesc
{
    TerrainStochasticTextureDesc diffuse;
    TerrainStochasticTextureDesc normal;
    TerrainStochasticTextureDesc height;
    float scale = kDefaultTerrainStochasticScale;
};

struct TerrainLayerDesc
{
    std::shared_ptr<TextureAsset> diffuse;
    std::shared_ptr<TextureAsset> normal;
    std::shared_ptr<TextureAsset> height;
    std::shared_ptr<TextureAsset> rmaos;
    TerrainLayerStochasticDesc stochastic;
    DirectX::XMFLOAT4 fallbackColor = { 0.45f, 0.42f, 0.36f, 1.0f };
    float uvScale = kDefaultTerrainLayerUvScale;
    float roughnessScale = 1.0f;
    float specularLevel = 0.04f;
    DirectX::XMFLOAT4 glintParameters = { 1.5f, 0.0f, 0.015f, 2.0f };
    // x=start distance in cells, y=end distance in cells, z=max weight scale.
    DirectX::XMFLOAT4 farOverlayParams = { 0.0f, 0.0f, 1.0f, 0.0f };
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

    std::uint32_t SetActiveTerrain(const TerrainMaterialDesc& desc, TextureFactory* textureFactory, MaterialManager* materialManager = nullptr);
    // Advances the main-thread terrain activation boundary after streaming
    // binding callbacks have been drained for the frame.
    void ProcessPendingUpdates();
    void ClearActiveTerrain();

    std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
    std::vector<ResourceIdentifier> GetSupportedKeys() override;
    std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
    std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

private:
    TerrainManager();
    enum class TerrainTextureSlot : std::uint8_t {
        Diffuse,
        Normal,
        Height,
        RMAOS
    };
    void RefreshTerrainLayerTextureBinding(
        std::uint32_t layerIndex,
        TerrainTextureSlot slot,
        const std::shared_ptr<TextureAsset>& texture,
        std::uint64_t terrainGeneration,
        std::size_t initialDependencyIndex);
    void InvalidateAndScheduleTerrainSetActivation();

    std::shared_ptr<DynamicStructuredBuffer<TerrainSetGPU>> m_sets;
    std::shared_ptr<DynamicStructuredBuffer<TerrainLayerGPU>> m_layers;
    std::shared_ptr<DynamicStructuredBuffer<TerrainStochasticLayerGPU>> m_stochasticLayers;
    std::shared_ptr<DynamicStructuredBuffer<TerrainLayerRefGPU>> m_layerRefs;
    std::shared_ptr<DynamicStructuredBuffer<TerrainRegionGPU>> m_regions;
    // Four exact Skyrim UNORM8 paint weights are stored in each GPU word.
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> m_weightBlocks;
    std::shared_ptr<ResourceGroup> m_textureGroup;
    std::vector<std::shared_ptr<TextureAsset>> m_layerTextures;
    std::vector<TerrainLayerGPU> m_layerData;
    TerrainSetGPU m_desiredSet{};
    std::vector<std::uint8_t> m_initialBindingReady;
    std::size_t m_readyInitialBindingCount = 0;
    std::uint64_t m_terrainGeneration = 0;
    std::uint32_t m_activationDelayFrames = 0;
    bool m_terrainSetActive = false;
    bool m_pendingTerrainSetActivation = false;
    std::vector<std::uint64_t> m_streamingBindingIDs;
    TextureStreamingManager* m_textureStreamingManager = nullptr;
};
