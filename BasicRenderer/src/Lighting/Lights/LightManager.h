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
#include <array>
#include <map>
#include <set>

#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include "Resources/ResourceIdentifier.h"
#include "Resources/Resource.h"
#include "Resources/ResourceGroup.h"
#include "Resources/Buffers/Buffer.h"
#include "Interfaces/IResourceResolver.h"
#include "Interfaces/IResourceProvider.h"
#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include <BasicRenderer/Extensions/Resources/DynamicStructuredBuffer.h>
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include "BasicRenderer/Streaming/LightStateArtifacts.h"

namespace br::render { class IShadowViewService; }
class SortedUnsignedIntBuffer;

// The sun as shaders see it (PerFrameCB::sunLightIndex/sunLightSlot): the first shadow-casting directional light, else
// the first directional one, by light buffer index. Both are 0xFFFFFFFF without one.
struct SunLightSelection {
	uint32_t lightIndex = 0xFFFFFFFFu;
	uint32_t activeSlot = 0xFFFFFFFFu;
};

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
	SunLightSelection GetSunLight();
	uint64_t GetPublicationRevision() const noexcept { return m_publicationRevision.load(std::memory_order_acquire); }
    void SetCurrentCamera(flecs::entity camera);
	void SetShadowViewService(br::render::IShadowViewService* service);
	// Uploads a light's data, stamping its slot's generation over data.generation.
	void UpdateLightBufferView(org::BufferView* view, const LightInfo& data);
    void UpdateLightViewInfo(flecs::entity light);
	// Chooses which shadow-casting spot and point lights have virtual shadow map faces (LocalShadowSelection.h),
	// creating and destroying their face views. Call once a frame, before the light tables are captured.
	void UpdateLocalShadows();
	std::vector<br::render::PublishedLocalShadowLight> CaptureLocalShadows();
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
    std::shared_ptr<DynamicStructuredBuffer<unsigned int>> m_directionalViewInfo; // Clipmap views' camera buffer indices.

	std::shared_ptr<org::ResourceGroup> m_pLightBufferResourceGroup;

	// Per light buffer slot: bumped each time the slot is given to a light (LightInfo::generation).
	std::vector<uint32_t> m_slotGenerations;
	// Mirrors of what the GPU tables hold, for choosing the sun: active light buffer indices, and the directional
	// lights among them with whether each casts shadows. Guarded by m_lightUpdateMutex.
	std::set<uint32_t> m_activeIndices;
	std::map<uint32_t, bool> m_directionalLights;

	std::shared_ptr<org::Buffer> m_pClusterBuffer;
	std::shared_ptr<org::Buffer> m_pLightPagesBuffer;

    // Settings funcs
	std::function<uint8_t()> getNumDirectionalLightCascades;
    std::function<uint16_t()> getShadowResolution;
	std::function<float()> getMaxShadowDistance;
	std::function<float()> getDirectionalShadowSceneExtent;
	std::function<float()> getDirectionalVirtualShadowSourceAngleDegrees;
	std::function<uint32_t()> getLocalShadowMaxFaces;
	br::render::IShadowViewService* m_shadowViews = nullptr;
	// Spot and point lights with virtual shadow map faces, by light buffer index, and which faces are taken. Guarded
	// by m_lightUpdateMutex: UpdateLightBufferView stamps each light's first slice from them.
	struct LocalShadowAssignment {
		uint64_t entityId = 0;
		uint32_t firstFace = 0;
		uint32_t faceCount = 0;
		std::vector<uint64_t> viewIDs;
	};
	std::map<uint32_t, LocalShadowAssignment> m_localShadows;
	std::array<bool, CLodVirtualShadowMaxLocalFaceCount> m_localFaceUsed{};
	unsigned int m_lightPagePoolSize = 0;

	std::mutex m_lightUpdateMutex;
	std::atomic_uint64_t m_publicationRevision{1};

	std::pair<Components::LightViewInfo, std::optional<Components::FrustumPlanes>>
		CreateDirectionalLightViewInfo(const LightInfo& info, uint64_t entityId);
	void RebuildDirectionalLightViewInfoBuffer(std::optional<uint64_t> excludedLightEntityId = std::nullopt);

	void RemoveLightViewInfo(flecs::entity light);
	// Drops a light's faces: destroys their views and frees the faces. The caller re-uploads the light.
	void ReleaseLocalShadow(uint32_t lightIndex);
	void UpdateLocalShadowFaceViews(const LocalShadowAssignment& assignment, const LightInfo& light);
};
