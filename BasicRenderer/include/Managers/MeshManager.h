#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ShaderBuffers.h"
#include "Mesh/Mesh.h"
#include "Import/CLodCache.h"
#include "Managers/Singletons/DirectStorageManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "RenderPasses/Base/PassReturn.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/Buffers/PagePool.h"
#include "Interfaces/IResourceProvider.h"

class Mesh;
class MeshInstance;
class Material;
class DynamicBuffer;
class ResourceGroup;
class BufferView;
class ViewManager;

class MeshManager : public IResourceProvider {
public:
	struct CLodActiveGroupRange {
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
	};

	struct CLodStreamingDebugStats {
		uint32_t residentGroups = 0;
		uint32_t residentAllocations = 0;
		uint32_t queuedRequests = 0;
		uint32_t queuedOrInFlightGroups = 0;
		uint32_t completedResults = 0;
		uint64_t residentAllocationBytes = 0;
		uint64_t completedResultBytes = 0;
		uint64_t totalStreamedBytes = 0;
	};

	struct CLodRayTracingResidentGroup {
		uint32_t groupGlobalIndex = 0;
		uint32_t groupLocalIndex = 0;
		ClusterLODGroup group{};
		ClusterLODGroupChunk chunk{};
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<uint32_t> meshPageIndices;
		std::vector<PagePool::PageAllocation> pageAllocations;
	};

	struct CLodRayTracingResidencySnapshot {
		std::vector<CLodRayTracingResidentGroup> residentGroups;
		PagePool* pagePool = nullptr;
		uint64_t pagePoolGeneration = 0;
	};

	// Represents the outcome of a single disk-streamed group IO.
	struct CLodPrefetchedChildLayout {
		uint32_t groupGlobalIndex = 0;
		CLodCache::GroupPayloadLayoutMetadata layout;
	};

	struct CLodDiskStreamingCompletion {
		uint32_t groupGlobalIndex = 0;
		bool success = false;
		ClusterLODGroupChunk chunk{};
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch;
		std::vector<std::vector<std::byte>> pageBlobs;
		std::vector<uint32_t> preAllocatedPages;
		std::vector<PagePool::PageAllocation> pageAllocations;
		std::vector<GroupPageMapEntry> pageMapEntries;
		uint64_t generation = 0;
		uint64_t totalStreamedBytes = 0;
		uint32_t fetchedPageCount = 0;
		std::string uploadPathLabel;
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
	};

	static std::unique_ptr<MeshManager> CreateUnique() {
		return std::unique_ptr<MeshManager>(new MeshManager());
	}
	~MeshManager();
	void AddMesh(std::shared_ptr<Mesh>& mesh, bool useMeshletReorderedVertices);
	void AddMeshInstance(MeshInstance* mesh, bool useMeshletReorderedVertices);
	void RemoveMesh(Mesh* mesh);
	void RemoveMeshInstance(MeshInstance* mesh);

	struct StaticMeshTemplateRequest {
		std::shared_ptr<Mesh> mesh;
		std::shared_ptr<Material> material;
	};

	struct StaticMeshTemplateRegistration {
		uint32_t meshTemplateIndex = 0;
		uint32_t clodOffsetIndex = 0;
		bool valid = false;
	};

	void AddMeshesBulk(const std::vector<std::shared_ptr<Mesh>>& meshes, bool useMeshletReorderedVertices);
	std::vector<StaticMeshTemplateRegistration> AddStaticMeshTemplatesBulk(const std::vector<StaticMeshTemplateRequest>& requests);
	uint32_t GetCLodMaxTraversalDepth() const { return m_clodActiveMaxTraversalDepth.load(std::memory_order_acquire); }

	void GetCLodActiveUniqueAssetGroupRanges(std::vector<CLodActiveGroupRange>& outRanges, uint32_t& outMaxGroupIndex) const;
	void GetCLodCoarsestUniqueAssetGroupRanges(std::vector<CLodActiveGroupRange>& outRanges) const;
	void GetCLodUniqueAssetParentMap(std::vector<int32_t>& outParentGroupByGlobal, uint32_t& outMaxGroupIndex) const;

