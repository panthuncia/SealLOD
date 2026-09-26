#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <limits>
#include <optional>
#include <mutex>
#include <cstdint>
#include <deque>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BasicRenderer/Extensions/Buffers/LazyDynamicStructuredBuffer.h"
#include <BasicRenderer/Extensions/Resources/DynamicStructuredBuffer.h>
#include "BasicRenderer/Extensions/Buffers/DynamicBuffer.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"
#include <BasicRenderer/Streaming/ObjectRequests.h>
#include "BasicRenderer/Extensions/Buffers/SortedUnsignedIntBuffer.h"
#include <BasicRenderer/Extensions/IndirectCommand.h>
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/RendererComponents.h"
#include "Interfaces/IResourceProvider.h"
#include "BasicRenderer/Assets/TechniqueDescriptor.h"
#include "Render/Runtime/BufferUploadPolicy.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Scene/Objects/ObjectBufferStateArtifacts.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "Utilities/TripleGenerationMailbox.h"

namespace org { class BufferView; }
namespace org { class GloballyIndexedResource; }
namespace org { class DynamicBuffer; }
namespace org::runtime { class IUploadService; }
class PublishedStateResourceResolver;
namespace br::render { class RendererStateRequestService; class VersionedGpuBufferBackingPool; struct PublishedGpuBufferVersion; struct PublishedRendererState; }
class Material;
class Mesh;

class ObjectManager : public org::IResourceProvider {
public:
	using ActiveDrawSetMutationCallback = std::function<void(
		const DrawWorkloadKey&, bool, std::uint64_t,
		std::shared_ptr<const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>>)>;
	static std::unique_ptr<ObjectManager> CreateUnique() {
		return std::unique_ptr<ObjectManager>(new ObjectManager());
	}
	~ObjectManager();


