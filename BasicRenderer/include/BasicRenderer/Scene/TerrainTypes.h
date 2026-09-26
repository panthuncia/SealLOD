#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <DirectXMath.h>

class TextureAsset;

inline constexpr float kDefaultTerrainLayerUvScale = 24.0f / 4096.0f;
inline constexpr float kDefaultTerrainRegionSizeWorld = 2048.0f;
inline constexpr float kDefaultTerrainStochasticScale = 3.4641016f;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_SNOW = 1u << 0;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_HEIGHT_FROM_DIFFUSE_ALPHA = 1u << 1;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_PBR = 1u << 2;
inline constexpr std::uint32_t TERRAIN_LAYER_FLAG_GLINT = 1u << 3;
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