	// Fused single-pass snapshot that combines GetCLodActiveUniqueAssetGroupRanges,
	// GetCLodCoarsestUniqueAssetGroupRanges, and GetCLodUniqueAssetParentMap.
	struct CLodStreamingDomainSnapshot {
		std::vector<CLodActiveGroupRange> activeRanges;
		std::vector<CLodActiveGroupRange> coarsestRanges;
		std::vector<int32_t> parentGroupByGlobal;
		std::vector<float> groupOriginalErrorByGlobal;
		uint32_t maxGroupIndex = 0;
	};
	void GetCLodStreamingDomainSnapshot(CLodStreamingDomainSnapshot& outSnapshot) const;

	enum class CLodStreamingDomainEventKind : uint8_t {
		SharedMeshAdded,
		ActiveRangeAdded,
		ActiveRangeRemoved,
		FullReset,
	};

	struct CLodStreamingDomainEvent {
		CLodStreamingDomainEventKind kind = CLodStreamingDomainEventKind::FullReset;
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
		std::vector<CLodActiveGroupRange> coarsestRanges;
	};

	void DrainCLodStreamingDomainEvents(std::vector<CLodStreamingDomainEvent>& outEvents, uint64_t& outGeneration);
	bool TryResolveCLodGroup(uint32_t groupGlobalIndex, uint32_t& outGroupsBase, uint32_t& outGroupLocalIndex, uint32_t* outGroupCount = nullptr) const;
	bool TryGetCLodParentGroup(uint32_t groupGlobalIndex, uint32_t& outParentGlobalIndex) const;
	void GetCLodChildGroups(uint32_t parentGroupGlobalIndex, std::vector<uint32_t>& outChildGroups) const;

	// Patch a single group's error field in the GPU groups buffer.
	// Used by the streaming system to override error for residency transitions.
	void PatchCLodGroupError(uint32_t groupGlobalIndex, float error);

	// Returns true when mesh/instance adds or removes have changed the
	// streaming structure since the last call.  After returning true the
	// flag is cleared automatically so subsequent calls return false until
	// the next structural change.
	bool ConsumeCLodStreamingStructureDirty();

	CLodStreamingDebugStats GetCLodStreamingDebugStats() const;
	void GetCLodRayTracingResidencySnapshot(CLodRayTracingResidencySnapshot& outSnapshot) const;
	void ProcessCLodDiskStreamingIO();

	// Drains groups that completed disk streaming since the last call.
	// The extension uses this to learn which groups became resident (or failed)
	// so it can update the GPU-visible non-resident bitset accordingly.
	void DrainCompletedCLodDiskStreamingGroups(std::vector<CLodDiskStreamingCompletion>& outCompletions);

	// Focused eviction: frees a resident group's page-pool pages, marks it
	// non-resident, and uploads the chunk table.  Returns true on success.
	bool FreeCLodGroupEviction(uint32_t groupGlobalIndex);
	bool EvictCLodGroupResidency(uint32_t groupGlobalIndex, bool clearPageMapEntries);

	enum class CLodPageMapWriteReason : uint8_t {
		Commit,
		EvictClear,
		EvictClearSkippedResidentReference,
	};

	struct CLodPageMapWriteEvent {
		CLodPageMapWriteReason reason = CLodPageMapWriteReason::Commit;
		uint32_t groupGlobalIndex = 0u;
		uint32_t groupLocalIndex = 0u;
		uint32_t groupsBase = 0u;
		uint32_t meshPageIndex = 0u;
		uint32_t physicalPage = ~0u;
		uint32_t slabDescriptorIndex = 0u;
		uint32_t slabByteOffset = 0u;
		uint32_t previousSlabDescriptorIndex = 0u;
		uint32_t previousSlabByteOffset = 0u;
		uint32_t referencedResidentGroupCount = 0u;
	};

	void SetCLodPageMapWriteCallback(std::function<void(const CLodPageMapWriteEvent&)> fn);
	bool CommitCLodGroupResidency(
		uint32_t groupGlobalIndex,
		const ClusterLODGroupChunk& chunk,
		std::span<const uint32_t> meshPageIndices,
		std::span<const GroupPageMapEntry> pageMapEntries,
		std::span<const PagePool::PageAllocation> pageAllocations,
		uint64_t streamedBytes = 0u);