	Components::ObjectDrawInfo AddObject(const PerObjectCB& perObjectCB, const Components::MeshInstances* meshInstances);
	std::vector<Components::ObjectDrawInfo> AddObjectsBulk(const std::vector<br::render::ObjectBuildInfo>& objects);
	std::vector<Components::ObjectDrawInfo> AddStaticGroupsBulk(const std::vector<br::render::StaticGroupBuildInfo>& groups);
	static br::render::PreparedStaticGroupsBulkPlan PrepareStaticGroupsBulkPlan(const std::vector<br::render::StaticGroupBuildInfo>& groups);
	static br::render::StaticImportPacketPlan PrepareStaticImportPacketPlan(const std::vector<br::render::StaticGroupBuildInfo>& groups);
	static br::render::StaticImportPacket BuildStaticImportPacket(br::render::StaticImportPacketPlan plan);
	static br::render::StaticImportBuildBatch PrepareStaticImportBuildBatch(const std::vector<br::render::StaticGroupBuildInfo>& groups);
	static void FinalizeStaticImportBuildBatch(br::render::StaticImportBuildBatch& build);
	static br::render::StaticImportBuildBatch FinalizeStaticRecipeBuild(br::render::StaticRecipeView view);
	void PrepareStaticGroupCommitResourcesAsync(const br::render::PreparedStaticGroupsBulkPlan& plan);
	void RequestStaticImportPacketResources(const br::render::StaticImportPacketPlan& plan);
	void RequestStaticImportTransactionResources(const br::render::StaticImportBuildBatch& build);
	// Supplies a workload-derived upper bound before static transactions begin
	// consuming the persistent graph-owned object buffers. This changes only
	// backing capacity; logical row counts and immutable version identity remain
	// driven by committed writes.
	void RequestStaticImportGraphCapacityHint(
		std::uint64_t transformRows, std::uint64_t drawRecords);
	br::render::StaticImportResourceProbe CreateStaticImportResourceProbe() const;
	br::render::StaticImportResourceProbeStatus ProbeStaticImportTransactionResources(
		br::render::StaticImportBuildBatch& build,
		br::render::StaticImportResourceProbe& probe);
	br::render::StaticImportReservationStatus TryReserveStaticImportTransaction(
		br::render::StaticImportBuildBatch build,
		br::render::StaticImportReservation& reservation);
	std::vector<br::render::StaticImportReservationStatus> TryReserveStaticImportTransactionsBatch(
		std::vector<br::render::StaticImportBuildBatch>& builds,
		std::vector<br::render::StaticImportReservation>& reservations);
	std::vector<br::render::StaticImportReservationStatus> TryReserveStaticImportTransactionsInPlace(
		std::span<br::render::StaticImportBuildBatch*> builds,
		std::vector<br::render::StaticImportReservation>& reservations);
	br::render::MaterializedStaticImportTransaction MaterializeStaticImportTransaction(br::render::StaticImportReservation reservation) const;
	br::render::MaterializedStaticImportTransaction MaterializeStaticImportTransaction(
		br::render::StaticImportReservation&& reservation,
		br::render::StaticImportBuildBatch& buildScratch) const;
	// Copies immutable transaction rows into the versioned buffer journals. This
	// is safe to run during worker preparation after ranges have been reserved;
	// publication only activates generations and selects the resulting versions.
	void StageStaticImportTransactionUploads(
		br::render::MaterializedStaticImportTransaction& transaction,
		bool includeDrawRecords = true);
	br::render::StaticImportPublishResult PublishStaticImportTransaction(br::render::MaterializedStaticImportTransaction transaction);
	br::render::StaticImportBulkPublishResult PublishStaticImportTransactionsBulk(std::span<br::render::MaterializedStaticImportTransaction*> transactions);
	void CancelStaticImportTransaction(br::render::StaticImportReservation reservation, std::uint64_t retireFrame = 0);
	std::vector<Components::ObjectDrawInfo> PublishStaticImportPacket(br::render::StaticImportPacket packet);
	std::vector<Components::ObjectDrawInfo> CommitPreparedStaticGroupsBulk(const br::render::PreparedStaticGroupsBulkPlan& plan);
	br::render::StaticObjectRemovalPayload BuildStaticObjectRemovalPayload(std::span<const Components::ObjectDrawInfo> drawInfos) const;
	void RemoveObject(const Components::ObjectDrawInfo* drawInfo);
	void RemoveObjectsBulk(
		const std::vector<const Components::ObjectDrawInfo*>& drawInfos,
		const br::render::RemoveObjectsBulkOptions& options);
	void RemoveObjectsBulk(const std::vector<const Components::ObjectDrawInfo *> &drawInfos);
	br::render::StaticObjectRemovalResult RemoveStaticObjectsBulk(
		std::span<const br::render::StaticObjectRemovalPayload> payloads,
		const br::render::RemoveObjectsBulkOptions& options);
	br::render::StaticObjectRemovalResult RemoveStaticObjectsBulk(std::span<const br::render::StaticObjectRemovalPayload> payloads);
	br::render::StaticVisibilityUpdateResult SetStaticObjectsVisibleBulk(
		std::span<br::render::StaticObjectResidencyHandle*> handles,
		bool visible);
	void UpdatePerObjectBuffer(org::BufferView*, PerObjectCB& data);
	void UpdateNormalMatrixBuffer(org::BufferView* view, void* data);
	void PublishDeferredRetireCompletedFrame(std::uint64_t completedFrame, std::uint64_t retireDelayFrames);
	std::uint64_t MakeDeferredRetireFrame() const;
	std::vector<br::render::ActiveDrawSetDebugStats> SnapshotActiveDrawSetDebugStats() const;

	org::runtime::BulkWriteHandle BeginPerObjectBulkWrite();
	void EndPerObjectBulkWrite(size_t dirtyOffset, size_t dirtySize);
	org::runtime::BulkWriteHandle BeginPerInstanceTransformBulkWrite();
	void EndPerInstanceTransformBulkWrite(size_t dirtyOffset, size_t dirtySize);
	org::runtime::BulkWriteHandle BeginNormalMatrixBulkWrite();
	void EndNormalMatrixBulkWrite(size_t dirtyOffset, size_t dirtySize);

	std::shared_ptr<org::DynamicBuffer>& GetPerObjectBuffers() {
		return m_perObjectBuffers;
	}

