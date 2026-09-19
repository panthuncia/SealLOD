#pragma once

#include <array>
#include <atomic>
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
#include "Render/AsyncStateGraph.h"
#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"

class TextureFactory;
class MaterialManager;
class TextureStreamingManager;
class PublishedStateResourceResolver;
namespace org::runtime { class IDescriptorService; class IUploadService; }

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

class TerrainManager : public org::IResourceProvider
{
public:
    static std::unique_ptr<TerrainManager> CreateUnique();

    std::uint32_t SetActiveTerrain(const TerrainMaterialDesc& desc, TextureFactory* textureFactory, MaterialManager* materialManager = nullptr);
    // Reconciles level-triggered graph binding observations and advances the
    // terrain publication boundary.
    void ProcessPendingUpdates();
    void ClearActiveTerrain();
	void SetRendererStateServices(
		void* requests,
		std::shared_ptr<org::runtime::IUploadService> uploads,
		std::shared_ptr<org::runtime::IDescriptorService> descriptors) noexcept {
		m_rendererStateRequests = requests;
		m_uploadService = std::move(uploads);
		m_descriptorService = std::move(descriptors);
	}
	bool TryActivatePublishedTerrainState(
		const std::shared_ptr<const br::render::PublishedRendererState>& published);

    std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
    std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
    std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override;
    std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override;

private:
    TerrainManager();
    enum class TerrainTextureSlot : std::uint8_t {
        Diffuse,
        Normal,
        Height,
        RMAOS
    };
    struct GraphTextureBinding {
        std::uint32_t layerIndex = 0;
        TerrainTextureSlot slot = TerrainTextureSlot::Diffuse;
        std::shared_ptr<TextureAsset> texture;
        br::render::ArtifactAddress address{};
    };
	void RequestGraphState();

    std::shared_ptr<DynamicStructuredBuffer<TerrainSetGPU>> m_sets;
    std::shared_ptr<DynamicStructuredBuffer<TerrainLayerGPU>> m_layers;
    std::shared_ptr<DynamicStructuredBuffer<TerrainStochasticLayerGPU>> m_stochasticLayers;
    std::shared_ptr<DynamicStructuredBuffer<TerrainLayerRefGPU>> m_layerRefs;
    std::shared_ptr<DynamicStructuredBuffer<TerrainRegionGPU>> m_regions;
    // Four exact Skyrim UNORM8 paint weights are stored in each GPU word.
    std::shared_ptr<DynamicStructuredBuffer<std::uint32_t>> m_weightBlocks;
    std::shared_ptr<org::ResourceGroup> m_textureGroup;
    std::vector<std::shared_ptr<TextureAsset>> m_layerTextures;
    std::vector<TerrainLayerGPU> m_layerData;
	std::vector<TerrainStochasticLayerGPU> m_stochasticLayerData;
	std::vector<TerrainLayerRefGPU> m_layerRefData;
	std::vector<TerrainRegionGPU> m_regionData;
	std::vector<std::uint32_t> m_weightBlockData;
    TerrainSetGPU m_desiredSet{};
    std::uint64_t m_terrainGeneration = 0;
    std::vector<std::uint64_t> m_streamingBindingIDs;
    std::vector<GraphTextureBinding> m_graphTextureBindings;
    TextureStreamingManager* m_textureStreamingManager = nullptr;
	void* m_rendererStateRequests = nullptr;
	std::shared_ptr<org::runtime::IUploadService> m_uploadService;
	std::shared_ptr<org::runtime::IDescriptorService> m_descriptorService;
	std::array<std::shared_ptr<PublishedStateResourceResolver>, 7> m_terrainResolvers;
	std::array<std::shared_ptr<br::render::VersionedBufferFamily>, 6> m_bufferFamilies;
	std::uint64_t m_terrainRowsRevision = 0;
	std::uint64_t m_terrainStateRevision = 0;
	std::uint64_t m_activeTerrainPublishedRevision = 0;
	std::uint32_t m_terrainGraphStableFrames = 0;
	bool m_terrainGraphDirty = false;
	bool m_terrainGraphRequestPending = false;
	bool m_terrainGraphActive = false;
	// 0 = building, 1 = built, 2 = failed/cancelled; written by the awaiter's
	// continuation on a worker, read on the owner thread.
	std::shared_ptr<std::atomic<int>> m_terrainGraphOutcome;
	std::shared_ptr<br::render::ArtifactAwaiter> m_terrainGraphAwaiter;
};