	// Queues disk I/O for a group without any residency side-effects.
	// Returns true if the request was queued (or was already in the queue).
	struct CLodGroupDiskIOBatchRequest {
		uint32_t groupGlobalIndex = 0u;
		std::vector<bool> segmentNeedsFetch;
		std::vector<uint32_t> preAllocatedPages;
		std::vector<uint32_t> childLayoutPrefetchGroups;
		uint32_t priority = 0u;
		std::optional<CLodCache::GroupPayloadLayoutMetadata> prefetchedLayout;
	};
	uint32_t QueueCLodGroupDiskIOBatch(const std::vector<CLodGroupDiskIOBatchRequest>& requests, std::vector<bool>* outQueuedByRequest = nullptr);
	bool QueueCLodGroupDiskIO(uint32_t groupGlobalIndex, const std::vector<bool>& segmentNeedsFetch = {}, const std::vector<uint32_t>& preAllocatedPages = {}, uint32_t priority = 0u, const CLodCache::GroupPayloadLayoutMetadata* prefetchedLayout = nullptr);
	bool TryGetCLodGroupPayloadLayout(uint32_t groupGlobalIndex, CLodCache::GroupPayloadLayoutMetadata& outLayout, std::string* outMessage = nullptr);
	bool IsCLodStreamingDirectStorageEnabled() const { return m_clodStreamingDirectStorageEnabled.load(std::memory_order_acquire); }
	bool HasPendingCLodDirectStorageLaunches() const;
	bool HasPendingCLodDirectStorageUploads() const;
	void CollectCLodDirectStorageCompletionWaits(std::vector<ExternalTimelinePoint>& outWaits) const;
	bool LaunchPendingCLodDirectStorageUploads(rhi::Timeline waitTimeline, uint64_t waitValue);

	// Returns true if the group currently has disk I/O queued or in-flight.
	bool IsCLodGroupDiskIOQueued(uint32_t groupGlobalIndex) const;

	// Invalidates all in-flight and queued disk streaming IO.
	// Bumps a generation counter so that stale in-flight results are rejected.
	// Must be called when page allocations are invalidated (e.g. render graph rebuild).
	void InvalidateCLodDiskStreamingPipeline();

	struct CLodGroupStreamingInfo {
		struct ReferencedPageSegment {
			uint32_t meshPageIndex = 0;
			uint32_t sourceGroupLocalIndex = 0;
			uint32_t sourceGroupGlobalIndex = 0;
			uint32_t segmentGlobalIndex = 0;
			ClusterLODGroupSegment segment{};
		};

		ClusterLODRuntimeSummary::GroupChunkHint hint{};
		uint32_t groupsBase = 0;
		uint32_t pageMapBase = 0;
		uint32_t pageCount = 0;
		ClusterLODGroup group{};
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<ReferencedPageSegment> referencedPageSegments;
		std::vector<uint32_t> meshPageIndices;
		uint32_t vertexByteSize = 0;
		bool valid = false;
	};
	// Retrieves the chunk hint and vertex byte size for a group so that
	// the caller can compute the estimated page count before dispatching I/O.
	CLodGroupStreamingInfo GetCLodGroupStreamingInfo(uint32_t groupGlobalIndex) const;

	void UpdatePerMeshBuffer(std::unique_ptr<BufferView>& view, PerMeshCB& data);
	void UpdatePerMeshInstanceBuffer(std::unique_ptr<BufferView>& view, PerMeshInstanceCB& data);
	std::unique_ptr<BufferView> AllocatePerMeshOverrideBuffer(const PerMeshCB& data);
	void ReleasePerMeshOverrideBuffer(std::unique_ptr<BufferView>& view);
	void SetViewManager(ViewManager* viewManager) { m_pViewManager = viewManager; }

	// Access the CLod page pool (may be null if no CLod meshes loaded).
	PagePool* GetCLodPagePool() const { return m_clodPagePool.get(); }
	void SetCLodStreamingUploadFunction(PagePool::UploadFn fn);
	uint64_t GetActiveMeshletCount() const { return m_activeMeshletCount; }

	std::shared_ptr<Resource> ProvideResource(ResourceIdentifier const& key) override;
	std::vector<ResourceIdentifier> GetSupportedKeys() override;

private:
	MeshManager();
	std::unordered_map<ResourceIdentifier, std::shared_ptr<Resource>, ResourceIdentifier::Hasher> m_resources;

	// Base meshes
	std::shared_ptr<DynamicBuffer> m_perMeshBuffers;

	// mesh instances
	std::shared_ptr<DynamicBuffer> m_perMeshInstanceBuffers;