	std::uint64_t GetResidentInstanceDrawRecordCount() const {
		// The graph-owned visibility-generation table has exactly one logical row per allocated
		// draw-record index. Use that logical extent for immutable active-list
		// validation; backing capacity can temporarily lag while an asynchronous grow
		// is awaiting publication.
		return m_drawRecordVisibilityGenerations.size();
	}
	std::uint64_t GetResidentInstanceTransformCount() const {
		return m_perInstanceTransformBuffers
			? m_perInstanceTransformBuffers->GetBufferSize() / sizeof(PerInstanceTransformCB)
			: 0u;
	}
	std::span<const std::uint32_t> GetDrawRecordVisibilityGenerations() const {
		return m_drawRecordVisibilityGenerations;
	}
	std::shared_ptr<DynamicStructuredBuffer<SkinnedAssemblyPlacementGPU>>& GetSkinnedAssemblyPlacements() { return m_skinnedAssemblyPlacements; }
	std::shared_ptr<SortedUnsignedIntBuffer>& GetActiveSkinnedAssemblyPlacements() { return m_activeSkinnedAssemblyPlacements; }
	std::span<const SkinnedAssemblyPlacementGPU> GetSkinnedAssemblyPlacementCPU() const { return m_skinnedAssemblyPlacementCPU; }

	std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
	std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
	std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override;
	std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override;
	void SetRendererStateServices(br::render::RendererStateRequestService* requests,
		std::shared_ptr<org::runtime::IUploadService> uploads, std::uint32_t framesInFlight);
	void SetDesiredBufferStateReadyCallback(br::render::DesiredBufferStateReadyCallback callback);
	std::uint64_t PublishDesiredBufferState();
	void AcknowledgePublishedBufferState(
		const std::shared_ptr<const br::render::PublishedRendererState>& published);
	// Called for every committed manifest: its Geometry root is what frames now
	// draw with, so its coverage releases draw-records roots waiting on it.
	// Posts suspension notifications only; never waits on the state graph.
	void ObserveResidentGeometry(const br::render::PublishedRendererState& committed);
	std::optional<br::render::ArtifactRequirement> DesiredBufferStateRequirement() const;
	// Owner-thread entry: seals nothing itself, it schedules PublishDesiredBufferState
	// on a worker so the renderer thread never enters the state-graph mutex.
	void ScheduleDesiredBufferStatePublish();
	br::render::ArtifactVersionHandle DesiredBufferStateHandle() const;
	br::render::DesiredObjectBufferStateCut DesiredBufferStateCut() const;
	// Static draw records name mesh-template and CLod rows that shaders resolve
	// through the published Geometry root. Each static transaction records the
	// geometry mutation sequence current at commit (its templates were accepted
	// earlier), and every draw-records root carries a minimum publication
	// dependency on a Geometry root covering that sequence.
	void SetGeometryCoverageSource(std::function<std::uint64_t()> source);
	[[nodiscard]] std::uint64_t RequiredGeometryCoverage() const noexcept {
		return m_requiredGeometryCoveragePublished.load(std::memory_order_acquire);
	}
	std::shared_ptr<SortedUnsignedIntBuffer> TryGetActiveDrawSetIndices(const DrawWorkloadKey& workloadKey) {
		auto it = m_activeDrawSetIndices.find(workloadKey);
		return it != m_activeDrawSetIndices.end() ? it->second : nullptr;
	}
	void SetActiveDrawSetMutationCallback(ActiveDrawSetMutationCallback callback);
	std::shared_ptr<SortedUnsignedIntBuffer> GetActiveDrawSetIndices(const DrawWorkloadKey& workloadKey) {
		auto buffer = TryGetActiveDrawSetIndices(workloadKey);
		if (!buffer) {
			throw std::runtime_error("Active draw set indices for given flags not found");
		}
		return buffer;
	}
	std::shared_ptr<SortedUnsignedIntBuffer> GetActiveDrawSetIndices(MaterialCompileFlags flags, const RenderPhase& renderPhase, bool clodOnly = false) {
        return GetActiveDrawSetIndices(DrawWorkloadKey { flags, renderPhase, clodOnly });
    }
    uint64_t GetDrawSetDeclarationRevision() const { return m_drawSetDeclarationRevision; }
	br::render::ObjectStorageStats GetStats() const;

private:
	void PublishSkinnedAssemblyPlacements(br::render::MaterializedStaticImportTransaction& transaction);
	void PublishSkinnedPlacementSourceVersionLocked();
	std::uint32_t AllocateSkinnedAssemblyPlacement(SkinnedAssemblyPlacementGPU placement);
	void FreeSkinnedAssemblyPlacement(std::uint32_t placementIndex);
	ObjectManager();
	struct GraphBufferBinding {
		org::ResourceIdentifier identifier;
		std::shared_ptr<org::DynamicBuffer> buffer;
		br::render::ArtifactKey key;
		std::uint64_t catalogVariant = 0;
		std::uint32_t elementStride = 0;
		br::render::ArtifactVersionID submittedVersion{};
		br::render::ArtifactVersionHandle submittedHandle{};
		std::shared_ptr<br::render::VersionedGpuBufferBackingPool> backingPool;
	};

