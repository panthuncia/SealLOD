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

#include <tbb/concurrent_queue.h>

#include "Interfaces/IResourceProvider.h"
#include "Render/AsyncStateGraph.h"
#include "Render/TextureImageTableArtifacts.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Resources/Texture.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/SerializedTaskPump.h"

class TextureFactory;
namespace br::render {
class RendererStateRequestService;
struct TextureTransferArtifact;
}
namespace org { class CopyPass; }
namespace org { class Buffer; }
class MaterialTextureTransferService;
class PublishedStateResourceResolver;

namespace org::runtime {
class IReadbackService;
class IUploadService;
class IDescriptorService;
}

struct MaterialTextureStreamingRecord {
	std::string identifier;
	uint32_t streamingTextureID = 0;
	uint32_t imageDescriptorIndex = UINT32_MAX;
	uint64_t imageResourceID = 0;
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

struct MaterialTextureStreamingReadinessStats {
	uint32_t fullResolutionResidentTextureCount = 0;
	uint32_t pendingReloadTextureCount = 0;
};

struct TextureStreamingBindingOptions {
	bool seedCurrentBinding = true;
	// Exact graph versions are reserved for consumers which embed a concrete
	// descriptor in an immutable transaction (terrain and pinned resources).
	// Materials resolve stable IDs through the mutable image table instead.
	bool requiresExactGraphPublication = true;
	bool alphaTested = false;
	bool allowIdleCoarsening = true;
	uint32_t maximumResidentTopMip = (std::numeric_limits<uint32_t>::max)();
};

class TextureStreamingManager : public org::IResourceProvider {
public:
	using BindingChangedCallback = std::function<void(TextureAsset&)>;

	static std::unique_ptr<TextureStreamingManager> CreateUnique() {
		return std::unique_ptr<TextureStreamingManager>(new TextureStreamingManager());
	}
	~TextureStreamingManager();

	void Initialize(TextureFactory& textureFactory, uint32_t framesInFlight);
	void SetRendererStateRequestService(br::render::RendererStateRequestService* service,
		std::shared_ptr<org::runtime::IUploadService> uploads = {});
	void SetDescriptorService(std::shared_ptr<org::runtime::IDescriptorService> descriptors) {
		m_descriptorService = std::move(descriptors);
	}
	void Shutdown();
	void EnqueueFrameTick(uint64_t frameIndex);
	void EnqueueTextureUploadAdvance(const std::shared_ptr<TextureAsset>& texture, const char* reason = "external");
	// Binding adoption and image-table publication are owned by the streaming
	// worker and the state graph; the renderer thread only observes the
	// published texture-image fragment through its lease.
	void AcknowledgePublishedImageTable(
		const std::shared_ptr<const br::render::PublishedRendererState>& published);
	std::shared_ptr<org::Resource> ResolvePublishedImageTableResourceForDiagnostics() const;
	std::shared_ptr<org::Resource> PublishedImageTableReadbackAnchorForDiagnostics() const;
	bool RequestExternalMaterialTextureReadback(
		const std::shared_ptr<org::PixelBuffer>& image,
		std::wstring outputFile,
		std::function<void()> callback);
	uint64_t RegisterTextureBinding(
		const std::shared_ptr<TextureAsset>& texture,
		BindingChangedCallback onBindingChanged,
		std::string debugLabel = {},
		TextureStreamingBindingOptions options = {});
	void UnregisterTextureBinding(uint64_t bindingID);
	void UnregisterTextureBindings(const std::vector<uint64_t>& bindingIDs);

	std::shared_ptr<org::RenderPass> CreateTextureStreamingFeedbackReadbackPass();
	MaterialTextureStreamingStats GetTextureStreamingStats(const std::vector<std::shared_ptr<org::Resource>>& activeTextureResources,
		uint64_t* sequence = nullptr) const;
	// Advances whenever the streaming worker publishes new stats; callers cache on it.
	uint64_t PublishedStatsSequence() const noexcept;
	MaterialTextureStreamingReadinessStats GetTextureStreamingReadinessStats() const;