	std::shared_ptr<DynamicBuffer> m_perMeshInstanceClodOffsets;
	std::shared_ptr<DynamicBuffer> m_clodSharedGroupChunks;
	std::shared_ptr<DynamicBuffer> m_clodMeshMetadata;
	std::shared_ptr<DynamicBuffer> m_clodHierarchyLevelInfos;
	std::shared_ptr<DynamicBuffer> m_clusterLODGroups;
	std::shared_ptr<DynamicBuffer> m_clusterLODSegments;

	//std::shared_ptr<DynamicBuffer> m_clusterLODMeshlets;
	//std::shared_ptr<DynamicBuffer> m_clusterLODMeshletBounds;
	std::shared_ptr<DynamicBuffer> m_clusterLODNodes;
	std::shared_ptr<DynamicBuffer> m_clodGroupPageMap;
	uint64_t m_activeMeshletCount = 0;

	struct CLodSharedStreamingState {
		struct ResidentGroupAllocations {
			// Per-child page allocations (one page per child)
			std::vector<PagePool::PageAllocation> pageAllocations;

			void Reset() {
				pageAllocations.clear();
			}
		};

		Mesh* mesh = nullptr;
		std::unique_ptr<BufferView> ownedMeshMetadataView;
		uint32_t clodMeshMetadataIndex = 0;
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
		std::unique_ptr<BufferView> ownedGroupChunksView;
		BufferView* groupChunksView = nullptr;
		std::vector<ClusterLODGroupChunk> baselineGroupChunks;
		std::vector<uint8_t> groupResidentFlags;
		std::vector<ResidentGroupAllocations> residentGroupAllocations;
		uint32_t activeInstanceCount = 0;

		// Copies of hierarchy data needed at streaming-apply time.
		// (The Mesh releases its CPU copies after setup via ReleaseCLodHierarchyCpuData.)
		std::vector<ClusterLODGroup> groups;
		std::vector<ClusterLODGroupSegment> segments;
		std::vector<uint32_t> groupPageReferences;
		std::vector<uint32_t> groupPageReferenceOffsets;

		// Parent-child mapping and original error values, copied from
		// the runtime summary at AddMesh time so that the streaming
		// domain snapshot always has reliable data regardless of mesh
		// object lifetime or summary state.
		std::vector<int32_t> parentGroupByLocal;
		std::vector<std::vector<uint32_t>> childrenByLocalParent;
		std::vector<float> groupErrorByLocal;
		std::vector<ClusterLODRuntimeSummary::GroupRange> coarsestRanges;

		// GroupPageMap buffer view for this mesh's page map entries.
		std::unique_ptr<BufferView> ownedPageMapView;
		uint32_t pageMapGlobalBase = 0; // global offset into GroupPageMap buffer
		uint32_t totalPageMapEntries = 0;
		std::vector<GroupPageMapEntry> pageMapEntriesCPU; // CPU mirror for UpdateView
	};

	struct CLodSharedStreamingRange {
		uint32_t begin = 0;
		uint32_t end = 0;
		std::shared_ptr<CLodSharedStreamingState> state;
	};

	struct CLodStreamingInstanceState {
		MeshInstance* instance = nullptr;
		uint32_t meshInstanceIndex = 0;
		uint32_t groupsBase = 0;
		uint32_t groupCount = 0;
		std::shared_ptr<CLodSharedStreamingState> sharedMeshState;
	};

	std::unordered_map<uint32_t, CLodStreamingInstanceState> m_clodStreamingStateByInstanceIndex;
	std::unordered_map<const MeshInstance*, uint32_t> m_clodStreamingInstanceIndexByPtr;
	std::unordered_map<const Mesh*, std::shared_ptr<CLodSharedStreamingState>> m_clodSharedStreamingStateByMesh;
	std::vector<CLodSharedStreamingRange> m_clodSharedStreamingRanges;
	bool m_clodSharedStreamingRangesDirty = true;
	mutable std::mutex m_clodStreamingDomainEventsMutex;
	std::vector<CLodStreamingDomainEvent> m_clodStreamingDomainEvents;
	std::atomic<uint64_t> m_clodStreamingDomainEventGeneration{0};
	// Legacy fallback flag for old snapshot consumers.
	std::atomic<bool> m_clodStreamingStructureDirty{true};
	std::atomic<bool> m_clodStreamingDirectStorageEnabled{true};
	SettingsManager::Subscription m_clodStreamingDirectStorageSubscription;