	// Immutable producer cut handed from the ordered object-journal writer to
	// the graph submitter. Capturing happens once at the end of a mutation
	// transaction; graph submission never reaches back into mutable buffers.
	struct ObjectBufferSnapshotCut {
		std::vector<br::render::VersionedGpuBufferJournal::Capture> buffers;
		br::render::VersionedGpuBufferJournal::Capture visibility;
		br::render::VersionedGpuBufferJournal::Capture skinnedPlacements;
		br::render::VersionedGpuBufferJournal::Capture activeSkinnedPlacements;
		std::uint32_t residentTransformCount = 0;
		std::shared_ptr<const std::vector<SkinnedAssemblyPlacementGPU>> placementRecords;
		std::shared_ptr<const std::vector<br::render::PublishedActiveSkinnedPlacement>> activePlacementEntries;
		std::uint64_t fingerprint = 0;
		std::uint64_t coveredMutationGeneration = 0;
		std::uint64_t requiredGeometryCoverage = 0;
	};

	std::uint64_t SealDesiredBufferStateLocked();
	void RecordStaticGeometryRequirementLocked();

	struct DeferredBufferRangeRetire {
		std::shared_ptr<org::DynamicBuffer> buffer;
		std::uint64_t offset = 0;
		std::uint64_t size = 0;
		std::uint64_t retireFrame = 0;
	};

	struct ActiveDrawSetCompactionJob {
		DrawWorkloadKey workloadKey;
		std::shared_ptr<SortedUnsignedIntBuffer> buffer;
		std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> entries;
		std::vector<std::uint32_t> visibilityGenerations;
		std::uint64_t activeSetRevision = 0;
		std::uint64_t visibilityRevision = 0;
	};

	struct ActiveDrawSetCompactionResult {
		DrawWorkloadKey workloadKey;
		std::shared_ptr<SortedUnsignedIntBuffer> buffer;
		std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry> entries;
		std::uint64_t activeSetRevision = 0;
		std::uint64_t visibilityRevision = 0;
		std::size_t inputEntries = 0;
		std::uint64_t buildUs = 0;
	};