	std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
	std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
	std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override;

private:
	TextureStreamingManager();
	void ScheduleDrain();
	void Drain();

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
		std::shared_ptr<org::PixelBuffer> previousImage;
		std::shared_ptr<org::PixelBuffer> newImage;
		TextureStreamingGPUInfo metadata{};
		std::shared_ptr<const br::render::TextureTransferArtifact> transfer;
		bool graphRequested = false;
		bool graphReady = false;
		br::render::ArtifactVersionID graphVersion{};
	};
	void ApplyRegisterCommand(WorkerCommand&& command);
	void ApplyUnregisterCommand(uint64_t bindingID);
	void QueueBindingChanged(TextureAsset& texture, std::shared_ptr<org::PixelBuffer> previousImage);
	void FinishBindingMailboxRequest(uint32_t streamingTextureID, const std::shared_ptr<TextureAsset>& texture);
	void QueueCommand(WorkerCommand&& command);
	void EnqueueTextureMetadataRefresh(const std::shared_ptr<TextureAsset>& texture, const char* reason);
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
	void QueueTextureImageTableMetadata(const std::shared_ptr<TextureAsset>& texture);
	void FlushPendingTextureImageTableMetadata();
	void AppendTextureDisplayGates(const TextureAsset& texture,
		const TextureAsset::PublishedBindingSnapshot& published,
		std::vector<br::render::ArtifactIntent>& intents);
	void ReleaseTextureDisplayGates(std::vector<br::render::ArtifactIntent> gates);
	void PublishTextureImageTable();
	MaterialTextureStreamingStats BuildTextureStreamingStats() const;

	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>, org::ResourceIdentifier::Hasher> m_resources;
	std::shared_ptr<DynamicStructuredBuffer<TextureStreamingGPUInfo>> m_textureStreamingMetadataBuffer;
	std::shared_ptr<PublishedStateResourceResolver> m_textureImageTableResolver;
	br::render::VersionedGpuBufferJournal m_textureImageTableJournal{ sizeof(TextureStreamingGPUInfo) };
	std::shared_ptr<br::render::VersionedBufferFamily> m_textureImageTableFamily;
	std::vector<std::shared_ptr<const br::render::TextureImageHoldChunk>> m_textureImageHoldChunks;
	std::mutex m_pendingTextureImageTableMetadataMutex;
	std::vector<std::weak_ptr<TextureAsset>> m_pendingTextureImageTableMetadata;
	std::unordered_set<std::uint32_t> m_pendingTextureImageTableMetadataIDs;
	std::uint64_t m_textureImageTableEpoch = 0;
	// Display gates already requested per streaming texture, one bit per
	// br::render::TextureDisplayQuality. Worker drain only.
	static constexpr std::uint8_t kAllTextureDisplayGates = 0b11u;
	std::unordered_map<std::uint32_t, std::uint8_t> m_textureDisplayGatesRequested;
	std::uint64_t m_textureDisplayGateRevision = 0;
	// Gates satisfied by image-table rows up to tableEpoch, waiting for a ready
	// root that contains those rows.
	struct PendingTextureDisplayGate {
		std::uint64_t tableEpoch = 0;
		br::render::ArtifactIntent intent;
	};
	std::mutex m_pendingTextureDisplayGatesMutex;
	std::vector<PendingTextureDisplayGate> m_pendingTextureDisplayGates;
	std::uint64_t m_textureImageTableLogicalExtent = 1;
	bool m_textureImageTableDirty = false;
	br::render::ArtifactVersionHandle m_textureImageTableHandle;
	// Set while the newest submitted table root has not reached UploadSubmitted;
	// cleared by that root's awaiter so the worker never polls the graph.
	std::atomic<bool> m_textureImageTableBuildInFlight{ false };
	std::mutex m_textureImageTableAwaiterMutex;
	br::render::ArtifactAwaiter m_textureImageTableAwaiter;
	std::uint32_t m_framesInFlight = 1;
	std::uint64_t m_lastTextureImageTableAdmissionRetirementEpoch = 0;
	std::atomic<std::uint64_t> m_textureImageTableAcknowledgedEpoch{ 0 };
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_textureStreamingFeedbackBuffer;
	// The image-table journal, hold chunks and bootstrap metadata buffer have a
	// single writer: the serialized worker drain (m_commandPump).
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
	br::render::RendererStateRequestService* m_rendererStateRequests = nullptr;
	std::shared_ptr<org::runtime::IUploadService> m_uploadService;
	std::shared_ptr<org::runtime::IDescriptorService> m_descriptorService;
	std::mutex m_graphBindingAwaiterMutex;
	std::unordered_map<uint32_t, br::render::ArtifactAwaiter> m_graphBindingAwaiters;
	std::mutex m_bindingMailboxMutex;
	std::unordered_set<uint32_t> m_activeBindingMailboxRequests;
	std::unordered_set<uint32_t> m_dirtyBindingMailboxes;
	std::unique_ptr<MaterialTextureTransferService> m_materialTextureTransfers;
	TaskScope m_taskScope;
	std::mutex m_workerCommandMutex;
	std::deque<WorkerCommand> m_workerCommands;
	br::SerializedTaskPump m_commandPump;
	std::atomic<bool> m_workerQuit{false};
	uint64_t m_lastProcessedReadbackFence = 0;
	std::atomic<bool> m_initialized{false};
	struct ReadbackSlot {
		std::shared_ptr<org::Buffer> staging;
		std::vector<uint32_t> activeStreamingTextureIDs;
		uint64_t capacityBytes = 0;
		uint64_t copyBytes = 0;
		uint64_t fenceValue = 0;
		bool inFlight = false;
	};
	struct ReadbackCallbackState {
		std::mutex mutex;
		TextureStreamingManager* owner = nullptr;
	};
	std::shared_ptr<ReadbackCallbackState> m_readbackCallbackState =
		std::make_shared<ReadbackCallbackState>();
	rhi::TimelinePtr m_readbackFencePtr;
	rhi::Timeline m_readbackFence;
	std::atomic<uint64_t> m_readbackFenceCounter{0};
	std::mutex m_readbackSlotMutex;
	std::vector<ReadbackSlot> m_readbackSlots;
	uint32_t m_readbackSlotCursor = 0;
	mutable std::mutex m_statsMutex;
	MaterialTextureStreamingStats m_publishedStats;
	uint64_t m_publishedStatsSequence = 1;
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
