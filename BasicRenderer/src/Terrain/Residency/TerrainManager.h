#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <DirectXMath.h>
#include "Interfaces/IResourceProvider.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include <BasicRenderer/Extensions/Resources/DynamicStructuredBuffer.h>
#include "Resources/ResourceGroup.h"
#include "BasicRenderer/Assets/Texture.h"
#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>

class TextureFactory;
class MaterialManager;
class TextureStreamingManager;
class PublishedStateResourceResolver;
namespace org::runtime { class IDescriptorService; class IUploadService; }

#include <BasicRenderer/Scene/TerrainTypes.h>

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