	void StartDeferredRetireWorker();
	void StopDeferredRetireWorker();
	void ScheduleDeferredRetireDrain();
	void DeferredRetireDrain(const br::TaskContext& context);
	void EnqueueDeferredBufferRangeRetire(
		const std::shared_ptr<org::DynamicBuffer>& buffer,
		std::uint64_t offset,
		std::uint64_t size,
		std::uint64_t retireFrame);
	void EnqueueDeferredBufferRangeRetires(
		const std::shared_ptr<org::DynamicBuffer>& buffer,
		const std::vector<Components::ObjectDrawInfo::BufferRange>& ranges,
		std::uint64_t retireFrame);
	void EnqueueDeferredBufferRangeRetires(std::vector<DeferredBufferRangeRetire> retires);
	void StartActiveDrawSetCompactionWorker();
	void StopActiveDrawSetCompactionWorker();
	void RunActiveDrawSetCompaction(ActiveDrawSetCompactionJob job, const br::TaskContext& context);
	void ScheduleActiveDrawSetCompactionDrain();
	std::vector<br::render::ActiveDrawSetCompactionPublishResult> PublishActiveDrawSetCompactionResults(
		std::size_t maxResults = 0);
	void PumpActiveDrawSetCompactionRequests(std::size_t maxRequests);
	void MaybeQueueActiveDrawSetCompaction(
		const DrawWorkloadKey& workloadKey,
		const std::shared_ptr<SortedUnsignedIntBuffer>& buffer);

	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>, org::ResourceIdentifier::Hasher> m_resources;
	std::shared_ptr<org::DynamicBuffer> m_perObjectBuffers; // Per object constant buffer
	std::shared_ptr<org::DynamicBuffer> m_perInstanceTransformBuffers; // Per instance transform/object data
	std::shared_ptr<org::DynamicBuffer> m_instanceDrawRecordBuffers; // Compact draw records consumed by GPU culling
	std::shared_ptr<org::DynamicBuffer> m_masterIndirectCommandsBuffer; // Indirect draw command buffer
	std::shared_ptr<org::DynamicBuffer> m_normalMatrixBuffer; // Normal matrices for each object
	std::unordered_map<DrawWorkloadKey, std::shared_ptr<SortedUnsignedIntBuffer>, DrawWorkloadKey::Hasher> m_activeDrawSetIndices; // Indices into m_drawSetCommandsBuffer for active objects per workload
	ActiveDrawSetMutationCallback m_activeDrawSetMutationCallback;
	std::vector<std::uint32_t> m_drawRecordVisibilityGenerations;
	std::shared_ptr<DynamicStructuredBuffer<SkinnedAssemblyPlacementGPU>> m_skinnedAssemblyPlacements;
	std::shared_ptr<SortedUnsignedIntBuffer> m_activeSkinnedAssemblyPlacements;
	// Stages only the changed rows of the skinned placement buffer (contiguous
	// runs of `indices`) instead of re-uploading the whole table per publish.
	void StageSkinnedAssemblyPlacementRows(std::vector<std::uint32_t> indices);
	std::vector<SkinnedAssemblyPlacementGPU> m_skinnedAssemblyPlacementCPU;
	std::vector<std::uint32_t> m_freeSkinnedAssemblyPlacementIndices;
	std::vector<std::uint8_t> m_skinnedAssemblyPlacementFree;
	std::shared_ptr<const std::vector<SkinnedAssemblyPlacementGPU>> m_publishedSkinnedPlacementRecords;
	std::shared_ptr<const std::vector<br::render::PublishedActiveSkinnedPlacement>>
		m_publishedActiveSkinnedPlacementEntries;
	std::vector<GraphBufferBinding> m_graphBufferBindings;
	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<PublishedStateResourceResolver>,
		org::ResourceIdentifier::Hasher> m_graphBufferResolvers;
	br::render::RendererStateRequestService* m_rendererStateRequests = nullptr;
	std::shared_ptr<org::runtime::IUploadService> m_uploadService;
	std::uint64_t m_objectBufferStateRevision = 0;
	br::render::ArtifactVersionHandle m_objectBufferStateVersion{};
	std::atomic<std::uint64_t> m_activeObjectBufferStateRevision{ 0 };
	std::uint64_t m_objectBufferFingerprint = 0;
	mutable std::mutex m_objectBufferGraphStateMutex;
	std::atomic_bool m_objectBufferGraphDirty{ true };
	br::TripleGenerationMailbox<ObjectBufferSnapshotCut> m_objectBufferSnapshotMailbox;
	std::uint64_t m_objectBufferSnapshotGeneration = 0;
	std::uint64_t m_objectBufferSubmittedSnapshotGeneration = 0;
	std::atomic<std::uint64_t> m_objectBufferMutationGeneration{ 0 };
	std::uint64_t m_objectBufferSubmittedMutationGeneration = 0;
	std::uint64_t m_drawRecordVisibilityRevision = 1;
	br::render::VersionedGpuBufferJournal m_visibilityGenerationJournal{ sizeof(std::uint32_t) };
	br::render::ArtifactVersionID m_visibilityGenerationSubmittedVersion{};
	br::render::ArtifactVersionHandle m_visibilityGenerationSubmittedHandle{};
	std::shared_ptr<br::render::VersionedGpuBufferBackingPool> m_visibilityGenerationBackingPool;
	br::render::VersionedGpuBufferJournal m_skinnedPlacementJournal{ sizeof(SkinnedAssemblyPlacementGPU) };
	br::render::VersionedGpuBufferJournal m_activeSkinnedPlacementJournal{
		sizeof(br::render::PublishedActiveSkinnedPlacement) };
	br::render::ArtifactVersionID m_skinnedPlacementSubmittedVersion{};
	br::render::ArtifactVersionID m_activeSkinnedPlacementSubmittedVersion{};
	br::render::ArtifactVersionHandle m_skinnedPlacementSubmittedHandle{};
	br::render::ArtifactVersionHandle m_activeSkinnedPlacementSubmittedHandle{};
	std::vector<br::render::ArtifactVersionHandle> m_objectBufferCutVersions;
	std::shared_ptr<br::render::ResidentGeometryCoverage> m_residentGeometryCoverage;
	br::render::ArtifactVersionHandle m_geometryCoverageGate{};
	std::shared_ptr<br::render::VersionedGpuBufferBackingPool> m_skinnedPlacementBackingPool;
	std::shared_ptr<br::render::VersionedGpuBufferBackingPool> m_activeSkinnedPlacementBackingPool;
	std::uint32_t m_graphFramesInFlight = 1;
	std::uint64_t m_lastBufferStatePublicationRetirementEpoch = 0;
	mutable std::mutex m_desiredBufferStateReadyCallbackMutex;
	br::render::DesiredBufferStateReadyCallback m_desiredBufferStateReadyCallback;
	std::atomic<std::uint64_t> m_nextStaticImportTransactionID{ 1 };
	// Serializes the ordered producer side of the static CPU journals,
	// visibility generations, and active lists. Transactions issue logical
	// coverage generations while the graph submitter captures the newest
	// coherent cut only after its admission gates open.
	mutable std::mutex m_staticPublicationMutationMutex;
	std::function<std::uint64_t()> m_geometryCoverageSource;
	std::uint64_t m_requiredGeometryCoverage = 0; // m_staticPublicationMutationMutex
	std::atomic<std::uint64_t> m_requiredGeometryCoveragePublished{ 0 };
	std::shared_ptr<org::LazyDynamicStructuredBuffer<PerMeshInstanceCB>> m_perMeshInstanceBuffers; // Indices into m_perObjectBuffers for each mesh instance in each object
    uint64_t m_drawSetDeclarationRevision = 1u;
	br::render::ObjectStorageStats m_stats{};
	std::mutex m_deferredRetireMutex;
	std::deque<DeferredBufferRangeRetire> m_deferredRetireQueue;
	TaskScope m_deferredRetireScope;
	TaskScope m_desiredPublishScope;
	std::atomic_bool m_desiredPublishScheduled{ false };
	std::atomic_bool m_deferredRetireDrainScheduled{ false };
	std::atomic_bool m_deferredRetireStop{ false };
	std::atomic<std::uint64_t> m_deferredRetireCompletedFrame{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireDelayFrames{ 4 };
	std::atomic<std::uint64_t> m_deferredRetireRangesQueued{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireRangesRetired{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireBytesQueued{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireBytesRetired{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireQueueDepth{ 0 };
	std::atomic<std::uint64_t> m_deferredRetireWorkerUs{ 0 };
	std::mutex m_activeDrawSetCompactionMutex;
	std::deque<DrawWorkloadKey> m_activeDrawSetCompactionRequests;
	std::deque<ActiveDrawSetCompactionJob> m_activeDrawSetCompactionJobs;
	std::deque<ActiveDrawSetCompactionResult> m_activeDrawSetCompactionResults;
	std::unordered_set<DrawWorkloadKey, DrawWorkloadKey::Hasher> m_activeDrawSetCompactionQueued;
	TaskScope m_activeDrawSetCompactionScope;
	std::atomic_bool m_activeDrawSetCompactionStop{ false };
	std::atomic_bool m_activeDrawSetCompactionDrainScheduled{ false };
	std::mutex m_objectUpdateMutex; // Mutex for thread safety
	std::mutex m_normalMatrixUpdateMutex; // Mutex for thread safety

	std::shared_ptr<SortedUnsignedIntBuffer> EnsureActiveDrawSetIndices(const DrawWorkloadKey& workloadKey, std::size_t initialCapacity = 1);
	std::uint32_t ActivateDrawRecordCPU(std::uint32_t drawRecordIndex);
	std::uint32_t AdvanceDrawRecordVisibilityGenerationCPU(std::uint32_t drawRecordIndex);
	void JournalDrawRecordVisibilityRange(std::size_t first, std::size_t count);
	std::uint32_t ActivateDrawRecord(std::uint32_t drawRecordIndex);
	void TombstoneDrawRecord(std::uint32_t drawRecordIndex);
	void TombstoneDrawRecords(std::span<const std::uint32_t> drawRecordIndices);
	void AppendActiveDrawSetEntries(const DrawWorkloadKey& workloadKey, const std::vector<SortedUnsignedIntBuffer::ActiveDrawSetEntry>& entries);
	void AssignStaticImportTransactionGenerations(br::render::MaterializedStaticImportTransaction& transaction);
	void AssignStaticImportTransactionGenerations(std::span<br::render::MaterializedStaticImportTransaction*> transactions);
};