	// Incremental debug-stats counters — updated in place by residency mutations.
	std::atomic<uint32_t> m_debugResidentGroups{0};
	std::atomic<uint32_t> m_debugResidentAllocations{0};
	std::atomic<uint64_t> m_debugTotalStreamedBytes{0};
	std::function<void(const CLodPageMapWriteEvent&)> m_clodPageMapWriteCallback;
	std::atomic<uint32_t> m_clodActiveMaxTraversalDepth{0};

	struct CLodDiskStreamingRequest {
		uint32_t groupGlobalIndex = 0;
		ClusterLODCacheSource cacheSource{};
		uint32_t groupsBase = 0;
		uint32_t groupLocalIndex = 0;
		std::optional<CLodCache::GroupPayloadLayoutMetadata> prefetchedLayout;
		std::vector<ClusterLODGroupDiskLocator> pageDiskLocators;
		uint32_t pageMapBase = 0;
		uint32_t pageCount = 0;
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch; // true = fetch from disk; false = reuse existing slab data
		std::vector<uint32_t> preAllocatedPages; // page IDs pre-allocated by the LRU
		std::vector<uint32_t> childLayoutPrefetchGroups;
		uint64_t generation = 0; // generation at time of request
		uint32_t priority = 0; // streaming priority for I/O dispatch ordering
	};

	struct CLodDiskStreamingResult {
		uint32_t groupGlobalIndex = 0;
		bool success = false;
		ClusterLODCacheSource cacheSource{};
		std::string uploadPathLabel = "CpuReadThenCpuUpload";
		std::optional<ClusterLODGroupChunk> groupChunkMetadata;
		std::vector<bool> segmentNeedsFetch;
		std::vector<uint32_t> meshPageIndices;
		std::vector<uint32_t> directStoragePageBlobSizes;
		std::vector<uint64_t> directStoragePageBlobOffsets;
		bool directStorageGpuUploadPending = false;
		std::vector<std::vector<std::byte>> pageBlobs;
		std::vector<uint32_t> preAllocatedPages; // forwarded from request
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
		uint64_t generation = 0; // generation at time of request
	};

	struct CLodPendingDirectStorageUpload {
		uint32_t groupGlobalIndex = 0;
		uint64_t generation = 0;
		std::shared_ptr<CLodSharedStreamingState> sharedState;
		uint32_t groupLocalIndex = 0;
		ClusterLODGroupChunk chunk{};
		std::vector<PagePool::PageAllocation> pageAllocations;
		std::vector<GroupPageMapEntry> pageMapEntries;
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch;
		uint32_t fetchedPageCount = 0;
		uint64_t totalBlobBytes = 0;
		std::string uploadPathLabel = "DirectStorageGpuDirect";
		DirectStorageAsyncRequestHandle uploadHandle;
		rhi::Timeline completionTimeline;
		uint64_t completionValue = 0;
		std::vector<uint32_t> pageIds;
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
	};

	struct CLodPendingDirectStorageLaunch {
		uint32_t groupGlobalIndex = 0;
		uint64_t generation = 0;
		ClusterLODCacheSource cacheSource{};
		std::shared_ptr<CLodSharedStreamingState> sharedState;
		uint32_t groupLocalIndex = 0;
		ClusterLODGroupChunk chunk{};
		std::vector<PagePool::PageAllocation> pageAllocations;
		std::vector<GroupPageMapEntry> pageMapEntries;
		std::vector<uint32_t> meshPageIndices;
		std::vector<bool> segmentNeedsFetch;
		std::vector<br::DirectStorageBufferRegionCopy> copies;
		std::vector<uint32_t> pageIds;
		std::vector<CLodPrefetchedChildLayout> prefetchedChildLayouts;
		uint32_t fetchedPageCount = 0;
		uint64_t totalBlobBytes = 0;
		std::string uploadPathLabel = "DirectStorageGpuDirect";
	};

	// Pending requests waiting to be dispatched (guarded by m_clodDiskStreamingMutex).
	mutable std::mutex m_clodDiskStreamingMutex;
	std::vector<CLodDiskStreamingRequest> m_clodDiskStreamingRequests;
	std::unordered_set<uint32_t> m_clodDiskStreamingQueuedGroups;

	// Generation counter for invalidating in-flight disk IO across rebuilds.
	std::atomic<uint64_t> m_clodDiskStreamingGeneration{0};

	// Guards m_clodDiskStreamingResults and m_clodDiskStreamingCompletions.
	mutable std::mutex m_clodDiskStreamingResultsMutex;

