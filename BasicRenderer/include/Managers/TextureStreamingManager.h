#pragma once

#include <chrono>
#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>

#include "Interfaces/IResourceProvider.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/Texture.h"

class TextureFactory;
class CopyPass;
class Buffer;

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
	~TextureStreamingManager();

	void Initialize(TextureFactory& textureFactory, uint32_t framesInFlight);
	void Shutdown();
	void EnqueueFrameTick(uint64_t frameIndex);
	void EnqueueTextureUploadAdvance(const std::shared_ptr<TextureAsset>& texture, const char* reason = "external");
	std::size_t DrainPendingBindingChanges();

	uint64_t RegisterTextureBinding(
		const std::shared_ptr<TextureAsset>& texture,
		TextureFactory& textureFactory,
		BindingChangedCallback onBindingChanged,
		std::string debugLabel = {},
		bool seedCurrentBinding = true);
	void UnregisterTextureBinding(uint64_t bindingID);
	void UnregisterTextureBindings(const std::vector<uint64_t>& bindingIDs);

	std::shared_ptr<CopyPass> CreateTextureStreamingFeedbackReadbackPass();
	MaterialTextureStreamingStats GetTextureStreamingStats(const std::vector<std::shared_ptr<Resource>>& activeTextureResources) const;

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

private:
	TextureStreamingManager();
	void WorkerMain();

	struct TextureBindingOwner {
		uint64_t bindingID = 0;
		uint32_t streamingTextureID = 0;
		std::weak_ptr<TextureAsset> texture;
		BindingChangedCallback onBindingChanged;
		std::string debugLabel;
	};
	struct WorkerCommand {
		enum class Kind : uint8_t { Register, Unregister, MarkDirty, FrameTick } kind = Kind::FrameTick;
		uint64_t bindingID = 0;
		uint64_t frameIndex = 0;
		std::shared_ptr<TextureAsset> texture;
		BindingChangedCallback callback;
		std::string debugLabel;
		bool seedCurrentBinding = true;
		bool needsUploadAdvance = false;
		std::string reason;
	};
	struct PendingBindingChange {
		uint32_t streamingTextureID = 0;
		uint64_t bindingRevision = 0;
		uint64_t streamingStateRevision = 0;
		std::chrono::steady_clock::time_point queuedAt{};
		std::shared_ptr<TextureAsset> texture;
		std::shared_ptr<PixelBuffer> previousImage;
		std::shared_ptr<PixelBuffer> newImage;
		TextureStreamingGPUInfo metadata{};
		std::vector<std::shared_ptr<PixelBuffer>> supersededImages;
		std::vector<std::pair<uint64_t, BindingChangedCallback>> callbacks;
	};
	void ApplyRegisterCommand(WorkerCommand&& command);
	void ApplyUnregisterCommand(uint64_t bindingID);
	void QueueBindingChanged(TextureAsset& texture, std::shared_ptr<PixelBuffer> previousImage);
	void QueueCommand(WorkerCommand&& command);
	void PollCompletedReadbackSlots(uint64_t& lastProcessedFence);
	void EnsureTextureUploadAdvanced(const std::shared_ptr<TextureAsset>& texture, TextureFactory& textureFactory);
	void FlushDirtyTextureMetadata(const std::shared_ptr<TextureAsset>& texture);
	void MarkTextureStreamingMetadataDirty(const std::shared_ptr<TextureAsset>& texture, bool needsUploadAdvance = false, const char* reason = "unknown");
	void ProcessPendingTextureUpdates(uint64_t frameIndex, TextureFactory& textureFactory);

	void BeginTextureStreamingFeedbackFrame(uint64_t frameIndex);
	bool UpdateTextureStreamingMetadata(const std::shared_ptr<TextureAsset>& texture);
	void NotifyBindingChanged(TextureAsset& texture);
	void TrackTexture(const std::shared_ptr<TextureAsset>& texture);
	void RecordTextureDirtyReason(const char* reason);
	MaterialTextureStreamingStats BuildTextureStreamingStats() const;

	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::shared_ptr<DynamicStructuredBuffer<TextureStreamingGPUInfo>> m_textureStreamingMetadataBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_textureStreamingFeedbackBuffer;
	// Orders worker metadata publication against main-thread adoption writes.  The
	// buffer has its own call-level lock, but that alone cannot prevent an older
	// adoption snapshot from overwriting a newer worker revision.
	std::mutex m_gpuMetadataPublicationMutex;
	std::unordered_map<uint64_t, std::weak_ptr<TextureAsset>> m_textureAssetsByImageResourceID;
	std::unordered_map<uint32_t, std::weak_ptr<TextureAsset>> m_streamingTexturesByID;
	std::unordered_map<uint32_t, uint64_t> m_textureStreamingMetadataRevisions;
	std::vector<uint32_t> m_activeTextureStreamingFeedbackIDs;
	std::unordered_set<uint32_t> m_activeTextureStreamingFeedbackIDSet;
	mutable std::mutex m_activeFeedbackMutex;
	std::mutex m_textureStreamingFeedbackMutex;
	std::vector<std::pair<uint32_t, uint32_t>> m_pendingTextureStreamingFeedback;
	std::vector<uint32_t> m_dirtyTextureStreamingIDs;
	std::unordered_set<uint32_t> m_dirtyTextureStreamingIDSet;
	std::vector<uint32_t> m_texturesNeedingUploadAdvance;
	std::unordered_set<uint32_t> m_texturesNeedingUploadAdvanceSet;
	std::unordered_map<uint64_t, TextureBindingOwner> m_bindingsByID;
	std::unordered_map<uint32_t, std::vector<uint64_t>> m_bindingIDsByStreamingTextureID;
	std::atomic<uint64_t> m_nextBindingID{1u};
	TextureFactory* m_textureFactory = nullptr;
	std::thread m_workerThread;
	std::mutex m_workerCommandMutex;
	std::condition_variable m_workerCV;
	std::deque<WorkerCommand> m_workerCommands;
	std::atomic<bool> m_workerQuit{false};
	std::atomic<bool> m_initialized{false};
	struct ReadbackSlot {
		std::shared_ptr<Buffer> staging;
		std::vector<uint32_t> activeStreamingTextureIDs;
		uint64_t capacityBytes = 0;
		uint64_t copyBytes = 0;
		uint64_t fenceValue = 0;
		bool inFlight = false;
	};
	rhi::TimelinePtr m_readbackFencePtr;
	rhi::Timeline m_readbackFence;
	std::atomic<uint64_t> m_readbackFenceCounter{0};
	std::mutex m_readbackSlotMutex;
	std::vector<ReadbackSlot> m_readbackSlots;
	uint32_t m_readbackSlotCursor = 0;
	std::mutex m_pendingBindingChangeMutex;
	std::vector<PendingBindingChange> m_pendingBindingChanges;
	std::mutex m_liveBindingMutex;
	std::unordered_set<uint64_t> m_liveBindingIDs;
	mutable std::mutex m_statsMutex;
	MaterialTextureStreamingStats m_publishedStats;
	std::chrono::steady_clock::time_point m_lastTextureUpdateStatsLog = {};
	uint64_t m_textureDirtyReasonFeedback = 0;
	uint64_t m_textureDirtyReasonIdleCoarsen = 0;
	uint64_t m_textureDirtyReasonTrackBinding = 0;
	uint64_t m_textureDirtyReasonUploadStateRevision = 0;
	uint64_t m_textureDirtyReasonUploadPending = 0;
	uint64_t m_textureDirtyReasonOther = 0;
	std::atomic<uint64_t> m_textureBindingRefreshCount{0};
	uint64_t m_textureBindingChangedWithoutOwnerCount = 0;
};
