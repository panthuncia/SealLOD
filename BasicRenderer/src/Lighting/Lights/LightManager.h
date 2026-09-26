#pragma once

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>
#include <cstddef>

#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include <BasicRenderer/Extensions/Resources/DynamicStructuredBuffer.h>
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"

class ShadowMaps;
class LinearShadowMaps;
class IndirectCommandBufferManager;
namespace br::render { class IShadowViewService; }
class SortedUnsignedIntBuffer;

struct AddLightReturn {
	Components::LightViewInfo lightViewInfo;
	std::optional<Components::FrustumPlanes> frustumPlanes;
};

class LightManager: public org::IResourceProvider {
public:
    std::vector<std::shared_ptr<const std::vector<std::byte>>> CaptureTableImages() const;
	static std::unique_ptr<LightManager> CreateUnique() {
		return std::unique_ptr<LightManager>(new LightManager());
	}
    ~LightManager();
    AddLightReturn AddLight(LightInfo* lightInfo, uint64_t entityId);
    void RemoveLight(LightInfo* light);
	void RemoveLight(flecs::entity light);
	unsigned int GetNumLights();
	uint64_t GetPublicationRevision() const noexcept { return m_publicationRevision.load(std::memory_order_acquire); }
    void SetCurrentCamera(flecs::entity camera);
	void SetShadowViewService(br::render::IShadowViewService* service);
	void UpdateLightBufferView(org::BufferView* view, const LightInfo& data);
    void UpdateLightViewInfo(flecs::entity light);
	unsigned int GetLightPagePoolSize() { return m_lightPagePoolSize; }
	std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
	std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
	std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override;

private:
    LightManager();
	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>, org::ResourceIdentifier::Hasher> m_resources;
	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::IResourceResolver>, org::ResourceIdentifier::Hasher> m_resolvers;
	flecs::entity m_currentCamera;
    std::shared_ptr<org::LazyDynamicStructuredBuffer<LightInfo>> m_lightBuffer;
	std::shared_ptr<SortedUnsignedIntBuffer> m_activeLightIndices; // Sorted list of active light indices
    std::shared_ptr<DynamicStructuredBuffer<unsigned int>> m_spotViewInfo; // Indices into camera buffer
    std::shared_ptr<DynamicStructuredBuffer<unsigned int>> m_pointViewInfo;
    std::shared_ptr<DynamicStructuredBuffer<unsigned int>> m_directionalViewInfo;

	std::shared_ptr<org::ResourceGroup> m_pLightViewInfoResourceGroup;
	std::shared_ptr<org::ResourceGroup> m_pLightBufferResourceGroup;

	std::shared_ptr<org::Buffer> m_pClusterBuffer;
	std::shared_ptr<org::Buffer> m_pLightPagesBuffer;

    // TODO: The buffer size and increment size are low for testing.
    unsigned int m_commandBufferSize = 1;
	bool m_resizeCommandBuffers = false;
	static constexpr unsigned int m_commandBufferIncrementSize = 1;

    // Settings funcs
	std::function<uint8_t()> getNumDirectionalLightCascades;
    std::function<uint16_t()> getShadowResolution;
	std::function<float()> getMaxShadowDistance;
	std::function<float()> getDirectionalShadowSceneExtent;
	std::function<float()> getDirectionalVirtualShadowSourceAngleDegrees;
    std::function<void(std::shared_ptr<void>)> markForDelete;
	br::render::IShadowViewService* m_shadowViews = nullptr;
	unsigned int m_lightPagePoolSize = 0;

	std::mutex m_lightUpdateMutex;
	std::atomic_uint64_t m_publicationRevision{1};

    std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
        CreatePointLightViewInfo(const LightInfo& info, uint64_t entityId);
	std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
		CreateSpotLightViewInfo(const LightInfo& info, uint64_t entityId);
	std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
		CreateDirectionalLightViewInfo(const LightInfo& info, uint64_t entityId);
	void RebuildDirectionalLightViewInfoBuffer(std::optional<uint64_t> excludedLightEntityId = std::nullopt);

	void RemoveLightViewInfo(flecs::entity light);
};