	// Completed results waiting to be applied on the main thread.
	std::vector<CLodDiskStreamingResult> m_clodDiskStreamingResults;
	std::vector<CLodDiskStreamingCompletion> m_clodDiskStreamingCompletions;
	std::vector<CLodPendingDirectStorageLaunch> m_clodPendingDirectStorageLaunches;
	std::vector<CLodPendingDirectStorageUpload> m_clodPendingDirectStorageUploads;
	PagePool::UploadFn m_clodStreamingUploadFn;

	rhi::TimelinePtr m_clodDirectStorageCompletionFencePtr;
	rhi::Timeline m_clodDirectStorageCompletionFenceHandle;
	std::atomic<uint64_t> m_clodDirectStorageCompletionFenceCounter{0};

	// Guards CLodSharedStreamingState interiors (groupResidentFlags,
	// baselineGroupChunks, residentGroupAllocations),
	// m_clodPagePool, and m_clodSharedGroupChunks UpdateView calls.
	mutable std::mutex m_clodResidencyMutex;

	// Maximum number of IO requests dispatched per ProcessCLodDiskStreamingIO call.
	static constexpr uint32_t kMaxIoBatchSize = 128u;

	void DispatchCLodDiskStreamingBatch();
	bool QueueCLodDiskStreamingRequest(uint32_t groupGlobalIndex, CLodSharedStreamingState& state, uint32_t groupLocalIndex, bool& outQueued, const std::vector<bool>& segmentNeedsFetch = {}, const std::vector<uint32_t>& preAllocatedPages = {}, uint32_t priority = 0u);
	bool QueueCLodDiskStreamingRequest(uint32_t groupGlobalIndex, CLodSharedStreamingState& state, uint32_t groupLocalIndex, bool& outQueued, const std::vector<bool>& segmentNeedsFetch = {}, const std::vector<uint32_t>& preAllocatedPages = {}, uint32_t priority = 0u, const CLodCache::GroupPayloadLayoutMetadata* prefetchedLayout = nullptr);

	enum class DiskStreamingApplyResult {
		Prepared,
		DeferredPendingUpload,
		FailedPermanent,
	};
	DiskStreamingApplyResult PrepareCompletedCLodDiskStreamingResult(CLodDiskStreamingResult& result, const std::vector<uint32_t>& preAllocatedPages, CLodDiskStreamingCompletion& outCompletion);
	void FinalizePendingCLodDirectStorageUploads(uint64_t currentGeneration, std::vector<CLodDiskStreamingCompletion>& outCompletions, std::vector<uint32_t>& outFinishedGroups);
	void UploadCLodGroupChunkTable(const CLodSharedStreamingState& state);
	void UploadCLodGroupChunk(const CLodSharedStreamingState& state, uint32_t groupLocalIndex);
	void UploadCLodGroupPageMapRange(
		CLodSharedStreamingState& state,
		uint32_t pageMapOffset,
		std::span<const GroupPageMapEntry> pageMapEntries);
	bool IsCLodGroupResident(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const;
	bool IsCLodMeshPageReferencedByResidentGroup(const CLodSharedStreamingState& state, uint32_t meshPageIndex) const;
	void DeallocateCLodGroupChunkAllocations(CLodSharedStreamingState& state, uint32_t groupLocalIndex);
	void ReleaseAllCLodGroupChunkAllocations(CLodSharedStreamingState& state);
 	static void ZeroCLodGroupChunkCounts(ClusterLODGroupChunk& chunk);
	bool ApplyCLodGroupEviction(CLodSharedStreamingState& state, uint32_t groupLocalIndex, bool clearPageMapEntries);

	void RebuildCLodSharedStreamingRangeIndex();
	void PublishCLodStreamingDomainEvent(CLodStreamingDomainEvent event);
	void PublishCLodStreamingDomainEventForSharedState(CLodStreamingDomainEventKind kind, const std::shared_ptr<CLodSharedStreamingState>& sharedState);
	void RecomputeCLodActiveMaxTraversalDepth();
	std::shared_ptr<CLodSharedStreamingState> FindCLodSharedStreamingStateByGlobalGroup(uint32_t groupGlobalIndex, uint32_t& outGroupLocalIndex);
	std::vector<uint32_t> GetCLodGroupMeshPageIndices(const CLodSharedStreamingState& state, uint32_t groupLocalIndex) const;

	ViewManager* m_pViewManager;

	// Page pool for CLod streaming
	std::unique_ptr<PagePool> m_clodPagePool;
};
