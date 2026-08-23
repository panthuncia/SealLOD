#pragma once

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "ShaderBuffers.h"
#include "OpenRenderGraph/OpenRenderGraph.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Scene/Components.h"

class ShadowMaps;
class LinearShadowMaps;
class IndirectCommandBufferManager;
class ViewManager;
class SortedUnsignedIntBuffer;

struct AddLightReturn {
	Components::LightViewInfo lightViewInfo;
	std::optional<Components::FrustumPlanes> frustumPlanes;
};

class LightManager: public IResourceProvider {
public:
	static std::unique_ptr<LightManager> CreateUnique() {
		return std::unique_ptr<LightManager>(new LightManager());
	}
    ~LightManager();
    AddLightReturn AddLight(LightInfo* lightInfo, uint64_t entityId);
    void RemoveLight(LightInfo* light);
	void RemoveLight(flecs::entity light);
    unsigned int GetNumLights();
    void SetCurrentCamera(flecs::entity camera);
	void SetViewManager(ViewManager* viewManager);
	void UpdateLightBufferView(BufferView* view, const LightInfo& data);
    void UpdateLightViewInfo(flecs::entity light);
	unsigned int GetLightPagePoolSize() { return m_lightPagePoolSize; }
	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

private:
    LightManager();
	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::unordered_map<ResourceIdentifier, std::shared_ptr<IResourceResolver>, ResourceIdentifier::Hasher> m_resolvers;
	flecs::entity m_currentCamera;
    std::shared_ptr<LazyDynamicStructuredBuffer<LightInfo>> m_lightBuffer;
	std::shared_ptr<SortedUnsignedIntBuffer> m_activeLightIndices; // Sorted list of active light indices
    std::shared_ptr<DynamicStructuredBuffer<unsigned int>> m_spotViewInfo; // Indices into camera buffer
    std::shared_ptr<DynamicStructuredBuffer<unsigned int>> m_pointViewInfo;
    std::shared_ptr<DynamicStructuredBuffer<unsigned int>> m_directionalViewInfo;

	std::shared_ptr<ResourceGroup> m_pLightViewInfoResourceGroup;
	std::shared_ptr<ResourceGroup> m_pLightBufferResourceGroup;

	std::shared_ptr<Buffer> m_pClusterBuffer;
	std::shared_ptr<Buffer> m_pLightPagesBuffer;

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
	ViewManager* m_pViewManager = nullptr;
	unsigned int m_lightPagePoolSize = 0;

	std::mutex m_lightUpdateMutex;

    std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
        CreatePointLightViewInfo(const LightInfo& info, uint64_t entityId);
	std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
		CreateSpotLightViewInfo(const LightInfo& info, uint64_t entityId);
	std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
		CreateDirectionalLightViewInfo(const LightInfo& info, uint64_t entityId);
	void RebuildDirectionalLightViewInfoBuffer(std::optional<uint64_t> excludedLightEntityId = std::nullopt);

	void RemoveLightViewInfo(flecs::entity light);
};
