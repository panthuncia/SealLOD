#pragma once

#include <chrono>
#include <cstddef>
#include <atomic>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Interfaces/IResourceProvider.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/Texture.h"
#include "Managers/Singletons/TaskSchedulerManager.h"

class TextureFactory;
namespace org { class CopyPass; }
using org::CopyPass;
namespace org { class Buffer; }
using org::Buffer;
class MaterialTextureTransferService;

namespace org::runtime {
class IReadbackService;
}

struct MaterialTextureStreamingRecord {
	std::string identifier;
	uint64_t residentBytes = 0;
	uint32_t residentWidth = 0;
	uint32_t residentHeight = 0;
	uint32_t expectedResidentWidth = 0;
	uint32_t expectedResidentHeight = 0;
	uint32_t totalMipCount = 0;
	uint32_t residentTopMip = 0;
	uint32_t residentMipCount = 0;
	uint32_t requestedTopMip = 0;
	uint32_t feedbackTopMip = UINT32_MAX;
	bool eligible = false;
	bool enabled = false;
	bool alphaTested = false;
};

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
	std::vector<uint32_t> requestedTopMipHistogram = {};
	std::vector<uint32_t> feedbackTopMipHistogram = {};
	uint32_t texturesWithoutFeedback = 0;
	std::vector<uint64_t> residentBytesByTopMip = {};
	uint32_t residentShapeMismatchTextureCount = 0;
	uint64_t residentShapeMismatchBytes = 0;
	uint32_t distinctPreparedTextureCount = 0;
	uint64_t distinctPreparedTextureBytes = 0;
	uint32_t activeMaterialResourceCount = 0;
	uint64_t activeMaterialResourceBytes = 0;
	uint32_t externallyManagedActiveResourceCount = 0;
	uint64_t externallyManagedActiveResourceBytes = 0;
	uint32_t graphManagedParticipatingActiveResourceCount = 0;
	uint64_t graphManagedParticipatingActiveResourceBytes = 0;
	uint32_t alphaTestedTextureCount = 0;
	uint32_t alphaTestedMipCapViolationCount = 0;
	uint32_t idleCoarseningDisabledTextureCount = 0;
	uint32_t residencyConstrainedTextureCount = 0;
	uint32_t residencyConstraintViolationCount = 0;
	std::vector<uint64_t> publishedResourceIDs = {};
	std::vector<uint64_t> participatingPublishedResourceIDs = {};
	std::vector<MaterialTextureStreamingRecord> largestResidentTextures = {};
};

struct TextureStreamingBindingOptions {
	bool seedCurrentBinding = true;
	bool alphaTested = false;
	bool allowIdleCoarsening = true;
	uint32_t maximumResidentTopMip = (std::numeric_limits<uint32_t>::max)();
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
	void RetirePatchedBindingResources();
	bool RequestExternalMaterialTextureReadback(
		const std::shared_ptr<PixelBuffer>& image,
		std::wstring outputFile,
		std::function<void()> callback);

	uint64_t RegisterTextureBinding(
		const std::shared_ptr<TextureAsset>& texture,
		BindingChangedCallback onBindingChanged,
		std::string debugLabel = {},
		TextureStreamingBindingOptions options = {});
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
	void ScheduleDrain();
	void Drain(const br::TaskContext& context);

	struct TextureBindingOwner {
		uint64_t bindingID = 0;
		uint32_t streamingTextureID = 0;
		std::weak_ptr<TextureAsset> texture;
		std::string debugLabel;
		TextureStreamingBindingOptions options{};
	};
	struct WorkerCommand {
		enum class Kind : uint8_t { Register, Unregister, MarkDirty, FrameTick } kind = Kind::FrameTick;
		uint64_t bindingID = 0;
		uint64_t frameIndex = 0;
		std::shared_ptr<TextureAsset> texture;
		std::string debugLabel;
		TextureStreamingBindingOptions options{};
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
	};
	struct MainThreadBindingOwner {
		uint32_t streamingTextureID = 0;
		std::weak_ptr<TextureAsset> texture;
		BindingChangedCallback callback;
		uint64_t appliedBindingRevision = 0;
		uint64_t appliedImageResourceID = 0;
	};
	void ApplyRegisterCommand(WorkerCommand&& command);
	void ApplyUnregisterCommand(uint64_t bindingID);
	void QueueBindingChanged(TextureAsset& texture, std::shared_ptr<PixelBuffer> previousImage);
	void QueueCommand(WorkerCommand&& command);
	void MarkLiveTextureBindingsDirty(uint32_t streamingTextureID);
	std::size_t RefreshDirtyLiveBindings();
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
	std::unordered_map<uint32_t, uint32_t> m_alphaTestedBindingCountsByStreamingTextureID;
	std::unordered_map<uint32_t, uint32_t> m_idleCoarseningDisabledBindingCountsByStreamingTextureID;
	std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> m_maximumResidentTopMipBindingCountsByStreamingTextureID;
	std::atomic<uint64_t> m_nextBindingID{1u};
	TextureFactory* m_textureFactory = nullptr;
	std::unique_ptr<MaterialTextureTransferService> m_materialTextureTransfers;
	TaskScope m_taskScope;
	std::mutex m_workerCommandMutex;
	std::deque<WorkerCommand> m_workerCommands;
	std::atomic<bool> m_drainScheduled{false};
	std::atomic<bool> m_workerQuit{false};
	uint64_t m_lastProcessedReadbackFence = 0;
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
	std::vector<std::shared_ptr<PixelBuffer>> m_imagesPendingOwnerPatchRetirement;
	std::mutex m_liveBindingMutex;
	std::unordered_map<uint64_t, MainThreadBindingOwner> m_liveBindingsByID;
	std::unordered_map<uint32_t, std::vector<uint64_t>> m_liveBindingIDsByStreamingTextureID;
	std::vector<uint64_t> m_dirtyLiveBindingIDs;
	std::unordered_set<uint64_t> m_dirtyLiveBindingIDSet;
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
