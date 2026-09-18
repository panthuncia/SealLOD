#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>
#include <functional>
#include <cstring>
#include <unordered_set>

#include <tbb/concurrent_queue.h>


#include "Materials/Material.h"
#include "Managers/TextureStreamingManager.h"
#include "Interfaces/IResourceProvider.h"
#include "Resources/Buffers/DynamicStructuredBuffer.h"
#include "Render/IndirectCommand.h"
#include "Render/ITextureStreamingFeedbackService.h"
#include "Render/MaterialCompileFlagsSlotRegistry.h"
#include "Render/MaterialStateArtifacts.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/RasterBucketFlags.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"

namespace org::runtime {
class IReadbackService;
class IUploadService;
class IDescriptorService;
}
namespace org { class DynamicGloballyIndexedResource; }
namespace br::render { class RendererStateRequestService; class VersionedGpuBufferBackingPool; }

class TextureFactory;
namespace org { class CopyPass; }
using org::CopyPass;

// Manages buffers for per-material-compile-flag work (e.g., visibility buffer per-material)
class MaterialManager : public IResourceProvider, public ITextureStreamingFeedbackService {
public:
	~MaterialManager();
	static std::unique_ptr<MaterialManager> CreateUnique() {
		return std::unique_ptr<MaterialManager>(new MaterialManager());
	}
	unsigned int AcquireCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count = 1u);
	bool ReleaseCompileFlagsSlot(MaterialCompileFlags flags, unsigned int count = 1u);
	bool TryGetCompileFlagsSlot(MaterialCompileFlags flags, unsigned int& slot) const;
	unsigned int GetMaterialSlot(unsigned int materialID, std::optional<PerMaterialCB> data = std::nullopt);
	unsigned int AcquireRasterBucket(MaterialRasterFlags rasterFlags, unsigned int count = 1u);
	void ReleaseRasterBucket(MaterialRasterFlags rasterFlags);

	unsigned int IncrementMaterialUsageCount(Material& material,
		bool refreshTextureBindings = false, unsigned int count = 1u);
	struct MaterialUsageCapture {
		br::render::MaterialUsageBatchEntry entry;
		std::vector<std::shared_ptr<TextureAsset>> textureServiceInputs;
	};
	MaterialUsageCapture CaptureMaterialUsage(
		Material& material, unsigned int count, bool refreshTextureBindings);
	std::shared_ptr<const br::render::MaterialUsageReservation> ReserveMaterialUsage(
		const std::vector<MaterialUsageCapture>& captures);
	void RegisterMaterialSource(const std::shared_ptr<Material>& material);
	bool ApplyMaterialRowArtifact(const br::render::MaterialRowArtifact& row);
	void DecrementMaterialUsageCount(const Material& material);
	void InitializeTextureStreaming(TextureFactory& textureFactory, uint32_t framesInFlight);
	void ShutdownTextureStreaming();
	void BeginTextureStreamingFeedbackFrame(uint64_t frameIndex);
	void ProcessPendingMaterialUpdates(uint64_t frameIndex);
	void AcknowledgePublishedTextureImageTable(
		const std::shared_ptr<const br::render::PublishedRendererState>& published) {
		if (m_textureStreamingManager)
			m_textureStreamingManager->AcknowledgePublishedImageTable(published);
	}
	std::shared_ptr<RenderPass> CreateTextureStreamingFeedbackReadbackPass() override;
	void SetTextureStreamingFeedbackSuppressed(bool suppressed) { m_textureStreamingFeedbackSuppressed = suppressed; }
	MaterialTextureStreamingStats GetMaterialTextureStreamingStats() const;
	MaterialTextureStreamingReadinessStats GetMaterialTextureStreamingReadinessStats() const;
	void RegisterStreamingTexture(const std::shared_ptr<TextureAsset>& texture, TextureFactory& textureFactory);
	TextureStreamingManager* GetTextureStreamingManager() const { return m_textureStreamingManager.get(); }
	using RequestTextureReadbackFn =
		std::function<void(std::shared_ptr<PixelBuffer>, std::wstring, std::function<void()>)>;
	void SetRequestTextureReadbackFn(RequestTextureReadbackFn fn) {
		m_requestTextureReadback = std::move(fn);
	}

	void UpdateMaterialDataBuffer(Material& material);
	void MarkMaterialDirty(Material& material);
	void UpdateOpenPBRMaterialDataBuffer(unsigned int materialSlot, const PerMaterialOpenPBRCB& data);

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;
	std::vector<ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<IResourceResolver> ProvideResolver(ResourceIdentifier const& key) override;

	std::uint64_t CommitGpuVisibleSnapshot(bool forceGraphSnapshot = false);
	void ScheduleGpuVisibleSnapshotCommit(bool forceGraphSnapshot = false);
	std::uint64_t DesiredPublishedStateRevision() const noexcept { return m_materialStateRevision; }
	br::render::ArtifactVersionID DesiredPublishedStateVersion() const noexcept {
		return m_materialStateHandle.version;
	}
	br::render::ArtifactVersionHandle DesiredPublishedStateHandle() const {
		return m_materialStateHandle;
	}
	bool TryActivatePublishedMaterialState(
		const std::shared_ptr<const br::render::PublishedRendererState>& published);
	void SetRendererStateServices(br::render::RendererStateRequestService* requests,
		std::shared_ptr<org::runtime::IUploadService> uploads) {
		m_rendererStateRequests = requests;
		m_uploadService = uploads;
		if (m_textureStreamingManager) m_textureStreamingManager->SetRendererStateRequestService(requests, uploads);
	}
	void SetDescriptorService(std::shared_ptr<org::runtime::IDescriptorService> descriptors);
	// These read registry state, so they take the registry lock. They are not
	// on any hot path (the raster-bucket tables are touched at material
	// admission), which is why they are not served from a published snapshot.
	unsigned int GetRasterBucketCount() const;
	unsigned int GetRasterBucketForFlags(MaterialRasterFlags rasterFlags) const;
	MaterialRasterFlags GetRasterFlagsForBucket(unsigned int bucketIndex) const;
	bool RequestExternalMaterialTextureReadback(
		const std::shared_ptr<PixelBuffer>& image,
		std::wstring outputFile,
		std::function<void()> callback);
