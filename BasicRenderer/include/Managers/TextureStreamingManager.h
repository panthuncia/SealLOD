#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Interfaces/IResourceProvider.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/Texture.h"

class TextureFactory;

namespace rg::runtime {
class IReadbackService;
}

struct MaterialTextureStreamingStats {
	uint32_t uniqueMaterialTextureCount = 0;
	uint32_t uniqueStreamableTextureCount = 0;
	uint32_t uniqueStreamingEnabledTextureCount = 0;
	uint32_t fullResolutionResidentTextureCount = 0;
	uint32_t streamableFullResolutionResidentTextureCount = 0;
	uint32_t pendingReloadTextureCount = 0;
	uint64_t totalResidentBytes = 0;
	uint64_t streamableResidentBytes = 0;
	std::vector<uint32_t> residentTopMipHistogram = {};
};

class TextureStreamingManager : public IResourceProvider {
public:
	using BindingChangedCallback = std::function<void(TextureAsset&)>;

	static std::unique_ptr<TextureStreamingManager> CreateUnique() {
		return std::unique_ptr<TextureStreamingManager>(new TextureStreamingManager());
	}

	uint64_t RegisterTextureBinding(
		const std::shared_ptr<TextureAsset>& texture,
		TextureFactory& textureFactory,
		BindingChangedCallback onBindingChanged,
		std::string debugLabel = {});
	void UnregisterTextureBinding(uint64_t bindingID);
	void UnregisterTextureBindings(const std::vector<uint64_t>& bindingIDs);

	void EnsureTextureUploadAdvanced(const std::shared_ptr<TextureAsset>& texture, TextureFactory& textureFactory);
	void FlushDirtyTextureMetadata(const std::shared_ptr<TextureAsset>& texture);
	void MarkTextureStreamingMetadataDirty(const std::shared_ptr<TextureAsset>& texture, bool needsUploadAdvance = false, const char* reason = "unknown");
	void ProcessPendingTextureUpdates(uint64_t frameIndex, TextureFactory& textureFactory);
	void RequestTextureStreamingFeedbackReadback(rg::runtime::IReadbackService* readbackService);
	MaterialTextureStreamingStats GetTextureStreamingStats(const std::vector<std::shared_ptr<Resource>>& activeTextureResources) const;

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

private:
	TextureStreamingManager();

	struct TextureBindingOwner {
		uint64_t bindingID = 0;
		uint32_t streamingTextureID = 0;
		std::weak_ptr<TextureAsset> texture;
		BindingChangedCallback onBindingChanged;
		std::string debugLabel;
	};

	void BeginTextureStreamingFeedbackFrame(uint64_t frameIndex);
	bool UpdateTextureStreamingMetadata(const std::shared_ptr<TextureAsset>& texture);
	void NotifyBindingChanged(TextureAsset& texture);
	void TrackTexture(const std::shared_ptr<TextureAsset>& texture);
	void RecordTextureDirtyReason(const char* reason);

	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::shared_ptr<DynamicStructuredBuffer<TextureStreamingGPUInfo>> m_textureStreamingMetadataBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_textureStreamingFeedbackBuffer;
	std::unordered_map<uint64_t, std::weak_ptr<TextureAsset>> m_textureAssetsByImageResourceID;
	std::unordered_map<uint32_t, std::weak_ptr<TextureAsset>> m_streamingTexturesByID;
	std::unordered_map<uint32_t, uint64_t> m_textureStreamingMetadataRevisions;
	std::vector<uint32_t> m_activeTextureStreamingFeedbackIDs;
	std::unordered_set<uint32_t> m_activeTextureStreamingFeedbackIDSet;
	std::mutex m_textureStreamingFeedbackMutex;
	std::vector<std::pair<uint32_t, uint32_t>> m_pendingTextureStreamingFeedback;
	std::vector<uint32_t> m_dirtyTextureStreamingIDs;
	std::unordered_set<uint32_t> m_dirtyTextureStreamingIDSet;
	std::vector<uint32_t> m_texturesNeedingUploadAdvance;
	std::unordered_set<uint32_t> m_texturesNeedingUploadAdvanceSet;
	std::unordered_map<uint64_t, TextureBindingOwner> m_bindingsByID;
	std::unordered_map<uint32_t, std::vector<uint64_t>> m_bindingIDsByStreamingTextureID;
	uint64_t m_nextBindingID = 1u;
	std::chrono::steady_clock::time_point m_lastTextureUpdateStatsLog = {};
	uint64_t m_textureDirtyReasonFeedback = 0;
	uint64_t m_textureDirtyReasonIdleCoarsen = 0;
	uint64_t m_textureDirtyReasonTrackBinding = 0;
	uint64_t m_textureDirtyReasonUploadStateRevision = 0;
	uint64_t m_textureDirtyReasonUploadPending = 0;
	uint64_t m_textureDirtyReasonOther = 0;
	uint64_t m_textureBindingRefreshCount = 0;
	uint64_t m_textureBindingChangedWithoutOwnerCount = 0;
};