private:
	MaterialManager();
	TaskScope m_snapshotCommitScope;
	std::atomic_bool m_snapshotCommitScheduled{ false };
	// Dirty-material rows are rebuilt and journaled on the serialized material
	// acceptance domain, never on the renderer thread.
	std::atomic_bool m_dirtyMaterialFlushScheduled{ false };
	void ScheduleDirtyMaterialFlush();
	void FlushDirtyMaterials();
	std::atomic_bool m_forceSnapshotCommit{ false };
	std::uint64_t m_materialRowsAppliedSinceGraphSnapshot = 0;
	void UpdateMaterialTextureUsage(const Material& material, int delta);
	void RefreshMaterialTextureUsage(const Material& material);
	void TrackMaterialTextureAssets(const Material& material, int delta);
	void TrackMaterialTextureAssets(std::uint32_t materialID,
		const std::vector<std::shared_ptr<TextureAsset>>& textureAssets,
		bool alphaTested, int delta);
	// Untracking reads the stored binding IDs, so it needs no asset list. The
	// delta form used to be called with a freshly collected one that it ignored.
	void UntrackMaterialTextureAssets(std::uint32_t materialID);
	bool MaterialTextureAssetBindingsChanged(const Material& material) const;
	bool MaterialTextureAssetBindingsChanged(const Material& material,
		const std::vector<std::shared_ptr<TextureAsset>>& textureAssets) const;
	void FlushDirtyMaterial(Material& material, bool refreshTextureBindings = false);
	void EnsureMaterialBufferCapacity(unsigned int requiredSlots);
	void EnsureCompileFlagsBufferCapacity(unsigned int requiredSlots);
	std::vector<std::shared_ptr<Resource>> CollectActiveMaterialTextureResources() const;
	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;
	std::unordered_map<ResourceIdentifier, std::shared_ptr<IResourceResolver>, ResourceIdentifier::Hasher> m_resolvers;
	std::array<std::shared_ptr<PublishedStateResourceResolver>, 3> m_materialTableResolvers;
	std::unordered_map<uint32_t, std::vector<std::shared_ptr<Resource>>> m_trackedMaterialTextures;
	std::atomic<uint64_t> m_trackedTexturesRevision{ 1 };
	// Debug statistics: rebuilt off-thread when their inputs change; the owner
	// thread only copies the latest result.
	mutable std::mutex m_streamingStatsMutex;
	mutable MaterialTextureStreamingStats m_cachedStreamingStats;
	mutable uint64_t m_cachedStreamingStatsTrackedRevision = 0;
	mutable uint64_t m_cachedStreamingStatsPublishedSequence = 0;
	mutable bool m_cachedStreamingStatsValid = false;
	mutable std::atomic_bool m_streamingStatsRefreshScheduled{ false };
	void ScheduleStreamingStatsRefresh() const;
	std::unordered_map<uint32_t, Material*> m_activeMaterialsByID;
	std::unordered_map<uint32_t, std::weak_ptr<Material>> m_ingestedMaterialSourcesByID;
	std::unordered_map<uint32_t, std::vector<uint64_t>> m_materialTextureStreamingBindingIDs;
	std::unordered_map<uint32_t, std::vector<uint32_t>> m_materialTextureStreamingTextureIDs;
	std::unordered_map<uint32_t, std::uint64_t> m_materialRowSourceRevisions;
	std::shared_ptr<void> m_reservationLifetime = std::make_shared<int>(0);
	bool m_textureStreamingFeedbackSuppressed = false;
	MaterialCompileFlagsSlotRegistry m_compileFlagsRegistry;

	// Two ownership domains, not one lock.
	//
	// The slot registry below (ID->slot mapping, free list, usage counts,
	// compile-flag slots, raster buckets) is guarded by m_registryMutex. Import
	// workers need a slot the moment they ask for one, so this stays
	// synchronous; the rule is that a registry critical section never builds
	// constant buffers, journals a row, registers a texture binding or submits
	// to the state graph.
	//
	// Everything else (dirty set, upload signatures, row revisions, journals,
	// the active/ingested material maps, texture tracking) is authoring state
	// owned exclusively by TaskDomain::MaterialAcceptance, which has limit 1.
	// It takes no lock at all. Off-domain writers post a mutation instead; see
	// PostMaterialMutation. Read AssertOnMaterialAcceptance as documentation of
	// which functions rely on that ownership.
	mutable std::mutex m_registryMutex;
	// Authoring mutations posted by threads that do not own the acceptance
	// domain. Drained at the head of every acceptance task, in post order.
	tbb::concurrent_queue<std::function<void()>> m_postedMaterialMutations;
	void PostMaterialMutation(std::function<void()> mutation);
	void DrainPostedMaterialMutations();

	unsigned int m_materialSlotsUsed = 0;
	std::vector<unsigned int> m_freeMaterialSlots;
	std::vector<unsigned int> m_materialUsageCounts = { };
	std::unordered_map<std::uint32_t, std::uint64_t> m_pendingMaterialUsageCounts;
	std::unordered_set<std::uint32_t> m_materialReservationOwnedIDs;
	std::unordered_map <unsigned int, unsigned int> m_materialIDSlotMapping;
	struct MaterialGpuUploadSignature {
		PerMaterialCB materialData = {};
		PerMaterialEvalCB evalData = {};
		PerMaterialOpenPBRCB openPBRData = {};
		bool valid = false;
	};
	std::vector<MaterialGpuUploadSignature> m_materialUploadSignatures;
	void JournalMaterialRow(unsigned int materialSlot);
	// Registry lock already held. GetMaterialSlot used to re-enter the one
	// recursive mutex from callers that held it; the registry lock is not
	// recursive, so nesting is expressed by calling this directly.
	unsigned int GetMaterialSlotLocked(unsigned int materialID,
		std::optional<PerMaterialCB> data = std::nullopt);
	// Registry lock already held.
	unsigned int GetRasterBucketForFlagsLocked(MaterialRasterFlags rasterFlags) const;
	// Registry lock already held. The usage reservation's commit applies rows
	// while it holds that lock, so it cannot go through the public entry point.
	bool ApplyMaterialRowArtifactLocked(const br::render::MaterialRowArtifact& row);
	// Slots the registry (re)allocated whose upload signature the acceptance
	// domain must clear before it trusts it. A recycled slot would otherwise
	// inherit the previous material's row. Deliberately a lock-free queue: the
	// drain must never need m_registryMutex, because it runs at points that may
	// already hold it and applies mutations that take it themselves.
	tbb::concurrent_queue<unsigned int> m_slotsNeedingSignatureReset;
	br::render::VersionedGpuBufferJournal m_materialBaseJournal{ sizeof(PerMaterialCB) };
	br::render::VersionedGpuBufferJournal m_materialEvalJournal{ sizeof(PerMaterialEvalCB) };
	br::render::VersionedGpuBufferJournal m_materialOpenPbrJournal{ sizeof(PerMaterialOpenPBRCB) };
	// Written by the registry when it grows, read by the acceptance domain when
	// it journals a row. Monotonic, so an atomic scalar is the whole contract.
	std::atomic<unsigned int> m_materialBufferCapacity{ 0u };

	static constexpr unsigned int kBufferGrowthSize = 100;
	static constexpr unsigned int kInitialMaterialBufferCapacity = 4096;

	static constexpr unsigned int kScanBlockSize = 1024;

	// Material raster flags to raster bin mapping
	std::unordered_map<uint32_t, unsigned int> m_rasterFlagToBucketMapping;
	std::vector<MaterialRasterFlags> m_bucketToRasterFlagMapping;
	std::vector<unsigned int> m_rasterBucketUsageCounts;
	unsigned int m_rasterBucketsUsed = 0;
	std::vector<unsigned int> m_freeRasterBuckets;

	// Visibility buffer
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_materialPixelCountBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_materialOffsetBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_materialWriteCursorBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_blockSumsBuffer;
	std::shared_ptr<DynamicStructuredBuffer<uint32_t>> m_scannedBlockSumsBuffer;
	std::shared_ptr<DynamicStructuredBuffer<MaterialEvaluationIndirectCommand>> m_materialEvaluationCommandBuffer;

	std::unique_ptr<TextureStreamingManager> m_textureStreamingManager;
	std::vector<uint32_t> m_dirtyMaterialIDs;
	std::unordered_set<uint32_t> m_dirtyMaterialIDSet;
	std::chrono::steady_clock::time_point m_lastMaterialUpdateStatsLog = {};
	RequestTextureReadbackFn m_requestTextureReadback;
	br::render::RendererStateRequestService* m_rendererStateRequests = nullptr;
	std::shared_ptr<org::runtime::IUploadService> m_uploadService;
	std::shared_ptr<org::runtime::IDescriptorService> m_descriptorService;
	std::atomic_uint64_t m_materialRowsRevision{ 1 };
	bool m_materialGraphActive = false;
	std::uint64_t m_activeMaterialPublishedRevision = 0;
	std::uint64_t m_acknowledgedMaterialPublishedRevision = 0;
	std::array<std::shared_ptr<org::DynamicGloballyIndexedResource>, 3> m_materialStartupFallbacks;
	std::uint64_t m_materialStateFingerprint = 0;
	std::uint64_t m_pendingMaterialStateFingerprint = 0;
	std::uint32_t m_materialStateStableFrames = 0;
	std::uint32_t m_materialStateDirtyFrames = 0;
	std::uint64_t m_materialStateRevision = 0;
	br::render::ArtifactVersionHandle m_materialStateHandle{};
	std::array<std::unique_ptr<br::render::VersionedBufferFamily>, 3> m_materialBufferFamilies;
	std::uint64_t m_materialTableHandleRowsRevision = 0;
	std::array<br::render::ArtifactVersionHandle, 3> m_materialTableHandles{};
	std::unordered_set<uint64_t> m_traceReadbackResourceIDs;
	std::weak_ptr<TextureAsset> m_traceBaseColorTexture;
	bool m_traceLateReadbackRequested = false;
};
